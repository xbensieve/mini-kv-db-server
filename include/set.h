#ifndef SET_H
#define SET_H

#include "kv.h"
#include <stddef.h>

int kv_sadd(kv_store_t *store, const char *key, const char *value);
int kv_srem(kv_store_t *store, const char *key, const char *value);
const char **kv_smembers(kv_store_t *store, const char *key, size_t *out_count);
int kv_scard(kv_store_t *store, const char *key);

#endif
