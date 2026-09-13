#define _POSIX_C_SOURCE 200809L

/**
 * @file benchmark.c
 * @brief High-performance concurrent TCP benchmarking client for Mini Key-Value Database Server.
 *
 * Spawns N concurrent worker threads, each maintaining an active TCP connection to evaluate
 * server throughput (QPS), average latency, and command handling under contention.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BENCH_DEFAULT_HOST "127.0.0.1"
#define BENCH_DEFAULT_PORT 6379
#define BENCH_DEFAULT_CONCURRENCY 16
#define BENCH_DEFAULT_REQUESTS 20000
#define BENCH_BUFFER_SIZE 1024

typedef struct {
    const char *host;
    uint16_t port;
    int concurrency;
    int total_requests;
} bench_config_t;

typedef struct {
    int thread_id;
    const bench_config_t *config;
    int requests_to_run;
    _Atomic uint64_t completed_sets;
    _Atomic uint64_t completed_gets;
    _Atomic uint64_t error_count;
    double thread_elapsed_sec;
} worker_context_t;

/**
 * @brief Reads a full line (terminated with '\n') from a connected TCP socket.
 */
static ssize_t bench_recv_line(int socket_fd, char *buffer, size_t max_length) {
    size_t bytes_read = 0U;
    while (bytes_read + 1U < max_length) {
        char ch = '\0';
        ssize_t result = recv(socket_fd, &ch, 1U, 0);
        if (result <= 0) {
            return -1;
        }
        buffer[bytes_read++] = ch;
        if (ch == '\n') {
            break;
        }
    }
    buffer[bytes_read] = '\0';
    return (ssize_t)bytes_read;
}

/**
 * @brief Establishes a TCP stream connection to the target server.
 */
