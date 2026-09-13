#define _POSIX_C_SOURCE 200809L

#include "aof.h"
#include "kv_internal.h"
#include "list.h"
#include "set.h"
#include "eviction.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern void kv_set_io_fail(int fail);

static void strip_line_endings(char *line) {
    size_t len = strlen(line);
    while (len > 0U && (line[len - 1U] == '\n' || line[len - 1U] == '\r')) {
        line[--len] = '\0';
    }
}

int replay_aof_log(kv_store_t *store, const char *path) {
    if (store == NULL || path == NULL) {
        return 0;
    }
    FILE *rf = fopen(path, "r");
    if (rf == NULL) {
        return 0;
    }

    char line[8192];
    while (fgets(line, sizeof(line), rf) != NULL) {
        strip_line_endings(line);
        if (line[0] == '\0') {
            continue;
        }

        if (strncmp(line, "SET\t", 4) == 0 || strncmp(line, "SET ", 4) == 0) {
            char delim = line[3];
            char *key_start = line + 4;
            char *sep = strchr(key_start, delim);
            if (sep == NULL) {
                continue;
            }
            *sep = '\0';
            const char *key = key_start;
            const char *val = sep + 1;
            if (validate_key(key, NULL) == KV_OK && validate_value(val, NULL) == KV_OK) {
                kv_set_internal(store, key, val);
            }
        } else if (strncmp(line, "DELETE\t", 7) == 0 || strncmp(line, "DELETE ", 7) == 0) {
            char delim = line[6];
            char *key = line + 7;
            char *sep = strchr(key, delim);
            if (sep != NULL) {
                *sep = '\0';
            }
            if (validate_key(key, NULL) == KV_OK) {
                kv_delete_internal(store, key);
            }
        } else if (strncmp(line, "LPUSH\t", 6) == 0 || strncmp(line, "LPUSH ", 6) == 0) {
            char delim = line[5];
            char *key_start = line + 6;
            char *sep = strchr(key_start, delim);
            if (sep == NULL) {
                continue;
            }
            *sep = '\0';
            const char *key = key_start;
            const char *val = sep + 1;
            if (validate_key(key, NULL) == KV_OK && validate_value(val, NULL) == KV_OK) {
                kv_lpush(store, key, val);
            }
        } else if (strncmp(line, "RPOP\t", 5) == 0 || strncmp(line, "RPOP ", 5) == 0) {
            char delim = line[4];
            char *key = line + 5;
            char *sep = strchr(key, delim);
            if (sep != NULL) {
                *sep = '\0';
            }
            if (validate_key(key, NULL) == KV_OK) {
                char *popped = NULL;
                kv_rpop(store, key, &popped);
                if (popped != NULL) {
                    free(popped);
                }
            }
        } else if (strncmp(line, "SADD\t", 5) == 0 || strncmp(line, "SADD ", 5) == 0) {
            char delim = line[4];
            char *key_start = line + 5;
            char *sep = strchr(key_start, delim);
            if (sep == NULL) {
                continue;
            }
            *sep = '\0';
            const char *key = key_start;
            const char *val = sep + 1;
            if (validate_key(key, NULL) == KV_OK && validate_value(val, NULL) == KV_OK) {
                kv_sadd(store, key, val);
            }
        } else if (strncmp(line, "SREM\t", 5) == 0 || strncmp(line, "SREM ", 5) == 0) {
            char delim = line[4];
            char *key_start = line + 5;
            char *sep = strchr(key_start, delim);
            if (sep == NULL) {
                continue;
            }
            *sep = '\0';
            const char *key = key_start;
            const char *val = sep + 1;
            if (validate_key(key, NULL) == KV_OK && validate_value(val, NULL) == KV_OK) {
                kv_srem(store, key, val);
            }
        } else if (strncmp(line, "PEXPIREAT\t", 10) == 0 || strncmp(line, "PEXPIREAT ", 10) == 0) {
            char delim = line[9];
            char *key_start = line + 10;
            char *sep = strchr(key_start, delim);
            if (sep == NULL) {
                continue;
            }
            *sep = '\0';
            const char *key = key_start;
            const char *ts_str = sep + 1;
            if (validate_key(key, NULL) == KV_OK) {
                int64_t exp_ms = strtoll(ts_str, NULL, 10);
                kv_set_expire_at_ms_internal(store, key, exp_ms);
            }
        }
    }

    fclose(rf);
    return 0;
}

