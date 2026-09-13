#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "kv.h"
#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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

typedef struct {
    kv_store_t store;
    server_t server;
    pthread_t thread;
    uint16_t port;
} server_fixture_t;

static void *server_thread_func(void *arg) {
    server_t *server = (server_t *)arg;
    (void)server_run(server);
    return NULL;
}

static int fixture_start(server_fixture_t *fix) {
    if (kv_init(&fix->store, 32U) != KV_OK) {
        return -1;
    }
    if (server_init(&fix->server, &fix->store, 0U) != 0) {
        kv_destroy(&fix->store);
        return -1;
    }
    fix->port = server_get_port(&fix->server);
    if (pthread_create(&fix->thread, NULL, server_thread_func, &fix->server) != 0) {
        server_destroy(&fix->server);
        kv_destroy(&fix->store);
        return -1;
    }
    return 0;
}

static void fixture_stop(server_fixture_t *fix) {
    server_stop(&fix->server);
    (void)pthread_join(fix->thread, NULL);
    server_destroy(&fix->server);
    kv_destroy(&fix->store);
}

static int connect_to_server(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) <= 0) {
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int recv_line(int fd, char *buf, size_t max_len) {
    size_t received = 0U;
    while (received + 1U < max_len) {
        char ch = '\0';
        ssize_t n = recv(fd, &ch, 1U, 0);
        if (n <= 0) {
            return (int)n;
        }
        buf[received++] = ch;
        if (ch == '\n') {
            break;
        }
    }
    buf[received] = '\0';
    return (int)received;
}

static void test_tcp_server_lifecycle(void) {
    server_fixture_t fix;
    ASSERT_EQ_INT(fixture_start(&fix), 0);
    ASSERT_TRUE(fix.port > 0U);

    int client = connect_to_server(fix.port);
    ASSERT_TRUE(client >= 0);

    const char *ping_cmd = "PING\r\n";
    ASSERT_EQ_INT((int)send(client, ping_cmd, strlen(ping_cmd), 0), (int)strlen(ping_cmd));

    char resp[128];
    ASSERT_TRUE(recv_line(client, resp, sizeof(resp)) > 0);
    ASSERT_EQ_STR(resp, "PONG\r\n");

    const char *quit_cmd = "QUIT\r\n";
    ASSERT_EQ_INT((int)send(client, quit_cmd, strlen(quit_cmd), 0), (int)strlen(quit_cmd));

    ASSERT_TRUE(recv_line(client, resp, sizeof(resp)) > 0);
    ASSERT_EQ_STR(resp, "BYE\r\n");

    /* Assert server closed connection after BYE */
    char eof_check = '\0';
    ASSERT_EQ_INT((int)recv(client, &eof_check, 1U, 0), 0);

    close(client);
    fixture_stop(&fix);
}

static void test_tcp_server_crud(void) {
    server_fixture_t fix;
    ASSERT_EQ_INT(fixture_start(&fix), 0);

    int client = connect_to_server(fix.port);
    ASSERT_TRUE(client >= 0);

    char resp[256];

    /* SET key value */
    const char *cmd_set = "SET my_key hello_world\r\n";
    ASSERT_EQ_INT((int)send(client, cmd_set, strlen(cmd_set), 0), (int)strlen(cmd_set));
    ASSERT_TRUE(recv_line(client, resp, sizeof(resp)) > 0);
    ASSERT_EQ_STR(resp, "OK\r\n");

    /* GET key */
    const char *cmd_get = "GET my_key\r\n";
    ASSERT_EQ_INT((int)send(client, cmd_get, strlen(cmd_get), 0), (int)strlen(cmd_get));
    ASSERT_TRUE(recv_line(client, resp, sizeof(resp)) > 0);
    ASSERT_EQ_STR(resp, "VALUE hello_world\r\n");

    /* EXISTS existing key */
    const char *cmd_exists = "EXISTS my_key\r\n";
    ASSERT_EQ_INT((int)send(client, cmd_exists, strlen(cmd_exists), 0), (int)strlen(cmd_exists));
    ASSERT_TRUE(recv_line(client, resp, sizeof(resp)) > 0);
    ASSERT_EQ_STR(resp, "EXISTS\r\n");

    /* EXISTS missing key */
    const char *cmd_missing = "EXISTS unknown_key\r\n";
    ASSERT_EQ_INT((int)send(client, cmd_missing, strlen(cmd_missing), 0), (int)strlen(cmd_missing));
    ASSERT_TRUE(recv_line(client, resp, sizeof(resp)) > 0);
    ASSERT_EQ_STR(resp, "NOT_FOUND\r\n");

    /* DELETE key */
    const char *cmd_del = "DELETE my_key\r\n";
    ASSERT_EQ_INT((int)send(client, cmd_del, strlen(cmd_del), 0), (int)strlen(cmd_del));
    ASSERT_TRUE(recv_line(client, resp, sizeof(resp)) > 0);
    ASSERT_EQ_STR(resp, "DELETED\r\n");

    /* DELETE again */
    ASSERT_EQ_INT((int)send(client, cmd_del, strlen(cmd_del), 0), (int)strlen(cmd_del));
    ASSERT_TRUE(recv_line(client, resp, sizeof(resp)) > 0);
    ASSERT_EQ_STR(resp, "NOT_FOUND\r\n");

    /* GET after DELETE */
    ASSERT_EQ_INT((int)send(client, cmd_get, strlen(cmd_get), 0), (int)strlen(cmd_get));
    ASSERT_TRUE(recv_line(client, resp, sizeof(resp)) > 0);
    ASSERT_EQ_STR(resp, "NOT_FOUND\r\n");

    /* SAVE command */
    const char *cmd_save = "SAVE\r\n";
    ASSERT_EQ_INT((int)send(client, cmd_save, strlen(cmd_save), 0), (int)strlen(cmd_save));
    ASSERT_TRUE(recv_line(client, resp, sizeof(resp)) > 0);
    ASSERT_EQ_STR(resp, "OK\r\n");

    close(client);
    fixture_stop(&fix);
}

static void test_tcp_server_fragmentation(void) {
    server_fixture_t fix;
    ASSERT_EQ_INT(fixture_start(&fix), 0);

    int client = connect_to_server(fix.port);
    ASSERT_TRUE(client >= 0);

    /* Send command byte-by-byte with small delays to force TCP fragmentation */
    const char *cmd = "SET fragmented_k fragment_val_123\r\n";
    size_t cmd_len = strlen(cmd);
    for (size_t i = 0U; i < cmd_len; ++i) {
        ASSERT_EQ_INT((int)send(client, &cmd[i], 1U, 0), 1);
        usleep(200);
    }

    char resp[128];
    ASSERT_TRUE(recv_line(client, resp, sizeof(resp)) > 0);
    ASSERT_EQ_STR(resp, "OK\r\n");

    /* Retrieve value to verify integrity */
    const char *get_cmd = "GET fragmented_k\r\n";
    ASSERT_EQ_INT((int)send(client, get_cmd, strlen(get_cmd), 0), (int)strlen(get_cmd));
    ASSERT_TRUE(recv_line(client, resp, sizeof(resp)) > 0);
    ASSERT_EQ_STR(resp, "VALUE fragment_val_123\r\n");

    close(client);
    fixture_stop(&fix);
}

static void test_tcp_server_pipelining(void) {
    server_fixture_t fix;
    ASSERT_EQ_INT(fixture_start(&fix), 0);

    int client = connect_to_server(fix.port);
    ASSERT_TRUE(client >= 0);

    /* Batch multiple commands in a single network transmission */
    const char *batch =
        "SET pipe1 apple\r\n"
        "SET pipe2 banana\r\n"
        "GET pipe1\r\n"
        "GET pipe2\r\n"
        "DELETE pipe1\r\n"
        "EXISTS pipe1\r\n";

    ASSERT_EQ_INT((int)send(client, batch, strlen(batch), 0), (int)strlen(batch));

    char resp[128];

    ASSERT_TRUE(recv_line(client, resp, sizeof(resp)) > 0);
    ASSERT_EQ_STR(resp, "OK\r\n");

    ASSERT_TRUE(recv_line(client, resp, sizeof(resp)) > 0);
    ASSERT_EQ_STR(resp, "OK\r\n");

    ASSERT_TRUE(recv_line(client, resp, sizeof(resp)) > 0);
    ASSERT_EQ_STR(resp, "VALUE apple\r\n");

    ASSERT_TRUE(recv_line(client, resp, sizeof(resp)) > 0);
    ASSERT_EQ_STR(resp, "VALUE banana\r\n");

    ASSERT_TRUE(recv_line(client, resp, sizeof(resp)) > 0);
    ASSERT_EQ_STR(resp, "DELETED\r\n");

    ASSERT_TRUE(recv_line(client, resp, sizeof(resp)) > 0);
    ASSERT_EQ_STR(resp, "NOT_FOUND\r\n");

    close(client);
    fixture_stop(&fix);
}

static void test_tcp_server_concurrent_clients(void) {
    server_fixture_t fix;
    ASSERT_EQ_INT(fixture_start(&fix), 0);

    enum { CLIENT_COUNT = 8 };
    int clients[CLIENT_COUNT];

    for (int i = 0; i < CLIENT_COUNT; ++i) {
        clients[i] = connect_to_server(fix.port);
        ASSERT_TRUE(clients[i] >= 0);
    }

    /* Concurrently set keys across clients */
    for (int i = 0; i < CLIENT_COUNT; ++i) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "SET key_%d val_%d\r\n", i, i * 10);
        ASSERT_EQ_INT((int)send(clients[i], cmd, strlen(cmd), 0), (int)strlen(cmd));
    }

    /* Verify each client receives OK */
    for (int i = 0; i < CLIENT_COUNT; ++i) {
        char resp[128];
        ASSERT_TRUE(recv_line(clients[i], resp, sizeof(resp)) > 0);
        ASSERT_EQ_STR(resp, "OK\r\n");
    }

    /* Concurrently retrieve keys across clients */
    for (int i = 0; i < CLIENT_COUNT; ++i) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "GET key_%d\r\n", i);
        ASSERT_EQ_INT((int)send(clients[i], cmd, strlen(cmd), 0), (int)strlen(cmd));
    }

    /* Verify each client gets its expected value */
    for (int i = 0; i < CLIENT_COUNT; ++i) {
        char expected[128];
        char resp[128];
        snprintf(expected, sizeof(expected), "VALUE val_%d\r\n", i * 10);
        ASSERT_TRUE(recv_line(clients[i], resp, sizeof(resp)) > 0);
        ASSERT_EQ_STR(resp, expected);
        close(clients[i]);
    }

    fixture_stop(&fix);
}

