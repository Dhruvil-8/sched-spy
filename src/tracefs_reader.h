#ifndef SCHED_SPY_TRACEFS_READER_H
#define SCHED_SPY_TRACEFS_READER_H

#include "sched_spy.h"
#include <stddef.h>

typedef struct {
    char tracefs_path[256];
    int fd;
    char buffer[32768];
    size_t buffer_len;
    int configured;
} TracefsReader;

int tracefs_enable_events(const char *tracefs_path, int enable, char *err, size_t err_len);
int tracefs_reader_open(TracefsReader *reader, const char *tracefs_path, char *err, size_t err_len);
void tracefs_reader_close(TracefsReader *reader);
int tracefs_reader_next(TracefsReader *reader, RawEvent *event, int timeout_ms);
int parse_trace_line(const char *line, RawEvent *event);

#endif
