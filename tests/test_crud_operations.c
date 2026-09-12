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

/* --- Test Cases --- */

static void test_set_and_get_basic(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 16U), KV_OK);
    ASSERT_EQ_SIZE(store.size, 0U);

    ASSERT_EQ_INT(kv_set(&store, "user:1", "Alice"), KV_OK);
    ASSERT_EQ_SIZE(store.size, 1U);
    ASSERT_EQ_INT(kv_exists(&store, "user:1"), 1);
    ASSERT_EQ_STR(kv_get(&store, "user:1"), "Alice");

    ASSERT_EQ_INT(kv_set(&store, "user:2", "Bob"), KV_OK);
    ASSERT_EQ_SIZE(store.size, 2U);
    ASSERT_EQ_INT(kv_exists(&store, "user:2"), 1);
    ASSERT_EQ_STR(kv_get(&store, "user:2"), "Bob");
    ASSERT_EQ_STR(kv_get(&store, "user:1"), "Alice");

    kv_destroy(&store);
}

static void test_set_overwrite_existing(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 16U), KV_OK);

    ASSERT_EQ_INT(kv_set(&store, "host", "127.0.0.1"), KV_OK);
    ASSERT_EQ_SIZE(store.size, 1U);
    ASSERT_EQ_STR(kv_get(&store, "host"), "127.0.0.1");

    /* Overwrite with a new value */
    ASSERT_EQ_INT(kv_set(&store, "host", "192.168.1.1"), KV_OK);
    ASSERT_EQ_SIZE(store.size, 1U);
    ASSERT_EQ_STR(kv_get(&store, "host"), "192.168.1.1");

    /* Overwrite again with a shorter value */
    ASSERT_EQ_INT(kv_set(&store, "host", "::1"), KV_OK);
    ASSERT_EQ_SIZE(store.size, 1U);
    ASSERT_EQ_STR(kv_get(&store, "host"), "::1");

    kv_destroy(&store);
}

static void test_get_and_exists_nonexistent(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 16U), KV_OK);

    ASSERT_NULL(kv_get(&store, "missing"));
    ASSERT_EQ_INT(kv_exists(&store, "missing"), 0);

    ASSERT_EQ_INT(kv_set(&store, "present", "value"), KV_OK);
    ASSERT_NULL(kv_get(&store, "still_missing"));
    ASSERT_EQ_INT(kv_exists(&store, "still_missing"), 0);

    kv_destroy(&store);
}

static void test_delete_existing_and_nonexistent(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 16U), KV_OK);

    ASSERT_EQ_INT(kv_set(&store, "item", "temp"), KV_OK);
    ASSERT_EQ_SIZE(store.size, 1U);
    ASSERT_EQ_INT(kv_exists(&store, "item"), 1);

    /* Delete existing */
    ASSERT_EQ_INT(kv_delete(&store, "item"), KV_OK);
    ASSERT_EQ_SIZE(store.size, 0U);
    ASSERT_NULL(kv_get(&store, "item"));
    ASSERT_EQ_INT(kv_exists(&store, "item"), 0);

    /* Deleting already deleted key returns KV_ERR_NOT_FOUND (1) */
    ASSERT_EQ_INT(kv_delete(&store, "item"), KV_ERR_NOT_FOUND);

    /* Deleting non-existent key returns KV_ERR_NOT_FOUND (1) */
    ASSERT_EQ_INT(kv_delete(&store, "never_existed"), KV_ERR_NOT_FOUND);

    kv_destroy(&store);
}

static void test_edge_empty_keys(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 16U), KV_OK);

    ASSERT_EQ_INT(kv_set(&store, "", "value"), KV_ERR_EMPTY_KEY);
    ASSERT_NULL(kv_get(&store, ""));
    ASSERT_EQ_INT(kv_exists(&store, ""), 0);
    ASSERT_EQ_INT(kv_delete(&store, ""), KV_ERR_INVALID_PARAM);
    ASSERT_EQ_SIZE(store.size, 0U);

    kv_destroy(&store);
}

