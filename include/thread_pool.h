#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include "kv.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>

/**
 * @file thread_pool.h
 * @brief Generic POSIX Thread Pool with bounded task queue (Phase 07).
 *
 * Implements a fixed-size worker pool executing tasks submitted from an event loop.
 * The task queue is structured as a bounded ring buffer protected by a queue mutex
 * and a not-empty condition variable.
 */

#ifndef KV_ERR_AGAIN
#define KV_ERR_AGAIN (-10)
#endif

/**
 * @brief Unit of work in the thread pool queue.
 */
typedef struct task {
    void (*function)(void *arg); /**< Function to execute. */
    void *arg;                   /**< Context argument passed to function. */
} task_t;

/**
 * @brief Thread pool runtime state.
 */
typedef struct thread_pool {
    pthread_t *workers;              /**< Array of worker thread handles. */
    size_t num_threads;              /**< Number of worker threads. */

    task_t *queue;                   /**< Ring buffer holding pending tasks. */
    size_t queue_capacity;           /**< Maximum capacity of the task queue. */
    size_t queue_head;               /**< Index of next task to consume. */
    size_t queue_tail;               /**< Index of next slot to insert task. */
    size_t queue_count;              /**< Current number of pending tasks in queue. */

    pthread_mutex_t queue_lock;      /**< Mutex synchronizing access to the queue. */
    pthread_cond_t queue_not_empty;  /**< Condition signaled when task is queued. */

    atomic_int shutdown;             /**< 1 when pool is shutting down, 0 otherwise. */
} thread_pool_t;

/**
 * @brief Creates and starts a new thread pool.
 *
 * @param num_threads Number of worker threads to spawn (must be > 0).
 * @param max_queue_size Maximum capacity of the task queue (must be > 0).
 * @return Pointer to initialized thread_pool_t, or NULL on allocation/thread failure.
 */
thread_pool_t *thread_pool_create(size_t num_threads, size_t max_queue_size);

/**
 * @brief Submits a task to the thread pool in a non-blocking manner.
 *
 * If the task queue is full, this function returns KV_ERR_AGAIN immediately
 * without blocking the caller.
 *
 * @param pool Target thread pool.
 * @param function Function pointer to execute.
 * @param arg Argument passed to function.
 * @return KV_OK on success, KV_ERR_AGAIN if queue is full, or negative error code.
 */
int thread_pool_submit(thread_pool_t *pool, void (*function)(void *arg), void *arg);

/**
 * @brief Gracefully shuts down the thread pool.
 *
 * Sets shutdown flag, broadcasts condition variable to wake workers, drains all
 * remaining queued tasks, joins all worker threads, and releases allocated memory.
 *
 * @param pool Target thread pool (safe no-op if NULL).
 */
void thread_pool_destroy(thread_pool_t *pool);

#endif /* THREAD_POOL_H */
