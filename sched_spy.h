#ifndef SCHED_SPY_H
#define SCHED_SPY_H

#include <inttypes.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCHED_SPY_COMM_LEN 16
#define SCHED_SPY_MAX_TARGETS 64
#define SCHED_SPY_HIST_BUCKETS 65
#define SCHED_SPY_SPIKES 256

typedef enum {
    RAW_SCHED_WAKEUP = 1,
    RAW_SCHED_SWITCH = 2,
    RAW_SCHED_MIGRATE = 3,
    RAW_SCHED_WAKEUP_NEW = 4
} RawEventType;

typedef struct {
    RawEventType type;
    uint64_t timestamp_ns;
    int cpu;

    char comm[SCHED_SPY_COMM_LEN];
    pid_t pid;
    int prio;
    int target_cpu;

    char prev_comm[SCHED_SPY_COMM_LEN];
    pid_t prev_pid;
    int prev_prio;
    long prev_state;
    char next_comm[SCHED_SPY_COMM_LEN];
    pid_t next_pid;
    int next_prio;

    int orig_cpu;
    int dest_cpu;
} RawEvent;

typedef struct {
    pid_t pid;
    char comm[SCHED_SPY_COMM_LEN];
    uint64_t wakeup_ns;
    uint64_t run_ns;
    uint64_t latency_ns;
    char preempted_by[SCHED_SPY_COMM_LEN];
    pid_t preempted_by_pid;
    int cpu;
    int migrated;
} SchedEvent;

typedef struct {
    int all;
    pid_t pids[SCHED_SPY_MAX_TARGETS];
    size_t pid_count;
    char comms[SCHED_SPY_MAX_TARGETS][SCHED_SPY_COMM_LEN];
    size_t comm_count;

    uint64_t threshold_ns;
    uint64_t min_latency_ns;
    uint64_t keep_alive_ns;
    int refresh_ms;
    int duration_sec;
    int no_terminal;
    int verbose;
    int force_tracefs;
    int serve_port;

    const char *json_path;
    const char *stats_json_path;
} SchedSpyConfig;

typedef struct {
    uint64_t count;
    uint64_t min_ns;
    uint64_t max_ns;
    double mean_ns;
    double m2_ns;
    uint64_t histogram[SCHED_SPY_HIST_BUCKETS];
    uint64_t migrations;
    uint64_t cpu_mask;
} PidMetrics;

typedef struct {
    SchedEvent event;
    uint64_t sequence;
} SpikeRecord;

typedef struct MetricsNode {
    pid_t pid;
    char comm[SCHED_SPY_COMM_LEN];
    PidMetrics metrics;
    struct MetricsNode *next;
} MetricsNode;

typedef struct {
    MetricsNode *head;
    SpikeRecord spikes[SCHED_SPY_SPIKES];
    size_t spike_start;
    size_t spike_count;
    uint64_t spike_sequence;
    uint64_t total_events;
    uint64_t total_migrations;
    uint64_t lost_events;
    uint64_t threshold_ns;
    void *lock;
} MetricsStore;

typedef struct {
    int major;
    int minor;
    int patch;
    int perf_event_paranoid;
    int perf_event_open_ok;
    char tracefs_path[256];
    char kernel_release[128];
    char perf_error[128];
} ProbeResult;

typedef struct JsonWriter JsonWriter;

uint64_t sched_spy_now_ns(void);
void sched_spy_default_config(SchedSpyConfig *cfg);
int sched_spy_config_add_pid(SchedSpyConfig *cfg, pid_t pid);
int sched_spy_config_add_comm(SchedSpyConfig *cfg, const char *comm);
int sched_spy_target_matches(const SchedSpyConfig *cfg, pid_t pid, const char *comm);
int sched_spy_validate_config(const SchedSpyConfig *cfg, char *err, size_t err_len);

#ifdef __cplusplus
}
#endif

#endif