static void test_edge_whitespace_keys(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 16U), KV_OK);

    ASSERT_EQ_INT(kv_set(&store, "hello world", "val"), KV_ERR_KEY_WHITESPACE);
    ASSERT_EQ_INT(kv_set(&store, " leading", "val"), KV_ERR_KEY_WHITESPACE);
    ASSERT_EQ_INT(kv_set(&store, "trailing ", "val"), KV_ERR_KEY_WHITESPACE);
    ASSERT_EQ_INT(kv_set(&store, "key\twith\ttab", "val"), KV_ERR_KEY_WHITESPACE);
    ASSERT_EQ_INT(kv_set(&store, "key\nwith\nnewline", "val"), KV_ERR_KEY_WHITESPACE);

    ASSERT_NULL(kv_get(&store, "hello world"));
    ASSERT_EQ_INT(kv_exists(&store, "hello world"), 0);
    ASSERT_EQ_INT(kv_delete(&store, "hello world"), KV_ERR_INVALID_PARAM);
    ASSERT_EQ_SIZE(store.size, 0U);

    kv_destroy(&store);
}

static void test_edge_null_pointers(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 16U), KV_OK);

    /* Null key or value */
    ASSERT_EQ_INT(kv_set(&store, NULL, "val"), KV_ERR_INVALID_PARAM);
    ASSERT_EQ_INT(kv_set(&store, "key", NULL), KV_ERR_INVALID_PARAM);
    ASSERT_NULL(kv_get(&store, NULL));
    ASSERT_EQ_INT(kv_exists(&store, NULL), 0);
    ASSERT_EQ_INT(kv_delete(&store, NULL), KV_ERR_INVALID_PARAM);

    /* Null store */
    ASSERT_EQ_INT(kv_set(NULL, "key", "val"), KV_ERR_INVALID_PARAM);
    ASSERT_NULL(kv_get(NULL, "key"));
    ASSERT_EQ_INT(kv_exists(NULL, "key"), 0);
    ASSERT_EQ_INT(kv_delete(NULL, "key"), KV_ERR_INVALID_PARAM);
    ASSERT_EQ_INT(kv_init(NULL, 16U), KV_ERR_INVALID_PARAM);
    ASSERT_EQ_INT(kv_init(&store, 0U), KV_ERR_INVALID_PARAM);

    kv_destroy(NULL);
    kv_destroy(&store);
}

static void test_edge_empty_values(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 16U), KV_OK);

    /* Empty string value is a valid zero-length UTF-8 payload */
    ASSERT_EQ_INT(kv_set(&store, "empty_key", ""), KV_OK);
    ASSERT_EQ_SIZE(store.size, 1U);
    ASSERT_EQ_INT(kv_exists(&store, "empty_key"), 1);

    const char *val = kv_get(&store, "empty_key");
    ASSERT_NOT_NULL(val);
    ASSERT_EQ_STR(val, "");
    ASSERT_EQ_SIZE(strlen(val), 0U);

    /* Overwrite empty with non-empty */
    ASSERT_EQ_INT(kv_set(&store, "empty_key", "now_filled"), KV_OK);
    ASSERT_EQ_STR(kv_get(&store, "empty_key"), "now_filled");

    /* Overwrite back to empty */
    ASSERT_EQ_INT(kv_set(&store, "empty_key", ""), KV_OK);
    ASSERT_EQ_STR(kv_get(&store, "empty_key"), "");

    kv_destroy(&store);
}

static void test_boundary_max_key_length(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 16U), KV_OK);

    char max_key[KV_MAX_KEY_LEN + 1U];
    memset(max_key, 'k', KV_MAX_KEY_LEN);
    max_key[KV_MAX_KEY_LEN] = '\0';

    /* Exact boundary: 256 bytes must succeed */
    ASSERT_EQ_INT(kv_set(&store, max_key, "value_at_256"), KV_OK);
    ASSERT_EQ_SIZE(store.size, 1U);
    ASSERT_EQ_STR(kv_get(&store, max_key), "value_at_256");
    ASSERT_EQ_INT(kv_exists(&store, max_key), 1);
    ASSERT_EQ_INT(kv_delete(&store, max_key), KV_OK);
    ASSERT_EQ_SIZE(store.size, 0U);

    /* Exceeding boundary: 257 bytes must be rejected */
    char over_key[KV_MAX_KEY_LEN + 2U];
    memset(over_key, 'k', KV_MAX_KEY_LEN + 1U);
    over_key[KV_MAX_KEY_LEN + 1U] = '\0';

    ASSERT_EQ_INT(kv_set(&store, over_key, "value_overflow"), KV_ERR_KEY_TOO_LONG);
    ASSERT_NULL(kv_get(&store, over_key));
    ASSERT_EQ_INT(kv_exists(&store, over_key), 0);
    ASSERT_EQ_INT(kv_delete(&store, over_key), KV_ERR_INVALID_PARAM);
    ASSERT_EQ_SIZE(store.size, 0U);

    kv_destroy(&store);
}

