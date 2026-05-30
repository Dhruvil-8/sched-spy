#ifndef SCHED_SPY_PERF_READER_H
#define SCHED_SPY_PERF_READER_H

#include "sched_spy.h"

int perf_reader_available(const ProbeResult *probe);
const char *perf_reader_status(void);

#endif
