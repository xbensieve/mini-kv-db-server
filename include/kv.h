#ifndef KV_H
#define KV_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/types.h>

/**
 * @file kv.h
 * @brief Core in-memory key-value database storage API.
 *
 * Implements a hash table with separate chaining, dynamic rehashing,
 * explicit memory tracking/accounting, budget caps, Append-Only File (AOF)
 * persistence, snapshotting, TTL/eviction, and polymorphic types (String, List, Set).
 */

#define KV_MAX_KEY_LEN 256U
#define KV_MAX_VALUE_LEN 4096U
#define KV_DEFAULT_MAX_CAPACITY 0U
#define KV_DEFAULT_MEMORY_BUDGET 0U

/* Load factor threshold: 0.75 (NUM / DEN) */
#define KV_LOAD_FACTOR_NUM 3U
#define KV_LOAD_FACTOR_DEN 4U

/* Durability levels */
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
#define KV_ERR_AGAIN (-10)
#define KV_ERR_WRONG_TYPE (-11)

/**
 * @brief Polymorphic entry type tag.
 */
typedef enum {
    KV_TYPE_STRING = 0,
    KV_TYPE_LIST = 1,
    KV_TYPE_SET = 2
} kv_type_t;

/**
 * @brief Doubly-linked list node for list data structure.
 */
typedef struct kv_list_node {
    char *value;
    struct kv_list_node *prev;
    struct kv_list_node *next;
} kv_list_node_t;

/**
 * @brief Doubly-linked list container.
 */
typedef struct kv_list {
    kv_list_node_t *head;
    kv_list_node_t *tail;
    union {
        size_t count;
        size_t size;
    };
} kv_list_t;

/**
 * @brief Hash set bucket collision node.
 */
typedef struct kv_set_node {
    char *value;
    struct kv_set_node *next;
} kv_set_node_t;

/**
 * @brief Hash set container.
 */
typedef struct kv_set {
    kv_set_node_t **buckets;
    size_t bucket_count;
    union {
        size_t count;
        size_t size;
    };
} kv_set_t;

/**
 * @brief Node in a bucket's collision chain.
 */
typedef struct kv_entry {
    char *key;                    /**< Null-terminated key string (OWN by entry). */
    union {
        void *value_ptr;          /**< Pointer to payload (String, List, or Set). */
        char *value;              /**< Compatibility alias for String values. */
    };
    kv_type_t type;               /**< Entry type (String, List, Set). */
    int64_t expire_at_ms;         /**< Absolute expiration in ms (0 = persistent). */
    uint64_t last_accessed_time;  /**< LRU access counter. */
    struct kv_entry *next;        /**< Pointer to next entry in collision chain. */
} kv_entry_t;

#include <stdatomic.h>

/**
 * @brief Hash table key-value store with granular lock sharding, accounting, and persistence.
 */
typedef struct kv_store {
    kv_entry_t **buckets;        /**< Dynamic array of bucket head pointers (OWN by store). */
    _Atomic size_t bucket_count;         /**< Total number of allocated buckets. */
    _Atomic size_t size;                 /**< Current count of active key-value entries. */
    _Atomic size_t max_capacity;         /**< Entry count capacity limit (0 = unbounded). */
    _Atomic size_t allocated_bytes;      /**< Total heap bytes currently owned by the store. */
    _Atomic size_t peak_allocated_bytes; /**< Maximum allocated_bytes observed over store lifecycle. */
    _Atomic size_t max_memory_budget;    /**< Maximum memory budget in bytes (0 = unbounded). */
    _Atomic int auto_resize;             /**< 1 = auto-resize enabled on load factor threshold; 0 = disabled. */
    FILE *aof_fp;                /**< Open AOF file stream (or NULL if in-memory only). */
    char *aof_path;              /**< Heap copy of AOF file path (OWN by store, or NULL). */
    int durability_level;        /**< Durability policy: KV_DURABILITY_*. */
    pthread_mutex_t *bucket_locks; /**< Sharded recursive mutex array protecting buckets and entries. */
    size_t num_locks;             /**< Total number of bucket lock shards. */
    pthread_mutex_t aof_lock;     /**< Mutex protecting AOF file writes and rewrite mutation buffer. */
    int lock_initialized;        /**< 1 if bucket and AOF locks were initialized. */
    _Atomic uint64_t lru_clock;  /**< Monotonic access sequence for LRU. */
    pid_t bgsave_pid;            /**< Active BGSAVE child process PID (0 if none). */
    pid_t aof_rewrite_pid;       /**< Active BGREWRITEAOF child process PID (0 if none). */
    char *aof_rewrite_buf;       /**< Buffer accumulating mutations during BGREWRITEAOF. */
    size_t aof_rewrite_buf_len;  /**< Byte length in rewrite buffer. */
    size_t aof_rewrite_buf_cap;  /**< Capacity of rewrite buffer. */
} kv_store_t;

