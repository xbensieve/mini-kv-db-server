#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "server.h"
#include "thread_pool.h"
#include "protocol.h"
#include "eviction.h"
#include "kv_internal.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

int server_send_all(int fd, const char *buf, size_t len) {
    size_t total_sent = 0U;
    while (total_sent < len) {
        ssize_t n = send(fd, buf + total_sent, len - total_sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(100);
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        total_sent += (size_t)n;
    }
    return 0;
}

static void client_release_ref(server_t *server, server_client_t *client) {
    pthread_mutex_lock(&server->clients_lock);
    client->ref_count--;
    int free_needed = (client->ref_count == 0);
    pthread_mutex_unlock(&server->clients_lock);

    if (free_needed) {
        client_cmd_t *cmd = client->cmd_queue_head;
        while (cmd != NULL) {
            client_cmd_t *next = cmd->next;
            free(cmd);
            cmd = next;
        }
        pthread_mutex_destroy(&client->client_lock);
        free(client);
    }
}

static void server_close_client(server_t *server, server_client_t *client) {
    if (server == NULL || client == NULL) {
        return;
    }

    pthread_mutex_lock(&server->clients_lock);
    pthread_mutex_lock(&client->client_lock);

    if (client->fd < 0) {
        /* Already closed */
        pthread_mutex_unlock(&client->client_lock);
        pthread_mutex_unlock(&server->clients_lock);
        return;
    }

    client->closed = 1;

    if (server->epoll_fd >= 0) {
        epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
    }
    close(client->fd);
    client->fd = -1;

    pthread_mutex_unlock(&client->client_lock);

    server_client_t **curr = &server->clients;
    while (*curr != NULL) {
        if (*curr == client) {
            *curr = client->next;
            if (server->client_count > 0U) {
                server->client_count--;
            }
            break;
        }
        curr = &(*curr)->next;
    }

    client->ref_count--;
    int free_needed = (client->ref_count == 0);
    pthread_mutex_unlock(&server->clients_lock);

    if (free_needed) {
        client_cmd_t *cmd = client->cmd_queue_head;
        while (cmd != NULL) {
            client_cmd_t *next = cmd->next;
            free(cmd);
            cmd = next;
        }
        pthread_mutex_destroy(&client->client_lock);
        free(client);
    }
}

static server_client_t *server_find_client(server_t *server, int fd) {
    if (server == NULL) {
        return NULL;
    }

    pthread_mutex_lock(&server->clients_lock);
    server_client_t *curr = server->clients;
    while (curr != NULL) {
        if (curr->fd == fd) {
            pthread_mutex_unlock(&server->clients_lock);
            return curr;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&server->clients_lock);
    return NULL;
}



static void server_worker_client_task(void *arg) {
    server_client_t *client = (server_client_t *)arg;
    server_t *server = client->server;

    while (1) {
        pthread_mutex_lock(&client->client_lock);

        if (client->closed || client->cmd_queue_head == NULL) {
            client->task_in_flight = 0;
            pthread_mutex_unlock(&client->client_lock);
            break;
        }

        client_cmd_t *cmd = client->cmd_queue_head;
        client->cmd_queue_head = cmd->next;
        if (client->cmd_queue_head == NULL) {
            client->cmd_queue_tail = NULL;
        }
        pthread_mutex_unlock(&client->client_lock);

        char resp_buf[SERVER_MAX_LINE_LEN + 128U];
        size_t resp_len = 0U;
        int should_close = 0;
        int should_detach = 0;

        protocol_execute_command(server, client->fd, cmd->line, cmd->len,
                               resp_buf, sizeof(resp_buf), &resp_len,
                               &should_close, &should_detach);
        free(cmd);

        int do_close = 0;
        int do_detach = 0;
        pthread_mutex_lock(&client->client_lock);
        if (!client->closed && client->fd >= 0 && resp_len > 0U) {
            if (server_send_all(client->fd, resp_buf, resp_len) != 0) {
                do_close = 1;
            }
        }
        if (should_close) {
            do_close = 1;
        }
        if (should_detach) {
            do_detach = 1;
            if (server->epoll_fd >= 0 && client->fd >= 0) {
                epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
            }
            client->fd = -1; // Detached, don't close it
        }
        pthread_mutex_unlock(&client->client_lock);

        if (do_detach || do_close) {
            server_close_client(server, client);
            break;
        }
    }

    client_release_ref(server, client);
}

static int client_handle_read(server_t *server, server_client_t *client) {
    while (1) {
        size_t avail = sizeof(client->read_buf) - client->read_len;
        if (avail == 0U) {
            return -1;
        }

        ssize_t n = recv(client->fd, client->read_buf + client->read_len, avail, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }

        client->read_len += (size_t)n;
    }

    while (1) {
        char *newline_ptr = memchr(client->read_buf, '\n', client->read_len);
        if (newline_ptr == NULL) {
            if (client->read_len >= SERVER_MAX_LINE_LEN) {
                return -1;
            }
            break;
        }

        size_t line_len = (size_t)(newline_ptr - client->read_buf);
        size_t total_consumed = line_len + 1U;

        if (line_len > SERVER_MAX_LINE_LEN) {
            return -1;
        }

        size_t cmd_len = line_len;
        if (cmd_len > 0U && client->read_buf[cmd_len - 1U] == '\r') {
            cmd_len--;
        }

        client_cmd_t *cmd = malloc(sizeof(client_cmd_t));
        if (cmd == NULL) {
            return -1;
        }
        memcpy(cmd->line, client->read_buf, cmd_len);
        cmd->line[cmd_len] = '\0';
        cmd->len = cmd_len;
        cmd->next = NULL;

        if (client->read_len > total_consumed) {
            memmove(client->read_buf, client->read_buf + total_consumed,
                    client->read_len - total_consumed);
            client->read_len -= total_consumed;
        } else {
            client->read_len = 0U;
        }

        pthread_mutex_lock(&server->clients_lock);
        pthread_mutex_lock(&client->client_lock);
        if (client->cmd_queue_tail != NULL) {
            client->cmd_queue_tail->next = cmd;
        } else {
            client->cmd_queue_head = cmd;
        }
        client->cmd_queue_tail = cmd;

        if (!client->task_in_flight && !client->closed) {
            client->task_in_flight = 1;
            client->ref_count++;
            int sub_rc = thread_pool_submit(server->pool, server_worker_client_task, client);
            if (sub_rc != KV_OK) {
                client->task_in_flight = 0;
                client->ref_count--;
            }
        }
        pthread_mutex_unlock(&client->client_lock);
        pthread_mutex_unlock(&server->clients_lock);
    }

    return 0;
}

/**
 * @brief Task function for active TTL sweep dispatched to the thread pool.
 */


int server_init(server_t *server, kv_store_t *store, uint16_t port) {
    if (server == NULL || store == NULL) {
        return -1;
    }

    memset(server, 0, sizeof(*server));
    server->store = store;
    server->store->bgsave_pid = 0;



    server->listen_fd = -1;
    server->epoll_fd = -1;
    server->shutdown_fd = -1;
    server->timer_fd = -1;

    signal(SIGPIPE, SIG_IGN);

    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) {
        return -1;
    }

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        close(listen_fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, 128) < 0) {
        close(listen_fd);
        return -1;
    }

    struct sockaddr_in bound_addr;
    socklen_t addr_len = sizeof(bound_addr);
    if (getsockname(listen_fd, (struct sockaddr *)&bound_addr, &addr_len) < 0) {
        close(listen_fd);
        return -1;
    }
    server->port = ntohs(bound_addr.sin_port);

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        close(listen_fd);
        return -1;
    }

    int shutdown_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (shutdown_fd < 0) {
        close(epoll_fd);
        close(listen_fd);
        return -1;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        close(shutdown_fd);
        close(epoll_fd);
        close(listen_fd);
        return -1;
    }

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = shutdown_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, shutdown_fd, &ev) < 0) {
        close(shutdown_fd);
        close(epoll_fd);
        close(listen_fd);
        return -1;
    }

    /* Create timerfd for periodic TTL sweep (100ms interval) */
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd >= 0) {
        struct itimerspec its;
        memset(&its, 0, sizeof(its));
        its.it_interval.tv_sec = 0;
        its.it_interval.tv_nsec = 100000000; /* 100ms */
        its.it_value.tv_sec = 0;
        its.it_value.tv_nsec = 100000000; /* First fire after 100ms */
        if (timerfd_settime(tfd, 0, &its, NULL) < 0) {
            close(tfd);
            tfd = -1;
        } else {
            memset(&ev, 0, sizeof(ev));
            ev.events = EPOLLIN;
            ev.data.fd = tfd;
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, tfd, &ev) < 0) {
                close(tfd);
                tfd = -1;
            }
        }
    }
    server->timer_fd = tfd;

    server->pool = thread_pool_create(SERVER_DEFAULT_WORKERS, SERVER_DEFAULT_QUEUE_SIZE);
    if (server->pool == NULL) {
        if (tfd >= 0) {
            close(tfd);
        }
        close(shutdown_fd);
        close(epoll_fd);
        close(listen_fd);
        return -1;
    }

    if (pthread_mutex_init(&server->clients_lock, NULL) != 0) {
        thread_pool_destroy(server->pool);
        server->pool = NULL;
        if (tfd >= 0) {
            close(tfd);
        }
        close(shutdown_fd);
        close(epoll_fd);
        close(listen_fd);
        return -1;
    }

    server->listen_fd = listen_fd;
    server->epoll_fd = epoll_fd;
    server->shutdown_fd = shutdown_fd;
    atomic_store(&server->running, 0);

    return 0;
}

