#ifndef KV_INTERNAL_H
#define KV_INTERNAL_H

#include "kv.h"

/* Internal helpers exposed across the newly split modules */

void list_destroy(void *value_ptr, size_t *freed_bytes);
void set_destroy(void *value_ptr, size_t *freed_bytes);

extern int g_io_fail;

void *tracked_malloc(size_t size);
void *tracked_calloc(size_t num, size_t size);
void tracked_free(void *ptr);

char *copy_string_bounded(const char *src, size_t max_len);
size_t bounded_strlen(const char *str, size_t max_len);
int validate_key(const char *key, size_t *out_len);
int validate_value(const char *value, size_t *out_len);
size_t hash_key(const char *key, size_t bucket_count);

int entry_is_expired(const kv_entry_t *entry, int64_t now_ms);
kv_entry_t *find_entry_unlocked(const kv_store_t *store, const char *key);

int kv_set_internal(kv_store_t *store, const char *key, const char *value);
int kv_delete_internal(kv_store_t *store, const char *key);
int kv_evict_lru_unlocked(kv_store_t *store);
int kv_resize_unlocked(kv_store_t *store, size_t new_bucket_count);

void append_aof_rewrite_buffer(kv_store_t *store, const char *data, size_t len);
int write_aof_record(kv_store_t *store, const char *format, const char *arg1, const char *arg2, long *out_pos);
int write_aof_pexpireat(kv_store_t *store, const char *key, int64_t expire_ms, long *out_pos);
void rollback_aof_record(kv_store_t *store, long pos);
int replay_aof_log(kv_store_t *store, const char *path);

size_t kv_lock_index(const kv_store_t *store, const char *key);
void kv_discard_aof_rewrite(kv_store_t *store);

void expire_and_delete_unlocked(kv_store_t *store, kv_entry_t *entry, kv_entry_t *prev, size_t index);

#endif /* KV_INTERNAL_H */
