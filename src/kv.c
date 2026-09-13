#define _POSIX_C_SOURCE 200809L

#include "kv.h"
#include "kv_internal.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/**
 * @brief Testing hook for simulated allocation failure (-1 = disabled).
 */
static int g_alloc_fail_countdown = -1;

void kv_set_alloc_fail_countdown(int countdown) {
    g_alloc_fail_countdown = countdown;
}

/**
 * @brief Testing hook for simulated disk I/O failure (0 = normal, 1 = fail).
 */
int g_io_fail = 0;

void kv_set_io_fail(int fail) {
    g_io_fail = fail;
}

void *tracked_malloc(size_t size) {
    if (g_alloc_fail_countdown == 0) {
        g_alloc_fail_countdown = -1;
        return NULL;
    }
    if (g_alloc_fail_countdown > 0) {
        g_alloc_fail_countdown--;
    }
    return malloc(size);
}

void *tracked_calloc(size_t num, size_t size) {
    if (g_alloc_fail_countdown == 0) {
        g_alloc_fail_countdown = -1;
        return NULL;
    }
    if (g_alloc_fail_countdown > 0) {
        g_alloc_fail_countdown--;
    }
    return calloc(num, size);
}

void tracked_free(void *ptr) {
    free(ptr);
}

int64_t kv_current_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000LL + (int64_t)(ts.tv_nsec / 1000000LL);
}

size_t bounded_strlen(const char *str, size_t max_len) {
    size_t len = 0U;
    while (len <= max_len && str[len] != '\0') {
        len++;
    }
    return len;
}

int validate_key(const char *key, size_t *out_len) {
    if (key == NULL) {
        return KV_ERR_INVALID_PARAM;
    }
    size_t len = bounded_strlen(key, KV_MAX_KEY_LEN);
    if (len == 0U) {
        return KV_ERR_EMPTY_KEY;
    }
    if (len > KV_MAX_KEY_LEN) {
        return KV_ERR_KEY_TOO_LONG;
    }
    for (size_t i = 0U; i < len; ++i) {
        if (isspace((unsigned char)key[i])) {
            return KV_ERR_KEY_WHITESPACE;
        }
    }
    if (out_len != NULL) {
        *out_len = len;
    }
    return KV_OK;
}

int validate_value(const char *value, size_t *out_len) {
    if (value == NULL) {
        return KV_ERR_INVALID_PARAM;
    }
    size_t len = bounded_strlen(value, KV_MAX_VALUE_LEN);
    if (len > KV_MAX_VALUE_LEN) {
        return KV_ERR_VAL_TOO_LONG;
    }
    if (out_len != NULL) {
        *out_len = len;
    }
    return KV_OK;
}

size_t hash_key(const char *key, size_t bucket_count) {
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)key; *p != '\0'; ++p) {
        hash ^= (uint64_t)*p;
        hash *= 1099511628211ULL;
    }
    return (size_t)(hash % (uint64_t)bucket_count);
}

char *copy_string_bounded(const char *src, size_t max_len) {
    if (src == NULL) {
        return NULL;
    }
    size_t len = bounded_strlen(src, max_len);
    if (len > max_len) {
        return NULL;
    }
    char *copy = tracked_malloc(len + 1U);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, src, len);
    copy[len] = '\0';
    return copy;
}

int entry_is_expired(const kv_entry_t *entry, int64_t now_ms) {
    if (entry == NULL) {
        return 0;
    }
    return (entry->expire_at_ms > 0 && entry->expire_at_ms <= now_ms);
}

void expire_and_delete_unlocked(kv_store_t *store, kv_entry_t *entry, kv_entry_t *prev, size_t index) {
    if (store == NULL || entry == NULL) {
        return;
    }
    if (prev != NULL) {
        prev->next = entry->next;
    } else {
        store->buckets[index] = entry->next;
    }

    size_t freed = sizeof(kv_entry_t) + strlen(entry->key) + 1U;
    if (entry->type == KV_TYPE_STRING) {
        if (entry->value != NULL) {
            freed += strlen(entry->value) + 1U;
            tracked_free(entry->value);
        }
    } else if (entry->type == KV_TYPE_LIST) {
        size_t list_freed = 0;
        list_destroy(entry->value_ptr, &list_freed);
        freed += list_freed;
    } else if (entry->type == KV_TYPE_SET) {
        size_t set_freed = 0;
        set_destroy(entry->value_ptr, &set_freed);
        freed += set_freed;
    }

    tracked_free(entry->key);
    tracked_free(entry);

    if (store->size > 0U) {
        store->size--;
    }
    if (store->allocated_bytes >= freed) {
        store->allocated_bytes -= freed;
    } else {
        store->allocated_bytes = 0U;
    }
}

