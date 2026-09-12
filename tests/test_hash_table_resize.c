#include "kv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define ASSERT_NULL(ptr) do { \
    g_total_assertions++; \
    if ((ptr) != NULL) { \
        fprintf(stderr, "  FAIL [%s:%d]: Expected NULL for '%s'.\n", __FILE__, __LINE__, #ptr); \
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

#define RUN_TEST(fn) do { \
    g_tests_run++; \
    g_current_test_failed = 0; \
    printf("RUN:  %s\n", #fn); \
    fn(); \
    if (!g_current_test_failed) { \
        g_tests_passed++; \
        printf("PASS: %s\n", #fn); \
    } else { \
        printf("FAIL: %s\n", #fn); \
    } \
} while (0)

/* --- Hash Table Resize Test Cases --- */

static void test_hash_table_resize_auto(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 4U), KV_OK);
    ASSERT_EQ_SIZE(kv_bucket_count(&store), 4U);

    /* Insert 2 items -> load factor 2/4 = 0.50 (no resize) */
    ASSERT_EQ_INT(kv_set(&store, "key_1", "val_1"), KV_OK);
    ASSERT_EQ_INT(kv_set(&store, "key_2", "val_2"), KV_OK);
    ASSERT_EQ_SIZE(kv_bucket_count(&store), 4U);
    ASSERT_EQ_SIZE(store.size, 2U);

    /* Inserting 3rd item -> (2+1)*4 = 12 <= 12 -> no resize before insert */
    ASSERT_EQ_INT(kv_set(&store, "key_3", "val_3"), KV_OK);

    /* Inserting 4th item -> (3+1)*4 = 16 > 12 -> triggers auto-resize! bucket_count becomes 8 */
    ASSERT_EQ_INT(kv_set(&store, "key_4", "val_4"), KV_OK);
    ASSERT_EQ_SIZE(kv_bucket_count(&store), 8U);
    ASSERT_EQ_SIZE(store.size, 4U);

    /* Insert keys up to 30 items, triggering successive resizes to 16, 32, and 64 */
    char kbuf[32];
    char vbuf[32];
    for (int i = 5; i <= 30; ++i) {
        snprintf(kbuf, sizeof(kbuf), "key_%d", i);
        snprintf(vbuf, sizeof(vbuf), "val_%d", i);
        ASSERT_EQ_INT(kv_set(&store, kbuf, vbuf), KV_OK);
    }
    ASSERT_EQ_SIZE(store.size, 30U);
    ASSERT_TRUE(kv_bucket_count(&store) >= 32U);

    /* Verify all 30 keys are accessible with exact values intact */
    for (int i = 1; i <= 30; ++i) {
        snprintf(kbuf, sizeof(kbuf), "key_%d", i);
        snprintf(vbuf, sizeof(vbuf), "val_%d", i);
        ASSERT_EQ_STR(kv_get(&store, kbuf), vbuf);
        ASSERT_EQ_INT(kv_exists(&store, kbuf), 1);
    }

    /* Delete 10 keys and verify remaining 20 keys are intact */
    for (int i = 1; i <= 10; ++i) {
        snprintf(kbuf, sizeof(kbuf), "key_%d", i);
        ASSERT_EQ_INT(kv_delete(&store, kbuf), KV_OK);
        ASSERT_NULL(kv_get(&store, kbuf));
    }
    ASSERT_EQ_SIZE(store.size, 20U);

    for (int i = 11; i <= 30; ++i) {
        snprintf(kbuf, sizeof(kbuf), "key_%d", i);
        snprintf(vbuf, sizeof(vbuf), "val_%d", i);
        ASSERT_EQ_STR(kv_get(&store, kbuf), vbuf);
    }

    kv_destroy(&store);
}

static void test_hash_table_resize_manual(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 4U), KV_OK);
    /* Disable auto-resize to test explicit manual resize */
    kv_set_auto_resize(&store, 0);

    ASSERT_EQ_INT(kv_set(&store, "alpha", "1"), KV_OK);
    ASSERT_EQ_INT(kv_set(&store, "beta", "2"), KV_OK);
    ASSERT_EQ_INT(kv_set(&store, "gamma", "3"), KV_OK);
    ASSERT_EQ_INT(kv_set(&store, "delta", "4"), KV_OK);
    ASSERT_EQ_INT(kv_set(&store, "epsilon", "5"), KV_OK);
    ASSERT_EQ_SIZE(kv_bucket_count(&store), 4U);
    ASSERT_EQ_SIZE(store.size, 5U);

    /* Manually upscale to 32 buckets */
    ASSERT_EQ_INT(kv_resize(&store, 32U), KV_OK);
    ASSERT_EQ_SIZE(kv_bucket_count(&store), 32U);
    ASSERT_EQ_SIZE(store.size, 5U);
    ASSERT_EQ_STR(kv_get(&store, "alpha"), "1");
    ASSERT_EQ_STR(kv_get(&store, "beta"), "2");
    ASSERT_EQ_STR(kv_get(&store, "gamma"), "3");
    ASSERT_EQ_STR(kv_get(&store, "delta"), "4");
    ASSERT_EQ_STR(kv_get(&store, "epsilon"), "5");

    /* Resizing to identical bucket count is a no-op returning KV_OK */
    ASSERT_EQ_INT(kv_resize(&store, 32U), KV_OK);
    ASSERT_EQ_SIZE(kv_bucket_count(&store), 32U);

    /* Invalid parameters to kv_resize */
    ASSERT_EQ_INT(kv_resize(&store, 0U), KV_ERR_INVALID_PARAM);
    ASSERT_EQ_INT(kv_resize(NULL, 16U), KV_ERR_INVALID_PARAM);

    /* Manually downscale to 8 buckets */
    ASSERT_EQ_INT(kv_resize(&store, 8U), KV_OK);
    ASSERT_EQ_SIZE(kv_bucket_count(&store), 8U);
    ASSERT_EQ_SIZE(store.size, 5U);
    ASSERT_EQ_STR(kv_get(&store, "alpha"), "1");
    ASSERT_EQ_STR(kv_get(&store, "beta"), "2");
    ASSERT_EQ_STR(kv_get(&store, "gamma"), "3");
    ASSERT_EQ_STR(kv_get(&store, "delta"), "4");
    ASSERT_EQ_STR(kv_get(&store, "epsilon"), "5");

    kv_destroy(&store);
}

