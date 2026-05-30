#ifndef SCHED_SPY_OUTPUT_JSON_H
#define SCHED_SPY_OUTPUT_JSON_H

#include "sched_spy.h"

struct JsonWriter {
    FILE *file;
};

int json_writer_open(JsonWriter *w, const char *path);
void json_writer_close(JsonWriter *w);
void json_writer_write_event(JsonWriter *w, const SchedEvent *event);
int json_write_stats_file(const char *path, MetricsStore *store);

#endif
