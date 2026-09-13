#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "thread_pool.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_current_test_failed = 0;
static int g_total_assertions = 0;

#define ASSERT_TRUE(expr) do { \
    g_total_assertions++; \
    if (!(expr)) { \
        fprintf(stderr, "  FAIL [%s:%d]: Assertion '%s' failed.\n", __FILE__, __LINE__, #expr); \
        g_current_test_failed = 1; \
        return; \
    } \
} while (0)

#define ASSERT_FALSE(expr) do { \
    g_total_assertions++; \
    if (expr) { \
        fprintf(stderr, "  FAIL [%s:%d]: Assertion '!%s' failed.\n", __FILE__, __LINE__, #expr); \
        g_current_test_failed = 1; \
        return; \
    } \
} while (0)

#define ASSERT_EQ_INT(actual, expected) do { \
    g_total_assertions++; \
    int a_ = (actual); \
    int e_ = (expected); \
    if (a_ != e_) { \
        fprintf(stderr, "  FAIL [%s:%d]: Expected %d, got %d for '%s'.\n", __FILE__, __LINE__, e_, a_, #actual); \
        g_current_test_failed = 1; \
        return; \
    } \
} while (0)

#define ASSERT_EQ_SIZE(actual, expected) do { \
    g_total_assertions++; \
    size_t a_ = (actual); \
    size_t e_ = (expected); \
    if (a_ != e_) { \
        fprintf(stderr, "  FAIL [%s:%d]: Expected %zu, got %zu for '%s'.\n", __FILE__, __LINE__, e_, a_, #actual); \
        g_current_test_failed = 1; \
        return; \
    } \
} while (0)

#define ASSERT_NOT_NULL(ptr) do { \
    g_total_assertions++; \
    if ((ptr) == NULL) { \
        fprintf(stderr, "  FAIL [%s:%d]: Expected non-NULL for '%s'.\n", __FILE__, __LINE__, #ptr); \
        g_current_test_failed = 1; \
        return; \
    } \
} while (0)

#define ASSERT_NULL(ptr) do { \
    g_total_assertions++; \
    if ((ptr) != NULL) { \
        fprintf(stderr, "  FAIL [%s:%d]: Expected NULL for '%s'.\n", __FILE__, __LINE__, #ptr); \
        g_current_test_failed = 1; \
        return; \
    } \
} while (0)

