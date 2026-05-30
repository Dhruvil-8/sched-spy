# sched-spy — Kernel Latency Profiler
> Measure scheduler latency: the invisible time between "ready to run" and "actually running."

---

## Table of Contents

1. [What This Tool Actually Measures](#1-what-this-tool-actually-measures)
2. [How Linux Scheduling Works (What We're Hooking Into)](#2-how-linux-scheduling-works)
3. [Kernel Interfaces Available](#3-kernel-interfaces-available)
4. [Architecture Decision: perf_event_open vs tracefs](#4-architecture-decision)
5. [File Structure](#5-file-structure)
6. [Data Structures](#6-data-structures)
7. [Phase 1 — Capability Detection + Environment Setup](#7-phase-1--capability-detection)
8. [Phase 2 — tracefs Reader (Raw Event Pipe)](#8-phase-2--tracefs-reader)
9. [Phase 3 — perf_event_open Tracepoint Attachment](#9-phase-3--perf_event_open)
10. [Phase 4 — Ring Buffer Reader](#10-phase-4--ring-buffer-reader)
11. [Phase 5 — Event Correlation Engine](#11-phase-5--event-correlation-engine)
12. [Phase 6 — Metrics Aggregation](#12-phase-6--metrics-aggregation)
13. [Phase 7 — Terminal Output (Live View)](#13-phase-7--terminal-output)
14. [Phase 8 — JSON + File Output](#14-phase-8--json--file-output)
15. [Phase 9 — HTTP Metrics Endpoint (http1.c integration)](#15-phase-9--http-metrics-endpoint)
16. [Phase 10 — Multi-CPU Awareness](#16-phase-10--multi-cpu-awareness)
17. [Phase 11 — Kernel Version Portability](#17-phase-11--kernel-version-portability)
18. [Phase 12 — Test Suite](#18-phase-12--test-suite)
19. [CLI Design — Full Specification](#19-cli-design)
20. [Error Handling Strategy](#20-error-handling-strategy)
21. [Security and Privilege Model](#21-security-and-privilege-model)
22. [Known Edge Cases and Pitfalls](#22-known-edge-cases-and-pitfalls)
23. [Build System](#23-build-system)
24. [Milestone Checklist](#24-milestone-checklist)

---

## 1. What This Tool Actually Measures

### The problem

When developers say "my process is slow," they look at CPU usage. But CPU usage only tells you what happened *while your process was running*. It tells you nothing about the time your process sat in the run queue, ready to execute, but waiting for the scheduler to pick it.

This waiting time is **scheduler latency** — and it's invisible to every standard tool.

### The three time regions of a process

```
SLEEPING ──→ RUNNABLE ──→ RUNNING
             ↑             ↑
             sched_wakeup  sched_switch (prev=other, next=our_pid)

             |←── latency ──→|
             This is what sched-spy measures.
```

- **Sleeping**: process is waiting on I/O, a lock, a timer, sleep(). Not using CPU, not waiting for CPU.
- **Runnable**: process is ready. It has work to do. It is sitting in the CPU run queue. The scheduler hasn't picked it yet.
- **Running**: process is on a CPU core executing instructions.

The gap between Runnable and Running is scheduler latency. A 1ms spike here means your process was stalled for 1ms even though it was ready — because another process (or kernel worker) was using the CPU.

### Why this matters

- **Web servers**: p99 latency spikes despite low CPU usage
- **Game loops**: frame time jitter even at 20% CPU
- **Audio/video**: buffer underruns despite "plenty of CPU"
- **Real-time systems**: missed deadlines nobody can explain
- **Database systems**: query latency spikes under load

All of these have the same invisible cause: scheduler latency. sched-spy makes it visible.

### What sched-spy reports

For a target PID, sched-spy reports:
- Every individual latency event: when it happened, how long, what was running instead
- Rolling statistics: min, max, mean, p50, p95, p99
- CPU affinity: which core the process runs on, whether it migrates
- Spike detection: events exceeding a configurable threshold
- Preemption cause: which process caused the delay and its comm name

---

## 2. How Linux Scheduling Works

### The Completely Fair Scheduler (CFS)

Linux uses CFS for normal processes (SCHED_NORMAL). CFS maintains a red-black tree of runnable tasks sorted by "virtual runtime." The task with the least virtual runtime is picked next. This ensures fairness but doesn't guarantee bounded latency.

### Scheduler events we care about

#### `sched_wakeup`
Fired when a sleeping process becomes runnable. This is the starting timestamp — the moment the process entered the run queue.

Fields available in the tracepoint:
```
comm    — process name (up to 16 chars)
pid     — process ID
prio    — scheduling priority
success — whether wakeup succeeded
target_cpu — which CPU run queue it was added to
```

#### `sched_switch`
Fired on every context switch. Contains both the outgoing and incoming process.

Fields:
```
prev_comm  — name of process being switched out
prev_pid   — PID being switched out
prev_prio  — priority of outgoing process
prev_state — why it's leaving CPU (R=running→runnable, S=sleep, D=uninterruptible)
next_comm  — name of process being switched in
next_pid   — PID being switched in
next_prio  — priority of incoming process
```

#### `sched_migrate_task`
Fired when a task is moved from one CPU's run queue to another.

Fields:
```
comm       — process name
pid        — process ID
prio       — priority
orig_cpu   — source CPU
dest_cpu   — destination CPU
```

#### `sched_wakeup_new`
Fired when a newly forked process becomes runnable for the first time. Same fields as `sched_wakeup`.

### Latency calculation

```
When sched_wakeup fires for PID X:
    store: wakeup_map[X] = { timestamp, cpu }

When sched_switch fires with next_pid == X:
    if wakeup_map[X] exists:
        latency = sched_switch.timestamp - wakeup_map[X].timestamp
        preempted_by = sched_switch.prev_comm
        emit SchedEvent { pid=X, latency, preempted_by, cpu }
        delete wakeup_map[X]
```

### Scheduler clock

Kernel tracepoints use `CLOCK_MONOTONIC` nanosecond timestamps. This is the right clock — it doesn't jump on NTP adjustments and doesn't drift.

---

## 3. Kernel Interfaces Available

There are three ways to read scheduler tracepoints from userspace. Each has tradeoffs.

### Interface A: tracefs (debugfs tracing)

**Path:** `/sys/kernel/tracing/` (or `/sys/kernel/debug/tracing/` on older kernels)

**How it works:** Write `1` to enable a tracepoint. Read from the `trace_pipe` file to get a stream of human-readable text events.

**Pros:**
- Works on any kernel ≥ 3.x
- No special headers needed — pure file I/O
- Easiest to implement correctly
- Readable output for debugging

**Cons:**
- Text format — must parse strings
- Higher overhead than binary interfaces
- Single consumer (only one process can read trace_pipe at a time)

**Sample output from trace_pipe:**
```
     sshd-1234  [002] d...  1234.567890: sched_switch: prev_comm=sshd prev_pid=1234 prev_prio=120 prev_state=S ==> next_comm=nginx next_pid=5678 next_prio=120
     nginx-5678 [002] d...  1234.567892: sched_wakeup: comm=nginx pid=5678 prio=120 success=1 target_cpu=2
```

### Interface B: perf_event_open

**How it works:** Use the `perf_event_open()` syscall to attach to a tracepoint by its ID. Data arrives in a mmap'd ring buffer in binary format — the same structure the kernel writes natively.

**Pros:**
- Binary format — no text parsing
- Very low overhead (ring buffer is zero-copy)
- Multiple consumers possible
- Per-CPU ring buffers — scales to many cores
- Enables reading exact tracepoint field layouts

**Cons:**
- More complex setup
- Requires knowing tracepoint format IDs (read from tracefs)
- API is poorly documented
- Behavior varies subtly across kernel versions

### Interface C: eBPF via bpf() syscall

**How it works:** Load a small eBPF program into the kernel that runs on each tracepoint hit. The program can do in-kernel aggregation and pass results to userspace via BPF maps.

**Pros:**
- Zero per-event userspace overhead
- Can do in-kernel histograms, aggregation
- Most powerful option

**Cons:**
- Requires kernel ≥ 4.7 for tracepoint programs
- Complex: requires libbpf or hand-written BPF bytecode
- CO-RE (Compile Once, Run Everywhere) adds significant complexity
- Not truly zero-dependency

### Our approach: Interface B (perf_event_open) with Interface A as fallback

Primary: `perf_event_open` binary ring buffer — correct, efficient, complete.
Fallback: `tracefs` text reader — if `perf_event_open` fails (containers, old kernels, restricted environments).

This gives us correctness and portability without eBPF complexity.

---

## 4. Architecture Decision

### Final architecture

```
┌─────────────────────────────────────────────────────┐
│                    sched-spy                         │
│                                                      │
│  ┌─────────────────────────────────────────────┐    │
│  │           Capability Probe                   │    │
│  │  Can we perf_event_open? → primary path      │    │
│  │  Fallback to tracefs?    → secondary path    │    │
│  └───────────────┬─────────────────────────────┘    │
│                  │                                    │
│  ┌───────────────▼─────────────────────────────┐    │
│  │         Event Source Layer                   │    │
│  │  perf_event_open reader  OR  tracefs reader  │    │
│  │  (per-CPU ring buffers)      (trace_pipe)    │    │
│  └───────────────┬─────────────────────────────┘    │
│                  │  raw SchedRawEvent structs         │
│  ┌───────────────▼─────────────────────────────┐    │
│  │        Event Correlation Engine              │    │
│  │  wakeup_map[pid] = timestamp                 │    │
│  │  on switch: compute latency, emit event      │    │
│  └───────────────┬─────────────────────────────┘    │
│                  │  SchedEvent structs                │
│  ┌───────────────▼─────────────────────────────┐    │
│  │          Metrics Aggregator                  │    │
│  │  per-PID stats: min/max/mean/p99/histogram   │    │
│  │  spike ring buffer (last 1000 spikes)        │    │
│  └───────────────┬─────────────────────────────┘    │
│                  │                                    │
│  ┌───────────────▼─────────────────────────────┐    │
│  │            Output Layer                      │    │
│  │  terminal (live)  JSON log  HTTP /metrics    │    │
│  └─────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────┘
```

### Threading model

- **Main thread**: terminal output, signal handling, HTTP server
- **Reader thread(s)**: one per CPU, reading perf ring buffers
- **Correlator thread**: consumes raw events from a lock-free SPSC queue per reader thread, does wakeup→switch correlation
- **Aggregator**: called from correlator thread, updates stats under a spinlock

Why one reader thread per CPU: perf ring buffers are per-CPU. A single thread reading all CPUs would serialize reads and introduce latency.

---

## 5. File Structure

```
sched-spy/
├── sched_spy.h              ← public API (for embedding)
├── sched_spy.c              ← main implementation
├── src/
│   ├── perf_reader.c        ← perf_event_open ring buffer reader
│   ├── perf_reader.h
│   ├── tracefs_reader.c     ← tracefs text-based fallback reader
│   ├── tracefs_reader.h
│   ├── correlator.c         ← wakeup→switch event correlation
│   ├── correlator.h
│   ├── metrics.c            ← statistics aggregation + histogram
│   ├── metrics.h
│   ├── terminal.c           ← live terminal output
│   ├── terminal.h
│   ├── output_json.c        ← JSON log writer
│   ├── output_json.h
│   ├── http_metrics.c       ← /metrics HTTP endpoint
│   ├── http_metrics.h
│   ├── ring_queue.c         ← lock-free SPSC ring queue
│   ├── ring_queue.h
│   └── probe.c              ← capability detection
├── test/
│   ├── test_correlator.c
│   ├── test_metrics.c
│   ├── test_ring_queue.c
│   ├── test_tracefs_parser.c
│   └── run_tests.sh
├── tools/
│   └── dump_tracepoint_format.sh   ← diagnostic: print kernel field layout
├── Makefile
└── README.md
```

---

## 6. Data Structures

### Raw event — what comes out of the kernel

```c
// The type of raw event received from the kernel
typedef enum {
    RAW_SCHED_WAKEUP,
    RAW_SCHED_SWITCH,
    RAW_SCHED_MIGRATE,
    RAW_SCHED_WAKEUP_NEW
} RawEventType;

// Normalized representation — same regardless of source (perf or tracefs)
typedef struct {
    RawEventType type;
    uint64_t     timestamp_ns;   // CLOCK_MONOTONIC nanoseconds
    int          cpu;            // which CPU this event occurred on

    // sched_wakeup / sched_wakeup_new fields
    char         comm[16];       // process name
    pid_t        pid;
    int          prio;
    int          target_cpu;

    // sched_switch fields
    char         prev_comm[16];
    pid_t        prev_pid;
    int          prev_prio;
    long         prev_state;     // TASK_RUNNING=0, TASK_INTERRUPTIBLE=1, etc.
    char         next_comm[16];
    pid_t        next_pid;
    int          next_prio;

    // sched_migrate fields
    int          orig_cpu;
    int          dest_cpu;
} RawEvent;
```

### Processed event — after correlation

```c
// A fully resolved latency measurement
typedef struct {
    pid_t    pid;
    char     comm[16];          // name of the process that was waiting
    uint64_t wakeup_ns;         // when it entered the run queue
    uint64_t run_ns;            // when the scheduler finally picked it
    uint64_t latency_ns;        // run_ns - wakeup_ns — the money number
    char     preempted_by[16];  // what was running on that CPU instead
    pid_t    preempted_by_pid;
    int      cpu;               // which CPU this happened on
    int      migrated;          // 1 if task migrated between wakeup and run
} SchedEvent;
```

### Wakeup tracking — the pending state

```c
#define WAKEUP_MAP_SIZE 4096    // power of two, open addressing hashmap

typedef struct {
    pid_t    pid;           // 0 = empty slot
    uint64_t timestamp_ns;
    int      cpu;
    char     comm[16];
    int      occupied;
} WakeupEntry;

typedef struct {
    WakeupEntry entries[WAKEUP_MAP_SIZE];
    uint64_t    hit_count;
    uint64_t    miss_count;   // switch arrived but no matching wakeup
    uint64_t    eviction_count; // wakeup replaced without matching switch
} WakeupMap;
```

### Metrics per PID

```c
#define HISTOGRAM_BUCKETS 64   // log2 scale from 1us to ~10 seconds

typedef struct {
    pid_t    pid;
    char     comm[16];

    // Running statistics (Welford's online algorithm — numerically stable)
    uint64_t count;
    double   mean;
    double   M2;             // for variance calculation
    uint64_t min_ns;
    uint64_t max_ns;

    // Percentile histogram — log2 bucket i covers [2^i ns, 2^(i+1) ns)
    uint64_t histogram[HISTOGRAM_BUCKETS];

    // Recent spikes (ring buffer of last 256 events > threshold)
    SchedEvent spikes[256];
    int        spike_head;
    int        spike_count;

    // CPU affinity tracking
    int        cpu_counts[256]; // how many times seen on each CPU
    int        migration_count;
} PidMetrics;
```

### Configuration

```c
typedef struct {
    pid_t    target_pids[64];     // PIDs to watch (0 = watch all)
    int      target_pid_count;
    char     target_comms[16][16]; // process names to watch
    int      target_comm_count;
    uint64_t spike_threshold_ns;  // default: 1ms
    int      use_perf;            // 1 = perf_event_open, 0 = tracefs
    int      output_terminal;
    int      output_json;
    char     json_path[256];
    int      output_http;
    int      http_port;
    int      verbose;
    int      show_all_cpus;       // show per-CPU breakdown
    int      refresh_rate_ms;     // terminal refresh interval
    int      keep_alive_sec;      // how long to keep wakeup entries
} Config;
```

### Lock-free SPSC ring queue (reader → correlator)

```c
// Single-producer single-consumer queue — no locks needed
// Each reader thread produces, correlator thread consumes

#define RING_QUEUE_SIZE 65536  // must be power of two

typedef struct {
    RawEvent   buf[RING_QUEUE_SIZE];
    _Atomic uint64_t head;   // written by producer
    _Atomic uint64_t tail;   // written by consumer
    char _pad[64];           // prevent false sharing
} RingQueue;
```

---

## 7. Phase 1 — Capability Detection

Before doing anything else, sched-spy must understand the environment it's running in. A clear error message is better than a cryptic failure 50 lines later.

### probe.c — capability detection

```c
typedef struct {
    int has_tracefs;           // /sys/kernel/tracing exists and is readable
    int has_debugfs_tracing;   // /sys/kernel/debug/tracing fallback
    int has_perf_event_open;   // perf_event_open syscall works
    int perf_paranoia;         // value of /proc/sys/kernel/perf_event_paranoid
    int is_root;               // running as root (uid == 0)
    int has_cap_sys_admin;     // CAP_SYS_ADMIN capability
    int has_cap_perfmon;       // CAP_PERFMON (kernel >= 5.8)
    int tracefs_mount;         // which path tracefs is at
    char tracefs_path[128];    // resolved path
    int kernel_major;
    int kernel_minor;
} ProbeResult;

ProbeResult probe_environment(void);
```

### Implementation

```c
ProbeResult probe_environment(void) {
    ProbeResult r = {0};

    // Kernel version
    struct utsname u;
    uname(&u);
    sscanf(u.release, "%d.%d", &r.kernel_major, &r.kernel_minor);

    // UID / capabilities
    r.is_root = (getuid() == 0);

    // Check CAP_SYS_ADMIN
    cap_t caps = cap_get_proc();
    if (caps) {
        cap_flag_value_t val;
        cap_get_flag(caps, CAP_SYS_ADMIN, CAP_EFFECTIVE, &val);
        r.has_cap_sys_admin = (val == CAP_SET);
        cap_free(caps);
    }

    // perf_event_paranoid
    FILE *f = fopen("/proc/sys/kernel/perf_event_paranoid", "r");
    if (f) {
        fscanf(f, "%d", &r.perf_paranoia);
        fclose(f);
    }

    // tracefs path
    if (access("/sys/kernel/tracing/events/sched", R_OK) == 0) {
        r.has_tracefs = 1;
        strncpy(r.tracefs_path, "/sys/kernel/tracing", sizeof(r.tracefs_path)-1);
    } else if (access("/sys/kernel/debug/tracing/events/sched", R_OK) == 0) {
        r.has_debugfs_tracing = 1;
        strncpy(r.tracefs_path, "/sys/kernel/debug/tracing",
                sizeof(r.tracefs_path)-1);
    }

    // Test if perf_event_open will actually work (do a dry run)
    r.has_perf_event_open = test_perf_event_open();

    return r;
}
```

### Dry-run perf test

```c
static int test_perf_event_open(void) {
    // Try to open a minimal perf event just to check if the syscall works
    struct perf_event_attr attr = {
        .type   = PERF_TYPE_SOFTWARE,
        .config = PERF_COUNT_SW_DUMMY,
        .size   = sizeof(attr),
    };
    int fd = syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}
```

### Environment report (shown with --verbose or on error)

```
sched-spy environment check:
  Kernel:        6.1.0
  Running as:    user (uid=1000)
  CAP_SYS_ADMIN: NO
  CAP_PERFMON:   NO
  perf_paranoid: 2  ← [WARNING: restricts perf_event_open to root]
  tracefs:       /sys/kernel/tracing ✓
  perf_event:    FAILED (paranoid=2 requires root or CAP_PERFMON)

→ Using fallback: tracefs reader
→ To use perf_event_open: sudo sched-spy, or:
     echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid
```

### Decision logic

```c
int choose_reader(ProbeResult *p, Config *cfg) {
    if (p->has_perf_event_open && !cfg->force_tracefs) {
        return READER_PERF;
    }
    if (p->has_tracefs || p->has_debugfs_tracing) {
        return READER_TRACEFS;
    }
    fprintf(stderr,
        "sched-spy: no kernel interface available.\n"
        "  Need either:\n"
        "    - tracefs at /sys/kernel/tracing (kernel >= 3.x)\n"
        "    - perf_event_open (run as root or set perf_event_paranoid=1)\n");
    return READER_NONE;
}
```

---

## 8. Phase 2 — tracefs Reader

The fallback reader. Text-based, works everywhere, slightly higher overhead.

### Enabling tracepoints

```c
static int tracefs_enable_event(const char *tracefs_path,
                                 const char *subsystem,
                                 const char *event) {
    char path[512];
    snprintf(path, sizeof(path), "%s/events/%s/%s/enable",
             tracefs_path, subsystem, event);

    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "Cannot enable %s/%s: %s\n",
                subsystem, event, strerror(errno));
        return -1;
    }
    write(fd, "1", 1);
    close(fd);
    return 0;
}

int tracefs_setup(const char *tracefs_path) {
    // First: clear any existing trace buffer
    char clear_path[512];
    snprintf(clear_path, sizeof(clear_path), "%s/trace", tracefs_path);
    int fd = open(clear_path, O_WRONLY | O_TRUNC);
    if (fd >= 0) close(fd);

    // Enable the events we need
    tracefs_enable_event(tracefs_path, "sched", "sched_wakeup");
    tracefs_enable_event(tracefs_path, "sched", "sched_wakeup_new");
    tracefs_enable_event(tracefs_path, "sched", "sched_switch");
    tracefs_enable_event(tracefs_path, "sched", "sched_migrate_task");

    // Set buffer size (per CPU, in KB)
    // Larger buffer = fewer lost events under load
    char buf_path[512];
    snprintf(buf_path, sizeof(buf_path), "%s/buffer_size_kb", tracefs_path);
    fd = open(buf_path, O_WRONLY);
    if (fd >= 0) {
        write(fd, "4096", 4);
        close(fd);
    }

    return 0;
}
```

### Disabling tracepoints on exit (critical — always clean up)

```c
void tracefs_teardown(const char *tracefs_path) {
    // Write 0 to disable each event
    tracefs_enable_event_value(tracefs_path, "sched", "sched_wakeup", "0");
    tracefs_enable_event_value(tracefs_path, "sched", "sched_wakeup_new", "0");
    tracefs_enable_event_value(tracefs_path, "sched", "sched_switch", "0");
    tracefs_enable_event_value(tracefs_path, "sched", "sched_migrate_task", "0");
}
// Register this in atexit() AND signal handlers (SIGINT, SIGTERM, SIGQUIT)
// A crash that leaves tracepoints enabled will profile everything until reboot.
```

### trace_pipe reader thread

```c
// trace_pipe is a blocking read — blocks until events available
// Run this in a dedicated thread

void *tracefs_reader_thread(void *arg) {
    TraceFSReader *r = (TraceFSReader *)arg;

    char pipe_path[512];
    snprintf(pipe_path, sizeof(pipe_path), "%s/trace_pipe", r->tracefs_path);

    int fd = open(pipe_path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("open trace_pipe");
        return NULL;
    }

    char line[1024];
    char partial[1024];
    int  partial_len = 0;

    while (r->running) {
        // Use poll to wait for data with a timeout (allows clean shutdown)
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ret = poll(&pfd, 1, 200); // 200ms timeout

        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) continue; // timeout, check running flag

        ssize_t n = read(fd, line, sizeof(line) - 1);
        if (n <= 0) continue;

        line[n] = '\0';
        parse_trace_lines(r, line, n);
    }

    close(fd);
    return NULL;
}
```

### tracefs line parser

```c
// Format:
//   comm-pid  [cpu] flags  timestamp: event_name: field=val field=val ...
//
// Example:
//   nginx-1234 [002] d... 1234567.890123: sched_switch: prev_comm=nginx \
//   prev_pid=1234 prev_prio=120 prev_state=S ==> next_comm=sshd \
//   next_pid=5678 next_prio=120

static int parse_trace_line(const char *line, RawEvent *ev) {
    memset(ev, 0, sizeof(*ev));

    // Skip leading whitespace
    while (*line == ' ') line++;

    // Find the comm-pid section
    const char *dash = strrchr(line, '-');
    // (Use last dash before first [ to get pid)

    // Find CPU: [NNN]
    const char *cpu_start = strchr(line, '[');
    if (!cpu_start) return -1;
    ev->cpu = atoi(cpu_start + 1);

    // Find timestamp: after the flags section "d..."
    const char *ts_start = strchr(cpu_start, ' ');
    while (ts_start && *ts_start == ' ') ts_start++;
    // Skip flags token
    ts_start = strchr(ts_start, ' ');
    while (ts_start && *ts_start == ' ') ts_start++;

    // Parse timestamp as double, convert to ns
    double ts_sec;
    if (sscanf(ts_start, "%lf:", &ts_sec) != 1) return -1;
    ev->timestamp_ns = (uint64_t)(ts_sec * 1e9);

    // Find event name
    const char *colon = strchr(ts_start, ':');
    if (!colon) return -1;
    colon++; // skip ':'
    while (*colon == ' ') colon++;

    // Identify event type
    if (strncmp(colon, "sched_switch:", 13) == 0) {
        ev->type = RAW_SCHED_SWITCH;
        return parse_sched_switch_fields(colon + 13, ev);
    } else if (strncmp(colon, "sched_wakeup_new:", 17) == 0) {
        ev->type = RAW_SCHED_WAKEUP_NEW;
        return parse_sched_wakeup_fields(colon + 17, ev);
    } else if (strncmp(colon, "sched_wakeup:", 13) == 0) {
        ev->type = RAW_SCHED_WAKEUP;
        return parse_sched_wakeup_fields(colon + 13, ev);
    } else if (strncmp(colon, "sched_migrate_task:", 19) == 0) {
        ev->type = RAW_SCHED_MIGRATE;
        return parse_sched_migrate_fields(colon + 19, ev);
    }
    return -1; // unknown event, skip
}
```

### Field parsers

```c
// Parse: "prev_comm=nginx prev_pid=1234 prev_prio=120 prev_state=S ==> next_comm=sshd next_pid=5678 next_prio=120"
static int parse_sched_switch_fields(const char *s, RawEvent *ev) {
    char prev_comm[16], next_comm[16];
    int  prev_pid, next_pid, prev_prio, next_prio;
    char prev_state_str[4];

    // sscanf with custom format handles this well
    int n = sscanf(s,
        " prev_comm=%15s prev_pid=%d prev_prio=%d prev_state=%3s"
        " ==> next_comm=%15s next_pid=%d next_prio=%d",
        prev_comm, &prev_pid, &prev_prio, prev_state_str,
        next_comm, &next_pid, &next_prio);

    if (n < 7) return -1;

    strncpy(ev->prev_comm, prev_comm, 15);
    strncpy(ev->next_comm, next_comm, 15);
    ev->prev_pid  = prev_pid;
    ev->next_pid  = next_pid;
    ev->prev_prio = prev_prio;
    ev->next_prio = next_prio;
    ev->prev_state = parse_task_state(prev_state_str);
    return 0;
}

// Parse: "comm=nginx pid=1234 prio=120 success=1 target_cpu=2"
static int parse_sched_wakeup_fields(const char *s, RawEvent *ev) {
    char comm[16];
    int pid, prio, success, target_cpu;

    int n = sscanf(s, " comm=%15s pid=%d prio=%d success=%d target_cpu=%d",
                   comm, &pid, &prio, &success, &target_cpu);
    if (n < 5) return -1;

    strncpy(ev->comm, comm, 15);
    ev->pid        = pid;
    ev->prio       = prio;
    ev->target_cpu = target_cpu;
    return 0;
}
```

---

## 9. Phase 3 — perf_event_open Tracepoint Attachment

The primary, high-performance path.

### How to get tracepoint IDs

Each tracepoint has a numeric ID assigned by the kernel. Read it from tracefs:

```c
static int get_tracepoint_id(const char *tracefs_path,
                              const char *subsystem,
                              const char *event) {
    char path[512];
    snprintf(path, sizeof(path), "%s/events/%s/%s/id",
             tracefs_path, subsystem, event);

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    int id;
    fscanf(f, "%d", &id);
    fclose(f);
    return id;
}

// Usage:
int sched_switch_id    = get_tracepoint_id(tracefs_path, "sched", "sched_switch");
int sched_wakeup_id    = get_tracepoint_id(tracefs_path, "sched", "sched_wakeup");
int sched_wakeup_new_id= get_tracepoint_id(tracefs_path, "sched", "sched_wakeup_new");
int sched_migrate_id   = get_tracepoint_id(tracefs_path, "sched", "sched_migrate_task");
```

### perf_event_attr setup

```c
static int open_tracepoint(int tp_id, int cpu, int group_fd) {
    struct perf_event_attr attr = {0};

    attr.type           = PERF_TYPE_TRACEPOINT;
    attr.size           = sizeof(attr);
    attr.config         = tp_id;        // tracepoint ID from tracefs
    attr.sample_type    = PERF_SAMPLE_RAW       // raw tracepoint data
                        | PERF_SAMPLE_TIME      // kernel timestamp
                        | PERF_SAMPLE_CPU       // which CPU
                        | PERF_SAMPLE_TID;      // PID + TID
    attr.sample_period  = 1;            // record every event (not sampling)
    attr.wakeup_events  = 1;            // wake up userspace every N events
    attr.disabled       = 1;            // start disabled; enable after mmap
    attr.exclude_kernel = 0;            // we WANT kernel events
    attr.exclude_hv     = 1;            // skip hypervisor events

    // -1 pid = all processes (requires CAP_SYS_ADMIN or paranoid <= 0)
    // specific pid = only that process (works with paranoid <= 2)
    pid_t pid = (cfg.target_pid_count == 1) ? cfg.target_pids[0] : -1;

    int fd = syscall(SYS_perf_event_open, &attr, pid, cpu, group_fd,
                     PERF_FLAG_FD_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "perf_event_open(cpu=%d, tp=%d): %s\n",
                cpu, tp_id, strerror(errno));
        return -1;
    }
    return fd;
}
```

### Opening per-CPU ring buffers

```c
#define MMAP_PAGES 128    // ring buffer size = 128 * PAGE_SIZE = 512KB per CPU

typedef struct {
    int      fd;
    void    *mmap_base;      // mmap'd ring buffer
    size_t   mmap_size;
    uint64_t prev_head;      // last read position
} PerfFd;

static int setup_perf_cpu(int cpu, int *tp_ids, int tp_count,
                           PerfFd *fds_out) {
    int group_fd = -1;

    for (int i = 0; i < tp_count; i++) {
        int fd = open_tracepoint(tp_ids[i], cpu, group_fd);
        if (fd < 0) return -1;

        if (i == 0) group_fd = fd; // first fd is the group leader

        fds_out[i].fd = fd;

        // mmap the ring buffer only on the group leader
        if (i == 0) {
            size_t mmap_size = (MMAP_PAGES + 1) * getpagesize();
            void *base = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
                              MAP_SHARED, fd, 0);
            if (base == MAP_FAILED) {
                perror("mmap perf ring buffer");
                return -1;
            }
            fds_out[i].mmap_base = base;
            fds_out[i].mmap_size = mmap_size;
            fds_out[i].prev_head = 0;
        }
    }

    // Enable all fds (ioctl on group leader enables the whole group)
    ioctl(group_fd, PERF_EVENT_IOC_RESET,  PERF_IOC_FLAG_GROUP);
    ioctl(group_fd, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);

    return 0;
}
```

---

## 10. Phase 4 — Ring Buffer Reader

### The perf ring buffer structure

```
mmap layout:
┌───────────────────────────────────┐  ← mmap_base
│   struct perf_event_mmap_page     │  one page (metadata)
│     .data_head  = write position  │
│     .data_tail  = read position   │
├───────────────────────────────────┤  ← mmap_base + PAGE_SIZE
│   ring data (MMAP_PAGES pages)    │  circular, kernel writes here
│   [ record | record | record... ] │
└───────────────────────────────────┘
```

The kernel writes records starting at `data_head` and wraps around. Userspace reads from `data_tail` to `data_head` and updates `data_tail` after reading.

### Reading records

```c
void perf_read_cpu(PerfFd *fd, RingQueue *queue) {
    struct perf_event_mmap_page *meta =
        (struct perf_event_mmap_page *)fd->mmap_base;

    char *data_base = (char *)fd->mmap_base + getpagesize();
    uint64_t data_size = (uint64_t)MMAP_PAGES * getpagesize();
    uint64_t data_mask = data_size - 1;

    // Memory barrier: ensure we read data_head after it's been written
    uint64_t head = __atomic_load_n(&meta->data_head, __ATOMIC_ACQUIRE);
    uint64_t tail = fd->prev_head;

    while (tail != head) {
        // Read record header (type, misc, size)
        struct perf_event_header hdr;
        uint64_t offset = tail & data_mask;

        // Handle wrap-around: record may straddle the ring buffer boundary
        if (offset + sizeof(hdr) > data_size) {
            // Copy in two parts
            size_t first  = data_size - offset;
            size_t second = sizeof(hdr) - first;
            memcpy((char *)&hdr,        data_base + offset, first);
            memcpy((char *)&hdr + first, data_base,          second);
        } else {
            memcpy(&hdr, data_base + offset, sizeof(hdr));
        }

        if (hdr.size == 0) break; // safety

        if (hdr.type == PERF_RECORD_SAMPLE) {
            // Read full record
            char record[4096];
            ring_copy(data_base, data_size, tail, record, hdr.size);
            RawEvent ev;
            if (parse_perf_sample(record, hdr.size, &ev) == 0) {
                ring_queue_push(queue, &ev);
            }
        } else if (hdr.type == PERF_RECORD_LOST) {
            // Kernel dropped events — ring buffer too small or consumer too slow
            uint64_t lost = *(uint64_t *)(data_base + ((tail + 16) & data_mask));
            fprintf(stderr, "WARNING: lost %llu events on CPU %d\n",
                    (unsigned long long)lost, fd->cpu);
        }

        tail += hdr.size;
    }

    // Update tail — tell kernel we've consumed up to here
    __atomic_store_n(&meta->data_tail, tail, __ATOMIC_RELEASE);
    fd->prev_head = tail;
}
```

### Parsing the perf sample binary format

The raw sample layout (for our `sample_type` flags):

```
[u64 pid, tid]          PERF_SAMPLE_TID
[u64 timestamp_ns]      PERF_SAMPLE_TIME
[u32 cpu, u32 reserved] PERF_SAMPLE_CPU
[u32 raw_size]          PERF_SAMPLE_RAW
[u8  raw_data[raw_size]]  tracepoint-specific binary payload
```

The `raw_data` layout is defined per-tracepoint in tracefs:

```bash
cat /sys/kernel/tracing/events/sched/sched_switch/format
```

```
name: sched_switch
ID: 315
format:
    field:unsigned short common_type;    offset:0; size:2; signed:0;
    field:unsigned char common_flags;    offset:2; size:1; signed:0;
    field:unsigned char common_preempt_count; offset:3; size:1; signed:0;
    field:int common_pid;                offset:4; size:4; signed:1;

    field:char prev_comm[16];            offset:8;  size:16; signed:1;
    field:pid_t prev_pid;               offset:24; size:4;  signed:1;
    field:int prev_prio;                 offset:28; size:4;  signed:1;
    field:long prev_state;              offset:32; size:8;  signed:1;
    field:char next_comm[16];           offset:40; size:16; signed:1;
    field:pid_t next_pid;               offset:56; size:4;  signed:1;
    field:int next_prio;                offset:60; size:4;  signed:1;
```

### Parsing the format file at runtime

Instead of hardcoding offsets (which differ across kernels), parse them at startup:

```c
typedef struct {
    char     name[64];
    int      offset;
    int      size;
    int      is_signed;
} FieldDesc;

typedef struct {
    FieldDesc fields[32];
    int       field_count;
} TraceFormat;

TraceFormat parse_tracepoint_format(const char *tracefs_path,
                                     const char *subsystem,
                                     const char *event) {
    char path[512];
    snprintf(path, sizeof(path), "%s/events/%s/%s/format",
             tracefs_path, subsystem, event);

    TraceFormat fmt = {0};
    FILE *f = fopen(path, "r");
    if (!f) return fmt;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        FieldDesc fd = {0};
        // Parse: "    field:TYPE name; offset:N; size:N; signed:N;"
        if (sscanf(line, " field:%*s %63[^;]; offset:%d; size:%d; signed:%d",
                   fd.name, &fd.offset, &fd.size, &fd.is_signed) == 4) {
            // Skip common_ fields
            if (strncmp(fd.name, "common_", 7) != 0) {
                fmt.fields[fmt.field_count++] = fd;
            }
        }
    }
    fclose(f);
    return fmt;
}
```

### Extracting field values using parsed offsets

```c
// Given a raw tracepoint payload and a parsed format, extract a field
static int64_t get_field_int(const char *raw, const TraceFormat *fmt,
                              const char *field_name) {
    for (int i = 0; i < fmt->field_count; i++) {
        if (strcmp(fmt->fields[i].name, field_name) == 0) {
            const char *ptr = raw + fmt->fields[i].offset;
            switch (fmt->fields[i].size) {
                case 1: return fmt->fields[i].is_signed
                            ? *(int8_t  *)ptr : *(uint8_t  *)ptr;
                case 2: return fmt->fields[i].is_signed
                            ? *(int16_t *)ptr : *(uint16_t *)ptr;
                case 4: return fmt->fields[i].is_signed
                            ? *(int32_t *)ptr : *(uint32_t *)ptr;
                case 8: return fmt->fields[i].is_signed
                            ? *(int64_t *)ptr : *(int64_t  *)ptr;
            }
        }
    }
    return 0;
}

static void get_field_str(const char *raw, const TraceFormat *fmt,
                           const char *field_name, char *out, size_t out_len) {
    for (int i = 0; i < fmt->field_count; i++) {
        if (strcmp(fmt->fields[i].name, field_name) == 0) {
            strncpy(out, raw + fmt->fields[i].offset, out_len - 1);
            return;
        }
    }
    out[0] = '\0';
}
```

This approach makes the binary parser kernel-version-independent. Offsets come from the running kernel at startup, not from hardcoded constants.

---

## 11. Phase 5 — Event Correlation Engine

This is the logical core of sched-spy. It receives raw events and produces latency measurements.

### The wakeup hashmap

```c
// Open-addressing hashmap with linear probing
// Key: pid_t (32-bit)
// Capacity: power of two (for fast modulo via &)

static uint32_t wakeup_hash(pid_t pid) {
    // FNV-1a variant for 32-bit ints
    uint32_t h = 2166136261u;
    h ^= (uint32_t)pid;
    h *= 16777619u;
    return h;
}

static WakeupEntry *wakeup_find(WakeupMap *m, pid_t pid) {
    uint32_t idx = wakeup_hash(pid) & (WAKEUP_MAP_SIZE - 1);
    for (int i = 0; i < WAKEUP_MAP_SIZE; i++) {
        WakeupEntry *e = &m->entries[(idx + i) & (WAKEUP_MAP_SIZE - 1)];
        if (!e->occupied) return NULL;    // empty slot = not found
        if (e->pid == pid) return e;
    }
    return NULL;
}

static void wakeup_insert(WakeupMap *m, pid_t pid, uint64_t ts_ns,
                           int cpu, const char *comm) {
    uint32_t idx = wakeup_hash(pid) & (WAKEUP_MAP_SIZE - 1);
    for (int i = 0; i < WAKEUP_MAP_SIZE; i++) {
        WakeupEntry *e = &m->entries[(idx + i) & (WAKEUP_MAP_SIZE - 1)];
        if (!e->occupied || e->pid == pid) {
            e->pid        = pid;
            e->timestamp_ns = ts_ns;
            e->cpu        = cpu;
            e->occupied   = 1;
            strncpy(e->comm, comm, 15);
            return;
        }
    }
    // Map full — should not happen with WAKEUP_MAP_SIZE=4096
    m->eviction_count++;
}

static void wakeup_remove(WakeupMap *m, pid_t pid) {
    uint32_t idx = wakeup_hash(pid) & (WAKEUP_MAP_SIZE - 1);
    for (int i = 0; i < WAKEUP_MAP_SIZE; i++) {
        WakeupEntry *e = &m->entries[(idx + i) & (WAKEUP_MAP_SIZE - 1)];
        if (!e->occupied) return;
        if (e->pid == pid) {
            // Tombstone + rehash to maintain probe chains
            e->occupied = 0;
            return;
        }
    }
}
```

### The correlator — main processing function

```c
// Called for each raw event from the ring queue
void correlator_process(Correlator *c, const RawEvent *ev) {
    switch (ev->type) {
    case RAW_SCHED_WAKEUP:
    case RAW_SCHED_WAKEUP_NEW:
        // Process is entering the run queue
        // Insert into wakeup map with timestamp
        // If already in map (woken up twice without running): update timestamp
        if (should_track(c->cfg, ev->pid, ev->comm)) {
            wakeup_insert(&c->wakeup_map, ev->pid,
                          ev->timestamp_ns, ev->target_cpu, ev->comm);
        }
        break;

    case RAW_SCHED_SWITCH:
        // A process is being switched IN: next_pid
        // Look for a pending wakeup for next_pid
        if (should_track(c->cfg, ev->next_pid, ev->next_comm)) {
            WakeupEntry *w = wakeup_find(&c->wakeup_map, ev->next_pid);
            if (w) {
                // Found the matching wakeup — compute latency
                SchedEvent se = {
                    .pid              = ev->next_pid,
                    .wakeup_ns        = w->timestamp_ns,
                    .run_ns           = ev->timestamp_ns,
                    .latency_ns       = ev->timestamp_ns - w->timestamp_ns,
                    .cpu              = ev->cpu,
                    .preempted_by_pid = ev->prev_pid,
                };
                strncpy(se.comm,           ev->next_comm,  15);
                strncpy(se.preempted_by,   ev->prev_comm,  15);
                se.migrated = (w->cpu != ev->cpu);

                wakeup_remove(&c->wakeup_map, ev->next_pid);

                // Emit to metrics aggregator
                metrics_record(c->metrics, &se);

                // Emit to output layer
                if (se.latency_ns >= c->cfg->spike_threshold_ns) {
                    output_spike(c->output, &se);
                }
            }
            // If no wakeup found: process was already runnable when we started
            // tracking, or wakeup event was lost. Count as miss, move on.
        }

        // ALSO: if prev_pid had a wakeup pending and prev_state == TASK_RUNNING,
        // the prev process is still runnable (preempted, not sleeping).
        // Do NOT remove it from wakeup_map — it will run again soon.
        // Only remove if prev_state indicates sleeping (TASK_INTERRUPTIBLE etc.)
        if (ev->prev_state != 0) {
            // Task is going to sleep — if it had a pending wakeup entry,
            // it's stale now, remove it
            wakeup_remove(&c->wakeup_map, ev->prev_pid);
        }
        break;

    case RAW_SCHED_MIGRATE:
        // Update the cpu in the wakeup entry if it exists
        {
            WakeupEntry *w = wakeup_find(&c->wakeup_map, ev->pid);
            if (w) {
                w->cpu = ev->dest_cpu;
            }
        }
        break;
    }
}
```

### Stale wakeup cleanup

If a process is killed or exits while in the run queue, its wakeup entry will sit in the map forever. Clean up periodically:

```c
void correlator_cleanup_stale(Correlator *c, uint64_t now_ns) {
    uint64_t max_age_ns = 5ULL * 1000 * 1000 * 1000; // 5 seconds

    for (int i = 0; i < WAKEUP_MAP_SIZE; i++) {
        WakeupEntry *e = &c->wakeup_map.entries[i];
        if (!e->occupied) continue;
        if (now_ns - e->timestamp_ns > max_age_ns) {
            e->occupied = 0;
            c->wakeup_map.eviction_count++;
        }
    }
}
// Call this every ~1 second from the correlator thread
```

### Filtering

```c
static int should_track(const Config *cfg, pid_t pid, const char *comm) {
    // If no filter set: track all
    if (cfg->target_pid_count == 0 && cfg->target_comm_count == 0) return 1;

    // Check PID filter
    for (int i = 0; i < cfg->target_pid_count; i++) {
        if (cfg->target_pids[i] == pid) return 1;
    }

    // Check comm name filter
    for (int i = 0; i < cfg->target_comm_count; i++) {
        if (strncmp(cfg->target_comms[i], comm, 15) == 0) return 1;
    }

    return 0;
}
```

---

## 12. Phase 6 — Metrics Aggregation

### Welford's online algorithm for mean and variance

Never accumulate a sum of values — floating point precision degrades. Use Welford's algorithm which is numerically stable regardless of value magnitude.

```c
// Update running mean and variance with a new value
static void welford_update(PidMetrics *m, double x) {
    m->count++;
    double delta  = x - m->mean;
    m->mean      += delta / m->count;
    double delta2 = x - m->mean;
    m->M2        += delta * delta2;
}

static double welford_variance(const PidMetrics *m) {
    if (m->count < 2) return 0.0;
    return m->M2 / (m->count - 1);
}

static double welford_stddev(const PidMetrics *m) {
    return sqrt(welford_variance(m));
}
```

### Histogram — log2 bucket approach

Stores latency distribution in 64 buckets. Bucket i covers `[2^i ns, 2^(i+1) ns)`. Bucket 0 = sub-nanosecond (impossible in practice), bucket 10 ≈ 1µs, bucket 20 ≈ 1ms, bucket 30 ≈ 1s.

```c
static int latency_bucket(uint64_t ns) {
    if (ns == 0) return 0;
    // __builtin_clzll: count leading zeros — gives floor(log2(ns))
    int bucket = 63 - __builtin_clzll(ns);
    if (bucket >= HISTOGRAM_BUCKETS) bucket = HISTOGRAM_BUCKETS - 1;
    return bucket;
}

static void histogram_add(PidMetrics *m, uint64_t latency_ns) {
    m->histogram[latency_bucket(latency_ns)]++;
}
```

### Approximate percentile from histogram

```c
static uint64_t histogram_percentile(const PidMetrics *m, double pct) {
    // pct: 0.0 to 1.0 (e.g., 0.99 for p99)
    uint64_t target = (uint64_t)(m->count * pct);
    uint64_t cumulative = 0;

    for (int i = 0; i < HISTOGRAM_BUCKETS; i++) {
        cumulative += m->histogram[i];
        if (cumulative >= target) {
            // Return midpoint of this bucket
            return (uint64_t)1 << i;
        }
    }
    return m->max_ns;
}
```

### Main record function

```c
void metrics_record(MetricsStore *store, const SchedEvent *ev) {
    // Find or create PidMetrics for this pid
    PidMetrics *m = metrics_get_or_create(store, ev->pid, ev->comm);
    if (!m) return;

    double lat_us = ev->latency_ns / 1000.0;
    welford_update(m, lat_us);
    histogram_add(m, ev->latency_ns);

    if (ev->latency_ns < m->min_ns || m->count == 1) m->min_ns = ev->latency_ns;
    if (ev->latency_ns > m->max_ns) m->max_ns = ev->latency_ns;

    // CPU tracking
    if (ev->cpu < 256) m->cpu_counts[ev->cpu]++;
    if (ev->migrated)  m->migration_count++;

    // Spike ring buffer
    if (ev->latency_ns >= store->cfg->spike_threshold_ns) {
        m->spikes[m->spike_head % 256] = *ev;
        m->spike_head++;
        if (m->spike_count < 256) m->spike_count++;
    }
}
```

---

## 13. Phase 7 — Terminal Output (Live View)

### Design

Refresh every 500ms. Use ANSI escape codes — no ncurses dependency.

```
╔══════════════════════════════════════════════════════════════════════╗
║  sched-spy v1.0   PID: 1234 (nginx)   threshold: 1.0ms   [q] quit  ║
╠══════════════════════════════════════════════════════════════════════╣
║  STATISTICS                                                          ║
║  Samples:  12,847    Min:  0.03ms    Mean:  0.21ms    Max:  14.2ms  ║
║  p50:  0.08ms        p95:  1.2ms     p99:  8.4ms     Stddev: 0.9ms  ║
║  Migrations: 23      Primary CPU: cpu2 (87%)                         ║
╠════════════════════════════════╦════════════════════════════════════╣
║  LATENCY HISTOGRAM (µs)        ║  RECENT SPIKES (> 1.0ms)          ║
║                                ║                                    ║
║  1-2µs   ████████████  42%    ║  14:02:33  14.2ms  kworker/u:0    ║
║  2-4µs   ██████        21%    ║  14:02:31   8.4ms  kworker/2:1    ║
║  4-8µs   ████          14%    ║  14:02:28   4.1ms  migration/2    ║
║  8-16µs  ████          13%    ║  14:02:25   2.9ms  kworker/0:1    ║
║  16-32µs ██             7%    ║  14:02:21   2.1ms  ksoftirqd/2    ║
║  32-64µs █              2%    ║                                    ║
║  64+µs   █              1%    ║                                    ║
║  1ms+    spikes:    12        ║                                    ║
╠════════════════════════════════╩════════════════════════════════════╣
║  LIVE EVENTS (last 10)                                               ║
║  14:02:35.123  0.04ms  [cpu2]  woken by: -                          ║
║  14:02:35.120  0.08ms  [cpu1]  woken by: -                          ║
║  14:02:35.089  0.12ms  [cpu2]  woken by: -                          ║
║  14:02:34.991 ⚠ 2.1ms [cpu0]  woken by: ksoftirqd/0               ║
╚══════════════════════════════════════════════════════════════════════╝
```

### Implementation approach

```c
#define CLEAR_SCREEN    "\033[2J\033[H"
#define BOLD            "\033[1m"
#define RED             "\033[31m"
#define YELLOW          "\033[33m"
#define GREEN           "\033[32m"
#define RESET           "\033[0m"

void terminal_render(const MetricsStore *store, const Config *cfg) {
    // Build entire frame into a buffer, then write once (prevents flicker)
    char frame[8192];
    int  len = 0;

    len += snprintf(frame+len, sizeof(frame)-len, CLEAR_SCREEN);

    // Header
    len += render_header(frame+len, sizeof(frame)-len, cfg);

    // Stats row
    PidMetrics *m = metrics_get(store, cfg->target_pids[0]);
    if (m) {
        len += render_stats(frame+len, sizeof(frame)-len, m);
        len += render_histogram(frame+len, sizeof(frame)-len, m);
        len += render_spikes(frame+len, sizeof(frame)-len, m);
        len += render_live_events(frame+len, sizeof(frame)-len, m);
    }

    write(STDOUT_FILENO, frame, len);
}
```

### Rendering the histogram bar

```c
static int render_histogram_bar(char *buf, size_t sz,
                                 uint64_t count, uint64_t total,
                                 int bar_width) {
    if (total == 0) return snprintf(buf, sz, "%*s  0%%", bar_width, "");

    double pct    = (double)count / total;
    int    filled = (int)(pct * bar_width);

    int len = 0;
    for (int i = 0; i < bar_width; i++) {
        len += snprintf(buf+len, sz-len, i < filled ? "█" : " ");
    }
    len += snprintf(buf+len, sz-len, "  %3.0f%%", pct * 100);
    return len;
}
```

### Format nanoseconds human-readable

```c
static const char *format_ns(uint64_t ns, char *buf, size_t sz) {
    if (ns < 1000) {
        snprintf(buf, sz, "%lluns", (unsigned long long)ns);
    } else if (ns < 1000000) {
        snprintf(buf, sz, "%.2fµs", ns / 1000.0);
    } else if (ns < 1000000000) {
        snprintf(buf, sz, "%.2fms", ns / 1000000.0);
    } else {
        snprintf(buf, sz, "%.2fs", ns / 1000000000.0);
    }
    return buf;
}
```

---

## 14. Phase 8 — JSON + File Output

### JSON log format

Each spike event is a line of newline-delimited JSON (NDJSON):

```json
{"ts":"14:02:33.123","pid":1234,"comm":"nginx","latency_ns":14200000,"latency_ms":14.2,"cpu":0,"preempted_by":"kworker/u:0","preempted_by_pid":87,"migrated":false}
```

### JSON stats dump (on exit or on signal)

```json
{
  "pid": 1234,
  "comm": "nginx",
  "duration_sec": 60,
  "samples": 12847,
  "min_ns": 31000,
  "max_ns": 14200000,
  "mean_us": 0.21,
  "stddev_us": 0.9,
  "p50_ns": 80000,
  "p95_ns": 1200000,
  "p99_ns": 8400000,
  "spikes_above_threshold": 12,
  "threshold_ns": 1000000,
  "migrations": 23,
  "cpu_affinity": {"cpu0": 12, "cpu1": 98, "cpu2": 12737},
  "histogram": [0,0,0,0,0,0,0,0,0,0,5394,3100,1800,...]
}
```

### Implementation

```c
void output_json_event(FILE *f, const SchedEvent *ev) {
    // Get wall clock time for human-readable timestamp
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    char timestr[32];
    struct tm *tm = localtime(&ts.tv_sec);
    strftime(timestr, sizeof(timestr), "%H:%M:%S", tm);

    fprintf(f,
        "{\"ts\":\"%s.%03ld\","
        "\"pid\":%d,\"comm\":\"%s\","
        "\"latency_ns\":%llu,\"latency_ms\":%.3f,"
        "\"cpu\":%d,"
        "\"preempted_by\":\"%s\",\"preempted_by_pid\":%d,"
        "\"migrated\":%s}\n",
        timestr, ts.tv_nsec / 1000000,
        ev->pid, ev->comm,
        (unsigned long long)ev->latency_ns,
        ev->latency_ns / 1000000.0,
        ev->cpu,
        ev->preempted_by, ev->preempted_by_pid,
        ev->migrated ? "true" : "false");

    fflush(f); // flush after each event so output is usable during run
}
```

---

## 15. Phase 9 — HTTP Metrics Endpoint

If `http1.c` is already built, integrating it here is ~50 lines.

### Endpoints

| Endpoint | Method | Response |
|----------|--------|----------|
| `/` | GET | HTML dashboard (auto-refreshes every 2s) |
| `/metrics` | GET | JSON stats for all tracked PIDs |
| `/metrics/:pid` | GET | JSON stats for specific PID |
| `/events` | GET | SSE stream of spike events |
| `/health` | GET | `{"status":"ok"}` |

### /metrics handler

```c
void handle_metrics(Request *req, Response *res) {
    MetricsStore *store = (MetricsStore *)req->_conn; // passed via server userdata

    char buf[65536];
    int  len = 0;

    len += snprintf(buf+len, sizeof(buf)-len, "{\"pids\":[");

    int first = 1;
    for (int i = 0; i < store->count; i++) {
        PidMetrics *m = &store->metrics[i];
        if (!first) len += snprintf(buf+len, sizeof(buf)-len, ",");
        first = 0;

        len += snprintf(buf+len, sizeof(buf)-len,
            "{\"pid\":%d,\"comm\":\"%s\","
            "\"count\":%llu,"
            "\"mean_us\":%.2f,"
            "\"p99_ns\":%llu,"
            "\"max_ns\":%llu,"
            "\"spikes\":%d}",
            m->pid, m->comm,
            (unsigned long long)m->count,
            m->mean,
            (unsigned long long)histogram_percentile(m, 0.99),
            (unsigned long long)m->max_ns,
            m->spike_count);
    }

    len += snprintf(buf+len, sizeof(buf)-len, "]}");
    res_json(res, 200, buf);
}
```

### SSE spike stream

```c
void handle_events_sse(Request *req, Response *res) {
    res_sse_start(res);

    // This handler runs until client disconnects
    // sched-spy pushes events to a shared spike_queue
    // This thread drains the queue and sends SSE events
    while (1) {
        SchedEvent ev;
        if (spike_queue_pop(&global_spike_queue, &ev)) {
            char data[256];
            snprintf(data, sizeof(data),
                "{\"pid\":%d,\"latency_ms\":%.2f,\"preempted_by\":\"%s\"}",
                ev.pid, ev.latency_ns / 1000000.0, ev.preempted_by);
            res_sse_event(res, "spike", data, NULL);
        } else {
            // Send keepalive comment every 15s to prevent timeout
            res_sse_event(res, NULL, ": keepalive", NULL);
            usleep(15 * 1000 * 1000);
        }
    }
}
```

---

## 16. Phase 10 — Multi-CPU Awareness

### Per-CPU reader threads

```c
typedef struct {
    int        cpu;
    PerfFd     perf_fds[4];     // one per tracepoint
    RingQueue  queue;           // SPSC to correlator
    pthread_t  thread;
    int        running;
} CpuReader;

void *cpu_reader_thread(void *arg) {
    CpuReader *r = (CpuReader *)arg;

    // Set CPU affinity of this thread to the CPU it reads
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(r->cpu, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    // Use epoll to wait for ring buffer events
    int epfd = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = r };
    epoll_ctl(epfd, EPOLL_CTL_ADD, r->perf_fds[0].fd, &ev);

    while (r->running) {
        struct epoll_event events[4];
        int n = epoll_wait(epfd, events, 4, 100);
        if (n < 0 && errno == EINTR) continue;

        perf_read_cpu(&r->perf_fds[0], &r->queue);
    }

    close(epfd);
    return NULL;
}
```

### Getting CPU count

```c
static int get_cpu_count(void) {
    return (int)sysconf(_SC_NPROCESSORS_ONLN);
}
```

### Starting all reader threads

```c
void start_readers(SchedSpy *s) {
    int ncpus = get_cpu_count();
    s->readers = calloc(ncpus, sizeof(CpuReader));
    s->reader_count = ncpus;

    for (int cpu = 0; cpu < ncpus; cpu++) {
        CpuReader *r = &s->readers[cpu];
        r->cpu = cpu;
        ring_queue_init(&r->queue);

        int tp_ids[] = {
            s->tp_sched_switch,
            s->tp_sched_wakeup,
            s->tp_sched_wakeup_new,
            s->tp_sched_migrate
        };
        setup_perf_cpu(cpu, tp_ids, 4, r->perf_fds);

        r->running = 1;
        pthread_create(&r->thread, NULL, cpu_reader_thread, r);
    }
}
```

### Correlator consumes from all CPU queues in round-robin

```c
void *correlator_thread(void *arg) {
    SchedSpy *s = (SchedSpy *)arg;

    while (s->running) {
        int got_any = 0;

        for (int i = 0; i < s->reader_count; i++) {
            RawEvent ev;
            while (ring_queue_pop(&s->readers[i].queue, &ev)) {
                correlator_process(&s->correlator, &ev);
                got_any = 1;
            }
        }

        if (!got_any) {
            // No events — sleep briefly to avoid busy-spinning
            struct timespec ts = { .tv_nsec = 100000 }; // 100µs
            nanosleep(&ts, NULL);
        }

        // Periodic cleanup
        static uint64_t last_cleanup = 0;
        uint64_t now = now_ns();
        if (now - last_cleanup > 1000000000ULL) { // every 1 second
            correlator_cleanup_stale(&s->correlator, now);
            last_cleanup = now;
        }
    }
    return NULL;
}
```

---

## 17. Phase 11 — Kernel Version Portability

### Version-dependent behaviors

| Kernel Version | Behavior |
|----------------|----------|
| < 3.x | tracefs may not exist; use debugfs |
| 3.x - 4.x | perf_event_open works, paranoid defaults vary |
| >= 4.7 | eBPF tracepoint programs available (not used) |
| >= 5.8 | CAP_PERFMON added (replaces CAP_SYS_ADMIN for perf) |
| >= 5.x | tracefs moved from debugfs to own mount |
| >= 6.x | Some tracepoint field layouts changed |

### Defensive field access

Always use the parsed `TraceFormat` struct from Phase 4 (runtime offset reading). Never hardcode byte offsets. This alone handles most kernel version differences.

### tracefs mount point detection

```c
static const char *find_tracefs(void) {
    // Check modern path first
    if (access("/sys/kernel/tracing/trace_pipe", R_OK) == 0)
        return "/sys/kernel/tracing";

    // Old path under debugfs
    if (access("/sys/kernel/debug/tracing/trace_pipe", R_OK) == 0)
        return "/sys/kernel/debug/tracing";

    // Check /proc/mounts for tracefs
    FILE *f = fopen("/proc/mounts", "r");
    if (f) {
        char device[64], mountpoint[256], fstype[32], rest[256];
        while (fscanf(f, "%63s %255s %31s %255s", device, mountpoint, fstype, rest) == 4) {
            if (strcmp(fstype, "tracefs") == 0) {
                fclose(f);
                return strdup(mountpoint); // caller owns
            }
        }
        fclose(f);
    }

    return NULL;
}
```

### prev_state field interpretation

The `prev_state` field in `sched_switch` changed meaning in kernel 4.14. Before 4.14, the raw value matched `TASK_*` constants directly. After 4.14, it was changed to a bitmask.

```c
static int task_is_runnable(long prev_state, int kernel_major, int kernel_minor) {
    // 0 = TASK_RUNNING in all versions
    return (prev_state == 0);
}
```

---

## 18. Phase 12 — Test Suite

### test_correlator.c

```c
// Test 1: Basic wakeup → switch correlation
void test_basic_correlation() {
    Correlator c = {0};
    MetricsStore store = {0};
    correlator_init(&c, &store, &default_cfg);

    RawEvent wakeup = {
        .type         = RAW_SCHED_WAKEUP,
        .timestamp_ns = 1000000,   // 1ms
        .pid          = 1234,
        .target_cpu   = 0,
    };
    strncpy(wakeup.comm, "nginx", 15);

    RawEvent switch_ev = {
        .type         = RAW_SCHED_SWITCH,
        .timestamp_ns = 1002000,   // 1ms + 2µs
        .next_pid     = 1234,
        .prev_pid     = 9999,
        .cpu          = 0,
    };
    strncpy(switch_ev.next_comm, "nginx", 15);
    strncpy(switch_ev.prev_comm, "kworker", 15);

    correlator_process(&c, &wakeup);
    correlator_process(&c, &switch_ev);

    PidMetrics *m = metrics_get(&store, 1234);
    assert(m != NULL);
    assert(m->count == 1);
    assert(m->max_ns == 2000);  // 2µs
    printf("PASS: test_basic_correlation\n");
}

// Test 2: Switch arrives before wakeup (out of order events)
void test_switch_before_wakeup() { ... }

// Test 3: Multiple PIDs tracked simultaneously
void test_multiple_pids() { ... }

// Test 4: Process exits during tracking (stale cleanup)
void test_stale_cleanup() { ... }

// Test 5: Migration — wakeup on cpu0, runs on cpu1
void test_migration_detected() { ... }

// Test 6: Double wakeup (woken twice, runs once)
void test_double_wakeup() { ... }

// Test 7: Filter — only track specific PID
void test_pid_filter() { ... }

// Test 8: Filter — only track by comm name
void test_comm_filter() { ... }
```

### test_metrics.c

```c
// Test Welford mean: add 1000 random values, compare to naive mean
void test_welford_mean() { ... }

// Test histogram percentile: known distribution, verify p50/p99
void test_histogram_percentile() { ... }

// Test spike ring buffer: add 300 spikes, verify FIFO behavior
void test_spike_ring_buffer() { ... }
```

### test_tracefs_parser.c

```c
// Feed real trace_pipe lines from a saved capture, verify parsed output
void test_parse_sched_switch() {
    const char *line =
        "    nginx-1234  [002] d...  1234567.890123: sched_switch: "
        "prev_comm=nginx prev_pid=1234 prev_prio=120 prev_state=S "
        "==> next_comm=sshd next_pid=5678 next_prio=120";

    RawEvent ev;
    int ret = parse_trace_line(line, &ev);
    assert(ret == 0);
    assert(ev.type == RAW_SCHED_SWITCH);
    assert(ev.prev_pid == 1234);
    assert(ev.next_pid == 5678);
    assert(ev.cpu == 2);
    printf("PASS: test_parse_sched_switch\n");
}

void test_parse_sched_wakeup() { ... }
void test_parse_fragmented_line() { ... } // line split across two reads
```

### test_ring_queue.c

```c
// Test SPSC queue: producer/consumer in separate threads, verify no drops
void test_ring_queue_concurrent() { ... }

// Test wrap-around
void test_ring_queue_wrap() { ... }

// Test full queue behavior (producer blocks or drops?)
void test_ring_queue_full() { ... }
```

### Integration test: generate artificial scheduler events

```bash
# Spawn a process that sleeps for exactly 1ms in a tight loop
# Verify sched-spy reports latencies near 1ms

cat > /tmp/sleeper.c << 'EOF'
#include <time.h>
int main() {
    struct timespec ts = { .tv_nsec = 1000000 }; // 1ms
    for (int i = 0; i < 1000; i++) nanosleep(&ts, NULL);
    return 0;
}
EOF
gcc /tmp/sleeper.c -o /tmp/sleeper

PID=$(/tmp/sleeper & echo $!)
./sched-spy --pid $PID --json /tmp/out.json --no-terminal
wait $PID

# Verify output: mean should be ≥ 1ms, max reasonable (< 50ms on unloaded system)
python3 -c "
import json
data = json.load(open('/tmp/out.json'))
assert data['mean_us'] >= 1000, f'mean too low: {data[\"mean_us\"]}'
print('PASS: integration test')
"
```

---

## 19. CLI Design — Full Specification

```
USAGE:
  sched-spy [OPTIONS]

TARGET SELECTION (at least one required):
  --pid PID              Watch a specific PID (repeatable: --pid 1 --pid 2)
  --comm NAME            Watch processes by comm name (e.g. --comm nginx)
  --all                  Watch ALL processes (requires root)

FILTERING:
  --threshold MS         Only show latency spikes above MS milliseconds
                         Default: 1.0ms
  --min-latency NS       Minimum latency to record (ns). Default: 0

OUTPUT:
  --json FILE            Write NDJSON spike log to FILE
  --stats-json FILE      Write final statistics to FILE on exit
  --no-terminal          Suppress live terminal output (useful for --json mode)
  --verbose              Show environment probe results, lost event counts

SERVER:
  --serve PORT           Expose /metrics HTTP endpoint on PORT
                         (requires http1.c to be compiled in)

BEHAVIOR:
  --duration SEC         Run for SEC seconds then exit
  --force-tracefs        Skip perf_event_open, use tracefs fallback
  --refresh MS           Terminal refresh interval. Default: 500ms
  --keep-alive SEC       How long to keep stale wakeup entries. Default: 5

EXAMPLES:
  # Watch nginx, show spikes > 2ms
  sched-spy --pid $(pgrep nginx) --threshold 2

  # Watch all processes named "postgres", log to file
  sched-spy --comm postgres --json /tmp/pg_latency.ndjson

  # Run for 60 seconds and dump stats
  sched-spy --pid 1234 --duration 60 --stats-json /tmp/stats.json

  # Expose live metrics on port 9090
  sched-spy --pid 1234 --serve 9090 --no-terminal

  # Force tracefs fallback (useful in containers)
  sched-spy --pid 1234 --force-tracefs

SIGNALS:
  SIGINT (Ctrl-C)    Print final statistics, clean up tracepoints, exit
  SIGUSR1            Dump current statistics to stdout (without stopping)
  SIGUSR2            Reset all statistics counters
```

---

## 20. Error Handling Strategy

### Hard errors (unrecoverable — exit with message)

```c
// No kernel interface available
if (reader == READER_NONE) {
    fprintf(stderr, "sched-spy: no kernel tracing interface available.\n");
    fprintf(stderr, "  Run as root, or set perf_event_paranoid=1\n");
    exit(1);
}

// Target PID does not exist
if (kill(cfg.target_pids[0], 0) < 0 && errno == ESRCH) {
    fprintf(stderr, "sched-spy: PID %d not found\n", cfg.target_pids[0]);
    exit(1);
}
```

### Soft errors (recoverable — log and continue)

```c
// Lost events in perf ring buffer
if (record_type == PERF_RECORD_LOST) {
    atomic_fetch_add(&stats.lost_events, lost_count);
    if (cfg.verbose) {
        fprintf(stderr, "[%llu events lost on CPU %d]\n",
                (unsigned long long)lost_count, cpu);
    }
}

// Wakeup with no matching switch (process exited before running)
// → increment miss counter, continue
c->wakeup_map.miss_count++;
```

### Cleanup on all exit paths

```c
// Register cleanup in main()
atexit(cleanup_tracepoints);
signal(SIGINT,  signal_handler);
signal(SIGTERM, signal_handler);
signal(SIGQUIT, signal_handler);
signal(SIGPIPE, SIG_IGN);         // never crash on broken socket

static void cleanup_tracepoints(void) {
    if (global_tracefs_path) {
        tracefs_teardown(global_tracefs_path);
    }
    // Munmap all perf ring buffers
    // Join all reader threads
    // Close all fds
}
```

---

## 21. Security and Privilege Model

### What each interface needs

| Interface | Required Privilege |
|-----------|-------------------|
| tracefs enable/read | Read access to `/sys/kernel/tracing` — often root only |
| perf_event_open (all PIDs) | `CAP_SYS_ADMIN` or `perf_event_paranoid <= -1` |
| perf_event_open (own PID) | No special privileges needed |
| perf_event_open (other PID) | `perf_event_paranoid <= 1` and same UID, OR root |

### Privilege dropping

After setup (opening fds, enabling tracepoints), drop root privileges:

```c
static void drop_privileges(void) {
    if (getuid() != 0) return; // already not root

    // Drop to the user who invoked via sudo, if available
    const char *sudo_uid = getenv("SUDO_UID");
    const char *sudo_gid = getenv("SUDO_GID");

    if (sudo_uid && sudo_gid) {
        uid_t uid = (uid_t)atoi(sudo_uid);
        gid_t gid = (gid_t)atoi(sudo_gid);

        if (setgid(gid) < 0 || setuid(uid) < 0) {
            // Non-fatal: continue as root if drop fails
            fprintf(stderr, "Warning: could not drop root privileges\n");
        }
    }
}
// Call after all perf fds are open and tracefs is configured
```

### tracefs path injection prevention

The tracefs path comes from `/proc/mounts` or known hardcoded paths — never from user input. The only user input that touches filesystem paths is `--json FILE` and `--stats-json FILE`.

```c
// Validate output file path before opening
static int validate_output_path(const char *path) {
    // Reject paths that escape via ..
    if (strstr(path, "..")) return -1;
    // Reject paths to /sys, /proc, /dev
    if (strncmp(path, "/sys",  4) == 0) return -1;
    if (strncmp(path, "/proc", 5) == 0) return -1;
    if (strncmp(path, "/dev",  4) == 0) return -1;
    return 0;
}
```

---

## 22. Known Edge Cases and Pitfalls

| Edge Case | Where | What Happens | Fix |
|-----------|-------|-------------|-----|
| Tracepoints left enabled after crash | tracefs | Kernel profiles everything until reboot | `atexit()` + signal handler for SIGINT/TERM/QUIT |
| perf ring buffer overrun | perf reader | Events silently dropped, `PERF_RECORD_LOST` emitted | Count lost events, warn user, increase buffer size |
| wakeup_map collision (PID reuse) | Correlator | New process inherits wakeup entry of old | Evict old entry, insert new. PID reuse is rare but real |
| sched_switch arrives before sched_wakeup | Correlator | No matching wakeup → counted as miss | Normal at startup; miss rate should drop after 1s |
| Process migrates between CPUs | Correlator | Wakeup on cpu0, switch on cpu1 | Track cpu per wakeup entry; set migrated=1 on mismatch |
| CPU hotplug (CPUs added/removed at runtime) | Reader threads | Reader for that CPU dies or misses new CPU | Handle PERF_RECORD_LOST, re-probe CPU count on error |
| Container without tracefs | Probe | Silent failure | Probe checks explicitly; error message with solution |
| Docker with seccomp blocking perf_event_open | Probe | EPERM on syscall | Detect EPERM, suggest `--cap-add SYS_ADMIN` or `--force-tracefs` |
| High-frequency wakeup flood (>1M/sec) | Ring queue | Queue fills, events dropped | Increase RING_QUEUE_SIZE; filter by PID to reduce volume |
| Clock discontinuity (NTP step) | Correlator | Latency = negative or astronomically large | Filter: discard latency_ns > 10 seconds as invalid |
| Kernel with PREEMPT_RT patch | All | Scheduler behavior differs slightly | No code changes needed; just document behavior difference |
| `/proc/PID/comm` truncates at 15 chars | Comm filter | Long process names won't match | Truncate user input to 15 chars before comparison |
| SIGPIPE when HTTP client disconnects | HTTP layer | Process killed by SIGPIPE | `signal(SIGPIPE, SIG_IGN)` at startup |

---

## 23. Build System

### Makefile

```makefile
CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE
LDFLAGS = -lpthread

# Optional: include http1.c for --serve support
HTTP_SRC = ../http1/http1.c
HTTP_FLAG = -DSCHED_SPY_HTTP

.PHONY: all clean test debug

all: sched-spy

# Without HTTP server
sched-spy: sched_spy.c src/perf_reader.c src/tracefs_reader.c \
           src/correlator.c src/metrics.c src/terminal.c \
           src/output_json.c src/ring_queue.c src/probe.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# With HTTP /metrics endpoint
sched-spy-http: sched_spy.c src/*.c $(HTTP_SRC)
	$(CC) $(CFLAGS) $(HTTP_FLAG) $^ -o $@ $(LDFLAGS)

test:
	bash test/run_tests.sh

# Debug: AddressSanitizer + UBSan
debug: sched_spy.c src/*.c
	$(CC) -g -fsanitize=address,undefined $(CFLAGS) $^ -o sched-spy-debug $(LDFLAGS)

clean:
	rm -f sched-spy sched-spy-http sched-spy-debug test/test_*
```

### Compile flags

| Flag | Why |
|------|-----|
| `-D_GNU_SOURCE` | Enables `perf_event_open`, `epoll`, `sched_setaffinity` |
| `-std=c11` | `_Atomic` for lock-free queue |
| `-O2` | Ring buffer reader runs hot — needs optimization |
| `-lpthread` | Reader threads |
| `-fsanitize=address` | Debug: catch out-of-bounds in ring buffer code |

---

## 24. Milestone Checklist

Work through these in order. Each milestone must be solid before proceeding.

### Milestone 1 — Environment Probe
- [ ] Detect tracefs path (`/sys/kernel/tracing` or debugfs fallback)
- [ ] Read kernel version
- [ ] Check `perf_event_paranoid`
- [ ] Dry-run `perf_event_open` test
- [ ] Print clear human-readable environment report
- [ ] Decision: choose reader or exit with actionable error

### Milestone 2 — tracefs Reader (Fallback Path)
- [ ] Enable `sched_wakeup`, `sched_switch`, `sched_migrate_task`, `sched_wakeup_new`
- [ ] Open `trace_pipe` with poll loop
- [ ] Parse `sched_switch` lines correctly
- [ ] Parse `sched_wakeup` lines correctly
- [ ] Parse `sched_migrate_task` lines correctly
- [ ] Handle partial reads across poll cycles
- [ ] Disable tracepoints on exit (atexit + signal handlers)
- [ ] Test: run and see raw events in verbose mode

### Milestone 3 — Correlator (Core Logic)
- [ ] WakeupMap: insert, find, remove, stale cleanup
- [ ] correlator_process: wakeup → switch → SchedEvent
- [ ] prev_state logic (don't remove runnable tasks from wakeup_map)
- [ ] Migration detection
- [ ] PID and comm-name filtering
- [ ] Stale entry cleanup every 1 second
- [ ] Test: all correlator unit tests pass

### Milestone 4 — Metrics Aggregation
- [ ] Welford online mean + variance
- [ ] Log2 histogram
- [ ] Percentile calculation (p50, p95, p99)
- [ ] Spike ring buffer (last 256 above threshold)
- [ ] CPU affinity tracking
- [ ] Migration counter
- [ ] Test: known distribution → correct percentiles

### Milestone 5 — Terminal Output
- [ ] Header bar with PID/comm/threshold
- [ ] Stats row (count, min, mean, max, p99, stddev)
- [ ] Histogram bar chart
- [ ] Recent spikes list
- [ ] Live event feed (last 10)
- [ ] 500ms refresh without flicker
- [ ] Clean exit on `q` keypress

### Milestone 6 — perf_event_open Primary Path
- [ ] Read tracepoint IDs from tracefs
- [ ] Parse tracepoint format files (field offsets)
- [ ] Open per-CPU ring buffers
- [ ] Read ring buffer, handle wrap-around
- [ ] Parse binary sample records using runtime offsets
- [ ] Handle `PERF_RECORD_LOST`
- [ ] Enable/disable via ioctl
- [ ] Test: same output as tracefs path on same workload

### Milestone 7 — Multi-CPU Threading
- [ ] One reader thread per CPU
- [ ] CPU affinity pinning per thread
- [ ] SPSC ring queues (reader → correlator)
- [ ] Correlator thread drains all queues
- [ ] Test: 100k events/sec without drops on 4-CPU system

### Milestone 8 — JSON Output
- [ ] NDJSON spike log to file
- [ ] Final stats JSON dump on exit
- [ ] `--duration` flag (run for N seconds then dump)
- [ ] `SIGUSR1` dumps stats without stopping

### Milestone 9 — HTTP Endpoint
- [ ] `/metrics` JSON endpoint
- [ ] `/events` SSE spike stream
- [ ] `/` HTML auto-refresh dashboard
- [ ] `--serve PORT` flag
- [ ] Works alongside terminal output

### Milestone 10 — Portability + Hardening
- [ ] Kernel version portability: 4.x, 5.x, 6.x tested
- [ ] Container fallback (tracefs reader when perf blocked)
- [ ] Privilege drop after setup
- [ ] All signal handlers registered
- [ ] All edge cases from section 22 tested
- [ ] AddressSanitizer: zero errors under 60s load
- [ ] UBSanitizer: zero errors
- [ ] No tracepoints left enabled after any exit path

### Milestone 11 — CLI + README
- [ ] All `--flags` from section 19 implemented
- [ ] `--help` output is complete
- [ ] README: what it measures, install, usage examples, container guide
- [ ] Example JSON output in README

---

*sched-spy — start with the probe, ship the correlator, everything else is output.*
