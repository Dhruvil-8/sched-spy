#include "src/correlator.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void cfg_for_pid(SchedSpyConfig *cfg, pid_t pid) {
    sched_spy_default_config(cfg);
    assert(sched_spy_config_add_pid(cfg, pid) == 0);
}

static RawEvent wakeup(pid_t pid, uint64_t ns, int cpu, const char *comm) {
    RawEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = RAW_SCHED_WAKEUP;
    ev.timestamp_ns = ns;
    ev.pid = pid;
    ev.cpu = cpu;
    ev.target_cpu = cpu;
    snprintf(ev.comm, sizeof(ev.comm), "%s", comm);
    return ev;
}

static RawEvent sw(pid_t next_pid, uint64_t ns, int cpu) {
    RawEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = RAW_SCHED_SWITCH;
    ev.timestamp_ns = ns;
    ev.cpu = cpu;
    ev.prev_pid = 999;
    ev.next_pid = next_pid;
    snprintf(ev.prev_comm, sizeof(ev.prev_comm), "busy");
    snprintf(ev.next_comm, sizeof(ev.next_comm), "target");
    return ev;
}

static void test_basic_correlation(void) {
    SchedSpyConfig cfg;
    cfg_for_pid(&cfg, 1234);
    MetricsStore store;
    assert(metrics_init(&store, cfg.threshold_ns) == 0);
    Correlator c;
    correlator_init(&c, &cfg, &store, NULL);

    RawEvent w = wakeup(1234, 1000000, 0, "nginx");
    RawEvent s = sw(1234, 1002000, 0);
    correlator_process(&c, &w);
    correlator_process(&c, &s);

    PidMetrics *m = metrics_get(&store, 1234);
    assert(m);
    assert(m->count == 1);
    assert(m->max_ns == 2000);
    metrics_destroy(&store);
}

static void test_migration_detected(void) {
    SchedSpyConfig cfg;
    cfg_for_pid(&cfg, 55);
    MetricsStore store;
    assert(metrics_init(&store, cfg.threshold_ns) == 0);
    Correlator c;
    correlator_init(&c, &cfg, &store, NULL);

    RawEvent w = wakeup(55, 10, 0, "app");
    RawEvent m;
    memset(&m, 0, sizeof(m));
    m.type = RAW_SCHED_MIGRATE;
    m.pid = 55;
    m.dest_cpu = 1;
    RawEvent s = sw(55, 20, 1);
    correlator_process(&c, &w);
    correlator_process(&c, &m);
    correlator_process(&c, &s);

    PidMetrics *pm = metrics_get(&store, 55);
    assert(pm);
    assert(pm->migrations == 0);
    assert(pm->cpu_mask == 2);
    metrics_destroy(&store);
}

static void test_comm_filter(void) {
    SchedSpyConfig cfg;
    sched_spy_default_config(&cfg);
    assert(sched_spy_config_add_comm(&cfg, "nginx") == 0);
    MetricsStore store;
    assert(metrics_init(&store, cfg.threshold_ns) == 0);
    Correlator c;
    correlator_init(&c, &cfg, &store, NULL);

    RawEvent w1 = wakeup(1, 100, 0, "other");
    RawEvent s1 = sw(1, 200, 0);
    RawEvent w2 = wakeup(2, 100, 0, "nginx");
    RawEvent s2 = sw(2, 200, 0);
    correlator_process(&c, &w1);
    correlator_process(&c, &s1);
    correlator_process(&c, &w2);
    correlator_process(&c, &s2);

    assert(metrics_get(&store, 1) == NULL);
    assert(metrics_get(&store, 2) != NULL);
    metrics_destroy(&store);
}

int main(void) {
    test_basic_correlation();
    test_migration_detected();
    test_comm_filter();
    puts("PASS: test_correlator");
    return 0;
}
