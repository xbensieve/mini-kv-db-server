#include "kv.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    kv_store_t store;
    if (kv_init(&store, 16U) != 0) {
        fprintf(stderr, "failed to initialize store\n");
        return 1;
    }

    char line[4096];
    puts("Mini KV Phase 01 starter. Commands: SET key value | GET key | DELETE key | EXISTS key | QUIT");

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
            puts(kv_set(&store, key, value) == 0 ? "OK" : "ERR internal");
        } else if (strcmp(command, "GET") == 0 && fields == 2) {
            const char *result = kv_get(&store, key);
            if (result == NULL) puts("NOT_FOUND");
            else printf("VALUE %s\n", result);
        } else if (strcmp(command, "DELETE") == 0 && fields == 2) {
            int result = kv_delete(&store, key);
            puts(result == 0 ? "DELETED" : (result == 1 ? "NOT_FOUND" : "ERR internal"));
        } else if (strcmp(command, "EXISTS") == 0 && fields == 2) {
            puts(kv_exists(&store, key) ? "EXISTS" : "NOT_FOUND");
        } else if (strcmp(command, "QUIT") == 0) {
            break;
        } else {
            puts("ERR invalid_arguments");
        }
    }

    kv_destroy(&store);
    return 0;
}
