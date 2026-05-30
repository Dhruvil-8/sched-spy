#include "output_json.h"

#include "metrics.h"

#include <errno.h>
#include <string.h>

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

int json_writer_open(JsonWriter *w, const char *path) {
    memset(w, 0, sizeof(*w));
    if (!path) {
        return 0;
    }
    w->file = fopen(path, "a");
    return w->file ? 0 : -1;
}

void json_writer_close(JsonWriter *w) {
    if (w && w->file) {
        fclose(w->file);
        w->file = NULL;
    }
}

void json_writer_write_event(JsonWriter *w, const SchedEvent *event) {
    if (!w || !w->file) {
        return;
    }
    fprintf(w->file,
            "{\"pid\":%d,\"comm\":",
            (int)event->pid);
    json_escape(w->file, event->comm);
    fprintf(w->file,
            ",\"wakeup_ns\":%" PRIu64 ",\"run_ns\":%" PRIu64 ",\"latency_ns\":%" PRIu64
            ",\"latency_ms\":%.6f,\"preempted_by\":",
            event->wakeup_ns,
            event->run_ns,
            event->latency_ns,
            (double)event->latency_ns / 1000000.0);
    json_escape(w->file, event->preempted_by);
    fprintf(w->file,
            ",\"preempted_by_pid\":%d,\"cpu\":%d,\"migrated\":%s}\n",
            (int)event->preempted_by_pid,
            event->cpu,
            event->migrated ? "true" : "false");
    fflush(w->file);
}

int json_write_stats_file(const char *path, MetricsStore *store) {
    if (!path) {
        return 0;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        return -1;
    }
    metrics_write_json(store, f);
    return fclose(f);
}