/**
 * @brief Initializes an in-memory key-value store with unbounded capacity and budget.
 */
int kv_init(kv_store_t *store, size_t bucket_count);

/**
 * @brief Initializes a store with a configured maximum entry capacity.
 */
int kv_init_with_capacity(kv_store_t *store, size_t bucket_count, size_t max_capacity);

/**
 * @brief Initializes a store with both entry capacity and memory budget limits.
 */
int kv_init_with_budget(kv_store_t *store, size_t bucket_count, size_t max_capacity, size_t max_memory_budget);

/**
 * @brief Initializes a store with AOF persistence and replays historical log entries.
 */
int kv_init_with_aof(kv_store_t *store, size_t bucket_count, const char *aof_path);

/**
 * @brief Full store initializer configuring capacity, memory budget, and AOF persistence.
 */
int kv_init_full(kv_store_t *store, size_t bucket_count, size_t max_capacity, size_t max_memory_budget, const char *aof_path);

/**
 * @brief Dynamically resizes the hash table bucket array and rehashes all entries.
 */
int kv_resize(kv_store_t *store, size_t new_bucket_count);

/**
 * @brief Configures whether automatic resizing on load factor threshold is enabled.
 */
void kv_set_auto_resize(kv_store_t *store, int enabled);

/**
 * @brief Returns current bucket count of the store.
 */
size_t kv_bucket_count(const kv_store_t *store);

/**
 * @brief Sets or updates the maximum entry capacity limit.
 */
void kv_set_max_capacity(kv_store_t *store, size_t max_capacity);

/**
 * @brief Sets or updates the maximum memory budget limit.
 */
void kv_set_memory_budget(kv_store_t *store, size_t budget_bytes);

/**
 * @brief Sets the durability policy for AOF writes.
 */
void kv_set_durability(kv_store_t *store, int level);

/**
 * @brief Forces buffered AOF data to disk via fflush() and fsync().
 */
int kv_sync(kv_store_t *store);

/**
 * @brief Returns the AOF log file path configured for this store.
 */
const char *kv_aof_path(const kv_store_t *store);

/**
 * @brief Returns the total heap bytes currently allocated and owned by the store.
 */
size_t kv_allocated_bytes(const kv_store_t *store);

/**
 * @brief Returns the peak heap bytes allocated across the store's lifecycle.
 */
size_t kv_peak_allocated_bytes(const kv_store_t *store);

/**
 * @brief Returns the current memory budget limit in bytes.
 */
size_t kv_max_memory_budget(const kv_store_t *store);

/**
 * @brief Frees all allocated memory in the store, closes AOF file, and resets fields.
 */
void kv_destroy(kv_store_t *store);

/**
 * @brief Sets a string key-value pair in the store and appends to AOF if enabled.
 */
int kv_set(kv_store_t *store, const char *key, const char *value);

/**
 * @brief Retrieves the string value for a given key.
 */
const char *kv_get(const kv_store_t *store, const char *key);

/**
 * @brief Deletes a key and its value from the store and appends to AOF if enabled.
 */
int kv_delete(kv_store_t *store, const char *key);

/**
 * @brief Checks if a key exists in the store.
 */
int kv_exists(const kv_store_t *store, const char *key);

/**
 * @brief Returns current real time in milliseconds since epoch.
 */
int64_t kv_current_time_ms(void);

/**
 * @brief Testing hook: configures simulated allocation failure after N allocations.
 */
void kv_set_alloc_fail_countdown(int countdown);

/**
 * @brief Testing hook: configures simulated disk I/O failure.
 */
void kv_set_io_fail(int fail);

/**
 * @brief Acquires the specific bucket shard lock corresponding to the given key.
 *
 * Maps key hash modulo num_locks to a specific recursive mutex in bucket_locks.
 *
 * @param store Pointer to key-value store.
 * @param key Key string used to determine the lock shard via modulo hash.
 */
void kv_acquire_bucket_lock(kv_store_t *store, const char *key);

/**
 * @brief Releases the specific bucket shard lock corresponding to the given key.
 *
 * @param store Pointer to key-value store.
 * @param key Key string used to determine the lock shard via modulo hash.
 */
void kv_release_bucket_lock(kv_store_t *store, const char *key);

/**
 * @brief Sequentially acquires all bucket shard locks in strict index order (0 to num_locks - 1).
 *
 * Used for global operations (resizing, snapshots, AOF compaction, full sweeps) to ensure
 * total point-in-time state consistency and prevent deadlocks.
 *
 * @param store Pointer to key-value store.
 */
void kv_acquire_all_locks(kv_store_t *store);

/**
 * @brief Sequentially releases all bucket shard locks in strict index order.
 *
 * @param store Pointer to key-value store.
 */
void kv_release_all_locks(kv_store_t *store);

/* Modular header inclusions */
#include "list.h"
#include "set.h"
#include "snapshot.h"
#include "eviction.h"
#include "aof.h"

#endif /* KV_H */
