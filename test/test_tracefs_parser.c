#include "src/tracefs_reader.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_parse_sched_switch(void) {
    const char *line =
        "    nginx-1234  [002] d...  1234567.890123: sched_switch: "
        "prev_comm=nginx prev_pid=1234 prev_prio=120 prev_state=S "
        "==> next_comm=sshd next_pid=5678 next_prio=120";

    RawEvent ev;
    assert(parse_trace_line(line, &ev) == 0);
    assert(ev.type == RAW_SCHED_SWITCH);
    assert(ev.cpu == 2);
    assert(ev.prev_pid == 1234);
    assert(ev.next_pid == 5678);
    assert(strcmp(ev.prev_comm, "nginx") == 0);
    assert(strcmp(ev.next_comm, "sshd") == 0);
}

static void test_parse_sched_wakeup(void) {
    const char *line =
        "     sshd-5678  [003] d...  1234568.000001: sched_wakeup: "
        "comm=sshd pid=5678 prio=120 success=1 target_cpu=3";

    RawEvent ev;
    assert(parse_trace_line(line, &ev) == 0);
    assert(ev.type == RAW_SCHED_WAKEUP);
    assert(ev.cpu == 3);
    assert(ev.pid == 5678);
    assert(ev.target_cpu == 3);
    assert(strcmp(ev.comm, "sshd") == 0);
}

static void test_parse_migrate(void) {
    const char *line =
        "     sshd-5678  [000] d...  1234568.000002: sched_migrate_task: "
        "comm=sshd pid=5678 prio=120 orig_cpu=0 dest_cpu=1";

    RawEvent ev;
    assert(parse_trace_line(line, &ev) == 0);
    assert(ev.type == RAW_SCHED_MIGRATE);
    assert(ev.pid == 5678);
    assert(ev.orig_cpu == 0);
    assert(ev.dest_cpu == 1);
}

int main(void) {
    test_parse_sched_switch();
    test_parse_sched_wakeup();
    test_parse_migrate();
    puts("PASS: test_tracefs_parser");
    return 0;
}
