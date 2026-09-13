#ifndef SERVER_H
#define SERVER_H

#include "kv.h"
#include "thread_pool.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/**
 * @file server.h
 * @brief Non-blocking I/O multiplexed TCP Server with thread pool.
 *
 * Implements an epoll-based event loop handling network I/O, offloading command
 * execution to a thread pool with EPOLLONESHOT concurrency protection.
 */

#define SERVER_DEFAULT_PORT 6379U
#define SERVER_MAX_CLIENTS 1024U
#define SERVER_MAX_LINE_LEN 8192U
#define SERVER_READ_BUF_SIZE 16384U
#define SERVER_WRITE_BUF_SIZE 16384U
#define SERVER_MAX_EVENTS 64
#define SERVER_DEFAULT_WORKERS 4U
#define SERVER_DEFAULT_QUEUE_SIZE 1024U

/**
 * @brief Node in per-client FIFO command queue.
 */
typedef struct client_cmd {
    char line[SERVER_MAX_LINE_LEN + 1U];
    size_t len;
    struct client_cmd *next;
} client_cmd_t;

/**
 * @brief Per-client connection state and buffers.
 */
typedef struct server_client {
    int fd;                                     /**< Socket file descriptor. */
    char read_buf[SERVER_READ_BUF_SIZE];        /**< Buffer accumulating incoming fragmented bytes. */
    size_t read_len;                            /**< Number of valid unparsed bytes in read_buf. */
    int close_after_write;                      /**< 1 if client socket should close after response. */
    int closed;                                 /**< 1 if client is closed or marked for closing. */
    pthread_mutex_t client_lock;                /**< Mutex synchronizing client writes and command queue. */
    client_cmd_t *cmd_queue_head;               /**< Head of FIFO pending command queue. */
    client_cmd_t *cmd_queue_tail;               /**< Tail of FIFO pending command queue. */
    int task_in_flight;                         /**< 1 if a worker task is currently active for client. */
    int ref_count;                              /**< Active reference count (epoll list + worker thread). */
    struct server *server;                      /**< Pointer to parent server instance. */
    struct server_client *next;                 /**< Singly linked list pointer for active tracking. */
} server_client_t;

/**
 * @brief Server configuration and runtime state.
 */
typedef struct server {
    int listen_fd;                              /**< Non-blocking listening socket descriptor. */
    int epoll_fd;                               /**< epoll instance descriptor. */
    int shutdown_fd;                            /**< eventfd descriptor for wake-up and graceful shutdown. */
    uint16_t port;                              /**< TCP port bound (host byte order). */
    atomic_int running;                         /**< 1 while event loop runs, 0 when stopped. */
    kv_store_t *store;                          /**< Borrowed pointer to initialized KV store. */
    server_client_t *clients;                   /**< Head of singly linked list of active clients. */
    size_t client_count;                        /**< Current count of active client connections. */
    pthread_mutex_t clients_lock;               /**< Mutex protecting active clients linked list. */
    thread_pool_t *pool;                        /**< Worker pool for asynchronous command execution. */
    int timer_fd;                               /**< timerfd for periodic TTL sweep (-1 if inactive). */
} server_t;

/**
 * @brief Sends all bytes across socket descriptor handling partial sends.
 */
int server_send_all(int fd, const char *buf, size_t len);

/**
 * @brief Initializes a server instance with a given store and port.
 */
int server_init(server_t *server, kv_store_t *store, uint16_t port);

/**
 * @brief Retrieves the actual port bound to the listening socket.
 */
uint16_t server_get_port(const server_t *server);

/**
 * @brief Runs the single-threaded epoll event loop until server_stop() is called.
 */
int server_run(server_t *server);

/**
 * @brief Signals the server to terminate its event loop gracefully.
 */
void server_stop(server_t *server);

/**
 * @brief Cleans up listening socket, epoll, shutdown fd, active clients, and buffers.
 */
void server_destroy(server_t *server);

#endif /* SERVER_H */
