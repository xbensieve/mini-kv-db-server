#ifndef KV_H
#define KV_H

#include <stddef.h>
#include <stdio.h>

/**
 * @file kv.h
 * @brief Core in-memory key-value database storage API (Phase 01 through Phase 04).
 *
 * Implements a hash table with separate chaining, dynamic rehashing,
 * explicit memory tracking/accounting, budget caps, and Append-Only File (AOF)
 * persistence with crash recovery.
 *
 * Memory ownership conventions (docs/design/MEMORY-OWNERSHIP.md):
 * - OWN: The component/caller is responsible for allocating and freeing the buffer.
 * - BORROW: The callee receives a reference and must not free or mutate it.
 */

#define KV_MAX_KEY_LEN 256U
#define KV_MAX_VALUE_LEN 4096U
#define KV_DEFAULT_MAX_CAPACITY 0U
#define KV_DEFAULT_MEMORY_BUDGET 0U

/* Load factor threshold: 0.75 (NUM / DEN) */
#define KV_LOAD_FACTOR_NUM 3U
#define KV_LOAD_FACTOR_DEN 4U

/* Durability levels (docs/design/PERSISTENCE-AND-RECOVERY.md) */
#define KV_DURABILITY_NONE 0   /**< In-memory only; no disk write. */
#define KV_DURABILITY_FLUSH 1  /**< Mutations written and fflush() invoked. */
#define KV_DURABILITY_SYNC 2   /**< Mutations written, fflush(), and fsync() invoked. */

/* Status codes */
#define KV_OK 0
#define KV_ERR_NOT_FOUND 1
#define KV_ERR_INTERNAL (-1)
#define KV_ERR_INVALID_PARAM (-2)
#define KV_ERR_KEY_TOO_LONG (-3)
#define KV_ERR_VAL_TOO_LONG (-4)
#define KV_ERR_EMPTY_KEY (-5)
#define KV_ERR_KEY_WHITESPACE (-6)
#define KV_ERR_CAPACITY_FULL (-7)
#define KV_ERR_BUDGET_EXCEEDED (-8)
#define KV_ERR_IO (-9)

/**
 * @brief Node in a bucket's collision chain.
 */
typedef struct kv_entry {
    char *key;             /**< Null-terminated key string (OWN by entry). */
    char *value;           /**< Null-terminated value string (OWN by entry). */
    struct kv_entry *next; /**< Pointer to next entry in collision chain. */
} kv_entry_t;

/**
 * @brief Hash table key-value store with memory accounting and AOF persistence.
 */
typedef struct {
    kv_entry_t **buckets;        /**< Dynamic array of bucket head pointers (OWN by store). */
    size_t bucket_count;         /**< Total number of allocated buckets. */
    size_t size;                 /**< Current count of active key-value entries. */
    size_t max_capacity;         /**< Entry count capacity limit (0 = unbounded). */
    size_t allocated_bytes;      /**< Total heap bytes currently owned by the store. */
    size_t peak_allocated_bytes; /**< Maximum allocated_bytes observed over store lifecycle. */
    size_t max_memory_budget;    /**< Maximum memory budget in bytes (0 = unbounded). */
    int auto_resize;             /**< 1 = auto-resize enabled on load factor threshold; 0 = disabled. */
    FILE *aof_fp;                /**< Open AOF file stream (or NULL if in-memory only). */
    char *aof_path;              /**< Heap copy of AOF file path (OWN by store, or NULL). */
    int durability_level;        /**< Durability policy: KV_DURABILITY_*. */
} kv_store_t;

/**
 * @brief Initializes an in-memory key-value store with unbounded capacity and budget.
 *
 * @param store Pointer to kv_store_t structure to initialize.
 * @param bucket_count Number of hash buckets to allocate (must be > 0).
 * @return KV_OK (0) on success, or negative error code on failure.
 */
int kv_init(kv_store_t *store, size_t bucket_count);