kv_entry_t *find_entry_unlocked(const kv_store_t *store, const char *key) {
    if (store == NULL || store->buckets == NULL || key == NULL) {
        return NULL;
    }
    size_t index = hash_key(key, store->bucket_count);
    kv_entry_t *prev = NULL;
    kv_entry_t *entry = store->buckets[index];
    int64_t now = kv_current_time_ms();

    while (entry != NULL) {
        if (strcmp(entry->key, key) == 0) {
            if (entry_is_expired(entry, now)) {
                expire_and_delete_unlocked((kv_store_t *)store, entry, prev, index);
                return NULL;
            }
            return entry;
        }
        prev = entry;
        entry = entry->next;
    }
    return NULL;
}

void append_aof_rewrite_buffer(kv_store_t *store, const char *data, size_t len) {
    if (store == NULL || data == NULL || len == 0U || store->aof_rewrite_pid <= 0) {
        return;
    }
    pthread_mutex_lock(&store->aof_lock);
    if (store->aof_rewrite_pid <= 0) {
        pthread_mutex_unlock(&store->aof_lock);
        return;
    }
    if (store->aof_rewrite_buf == NULL) {
        store->aof_rewrite_buf_cap = (len > 4096U) ? len * 2U : 4096U;
        store->aof_rewrite_buf = malloc(store->aof_rewrite_buf_cap);
        store->aof_rewrite_buf_len = 0U;
    } else if (store->aof_rewrite_buf_len + len > store->aof_rewrite_buf_cap) {
        while (store->aof_rewrite_buf_len + len > store->aof_rewrite_buf_cap) {
            store->aof_rewrite_buf_cap *= 2U;
        }
        store->aof_rewrite_buf = realloc(store->aof_rewrite_buf, store->aof_rewrite_buf_cap);
    }
    if (store->aof_rewrite_buf != NULL) {
        memcpy(store->aof_rewrite_buf + store->aof_rewrite_buf_len, data, len);
        store->aof_rewrite_buf_len += len;
    }
    pthread_mutex_unlock(&store->aof_lock);
}

int kv_init(kv_store_t *store, size_t bucket_count) {
    return kv_init_full(store, bucket_count, KV_DEFAULT_MAX_CAPACITY, KV_DEFAULT_MEMORY_BUDGET, NULL);
}

int kv_init_with_capacity(kv_store_t *store, size_t bucket_count, size_t max_capacity) {
    return kv_init_full(store, bucket_count, max_capacity, KV_DEFAULT_MEMORY_BUDGET, NULL);
}

int kv_init_with_budget(kv_store_t *store, size_t bucket_count, size_t max_capacity, size_t max_memory_budget) {
    return kv_init_full(store, bucket_count, max_capacity, max_memory_budget, NULL);
}

int kv_init_with_aof(kv_store_t *store, size_t bucket_count, const char *aof_path) {
    return kv_init_full(store, bucket_count, KV_DEFAULT_MAX_CAPACITY, KV_DEFAULT_MEMORY_BUDGET, aof_path);
}

#define KV_DEFAULT_NUM_LOCKS 32U

size_t kv_lock_index(const kv_store_t *store, const char *key) {
    if (store == NULL || store->num_locks == 0U || key == NULL) {
        return 0U;
    }
    return hash_key(key, store->num_locks);
}

void kv_acquire_bucket_lock(kv_store_t *store, const char *key) {
    if (store != NULL && store->lock_initialized && store->bucket_locks != NULL && key != NULL) {
        size_t idx = kv_lock_index(store, key);
        pthread_mutex_lock(&store->bucket_locks[idx]);
    }
}

