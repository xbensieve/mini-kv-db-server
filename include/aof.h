#ifndef AOF_H
#define AOF_H

#include "kv.h"

int kv_init_with_aof(kv_store_t *store, size_t bucket_count, const char *aof_path);
void kv_set_durability(kv_store_t *store, int level);
int kv_sync(kv_store_t *store);
const char *kv_aof_path(const kv_store_t *store);
int kv_bgrewriteaof(kv_store_t *store);
int kv_merge_aof_rewrite(kv_store_t *store, pid_t child_pid);

#endif
