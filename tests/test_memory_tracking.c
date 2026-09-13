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

/* --- Memory Tracking Test Cases --- */

static void test_memory_tracking_metrics(void) {
    kv_store_t store;
    const size_t bucket_count = 16U;
    ASSERT_EQ_INT(kv_init(&store, bucket_count), KV_OK);

    size_t expected_bytes = bucket_count * sizeof(kv_entry_t *);
    ASSERT_EQ_SIZE(kv_allocated_bytes(&store), expected_bytes);
    ASSERT_EQ_SIZE(kv_peak_allocated_bytes(&store), expected_bytes);

    /* Insert entry 1 */
    const char *k1 = "user:1";
    const char *v1 = "Alice";
    size_t e1_bytes = sizeof(kv_entry_t) + (strlen(k1) + 1U) + (strlen(v1) + 1U);
    ASSERT_EQ_INT(kv_set(&store, k1, v1), KV_OK);
    expected_bytes += e1_bytes;
    ASSERT_EQ_SIZE(kv_allocated_bytes(&store), expected_bytes);
    ASSERT_EQ_SIZE(kv_peak_allocated_bytes(&store), expected_bytes);

    /* Insert entry 2 */
    const char *k2 = "user:2";
    const char *v2 = "Alexander";
    size_t e2_bytes = sizeof(kv_entry_t) + (strlen(k2) + 1U) + (strlen(v2) + 1U);
    ASSERT_EQ_INT(kv_set(&store, k2, v2), KV_OK);
    expected_bytes += e2_bytes;
    ASSERT_EQ_SIZE(kv_allocated_bytes(&store), expected_bytes);
    ASSERT_EQ_SIZE(kv_peak_allocated_bytes(&store), expected_bytes);

    /* Update entry 1 with longer value (grow) */
    const char *v1_long = "Alice_In_Wonderland";
    size_t v1_diff = strlen(v1_long) - strlen(v1);
    ASSERT_EQ_INT(kv_set(&store, k1, v1_long), KV_OK);
    expected_bytes += v1_diff;
    ASSERT_EQ_SIZE(kv_allocated_bytes(&store), expected_bytes);
    ASSERT_EQ_SIZE(kv_peak_allocated_bytes(&store), expected_bytes);
    size_t peak_mark = expected_bytes;

    /* Update entry 1 with shorter value (shrink) */
    const char *v1_short = "A";
    size_t v1_shrink = strlen(v1_long) - strlen(v1_short);
    ASSERT_EQ_INT(kv_set(&store, k1, v1_short), KV_OK);
    expected_bytes -= v1_shrink;
    ASSERT_EQ_SIZE(kv_allocated_bytes(&store), expected_bytes);
    /* Peak must remain at previous high-water mark */
    ASSERT_EQ_SIZE(kv_peak_allocated_bytes(&store), peak_mark);

    /* Delete entry 2 */
    ASSERT_EQ_INT(kv_delete(&store, k2), KV_OK);
    expected_bytes -= e2_bytes;
    ASSERT_EQ_SIZE(kv_allocated_bytes(&store), expected_bytes);
    ASSERT_EQ_SIZE(kv_peak_allocated_bytes(&store), peak_mark);

    /* Delete entry 1 */
    size_t e1_final_bytes = sizeof(kv_entry_t) + (strlen(k1) + 1U) + (strlen(v1_short) + 1U);
    ASSERT_EQ_INT(kv_delete(&store, k1), KV_OK);
    expected_bytes -= e1_final_bytes;
    ASSERT_EQ_SIZE(kv_allocated_bytes(&store), bucket_count * sizeof(kv_entry_t *));
    ASSERT_EQ_SIZE(kv_peak_allocated_bytes(&store), peak_mark);

    /* Clean destruction */
    kv_destroy(&store);
    ASSERT_EQ_SIZE(kv_allocated_bytes(&store), 0U);
    ASSERT_EQ_SIZE(kv_peak_allocated_bytes(&store), 0U);
}