/**
 * @brief Initializes a store with a configured maximum entry capacity.
 *
 * @param store Pointer to kv_store_t structure to initialize.
 * @param bucket_count Number of hash buckets to allocate (must be > 0).
 * @param max_capacity Maximum number of entries allowed (0 = unbounded).
 * @return KV_OK (0) on success, or negative error code on failure.
 */
int kv_init_with_capacity(kv_store_t *store, size_t bucket_count, size_t max_capacity);

/**
 * @brief Initializes a store with both entry capacity and memory budget limits.
 *
 * @param store Pointer to kv_store_t structure to initialize.
 * @param bucket_count Number of hash buckets to allocate (must be > 0).
 * @param max_capacity Maximum number of entries allowed (0 = unbounded).
 * @param max_memory_budget Maximum memory budget in bytes (0 = unbounded).
 * @return KV_OK (0) on success, or negative error code on failure.
 */
int kv_init_with_budget(kv_store_t *store, size_t bucket_count, size_t max_capacity, size_t max_memory_budget);

/**
 * @brief Initializes a store with AOF persistence and replays historical log entries.
 *
 * If aof_path exists, historical mutations are replayed sequentially into memory.
 * Subsequent mutations are appended to this file.
 *
 * @param store Pointer to kv_store_t structure to initialize.
 * @param bucket_count Number of hash buckets to allocate (must be > 0).
 * @param aof_path File path for the AOF log file (BORROW).
 * @return KV_OK (0) on success, or negative error code on failure.
 */
int kv_init_with_aof(kv_store_t *store, size_t bucket_count, const char *aof_path);

/**
 * @brief Full store initializer configuring capacity, memory budget, and AOF persistence.
 *
 * @param store Pointer to kv_store_t structure to initialize.
 * @param bucket_count Number of hash buckets to allocate (must be > 0).
 * @param max_capacity Maximum entries (0 = unbounded).
 * @param max_memory_budget Maximum memory budget in bytes (0 = unbounded).
 * @param aof_path File path for AOF log (or NULL for in-memory).
 * @return KV_OK (0) on success, or negative error code on failure.
 */
int kv_init_full(kv_store_t *store, size_t bucket_count, size_t max_capacity, size_t max_memory_budget, const char *aof_path);

/**
 * @brief Dynamically resizes the hash table bucket array and rehashes all entries.
 *
 * Accounts for the temporary memory spike while old and new arrays co-exist.
 * Defensively aborts if new allocation fails or budget is exceeded, preserving existing table.
 *
 * @param store Pointer to initialized kv_store_t.
 * @param new_bucket_count New number of buckets (must be > 0).
 * @return KV_OK (0) on success, or negative error code on failure.
 */
int kv_resize(kv_store_t *store, size_t new_bucket_count);

/**
 * @brief Configures whether automatic resizing on load factor threshold is enabled.
 *
 * @param store Pointer to initialized kv_store_t.
 * @param enabled 1 to enable auto-resizing, 0 to disable.
 */
void kv_set_auto_resize(kv_store_t *store, int enabled);

/**
 * @brief Returns current bucket count of the store.
 *
 * @param store Pointer to initialized kv_store_t.
 * @return Bucket count, or 0 if store is NULL.
 */
size_t kv_bucket_count(const kv_store_t *store);

/**
 * @brief Sets or updates the maximum entry capacity limit.
 *
 * @param store Pointer to initialized kv_store_t.
 * @param max_capacity Maximum capacity (0 = unbounded).
 */
void kv_set_max_capacity(kv_store_t *store, size_t max_capacity);

/**
 * @brief Sets or updates the maximum memory budget limit.
 *
 * @param store Pointer to initialized kv_store_t.
 * @param budget_bytes Maximum memory budget in bytes (0 = unbounded).
 */
void kv_set_memory_budget(kv_store_t *store, size_t budget_bytes);

