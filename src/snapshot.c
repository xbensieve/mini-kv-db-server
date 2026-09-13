#define _POSIX_C_SOURCE 200809L

#include "snapshot.h"
#include "kv_internal.h"
#include "eviction.h"
#include "list.h"
#include "set.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define SNAPSHOT_MAGIC "MINIKV01"
#define SNAPSHOT_MAGIC_LEN 8U

static uint32_t adler32(uint32_t adler, const unsigned char *buf, size_t len) {
    uint32_t s1 = adler & 0xffffU;
    uint32_t s2 = (adler >> 16) & 0xffffU;
    for (size_t n = 0; n < len; n++) {
        s1 = (s1 + buf[n]) % 65521U;
        s2 = (s2 + s1) % 65521U;
    }
    return (s2 << 16) | s1;
}

static size_t write_uint64(FILE *fp, uint64_t val, uint32_t *checksum) {
    unsigned char buf[8];
    for (int i = 0; i < 8; i++) {
        buf[i] = (unsigned char)(val & 0xFFU);
        val >>= 8;
    }
    *checksum = adler32(*checksum, buf, 8U);
    return fwrite(buf, 1, 8, fp);
}

static size_t write_uint32(FILE *fp, uint32_t val, uint32_t *checksum) {
    unsigned char buf[4];
    for (int i = 0; i < 4; i++) {
        buf[i] = (unsigned char)(val & 0xFFU);
        val >>= 8;
    }
    *checksum = adler32(*checksum, buf, 4U);
    return fwrite(buf, 1, 4, fp);
}

static size_t write_bytes(FILE *fp, const char *buf, size_t len, uint32_t *checksum) {
    *checksum = adler32(*checksum, (const unsigned char *)buf, len);
    return fwrite(buf, 1, len, fp);
}

static int read_uint64(FILE *fp, uint64_t *val, uint32_t *checksum) {
    unsigned char buf[8];
    if (fread(buf, 1, 8, fp) != 8U) return -1;
    *checksum = adler32(*checksum, buf, 8U);
    *val = 0U;
    for (int i = 0; i < 8; i++) {
        *val |= ((uint64_t)buf[i]) << (8 * i);
    }
    return 0;
}

static int read_uint32(FILE *fp, uint32_t *val, uint32_t *checksum) {
    unsigned char buf[4];
    if (fread(buf, 1, 4, fp) != 4U) return -1;
    *checksum = adler32(*checksum, buf, 4U);
    *val = 0U;
    for (int i = 0; i < 4; i++) {
        *val |= ((uint32_t)buf[i]) << (8 * i);
    }
    return 0;
}

static int read_bytes(FILE *fp, char *buf, size_t len, uint32_t *checksum) {
    if (fread(buf, 1, len, fp) != len) return -1;
    *checksum = adler32(*checksum, (const unsigned char *)buf, len);
    return 0;
}