void kv_release_bucket_lock(kv_store_t *store, const char *key) {
    if (store != NULL && store->lock_initialized && store->bucket_locks != NULL && key != NULL) {
        size_t idx = kv_lock_index(store, key);
        pthread_mutex_unlock(&store->bucket_locks[idx]);
    }
}

void kv_acquire_all_locks(kv_store_t *store) {
    if (store != NULL && store->lock_initialized && store->bucket_locks != NULL) {
        for (size_t i = 0U; i < store->num_locks; ++i) {
            pthread_mutex_lock(&store->bucket_locks[i]);
        }
    }
}

void kv_release_all_locks(kv_store_t *store) {
    if (store != NULL && store->lock_initialized && store->bucket_locks != NULL) {
        for (size_t i = store->num_locks; i > 0U; --i) {
            pthread_mutex_unlock(&store->bucket_locks[i - 1U]);
        }
    }
}

static void destroy_store_locks(kv_store_t *store) {
    if (store != NULL && store->lock_initialized && store->bucket_locks != NULL) {
        for (size_t i = 0U; i < store->num_locks; ++i) {
            pthread_mutex_destroy(&store->bucket_locks[i]);
        }
        pthread_mutex_destroy(&store->aof_lock);
        free(store->bucket_locks);
        store->bucket_locks = NULL;
        store->num_locks = 0U;
        store->lock_initialized = 0;
    }
}

int kv_init_full(kv_store_t *store, size_t bucket_count, size_t max_capacity, size_t max_memory_budget, const char *aof_path) {
    if (store == NULL || bucket_count == 0U) {
        return KV_ERR_INVALID_PARAM;
    }

    size_t buckets_bytes = bucket_count * sizeof(kv_entry_t *);
    if (max_memory_budget > 0U && buckets_bytes > max_memory_budget) {
        return KV_ERR_BUDGET_EXCEEDED;
    }

    kv_entry_t **buckets = tracked_calloc(bucket_count, sizeof(kv_entry_t *));
    if (buckets == NULL) {
        return KV_ERR_INTERNAL;
    }

    store->buckets = buckets;
    atomic_init(&store->bucket_count, bucket_count);
    atomic_init(&store->size, 0U);
    atomic_init(&store->max_capacity, max_capacity);
    atomic_init(&store->allocated_bytes, buckets_bytes);
    atomic_init(&store->peak_allocated_bytes, buckets_bytes);
    atomic_init(&store->max_memory_budget, max_memory_budget);
    atomic_init(&store->auto_resize, 1);
    store->aof_fp = NULL;
    store->aof_path = NULL;
    store->durability_level = KV_DURABILITY_FLUSH;
    atomic_init(&store->lru_clock, 0U);
    store->bgsave_pid = 0;
    store->aof_rewrite_pid = 0;
    store->aof_rewrite_buf = NULL;
    store->aof_rewrite_buf_len = 0U;
    store->aof_rewrite_buf_cap = 0U;

    store->num_locks = bucket_count;
    if (store->num_locks > 64U) {
        store->num_locks = 64U;
    } else if (store->num_locks < KV_DEFAULT_NUM_LOCKS) {
        store->num_locks = KV_DEFAULT_NUM_LOCKS;
    }
    store->bucket_locks = calloc(store->num_locks, sizeof(pthread_mutex_t));
    if (store->bucket_locks == NULL) {
        tracked_free(store->buckets);
        store->buckets = NULL;
        return KV_ERR_INTERNAL;
    }

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    for (size_t i = 0U; i < store->num_locks; ++i) {
        pthread_mutex_init(&store->bucket_locks[i], &attr);
    }
    pthread_mutex_init(&store->aof_lock, &attr);
    pthread_mutexattr_destroy(&attr);
    store->lock_initialized = 1;

    if (aof_path != NULL) {
        size_t path_len = strlen(aof_path);
        size_t path_bytes = path_len + 1U;

        if (max_memory_budget > 0U && store->allocated_bytes + path_bytes > max_memory_budget) {
            destroy_store_locks(store);
            tracked_free(store->buckets);
            store->buckets = NULL;
            return KV_ERR_BUDGET_EXCEEDED;
        }

        store->aof_path = tracked_malloc(path_bytes);
        if (store->aof_path == NULL) {
            destroy_store_locks(store);
            tracked_free(store->buckets);
            store->buckets = NULL;
            return KV_ERR_INTERNAL;
        }
        memcpy(store->aof_path, aof_path, path_len);
        store->aof_path[path_len] = '\0';
        store->allocated_bytes += path_bytes;
        if (store->allocated_bytes > store->peak_allocated_bytes) {
            store->peak_allocated_bytes = store->allocated_bytes;
        }

        /* Replay existing log */
        replay_aof_log(store, aof_path);

        store->aof_fp = fopen(aof_path, "a+");
        if (store->aof_fp == NULL) {
            destroy_store_locks(store);
            tracked_free(store->aof_path);
            store->aof_path = NULL;
            tracked_free(store->buckets);
            store->buckets = NULL;
            return KV_ERR_IO;
        }
    }

    return KV_OK;
}

