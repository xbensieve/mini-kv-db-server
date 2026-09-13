#ifndef LIST_H
#define LIST_H

#include "kv.h"

int kv_lpush(kv_store_t *store, const char *key, const char *value);
int kv_rpop(kv_store_t *store, const char *key, char **out_value);
int kv_llen(kv_store_t *store, const char *key);

#endif
