#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "kv.h"

#define TEST_AOF_PATH "test_rewrite.aof"
#define TEMP_AOF_PREFIX "temp-rewrite-aof"

static void cleanup_test_files() {
    unlink(TEST_AOF_PATH);
}

static void test_aof_compaction() {
    printf("Running test_aof_compaction...\n");
    cleanup_test_files();

    kv_store_t store;
    assert(kv_init_full(&store, 16, 0, 0, TEST_AOF_PATH) == KV_OK);

    /* Write 1000 updates to the same key, and some other keys */
    for (int i = 0; i < 1000; i++) {
        char val[32];
        snprintf(val, sizeof(val), "val%d", i);
        assert(kv_set(&store, "counter", val) == KV_OK);
    }
    assert(kv_set(&store, "static_key", "hello") == KV_OK);

    /* Check file size before rewrite */
    long size_before = ftell(store.aof_fp);
    assert(size_before > 10000); /* 1000 lines * ~15 chars */

    /* Trigger rewrite */
    assert(kv_bgrewriteaof(&store) == KV_OK);

    /* Wait for child to exit */
    int status;
    pid_t p = waitpid(store.aof_rewrite_pid, &status, 0);
    assert(p == store.aof_rewrite_pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* Call merge to simulate event loop */
    assert(kv_merge_aof_rewrite(&store, p) == KV_OK);

    /* Check file size after rewrite */
    fseek(store.aof_fp, 0, SEEK_END);
    long size_after = ftell(store.aof_fp);
    assert(size_after < 100); /* 2 SETs */

    kv_destroy(&store);

    /* Verify loading the compacted file */
    kv_store_t store2;
    assert(kv_init_full(&store2, 16, 0, 0, TEST_AOF_PATH) == KV_OK);
    
    const char *v1 = kv_get(&store2, "counter");
    assert(v1 != NULL && strcmp(v1, "val999") == 0);
    const char *v2 = kv_get(&store2, "static_key");
    assert(v2 != NULL && strcmp(v2, "hello") == 0);

    kv_destroy(&store2);
    cleanup_test_files();
    printf("test_aof_compaction passed.\n");
}

static void test_aof_rewrite_concurrent_writes() {
    printf("Running test_aof_rewrite_concurrent_writes...\n");
    cleanup_test_files();

    kv_store_t store;
    assert(kv_init_full(&store, 16, 0, 0, TEST_AOF_PATH) == KV_OK);

    assert(kv_set(&store, "key1", "val1") == KV_OK);

    /* Trigger rewrite */
    assert(kv_bgrewriteaof(&store) == KV_OK);

    /* While rewrite child is active (but sleeping or running), write new keys to parent */
    for (int i = 0; i < 500; i++) {
        char key[32];
        char val[32];
        snprintf(key, sizeof(key), "concurrent%d", i);
        snprintf(val, sizeof(val), "val%d", i);
        assert(kv_set(&store, key, val) == KV_OK);
    }

    /* Wait for child to exit */
    int status;
    pid_t p = waitpid(store.aof_rewrite_pid, &status, 0);
    assert(p == store.aof_rewrite_pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* Call merge */
    assert(kv_merge_aof_rewrite(&store, p) == KV_OK);

    kv_destroy(&store);

    /* Verify loading the file incorporates the concurrent writes! */
    kv_store_t store2;
    assert(kv_init_full(&store2, 16, 0, 0, TEST_AOF_PATH) == KV_OK);
    
    const char *v1 = kv_get(&store2, "key1");
    assert(v1 != NULL && strcmp(v1, "val1") == 0);

    for (int i = 0; i < 500; i++) {
        char key[32];
        char val_exp[32];
        snprintf(key, sizeof(key), "concurrent%d", i);
        snprintf(val_exp, sizeof(val_exp), "val%d", i);
        const char *v = kv_get(&store2, key);
        assert(v != NULL && strcmp(v, val_exp) == 0);
    }

    kv_destroy(&store2);
    cleanup_test_files();
    printf("test_aof_rewrite_concurrent_writes passed.\n");
}

static void test_rewrite_rejection() {
    printf("Running test_rewrite_rejection...\n");
    kv_store_t store;
    assert(kv_init_full(&store, 16, 0, 0, TEST_AOF_PATH) == KV_OK);

    store.bgsave_pid = 12345; /* Fake bgsave pid */
    assert(kv_bgrewriteaof(&store) == KV_ERR_AGAIN);

    store.bgsave_pid = 0;
    store.aof_rewrite_pid = 12345; /* Fake rewrite pid */
    assert(kv_bgrewriteaof(&store) == KV_ERR_AGAIN);

    kv_destroy(&store);
    cleanup_test_files();
    printf("test_rewrite_rejection passed.\n");
}

int main() {
    test_aof_compaction();
    test_aof_rewrite_concurrent_writes();
    test_rewrite_rejection();
    printf("All aof rewrite tests passed.\n");
    return 0;
}
