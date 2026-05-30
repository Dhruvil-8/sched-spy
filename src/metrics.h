#ifndef SCHED_SPY_METRICS_H
#define SCHED_SPY_METRICS_H

#include "sched_spy.h"

int metrics_init(MetricsStore *store, uint64_t threshold_ns);
void metrics_destroy(MetricsStore *store);
void metrics_reset(MetricsStore *store, uint64_t threshold_ns);
void metrics_record(MetricsStore *store, const SchedEvent *event);
PidMetrics *metrics_get(MetricsStore *store, pid_t pid);
double metrics_stddev_ns(const PidMetrics *m);
uint64_t metrics_percentile_ns(const PidMetrics *m, double percentile);
void metrics_write_json(MetricsStore *store, FILE *out);
void metrics_write_prometheus(MetricsStore *store, FILE *out);
void metrics_print_text(MetricsStore *store, FILE *out);

#endif
