#define _POSIX_C_SOURCE 200809L

#include "eviction.h"
#include "kv_internal.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t g_xorshift_state = 2463534242UL;

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    if (x == 0U) {
        x = 2463534242UL;
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

void kv_set_expire_at_ms_internal(kv_store_t *store, const char *key, int64_t expire_at_ms) {
    if (store == NULL || key == NULL || store->buckets == NULL) {
        return;
    }
    size_t idx = hash_key(key, store->bucket_count);
    for (kv_entry_t *entry = store->buckets[idx]; entry != NULL; entry = entry->next) {
        if (strcmp(entry->key, key) == 0) {
            entry->expire_at_ms = expire_at_ms;
            return;
        }
    }
}

int kv_pexpireat(kv_store_t *store, const char *key, int64_t unix_ms) {
    if (store == NULL || key == NULL || !store->lock_initialized) {
        return KV_ERR_INVALID_PARAM;
    }

    pthread_mutex_lock(&store->lock);

    kv_entry_t *entry = find_entry_unlocked(store, key);
    if (entry == NULL) {
        pthread_mutex_unlock(&store->lock);
        return 0;
    }

    entry->expire_at_ms = unix_ms;

    long pos = -1;
    write_aof_pexpireat(store, key, unix_ms, &pos);

    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRId64, unix_ms);
    append_aof_rewrite_buffer(store, "PEXPIREAT\t", 10);
    append_aof_rewrite_buffer(store, key, strlen(key));
    append_aof_rewrite_buffer(store, "\t", 1);
    append_aof_rewrite_buffer(store, buf, strlen(buf));
    append_aof_rewrite_buffer(store, "\n", 1);

    pthread_mutex_unlock(&store->lock);
    return 1;
}

int kv_expire(kv_store_t *store, const char *key, int64_t seconds) {
    return kv_pexpireat(store, key, kv_current_time_ms() + (seconds * 1000LL));
}

int kv_pexpire(kv_store_t *store, const char *key, int64_t milliseconds) {
    return kv_pexpireat(store, key, kv_current_time_ms() + milliseconds);
}

int64_t kv_pttl(kv_store_t *store, const char *key) {
    if (store == NULL || key == NULL || !store->lock_initialized) {
        return -2;
    }

    pthread_mutex_lock(&store->lock);

    kv_entry_t *entry = find_entry_unlocked(store, key);
    if (entry == NULL) {
        pthread_mutex_unlock(&store->lock);
        return -2;
    }

    if (entry->expire_at_ms == 0) {
        pthread_mutex_unlock(&store->lock);
        return -1;
    }

    int64_t rem = entry->expire_at_ms - kv_current_time_ms();
    pthread_mutex_unlock(&store->lock);
    return (rem < 0) ? 0 : rem;
}

int64_t kv_ttl(kv_store_t *store, const char *key) {
    int64_t pt = kv_pttl(store, key);
    if (pt < 0) {
        return pt;
    }
    return (pt + 999LL) / 1000LL;
}

int kv_persist(kv_store_t *store, const char *key) {
    if (store == NULL || key == NULL || !store->lock_initialized) {
        return KV_ERR_INVALID_PARAM;
    }

    pthread_mutex_lock(&store->lock);

    kv_entry_t *entry = find_entry_unlocked(store, key);
    if (entry == NULL || entry->expire_at_ms == 0) {
        pthread_mutex_unlock(&store->lock);
        return 0;
    }

    entry->expire_at_ms = 0;

    long pos = -1;
    write_aof_pexpireat(store, key, 0, &pos);

    append_aof_rewrite_buffer(store, "PEXPIREAT\t", 10);
    append_aof_rewrite_buffer(store, key, strlen(key));
    append_aof_rewrite_buffer(store, "\t0\n", 3);

    pthread_mutex_unlock(&store->lock);
    return 1;
}

