#include "src/metrics.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static SchedEvent event_for(pid_t pid, uint64_t latency_ns) {
    SchedEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.pid = pid;
    snprintf(ev.comm, sizeof(ev.comm), "p%d", (int)pid);
    ev.latency_ns = latency_ns;
    ev.cpu = 1;
    return ev;
}

static void test_welford_mean(void) {
    MetricsStore store;
    assert(metrics_init(&store, 1000) == 0);
    for (int i = 1; i <= 1000; i++) {
        SchedEvent ev = event_for(10, (uint64_t)i);
        metrics_record(&store, &ev);
    }
    PidMetrics *m = metrics_get(&store, 10);
    assert(m);
    assert(m->count == 1000);
    assert(fabs(m->mean_ns - 500.5) < 0.001);
    assert(m->min_ns == 1);
    assert(m->max_ns == 1000);
    metrics_destroy(&store);
}

static void test_percentile_and_spikes(void) {
    MetricsStore store;
    assert(metrics_init(&store, 1) == 0);
    for (int i = 1; i <= 300; i++) {
        SchedEvent ev = event_for(20, (uint64_t)i);
        metrics_record(&store, &ev);
    }
    PidMetrics *m = metrics_get(&store, 20);
    assert(m);
    assert(metrics_percentile_ns(m, 50.0) >= 150);
    assert(metrics_percentile_ns(m, 99.0) >= 256);
    assert(store.spike_count == SCHED_SPY_SPIKES);
    metrics_destroy(&store);
}

int main(void) {
    test_welford_mean();
    test_percentile_and_spikes();
    puts("PASS: test_metrics");
    return 0;
}