static void test_memory_budget_enforcement(void) {
    kv_store_t store;
    const size_t bucket_count = 4U;
    const size_t bucket_bytes = bucket_count * sizeof(kv_entry_t *);

    const char *k1 = "k1";
    const char *v1 = "val1";
    size_t e1_bytes = sizeof(kv_entry_t) + (strlen(k1) + 1U) + (strlen(v1) + 1U);

    /* Budget allows bucket array + entry 1 + 5 spare bytes */
    size_t budget = bucket_bytes + e1_bytes + 5U;
    ASSERT_EQ_INT(kv_init_with_budget(&store, bucket_count, 0U, budget), KV_OK);
    ASSERT_EQ_SIZE(kv_max_memory_budget(&store), budget);

    /* First insertion fits within budget */
    ASSERT_EQ_INT(kv_set(&store, k1, v1), KV_OK);
    ASSERT_EQ_SIZE(store.size, 1U);

    /* Second insertion requires more than remaining 5 bytes -> triggers LRU eviction */
    const char *k2 = "k2";
    const char *v2 = "val2";
    ASSERT_EQ_INT(kv_set(&store, k2, v2), KV_OK);
    ASSERT_EQ_SIZE(store.size, 1U);
    ASSERT_NOT_NULL(kv_get(&store, k2));
    ASSERT_NULL(kv_get(&store, k1));

    /* Overwriting k2 with a large value exceeding budget must fail (and evicts/removes k2) */
    const char *v2_huge = "this_value_is_definitely_exceeding_the_five_spare_bytes";
    ASSERT_EQ_INT(kv_set(&store, k2, v2_huge), KV_ERR_BUDGET_EXCEEDED);
    ASSERT_NULL(kv_get(&store, k2));
    ASSERT_EQ_SIZE(store.size, 0U);

    /* Dynamically increase budget */
    size_t new_budget = budget + 500U;
    kv_set_memory_budget(&store, new_budget);
    ASSERT_EQ_SIZE(kv_max_memory_budget(&store), new_budget);

    /* Now k2 insertion succeeds */
    ASSERT_EQ_INT(kv_set(&store, k2, v2), KV_OK);
    ASSERT_EQ_SIZE(store.size, 1U);
    ASSERT_EQ_STR(kv_get(&store, k2), v2);

    kv_destroy(&store);
}

static void test_teardown_and_reinit_zero_bytes(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 8U), KV_OK);

    char kbuf[32];
    char vbuf[64];
    for (int i = 0; i < 20; ++i) {
        snprintf(kbuf, sizeof(kbuf), "k_%d", i);
        snprintf(vbuf, sizeof(vbuf), "value_payload_%d", i);
        ASSERT_EQ_INT(kv_set(&store, kbuf, vbuf), KV_OK);
    }
    ASSERT_EQ_SIZE(store.size, 20U);
    ASSERT_TRUE(kv_allocated_bytes(&store) > 0U);

    /* Teardown must return allocated_bytes precisely to zero */
    kv_destroy(&store);
    ASSERT_EQ_SIZE(kv_allocated_bytes(&store), 0U);
    ASSERT_EQ_SIZE(kv_peak_allocated_bytes(&store), 0U);
    ASSERT_EQ_SIZE(store.size, 0U);
    ASSERT_NULL(store.buckets);

    /* Reinitialize with different bucket count */
    ASSERT_EQ_INT(kv_init(&store, 32U), KV_OK);
    ASSERT_EQ_SIZE(kv_allocated_bytes(&store), 32U * sizeof(kv_entry_t *));
    ASSERT_EQ_SIZE(store.size, 0U);

    ASSERT_EQ_INT(kv_set(&store, "key_after_reinit", "val_after_reinit"), KV_OK);
    ASSERT_EQ_STR(kv_get(&store, "key_after_reinit"), "val_after_reinit");

    kv_destroy(&store);
    ASSERT_EQ_SIZE(kv_allocated_bytes(&store), 0U);
}

static void test_defensive_deallocation_node_fail(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 8U), KV_OK);
    size_t initial_bytes = kv_allocated_bytes(&store);

    /* Next allocation (calloc for kv_entry_t) will fail */
    kv_set_alloc_fail_countdown(0);
    ASSERT_EQ_INT(kv_set(&store, "k_node_fail", "v_node_fail"), KV_ERR_INTERNAL);
    ASSERT_EQ_SIZE(store.size, 0U);
    ASSERT_EQ_SIZE(kv_allocated_bytes(&store), initial_bytes);
    ASSERT_NULL(kv_get(&store, "k_node_fail"));

    kv_set_alloc_fail_countdown(-1);
    kv_destroy(&store);
}

