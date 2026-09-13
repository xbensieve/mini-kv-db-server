#include "kv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>

static void cleanup_snapshot(void) {
    unlink("snapshot.snap");
    unlink("snapshot.snap.tmp");
}

static void test_snapshot_serialization(void) {
    printf("Running test_snapshot_serialization...\n");
    cleanup_snapshot();

    kv_store_t store;
    assert(kv_init(&store, 16) == KV_OK);

    assert(kv_set(&store, "key1", "val1") == KV_OK);
    assert(kv_set(&store, "key2", "val2") == KV_OK);
    assert(kv_expire(&store, "key2", 1000) == 1); /* expires in 1000s */

    assert(kv_save(&store, "snapshot.snap") == KV_OK);

    kv_destroy(&store);

    /* Load snapshot */
    kv_store_t store2;
    assert(kv_init(&store2, 16) == KV_OK);
    assert(kv_load_snapshot(&store2, "snapshot.snap") == KV_OK);

    const char *val1 = kv_get(&store2, "key1");
    assert(val1 != NULL && strcmp(val1, "val1") == 0);
    assert(kv_ttl(&store2, "key1") == -1); /* persistent */

    const char *val2 = kv_get(&store2, "key2");
    assert(val2 != NULL && strcmp(val2, "val2") == 0);
    int64_t ttl2 = kv_ttl(&store2, "key2");
    if (!(ttl2 > 0 && ttl2 <= 1000)) {
        printf("FAILED ASSERTION: ttl2=%ld\n", (long)ttl2);
    }
    assert(ttl2 > 0 && ttl2 <= 1000);

    kv_destroy(&store2);
    cleanup_snapshot();
    printf("test_snapshot_serialization passed.\n");
}

static void test_bgsave_cow_semantics(void) {
    printf("Running test_bgsave_cow_semantics...\n");
    cleanup_snapshot();

    kv_store_t store;
    assert(kv_init(&store, 16) == KV_OK);

    for (int i = 0; i < 1000; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "key%d", i);
        snprintf(val, sizeof(val), "val%d", i);
        assert(kv_set(&store, key, val) == KV_OK);
    }

    assert(kv_bgsave(&store, "snapshot.snap") == KV_OK);
    assert(store.bgsave_pid > 0);

    /* Parent modifies 500 keys while child is writing */
    for (int i = 0; i < 500; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "key%d", i);
        snprintf(val, sizeof(val), "NEWVAL%d", i);
        assert(kv_set(&store, key, val) == KV_OK);
    }

    /* Wait for child to exit */
    int status;
    assert(waitpid(store.bgsave_pid, &status, 0) == store.bgsave_pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    store.bgsave_pid = 0;
    kv_destroy(&store);

    /* Load snapshot and verify it has original values */
    kv_store_t store2;
    assert(kv_init(&store2, 16) == KV_OK);
    assert(kv_load_snapshot(&store2, "snapshot.snap") == KV_OK);

    for (int i = 0; i < 1000; i++) {
        char key[32], expected[32];
        snprintf(key, sizeof(key), "key%d", i);
        snprintf(expected, sizeof(expected), "val%d", i);
        
        const char *val = kv_get(&store2, key);
        assert(val != NULL && strcmp(val, expected) == 0); /* COW worked! */
    }

    kv_destroy(&store2);
    cleanup_snapshot();
    printf("test_bgsave_cow_semantics passed.\n");
}

static void test_bgsave_rejection(void) {
    printf("Running test_bgsave_rejection...\n");
    cleanup_snapshot();

    kv_store_t store;
    assert(kv_init(&store, 16) == KV_OK);
    assert(kv_set(&store, "a", "b") == KV_OK);

    assert(kv_bgsave(&store, "snapshot.snap") == KV_OK);
    assert(kv_bgsave(&store, "snapshot.snap") == KV_ERR_AGAIN); /* rejected */

    int status;
    assert(waitpid(store.bgsave_pid, &status, 0) == store.bgsave_pid);
    store.bgsave_pid = 0;

    kv_destroy(&store);
    cleanup_snapshot();
    printf("test_bgsave_rejection passed.\n");
}

int main(void) {
    test_snapshot_serialization();
    test_bgsave_cow_semantics();
    test_bgsave_rejection();
    printf("All snapshot tests passed.\n");
    return 0;
}
