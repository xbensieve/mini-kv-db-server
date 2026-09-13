#define _POSIX_C_SOURCE 200809L

#include "replication.h"
#include "kv_internal.h"
#include "snapshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <errno.h>

replication_state_t g_replication_state = {0};

void replication_init(replication_state_t *state, kv_store_t *store, const char *master_host, uint16_t master_port) {
    memset(state, 0, sizeof(*state));
    state->store = store;
    if (master_host && master_port > 0) {
        state->mode = REPLICA_MODE_REPLICA;
        state->master_host = strdup(master_host);
        state->master_port = master_port;
    } else {
        state->mode = REPLICA_MODE_MASTER;
    }
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&state->replicas_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

void replication_add_replica(replication_state_t *state, int socket_fd) {
    if (state->mode != REPLICA_MODE_MASTER) return;
    
    replica_node_t *node = malloc(sizeof(replica_node_t));
    if (!node) return;
    
    node->socket_fd = socket_fd;
    
    pthread_mutex_lock(&state->replicas_lock);
    node->next = state->replicas_head;
    state->replicas_head = node;
    pthread_mutex_unlock(&state->replicas_lock);
}

void replication_broadcast(replication_state_t *state, const char *data, size_t len) {
    if (state->mode != REPLICA_MODE_MASTER) return;
    
    pthread_mutex_lock(&state->replicas_lock);
    replica_node_t *curr = state->replicas_head;
    replica_node_t *prev = NULL;
    
    while (curr) {
        ssize_t sent = send(curr->socket_fd, data, len, 0);
        if (sent < 0) {
            // Remove replica on error
            replica_node_t *to_free = curr;
            if (prev) {
                prev->next = curr->next;
            } else {
                state->replicas_head = curr->next;
            }
            curr = curr->next;
            close(to_free->socket_fd);
            free(to_free);
        } else {
            prev = curr;
            curr = curr->next;
        }
    }
    pthread_mutex_unlock(&state->replicas_lock);
}

int replication_handle_sync(replication_state_t *state, int socket_fd) {
    if (state->mode != REPLICA_MODE_MASTER) {
        const char *err = "-ERR Server is not a master\r\n";
        send(socket_fd, err, strlen(err), 0);
        return -1;
    }
    
    // Trigger BGSAVE
    const char *snap_file = "master_sync.snap";
    
    if (kv_bgsave(state->store, snap_file) != KV_OK) {
        const char *err = "-ERR Failed to start BGSAVE\r\n";
        send(socket_fd, err, strlen(err), 0);
        return -1;
    }
    
    // Wait for BGSAVE to complete
    int status;
    waitpid(state->store->bgsave_pid, &status, 0);
    state->store->bgsave_pid = 0;
    
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        const char *err = "-ERR BGSAVE failed\r\n";
        send(socket_fd, err, strlen(err), 0);
        return -1;
    }
    
    // Open snapshot and send it
    FILE *fp = fopen(snap_file, "rb");
    if (!fp) {
        const char *err = "-ERR Could not open snapshot\r\n";
        send(socket_fd, err, strlen(err), 0);
        return -1;
    }
    
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    char header[64];
    int header_len = snprintf(header, sizeof(header), "+SNAPSHOT %ld\r\n", size);
    send(socket_fd, header, (size_t)header_len, 0);
    
    char buf[4096];
    size_t bytes_read;
    while ((bytes_read = fread(buf, 1, sizeof(buf), fp)) > 0) {
        send(socket_fd, buf, bytes_read, 0);
    }
    fclose(fp);
    
    // Add to replica broadcast list
    replication_add_replica(state, socket_fd);
    
    return 0;
}

