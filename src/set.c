#define _POSIX_C_SOURCE 200809L

#include "set.h"
#include "kv_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void set_destroy(void *value_ptr, size_t *freed_bytes) {
    if (value_ptr == NULL) {
        if (freed_bytes != NULL) {
            *freed_bytes = 0U;
        }
        return;
    }
    size_t f = sizeof(kv_set_t);
    kv_set_t *set = (kv_set_t *)value_ptr;
    f += set->bucket_count * sizeof(kv_set_node_t *);
    for (size_t i = 0U; i < set->bucket_count; ++i) {
        kv_set_node_t *node = set->buckets[i];
        while (node != NULL) {
            kv_set_node_t *next = node->next;
            f += sizeof(kv_set_node_t) + strlen(node->value) + 1U;
            tracked_free(node->value);
            tracked_free(node);
            node = next;
        }
    }
    tracked_free(set->buckets);
    tracked_free(set);
    if (freed_bytes != NULL) {
        *freed_bytes = f;
    }
}

int kv_sadd(kv_store_t *store, const char *key, const char *value) {
    if (store == NULL || key == NULL || value == NULL || !store->lock_initialized) {
        return KV_ERR_INVALID_PARAM;
    }

    size_t key_len = 0U;
    size_t val_len = 0U;
    int err = validate_key(key, &key_len);
    if (err != KV_OK) return err;
    err = validate_value(value, &val_len);
    if (err != KV_OK) return err;

    pthread_mutex_lock(&store->lock);

    kv_entry_t *entry = find_entry_unlocked(store, key);
    kv_set_t *set = NULL;

    if (entry != NULL) {
        if (entry->type != KV_TYPE_SET) {
            pthread_mutex_unlock(&store->lock);
            return KV_ERR_WRONG_TYPE;
        }
        set = (kv_set_t *)entry->value_ptr;
        entry->last_accessed_time = ++store->lru_clock;
    } else {
        if (store->max_capacity > 0U && store->size >= store->max_capacity) {
            pthread_mutex_unlock(&store->lock);
            return KV_ERR_CAPACITY_FULL;
        }

        size_t initial_buckets = 16U;
        size_t entry_overhead = sizeof(kv_entry_t) + key_len + 1U + sizeof(kv_set_t) + (initial_buckets * sizeof(kv_set_node_t *));
        if (store->max_memory_budget > 0U) {
            while (store->allocated_bytes + entry_overhead > store->max_memory_budget && store->size > 0U) {
                if (kv_evict_lru_unlocked(store) != KV_OK) break;
            }
            if (store->allocated_bytes + entry_overhead > store->max_memory_budget) {
                pthread_mutex_unlock(&store->lock);
                return KV_ERR_BUDGET_EXCEEDED;
            }
        }

        entry = tracked_malloc(sizeof(kv_entry_t));
        if (entry == NULL) {
            pthread_mutex_unlock(&store->lock);
            return KV_ERR_INTERNAL;
        }
        entry->key = copy_string_bounded(key, KV_MAX_KEY_LEN);
        if (entry->key == NULL) {
            tracked_free(entry);
            pthread_mutex_unlock(&store->lock);
            return KV_ERR_INTERNAL;
        }
        set = tracked_calloc(1U, sizeof(kv_set_t));
        if (set == NULL) {
            tracked_free(entry->key);
            tracked_free(entry);
            pthread_mutex_unlock(&store->lock);
            return KV_ERR_INTERNAL;
        }
        set->bucket_count = initial_buckets;
        set->buckets = tracked_calloc(initial_buckets, sizeof(kv_set_node_t *));
        if (set->buckets == NULL) {
            tracked_free(set);
            tracked_free(entry->key);
            tracked_free(entry);
            pthread_mutex_unlock(&store->lock);
            return KV_ERR_INTERNAL;
        }

        entry->type = KV_TYPE_SET;
        entry->value_ptr = set;
        entry->expire_at_ms = 0;
        entry->last_accessed_time = ++store->lru_clock;

        size_t idx = hash_key(key, store->bucket_count);
        entry->next = store->buckets[idx];
        store->buckets[idx] = entry;
        store->size++;
        store->allocated_bytes += entry_overhead;
        if (store->allocated_bytes > store->peak_allocated_bytes) {
            store->peak_allocated_bytes = store->allocated_bytes;
        }
    }

    size_t sidx = hash_key(value, set->bucket_count);
    for (kv_set_node_t *n = set->buckets[sidx]; n != NULL; n = n->next) {
        if (strcmp(n->value, value) == 0) {
            pthread_mutex_unlock(&store->lock);
            return 0; /* Duplicate */
        }
    }

    size_t node_overhead = sizeof(kv_set_node_t) + val_len + 1U;
    if (store->max_memory_budget > 0U) {
        while (store->allocated_bytes + node_overhead > store->max_memory_budget && store->size > 0U) {
            if (kv_evict_lru_unlocked(store) != KV_OK) break;
        }
        if (store->allocated_bytes + node_overhead > store->max_memory_budget) {
            pthread_mutex_unlock(&store->lock);
            return KV_ERR_BUDGET_EXCEEDED;
        }
    }

    kv_set_node_t *node = tracked_malloc(sizeof(kv_set_node_t));
    if (node == NULL) {
        pthread_mutex_unlock(&store->lock);
        return KV_ERR_INTERNAL;
    }
    node->value = copy_string_bounded(value, KV_MAX_VALUE_LEN);
    if (node->value == NULL) {
        tracked_free(node);
        pthread_mutex_unlock(&store->lock);
        return KV_ERR_INTERNAL;
    }

    node->next = set->buckets[sidx];
    set->buckets[sidx] = node;
    set->count++;

    store->allocated_bytes += node_overhead;
    if (store->allocated_bytes > store->peak_allocated_bytes) {
        store->peak_allocated_bytes = store->allocated_bytes;
    }

    long pos = -1;
    if (store->aof_fp != NULL) {
        write_aof_record(store, "SADD\t%s\t%s\n", key, value, &pos);
    }
    append_aof_rewrite_buffer(store, "SADD\t", 5);
    append_aof_rewrite_buffer(store, key, key_len);
    append_aof_rewrite_buffer(store, "\t", 1);
    append_aof_rewrite_buffer(store, value, val_len);
    append_aof_rewrite_buffer(store, "\n", 1);

    pthread_mutex_unlock(&store->lock);
    return 1;
}

