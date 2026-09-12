#include "kv.h"

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

/* --- Persistence Test Cases --- */

static const char *TEST_RECOVER_LOG = "tests/temp_test_recover.log";
static const char *TEST_CORRUPT_LOG = "tests/temp_test_corrupt.log";
static const char *TEST_IO_LOG = "tests/temp_test_io.log";

static void test_aof_write_and_recover(void) {
    unlink(TEST_RECOVER_LOG);

    kv_store_t store1;
    ASSERT_EQ_INT(kv_init_with_aof(&store1, 16U, TEST_RECOVER_LOG), KV_OK);
    ASSERT_EQ_STR(kv_aof_path(&store1), TEST_RECOVER_LOG);

    ASSERT_EQ_INT(kv_set(&store1, "user:1", "Alice"), KV_OK);
    ASSERT_EQ_INT(kv_set(&store1, "user:2", "Bob"), KV_OK);
    ASSERT_EQ_INT(kv_set(&store1, "user:3", "Charlie"), KV_OK);
    ASSERT_EQ_INT(kv_set(&store1, "user:1", "Alice_Updated"), KV_OK);
    ASSERT_EQ_INT(kv_delete(&store1, "user:2"), KV_OK);
    ASSERT_EQ_INT(kv_set(&store1, "user:4", "David"), KV_OK);

    ASSERT_EQ_SIZE(store1.size, 3U);
    ASSERT_EQ_STR(kv_get(&store1, "user:1"), "Alice_Updated");
    ASSERT_NULL(kv_get(&store1, "user:2"));
    ASSERT_EQ_STR(kv_get(&store1, "user:3"), "Charlie");
    ASSERT_EQ_STR(kv_get(&store1, "user:4"), "David");

    /* Close first store */
    kv_destroy(&store1);

    /* Reopen from same AOF log in store2 */
    kv_store_t store2;
    ASSERT_EQ_INT(kv_init_with_aof(&store2, 16U, TEST_RECOVER_LOG), KV_OK);
    ASSERT_EQ_SIZE(store2.size, 3U);
    ASSERT_EQ_STR(kv_get(&store2, "user:1"), "Alice_Updated");
    ASSERT_NULL(kv_get(&store2, "user:2"));
    ASSERT_EQ_INT(kv_exists(&store2, "user:2"), 0);
    ASSERT_EQ_STR(kv_get(&store2, "user:3"), "Charlie");
    ASSERT_EQ_STR(kv_get(&store2, "user:4"), "David");

    /* Append further mutations on store2 */
    ASSERT_EQ_INT(kv_delete(&store2, "user:3"), KV_OK);
    ASSERT_EQ_INT(kv_set(&store2, "user:5", "Eve"), KV_OK);
    ASSERT_EQ_SIZE(store2.size, 3U);
    kv_destroy(&store2);

    /* Reopen in store3 and verify extended history */
    kv_store_t store3;
    ASSERT_EQ_INT(kv_init_with_aof(&store3, 16U, TEST_RECOVER_LOG), KV_OK);
    ASSERT_EQ_SIZE(store3.size, 3U);
    ASSERT_NULL(kv_get(&store3, "user:3"));
    ASSERT_EQ_STR(kv_get(&store3, "user:5"), "Eve");
    ASSERT_EQ_STR(kv_get(&store3, "user:1"), "Alice_Updated");

    kv_destroy(&store3);
    unlink(TEST_RECOVER_LOG);
}

static void test_aof_corrupt_log_handling(void) {
    unlink(TEST_CORRUPT_LOG);

    /* Write log with valid records mixed with corrupted and partial lines */
    FILE *f = fopen(TEST_CORRUPT_LOG, "w");
    ASSERT_NOT_NULL(f);
    fprintf(f, "SET\tkey1\tvalid_value_1\n");
    fprintf(f, "\n");                                    /* Empty line */
    fprintf(f, "INVALID_COMMAND key val\n");             /* Unknown command */
    fprintf(f, "SET_NO_SEPARATOR\n");                    /* Corrupted command */
    fprintf(f, "SET\tkey2\tvalue with spaces\n");        /* Valid record with spaces in value */
    fprintf(f, "SET\tkey with space\tval\n");            /* Invalid key containing whitespace */
    fprintf(f, "DELETE\tkey1\n");                        /* Valid delete */
    fputs("GARBAGE DATA @#$%^&*()\n", f);              /* Garbage line */
    fprintf(f, "SET\tkey3");                             /* Partial write without trailing newline */
    fclose(f);

    /* Initializing store from corrupt log must safely recover valid entries without crash or leaks */
    kv_store_t store;
    ASSERT_EQ_INT(kv_init_with_aof(&store, 8U, TEST_CORRUPT_LOG), KV_OK);

    /* key1 was set and then deleted -> should be absent */
    ASSERT_NULL(kv_get(&store, "key1"));
    ASSERT_EQ_INT(kv_exists(&store, "key1"), 0);

    /* key2 was valid -> should be present with exact payload */
    ASSERT_EQ_STR(kv_get(&store, "key2"), "value with spaces");
    ASSERT_EQ_INT(kv_exists(&store, "key2"), 1);

    /* Invalid records were safely skipped */
    ASSERT_NULL(kv_get(&store, "key with space"));
    ASSERT_NULL(kv_get(&store, "key3"));
    ASSERT_EQ_SIZE(store.size, 1U);

    kv_destroy(&store);
    unlink(TEST_CORRUPT_LOG);
}

