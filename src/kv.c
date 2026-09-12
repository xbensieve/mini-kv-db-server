#define _POSIX_C_SOURCE 200809L

#include "kv.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
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
static int g_io_fail = 0;

void kv_set_io_fail(int fail) {
    g_io_fail = fail;
}

static void *tracked_malloc(size_t size) {
    if (g_alloc_fail_countdown == 0) {
        g_alloc_fail_countdown = -1;
        return NULL;
    }
    if (g_alloc_fail_countdown > 0) {
        g_alloc_fail_countdown--;
    }
    return malloc(size);
}

static void *tracked_calloc(size_t num, size_t size) {
    if (g_alloc_fail_countdown == 0) {
        g_alloc_fail_countdown = -1;
        return NULL;
    }
    if (g_alloc_fail_countdown > 0) {
        g_alloc_fail_countdown--;
    }
    return calloc(num, size);
}

static void tracked_free(void *ptr) {
    free(ptr);
}

/**
 * @brief Measures string length up to max_len + 1 without reading beyond bounds.
 */
static size_t bounded_strlen(const char *str, size_t max_len) {
    size_t len = 0U;
    while (len <= max_len && str[len] != '\0') {
        len++;
    }
    return len;
}

/**
 * @brief Validates a key string against specifications.
 *
 * Keys must be non-NULL, non-empty, <= KV_MAX_KEY_LEN, and contain no ASCII whitespace.
 */
static int validate_key(const char *key, size_t *out_len) {
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

/**
 * @brief Validates a value string against specifications.
 *
 * Values must be non-NULL and <= KV_MAX_VALUE_LEN. Empty string values are permitted.
 */
static int validate_value(const char *value, size_t *out_len) {
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

/**
 * @brief Computes 64-bit FNV-1a hash of a null-terminated key string.
 */
static size_t hash_key(const char *key, size_t bucket_count) {
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)key; *p != '\0'; ++p) {
        hash ^= (uint64_t)*p;
        hash *= 1099511628211ULL;
    }
    return (size_t)(hash % (uint64_t)bucket_count);
}

/**
 * @brief Allocates and copies a string up to max_len bytes with explicit null-termination.
 */
static char *copy_string_bounded(const char *src, size_t max_len) {
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

/* Forward declarations */
static int kv_set_internal(kv_store_t *store, const char *key, const char *value);
static int kv_delete_internal(kv_store_t *store, const char *key);

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

/**
 * @brief Strips trailing CR/LF characters from a string.
 */
static void strip_line_endings(char *line) {
    size_t len = strlen(line);
    while (len > 0U && (line[len - 1U] == '\n' || line[len - 1U] == '\r')) {
        line[--len] = '\0';
    }
}

/**
 * @brief Replays mutation records sequentially from an existing AOF log into the store.
 */
static int replay_aof_log(kv_store_t *store, const char *path) {
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
                continue; /* Corrupted/missing value separator */
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
        }
    }

    fclose(rf);
    return 0;
}

int kv_init_full(kv_store_t *store, size_t bucket_count, size_t max_capacity, size_t max_memory_budget, const char *aof_path) {
    if (store == NULL || bucket_count == 0U) {
        return KV_ERR_INVALID_PARAM;
    }
    memset(store, 0, sizeof(*store));

    size_t bucket_bytes = bucket_count * sizeof(*store->buckets);
    if (max_memory_budget > 0U && bucket_bytes > max_memory_budget) {
        return KV_ERR_BUDGET_EXCEEDED;
    }

    store->buckets = tracked_calloc(bucket_count, sizeof(*store->buckets));
    if (store->buckets == NULL) {
        return KV_ERR_INTERNAL;
    }
    store->bucket_count = bucket_count;
    store->size = 0U;
    store->max_capacity = max_capacity;
    store->max_memory_budget = max_memory_budget;
    store->allocated_bytes = bucket_bytes;
    store->peak_allocated_bytes = bucket_bytes;
    store->auto_resize = 1;
    store->durability_level = KV_DURABILITY_SYNC;

    if (aof_path != NULL) {
        size_t path_len = strlen(aof_path);
        store->aof_path = malloc(path_len + 1U);
        if (store->aof_path == NULL) {
            kv_destroy(store);
            return KV_ERR_INTERNAL;
        }
        memcpy(store->aof_path, aof_path, path_len + 1U);

        /* Replay existing log if file exists */
        replay_aof_log(store, aof_path);

        /* Open in append mode for future mutations */
        store->aof_fp = fopen(aof_path, "a+");
        if (store->aof_fp == NULL) {
            kv_destroy(store);
            return KV_ERR_IO;
        }
    }

    return KV_OK;
}