int kv_srem(kv_store_t *store, const char *key, const char *value) {
    if (store == NULL || key == NULL || value == NULL || !store->lock_initialized) {
        return KV_ERR_INVALID_PARAM;
    }

    pthread_mutex_lock(&store->lock);

    kv_entry_t *entry = find_entry_unlocked(store, key);
    if (entry == NULL) {
        pthread_mutex_unlock(&store->lock);
        return 0;
    }
    if (entry->type != KV_TYPE_SET) {
        pthread_mutex_unlock(&store->lock);
        return KV_ERR_WRONG_TYPE;
    }

    kv_set_t *set = (kv_set_t *)entry->value_ptr;
    if (set == NULL || set->count == 0U) {
        pthread_mutex_unlock(&store->lock);
        return 0;
    }

    size_t sidx = hash_key(value, set->bucket_count);
    kv_set_node_t **cursor = &set->buckets[sidx];

    while (*cursor != NULL) {
        kv_set_node_t *node = *cursor;
        if (strcmp(node->value, value) == 0) {
            *cursor = node->next;

            size_t node_freed = sizeof(kv_set_node_t) + strlen(node->value) + 1U;
            tracked_free(node->value);
            tracked_free(node);
            set->count--;

            if (store->allocated_bytes >= node_freed) {
                store->allocated_bytes -= node_freed;
            } else {
                store->allocated_bytes = 0U;
            }

            long pos = -1;
            if (store->aof_fp != NULL) {
                write_aof_record(store, "SREM\t%s\t%s\n", key, value, &pos);
            }
            append_aof_rewrite_buffer(store, "SREM\t", 5);
            append_aof_rewrite_buffer(store, key, strlen(key));
            append_aof_rewrite_buffer(store, "\t", 1);
            append_aof_rewrite_buffer(store, value, strlen(value));
            append_aof_rewrite_buffer(store, "\n", 1);

            if (set->count == 0U) {
                kv_delete_internal(store, key);
            } else {
                entry->last_accessed_time = ++store->lru_clock;
            }

            pthread_mutex_unlock(&store->lock);
            return 1;
        }
        cursor = &node->next;
    }

    pthread_mutex_unlock(&store->lock);
    return 0;
}

int kv_scard(kv_store_t *store, const char *key) {
    if (store == NULL || key == NULL || !store->lock_initialized) {
        return KV_ERR_INVALID_PARAM;
    }

    pthread_mutex_lock(&store->lock);

    kv_entry_t *entry = find_entry_unlocked(store, key);
    if (entry == NULL) {
        pthread_mutex_unlock(&store->lock);
        return 0;
    }
    if (entry->type != KV_TYPE_SET) {
        pthread_mutex_unlock(&store->lock);
        return KV_ERR_WRONG_TYPE;
    }

    kv_set_t *set = (kv_set_t *)entry->value_ptr;
    int count = (set != NULL) ? (int)set->count : 0;
    entry->last_accessed_time = ++store->lru_clock;

    pthread_mutex_unlock(&store->lock);
    return count;
}

const char **kv_smembers(kv_store_t *store, const char *key, size_t *out_count) {
    if (store == NULL || key == NULL || out_count == NULL || !store->lock_initialized) {
        if (out_count != NULL) {
            *out_count = 0U;
        }
        return NULL;
    }

    pthread_mutex_lock(&store->lock);

    kv_entry_t *entry = find_entry_unlocked(store, key);
    if (entry == NULL || entry->type != KV_TYPE_SET) {
        pthread_mutex_unlock(&store->lock);
        *out_count = 0U;
        return NULL;
    }

    kv_set_t *set = (kv_set_t *)entry->value_ptr;
    if (set == NULL || set->count == 0U) {
        pthread_mutex_unlock(&store->lock);
        *out_count = 0U;
        return NULL;
    }

    const char **members = malloc(set->count * sizeof(const char *));
    if (members == NULL) {
        pthread_mutex_unlock(&store->lock);
        *out_count = 0U;
        return NULL;
    }

    size_t idx = 0U;
    for (size_t b = 0U; b < set->bucket_count; ++b) {
        for (kv_set_node_t *n = set->buckets[b]; n != NULL; n = n->next) {
            members[idx++] = n->value;
        }
    }

    *out_count = set->count;
    entry->last_accessed_time = ++store->lru_clock;

    pthread_mutex_unlock(&store->lock);
    return members;
}