static int bench_connect_socket(const char *host, uint16_t port) {
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &server_addr.sin_addr) <= 0) {
        close(socket_fd);
        return -1;
    }

    if (connect(socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

/**
 * @brief Thread entry routine executing concurrent SET and GET pipelines.
 */
static void *bench_worker_main(void *arg) {
    worker_context_t *ctx = (worker_context_t *)arg;
    int socket_fd = bench_connect_socket(ctx->config->host, ctx->config->port);
    if (socket_fd < 0) {
        atomic_fetch_add(&ctx->error_count, 1U);
        return NULL;
    }

    char send_buf[BENCH_BUFFER_SIZE];
    char recv_buf[BENCH_BUFFER_SIZE];

    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    for (int i = 0; i < ctx->requests_to_run; ++i) {
        /* 1. Send SET request */
        int set_len = snprintf(send_buf, sizeof(send_buf), "SET bench_t%d_k%d bench_val_%d\r\n",
                               ctx->thread_id, i, i);
        if (send(socket_fd, send_buf, (size_t)set_len, 0) != (ssize_t)set_len) {
            atomic_fetch_add(&ctx->error_count, 1U);
            break;
        }

        if (bench_recv_line(socket_fd, recv_buf, sizeof(recv_buf)) <= 0) {
            atomic_fetch_add(&ctx->error_count, 1U);
            break;
        }
        atomic_fetch_add(&ctx->completed_sets, 1U);

        /* 2. Send GET request */
        int get_len = snprintf(send_buf, sizeof(send_buf), "GET bench_t%d_k%d\r\n",
                               ctx->thread_id, i);
        if (send(socket_fd, send_buf, (size_t)get_len, 0) != (ssize_t)get_len) {
            atomic_fetch_add(&ctx->error_count, 1U);
            break;
        }

        if (bench_recv_line(socket_fd, recv_buf, sizeof(recv_buf)) <= 0) {
            atomic_fetch_add(&ctx->error_count, 1U);
            break;
        }
        atomic_fetch_add(&ctx->completed_gets, 1U);
    }

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    ctx->thread_elapsed_sec = (double)(end_time.tv_sec - start_time.tv_sec) +
                              (double)(end_time.tv_nsec - start_time.tv_nsec) / 1e9;

    close(socket_fd);
    return NULL;
}

static void print_usage(const char *prog_name) {
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -h <host>         Server hostname/IP (default: %s)\n", BENCH_DEFAULT_HOST);
    printf("  -p <port>         Server TCP port (default: %d)\n", BENCH_DEFAULT_PORT);
    printf("  -c <clients>      Number of parallel client threads (default: %d)\n", BENCH_DEFAULT_CONCURRENCY);
    printf("  -n <requests>     Total number of request pairs (SET + GET) (default: %d)\n", BENCH_DEFAULT_REQUESTS);
    printf("  --help            Display this help message\n");
}

int main(int argc, char *argv[]) {
    bench_config_t config = {
        .host = BENCH_DEFAULT_HOST,
        .port = BENCH_DEFAULT_PORT,
        .concurrency = BENCH_DEFAULT_CONCURRENCY,
        .total_requests = BENCH_DEFAULT_REQUESTS
    };

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            config.host = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            config.port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config.concurrency = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            config.total_requests = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (config.concurrency <= 0) config.concurrency = 1;
    if (config.total_requests <= 0) config.total_requests = 1000;

    int reqs_per_thread = config.total_requests / config.concurrency;

    printf("==================================================================\n");
    printf("  Mini Key-Value Database Server — Performance Benchmark          \n");
    printf("==================================================================\n");
    printf("Server Target:      %s:%u\n", config.host, config.port);
    printf("Concurrency:        %d client threads\n", config.concurrency);
    printf("Target Requests:    %d SET + %d GET operations (%d total operations)\n",
           config.total_requests, config.total_requests, config.total_requests * 2);
    printf("Requests/Thread:    %d pairs\n", reqs_per_thread);
    printf("------------------------------------------------------------------\n");
    printf("Executing benchmark...\n");

    pthread_t *threads = malloc((size_t)config.concurrency * sizeof(pthread_t));
    worker_context_t *contexts = calloc((size_t)config.concurrency, sizeof(worker_context_t));

    struct timespec global_start, global_end;
    clock_gettime(CLOCK_MONOTONIC, &global_start);

    for (int i = 0; i < config.concurrency; ++i) {
        contexts[i].thread_id = i;
        contexts[i].config = &config;
        contexts[i].requests_to_run = reqs_per_thread;
        atomic_init(&contexts[i].completed_sets, 0U);
        atomic_init(&contexts[i].completed_gets, 0U);
        atomic_init(&contexts[i].error_count, 0U);
        if (pthread_create(&threads[i], NULL, bench_worker_main, &contexts[i]) != 0) {
            fprintf(stderr, "Failed to spawn worker thread %d\n", i);
        }
    }

    uint64_t total_sets = 0U;
    uint64_t total_gets = 0U;
    uint64_t total_errors = 0U;

    for (int i = 0; i < config.concurrency; ++i) {
        pthread_join(threads[i], NULL);
        total_sets += atomic_load(&contexts[i].completed_sets);
        total_gets += atomic_load(&contexts[i].completed_gets);
        total_errors += atomic_load(&contexts[i].error_count);
    }

    clock_gettime(CLOCK_MONOTONIC, &global_end);
    double elapsed_sec = (double)(global_end.tv_sec - global_start.tv_sec) +
                         (double)(global_end.tv_nsec - global_start.tv_nsec) / 1e9;

    uint64_t total_ops = total_sets + total_gets;
    double qps = (elapsed_sec > 0.0) ? ((double)total_ops / elapsed_sec) : 0.0;
    double avg_latency_ms = (total_ops > 0U) ? ((elapsed_sec * 1000.0) / (double)total_ops) : 0.0;

    printf("------------------------------------------------------------------\n");
    printf("Benchmark Results:\n");
    printf("  Total Operations: %" PRIu64 " (%" PRIu64 " SETs, %" PRIu64 " GETs)\n", total_ops, total_sets, total_gets);
    printf("  Failed Operations: %" PRIu64 "\n", total_errors);
    printf("  Wall Clock Time:  %.4f seconds\n", elapsed_sec);
    printf("  Throughput (QPS): %.2f ops/sec\n", qps);
    printf("  Average Latency:  %.4f ms/op\n", avg_latency_ms);
    printf("==================================================================\n");

    free(threads);
    free(contexts);
    return 0;
}