int kv_resize_unlocked(kv_store_t *store, size_t new_bucket_count) {
    if (store == NULL || new_bucket_count == 0U) {
        return KV_ERR_INVALID_PARAM;
    }

    size_t new_buckets_bytes = new_bucket_count * sizeof(kv_entry_t *);
    size_t old_buckets_bytes = store->bucket_count * sizeof(kv_entry_t *);

    if (store->max_memory_budget > 0U) {
        if (store->allocated_bytes + new_buckets_bytes > store->max_memory_budget) {
            return KV_ERR_BUDGET_EXCEEDED;
        }
    }

    kv_entry_t **new_buckets = tracked_calloc(new_bucket_count, sizeof(kv_entry_t *));
    if (new_buckets == NULL) {
        return KV_ERR_INTERNAL;
    }

    store->allocated_bytes += new_buckets_bytes;
    if (store->allocated_bytes > store->peak_allocated_bytes) {
        store->peak_allocated_bytes = store->allocated_bytes;
    }

    for (size_t i = 0U; i < store->bucket_count; ++i) {
        kv_entry_t *entry = store->buckets[i];
        while (entry != NULL) {
            kv_entry_t *next = entry->next;
            size_t new_index = hash_key(entry->key, new_bucket_count);
            entry->next = new_buckets[new_index];
            new_buckets[new_index] = entry;
            entry = next;
        }
    }

    tracked_free(store->buckets);
    store->buckets = new_buckets;
    store->bucket_count = new_bucket_count;
    store->allocated_bytes -= old_buckets_bytes;

    return KV_OK;
}

int kv_resize(kv_store_t *store, size_t new_bucket_count) {
    if (store == NULL || new_bucket_count == 0U) {
        return KV_ERR_INVALID_PARAM;
    }

    kv_acquire_all_locks(store);
    int res = kv_resize_unlocked(store, new_bucket_count);
    kv_release_all_locks(store);
    return res;
}

void kv_set_auto_resize(kv_store_t *store, int enabled) {
    if (store != NULL) {
        kv_acquire_all_locks(store);
        store->auto_resize = enabled ? 1 : 0;
        kv_release_all_locks(store);
    }
}

size_t kv_bucket_count(const kv_store_t *store) {
    if (store == NULL) return 0U;
    return store->bucket_count;
}

void kv_set_max_capacity(kv_store_t *store, size_t max_capacity) {
    if (store != NULL) {
        kv_acquire_all_locks(store);
        store->max_capacity = max_capacity;
        kv_release_all_locks(store);
    }
}

void kv_set_memory_budget(kv_store_t *store, size_t budget_bytes) {
    if (store != NULL) {
        kv_acquire_all_locks(store);
        store->max_memory_budget = budget_bytes;
        kv_release_all_locks(store);
    }
}

size_t kv_allocated_bytes(const kv_store_t *store) {
    if (store == NULL) return 0U;
    return store->allocated_bytes;
}

size_t kv_peak_allocated_bytes(const kv_store_t *store) {
    if (store == NULL) return 0U;
    return store->peak_allocated_bytes;
}