int kv_resize(kv_store_t *store, size_t new_bucket_count) {
    if (store == NULL || store->buckets == NULL || new_bucket_count == 0U) {
        return KV_ERR_INVALID_PARAM;
    }
    if (new_bucket_count == store->bucket_count) {
        return KV_OK;
    }

    size_t new_bucket_bytes = new_bucket_count * sizeof(kv_entry_t *);
    size_t old_bucket_bytes = store->bucket_count * sizeof(kv_entry_t *);

    /* Check memory budget for the temporary coexistence of both arrays */
    if (store->max_memory_budget > 0U && store->allocated_bytes + new_bucket_bytes > store->max_memory_budget) {
        return KV_ERR_BUDGET_EXCEEDED;
    }

    /* Allocate new bucket array */
    kv_entry_t **new_buckets = tracked_calloc(new_bucket_count, sizeof(*new_buckets));
    if (new_buckets == NULL) {
        return KV_ERR_INTERNAL;
    }

    /* Record temporary memory spike while both arrays co-exist */
    store->allocated_bytes += new_bucket_bytes;
    if (store->allocated_bytes > store->peak_allocated_bytes) {
        store->peak_allocated_bytes = store->allocated_bytes;
    }

    /* Migrate all existing entries into new bucket array */
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

    /* Release old bucket array and adjust memory tracking */
    tracked_free(store->buckets);
    store->buckets = new_buckets;
    store->bucket_count = new_bucket_count;
    store->allocated_bytes -= old_bucket_bytes;

    return KV_OK;
}

void kv_set_auto_resize(kv_store_t *store, int enabled) {
    if (store != NULL) {
        store->auto_resize = enabled ? 1 : 0;
    }
}

size_t kv_bucket_count(const kv_store_t *store) {
    return (store != NULL) ? store->bucket_count : 0U;
}

void kv_set_max_capacity(kv_store_t *store, size_t max_capacity) {
    if (store != NULL) {
        store->max_capacity = max_capacity;
    }
}

void kv_set_memory_budget(kv_store_t *store, size_t budget_bytes) {
    if (store != NULL) {
        store->max_memory_budget = budget_bytes;
    }
}

void kv_set_durability(kv_store_t *store, int level) {
    if (store != NULL) {
        store->durability_level = level;
    }
}

int kv_sync(kv_store_t *store) {
    if (store == NULL || store->aof_fp == NULL) {
        return KV_OK;
    }
    if (fflush(store->aof_fp) != 0) {
        return KV_ERR_IO;
    }
    int fd = fileno(store->aof_fp);
    if (fd >= 0 && fsync(fd) != 0) {
        return KV_ERR_IO;
    }
    return KV_OK;
}

const char *kv_aof_path(const kv_store_t *store) {
    return (store != NULL) ? store->aof_path : NULL;
}

size_t kv_allocated_bytes(const kv_store_t *store) {
    return (store != NULL) ? store->allocated_bytes : 0U;
}

size_t kv_peak_allocated_bytes(const kv_store_t *store) {
    return (store != NULL) ? store->peak_allocated_bytes : 0U;
}

size_t kv_max_memory_budget(const kv_store_t *store) {
    return (store != NULL) ? store->max_memory_budget : 0U;
}

