#ifndef SCHED_SPY_HTTP_METRICS_H
#define SCHED_SPY_HTTP_METRICS_H

#include "metrics.h"

typedef struct {
    int port;
    int server_fd;
    volatile sig_atomic_t *running;
    MetricsStore *store;
    void *thread;
} HttpMetricsServer;

int http_metrics_start(HttpMetricsServer *server, int port, volatile sig_atomic_t *running, MetricsStore *store);
void http_metrics_stop(HttpMetricsServer *server);

#endif
