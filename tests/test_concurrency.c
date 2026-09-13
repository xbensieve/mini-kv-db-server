#define _POSIX_C_SOURCE 200809L

#include "kv.h"

#include <pthread.h>
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

#define ASSERT_EQ_STR(actual, expected) do { \
    g_total_assertions++; \
    const char *a_ = (actual); \
    const char *e_ = (expected); \
    if (a_ == NULL || e_ == NULL || strcmp(a_, e_) != 0) { \
        fprintf(stderr, "  FAIL [%s:%d]: Expected \"%s\", got \"%s\" for '%s'.\n", \
                __FILE__, __LINE__, e_ ? e_ : "(null)", a_ ? a_ : "(null)", #actual); \
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

enum {
    NUM_THREADS = 8,
    KEYS_PER_THREAD = 100,
    HAMMER_ITERATIONS = 400
};

typedef struct {
    kv_store_t *store;
    int thread_id;
    int success;
} thread_arg_t;

/* --- Test 1: Concurrent SET and GET on distinct keys --- */
static void *thread_distinct_keys(void *arg) {
    thread_arg_t *t = (thread_arg_t *)arg;
    char key[64];
    char val[64];

    for (int i = 0; i < KEYS_PER_THREAD; ++i) {
        snprintf(key, sizeof(key), "thread_%d_key_%d", t->thread_id, i);
        snprintf(val, sizeof(val), "val_%d_%d", t->thread_id, i);
        if (kv_set(t->store, key, val) != KV_OK) {
            t->success = 0;
            return NULL;
        }
    }

    for (int i = 0; i < KEYS_PER_THREAD; ++i) {
        snprintf(key, sizeof(key), "thread_%d_key_%d", t->thread_id, i);
        snprintf(val, sizeof(val), "val_%d_%d", t->thread_id, i);
        const char *got = kv_get(t->store, key);
        if (got == NULL || strcmp(got, val) != 0) {
            t->success = 0;
            return NULL;
        }
    }

    t->success = 1;
    return NULL;
}

static void test_concurrent_distinct_keys(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 16U), KV_OK);

    pthread_t threads[NUM_THREADS];
    thread_arg_t args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; ++i) {
        args[i].store = &store;
        args[i].thread_id = i;
        args[i].success = 0;
        ASSERT_EQ_INT(pthread_create(&threads[i], NULL, thread_distinct_keys, &args[i]), 0);
    }

    for (int i = 0; i < NUM_THREADS; ++i) {
        ASSERT_EQ_INT(pthread_join(threads[i], NULL), 0);
        ASSERT_EQ_INT(args[i].success, 1);
    }

    ASSERT_EQ_SIZE(store.size, (size_t)(NUM_THREADS * KEYS_PER_THREAD));

    /* Verify all entries still exist after joining */
    for (int i = 0; i < NUM_THREADS; ++i) {
        for (int k = 0; k < KEYS_PER_THREAD; ++k) {
            char key[64];
            char expected_val[64];
            snprintf(key, sizeof(key), "thread_%d_key_%d", i, k);
            snprintf(expected_val, sizeof(expected_val), "val_%d_%d", i, k);
            const char *val = kv_get(&store, key);
            ASSERT_NOT_NULL(val);
            ASSERT_EQ_STR(val, expected_val);
        }
    }

    kv_destroy(&store);
}

/* --- Test 2: Concurrent overwrites on the same keys --- */
static void *thread_same_key_hammer(void *arg) {
    thread_arg_t *t = (thread_arg_t *)arg;
    char val[64];

    for (int i = 0; i < HAMMER_ITERATIONS; ++i) {
        int key_idx = i % 4;
        char key[32];
        snprintf(key, sizeof(key), "shared_key_%d", key_idx);
        snprintf(val, sizeof(val), "thread_%d_iter_%d", t->thread_id, i);

        if (kv_set(t->store, key, val) != KV_OK) {
            t->success = 0;
            return NULL;
        }

        if (kv_exists(t->store, key) != 1) {
            t->success = 0;
            return NULL;
        }
    }

    t->success = 1;
    return NULL;
}

static void test_concurrent_same_key_hammer(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 8U), KV_OK);

    pthread_t threads[NUM_THREADS];
    thread_arg_t args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; ++i) {
        args[i].store = &store;
        args[i].thread_id = i;
        args[i].success = 0;
        ASSERT_EQ_INT(pthread_create(&threads[i], NULL, thread_same_key_hammer, &args[i]), 0);
    }

    for (int i = 0; i < NUM_THREADS; ++i) {
        ASSERT_EQ_INT(pthread_join(threads[i], NULL), 0);
        ASSERT_EQ_INT(args[i].success, 1);
    }

    /* There must be exactly 4 keys */
    ASSERT_EQ_SIZE(store.size, 4U);
    for (int i = 0; i < 4; ++i) {
        char key[32];
        snprintf(key, sizeof(key), "shared_key_%d", i);
        ASSERT_EQ_INT(kv_exists(&store, key), 1);
        ASSERT_NOT_NULL(kv_get(&store, key));
    }

    kv_destroy(&store);
}

