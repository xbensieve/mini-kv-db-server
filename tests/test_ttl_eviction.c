#define _DEFAULT_SOURCE
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

#define ASSERT_EQ_INT64(actual, expected) do { \
    g_total_assertions++; \
    int64_t a_ = (actual); \
    int64_t e_ = (expected); \
    if (a_ != e_) { \
        fprintf(stderr, "  FAIL [%s:%d]: Expected %lld, got %lld for '%s'.\n", __FILE__, __LINE__, (long long)e_, (long long)a_, #actual); \
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
        printf("PASS: %s\n", #test_func); \
        g_tests_passed++; \
    } \
} while (0)

static void test_ttl_passive_expiration(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 16U), KV_OK);

    ASSERT_EQ_INT(kv_set(&store, "key1", "val1"), KV_OK);
    ASSERT_EQ_INT(kv_pexpire(&store, "key1", 50), 1);

    /* Verify existence before expiration */
    ASSERT_EQ_INT(kv_exists(&store, "key1"), 1);
    ASSERT_NOT_NULL(kv_get(&store, "key1"));

    /* Wait for TTL to expire */
    usleep(70000); /* 70ms */

    /* Verify passive expiration on GET and EXISTS */
    ASSERT_EQ_INT(kv_exists(&store, "key1"), 0);
    ASSERT_NULL(kv_get(&store, "key1"));
    
    /* Store size should reflect the silent deletion */
    ASSERT_EQ_SIZE(store.size, 0U);

    kv_destroy(&store);
}

static void test_ttl_persist_command(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 16U), KV_OK);

    ASSERT_EQ_INT(kv_set(&store, "key1", "val1"), KV_OK);
    ASSERT_EQ_INT(kv_pexpire(&store, "key1", 50), 1);
    
    ASSERT_TRUE(kv_pttl(&store, "key1") > 0);

    /* Remove expiration */
    ASSERT_EQ_INT(kv_persist(&store, "key1"), 1);
    ASSERT_EQ_INT64(kv_pttl(&store, "key1"), (int64_t)-1); /* -1 means no TTL */

    /* Wait past original TTL */
    usleep(70000); /* 70ms */

    /* Verify key still exists */
    ASSERT_EQ_INT(kv_exists(&store, "key1"), 1);
    ASSERT_NOT_NULL(kv_get(&store, "key1"));

    kv_destroy(&store);
}

static void test_active_sweep_cycle(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 64U), KV_OK);

    /* Insert 200 keys with short TTLs */
    for (int i = 0; i < 200; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        ASSERT_EQ_INT(kv_set(&store, key, "val"), KV_OK);
        ASSERT_EQ_INT(kv_pexpire(&store, key, 10), 1);
    }
    
    ASSERT_EQ_SIZE(store.size, 200U);

    /* Wait for TTLs to expire */
    usleep(20000); /* 20ms */

    /* Trigger active sweep */
    size_t deleted = kv_expire_sample_sweep(&store, 200, 200); /* Large sample to clear them all */
    
    /* Active sweep should have removed them, size should be 0 */
    ASSERT_TRUE(deleted > 0);
    
    /* Might require a few sweeps depending on PRNG, but with 200 sample size it should clear most/all */
    int sweeps = 0;
    while (store.size > 0 && sweeps < 20) {
        kv_expire_sample_sweep(&store, 200, 200);
        sweeps++;
        usleep(2000); /* advance clock to prevent identical PRNG if state resetting happened */
    }

    ASSERT_EQ_SIZE(store.size, 0U);

    kv_destroy(&store);
}

static void test_lru_eviction_under_budget(void) {
    kv_store_t store;
    
    /* 
     * KV entry size = sizeof(kv_entry_t) (32) + key len + 1 + val len + 1
     * For "keyX", "valX": ~ 32 + 5 + 5 = 42 bytes per entry
     * Let's set a budget of ~1000 bytes, which allows ~20 keys.
     * Bucket array: 16 * 8 = 128 bytes.
     */
    size_t budget = 128 + (42 * 20); 
    ASSERT_EQ_INT(kv_init_with_budget(&store, 16U, 0U, budget), KV_OK);

    /* Fill up to budget */
    int i = 0;
    while (i < 100) {
        char key[32];
        snprintf(key, sizeof(key), "k%d", i);
        int res = kv_set(&store, key, "v");
        if (res == KV_ERR_BUDGET_EXCEEDED) {
            break;
        }
        ASSERT_EQ_INT(res, KV_OK);
        kv_get(&store, "k0"); /* Keep k0 as most recently used! */
        i++;
    }
    
    size_t filled_size = store.size;
    ASSERT_TRUE(filled_size > 0);
    
    /* Access the first key multiple times to update its LRU */
    for (int j = 0; j < 100; j++) {
        ASSERT_NOT_NULL(kv_get(&store, "k0"));
    }

    /* Insert one more key to trigger eviction */
    int evict_res = kv_set(&store, "k_new", "v");
    ASSERT_EQ_INT(evict_res, KV_OK);
    
    /* 
     * Since k0 was most recently accessed, it should survive eviction,
     * while some other key gets evicted.
     */
    ASSERT_EQ_INT(kv_exists(&store, "k0"), 1);
    
    /* Total size should remain roughly the same, meaning eviction worked */
    ASSERT_TRUE(store.size <= filled_size);

    kv_destroy(&store);
}

