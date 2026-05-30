#include "perf_reader.h"

int perf_reader_available(const ProbeResult *probe) {
    (void)probe;
    return 0;
}

const char *perf_reader_status(void) {
    return "perf_event_open backend is probed but not implemented; using tracefs";
}
