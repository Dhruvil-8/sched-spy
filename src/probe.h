#ifndef SCHED_SPY_PROBE_H
#define SCHED_SPY_PROBE_H

#include "sched_spy.h"

int probe_environment(ProbeResult *result);
void probe_print(const ProbeResult *result, FILE *out);

#endif