#define RUN_TEST(test_func) do { \
    g_tests_run++; \
    g_current_test_failed = 0; \
    printf("RUN:  %s\n", #test_func); \
    test_func(); \
    if (!g_current_test_failed) { \
        g_tests_passed++; \
        printf("PASS: %s\n", #test_func); \
    } else { \
        printf("FAIL: %s\n", #test_func); \
    } \
} while (0)

/* --- Test 1: Execution of 100 tasks on 4 worker threads --- */
static void task_increment_counter(void *arg) {
    atomic_int *counter = (atomic_int *)arg;
    atomic_fetch_add(counter, 1);
}

static void test_thread_pool_execution(void) {
    thread_pool_t *pool = thread_pool_create(4U, 256U);
    ASSERT_NOT_NULL(pool);

    atomic_int counter = 0;
    enum { TOTAL_TASKS = 100 };

    for (int i = 0; i < TOTAL_TASKS; ++i) {
        ASSERT_EQ_INT(thread_pool_submit(pool, task_increment_counter, &counter), KV_OK);
    }

    thread_pool_destroy(pool);

    ASSERT_EQ_INT(atomic_load(&counter), TOTAL_TASKS);
}

/* --- Test 2: Queue capacity limits and non-blocking KV_ERR_AGAIN --- */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int release;
    atomic_int started;
} blocking_task_sync_t;

static void blocking_worker_task(void *arg) {
    blocking_task_sync_t *sync = (blocking_task_sync_t *)arg;
    atomic_store(&sync->started, 1);

    pthread_mutex_lock(&sync->lock);
    while (!sync->release) {
        pthread_cond_wait(&sync->cond, &sync->lock);
    }
    pthread_mutex_unlock(&sync->lock);
}

static void dummy_task(void *arg) {
    atomic_int *val = (atomic_int *)arg;
    if (val != NULL) {
        atomic_fetch_add(val, 1);
    }
}

static void test_thread_pool_queue_limits(void) {
    /* 1 worker, queue capacity 2 */
    thread_pool_t *pool = thread_pool_create(1U, 2U);
    ASSERT_NOT_NULL(pool);

    blocking_task_sync_t sync;
    memset(&sync, 0, sizeof(sync));
    pthread_mutex_init(&sync.lock, NULL);
    pthread_cond_init(&sync.cond, NULL);
    sync.release = 0;
    atomic_init(&sync.started, 0);

    /* Submit task 1: worker pops this immediately and blocks */
    ASSERT_EQ_INT(thread_pool_submit(pool, blocking_worker_task, &sync), KV_OK);

    /* Wait until worker has started running the blocking task */
    while (!atomic_load(&sync.started)) {
        usleep(1000);
    }

    /* Queue capacity is 2. Worker has popped task 1, so queue has 0 items. */
    atomic_int count2 = 0;
    atomic_int count3 = 0;
    atomic_int count4 = 0;

    /* Fill slot 1 of queue */
    ASSERT_EQ_INT(thread_pool_submit(pool, dummy_task, &count2), KV_OK);
    /* Fill slot 2 of queue (queue is now completely full) */
    ASSERT_EQ_INT(thread_pool_submit(pool, dummy_task, &count3), KV_OK);

    /* Slot 3: Queue is full, submit must return KV_ERR_AGAIN */
    ASSERT_EQ_INT(thread_pool_submit(pool, dummy_task, &count4), KV_ERR_AGAIN);

    /* Release the blocking worker */
    pthread_mutex_lock(&sync.lock);
    sync.release = 1;
    pthread_cond_signal(&sync.cond);
    pthread_mutex_unlock(&sync.lock);

    thread_pool_destroy(pool);
    pthread_mutex_destroy(&sync.lock);
    pthread_cond_destroy(&sync.cond);

    /* Verify queued tasks finished */
    ASSERT_EQ_INT(atomic_load(&count2), 1);
    ASSERT_EQ_INT(atomic_load(&count3), 1);
    ASSERT_EQ_INT(atomic_load(&count4), 0);
}

/* --- Test 3: Graceful shutdown and complete queue draining --- */
static void sleep_and_increment_task(void *arg) {
    atomic_int *counter = (atomic_int *)arg;
    usleep(500); /* 0.5 ms delay */
    atomic_fetch_add(counter, 1);
}

static void test_thread_pool_graceful_shutdown(void) {
    thread_pool_t *pool = thread_pool_create(2U, 128U);
    ASSERT_NOT_NULL(pool);

    atomic_int counter = 0;
    enum { DRAIN_TASKS = 50 };

    for (int i = 0; i < DRAIN_TASKS; ++i) {
        ASSERT_EQ_INT(thread_pool_submit(pool, sleep_and_increment_task, &counter), KV_OK);
    }

    /* Immediately trigger destroy; must drain all 50 tasks before exiting */
    thread_pool_destroy(pool);

    ASSERT_EQ_INT(atomic_load(&counter), DRAIN_TASKS);
}

/* --- Test 4: Defensive validation on invalid parameters --- */
static void test_thread_pool_invalid_params(void) {
    ASSERT_NULL(thread_pool_create(0U, 10U));
    ASSERT_NULL(thread_pool_create(4U, 0U));

    thread_pool_t *pool = thread_pool_create(2U, 10U);
    ASSERT_NOT_NULL(pool);

    ASSERT_EQ_INT(thread_pool_submit(NULL, task_increment_counter, NULL), KV_ERR_INVALID_PARAM);
    ASSERT_EQ_INT(thread_pool_submit(pool, NULL, NULL), KV_ERR_INVALID_PARAM);

    thread_pool_destroy(pool);
    /* Destroying NULL is a safe no-op */
    thread_pool_destroy(NULL);
}

/* --- Test 5: Concurrent producer submissions under load --- */
typedef struct {
    thread_pool_t *pool;
    atomic_int *counter;
    int items;
} producer_arg_t;

static void *producer_thread(void *arg) {
    producer_arg_t *p = (producer_arg_t *)arg;
    for (int i = 0; i < p->items; ++i) {
        while (thread_pool_submit(p->pool, task_increment_counter, p->counter) == KV_ERR_AGAIN) {
            usleep(100);
        }
    }
    return NULL;
}

static void test_thread_pool_concurrent_submit(void) {
    thread_pool_t *pool = thread_pool_create(4U, 64U);
    ASSERT_NOT_NULL(pool);

    enum { PRODUCERS = 4, ITEMS_PER_PRODUCER = 100 };
    pthread_t threads[PRODUCERS];
    producer_arg_t args[PRODUCERS];
    atomic_int counter = 0;

    for (int i = 0; i < PRODUCERS; ++i) {
        args[i].pool = pool;
        args[i].counter = &counter;
        args[i].items = ITEMS_PER_PRODUCER;
        ASSERT_EQ_INT(pthread_create(&threads[i], NULL, producer_thread, &args[i]), 0);
    }

    for (int i = 0; i < PRODUCERS; ++i) {
        ASSERT_EQ_INT(pthread_join(threads[i], NULL), 0);
    }

    thread_pool_destroy(pool);

    ASSERT_EQ_INT(atomic_load(&counter), PRODUCERS * ITEMS_PER_PRODUCER);
}

int main(void) {
    printf("==================================================\n");
    printf("  Mini Key-Value Database Server — Thread Pool    \n");
    printf("==================================================\n");

    RUN_TEST(test_thread_pool_execution);
    RUN_TEST(test_thread_pool_queue_limits);
    RUN_TEST(test_thread_pool_graceful_shutdown);
    RUN_TEST(test_thread_pool_invalid_params);
    RUN_TEST(test_thread_pool_concurrent_submit);

    printf("==================================================\n");
    printf("Summary: %d/%d tests passed (%d assertions checked).\n",
           g_tests_passed, g_tests_run, g_total_assertions);

    if (g_tests_passed == g_tests_run) {
        printf("RESULT: ALL THREAD POOL TESTS PASSED\n");
        return 0;
    } else {
        printf("RESULT: %d TESTS FAILED\n", g_tests_run - g_tests_passed);
        return 1;
    }
}