static int kv_save_internal_unlocked(const kv_store_t *store, const char *filename) {
    char tmp_file[512];
    snprintf(tmp_file, sizeof(tmp_file), "%s.tmp", filename);

    FILE *fp = fopen(tmp_file, "wb");
    if (!fp) {
        return KV_ERR_IO;
    }

    uint32_t checksum = 1U;
    if (write_bytes(fp, SNAPSHOT_MAGIC, SNAPSHOT_MAGIC_LEN, &checksum) != SNAPSHOT_MAGIC_LEN) {
        fclose(fp);
        unlink(tmp_file);
        return KV_ERR_IO;
    }

    int64_t now_ms = kv_current_time_ms();

    for (size_t i = 0U; i < store->bucket_count; ++i) {
        kv_entry_t *entry = store->buckets[i];
        while (entry != NULL) {
            if (entry->expire_at_ms == 0 || entry->expire_at_ms > now_ms) {
                uint32_t klen = (uint32_t)strlen(entry->key);

                write_uint64(fp, (uint64_t)entry->expire_at_ms, &checksum);
                write_uint32(fp, klen, &checksum);
                write_bytes(fp, entry->key, klen, &checksum);

                uint8_t type = (uint8_t)entry->type;
                write_bytes(fp, (const char *)&type, 1U, &checksum);

                if (entry->type == KV_TYPE_STRING) {
                    uint32_t vlen = (uint32_t)strlen(entry->value);
                    write_uint32(fp, vlen, &checksum);
                    write_bytes(fp, entry->value, vlen, &checksum);
                } else if (entry->type == KV_TYPE_LIST) {
                    kv_list_t *list = (kv_list_t *)entry->value_ptr;
                    write_uint32(fp, (uint32_t)list->size, &checksum);
                    kv_list_node_t *node = list->tail;
                    while (node != NULL) {
                        uint32_t elen = (uint32_t)strlen(node->value);
                        write_uint32(fp, elen, &checksum);
                        write_bytes(fp, node->value, elen, &checksum);
                        node = node->prev;
                    }
                } else if (entry->type == KV_TYPE_SET) {
                    kv_set_t *set = (kv_set_t *)entry->value_ptr;
                    write_uint32(fp, (uint32_t)set->size, &checksum);
                    for (size_t b = 0U; b < set->bucket_count; ++b) {
                        kv_set_node_t *node = set->buckets[b];
                        while (node != NULL) {
                            uint32_t elen = (uint32_t)strlen(node->value);
                            write_uint32(fp, elen, &checksum);
                            write_bytes(fp, node->value, elen, &checksum);
                            node = node->next;
                        }
                    }
                }
            }
            entry = entry->next;
        }
    }

    /* End of file marker: (uint64_t)-1 */
    write_uint64(fp, (uint64_t)-1, &checksum);

    /* Write checksum */
    unsigned char csum_buf[4];
    for (int i = 0; i < 4; i++) {
        csum_buf[i] = (unsigned char)(checksum & 0xFFU);
        checksum >>= 8;
    }
    if (fwrite(csum_buf, 1, 4, fp) != 4U) {
        fclose(fp);
        unlink(tmp_file);
        return KV_ERR_IO;
    }

    fflush(fp);
    int fd = fileno(fp);
    if (fd >= 0) {
        fsync(fd);
    }
    fclose(fp);

    if (rename(tmp_file, filename) != 0) {
        unlink(tmp_file);
        return KV_ERR_IO;
    }

    return KV_OK;
}

int kv_save(kv_store_t *store, const char *filename) {
    if (store == NULL || filename == NULL) {
        return KV_ERR_INVALID_PARAM;
    }

    kv_acquire_all_locks(store);
    int res = kv_save_internal_unlocked(store, filename);
    kv_release_all_locks(store);
    return res;
}

int kv_bgsave(kv_store_t *store, const char *filename) {
    if (store == NULL || filename == NULL) {
        return KV_ERR_INVALID_PARAM;
    }

    kv_acquire_all_locks(store);

    if (store->bgsave_pid != 0 || store->aof_rewrite_pid != 0) {
        kv_release_all_locks(store);
        return KV_ERR_AGAIN;
    }

    pid_t pid = fork();
    if (pid < 0) {
        kv_release_all_locks(store);
        return KV_ERR_IO;
    } else if (pid == 0) {
        /* Child process: isolated address space, strictly NO mutex locking */
        int res = kv_save_internal_unlocked(store, filename);
        _exit(res == KV_OK ? 0 : 1);
    } else {
        /* Parent process: record child pid and release mutexes */
        store->bgsave_pid = pid;
        kv_release_all_locks(store);
        return KV_OK;
    }
}

