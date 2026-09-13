#define _POSIX_C_SOURCE 200809L

#include "kv.h"
#include "server.h"
#include "replication.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static server_t g_server;

static void handle_signal(int sig) {
    (void)sig;
    server_stop(&g_server);
}

static int run_cli(kv_store_t *store) {
    char line[4096];
    puts("Mini KV starter. Commands: SET key value | GET key | DELETE key | EXISTS key | QUIT");

    while (fgets(line, sizeof(line), stdin) != NULL) {
        line[strcspn(line, "\n")] = '\0';

        char command[16];
        char key[257];
        char value[4097];
        memset(command, 0, sizeof(command));
        memset(key, 0, sizeof(key));
        memset(value, 0, sizeof(value));

        int fields = sscanf(line, "%15s %256s %4096[^\n]", command, key, value);
        if (fields <= 0) {
            puts("ERR empty_command");
            continue;
        }

        if (strcmp(command, "SET") == 0 && fields == 3) {
            puts(kv_set(store, key, value) == 0 ? "OK" : "ERR internal");
        } else if (strcmp(command, "GET") == 0 && fields == 2) {
            const char *result = kv_get(store, key);
            if (result == NULL) {
                puts("NOT_FOUND");
            } else {
                printf("VALUE %s\n", result);
            }
        } else if (strcmp(command, "DELETE") == 0 && fields == 2) {
            int result = kv_delete(store, key);
            puts(result == 0 ? "DELETED" : (result == 1 ? "NOT_FOUND" : "ERR internal"));
        } else if (strcmp(command, "EXISTS") == 0 && fields == 2) {
            puts(kv_exists(store, key) ? "EXISTS" : "NOT_FOUND");
        } else if (strcmp(command, "QUIT") == 0) {
            break;
        } else {
            puts("ERR invalid_arguments");
        }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    uint16_t port = (uint16_t)SERVER_DEFAULT_PORT;
    const char *data_path = NULL;
    int cli_mode = 0;
    const char *replica_host = NULL;
    uint16_t replica_port = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            long p = strtol(argv[++i], NULL, 10);
            if (p > 0 && p <= 65535) {
                port = (uint16_t)p;
            } else {
                fprintf(stderr, "invalid port: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            data_path = argv[++i];
        } else if (strcmp(argv[i], "--replicaof") == 0 && i + 2 < argc) {
            replica_host = argv[++i];
            replica_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--cli") == 0) {
            cli_mode = 1;
        }
    }

    kv_store_t store;
    int init_res = (data_path != NULL) ? kv_init_with_aof(&store, 64U, data_path)
                                       : kv_init(&store, 64U);
    if (init_res != KV_OK) {
        fprintf(stderr, "failed to initialize store (code %d)\n", init_res);
        return 1;
    }

    if (cli_mode) {
        int ret = run_cli(&store);
        kv_destroy(&store);
        return ret;
    }

    replication_init(&g_replication_state, &store, replica_host, replica_port);
    replication_start(&g_replication_state);

    if (server_init(&g_server, &store, port) != 0) {
        fprintf(stderr, "failed to bind server to port %u\n", (unsigned int)port);
        replication_stop(&g_replication_state);
        kv_destroy(&store);
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (replica_host) {
        printf("Mini Key-Value Database Server running on port %u (Replica of %s:%u)\n", (unsigned int)server_get_port(&g_server), replica_host, replica_port);
    } else {
        printf("Mini Key-Value Database Server running on port %u (Master)\n", (unsigned int)server_get_port(&g_server));
    }
    server_run(&g_server);

    printf("Shutting down server gracefully...\n");
    server_destroy(&g_server);
    replication_stop(&g_replication_state);
    kv_destroy(&store);

    return 0;
}
