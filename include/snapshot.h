#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include "kv.h"

int kv_save(kv_store_t *store, const char *filename);
int kv_bgsave(kv_store_t *store, const char *filename);
int kv_load_snapshot(kv_store_t *store, const char *filename);

#endif