int kv_load_snapshot(kv_store_t *store, const char *filename) {
    if (store == NULL || filename == NULL) {
        return KV_ERR_INVALID_PARAM;
    }

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        return KV_ERR_IO;
    }

    uint32_t checksum = 1U;
    char magic[SNAPSHOT_MAGIC_LEN];
    if (fread(magic, 1, SNAPSHOT_MAGIC_LEN, fp) != SNAPSHOT_MAGIC_LEN) {
        fclose(fp);
        return KV_ERR_IO;
    }
    checksum = adler32(checksum, (const unsigned char *)magic, SNAPSHOT_MAGIC_LEN);

    if (memcmp(magic, SNAPSHOT_MAGIC, SNAPSHOT_MAGIC_LEN) != 0) {
        fclose(fp);
        return KV_ERR_IO;
    }

    int64_t now_ms = kv_current_time_ms();

    while (1) {
        uint64_t expire_at_ms = 0U;
        if (read_uint64(fp, &expire_at_ms, &checksum) != 0) goto err;

        if (expire_at_ms == (uint64_t)-1) {
            break; /* EOF marker */
        }

        uint32_t klen = 0U;
        if (read_uint32(fp, &klen, &checksum) != 0) goto err;
        if (klen > KV_MAX_KEY_LEN) goto err;

        char *key = malloc(klen + 1U);
        if (!key) goto err;
        if (read_bytes(fp, key, klen, &checksum) != 0) {
            free(key);
            goto err;
        }
        key[klen] = '\0';

        char type_byte = 0;
        if (read_bytes(fp, &type_byte, 1U, &checksum) != 0) {
            free(key);
            goto err;
        }

        int valid = (expire_at_ms == 0 || (int64_t)expire_at_ms > now_ms);

        if ((kv_type_t)type_byte == KV_TYPE_STRING) {
            uint32_t vlen = 0U;
            if (read_uint32(fp, &vlen, &checksum) != 0) { free(key); goto err; }
            if (vlen > KV_MAX_VALUE_LEN) { free(key); goto err; }
            char *val = malloc(vlen + 1U);
            if (!val) { free(key); goto err; }
            if (read_bytes(fp, val, vlen, &checksum) != 0) { free(key); free(val); goto err; }
            val[vlen] = '\0';

            if (valid) {
                if (kv_set(store, key, val) == KV_OK && expire_at_ms != 0) {
                    kv_set_expire_at_ms_internal(store, key, (int64_t)expire_at_ms);
                }
            }
            free(val);
        } else if ((kv_type_t)type_byte == KV_TYPE_LIST || (kv_type_t)type_byte == KV_TYPE_SET) {
            uint32_t count = 0U;
            if (read_uint32(fp, &count, &checksum) != 0) { free(key); goto err; }
            for (uint32_t i = 0; i < count; i++) {
                uint32_t elen = 0U;
                if (read_uint32(fp, &elen, &checksum) != 0) { free(key); goto err; }
                if (elen > KV_MAX_VALUE_LEN) { free(key); goto err; }
                char *val = malloc(elen + 1U);
                if (!val) { free(key); goto err; }
                if (read_bytes(fp, val, elen, &checksum) != 0) { free(key); free(val); goto err; }
                val[elen] = '\0';

                if (valid) {
                    if ((kv_type_t)type_byte == KV_TYPE_LIST) {
                        kv_lpush(store, key, val);
                    } else {
                        kv_sadd(store, key, val);
                    }
                }
                free(val);
            }
            if (valid && count > 0U && expire_at_ms != 0) {
                kv_set_expire_at_ms_internal(store, key, (int64_t)expire_at_ms);
            }
        }
        free(key);
    }

    unsigned char csum_buf[4];
    if (fread(csum_buf, 1, 4, fp) != 4U) goto err;

    uint32_t expected_checksum = 0U;
    for (int i = 0; i < 4; i++) {
        expected_checksum |= ((uint32_t)csum_buf[i]) << (8 * i);
    }

    if (checksum != expected_checksum) {
        fclose(fp);
        return KV_ERR_IO;
    }

    fclose(fp);
    return KV_OK;

err:
    fclose(fp);
    return KV_ERR_IO;
}