uint16_t server_get_port(const server_t *server) {
    if (server == NULL) {
        return 0U;
    }
    return server->port;
}

void server_stop(server_t *server) {
    if (server == NULL) {
        return;
    }

    atomic_store(&server->running, 0);
    if (server->shutdown_fd >= 0) {
        uint64_t val = 1U;
        ssize_t written = write(server->shutdown_fd, &val, sizeof(val));
        (void)written;
    }
}

static void eviction_sweep_task(void *arg) {
    kv_store_t *store = (kv_store_t *)arg;
    kv_expire_sample_sweep(store, 20U, 10U);
}

int server_run(server_t *server) {
    if (server == NULL || server->epoll_fd < 0) {
        return -1;
    }

    atomic_store(&server->running, 1);
    struct epoll_event events[SERVER_MAX_EVENTS];

    while (atomic_load(&server->running)) {
        int nfds = epoll_wait(server->epoll_fd, events, SERVER_MAX_EVENTS, 100);
        if (!atomic_load(&server->running)) {
            break;
        }
        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            uint32_t evs = events[i].events;

            if (fd == server->shutdown_fd) {
                uint64_t val = 0U;
                ssize_t n = read(server->shutdown_fd, &val, sizeof(val));
                (void)n;
                atomic_store(&server->running, 0);
                break;
            }

            /* Handle timerfd for active TTL sweep */
            if (fd == server->timer_fd) {
                uint64_t expirations = 0U;
                ssize_t n = read(server->timer_fd, &expirations, sizeof(expirations));
                (void)n;
                /* Best-effort: submit sweep task to pool, ignore if queue is full */
                (void)thread_pool_submit(server->pool, eviction_sweep_task, server->store);
                continue;
            }

            if (fd == server->listen_fd) {
                while (1) {
                    int client_fd = accept4(server->listen_fd, NULL, NULL,
                                            SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        break;
                    }

                    if (server->client_count >= SERVER_MAX_CLIENTS) {
                        close(client_fd);
                        break;
                    }

                    server_client_t *client = calloc(1U, sizeof(server_client_t));
                    if (client == NULL) {
                        close(client_fd);
                        break;
                    }

                    client->fd = client_fd;
                    client->server = server;
                    client->ref_count = 1;
                    pthread_mutex_init(&client->client_lock, NULL);

                    pthread_mutex_lock(&server->clients_lock);
                    client->next = server->clients;
                    server->clients = client;
                    server->client_count++;
                    pthread_mutex_unlock(&server->clients_lock);

                    struct epoll_event client_ev;
                    memset(&client_ev, 0, sizeof(client_ev));
                    client_ev.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR;
                    client_ev.data.fd = client_fd;

                    if (epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev) < 0) {
                        server_close_client(server, client);
                        break;
                    }
                }
                continue;
            }

            server_client_t *client = server_find_client(server, fd);
            if (client == NULL) {
                continue;
            }

            if (evs & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                server_close_client(server, client);
                continue;
            }

            if (evs & EPOLLIN) {
                if (client_handle_read(server, client) != 0) {
                    server_close_client(server, client);
                    continue;
                }
            }
        }

        /* Reap background save/rewrite child if any */
        pid_t p;
        int status;
        while ((p = waitpid(-1, &status, WNOHANG)) > 0) {
            if (p == server->store->bgsave_pid) {
                if (WIFEXITED(status)) {
                    printf("Background save completed with exit code %d\n", WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                    printf("Background save terminated by signal %d\n", WTERMSIG(status));
                }
                server->store->bgsave_pid = 0;
            } else if (p == server->store->aof_rewrite_pid) {
                if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                    printf("Background AOF rewrite completed. Merging...\n");
                    if (kv_merge_aof_rewrite(server->store, p) == KV_OK) {
                        printf("Background AOF merge successful.\n");
                    } else {
                        printf("Background AOF merge failed.\n");
                    }
                } else {
                    printf("Background AOF rewrite failed or terminated. Discarding...\n");
                    /* Clean up state */
                    kv_discard_aof_rewrite(server->store);
                    
                    /* Delete temp file */
                    char tmp_path[512];
                    snprintf(tmp_path, sizeof(tmp_path), "temp-rewrite-aof-%d.aof", p);
                    unlink(tmp_path);
                }
            }
        }
    }

    return 0;
}