static void test_boundary_max_value_length(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 16U), KV_OK);

    char *max_val = malloc(KV_MAX_VALUE_LEN + 1U);
    ASSERT_NOT_NULL(max_val);
    memset(max_val, 'v', KV_MAX_VALUE_LEN);
    max_val[KV_MAX_VALUE_LEN] = '\0';

    /* Exact boundary: 4096 bytes must succeed */
    ASSERT_EQ_INT(kv_set(&store, "big_val", max_val), KV_OK);
    ASSERT_EQ_SIZE(store.size, 1U);
    const char *retrieved = kv_get(&store, "big_val");
    ASSERT_NOT_NULL(retrieved);
    ASSERT_EQ_SIZE(strlen(retrieved), KV_MAX_VALUE_LEN);
    ASSERT_EQ_STR(retrieved, max_val);

    /* Exceeding boundary: 4097 bytes must be rejected */
    char *over_val = malloc(KV_MAX_VALUE_LEN + 2U);
    ASSERT_NOT_NULL(over_val);
    memset(over_val, 'v', KV_MAX_VALUE_LEN + 1U);
    over_val[KV_MAX_VALUE_LEN + 1U] = '\0';

    ASSERT_EQ_INT(kv_set(&store, "too_big", over_val), KV_ERR_VAL_TOO_LONG);
    ASSERT_NULL(kv_get(&store, "too_big"));
    ASSERT_EQ_SIZE(store.size, 1U);

    free(max_val);
    free(over_val);
    kv_destroy(&store);
}

static void test_boundary_capacity_limit(void) {
    kv_store_t store;
    /* Limit capacity to 3 entries */
    ASSERT_EQ_INT(kv_init_with_capacity(&store, 4U, 3U), KV_OK);

    ASSERT_EQ_INT(kv_set(&store, "k1", "v1"), KV_OK);
    ASSERT_EQ_INT(kv_set(&store, "k2", "v2"), KV_OK);
    ASSERT_EQ_INT(kv_set(&store, "k3", "v3"), KV_OK);
    ASSERT_EQ_SIZE(store.size, 3U);

    /* 4th key insertion must fail with capacity full error */
    ASSERT_EQ_INT(kv_set(&store, "k4", "v4"), KV_ERR_CAPACITY_FULL);
    ASSERT_EQ_SIZE(store.size, 3U);
    ASSERT_NULL(kv_get(&store, "k4"));

    /* Overwrite an existing key must succeed even at max capacity */
    ASSERT_EQ_INT(kv_set(&store, "k2", "v2_updated"), KV_OK);
    ASSERT_EQ_SIZE(store.size, 3U);
    ASSERT_EQ_STR(kv_get(&store, "k2"), "v2_updated");

    /* Deleting an entry frees a slot */
    ASSERT_EQ_INT(kv_delete(&store, "k1"), KV_OK);
    ASSERT_EQ_SIZE(store.size, 2U);

    /* Now inserting k4 succeeds */
    ASSERT_EQ_INT(kv_set(&store, "k4", "v4"), KV_OK);
    ASSERT_EQ_SIZE(store.size, 3U);
    ASSERT_EQ_STR(kv_get(&store, "k4"), "v4");

    /* Dynamically expand capacity to 5 */
    kv_set_max_capacity(&store, 5U);
    ASSERT_EQ_INT(kv_set(&store, "k5", "v5"), KV_OK);
    ASSERT_EQ_SIZE(store.size, 4U);

    kv_destroy(&store);
}

