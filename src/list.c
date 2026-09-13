#define _POSIX_C_SOURCE 200809L

#include "list.h"
#include "kv_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void list_destroy(void *value_ptr, size_t *freed_bytes) {
    if (value_ptr == NULL) {
        if (freed_bytes != NULL) {
            *freed_bytes = 0U;
        }
        return;
    }
    size_t f = sizeof(kv_list_t);
    kv_list_t *list = (kv_list_t *)value_ptr;
    kv_list_node_t *node = list->head;
    while (node != NULL) {
        kv_list_node_t *next = node->next;
        f += sizeof(kv_list_node_t) + strlen(node->value) + 1U;
        tracked_free(node->value);
        tracked_free(node);
        node = next;
    }
    tracked_free(list);
    if (freed_bytes != NULL) {
        *freed_bytes = f;
    }
}

int kv_lpush(kv_store_t *store, const char *key, const char *value) {
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
    kv_list_t *list = NULL;

    if (entry != NULL) {
        if (entry->type != KV_TYPE_LIST) {
            pthread_mutex_unlock(&store->lock);
            return KV_ERR_WRONG_TYPE;
        }
        list = (kv_list_t *)entry->value_ptr;
        entry->last_accessed_time = ++store->lru_clock;
    } else {
        if (store->max_capacity > 0U && store->size >= store->max_capacity) {
            pthread_mutex_unlock(&store->lock);
            return KV_ERR_CAPACITY_FULL;
        }

        size_t entry_overhead = sizeof(kv_entry_t) + key_len + 1U + sizeof(kv_list_t);
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
        list = tracked_calloc(1U, sizeof(kv_list_t));
        if (list == NULL) {
            tracked_free(entry->key);
            tracked_free(entry);
            pthread_mutex_unlock(&store->lock);
            return KV_ERR_INTERNAL;
        }
        entry->type = KV_TYPE_LIST;
        entry->value_ptr = list;
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

    size_t node_overhead = sizeof(kv_list_node_t) + val_len + 1U;
    if (store->max_memory_budget > 0U) {
        while (store->allocated_bytes + node_overhead > store->max_memory_budget && store->size > 0U) {
            if (kv_evict_lru_unlocked(store) != KV_OK) break;
        }
        if (store->allocated_bytes + node_overhead > store->max_memory_budget) {
            pthread_mutex_unlock(&store->lock);
            return KV_ERR_BUDGET_EXCEEDED;
        }
    }

    kv_list_node_t *node = tracked_malloc(sizeof(kv_list_node_t));
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

    node->prev = NULL;
    node->next = list->head;
    if (list->head != NULL) {
        list->head->prev = node;
    }
    list->head = node;
    if (list->tail == NULL) {
        list->tail = node;
    }
    list->count++;

    store->allocated_bytes += node_overhead;
    if (store->allocated_bytes > store->peak_allocated_bytes) {
        store->peak_allocated_bytes = store->allocated_bytes;
    }

    long pos = -1;
    if (store->aof_fp != NULL) {
        write_aof_record(store, "LPUSH\t%s\t%s\n", key, value, &pos);
    }
    append_aof_rewrite_buffer(store, "LPUSH\t", 6);
    append_aof_rewrite_buffer(store, key, key_len);
    append_aof_rewrite_buffer(store, "\t", 1);
    append_aof_rewrite_buffer(store, value, val_len);
    append_aof_rewrite_buffer(store, "\n", 1);

    int count = (int)list->count;
    pthread_mutex_unlock(&store->lock);
    return count;
}

int kv_rpop(kv_store_t *store, const char *key, char **out_value) {
    if (store == NULL || key == NULL || out_value == NULL || !store->lock_initialized) {
        return KV_ERR_INVALID_PARAM;
    }

    pthread_mutex_lock(&store->lock);

    kv_entry_t *entry = find_entry_unlocked(store, key);
    if (entry == NULL) {
        pthread_mutex_unlock(&store->lock);
        return KV_ERR_NOT_FOUND;
    }
    if (entry->type != KV_TYPE_LIST) {
        pthread_mutex_unlock(&store->lock);
        return KV_ERR_WRONG_TYPE;
    }

    kv_list_t *list = (kv_list_t *)entry->value_ptr;
    if (list == NULL || list->tail == NULL || list->count == 0U) {
        pthread_mutex_unlock(&store->lock);
        return KV_ERR_NOT_FOUND;
    }

    kv_list_node_t *node = list->tail;
    size_t vlen = strlen(node->value);
    char *ret = malloc(vlen + 1U);
    if (ret == NULL) {
        pthread_mutex_unlock(&store->lock);
        return KV_ERR_INTERNAL;
    }
    memcpy(ret, node->value, vlen);
    ret[vlen] = '\0';
    *out_value = ret;

    list->tail = node->prev;
    if (list->tail != NULL) {
        list->tail->next = NULL;
    } else {
        list->head = NULL;
    }
    list->count--;

    size_t node_freed = sizeof(kv_list_node_t) + vlen + 1U;
    tracked_free(node->value);
    tracked_free(node);

    if (store->allocated_bytes >= node_freed) {
        store->allocated_bytes -= node_freed;
    } else {
        store->allocated_bytes = 0U;
    }

    long pos = -1;
    if (store->aof_fp != NULL) {
        write_aof_record(store, "RPOP\t%s\n", key, NULL, &pos);
    }
    append_aof_rewrite_buffer(store, "RPOP\t", 5);
    append_aof_rewrite_buffer(store, key, strlen(key));
    append_aof_rewrite_buffer(store, "\n", 1);

    if (list->count == 0U) {
        kv_delete_internal(store, key);
    } else {
        entry->last_accessed_time = ++store->lru_clock;
    }

    pthread_mutex_unlock(&store->lock);
    return KV_OK;
}

int kv_llen(kv_store_t *store, const char *key) {
    if (store == NULL || key == NULL || !store->lock_initialized) {
        return KV_ERR_INVALID_PARAM;
    }

    pthread_mutex_lock(&store->lock);

    kv_entry_t *entry = find_entry_unlocked(store, key);
    if (entry == NULL) {
        pthread_mutex_unlock(&store->lock);
        return 0;
    }
    if (entry->type != KV_TYPE_LIST) {
        pthread_mutex_unlock(&store->lock);
        return KV_ERR_WRONG_TYPE;
    }

    kv_list_t *list = (kv_list_t *)entry->value_ptr;
    int count = (list != NULL) ? (int)list->count : 0;
    entry->last_accessed_time = ++store->lru_clock;

    pthread_mutex_unlock(&store->lock);
    return count;
}
