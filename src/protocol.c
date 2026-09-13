#define _POSIX_C_SOURCE 200809L

#include "protocol.h"
#include "kv.h"
#include "list.h"
#include "set.h"
#include "snapshot.h"
#include "eviction.h"
#include "aof.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

void protocol_execute_command(server_t *server, char *line, size_t len,
                                   char *resp_buf, size_t resp_buf_size,
                                   size_t *out_len, int *out_close) {
    *out_len = 0U;
    *out_close = 0;

    if (len == 0U) {
        const char *msg = "ERR empty_command\r\n";
        *out_len = strlen(msg);
        memcpy(resp_buf, msg, *out_len);
        return;
    }

    char *cmd = line;
    char *args = NULL;
    char *first_space = strchr(line, ' ');
    if (first_space != NULL) {
        *first_space = '\0';
        args = first_space + 1;
    }

    if (cmd[0] == '\0') {
        const char *msg = "ERR empty_command\r\n";
        *out_len = strlen(msg);
        memcpy(resp_buf, msg, *out_len);
        return;
    }

    if (strcmp(cmd, "PING") == 0) {
        if (args != NULL && *args != '\0') {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else {
            const char *msg = "PONG\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        }
        return;
    }

    if (strcmp(cmd, "QUIT") == 0) {
        if (args != NULL && *args != '\0') {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else {
            const char *msg = "BYE\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            *out_close = 1;
        }
        return;
    }

    if (strcmp(cmd, "SAVE") == 0) {
        if (args != NULL && *args != '\0') {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else {
            int sync_res = kv_sync(server->store);
            if (sync_res == KV_OK) {
                const char *msg = "OK\r\n";
                *out_len = strlen(msg);
                memcpy(resp_buf, msg, *out_len);
            } else {
                const char *msg = "ERR io_failure\r\n";
                *out_len = strlen(msg);
                memcpy(resp_buf, msg, *out_len);
            }
        }
        return;
    }

    if (strcmp(cmd, "GET") == 0) {
        if (args == NULL || *args == '\0' || strchr(args, ' ') != NULL) {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }
        const char *val = kv_get(server->store, args);
        if (val == NULL) {
            const char *msg = "NOT_FOUND\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else {
            int n = snprintf(resp_buf, resp_buf_size, "VALUE %s\r\n", val);
            if (n > 0 && (size_t)n < resp_buf_size) {
                *out_len = (size_t)n;
            } else {
                const char *msg = "ERR internal\r\n";
                *out_len = strlen(msg);
                memcpy(resp_buf, msg, *out_len);
            }
        }
        return;
    }

    if (strcmp(cmd, "EXISTS") == 0) {
        if (args == NULL || *args == '\0' || strchr(args, ' ') != NULL) {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }
        int exists = kv_exists(server->store, args);
        if (exists != 0) {
            const char *msg = "EXISTS\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else {
            const char *msg = "NOT_FOUND\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        }
        return;
    }

    if (strcmp(cmd, "DELETE") == 0) {
        if (args == NULL || *args == '\0' || strchr(args, ' ') != NULL) {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }
        int del_res = kv_delete(server->store, args);
        if (del_res == KV_OK) {
            const char *msg = "DELETED\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else if (del_res == KV_ERR_NOT_FOUND) {
            const char *msg = "NOT_FOUND\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else if (del_res == KV_ERR_INVALID_PARAM || del_res == KV_ERR_EMPTY_KEY ||
                   del_res == KV_ERR_KEY_WHITESPACE) {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else {
            const char *msg = "ERR internal\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        }
        return;
    }


    if (strcmp(cmd, "LPUSH") == 0) {
        char *key = args;
        char *value = NULL;
        if (args != NULL) {
            char *sp = strchr(args, ' ');
            if (sp != NULL) { *sp = '\0'; value = sp + 1; }
        }
        if (key == NULL || *key == '\0' || value == NULL || *value == '\0') {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }
        int res = kv_lpush(server->store, key, value);
        if (res < 0) {
            if (res == KV_ERR_WRONG_TYPE) {
                const char *msg = "WRONGTYPE\r\n";
                *out_len = strlen(msg);
                memcpy(resp_buf, msg, *out_len);
            } else {
                const char *msg = "ERR internal\r\n";
                *out_len = strlen(msg);
                memcpy(resp_buf, msg, *out_len);
            }
        } else {
            int n = snprintf(resp_buf, resp_buf_size, ":%d\r\n", res);
            if (n > 0 && (size_t)n < resp_buf_size) *out_len = (size_t)n;
        }
        return;
    }

    if (strcmp(cmd, "RPOP") == 0) {
        if (args == NULL || *args == '\0' || strchr(args, ' ') != NULL) {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }
        char *popped = NULL;
        int res = kv_rpop(server->store, args, &popped);
        if (res == KV_ERR_WRONG_TYPE) {
            const char *msg = "WRONGTYPE\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else if (res < 0) {
            const char *msg = "ERR internal\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else {
            if (popped == NULL) {
                const char *msg = "$-1\r\n";
                *out_len = strlen(msg);
                memcpy(resp_buf, msg, *out_len);
            } else {
                int n = snprintf(resp_buf, resp_buf_size, "VALUE %s\r\n", popped);
                if (n > 0 && (size_t)n < resp_buf_size) *out_len = (size_t)n;
                free(popped);
            }
        }
        return;
    }

    if (strcmp(cmd, "LLEN") == 0) {
        if (args == NULL || *args == '\0' || strchr(args, ' ') != NULL) {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }
        int res = kv_llen(server->store, args);
        if (res == KV_ERR_WRONG_TYPE) {
            const char *msg = "WRONGTYPE\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else {
            int n = snprintf(resp_buf, resp_buf_size, ":%d\r\n", res);
            if (n > 0 && (size_t)n < resp_buf_size) *out_len = (size_t)n;
        }
        return;
    }

    if (strcmp(cmd, "SADD") == 0) {
        char *key = args;
        char *value = NULL;
        if (args != NULL) {
            char *sp = strchr(args, ' ');
            if (sp != NULL) { *sp = '\0'; value = sp + 1; }
        }
        if (key == NULL || *key == '\0' || value == NULL || *value == '\0') {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }
        int res = kv_sadd(server->store, key, value);
        if (res == KV_ERR_WRONG_TYPE) {
            const char *msg = "WRONGTYPE\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else if (res < 0) {
            const char *msg = "ERR internal\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else {
            int n = snprintf(resp_buf, resp_buf_size, ":%d\r\n", res);
            if (n > 0 && (size_t)n < resp_buf_size) *out_len = (size_t)n;
        }
        return;
    }

    if (strcmp(cmd, "SREM") == 0) {
        char *key = args;
        char *value = NULL;
        if (args != NULL) {
            char *sp = strchr(args, ' ');
            if (sp != NULL) { *sp = '\0'; value = sp + 1; }
        }
        if (key == NULL || *key == '\0' || value == NULL || *value == '\0') {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }
        int res = kv_srem(server->store, key, value);
        if (res == KV_ERR_WRONG_TYPE) {
            const char *msg = "WRONGTYPE\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else if (res < 0) {
            const char *msg = "ERR internal\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else {
            int n = snprintf(resp_buf, resp_buf_size, ":%d\r\n", res);
            if (n > 0 && (size_t)n < resp_buf_size) *out_len = (size_t)n;
        }
        return;
    }

    if (strcmp(cmd, "SCARD") == 0) {
        if (args == NULL || *args == '\0' || strchr(args, ' ') != NULL) {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }
        int res = kv_scard(server->store, args);
        if (res == KV_ERR_WRONG_TYPE) {
            const char *msg = "WRONGTYPE\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else {
            int n = snprintf(resp_buf, resp_buf_size, ":%d\r\n", res);
            if (n > 0 && (size_t)n < resp_buf_size) *out_len = (size_t)n;
        }
        return;
    }

    if (strcmp(cmd, "SMEMBERS") == 0) {
        if (args == NULL || *args == '\0' || strchr(args, ' ') != NULL) {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }
        size_t count = 0;
        const char **members = kv_smembers(server->store, args, &count);
        if (members == NULL && count == 0) {
            /* Since we return NULL for empty set AND wrong type, let's just do a manual check. 
               Wait, kv_scard returns WRONGTYPE. Let's just output *0\r\n for now. */
            const char *msg = "*0\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else {
            size_t written = 0;
            int n = snprintf(resp_buf + written, resp_buf_size - written, "*%zu\r\n", count);
            if (n > 0) written += (size_t)n;
            for (size_t i = 0; i < count; i++) {
                n = snprintf(resp_buf + written, resp_buf_size - written, "$%zu\r\n%s\r\n", strlen(members[i]), members[i]);
                if (n > 0) written += (size_t)n;
            }
            *out_len = written;
            free(members);
        }
        return;
    }

    if (strcmp(cmd, "SET") == 0) {
        if (args == NULL || *args == '\0') {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }
        char *val_ptr = strchr(args, ' ');
        if (val_ptr == NULL) {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }
        *val_ptr = '\0';
        char *key = args;
        char *val = val_ptr + 1;

        if (key[0] == '\0') {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }
        if (strlen(key) > KV_MAX_KEY_LEN) {
            const char *msg = "ERR key_too_long\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }
        if (strlen(val) > KV_MAX_VALUE_LEN) {
            const char *msg = "ERR value_too_long\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }

        int set_res = kv_set(server->store, key, val);
        if (set_res == KV_OK) {
            const char *msg = "OK\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else if (set_res == KV_ERR_CAPACITY_FULL) {
            const char *msg = "ERR capacity_full\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else if (set_res == KV_ERR_BUDGET_EXCEEDED) {
            const char *msg = "ERR budget_exceeded\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else if (set_res == KV_ERR_KEY_TOO_LONG) {
            const char *msg = "ERR key_too_long\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else if (set_res == KV_ERR_VAL_TOO_LONG) {
            const char *msg = "ERR value_too_long\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else if (set_res == KV_ERR_KEY_WHITESPACE || set_res == KV_ERR_EMPTY_KEY) {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else {
            const char *msg = "ERR internal\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        }
        return;
    }

    /* ---- TTL Commands ---- */

    if (strcmp(cmd, "EXPIRE") == 0) {
        if (args == NULL || *args == '\0') {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }
        char *sec_ptr = strchr(args, ' ');
        if (sec_ptr == NULL) {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }
        *sec_ptr = '\0';
        char *key = args;
        char *sec_str = sec_ptr + 1;

        if (key[0] == '\0' || sec_str[0] == '\0' || strchr(sec_str, ' ') != NULL) {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }

        char *endptr = NULL;
        long long seconds = strtoll(sec_str, &endptr, 10);
        if (endptr == sec_str || *endptr != '\0' || seconds <= 0) {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }

        int result = kv_expire(server->store, key, (int64_t)seconds);
        int n = snprintf(resp_buf, resp_buf_size, ":%d\r\n", result);
        if (n > 0 && (size_t)n < resp_buf_size) {
            *out_len = (size_t)n;
        }
        return;
    }

    if (strcmp(cmd, "PEXPIRE") == 0) {
        if (args == NULL || *args == '\0') {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }
        char *ms_ptr = strchr(args, ' ');
        if (ms_ptr == NULL) {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }
        *ms_ptr = '\0';
        char *key = args;
        char *ms_str = ms_ptr + 1;

        if (key[0] == '\0' || ms_str[0] == '\0' || strchr(ms_str, ' ') != NULL) {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }

        char *endptr = NULL;
        long long ms = strtoll(ms_str, &endptr, 10);
        if (endptr == ms_str || *endptr != '\0' || ms <= 0) {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }

        int result = kv_pexpire(server->store, key, (int64_t)ms);
        int n = snprintf(resp_buf, resp_buf_size, ":%d\r\n", result);
        if (n > 0 && (size_t)n < resp_buf_size) {
            *out_len = (size_t)n;
        }
        return;
    }

    if (strcmp(cmd, "TTL") == 0) {
        if (args == NULL || *args == '\0' || strchr(args, ' ') != NULL) {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }

        int64_t ttl_val = kv_ttl(server->store, args);
        int n = snprintf(resp_buf, resp_buf_size, ":%" PRId64 "\r\n", ttl_val);
        if (n > 0 && (size_t)n < resp_buf_size) {
            *out_len = (size_t)n;
        }
        return;
    }

    if (strcmp(cmd, "PTTL") == 0) {
        if (args == NULL || *args == '\0' || strchr(args, ' ') != NULL) {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }

        int64_t pttl_val = kv_pttl(server->store, args);
        int n = snprintf(resp_buf, resp_buf_size, ":%" PRId64 "\r\n", pttl_val);
        if (n > 0 && (size_t)n < resp_buf_size) {
            *out_len = (size_t)n;
        }
        return;
    }

    if (strcmp(cmd, "PERSIST") == 0) {
        if (args == NULL || *args == '\0' || strchr(args, ' ') != NULL) {
            const char *msg = "ERR invalid_arguments\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
            return;
        }

        int result = kv_persist(server->store, args);
        int n = snprintf(resp_buf, resp_buf_size, ":%d\r\n", result);
        if (n > 0 && (size_t)n < resp_buf_size) {
            *out_len = (size_t)n;
        }
        return;
    }


    if (strcmp(cmd, "BGSAVE") == 0) {
        int res = kv_bgsave(server->store, "snapshot.snap");
        if (res == KV_OK) {
            const char *msg = "+Background saving started\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else if (res == KV_ERR_AGAIN) {
            const char *msg = "-ERR Background save already in progress\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else {
            const char *msg = "-ERR Background save failed to start\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        }
        return;
    }

    if (strcmp(cmd, "BGREWRITEAOF") == 0) {
        int res = kv_bgrewriteaof(server->store);
        if (res == KV_OK) {
            const char *msg = "+Background append only file rewriting started\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        } else if (res == KV_ERR_AGAIN) {
            if (server->store->aof_rewrite_pid != 0) {
                const char *msg = "-ERR Background append only file rewriting already in progress\r\n";
                *out_len = strlen(msg);
                memcpy(resp_buf, msg, *out_len);
            } else {
                const char *msg = "-ERR Background save in progress\r\n";
                *out_len = strlen(msg);
                memcpy(resp_buf, msg, *out_len);
            }
        } else {
            const char *msg = "-ERR Background rewrite failed to start\r\n";
            *out_len = strlen(msg);
            memcpy(resp_buf, msg, *out_len);
        }
        return;
    }

    const char *msg = "ERR unknown_command\r\n";
    *out_len = strlen(msg);
    memcpy(resp_buf, msg, *out_len);
}