static void *replica_thread_main(void *arg) {
    replication_state_t *state = (replication_state_t *)arg;
    
    while (state->is_running) {
        int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd < 0) {
            sleep(1);
            continue;
        }
        
        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(state->master_port);
        
        if (inet_pton(AF_INET, state->master_host, &server_addr.sin_addr) <= 0) {
            close(socket_fd);
            sleep(1);
            continue;
        }
        
        if (connect(socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            close(socket_fd);
            sleep(1);
            continue;
        }
        
        // Send SYNC
        const char *sync_cmd = "SYNC\r\n";
        if (send(socket_fd, sync_cmd, strlen(sync_cmd), 0) < 0) {
            close(socket_fd);
            sleep(1);
            continue;
        }
        
        // Wait for snapshot
        char header[64];
        size_t i = 0;
        while (i < sizeof(header) - 1) {
            ssize_t r = recv(socket_fd, &header[i], 1, 0);
            if (r <= 0) break;
            if (header[i] == '\n') {
                header[i+1] = '\0';
                break;
            }
            i++;
        }
        
        if (strncmp(header, "+SNAPSHOT ", 10) != 0) {
            close(socket_fd);
            sleep(1);
            continue;
        }
        
        long snap_size = atol(header + 10);
        const char *replica_snap = "replica_sync.snap";
        FILE *fp = fopen(replica_snap, "wb");
        if (fp) {
            long remaining = snap_size;
            char buf[4096];
            while (remaining > 0) {
                size_t to_read = (size_t)remaining > sizeof(buf) ? sizeof(buf) : (size_t)remaining;
                ssize_t r = recv(socket_fd, buf, to_read, 0);
                if (r <= 0) break;
                fwrite(buf, 1, (size_t)r, fp);
                remaining -= (long)r;
            }
            fclose(fp);
            
            // Reload store
            kv_destroy(state->store);
            kv_init_full(state->store, 1024, KV_DEFAULT_MAX_CAPACITY, KV_DEFAULT_MEMORY_BUDGET, NULL);
            kv_load_snapshot(state->store, replica_snap);
        }
        
        // Start receiving streaming AOF commands
        char cmdbuf[4096];
        size_t cmdlen = 0;
        while (state->is_running) {
            ssize_t r = recv(socket_fd, cmdbuf + cmdlen, sizeof(cmdbuf) - cmdlen - 1, 0);
            if (r <= 0) break;
            cmdlen += (size_t)r;
            cmdbuf[cmdlen] = '\0';
            
            // Split by newline and apply commands (simple parser)
            char *p = cmdbuf;
            char *nl;
            while ((nl = strchr(p, '\n')) != NULL) {
                *nl = '\0';
                
                // For simplicity, we just look at SET and DELETE which use tabs in AOF format
                // In a real system, we might need a proper parser for AOF records
                if (strncmp(p, "SET\t", 4) == 0) {
                    char *key = p + 4;
                    char *tab = strchr(key, '\t');
                    if (tab) {
                        *tab = '\0';
                        char *val = tab + 1;
                        kv_set_internal(state->store, key, val);
                    }
                } else if (strncmp(p, "DELETE\t", 7) == 0) {
                    char *key = p + 7;
                    kv_delete_internal(state->store, key);
                }
                
                p = nl + 1;
            }
            
            // Shift remaining bytes
            size_t consumed = (size_t)(p - cmdbuf);
            if (consumed < cmdlen) {
                memmove(cmdbuf, p, cmdlen - consumed);
                cmdlen -= consumed;
            } else {
                cmdlen = 0;
            }
        }
        
        close(socket_fd);
        sleep(1);
    }
    
    return NULL;
}

void replication_start(replication_state_t *state) {
    if (state->mode == REPLICA_MODE_REPLICA) {
        state->is_running = 1;
        pthread_create(&state->replica_thread, NULL, replica_thread_main, state);
    }
}

void replication_stop(replication_state_t *state) {
    if (!state->is_running) return;
    state->is_running = 0;
    if (state->mode == REPLICA_MODE_REPLICA) {
        pthread_join(state->replica_thread, NULL);
    }
    
    if (state->mode == REPLICA_MODE_MASTER) {
        pthread_mutex_lock(&state->replicas_lock);
        replica_node_t *curr = state->replicas_head;
        while (curr) {
            replica_node_t *next = curr->next;
            close(curr->socket_fd);
            free(curr);
            curr = next;
        }
        state->replicas_head = NULL;
        pthread_mutex_unlock(&state->replicas_lock);
    }
    
    if (state->master_host) {
        free(state->master_host);
        state->master_host = NULL;
    }
}