/* --- Test 3: Concurrent CRUD churn (SET, GET, EXISTS, DELETE) --- */
static void *thread_crud_churn(void *arg) {
    thread_arg_t *t = (thread_arg_t *)arg;
    char key[32];
    char val[64];

    for (int i = 0; i < 300; ++i) {
        int key_idx = (t->thread_id * 10 + i) % 32;
        snprintf(key, sizeof(key), "churn_%d", key_idx);
        snprintf(val, sizeof(val), "val_t%d_%d", t->thread_id, i);

        int op = (i + t->thread_id) % 4;
        if (op == 0 || op == 1) {
            if (kv_set(t->store, key, val) != KV_OK) {
                t->success = 0;
                return NULL;
            }
        } else if (op == 2) {
            (void)kv_exists(t->store, key);
        } else {
            (void)kv_delete(t->store, key);
        }
    }

    t->success = 1;
    return NULL;
}

static void test_concurrent_crud_churn(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 16U), KV_OK);

    pthread_t threads[NUM_THREADS];
    thread_arg_t args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; ++i) {
        args[i].store = &store;
        args[i].thread_id = i;
        args[i].success = 0;
        ASSERT_EQ_INT(pthread_create(&threads[i], NULL, thread_crud_churn, &args[i]), 0);
    }

    for (int i = 0; i < NUM_THREADS; ++i) {
        ASSERT_EQ_INT(pthread_join(threads[i], NULL), 0);
        ASSERT_EQ_INT(args[i].success, 1);
    }

    /* Verify store invariants */
    ASSERT_TRUE(kv_allocated_bytes(&store) > 0U);
    ASSERT_TRUE(store.size <= 32U);

    kv_destroy(&store);
}

/* --- Test 4: Concurrent resizing under heavy load --- */
static void *thread_resize_loader(void *arg) {
    thread_arg_t *t = (thread_arg_t *)arg;
    char key[64];
    char val[64];

    for (int i = 0; i < 150; ++i) {
        snprintf(key, sizeof(key), "resize_t%d_k%d", t->thread_id, i);
        snprintf(val, sizeof(val), "payload_%d", i);

        if (kv_set(t->store, key, val) != KV_OK) {
            t->success = 0;
            return NULL;
        }

        /* Read back occasionally to race against concurrent rehashing */
        if (i % 5 == 0) {
            const char *res = kv_get(t->store, key);
            if (res == NULL || strcmp(res, val) != 0) {
                t->success = 0;
                return NULL;
            }
        }
    }

    t->success = 1;
    return NULL;
}

static void test_concurrent_resizing_under_load(void) {
    kv_store_t store;
    /* Start with tiny bucket count to force multiple resizes */
    ASSERT_EQ_INT(kv_init(&store, 4U), KV_OK);

    pthread_t threads[NUM_THREADS];
    thread_arg_t args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; ++i) {
        args[i].store = &store;
        args[i].thread_id = i;
        args[i].success = 0;
        ASSERT_EQ_INT(pthread_create(&threads[i], NULL, thread_resize_loader, &args[i]), 0);
    }

    for (int i = 0; i < NUM_THREADS; ++i) {
        ASSERT_EQ_INT(pthread_join(threads[i], NULL), 0);
        ASSERT_EQ_INT(args[i].success, 1);
    }

    size_t total_keys = (size_t)(NUM_THREADS * 150);
    ASSERT_EQ_SIZE(store.size, total_keys);
    ASSERT_TRUE(kv_bucket_count(&store) >= 128U);

    /* Verify every single key is present */
    for (int i = 0; i < NUM_THREADS; ++i) {
        for (int k = 0; k < 150; ++k) {
            char key[64];
            char expected[64];
            snprintf(key, sizeof(key), "resize_t%d_k%d", i, k);
            snprintf(expected, sizeof(expected), "payload_%d", k);
            const char *val = kv_get(&store, key);
            ASSERT_NOT_NULL(val);
            ASSERT_EQ_STR(val, expected);
        }
    }

    kv_destroy(&store);
}

/* --- Test 5: Concurrent memory budget enforcement without deadlock --- */
typedef struct {
    kv_store_t *store;
    int thread_id;
    int budget_hits;
} budget_thread_arg_t;

static void *thread_budget_runner(void *arg) {
    budget_thread_arg_t *t = (budget_thread_arg_t *)arg;
    char key[64];
    char val[256];
    memset(val, 'X', sizeof(val) - 1);
    val[sizeof(val) - 1] = '\0';

    for (int i = 0; i < 100; ++i) {
        snprintf(key, sizeof(key), "b_t%d_k%d", t->thread_id, i);
        int res = kv_set(t->store, key, val);
        if (res == KV_ERR_BUDGET_EXCEEDED) {
            t->budget_hits++;
        }
    }
    return NULL;
}