static void test_defensive_deallocation_key_fail(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 8U), KV_OK);
    size_t initial_bytes = kv_allocated_bytes(&store);

    /* 1st allocation (calloc for entry) succeeds; 2nd allocation (key buffer) fails */
    kv_set_alloc_fail_countdown(1);
    ASSERT_EQ_INT(kv_set(&store, "k_key_fail", "v_key_fail"), KV_ERR_INTERNAL);
    ASSERT_EQ_SIZE(store.size, 0U);
    ASSERT_EQ_SIZE(kv_allocated_bytes(&store), initial_bytes);
    ASSERT_NULL(kv_get(&store, "k_key_fail"));

    kv_set_alloc_fail_countdown(-1);
    kv_destroy(&store);
}

static void test_defensive_deallocation_val_fail(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 8U), KV_OK);
    size_t initial_bytes = kv_allocated_bytes(&store);

    /* 1st (entry) and 2nd (key) succeed; 3rd (value buffer) fails */
    kv_set_alloc_fail_countdown(2);
    ASSERT_EQ_INT(kv_set(&store, "k_val_fail", "v_val_fail"), KV_ERR_INTERNAL);
    ASSERT_EQ_SIZE(store.size, 0U);
    ASSERT_EQ_SIZE(kv_allocated_bytes(&store), initial_bytes);
    ASSERT_NULL(kv_get(&store, "k_val_fail"));

    kv_set_alloc_fail_countdown(-1);
    kv_destroy(&store);
}

static void test_defensive_deallocation_update_fail(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 8U), KV_OK);

    ASSERT_EQ_INT(kv_set(&store, "persist_key", "original_value"), KV_OK);
    size_t current_bytes = kv_allocated_bytes(&store);

    /* Fail value allocation during update */
    kv_set_alloc_fail_countdown(0);
    ASSERT_EQ_INT(kv_set(&store, "persist_key", "failed_new_value"), KV_ERR_INTERNAL);

    /* Invariant: original value preserved, size unchanged, bytes unchanged */
    ASSERT_EQ_STR(kv_get(&store, "persist_key"), "original_value");
    ASSERT_EQ_SIZE(store.size, 1U);
    ASSERT_EQ_SIZE(kv_allocated_bytes(&store), current_bytes);

    kv_set_alloc_fail_countdown(-1);
    kv_destroy(&store);
}

static void test_defensive_init_bucket_fail(void) {
    kv_store_t store = {0};
    /* Bucket array allocation fails */
    kv_set_alloc_fail_countdown(0);
    ASSERT_EQ_INT(kv_init(&store, 16U), KV_ERR_INTERNAL);
    ASSERT_NULL(store.buckets);
    ASSERT_EQ_SIZE(kv_allocated_bytes(&store), 0U);

    kv_set_alloc_fail_countdown(-1);
}

int main(void) {
    printf("==================================================\n");
    printf("  Mini Key-Value Database Server — Memory Tests   \n");
    printf("==================================================\n");

    RUN_TEST(test_memory_tracking_metrics);
    RUN_TEST(test_memory_budget_enforcement);
    RUN_TEST(test_teardown_and_reinit_zero_bytes);
    RUN_TEST(test_defensive_deallocation_node_fail);
    RUN_TEST(test_defensive_deallocation_key_fail);
    RUN_TEST(test_defensive_deallocation_val_fail);
    RUN_TEST(test_defensive_deallocation_update_fail);
    RUN_TEST(test_defensive_init_bucket_fail);

    printf("==================================================\n");
    printf("Summary: %d/%d tests passed (%d assertions checked).\n",
           g_tests_passed, g_tests_run, g_total_assertions);

    if (g_tests_passed == g_tests_run) {
        printf("RESULT: ALL MEMORY TESTS PASSED\n");
        return 0;
    } else {
        printf("RESULT: %d TESTS FAILED\n", g_tests_run - g_tests_passed);
        return 1;
    }
}