size_t kv_expire_sample_sweep(kv_store_t *store, size_t sample_size, size_t max_keys_to_delete) {
    if (store == NULL || !store->lock_initialized) {
        return 0U;
    }

    pthread_mutex_lock(&store->lock);

    if (store->size == 0U) {
        pthread_mutex_unlock(&store->lock);
        return 0U;
    }

    size_t deleted = 0U;
    int64_t now = kv_current_time_ms();

    /* Try sampled pass */
    size_t tries = (sample_size < 16U) ? 16U : sample_size;
    for (size_t iter = 0U; iter < tries && deleted < max_keys_to_delete && store->size > 0U; ++iter) {
        size_t idx = (size_t)(xorshift32(&g_xorshift_state) % store->bucket_count);
        kv_entry_t *prev = NULL;
        kv_entry_t *curr = store->buckets[idx];

        while (curr != NULL && deleted < max_keys_to_delete) {
            if (curr->expire_at_ms > 0 && curr->expire_at_ms <= now) {
                kv_entry_t *to_del = curr;
                curr = curr->next;
                expire_and_delete_unlocked(store, to_del, prev, idx);
                deleted++;
            } else {
                prev = curr;
                curr = curr->next;
            }
        }
    }

    /* If high-volume sweep requested, do a full scan sweep */
    if (sample_size >= 100U || (deleted == 0U && store->size > 0U)) {
        for (size_t idx = 0U; idx < store->bucket_count && deleted < max_keys_to_delete && store->size > 0U; ++idx) {
            kv_entry_t *prev = NULL;
            kv_entry_t *curr = store->buckets[idx];

            while (curr != NULL && deleted < max_keys_to_delete) {
                if (curr->expire_at_ms > 0 && curr->expire_at_ms <= now) {
                    kv_entry_t *to_del = curr;
                    curr = curr->next;
                    expire_and_delete_unlocked(store, to_del, prev, idx);
                    deleted++;
                } else {
                    prev = curr;
                    curr = curr->next;
                }
            }
        }
    }

    pthread_mutex_unlock(&store->lock);
    return deleted;
}

int kv_evict_lru_unlocked(kv_store_t *store) {
    if (store == NULL || store->size == 0U || store->buckets == NULL) {
        return KV_ERR_NOT_FOUND;
    }

    kv_entry_t *best_entry = NULL;
    kv_entry_t *best_prev = NULL;
    size_t best_idx = 0U;
    uint64_t min_clock = UINT64_MAX;

    /* Sample up to 16 non-empty buckets using xorshift32 */
    size_t samples_found = 0U;
    for (size_t s = 0U; s < 64U && samples_found < 16U; ++s) {
        size_t idx = (size_t)(xorshift32(&g_xorshift_state) % store->bucket_count);
        if (store->buckets[idx] != NULL) {
            samples_found++;
            kv_entry_t *prev = NULL;
            for (kv_entry_t *curr = store->buckets[idx]; curr != NULL; prev = curr, curr = curr->next) {
                if (curr->last_accessed_time < min_clock) {
                    min_clock = curr->last_accessed_time;
                    best_entry = curr;
                    best_prev = prev;
                    best_idx = idx;
                }
            }
        }
    }

    /* If sampling did not find candidates, linear scan to guarantee eviction */
    if (best_entry == NULL) {
        for (size_t idx = 0U; idx < store->bucket_count; ++idx) {
            kv_entry_t *prev = NULL;
            for (kv_entry_t *curr = store->buckets[idx]; curr != NULL; prev = curr, curr = curr->next) {
                if (curr->last_accessed_time < min_clock) {
                    min_clock = curr->last_accessed_time;
                    best_entry = curr;
                    best_prev = prev;
                    best_idx = idx;
                }
            }
        }
    }

    if (best_entry != NULL) {
        expire_and_delete_unlocked(store, best_entry, best_prev, best_idx);
        return KV_OK;
    }

    return KV_ERR_INTERNAL;
}

int kv_evict_lru(kv_store_t *store) {
    if (store == NULL || !store->lock_initialized) {
        return KV_ERR_INVALID_PARAM;
    }
    pthread_mutex_lock(&store->lock);
    int res = kv_evict_lru_unlocked(store);
    pthread_mutex_unlock(&store->lock);
    return res;
}