static void test_tcp_server_malformed_and_limits(void) {
    server_fixture_t fix;
    ASSERT_EQ_INT(fixture_start(&fix), 0);

    int client = connect_to_server(fix.port);
    ASSERT_TRUE(client >= 0);

    char resp[128];

    /* Empty line */
    const char *empty_cmd = "\r\n";
    ASSERT_EQ_INT((int)send(client, empty_cmd, strlen(empty_cmd), 0), (int)strlen(empty_cmd));
    ASSERT_TRUE(recv_line(client, resp, sizeof(resp)) > 0);
    ASSERT_EQ_STR(resp, "ERR empty_command\r\n");

    /* Unknown command */
    const char *unknown_cmd = "BADCOMMAND key val\r\n";
    ASSERT_EQ_INT((int)send(client, unknown_cmd, strlen(unknown_cmd), 0), (int)strlen(unknown_cmd));
    ASSERT_TRUE(recv_line(client, resp, sizeof(resp)) > 0);
    ASSERT_EQ_STR(resp, "ERR unknown_command\r\n");

    /* Invalid arguments for GET */
    const char *get_no_args = "GET\r\n";
    ASSERT_EQ_INT((int)send(client, get_no_args, strlen(get_no_args), 0), (int)strlen(get_no_args));
    ASSERT_TRUE(recv_line(client, resp, sizeof(resp)) > 0);
    ASSERT_EQ_STR(resp, "ERR invalid_arguments\r\n");

    const char *get_too_many = "GET k1 k2\r\n";
    ASSERT_EQ_INT((int)send(client, get_too_many, strlen(get_too_many), 0), (int)strlen(get_too_many));
    ASSERT_TRUE(recv_line(client, resp, sizeof(resp)) > 0);
    ASSERT_EQ_STR(resp, "ERR invalid_arguments\r\n");

    /* Oversized command line (> 8192 bytes) triggering disconnection */
    char huge_payload[8200];
    memset(huge_payload, 'A', sizeof(huge_payload));
    ASSERT_EQ_INT((int)send(client, huge_payload, sizeof(huge_payload), 0), (int)sizeof(huge_payload));

    /* Server must terminate connection */
    char dummy = '\0';
    ssize_t n = recv(client, &dummy, 1U, 0);
    ASSERT_TRUE(n <= 0);

    close(client);
    fixture_stop(&fix);
}