static void test_aof_ttl_recovery(void) {
    const char *aof_file = "test_ttl.aof";
    remove(aof_file);

    kv_store_t store;
    ASSERT_EQ_INT(kv_init_with_aof(&store, 16U, aof_file), KV_OK);

    ASSERT_EQ_INT(kv_set(&store, "k1", "v1"), KV_OK);
    ASSERT_EQ_INT(kv_pexpire(&store, "k1", 10000), 1); /* Long TTL */

    ASSERT_EQ_INT(kv_set(&store, "k2", "v2"), KV_OK);
    ASSERT_EQ_INT(kv_pexpire(&store, "k2", 50), 1); /* Short TTL */
    
    ASSERT_EQ_INT(kv_set(&store, "k3", "v3"), KV_OK);
    ASSERT_EQ_INT(kv_pexpire(&store, "k3", 50), 1); 
    ASSERT_EQ_INT(kv_persist(&store, "k3"), 1); /* Persisted */

    kv_destroy(&store);

    usleep(70000); /* 70ms, k2 should expire */

    kv_store_t restored;
    ASSERT_EQ_INT(kv_init_with_aof(&restored, 16U, aof_file), KV_OK);

    ASSERT_EQ_INT(kv_exists(&restored, "k1"), 1);
    ASSERT_EQ_INT(kv_exists(&restored, "k2"), 0); /* Should not be loaded/expired on load */
    ASSERT_EQ_INT(kv_exists(&restored, "k3"), 1); /* Persisted, should survive */

    kv_destroy(&restored);
    remove(aof_file);
}

#define CONCURRENT_THREADS 8
#define ITERATIONS_PER_THREAD 1000

typedef struct {
    kv_store_t *store;
    int thread_id;
} thread_arg_t;

static void *worker_thread(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    kv_store_t *store = targ->store;
    
    for (int i = 0; i < ITERATIONS_PER_THREAD; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key_%d_%d", targ->thread_id, i % 100);
        
        kv_set(store, key, "value");
        kv_pexpire(store, key, (i % 50) + 10); /* 10-60ms TTL */
        
        if (i % 3 == 0) {
            kv_get(store, key); /* Trigger LRU updates / passive expiry */
        }
        
        if (i % 5 == 0) {
            kv_persist(store, key);
        }
        
        if (i % 100 == 0) {
            kv_expire_sample_sweep(store, 10, 5); /* Active sweep */
        }
    }
    return NULL;
}

static void test_concurrent_ttl_hammer(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 256U), KV_OK);
    kv_set_auto_resize(&store, 1);

    pthread_t threads[CONCURRENT_THREADS];
    thread_arg_t args[CONCURRENT_THREADS];

    for (int i = 0; i < CONCURRENT_THREADS; ++i) {
        args[i].store = &store;
        args[i].thread_id = i;
        ASSERT_EQ_INT(pthread_create(&threads[i], NULL, worker_thread, &args[i]), 0);
    }

    for (int i = 0; i < CONCURRENT_THREADS; ++i) {
        pthread_join(threads[i], NULL);
    }

    /* Verify structure is intact and lock is healthy (by destroying it) */
    kv_destroy(&store);
}

int main(void) {
    printf("Running TTL & Eviction Tests...\n");
    
    RUN_TEST(test_ttl_passive_expiration);
    RUN_TEST(test_ttl_persist_command);
    RUN_TEST(test_active_sweep_cycle);
    RUN_TEST(test_lru_eviction_under_budget);
    RUN_TEST(test_aof_ttl_recovery);
    RUN_TEST(test_concurrent_ttl_hammer);
    
    printf("\nTest Summary: %d/%d passed (%d assertions)\n", 
           g_tests_passed, g_tests_run, g_total_assertions);

    return (g_tests_run == g_tests_passed) ? EXIT_SUCCESS : EXIT_FAILURE;
}