static void test_collisions_and_chaining(void) {
    kv_store_t store;
    /* 1 bucket with auto-resize disabled guarantees all entries collide into bucket 0 */
    ASSERT_EQ_INT(kv_init(&store, 1U), KV_OK);
    kv_set_auto_resize(&store, 0);

    ASSERT_EQ_INT(kv_set(&store, "c1", "v1"), KV_OK);
    ASSERT_EQ_INT(kv_set(&store, "c2", "v2"), KV_OK);
    ASSERT_EQ_INT(kv_set(&store, "c3", "v3"), KV_OK);
    ASSERT_EQ_INT(kv_set(&store, "c4", "v4"), KV_OK);
    ASSERT_EQ_INT(kv_set(&store, "c5", "v5"), KV_OK);
    ASSERT_EQ_SIZE(store.size, 5U);
    ASSERT_EQ_SIZE(kv_bucket_count(&store), 1U);

    /* Verify all can be retrieved */
    ASSERT_EQ_STR(kv_get(&store, "c1"), "v1");
    ASSERT_EQ_STR(kv_get(&store, "c2"), "v2");
    ASSERT_EQ_STR(kv_get(&store, "c3"), "v3");
    ASSERT_EQ_STR(kv_get(&store, "c4"), "v4");
    ASSERT_EQ_STR(kv_get(&store, "c5"), "v5");

    /* Delete middle node (chain is prepended: c5 -> c4 -> c3 -> c2 -> c1) */
    ASSERT_EQ_INT(kv_delete(&store, "c3"), KV_OK);
    ASSERT_EQ_SIZE(store.size, 4U);
    ASSERT_NULL(kv_get(&store, "c3"));
    ASSERT_EQ_STR(kv_get(&store, "c5"), "v5");
    ASSERT_EQ_STR(kv_get(&store, "c4"), "v4");
    ASSERT_EQ_STR(kv_get(&store, "c2"), "v2");
    ASSERT_EQ_STR(kv_get(&store, "c1"), "v1");

    /* Delete head node (c5) */
    ASSERT_EQ_INT(kv_delete(&store, "c5"), KV_OK);
    ASSERT_EQ_SIZE(store.size, 3U);
    ASSERT_NULL(kv_get(&store, "c5"));
    ASSERT_EQ_STR(kv_get(&store, "c4"), "v4");
    ASSERT_EQ_STR(kv_get(&store, "c2"), "v2");
    ASSERT_EQ_STR(kv_get(&store, "c1"), "v1");

    /* Delete tail node (c1) */
    ASSERT_EQ_INT(kv_delete(&store, "c1"), KV_OK);
    ASSERT_EQ_SIZE(store.size, 2U);
    ASSERT_NULL(kv_get(&store, "c1"));
    ASSERT_EQ_STR(kv_get(&store, "c4"), "v4");
    ASSERT_EQ_STR(kv_get(&store, "c2"), "v2");

    /* Overwrite node in chain */
    ASSERT_EQ_INT(kv_set(&store, "c2", "v2_updated"), KV_OK);
    ASSERT_EQ_STR(kv_get(&store, "c2"), "v2_updated");

    /* Delete remaining nodes */
    ASSERT_EQ_INT(kv_delete(&store, "c4"), KV_OK);
    ASSERT_EQ_INT(kv_delete(&store, "c2"), KV_OK);
    ASSERT_EQ_SIZE(store.size, 0U);

    kv_destroy(&store);
}

static void test_lifecycle_and_reinit(void) {
    kv_store_t store;
    ASSERT_EQ_INT(kv_init(&store, 8U), KV_OK);
    ASSERT_EQ_INT(kv_set(&store, "key", "val"), KV_OK);
    kv_destroy(&store);

    /* Idempotent destroy */
    kv_destroy(&store);

    /* Reinitialization after destruction */
    ASSERT_EQ_INT(kv_init(&store, 4U), KV_OK);
    ASSERT_EQ_SIZE(store.size, 0U);
    ASSERT_EQ_INT(kv_set(&store, "new_key", "new_val"), KV_OK);
    ASSERT_EQ_STR(kv_get(&store, "new_key"), "new_val");
    kv_destroy(&store);
}

int main(void) {
    printf("==================================================\n");
    printf("  Mini Key-Value Database Server — CRUD Tests     \n");
    printf("==================================================\n");

    RUN_TEST(test_set_and_get_basic);
    RUN_TEST(test_set_overwrite_existing);
    RUN_TEST(test_get_and_exists_nonexistent);
    RUN_TEST(test_delete_existing_and_nonexistent);
    RUN_TEST(test_edge_empty_keys);
    RUN_TEST(test_edge_whitespace_keys);
    RUN_TEST(test_edge_null_pointers);
    RUN_TEST(test_edge_empty_values);
    RUN_TEST(test_boundary_max_key_length);
    RUN_TEST(test_boundary_max_value_length);
    RUN_TEST(test_boundary_capacity_limit);
    RUN_TEST(test_collisions_and_chaining);
    RUN_TEST(test_lifecycle_and_reinit);

    printf("==================================================\n");
    printf("Summary: %d/%d tests passed (%d assertions checked).\n",
           g_tests_passed, g_tests_run, g_total_assertions);

    if (g_tests_passed == g_tests_run) {
        printf("RESULT: ALL CRUD TESTS PASSED\n");
        return 0;
    } else {
        printf("RESULT: %d TESTS FAILED\n", g_tests_run - g_tests_passed);
        return 1;
    }
}