static void test_tcp_server_abrupt_disconnect(void) {
    server_fixture_t fix;
    ASSERT_EQ_INT(fixture_start(&fix), 0);

    int client1 = connect_to_server(fix.port);
    ASSERT_TRUE(client1 >= 0);

    /* Send partial command without newline and immediately close */
    const char *partial = "SET key_without_term incomplete_val";
    ASSERT_EQ_INT((int)send(client1, partial, strlen(partial), 0), (int)strlen(partial));
    close(client1);

    /* Verify server remains responsive for new clients */
    int client2 = connect_to_server(fix.port);
    ASSERT_TRUE(client2 >= 0);

    const char *ping_cmd = "PING\r\n";
    ASSERT_EQ_INT((int)send(client2, ping_cmd, strlen(ping_cmd), 0), (int)strlen(ping_cmd));

    char resp[128];
    ASSERT_TRUE(recv_line(client2, resp, sizeof(resp)) > 0);
    ASSERT_EQ_STR(resp, "PONG\r\n");

    close(client2);
    fixture_stop(&fix);
}

int main(void) {
    printf("==================================================\n");
    printf("  Mini Key-Value Database Server — TCP Tests      \n");
    printf("==================================================\n");

    RUN_TEST(test_tcp_server_lifecycle);
    RUN_TEST(test_tcp_server_crud);
    RUN_TEST(test_tcp_server_fragmentation);
    RUN_TEST(test_tcp_server_pipelining);
    RUN_TEST(test_tcp_server_concurrent_clients);
    RUN_TEST(test_tcp_server_malformed_and_limits);
    RUN_TEST(test_tcp_server_abrupt_disconnect);

    printf("==================================================\n");
    printf("Summary: %d/%d tests passed (%d assertions checked).\n",
           g_tests_passed, g_tests_run, g_total_assertions);

    if (g_tests_passed == g_tests_run) {
        printf("RESULT: ALL TCP SERVER TESTS PASSED\n");
        return 0;
    } else {
        printf("RESULT: %d TESTS FAILED\n", g_tests_run - g_tests_passed);
        return 1;
    }
}
