#include "metrics.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t *store_lock(MetricsStore *store) {
    return (pthread_mutex_t *)store->lock;
}

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

static int bucket_for(uint64_t value) {
    int bucket = 0;
    while (value > 0 && bucket < SCHED_SPY_HIST_BUCKETS - 1) {
        value >>= 1;
        bucket++;
    }
    return bucket;
}

static uint64_t bucket_upper(int bucket) {
    if (bucket <= 0) {
        return 0;
    }
    if (bucket >= 64) {
        return UINT64_MAX;
    }
    return (1ULL << bucket) - 1ULL;
}

static MetricsNode *find_or_create(MetricsStore *store, const SchedEvent *event) {
    for (MetricsNode *n = store->head; n; n = n->next) {
        if (n->pid == event->pid) {
            return n;
        }
    }

    MetricsNode *n = calloc(1, sizeof(*n));
    if (!n) {
        return NULL;
    }
    n->pid = event->pid;
    copy_comm(n->comm, event->comm);
    n->metrics.min_ns = UINT64_MAX;
    n->next = store->head;
    store->head = n;
    return n;
}

int metrics_init(MetricsStore *store, uint64_t threshold_ns) {
    memset(store, 0, sizeof(*store));
    store->threshold_ns = threshold_ns;
    store->lock = calloc(1, sizeof(pthread_mutex_t));
    if (!store->lock) {
        return -1;
    }
    if (pthread_mutex_init(store_lock(store), NULL) != 0) {
        free(store->lock);
        store->lock = NULL;
        return -1;
    }
    return 0;
}

void metrics_destroy(MetricsStore *store) {
    if (!store) {
        return;
    }
    MetricsNode *n = store->head;
    while (n) {
        MetricsNode *next = n->next;
        free(n);
        n = next;
    }
    if (store->lock) {
        pthread_mutex_destroy(store_lock(store));
        free(store->lock);
    }
    memset(store, 0, sizeof(*store));
}

void metrics_reset(MetricsStore *store, uint64_t threshold_ns) {
    pthread_mutex_lock(store_lock(store));
    MetricsNode *n = store->head;
    while (n) {
        MetricsNode *next = n->next;
        free(n);
        n = next;
    }
    pthread_mutex_t *lock = store_lock(store);
    memset(store, 0, sizeof(*store));
    store->threshold_ns = threshold_ns;
    store->lock = lock;
    pthread_mutex_unlock(lock);
}

void metrics_record(MetricsStore *store, const SchedEvent *event) {
    pthread_mutex_lock(store_lock(store));

    MetricsNode *node = find_or_create(store, event);
    if (!node) {
        pthread_mutex_unlock(store_lock(store));
        return;
    }

    PidMetrics *m = &node->metrics;
    m->count++;
    store->total_events++;
    if (event->latency_ns < m->min_ns) {
        m->min_ns = event->latency_ns;
    }
    if (event->latency_ns > m->max_ns) {
        m->max_ns = event->latency_ns;
    }

    double x = (double)event->latency_ns;
    double delta = x - m->mean_ns;
    m->mean_ns += delta / (double)m->count;
    double delta2 = x - m->mean_ns;
    m->m2_ns += delta * delta2;
    m->histogram[bucket_for(event->latency_ns)]++;
    if (event->migrated) {
        m->migrations++;
        store->total_migrations++;
    }
    if (event->cpu >= 0 && event->cpu < 64) {
        m->cpu_mask |= (1ULL << event->cpu);
    }

    if (event->latency_ns >= store->threshold_ns) {
        size_t idx;
        if (store->spike_count < SCHED_SPY_SPIKES) {
            idx = (store->spike_start + store->spike_count) % SCHED_SPY_SPIKES;
            store->spike_count++;
        } else {
            idx = store->spike_start;
            store->spike_start = (store->spike_start + 1) % SCHED_SPY_SPIKES;
        }
        store->spikes[idx].event = *event;
        store->spikes[idx].sequence = ++store->spike_sequence;
    }

    pthread_mutex_unlock(store_lock(store));
}

PidMetrics *metrics_get(MetricsStore *store, pid_t pid) {
    for (MetricsNode *n = store->head; n; n = n->next) {
        if (n->pid == pid) {
            return &n->metrics;
        }
    }
    return NULL;
}

double metrics_stddev_ns(const PidMetrics *m) {
    if (!m || m->count < 2) {
        return 0.0;
    }
    return sqrt(m->m2_ns / (double)(m->count - 1));
}