void kv_destroy(kv_store_t *store) {
    if (store == NULL) {
        return;
    }
    if (store->aof_fp != NULL) {
        fflush(store->aof_fp);
        fclose(store->aof_fp);
        store->aof_fp = NULL;
    }
    if (store->aof_path != NULL) {
        free(store->aof_path);
        store->aof_path = NULL;
    }
    if (store->buckets != NULL) {
        for (size_t i = 0U; i < store->bucket_count; ++i) {
            kv_entry_t *entry = store->buckets[i];
            while (entry != NULL) {
                kv_entry_t *next = entry->next;
                tracked_free(entry->key);
                tracked_free(entry->value);
                tracked_free(entry);
                entry = next;
            }
        }
        tracked_free(store->buckets);
        store->buckets = NULL;
    }
    store->bucket_count = 0U;
    store->size = 0U;
    store->max_capacity = 0U;
    store->allocated_bytes = 0U;
    store->peak_allocated_bytes = 0U;
    store->max_memory_budget = 0U;
    store->auto_resize = 0;
    store->durability_level = 0;
}

/**
 * @brief Appends a formatted mutation record to the open AOF file.
 */
static int write_aof_record(kv_store_t *store, const char *format, const char *arg1, const char *arg2, long *out_pos) {
    if (store->aof_fp == NULL) {
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

    int ret = 0;
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

/**
 * @brief Rolls back AOF file offset and truncates written bytes on mutation failure.
 */
static void rollback_aof_record(kv_store_t *store, long pos) {
    if (store->aof_fp != NULL && pos >= 0) {
        fflush(store->aof_fp);
        int fd = fileno(store->aof_fp);
        if (fd >= 0) {
            ftruncate(fd, (off_t)pos);
        }
        fseek(store->aof_fp, pos, SEEK_SET);
    }
}

/**
 * @brief Applies in-memory SET without writing to AOF (used by kv_set and AOF replay).
 */
static int kv_set_internal(kv_store_t *store, const char *key, const char *value) {
    size_t key_len = strlen(key);
    size_t val_len = strlen(value);
    size_t index = hash_key(key, store->bucket_count);

    /* Look for existing key in bucket chain */
    for (kv_entry_t *entry = store->buckets[index]; entry != NULL; entry = entry->next) {
        if (strcmp(entry->key, key) == 0) {
            size_t old_val_bytes = strlen(entry->value) + 1U;
            size_t new_val_bytes = val_len + 1U;

            if (new_val_bytes > old_val_bytes && store->max_memory_budget > 0U) {
                size_t delta = new_val_bytes - old_val_bytes;
                if (store->allocated_bytes + delta > store->max_memory_budget) {
                    return KV_ERR_BUDGET_EXCEEDED;
                }
            }

            char *new_value = copy_string_bounded(value, KV_MAX_VALUE_LEN);
            if (new_value == NULL) {
                return KV_ERR_INTERNAL;
            }

            tracked_free(entry->value);
            entry->value = new_value;

            store->allocated_bytes = store->allocated_bytes - old_val_bytes + new_val_bytes;
            if (store->allocated_bytes > store->peak_allocated_bytes) {
                store->peak_allocated_bytes = store->allocated_bytes;
            }
            return KV_OK;
        }
    }

    /* Key is new: enforce capacity limits */
    if (store->max_capacity > 0U && store->size >= store->max_capacity) {
        return KV_ERR_CAPACITY_FULL;
    }

    /* Check load factor and trigger auto-resizing if threshold (0.75) is exceeded */
    if (store->auto_resize && (store->size + 1U) * KV_LOAD_FACTOR_DEN > store->bucket_count * KV_LOAD_FACTOR_NUM) {
        size_t new_count = store->bucket_count * 2U;
        if (new_count > store->bucket_count) {
            int resize_res = kv_resize(store, new_count);
            if (resize_res == KV_OK) {
                index = hash_key(key, store->bucket_count);
            }
        }
    }

    /* Check memory budget for new entry */
    size_t entry_bytes = sizeof(kv_entry_t) + (key_len + 1U) + (val_len + 1U);
    if (store->max_memory_budget > 0U && store->allocated_bytes + entry_bytes > store->max_memory_budget) {
        return KV_ERR_BUDGET_EXCEEDED;
    }

    kv_entry_t *entry = tracked_calloc(1U, sizeof(*entry));
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

    entry->next = store->buckets[index];
    store->buckets[index] = entry;
    store->size++;
    store->allocated_bytes += entry_bytes;
    if (store->allocated_bytes > store->peak_allocated_bytes) {
        store->peak_allocated_bytes = store->allocated_bytes;
    }
    return KV_OK;
}

int kv_set(kv_store_t *store, const char *key, const char *value) {
    if (store == NULL || store->buckets == NULL) {
        return KV_ERR_INVALID_PARAM;
    }

    size_t key_len = 0U;
    int key_err = validate_key(key, &key_len);
    if (key_err != KV_OK) {
        return key_err;
    }

    size_t val_len = 0U;
    int val_err = validate_value(value, &val_len);
    if (val_err != KV_OK) {
        return val_err;
    }

    /* Step 1: Write mutation to AOF before modifying memory */
    long pos = -1;
    if (store->aof_fp != NULL) {
        int io_err = write_aof_record(store, "SET\t%s\t%s\n", key, value, &pos);
        if (io_err != KV_OK) {
            if (pos >= 0) {
                rollback_aof_record(store, pos);
            }
            return io_err;
        }
    }

    /* Step 2: Apply in-memory mutation */
    int mem_res = kv_set_internal(store, key, value);
    if (mem_res != KV_OK) {
        /* Roll back disk write if in-memory update failed */
        if (store->aof_fp != NULL && pos >= 0) {
            rollback_aof_record(store, pos);
        }
        return mem_res;
    }

    return KV_OK;
}

const char *kv_get(const kv_store_t *store, const char *key) {
    if (store == NULL || store->buckets == NULL) {
        return NULL;
    }
    if (validate_key(key, NULL) != KV_OK) {
        return NULL;
    }

    size_t index = hash_key(key, store->bucket_count);
    for (const kv_entry_t *entry = store->buckets[index]; entry != NULL; entry = entry->next) {
        if (strcmp(entry->key, key) == 0) {
            return entry->value;
        }
    }
    return NULL;
}

/**
 * @brief Applies in-memory DELETE without writing to AOF (used by kv_delete and AOF replay).
 */
static int kv_delete_internal(kv_store_t *store, const char *key) {
    size_t index = hash_key(key, store->bucket_count);
    kv_entry_t **cursor = &store->buckets[index];
    while (*cursor != NULL) {
        kv_entry_t *entry = *cursor;
        if (strcmp(entry->key, key) == 0) {
            *cursor = entry->next;
            size_t entry_bytes = sizeof(kv_entry_t) + strlen(entry->key) + 1U + strlen(entry->value) + 1U;
            tracked_free(entry->key);
            tracked_free(entry->value);
            tracked_free(entry);
            store->size--;
            store->allocated_bytes -= entry_bytes;
            return KV_OK;
        }
        cursor = &entry->next;
    }
    return KV_ERR_NOT_FOUND;
}

int kv_delete(kv_store_t *store, const char *key) {
    if (store == NULL || store->buckets == NULL) {
        return KV_ERR_INVALID_PARAM;
    }
    if (validate_key(key, NULL) != KV_OK) {
        return KV_ERR_INVALID_PARAM;
    }

    /* Check if key exists; no disk write if not found */
    if (!kv_exists(store, key)) {
        return KV_ERR_NOT_FOUND;
    }

    /* Write DELETE to AOF before modifying memory */
    long pos = -1;
    if (store->aof_fp != NULL) {
        int io_err = write_aof_record(store, "DELETE\t%s\n", key, NULL, &pos);
        if (io_err != KV_OK) {
            if (pos >= 0) {
                rollback_aof_record(store, pos);
            }
            return io_err;
        }
    }

    return kv_delete_internal(store, key);
}

int kv_exists(const kv_store_t *store, const char *key) {
    return kv_get(store, key) != NULL ? 1 : 0;
}