void server_destroy(server_t *server) {
    if (server == NULL) {
        return;
    }

    atomic_store(&server->running, 0);

    /* 1. Tear down thread pool first: workers drain and join before client/store teardown */
    if (server->pool != NULL) {
        thread_pool_destroy(server->pool);
        server->pool = NULL;
    }

    /* 2. All workers stopped. Safely clean up all client connections */
    pthread_mutex_lock(&server->clients_lock);
    server_client_t *curr = server->clients;
    while (curr != NULL) {
        server_client_t *next = curr->next;
        if (curr->fd >= 0) {
            if (server->epoll_fd >= 0) {
                epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, curr->fd, NULL);
            }
            close(curr->fd);
            curr->fd = -1;
        }
        client_cmd_t *cmd = curr->cmd_queue_head;
        while (cmd != NULL) {
            client_cmd_t *cnext = cmd->next;
            free(cmd);
            cmd = cnext;
        }
        pthread_mutex_destroy(&curr->client_lock);
        free(curr);
        curr = next;
    }
    server->clients = NULL;
    server->client_count = 0U;
    pthread_mutex_unlock(&server->clients_lock);

    pthread_mutex_destroy(&server->clients_lock);

    /* 3. Close descriptors */
    if (server->timer_fd >= 0) {
        if (server->epoll_fd >= 0) {
            epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, server->timer_fd, NULL);
        }
        close(server->timer_fd);
        server->timer_fd = -1;
    }

    if (server->shutdown_fd >= 0) {
        if (server->epoll_fd >= 0) {
            epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, server->shutdown_fd, NULL);
        }
        close(server->shutdown_fd);
        server->shutdown_fd = -1;
    }

    if (server->listen_fd >= 0) {
        if (server->epoll_fd >= 0) {
            epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, server->listen_fd, NULL);
        }
        close(server->listen_fd);
        server->listen_fd = -1;
    }

    if (server->epoll_fd >= 0) {
        close(server->epoll_fd);
        server->epoll_fd = -1;
    }

    server->store = NULL;
}