static void test_resize_memory_tracking(void) {
    kv_store_t store;
    const size_t initial_buckets = 4U;
    ASSERT_EQ_INT(kv_init(&store, initial_buckets), KV_OK);
    kv_set_auto_resize(&store, 0);

    ASSERT_EQ_INT(kv_set(&store, "m1", "val_1"), KV_OK);
    ASSERT_EQ_INT(kv_set(&store, "m2", "val_2"), KV_OK);

    size_t bytes_before_resize = kv_allocated_bytes(&store);

    /* Resize from 4 to 16 buckets */
    const size_t new_buckets = 16U;
    ASSERT_EQ_INT(kv_resize(&store, new_buckets), KV_OK);

    /* Net increase in allocated_bytes must be exactly (16 - 4) * sizeof(kv_entry_t *) */
    size_t expected_net_delta = (new_buckets - initial_buckets) * sizeof(kv_entry_t *);
    ASSERT_EQ_SIZE(kv_allocated_bytes(&store), bytes_before_resize + expected_net_delta);

    /* Peak allocated bytes must reflect the temporary spike where both arrays coexisted:
     * spike_peak >= bytes_before_resize + (16 * sizeof(kv_entry_t *)) */
    size_t spike_amount = new_buckets * sizeof(kv_entry_t *);
    ASSERT_TRUE(kv_peak_allocated_bytes(&store) >= bytes_before_resize + spike_amount);

    /* Teardown returns precisely to 0 */
    kv_destroy(&store);
    ASSERT_EQ_SIZE(kv_allocated_bytes(&store), 0U);
    ASSERT_EQ_SIZE(kv_peak_allocated_bytes(&store), 0U);
}

