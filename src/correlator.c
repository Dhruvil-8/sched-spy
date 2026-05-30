#include "correlator.h"

#include <string.h>

static void copy_comm(char dst[SCHED_SPY_COMM_LEN], const char *src) {
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= SCHED_SPY_COMM_LEN) {
        n = SCHED_SPY_COMM_LEN - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static size_t hash_pid(pid_t pid) {
    uint32_t x = (uint32_t)pid;
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    return (size_t)x & (WAKEUP_MAP_SIZE - 1);
}

static WakeupEntry *find_entry(Correlator *c, pid_t pid) {
    size_t start = hash_pid(pid);
    for (size_t i = 0; i < WAKEUP_MAP_SIZE; i++) {
        WakeupEntry *e = &c->entries[(start + i) & (WAKEUP_MAP_SIZE - 1)];
        if (e->occupied && e->pid == pid) {
            return e;
        }
    }
    return NULL;
}

static WakeupEntry *slot_for_insert(Correlator *c, pid_t pid) {
    size_t start = hash_pid(pid);
    WakeupEntry *first_empty = NULL;
    WakeupEntry *oldest = NULL;
    for (size_t i = 0; i < WAKEUP_MAP_SIZE; i++) {
        WakeupEntry *e = &c->entries[(start + i) & (WAKEUP_MAP_SIZE - 1)];
        if (e->occupied && e->pid == pid) {
            return e;
        }
        if (!e->occupied && !first_empty) {
            first_empty = e;
        }
        if (e->occupied && (!oldest || e->timestamp_ns < oldest->timestamp_ns)) {
            oldest = e;
        }
    }
    if (first_empty) {
        return first_empty;
    }
    c->stale_evictions++;
    return oldest;
}

static void remove_entry(WakeupEntry *e) {
    memset(e, 0, sizeof(*e));
}

static void insert_wakeup(Correlator *c, const RawEvent *ev) {
    if (!sched_spy_target_matches(c->cfg, ev->pid, ev->comm)) {
        return;
    }

    WakeupEntry *e = slot_for_insert(c, ev->pid);
    e->pid = ev->pid;
    e->timestamp_ns = ev->timestamp_ns;
    e->cpu = ev->target_cpu >= 0 ? ev->target_cpu : ev->cpu;
    copy_comm(e->comm, ev->comm);
    e->occupied = 1;
    c->inserts++;
}

static void process_switch(Correlator *c, const RawEvent *ev) {
    WakeupEntry *e = find_entry(c, ev->next_pid);
    if (!e) {
        c->misses++;
        return;
    }

    if (ev->timestamp_ns < e->timestamp_ns) {
        remove_entry(e);
        c->misses++;
        return;
    }

    uint64_t latency = ev->timestamp_ns - e->timestamp_ns;
    if (latency > 10000000000ULL) {
        remove_entry(e);
        c->misses++;
        return;
    }

    if (latency >= c->cfg->min_latency_ns) {
        SchedEvent out;
        memset(&out, 0, sizeof(out));
        out.pid = e->pid;
        copy_comm(out.comm, e->comm[0] ? e->comm : ev->next_comm);
        out.wakeup_ns = e->timestamp_ns;
        out.run_ns = ev->timestamp_ns;
        out.latency_ns = latency;
        copy_comm(out.preempted_by, ev->prev_comm);
        out.preempted_by_pid = ev->prev_pid;
        out.cpu = ev->cpu;
        out.migrated = (e->cpu >= 0 && ev->cpu >= 0 && e->cpu != ev->cpu);
        metrics_record(c->metrics, &out);
        if (c->json && latency >= c->cfg->threshold_ns) {
            json_writer_write_event(c->json, &out);
        }
        c->hits++;
    }

    remove_entry(e);
}

void correlator_init(Correlator *c, const SchedSpyConfig *cfg, MetricsStore *metrics, JsonWriter *json) {
    memset(c, 0, sizeof(*c));
    c->cfg = cfg;
    c->metrics = metrics;
    c->json = json;
}

void correlator_process(Correlator *c, const RawEvent *ev) {
    switch (ev->type) {
    case RAW_SCHED_WAKEUP:
    case RAW_SCHED_WAKEUP_NEW:
        insert_wakeup(c, ev);
        break;
    case RAW_SCHED_SWITCH:
        process_switch(c, ev);
        break;
    case RAW_SCHED_MIGRATE: {
        WakeupEntry *e = find_entry(c, ev->pid);
        if (e) {
            e->cpu = ev->dest_cpu;
        }
        break;
    }
    default:
        break;
    }
}

void correlator_cleanup_stale(Correlator *c, uint64_t now_ns) {
    uint64_t keep = c->cfg->keep_alive_ns;
    for (size_t i = 0; i < WAKEUP_MAP_SIZE; i++) {
        WakeupEntry *e = &c->entries[i];
        if (e->occupied && now_ns >= e->timestamp_ns && now_ns - e->timestamp_ns > keep) {
            remove_entry(e);
            c->stale_evictions++;
        }
    }
}
