#ifndef EVICTION_H
#define EVICTION_H

#include "kv.h"

int kv_expire(kv_store_t *store, const char *key, int64_t seconds);
int kv_pexpire(kv_store_t *store, const char *key, int64_t milliseconds);
int kv_pexpireat(kv_store_t *store, const char *key, int64_t unix_ms);
int64_t kv_ttl(kv_store_t *store, const char *key);
int64_t kv_pttl(kv_store_t *store, const char *key);
int kv_persist(kv_store_t *store, const char *key);
size_t kv_expire_sample_sweep(kv_store_t *store, size_t sample_size, size_t max_keys_to_delete);
int kv_evict_lru(kv_store_t *store);
void kv_set_expire_at_ms_internal(kv_store_t *store, const char *key, int64_t expire_at_ms);

#endif