size_t kv_max_memory_budget(const kv_store_t *store) {
    if (store == NULL) return 0U;
    return store->max_memory_budget;
}

void kv_destroy(kv_store_t *store) {
    if (store == NULL || store->buckets == NULL) {
        return;
    }

    if (store->lock_initialized) {
        kv_acquire_all_locks(store);
    }

    for (size_t i = 0U; i < store->bucket_count; ++i) {
        kv_entry_t *entry = store->buckets[i];
        while (entry != NULL) {
            kv_entry_t *next = entry->next;
            if (entry->type == KV_TYPE_STRING) {
                tracked_free(entry->value);
            } else if (entry->type == KV_TYPE_LIST) {
                list_destroy(entry->value_ptr, NULL);
            } else if (entry->type == KV_TYPE_SET) {
                set_destroy(entry->value_ptr, NULL);
            }
            tracked_free(entry->key);
            tracked_free(entry);
            entry = next;
        }
    }

    tracked_free(store->buckets);
    store->buckets = NULL;

    if (store->aof_fp != NULL) {
        fclose(store->aof_fp);
        store->aof_fp = NULL;
    }
    if (store->aof_path != NULL) {
        tracked_free(store->aof_path);
        store->aof_path = NULL;
    }
    if (store->aof_rewrite_buf != NULL) {
        free(store->aof_rewrite_buf);
        store->aof_rewrite_buf = NULL;
        store->aof_rewrite_buf_len = 0U;
        store->aof_rewrite_buf_cap = 0U;
    }

    store->bucket_count = 0U;
    store->size = 0U;
    store->allocated_bytes = 0U;
    store->peak_allocated_bytes = 0U;

    if (store->lock_initialized) {
        kv_release_all_locks(store);
        destroy_store_locks(store);
    }
}