static void test_concurrent_budget_enforcement(void) {
    kv_store_t store;
    /* Strict budget of 8192 bytes */
    ASSERT_EQ_INT(kv_init_with_budget(&store, 16U, 0U, 8192U), KV_OK);

    pthread_t threads[NUM_THREADS];
    budget_thread_arg_t args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; ++i) {
        args[i].store = &store;
        args[i].thread_id = i;
        args[i].budget_hits = 0;
        ASSERT_EQ_INT(pthread_create(&threads[i], NULL, thread_budget_runner, &args[i]), 0);
    }

    for (int i = 0; i < NUM_THREADS; ++i) {
        ASSERT_EQ_INT(pthread_join(threads[i], NULL), 0);
    }

    /* Threads must have successfully evicted keys and remained under budget */
    ASSERT_TRUE(kv_allocated_bytes(&store) <= 8192U);

    kv_destroy(&store);
}

/* --- Test 6: Concurrent AOF mutations and crash recovery --- */
#define TEST_CONC_AOF "test_concurrency.aof"

static void *thread_aof_mutations(void *arg) {
    thread_arg_t *t = (thread_arg_t *)arg;
    char key[64];
    char val[64];

    for (int i = 0; i < 50; ++i) {
        snprintf(key, sizeof(key), "aof_t%d_k%d", t->thread_id, i);
        snprintf(val, sizeof(val), "val_%d", i);

        if (kv_set(t->store, key, val) != KV_OK) {
            t->success = 0;
            return NULL;
        }

        /* Delete odd keys */
        if (i % 2 == 1) {
            if (kv_delete(t->store, key) != KV_OK) {
                t->success = 0;
                return NULL;
            }
        }
    }

    t->success = 1;
    return NULL;
}

static void test_concurrent_aof_persistence(void) {
    unlink(TEST_CONC_AOF);

    kv_store_t store;
    ASSERT_EQ_INT(kv_init_with_aof(&store, 16U, TEST_CONC_AOF), KV_OK);

    pthread_t threads[NUM_THREADS];
    thread_arg_t args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; ++i) {
        args[i].store = &store;
        args[i].thread_id = i;
        args[i].success = 0;
        ASSERT_EQ_INT(pthread_create(&threads[i], NULL, thread_aof_mutations, &args[i]), 0);
    }

    for (int i = 0; i < NUM_THREADS; ++i) {
        ASSERT_EQ_INT(pthread_join(threads[i], NULL), 0);
        ASSERT_EQ_INT(args[i].success, 1);
    }

    size_t expected_size = (size_t)(NUM_THREADS * 25);
    ASSERT_EQ_SIZE(store.size, expected_size);

    kv_destroy(&store);

    /* Replay AOF log into a fresh store to verify log determinism */
    kv_store_t replay_store;
    ASSERT_EQ_INT(kv_init_with_aof(&replay_store, 16U, TEST_CONC_AOF), KV_OK);
    ASSERT_EQ_SIZE(replay_store.size, expected_size);

    for (int i = 0; i < NUM_THREADS; ++i) {
        for (int k = 0; k < 50; ++k) {
            char key[64];
            snprintf(key, sizeof(key), "aof_t%d_k%d", i, k);
            if (k % 2 == 0) {
                char expected[64];
                snprintf(expected, sizeof(expected), "val_%d", k);
                const char *val = kv_get(&replay_store, key);
                ASSERT_NOT_NULL(val);
                ASSERT_EQ_STR(val, expected);
            } else {
                ASSERT_EQ_INT(kv_exists(&replay_store, key), 0);
            }
        }
    }

    kv_destroy(&replay_store);
    unlink(TEST_CONC_AOF);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("==================================================\n");
    printf("  Mini Key-Value Database Server — Concurrency    \n");
    printf("==================================================\n");

    RUN_TEST(test_concurrent_distinct_keys);
    RUN_TEST(test_concurrent_same_key_hammer);
    RUN_TEST(test_concurrent_crud_churn);
    RUN_TEST(test_concurrent_resizing_under_load);
    RUN_TEST(test_concurrent_budget_enforcement);
    RUN_TEST(test_concurrent_aof_persistence);

    printf("==================================================\n");
    printf("Summary: %d/%d tests passed (%d assertions checked).\n",
           g_tests_passed, g_tests_run, g_total_assertions);

    if (g_tests_passed == g_tests_run) {
        printf("RESULT: ALL CONCURRENCY TESTS PASSED\n");
        return 0;
    } else {
        printf("RESULT: %d TESTS FAILED\n", g_tests_run - g_tests_passed);
        return 1;
    }
}