/**
 * @brief Sets the durability policy for AOF writes.
 *
 * @param store Pointer to initialized kv_store_t.
 * @param level KV_DURABILITY_NONE, KV_DURABILITY_FLUSH, or KV_DURABILITY_SYNC.
 */
void kv_set_durability(kv_store_t *store, int level);

/**
 * @brief Forces buffered AOF data to disk via fflush() and fsync().
 *
 * @param store Pointer to initialized kv_store_t.
 * @return KV_OK (0) on success, or KV_ERR_IO on failure.
 */
int kv_sync(kv_store_t *store);

/**
 * @brief Returns the AOF log file path configured for this store.
 *
 * @param store Pointer to initialized kv_store_t.
 * @return Pointer to borrowed path string, or NULL if in-memory only.
 */
const char *kv_aof_path(const kv_store_t *store);

/**
 * @brief Returns the total heap bytes currently allocated and owned by the store.
 *
 * @param store Pointer to initialized kv_store_t.
 * @return Current allocated bytes.
 */
size_t kv_allocated_bytes(const kv_store_t *store);

/**
 * @brief Returns the peak heap bytes allocated across the store's lifecycle.
 *
 * @param store Pointer to initialized kv_store_t.
 * @return Peak allocated bytes.
 */
size_t kv_peak_allocated_bytes(const kv_store_t *store);

/**
 * @brief Returns the current memory budget limit in bytes.
 *
 * @param store Pointer to initialized kv_store_t.
 * @return Memory budget limit (0 = unbounded).
 */
size_t kv_max_memory_budget(const kv_store_t *store);

/**
 * @brief Frees all allocated memory in the store, closes AOF file, and resets fields.
 *
 * Idempotent: safe to call multiple times or on NULL/uninitialized store.
 *
 * @param store Pointer to kv_store_t to destroy.
 */
void kv_destroy(kv_store_t *store);

/**
 * @brief Sets a key-value pair in the store and appends to AOF if enabled.
 *
 * - BORROW key, BORROW value.
 * - If AOF persistence is enabled, writes and flushes/syncs to disk first.
 * - If disk write fails, in-memory state remains untouched and KV_ERR_IO is returned.
 *
 * @param store Pointer to initialized kv_store_t.
 * @param key Null-terminated key string.
 * @param value Null-terminated value string.
 * @return KV_OK (0) on success, or negative error code on failure.
 */
int kv_set(kv_store_t *store, const char *key, const char *value);

/**
 * @brief Retrieves the value for a given key.
 *
 * - BORROW key.
 * - Returns a borrowed pointer into the store's entry. The caller must not free it.
 *
 * @param store Pointer to initialized kv_store_t.
 * @param key Null-terminated key string.
 * @return Borrowed pointer to value string, or NULL if key is not found or invalid.
 */
const char *kv_get(const kv_store_t *store, const char *key);

/**
 * @brief Deletes a key and its value from the store and appends to AOF if enabled.
 *
 * @param store Pointer to initialized kv_store_t.
 * @param key Null-terminated key string to remove.
 * @return KV_OK (0) if deleted, KV_ERR_NOT_FOUND (1) if key was not found,
 *         or negative error code on invalid parameters or disk I/O failure.
 */
int kv_delete(kv_store_t *store, const char *key);

/**
 * @brief Checks if a key exists in the store.
 *
 * @param store Pointer to initialized kv_store_t.
 * @param key Null-terminated key string.
 * @return 1 if key exists, 0 if key does not exist or input is invalid.
 */
int kv_exists(const kv_store_t *store, const char *key);

/**
 * @brief Testing hook: configures simulated allocation failure after N allocations.
 *
 * @param countdown Number of successful allocations before injecting failure (-1 to disable).
 */
void kv_set_alloc_fail_countdown(int countdown);

/**
 * @brief Testing hook: configures simulated disk I/O failure.
 *
 * @param fail 1 to simulate I/O failure on mutations, 0 to operate normally.
 */
void kv_set_io_fail(int fail);

#endif