int kv_set_internal(kv_store_t *store, const char *key, const char *value) {
    size_t key_len = 0U;
    size_t val_len = 0U;
    int k_err = validate_key(key, &key_len);
    if (k_err != KV_OK) return k_err;
    int v_err = validate_value(value, &val_len);
    if (v_err != KV_OK) return v_err;

    /* Check if key already exists */
    size_t index = hash_key(key, store->bucket_count);
    kv_entry_t *existing = NULL;
    for (kv_entry_t *e = store->buckets[index]; e != NULL; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            existing = e;
            break;
        }
    }

    if (existing != NULL) {
        if (existing->type != KV_TYPE_STRING) {
            return KV_ERR_WRONG_TYPE;
        }
        size_t old_val_bytes = strlen(existing->value) + 1U;
        size_t new_val_bytes = val_len + 1U;
        size_t delta = (new_val_bytes > old_val_bytes) ? (new_val_bytes - old_val_bytes) : 0U;

        if (store->max_memory_budget > 0U && delta > 0U) {
            while (store->allocated_bytes + delta > store->max_memory_budget && store->size > 0U) {
                if (kv_evict_lru_unlocked(store) != KV_OK) {
                    break;
                }
            }
            if (store->allocated_bytes + delta > store->max_memory_budget) {
                return KV_ERR_BUDGET_EXCEEDED;
            }
        }

        /* Check if existing entry was evicted while evicting for budget */
        existing = NULL;
        index = hash_key(key, store->bucket_count);
        for (kv_entry_t *e = store->buckets[index]; e != NULL; e = e->next) {
            if (strcmp(e->key, key) == 0) {
                existing = e;
                break;
            }
        }
        if (existing != NULL) {
            char *new_value = copy_string_bounded(value, KV_MAX_VALUE_LEN);
            if (new_value == NULL) {
                return KV_ERR_INTERNAL;
            }
            tracked_free(existing->value);
            existing->value = new_value;
            existing->last_accessed_time = ++store->lru_clock;

            if (new_val_bytes > old_val_bytes) {
                store->allocated_bytes += (new_val_bytes - old_val_bytes);
                if (store->allocated_bytes > store->peak_allocated_bytes) {
                    store->peak_allocated_bytes = store->allocated_bytes;
                }
            } else {
                store->allocated_bytes -= (old_val_bytes - new_val_bytes);
            }

            append_aof_rewrite_buffer(store, "SET\t", 4);
            append_aof_rewrite_buffer(store, key, key_len);
            append_aof_rewrite_buffer(store, "\t", 1);
            append_aof_rewrite_buffer(store, value, val_len);
            append_aof_rewrite_buffer(store, "\n", 1);

            return KV_OK;
        }
    }

    /* New entry insertion */
    if (store->max_capacity > 0U && store->size >= store->max_capacity) {
        return KV_ERR_CAPACITY_FULL;
    }

    size_t entry_bytes = sizeof(kv_entry_t) + key_len + 1U + val_len + 1U;
    if (store->max_memory_budget > 0U) {
        while (store->allocated_bytes + entry_bytes > store->max_memory_budget && store->size > 0U) {
            if (kv_evict_lru_unlocked(store) != KV_OK) {
                break;
            }
        }
        if (store->allocated_bytes + entry_bytes > store->max_memory_budget) {
            return KV_ERR_BUDGET_EXCEEDED;
        }
    }

    kv_entry_t *entry = tracked_malloc(sizeof(kv_entry_t));
    if (entry == NULL) {
        return KV_ERR_INTERNAL;
    }
    entry->key = copy_string_bounded(key, KV_MAX_KEY_LEN);
    if (entry->key == NULL) {
        tracked_free(entry);
        return KV_ERR_INTERNAL;
    }
    entry->value = copy_string_bounded(value, KV_MAX_VALUE_LEN);
    if (entry->value == NULL) {
        tracked_free(entry->key);
        tracked_free(entry);
        return KV_ERR_INTERNAL;
    }
    entry->type = KV_TYPE_STRING;
    entry->expire_at_ms = 0;
    entry->last_accessed_time = ++store->lru_clock;

    index = hash_key(key, store->bucket_count);
    entry->next = store->buckets[index];
    store->buckets[index] = entry;
    store->size++;
    store->allocated_bytes += entry_bytes;
    if (store->allocated_bytes > store->peak_allocated_bytes) {
        store->peak_allocated_bytes = store->allocated_bytes;
    }

    append_aof_rewrite_buffer(store, "SET\t", 4);
    append_aof_rewrite_buffer(store, key, key_len);
    append_aof_rewrite_buffer(store, "\t", 1);
    append_aof_rewrite_buffer(store, value, val_len);
    append_aof_rewrite_buffer(store, "\n", 1);

    return KV_OK;
}

int kv_set(kv_store_t *store, const char *key, const char *value) {
    if (store == NULL || !store->lock_initialized) {
        return KV_ERR_INVALID_PARAM;
    }

    size_t key_len = 0U;
    size_t val_len = 0U;
    int err = validate_key(key, &key_len);
    if (err != KV_OK) {
        return err;
    }
    err = validate_value(value, &val_len);
    if (err != KV_OK) {
        return err;
    }

    size_t entry_bytes = sizeof(kv_entry_t) + key_len + 1U + val_len + 1U;
    int is_global = 0;
    if (store->auto_resize && ((store->size + 1U) * KV_LOAD_FACTOR_DEN >= store->bucket_count * KV_LOAD_FACTOR_NUM)) {
        is_global = 1;
    }
    if (store->max_memory_budget > 0U && store->allocated_bytes + entry_bytes > store->max_memory_budget) {
        is_global = 1;
    }

    if (is_global) {
        kv_acquire_all_locks(store);
        /* Re-check under global locks: resize if load factor is exceeded */
        if (store->auto_resize && ((store->size + 1U) * KV_LOAD_FACTOR_DEN >= store->bucket_count * KV_LOAD_FACTOR_NUM)) {
            kv_resize_unlocked(store, store->bucket_count * 2U);
        }
    } else {
        kv_acquire_bucket_lock(store, key);
        /* Re-check under lock; if another thread updated size or allocated_bytes, escalate cleanly */
        if ((store->auto_resize && ((store->size + 1U) * KV_LOAD_FACTOR_DEN >= store->bucket_count * KV_LOAD_FACTOR_NUM)) ||
            (store->max_memory_budget > 0U && store->allocated_bytes + entry_bytes > store->max_memory_budget)) {
            /* Drop bucket lock BEFORE acquiring all locks to prevent circular wait */
            kv_release_bucket_lock(store, key);
            kv_acquire_all_locks(store);
            if (store->auto_resize && ((store->size + 1U) * KV_LOAD_FACTOR_DEN >= store->bucket_count * KV_LOAD_FACTOR_NUM)) {
                kv_resize_unlocked(store, store->bucket_count * 2U);
            }
            is_global = 1;
        }
    }

    long aof_pos = -1;
    if (store->aof_fp != NULL) {
        int aof_err = write_aof_record(store, "SET\t%s\t%s\n", key, value, &aof_pos);
        if (aof_err != KV_OK) {
            if (is_global) {
                kv_release_all_locks(store);
            } else {
                kv_release_bucket_lock(store, key);
            }
            return aof_err;
        }
    }

    int res = kv_set_internal(store, key, value);
    if (res != KV_OK && store->aof_fp != NULL) {
        rollback_aof_record(store, aof_pos);
    }

    if (is_global) {
        kv_release_all_locks(store);
    } else {
        kv_release_bucket_lock(store, key);
    }
    return res;
}

