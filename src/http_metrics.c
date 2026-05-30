#include "http_metrics.h"

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void *server_thread(void *arg) {
    HttpMetricsServer *s = arg;
    while (*s->running) {
        struct sockaddr_in client;
        socklen_t len = sizeof(client);
        int fd = accept(s->server_fd, (struct sockaddr *)&client, &len);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        char req[1024];
        ssize_t n = read(fd, req, sizeof(req) - 1);
        if (n < 0) {
            close(fd);
            continue;
        }
        req[n] = '\0';

        FILE *out = fdopen(fd, "w");
        if (!out) {
            close(fd);
            continue;
        }
        if (strncmp(req, "GET /metrics", 12) == 0) {
            fprintf(out, "HTTP/1.1 200 OK\r\nContent-Type: text/plain; version=0.0.4\r\nConnection: close\r\n\r\n");
            metrics_write_prometheus(s->store, out);
        } else {
            fprintf(out,
                    "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
                    "<!doctype html><title>sched-spy</title><pre>Use /metrics for Prometheus text metrics.</pre>\n");
        }
        fclose(out);
    }
    return NULL;
}

int http_metrics_start(HttpMetricsServer *server, int port, volatile sig_atomic_t *running, MetricsStore *store) {
    memset(server, 0, sizeof(*server));
    server->port = port;
    server->running = running;
    server->store = store;
    server->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->server_fd < 0) {
        return -1;
    }

    int yes = 1;
    setsockopt(server->server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(server->server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(server->server_fd, 16) != 0) {
        close(server->server_fd);
        server->server_fd = -1;
        return -1;
    }

    pthread_t *thread = calloc(1, sizeof(*thread));
    if (!thread) {
        close(server->server_fd);
        server->server_fd = -1;
        return -1;
    }
    if (pthread_create(thread, NULL, server_thread, server) != 0) {
        free(thread);
        close(server->server_fd);
        server->server_fd = -1;
        return -1;
    }
    server->thread = thread;
    return 0;
}

void http_metrics_stop(HttpMetricsServer *server) {
    if (!server || server->server_fd < 0) {
        return;
    }
    shutdown(server->server_fd, SHUT_RDWR);
    close(server->server_fd);
    server->server_fd = -1;
    if (server->thread) {
        pthread_join(*(pthread_t *)server->thread, NULL);
        free(server->thread);
        server->thread = NULL;
    }
}