static void test_resize_oom_rollback(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 4U), KV_OK);
    kv_set_auto_resize(&store, 0);

    ASSERT_EQ_INT(kv_set(&store, "k_a", "val_a"), KV_OK);
    ASSERT_EQ_INT(kv_set(&store, "k_b", "val_b"), KV_OK);

    size_t initial_bytes = kv_allocated_bytes(&store);

    /* Simulate OOM failure precisely on new bucket array allocation */
    kv_set_alloc_fail_countdown(0);
    ASSERT_EQ_INT(kv_resize(&store, 8U), KV_ERR_INTERNAL);

    /* Invariant: Table remains fully operational at 4 buckets with zero state corruption */
    ASSERT_EQ_SIZE(kv_bucket_count(&store), 4U);
    ASSERT_EQ_SIZE(store.size, 2U);
    ASSERT_EQ_SIZE(kv_allocated_bytes(&store), initial_bytes);
    ASSERT_EQ_STR(kv_get(&store, "k_a"), "val_a");
    ASSERT_EQ_STR(kv_get(&store, "k_b"), "val_b");

    /* Test defensive rollback during automatic resize in kv_set */
    ASSERT_EQ_INT(kv_set(&store, "k_c", "val_c"), KV_OK);
    ASSERT_EQ_SIZE(kv_bucket_count(&store), 4U);
    ASSERT_EQ_SIZE(store.size, 3U);

    kv_set_auto_resize(&store, 1);
    /* Load factor threshold (> 0.75) is reached when inserting k_d (4th item into 4 buckets).
     * Countdown = 0 causes new bucket array allocation to fail during auto-resize. */
    kv_set_alloc_fail_countdown(0);
    /* kv_set should defensively handle the resize failure and proceed with insert */
    ASSERT_EQ_INT(kv_set(&store, "k_d", "val_d"), KV_OK);

    /* Bucket count remained 4 because resize failed, but k_d was still successfully added */
    ASSERT_EQ_SIZE(kv_bucket_count(&store), 4U);
    ASSERT_EQ_SIZE(store.size, 4U);
    ASSERT_EQ_STR(kv_get(&store, "k_a"), "val_a");
    ASSERT_EQ_STR(kv_get(&store, "k_b"), "val_b");
    ASSERT_EQ_STR(kv_get(&store, "k_c"), "val_c");
    ASSERT_EQ_STR(kv_get(&store, "k_d"), "val_d");

    kv_set_alloc_fail_countdown(-1);
    kv_destroy(&store);
}

static void test_resize_budget_exceeded(void) {
    kv_store_t store;
    const size_t bucket_count = 4U;
    const size_t bucket_bytes = bucket_count * sizeof(kv_entry_t *);

    const char *k1 = "k1";
    const char *v1 = "v1";
    size_t e1_bytes = sizeof(kv_entry_t) + (strlen(k1) + 1U) + (strlen(v1) + 1U);

    /* Budget allows 4 buckets + entry 1 + 20 bytes headroom.
     * An 8-bucket array requires 8 * sizeof(kv_entry_t *) = 64 bytes on 64-bit, which exceeds headroom. */
    size_t budget = bucket_bytes + e1_bytes + 20U;
    ASSERT_EQ_INT(kv_init_with_budget(&store, bucket_count, 0U, budget), KV_OK);
    kv_set_auto_resize(&store, 0);

    ASSERT_EQ_INT(kv_set(&store, k1, v1), KV_OK);
    size_t current_bytes = kv_allocated_bytes(&store);

    /* Manual resize to 8 buckets exceeds memory budget */
    ASSERT_EQ_INT(kv_resize(&store, 8U), KV_ERR_BUDGET_EXCEEDED);

    /* Invariant: Table remains at 4 buckets, entry intact, no memory leaked */
    ASSERT_EQ_SIZE(kv_bucket_count(&store), 4U);
    ASSERT_EQ_SIZE(store.size, 1U);
    ASSERT_EQ_SIZE(kv_allocated_bytes(&store), current_bytes);
    ASSERT_EQ_STR(kv_get(&store, k1), v1);

    kv_destroy(&store);
}

int main(void) {
    printf("==================================================\n");
    printf("  Mini Key-Value Database Server — Resize Tests   \n");
    printf("==================================================\n");

    RUN_TEST(test_hash_table_resize_auto);
    RUN_TEST(test_hash_table_resize_manual);
    RUN_TEST(test_resize_memory_tracking);
    RUN_TEST(test_resize_oom_rollback);
    RUN_TEST(test_resize_budget_exceeded);

    printf("==================================================\n");
    printf("Summary: %d/%d tests passed (%d assertions checked).\n",
           g_tests_passed, g_tests_run, g_total_assertions);

    if (g_tests_passed == g_tests_run) {
        printf("RESULT: ALL RESIZE TESTS PASSED\n");
        return 0;
    } else {
        printf("RESULT: %d TESTS FAILED\n", g_tests_run - g_tests_passed);
        return 1;
    }
}
