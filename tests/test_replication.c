#include "kv.h"
#include "protocol.h"
#include "replication.h"
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT_EQ_INT(expr, expected) \
    do { \
        int res = (expr); \
        if (res != (expected)) { \
            fprintf(stderr, "ASSERT_EQ_INT failed at %s:%d: %s (got %d, expected %d)\n", \
                    __FILE__, __LINE__, #expr, res, (expected)); \
            exit(1); \
        } \
    } while(0)

#define ASSERT_STR_EQ(expr, expected) \
    do { \
        const char *res = (expr); \
        if (strcmp(res, (expected)) != 0) { \
            fprintf(stderr, "ASSERT_STR_EQ failed at %s:%d: %s (got '%s', expected '%s')\n", \
                    __FILE__, __LINE__, #expr, res, (expected)); \
            exit(1); \
        } \
    } while(0)

static void test_replica_readonly(void) {
    printf("RUN:  test_replica_readonly\n");
    kv_store_t store;
    kv_init(&store, 32);
    server_t server;
    server_init(&server, &store, 0); // Port 0 doesn't actually start loop here
    
    // Set up replica mode
    replication_init(&g_replication_state, &store, "localhost", 12345);
    // Don't actually start the thread to avoid network stuff
    
    char resp[1024];
    size_t resp_len = 0;
    int should_close = 0;
    int should_detach = 0;
    
    // Try write command
    char cmd_set[] = "SET key val";
    protocol_execute_command(&server, -1, cmd_set, strlen(cmd_set), resp, sizeof(resp), &resp_len, &should_close, &should_detach);
    resp[resp_len] = '\0';
    
    ASSERT_STR_EQ(resp, "-ERR Server is running in read-only replica mode\r\n");
    
    // Try read command
    char cmd_get[] = "GET key";
    protocol_execute_command(&server, -1, cmd_get, strlen(cmd_get), resp, sizeof(resp), &resp_len, &should_close, &should_detach);
    resp[resp_len] = '\0';
    
    ASSERT_STR_EQ(resp, "NOT_FOUND\r\n");
    
    server_destroy(&server);
    kv_destroy(&store);
    
    printf("PASS: test_replica_readonly\n");
}

int main(void) {
    printf("==================================================\n");
    printf("  Mini Key-Value Database Server — Replication    \n");
    printf("==================================================\n");
    test_replica_readonly();
    printf("RESULT: ALL REPLICATION TESTS PASSED\n");
    return 0;
}
