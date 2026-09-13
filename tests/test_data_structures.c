#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "kv.h"

#define TEST_DB_PATH "test_ds.aof"
#define TEST_SNAP_PATH "test_ds.snap"

static void cleanup_test_files() {
    unlink(TEST_DB_PATH);
    unlink(TEST_SNAP_PATH);
}

static void test_type_enforcement() {
    printf("Running test_type_enforcement...\n");
    kv_store_t store;
    assert(kv_init(&store, 16) == KV_OK);

    assert(kv_set(&store, "str_key", "val") == KV_OK);
    assert(kv_lpush(&store, "str_key", "lval") == KV_ERR_WRONG_TYPE);
    assert(kv_sadd(&store, "str_key", "sval") == KV_ERR_WRONG_TYPE);

    assert(kv_lpush(&store, "list_key", "lval1") == 1);
    assert(kv_get(&store, "list_key") == NULL);
    assert(kv_sadd(&store, "list_key", "sval") == KV_ERR_WRONG_TYPE);

    assert(kv_sadd(&store, "set_key", "sval1") == 1);
    assert(kv_get(&store, "set_key") == NULL);
    assert(kv_lpush(&store, "set_key", "lval") == KV_ERR_WRONG_TYPE);

    kv_destroy(&store);
    printf("test_type_enforcement passed.\n");
}

static void test_list_mechanics() {
    printf("Running test_list_mechanics...\n");
    kv_store_t store;
    assert(kv_init(&store, 16) == KV_OK);

    for (int i = 0; i < 1000; i++) {
        char val[32];
        snprintf(val, sizeof(val), "val%d", i);
        assert(kv_lpush(&store, "mylist", val) == i + 1);
    }
    assert(kv_llen(&store, "mylist") == 1000);

    for (int i = 0; i < 1000; i++) {
        char *popped = NULL;
        assert(kv_rpop(&store, "mylist", &popped) == KV_OK);
        assert(popped != NULL);
        char exp[32];
        snprintf(exp, sizeof(exp), "val%d", i);
        assert(strcmp(popped, exp) == 0);
        free(popped);
    }
    
    assert(kv_llen(&store, "mylist") == 0);
    /* Redis behavior: empty list deletes the key */
    assert(kv_exists(&store, "mylist") == 0);

    kv_destroy(&store);
    printf("test_list_mechanics passed.\n");
}

static void test_set_mechanics() {
    printf("Running test_set_mechanics...\n");
    kv_store_t store;
    assert(kv_init(&store, 16) == KV_OK);

    assert(kv_sadd(&store, "myset", "val1") == 1);
    assert(kv_sadd(&store, "myset", "val2") == 1);
    assert(kv_sadd(&store, "myset", "val1") == 0); /* duplicate */
    assert(kv_scard(&store, "myset") == 2);

    assert(kv_srem(&store, "myset", "val1") == 1);
    assert(kv_srem(&store, "myset", "val1") == 0);
    assert(kv_scard(&store, "myset") == 1);

    size_t count = 0;
    const char **members = kv_smembers(&store, "myset", &count);
    assert(members != NULL);
    assert(count == 1);
    assert(strcmp(members[0], "val2") == 0);
    free(members);

    assert(kv_srem(&store, "myset", "val2") == 1);
    assert(kv_scard(&store, "myset") == 0);
    assert(kv_exists(&store, "myset") == 0);

    kv_destroy(&store);
    printf("test_set_mechanics passed.\n");
}

static void test_deep_memory_cleanup() {
    printf("Running test_deep_memory_cleanup...\n");
    kv_store_t store;
    assert(kv_init_with_budget(&store, 16, 0, 1024 * 1024) == KV_OK);

    /* Allocate massive list */
    for (int i = 0; i < 5000; i++) {
        assert(kv_lpush(&store, "biglist", "this is a slightly longer string to consume memory") > 0);
    }

    size_t peak = store.peak_allocated_bytes;
    assert(peak > 100000);

    /* Delete key, ensure memory is refunded */
    assert(kv_delete(&store, "biglist") == KV_OK);
    assert(store.allocated_bytes < 1000);

    /* Allocate massive set */
    for (int i = 0; i < 5000; i++) {
        char val[32];
        snprintf(val, sizeof(val), "val%d", i);
        assert(kv_sadd(&store, "bigcup", val) == 1);
    }
    
    assert(store.allocated_bytes > 100000);
    kv_destroy(&store); /* Valgrind will ensure zero leaks */
    printf("test_deep_memory_cleanup passed.\n");
}

static void test_persistence_serialization() {
    printf("Running test_persistence_serialization...\n");
    cleanup_test_files();

    kv_store_t store;
    assert(kv_init_full(&store, 16, 0, 0, TEST_DB_PATH) == KV_OK);

    kv_lpush(&store, "list1", "item1");
    kv_lpush(&store, "list1", "item2"); /* list1: head->item2->item1->tail */
    kv_sadd(&store, "set1", "s_item1");
    
    /* Snapshot */
    assert(kv_save(&store, TEST_SNAP_PATH) == KV_OK);
    kv_destroy(&store);

    /* Test Snapshot Load */
    kv_store_t store2;
    assert(kv_init(&store2, 16) == KV_OK);
    assert(kv_load_snapshot(&store2, TEST_SNAP_PATH) == KV_OK);

    assert(kv_llen(&store2, "list1") == 2);
    char *p = NULL;
    kv_rpop(&store2, "list1", &p);
    assert(strcmp(p, "item1") == 0); free(p);
    kv_rpop(&store2, "list1", &p);
    assert(strcmp(p, "item2") == 0); free(p);
    
    assert(kv_scard(&store2, "set1") == 1);
    kv_destroy(&store2);
    
    /* Test AOF Replay */
    kv_store_t store3;
    assert(kv_init_with_aof(&store3, 16, TEST_DB_PATH) == KV_OK);
    assert(kv_llen(&store3, "list1") == 2);
    assert(kv_scard(&store3, "set1") == 1);
    kv_destroy(&store3);

    cleanup_test_files();
    printf("test_persistence_serialization passed.\n");
}

int main() {
    test_type_enforcement();
    test_list_mechanics();
    test_set_mechanics();
    test_deep_memory_cleanup();
    test_persistence_serialization();
    printf("All data structures tests passed.\n");
    return 0;
}
