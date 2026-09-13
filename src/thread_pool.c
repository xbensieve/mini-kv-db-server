#define _POSIX_C_SOURCE 200809L

#include "thread_pool.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static void *thread_pool_worker(void *arg) {
    thread_pool_t *pool = (thread_pool_t *)arg;

    while (1) {
        pthread_mutex_lock(&pool->queue_lock);

        while (pool->queue_count == 0U && !atomic_load(&pool->shutdown)) {
            pthread_cond_wait(&pool->queue_not_empty, &pool->queue_lock);
        }

        /* If shutdown requested and queue is completely drained, exit thread */
        if (atomic_load(&pool->shutdown) && pool->queue_count == 0U) {
            pthread_mutex_unlock(&pool->queue_lock);
            break;
        }

        task_t task = pool->queue[pool->queue_head];
        pool->queue_head = (pool->queue_head + 1U) % pool->queue_capacity;
        pool->queue_count--;

        pthread_mutex_unlock(&pool->queue_lock);

        /* Execute task outside queue mutex */
        if (task.function != NULL) {
            task.function(task.arg);
        }
    }

    return NULL;
}

thread_pool_t *thread_pool_create(size_t num_threads, size_t max_queue_size) {
    if (num_threads == 0U || max_queue_size == 0U) {
        return NULL;
    }

    thread_pool_t *pool = calloc(1U, sizeof(thread_pool_t));
    if (pool == NULL) {
        return NULL;
    }

    pool->workers = calloc(num_threads, sizeof(pthread_t));
    if (pool->workers == NULL) {
        free(pool);
        return NULL;
    }

    pool->queue = calloc(max_queue_size, sizeof(task_t));
    if (pool->queue == NULL) {
        free(pool->workers);
        free(pool);
        return NULL;
    }

    pool->num_threads = num_threads;
    pool->queue_capacity = max_queue_size;
    pool->queue_head = 0U;
    pool->queue_tail = 0U;
    pool->queue_count = 0U;
    atomic_init(&pool->shutdown, 0);

    if (pthread_mutex_init(&pool->queue_lock, NULL) != 0) {
        free(pool->queue);
        free(pool->workers);
        free(pool);
        return NULL;
    }

    if (pthread_cond_init(&pool->queue_not_empty, NULL) != 0) {
        pthread_mutex_destroy(&pool->queue_lock);
        free(pool->queue);
        free(pool->workers);
        free(pool);
        return NULL;
    }

    for (size_t i = 0; i < num_threads; ++i) {
        if (pthread_create(&pool->workers[i], NULL, thread_pool_worker, pool) != 0) {
            /* Teardown already spawned threads */
            pthread_mutex_lock(&pool->queue_lock);
            atomic_store(&pool->shutdown, 1);
            pthread_cond_broadcast(&pool->queue_not_empty);
            pthread_mutex_unlock(&pool->queue_lock);

            for (size_t j = 0; j < i; ++j) {
                pthread_join(pool->workers[j], NULL);
            }

            pthread_mutex_destroy(&pool->queue_lock);
            pthread_cond_destroy(&pool->queue_not_empty);
            free(pool->queue);
            free(pool->workers);
            free(pool);
            return NULL;
        }
    }

    return pool;
}

int thread_pool_submit(thread_pool_t *pool, void (*function)(void *arg), void *arg) {
    if (pool == NULL || function == NULL) {
        return KV_ERR_INVALID_PARAM;
    }

    pthread_mutex_lock(&pool->queue_lock);

    if (atomic_load(&pool->shutdown)) {
        pthread_mutex_unlock(&pool->queue_lock);
        return KV_ERR_INTERNAL;
    }

    if (pool->queue_count >= pool->queue_capacity) {
        pthread_mutex_unlock(&pool->queue_lock);
        return KV_ERR_AGAIN;
    }

    pool->queue[pool->queue_tail].function = function;
    pool->queue[pool->queue_tail].arg = arg;
    pool->queue_tail = (pool->queue_tail + 1U) % pool->queue_capacity;
    pool->queue_count++;

    pthread_cond_signal(&pool->queue_not_empty);
    pthread_mutex_unlock(&pool->queue_lock);

    return KV_OK;
}

void thread_pool_destroy(thread_pool_t *pool) {
    if (pool == NULL) {
        return;
    }

    /* 1. Acquire queue_lock */
    pthread_mutex_lock(&pool->queue_lock);
    /* 2. Set shutdown flag to true */
    atomic_store(&pool->shutdown, 1);
    /* 3. Broadcast to wake all sleeping workers */
    pthread_cond_broadcast(&pool->queue_not_empty);
    /* 4. Release queue_lock */
    pthread_mutex_unlock(&pool->queue_lock);

    /* 5. Iterate through workers and join each */
    for (size_t i = 0; i < pool->num_threads; ++i) {
        pthread_join(pool->workers[i], NULL);
    }

    pthread_mutex_destroy(&pool->queue_lock);
    pthread_cond_destroy(&pool->queue_not_empty);

    free(pool->queue);
    free(pool->workers);
    free(pool);
}