void kv_set_durability(kv_store_t *store, int level) {
    if (store != NULL) {
        pthread_mutex_lock(&store->lock);
        store->durability_level = level;
        pthread_mutex_unlock(&store->lock);
    }
}

int kv_sync(kv_store_t *store) {
    if (store == NULL) {
        return KV_ERR_INVALID_PARAM;
    }
    pthread_mutex_lock(&store->lock);
    if (g_io_fail) {
        pthread_mutex_unlock(&store->lock);
        return KV_ERR_IO;
    }
    if (store->aof_fp == NULL) {
        pthread_mutex_unlock(&store->lock);
        return KV_OK;
    }
    if (fflush(store->aof_fp) != 0) {
        pthread_mutex_unlock(&store->lock);
        return KV_ERR_IO;
    }
    int fd = fileno(store->aof_fp);
    if (fd >= 0 && fsync(fd) != 0) {
        pthread_mutex_unlock(&store->lock);
        return KV_ERR_IO;
    }
    pthread_mutex_unlock(&store->lock);
    return KV_OK;
}

const char *kv_aof_path(const kv_store_t *store) {
    return (store != NULL) ? store->aof_path : NULL;
}

int write_aof_record(kv_store_t *store, const char *format, const char *arg1, const char *arg2, long *out_pos) {
    if (store == NULL || store->aof_fp == NULL) {
        return KV_OK;
    }
    if (g_io_fail) {
        return KV_ERR_IO;
    }

    long pos = ftell(store->aof_fp);
    if (pos < 0) {
        return KV_ERR_IO;
    }
    if (out_pos != NULL) {
        *out_pos = pos;
    }

    int ret;
    if (arg2 != NULL) {
        ret = fprintf(store->aof_fp, format, arg1, arg2);
    } else {
        ret = fprintf(store->aof_fp, format, arg1);
    }
    if (ret < 0) {
        return KV_ERR_IO;
    }

    if (store->durability_level >= KV_DURABILITY_FLUSH) {
        if (fflush(store->aof_fp) != 0) {
            return KV_ERR_IO;
        }
    }
    if (store->durability_level >= KV_DURABILITY_SYNC) {
        int fd = fileno(store->aof_fp);
        if (fd >= 0 && fsync(fd) != 0) {
            return KV_ERR_IO;
        }
    }
    return KV_OK;
}

int write_aof_pexpireat(kv_store_t *store, const char *key, int64_t expire_ms, long *out_pos) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRId64, expire_ms);
    return write_aof_record(store, "PEXPIREAT\t%s\t%s\n", key, buf, out_pos);
}

void rollback_aof_record(kv_store_t *store, long pos) {
    if (store != NULL && store->aof_fp != NULL && pos >= 0) {
        fflush(store->aof_fp);
        int fd = fileno(store->aof_fp);
        if (fd >= 0) {
            if (ftruncate(fd, (off_t)pos) != 0) {
                /* Rollback failure */
            }
        }
        fseek(store->aof_fp, pos, SEEK_SET);
    }
}

