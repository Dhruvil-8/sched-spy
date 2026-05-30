#ifndef SCHED_SPY_CORRELATOR_H
#define SCHED_SPY_CORRELATOR_H

#include "metrics.h"
#include "output_json.h"

#define WAKEUP_MAP_SIZE 4096

typedef struct {
    pid_t pid;
    uint64_t timestamp_ns;
    int cpu;
    char comm[SCHED_SPY_COMM_LEN];
    int occupied;
} WakeupEntry;

typedef struct {
    WakeupEntry entries[WAKEUP_MAP_SIZE];
    uint64_t inserts;
    uint64_t hits;
    uint64_t misses;
    uint64_t stale_evictions;
    const SchedSpyConfig *cfg;
    MetricsStore *metrics;
    JsonWriter *json;
} Correlator;

void correlator_init(Correlator *c, const SchedSpyConfig *cfg, MetricsStore *metrics, JsonWriter *json);
void correlator_process(Correlator *c, const RawEvent *ev);
void correlator_cleanup_stale(Correlator *c, uint64_t now_ns);

#endif
