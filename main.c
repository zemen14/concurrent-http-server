#define _POSIX_C_SOURCE 200809L
#define MAX_THREADS 128

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "queue.h"
#include "util.h"
#include "http.h"

static work_queue_t g_queue;

static int create_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    int optval = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t) port);

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, SOMAXCONN) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void *worker_thread(void *arg) {
    (void) arg;

    for (;;) {
        work_item_t item = queue_pop(&g_queue);
        http_process_connection(item.client_fd);
        close(item.client_fd);
    }

    return NULL;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    int parallelism = 1;
    int port = 0;
    int port_arg_index = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Invalid Port\n");
                return 1;
            }

            bool ok = false;
            parallelism = parse_int(argv[++i], &ok);

            if (!ok || parallelism <= 0) {
                fprintf(stderr, "Invalid Port\n");
                return 1;
            }
        } else {
            port_arg_index = i;
        }
    }

    if (port_arg_index == -1) {
        fprintf(stderr, "Invalid Port\n");
        return 1;
    }

    bool ok = false;
    port = parse_int(argv[port_arg_index], &ok);

    if (!ok || port < 1 || port > 65535) {
        fprintf(stderr, "Invalid Port\n");
        return 1;
    }

    int listen_fd = create_listen_socket(port);

    if (listen_fd < 0) {
        fprintf(stderr, "Invalid Port\n");
        return 1;
    }

    if (parallelism < 1) {
        parallelism = 1;
    }

    if (parallelism > MAX_THREADS) {
        parallelism = MAX_THREADS;
    }

    // Single-threaded execution for parallelism = 1.
    if (parallelism == 1) {
        for (;;) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);

            int client_fd = accept(
                listen_fd, (struct sockaddr *) &client_addr, &client_len);

            if (client_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                continue;
            }

            http_process_connection(client_fd);
            close(client_fd);
        }
    }

    // Multithreaded execution.
    queue_init(&g_queue);

    pthread_t threads[MAX_THREADS];

    for (int i = 0; i < parallelism; i++) {
        if (pthread_create(&threads[i], NULL, worker_thread, NULL) != 0) {
            close(listen_fd);
            fprintf(stderr, "Invalid Port\n");
            return 1;
        }
    }

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(
            listen_fd, (struct sockaddr *) &client_addr, &client_len);

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            continue;
        }

        work_item_t item;
        item.client_fd = client_fd;
        queue_push(&g_queue, item);
    }

    return 0;
}