int kv_bgrewriteaof(kv_store_t *store) {
    if (store == NULL || store->aof_path == NULL) {
        return KV_ERR_INVALID_PARAM;
    }

    pthread_mutex_lock(&store->lock);

    if (store->bgsave_pid > 0 || store->aof_rewrite_pid > 0) {
        pthread_mutex_unlock(&store->lock);
        return KV_ERR_AGAIN;
    }

    if (store->aof_fp != NULL) {
        fflush(store->aof_fp);
    }

    if (store->aof_rewrite_buf != NULL) {
        store->aof_rewrite_buf_len = 0U;
    }

    pid_t pid = fork();
    if (pid < 0) {
        pthread_mutex_unlock(&store->lock);
        return KV_ERR_IO;
    } else if (pid == 0) {
        /* Child process */
        char tmp_path[512];
        snprintf(tmp_path, sizeof(tmp_path), "temp-rewrite-aof-%d.aof", (int)getpid());

        FILE *tf = fopen(tmp_path, "w");
        if (tf == NULL) {
            _exit(1);
        }

        int64_t now_ms = kv_current_time_ms();

        for (size_t i = 0U; i < store->bucket_count; ++i) {
            kv_entry_t *entry = store->buckets[i];
            while (entry != NULL) {
                if (!entry_is_expired(entry, now_ms)) {
                    if (entry->type == KV_TYPE_STRING) {
                        fprintf(tf, "SET\t%s\t%s\n", entry->key, entry->value);
                    } else if (entry->type == KV_TYPE_LIST) {
                        kv_list_t *list = (kv_list_t *)entry->value_ptr;
                        kv_list_node_t *node = list->tail;
                        while (node != NULL) {
                            fprintf(tf, "LPUSH\t%s\t%s\n", entry->key, node->value);
                            node = node->prev;
                        }
                    } else if (entry->type == KV_TYPE_SET) {
                        kv_set_t *set = (kv_set_t *)entry->value_ptr;
                        for (size_t b = 0U; b < set->bucket_count; ++b) {
                            kv_set_node_t *snode = set->buckets[b];
                            while (snode != NULL) {
                                fprintf(tf, "SADD\t%s\t%s\n", entry->key, snode->value);
                                snode = snode->next;
                            }
                        }
                    }

                    if (entry->expire_at_ms > 0) {
                        fprintf(tf, "PEXPIREAT\t%s\t%" PRId64 "\n", entry->key, entry->expire_at_ms);
                    }
                }
                entry = entry->next;
            }
        }

        fflush(tf);
        int fd = fileno(tf);
        if (fd >= 0) {
            fsync(fd);
        }
        fclose(tf);
        _exit(0);
    } else {
        /* Parent process */
        store->aof_rewrite_pid = pid;
        pthread_mutex_unlock(&store->lock);
        return KV_OK;
    }
}

int kv_merge_aof_rewrite(kv_store_t *store, pid_t child_pid) {
    if (store == NULL || store->aof_path == NULL || child_pid <= 0) {
        return KV_ERR_INVALID_PARAM;
    }

    pthread_mutex_lock(&store->lock);

    if (store->aof_rewrite_pid != child_pid) {
        pthread_mutex_unlock(&store->lock);
        return KV_ERR_INVALID_PARAM;
    }

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "temp-rewrite-aof-%d.aof", (int)child_pid);

    FILE *tf = fopen(tmp_path, "a");
    if (tf == NULL) {
        store->aof_rewrite_pid = 0;
        store->aof_rewrite_buf_len = 0U;
        pthread_mutex_unlock(&store->lock);
        return KV_ERR_IO;
    }

    if (store->aof_rewrite_buf != NULL && store->aof_rewrite_buf_len > 0U) {
        if (fwrite(store->aof_rewrite_buf, 1, store->aof_rewrite_buf_len, tf) != store->aof_rewrite_buf_len) {
            fclose(tf);
            store->aof_rewrite_pid = 0;
            store->aof_rewrite_buf_len = 0U;
            pthread_mutex_unlock(&store->lock);
            return KV_ERR_IO;
        }
    }

    fflush(tf);
    int fd = fileno(tf);
    if (fd >= 0) {
        fsync(fd);
    }
    fclose(tf);

    if (store->aof_fp != NULL) {
        fclose(store->aof_fp);
        store->aof_fp = NULL;
    }

    if (rename(tmp_path, store->aof_path) != 0) {
        store->aof_rewrite_pid = 0;
        store->aof_rewrite_buf_len = 0U;
        pthread_mutex_unlock(&store->lock);
        return KV_ERR_IO;
    }

    store->aof_fp = fopen(store->aof_path, "a+");
    store->aof_rewrite_pid = 0;
    store->aof_rewrite_buf_len = 0U;

    if (store->aof_fp == NULL) {
        pthread_mutex_unlock(&store->lock);
        return KV_ERR_IO;
    }

    pthread_mutex_unlock(&store->lock);
    return KV_OK;
}