uint64_t metrics_percentile_ns(const PidMetrics *m, double percentile) {
    if (!m || m->count == 0) {
        return 0;
    }
    if (percentile <= 0.0) {
        return m->min_ns == UINT64_MAX ? 0 : m->min_ns;
    }
    if (percentile >= 100.0) {
        return m->max_ns;
    }

    uint64_t rank = (uint64_t)ceil((percentile / 100.0) * (double)m->count);
    if (rank == 0) {
        rank = 1;
    }

    uint64_t seen = 0;
    for (int i = 0; i < SCHED_SPY_HIST_BUCKETS; i++) {
        seen += m->histogram[i];
        if (seen >= rank) {
            return bucket_upper(i);
        }
    }
    return m->max_ns;
}

static void json_escape(FILE *out, const char *s) {
    fputc('"', out);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') {
            fputc('\\', out);
        }
        if ((unsigned char)*s >= 0x20) {
            fputc(*s, out);
        }
    }
    fputc('"', out);
}

void metrics_write_json(MetricsStore *store, FILE *out) {
    pthread_mutex_lock(store_lock(store));
    fprintf(out, "{\n  \"total_events\": %" PRIu64 ",\n  \"lost_events\": %" PRIu64 ",\n  \"total_migrations\": %" PRIu64 ",\n  \"pids\": [\n",
            store->total_events, store->lost_events, store->total_migrations);
    int first = 1;
    for (MetricsNode *n = store->head; n; n = n->next) {
        PidMetrics *m = &n->metrics;
        if (!first) {
            fprintf(out, ",\n");
        }
        first = 0;
        fprintf(out, "    {\"pid\": %d, \"comm\": ", (int)n->pid);
        json_escape(out, n->comm);
        fprintf(out,
                ", \"count\": %" PRIu64 ", \"min_ns\": %" PRIu64 ", \"mean_ns\": %.0f, \"max_ns\": %" PRIu64
                ", \"stddev_ns\": %.0f, \"p50_ns\": %" PRIu64 ", \"p95_ns\": %" PRIu64 ", \"p99_ns\": %" PRIu64
                ", \"migrations\": %" PRIu64 ", \"cpu_mask\": %" PRIu64 "}",
                m->count,
                m->min_ns == UINT64_MAX ? 0 : m->min_ns,
                m->mean_ns,
                m->max_ns,
                metrics_stddev_ns(m),
                metrics_percentile_ns(m, 50.0),
                metrics_percentile_ns(m, 95.0),
                metrics_percentile_ns(m, 99.0),
                m->migrations,
                m->cpu_mask);
    }
    fprintf(out, "\n  ]\n}\n");
    pthread_mutex_unlock(store_lock(store));
}

void metrics_write_prometheus(MetricsStore *store, FILE *out) {
    pthread_mutex_lock(store_lock(store));
    fprintf(out, "# TYPE sched_spy_events_total counter\nsched_spy_events_total %" PRIu64 "\n", store->total_events);
    fprintf(out, "# TYPE sched_spy_lost_events_total counter\nsched_spy_lost_events_total %" PRIu64 "\n", store->lost_events);
    for (MetricsNode *n = store->head; n; n = n->next) {
        PidMetrics *m = &n->metrics;
        fprintf(out, "sched_spy_latency_mean_ns{pid=\"%d\",comm=\"%s\"} %.0f\n", (int)n->pid, n->comm, m->mean_ns);
        fprintf(out, "sched_spy_latency_p99_ns{pid=\"%d\",comm=\"%s\"} %" PRIu64 "\n", (int)n->pid, n->comm, metrics_percentile_ns(m, 99.0));
        fprintf(out, "sched_spy_latency_max_ns{pid=\"%d\",comm=\"%s\"} %" PRIu64 "\n", (int)n->pid, n->comm, m->max_ns);
    }
    pthread_mutex_unlock(store_lock(store));
}

void metrics_print_text(MetricsStore *store, FILE *out) {
    pthread_mutex_lock(store_lock(store));
    fprintf(out, "events=%" PRIu64 " lost=%" PRIu64 " migrations=%" PRIu64 "\n",
            store->total_events, store->lost_events, store->total_migrations);
    for (MetricsNode *n = store->head; n; n = n->next) {
        PidMetrics *m = &n->metrics;
        fprintf(out,
                "pid=%d comm=%s count=%" PRIu64 " min=%.3fms mean=%.3fms p99=%.3fms max=%.3fms migrations=%" PRIu64 "\n",
                (int)n->pid,
                n->comm,
                m->count,
                (double)(m->min_ns == UINT64_MAX ? 0 : m->min_ns) / 1000000.0,
                m->mean_ns / 1000000.0,
                (double)metrics_percentile_ns(m, 99.0) / 1000000.0,
                (double)m->max_ns / 1000000.0,
                m->migrations);
    }
    pthread_mutex_unlock(store_lock(store));
}
