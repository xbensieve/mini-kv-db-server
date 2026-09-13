#ifndef REPLICATION_H
#define REPLICATION_H

#include <stdint.h>
#include <pthread.h>
#include <stddef.h>
#include "kv.h"

typedef enum {
    REPLICA_MODE_NONE = 0,
    REPLICA_MODE_MASTER = 1,
    REPLICA_MODE_REPLICA = 2
} replica_mode_t;

typedef struct replica_node {
    int socket_fd;
    struct replica_node *next;
} replica_node_t;

typedef struct {
    replica_mode_t mode;
    char *master_host;
    uint16_t master_port;
    pthread_t replica_thread;
    int is_running;
    
    // For master
    replica_node_t *replicas_head;
    pthread_mutex_t replicas_lock;
    
    // Store reference
    kv_store_t *store;
} replication_state_t;

extern replication_state_t g_replication_state;

void replication_init(replication_state_t *state, kv_store_t *store, const char *master_host, uint16_t master_port);
void replication_start(replication_state_t *state);
void replication_stop(replication_state_t *state);

// Master APIs
void replication_add_replica(replication_state_t *state, int socket_fd);
void replication_broadcast(replication_state_t *state, const char *data, size_t len);
int replication_handle_sync(replication_state_t *state, int socket_fd);

#endif /* REPLICATION_H */