const char *kv_get(const kv_store_t *store, const char *key) {
    if (store == NULL || !store->lock_initialized || key == NULL) {
        return NULL;
    }
    kv_acquire_bucket_lock((kv_store_t *)store, key);
    kv_entry_t *entry = find_entry_unlocked(store, key);
    if (entry == NULL) {
        kv_release_bucket_lock((kv_store_t *)store, key);
        return NULL;
    }
    if (entry->type != KV_TYPE_STRING) {
        kv_release_bucket_lock((kv_store_t *)store, key);
        return NULL;
    }
    entry->last_accessed_time = ++((kv_store_t *)store)->lru_clock;
    const char *val = entry->value;
    kv_release_bucket_lock((kv_store_t *)store, key);
    return val;
}

int kv_delete_internal(kv_store_t *store, const char *key) {
    int err = validate_key(key, NULL);
    if (err != KV_OK) {
        return KV_ERR_INVALID_PARAM;
    }

    size_t index = hash_key(key, store->bucket_count);
    kv_entry_t **cursor = &store->buckets[index];
    kv_entry_t *prev = NULL;

    while (*cursor != NULL) {
        kv_entry_t *entry = *cursor;
        if (strcmp(entry->key, key) == 0) {
            expire_and_delete_unlocked(store, entry, prev, index);

            append_aof_rewrite_buffer(store, "DELETE\t", 7);
            append_aof_rewrite_buffer(store, key, strlen(key));
            append_aof_rewrite_buffer(store, "\n", 1);

            return KV_OK;
        }
        prev = entry;
        cursor = &entry->next;
    }
    return KV_ERR_NOT_FOUND;
}

int kv_delete(kv_store_t *store, const char *key) {
    if (store == NULL || !store->lock_initialized) {
        return KV_ERR_INVALID_PARAM;
    }
    int err = validate_key(key, NULL);
    if (err != KV_OK) {
        return KV_ERR_INVALID_PARAM;
    }

    kv_acquire_bucket_lock(store, key);

    long aof_pos = -1;
    if (store->aof_fp != NULL) {
        int aof_err = write_aof_record(store, "DELETE\t%s\n", key, NULL, &aof_pos);
        if (aof_err != KV_OK) {
            kv_release_bucket_lock(store, key);
            return aof_err;
        }
    }

    int res = kv_delete_internal(store, key);
    if (res != KV_OK && store->aof_fp != NULL) {
        rollback_aof_record(store, aof_pos);
    }

    kv_release_bucket_lock(store, key);
    return res;
}

int kv_exists(const kv_store_t *store, const char *key) {
    if (store == NULL || !store->lock_initialized || key == NULL) {
        return 0;
    }
    kv_acquire_bucket_lock((kv_store_t *)store, key);
    int exists = (find_entry_unlocked(store, key) != NULL);
    kv_release_bucket_lock((kv_store_t *)store, key);
    return exists;
}