static void test_aof_io_failure(void) {
    unlink(TEST_IO_LOG);

    kv_store_t store;
    ASSERT_EQ_INT(kv_init_with_aof(&store, 8U, TEST_IO_LOG), KV_OK);

    ASSERT_EQ_INT(kv_set(&store, "committed_1", "data_1"), KV_OK);
    ASSERT_EQ_SIZE(store.size, 1U);
    ASSERT_EQ_STR(kv_get(&store, "committed_1"), "data_1");

    /* Simulate disk write failure */
    kv_set_io_fail(1);

    /* SET must fail with KV_ERR_IO and NOT modify memory */
    ASSERT_EQ_INT(kv_set(&store, "uncommitted", "bad_data"), KV_ERR_IO);
    ASSERT_NULL(kv_get(&store, "uncommitted"));
    ASSERT_EQ_SIZE(store.size, 1U);

    /* DELETE must fail with KV_ERR_IO and NOT modify memory */
    ASSERT_EQ_INT(kv_delete(&store, "committed_1"), KV_ERR_IO);
    ASSERT_EQ_STR(kv_get(&store, "committed_1"), "data_1");
    ASSERT_EQ_SIZE(store.size, 1U);

    /* Restore normal I/O */
    kv_set_io_fail(0);

    /* Subsequent normal mutations succeed */
    ASSERT_EQ_INT(kv_set(&store, "committed_2", "data_2"), KV_OK);
    ASSERT_EQ_SIZE(store.size, 2U);
    kv_destroy(&store);

    /* Reopen log to verify only committed data persists on disk */
    kv_store_t verify_store;
    ASSERT_EQ_INT(kv_init_with_aof(&verify_store, 8U, TEST_IO_LOG), KV_OK);
    ASSERT_EQ_SIZE(verify_store.size, 2U);
    ASSERT_EQ_STR(kv_get(&verify_store, "committed_1"), "data_1");
    ASSERT_EQ_STR(kv_get(&verify_store, "committed_2"), "data_2");
    ASSERT_NULL(kv_get(&verify_store, "uncommitted"));

    kv_destroy(&verify_store);
    unlink(TEST_IO_LOG);
}

static void test_aof_durability_and_sync(void) {
    unlink(TEST_IO_LOG);

    kv_store_t store;
    ASSERT_EQ_INT(kv_init_with_aof(&store, 4U, TEST_IO_LOG), KV_OK);

    /* Durability levels */
    kv_set_durability(&store, KV_DURABILITY_FLUSH);
    ASSERT_EQ_INT(kv_set(&store, "d1", "flush_mode"), KV_OK);

    kv_set_durability(&store, KV_DURABILITY_SYNC);
    ASSERT_EQ_INT(kv_set(&store, "d2", "sync_mode"), KV_OK);

    /* Explicit sync */
    ASSERT_EQ_INT(kv_sync(&store), KV_OK);

    kv_destroy(&store);
    unlink(TEST_IO_LOG);
}

int main(void) {
    printf("==================================================\n");
    printf("  Mini Key-Value Database Server — AOF Tests      \n");
    printf("==================================================\n");

    RUN_TEST(test_aof_write_and_recover);
    RUN_TEST(test_aof_corrupt_log_handling);
    RUN_TEST(test_aof_io_failure);
    RUN_TEST(test_aof_durability_and_sync);

    printf("==================================================\n");
    printf("Summary: %d/%d tests passed (%d assertions checked).\n",
           g_tests_passed, g_tests_run, g_total_assertions);

    if (g_tests_passed == g_tests_run) {
        printf("RESULT: ALL PERSISTENCE TESTS PASSED\n");
        return 0;
    } else {
        printf("RESULT: %d TESTS FAILED\n", g_tests_run - g_tests_passed);
        return 1;
    }
}
