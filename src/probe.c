#include "probe.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/perf_event.h>
#include <sys/syscall.h>
#endif

static int read_first_int(const char *path, int fallback) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return fallback;
    }
    int v = fallback;
    if (fscanf(f, "%d", &v) != 1) {
        v = fallback;
    }
    fclose(f);
    return v;
}

static void detect_tracefs(ProbeResult *r) {
    const char *candidates[] = {
        "/sys/kernel/tracing",
        "/sys/kernel/debug/tracing",
        NULL
    };

    for (int i = 0; candidates[i]; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/trace_pipe", candidates[i]);
        if (access(path, R_OK) == 0) {
            snprintf(r->tracefs_path, sizeof(r->tracefs_path), "%s", candidates[i]);
            return;
        }
    }

    FILE *f = fopen("/proc/mounts", "r");
    if (!f) {
        return;
    }
    char dev[128], mount[256], type[64], opts[256];
    while (fscanf(f, "%127s %255s %63s %255s %*d %*d", dev, mount, type, opts) == 4) {
        if (strcmp(type, "tracefs") == 0) {
            snprintf(r->tracefs_path, sizeof(r->tracefs_path), "%s", mount);
            break;
        }
    }
    fclose(f);
}

static void detect_kernel(ProbeResult *r) {
    struct utsname u;
    if (uname(&u) == 0) {
        snprintf(r->kernel_release, sizeof(r->kernel_release), "%s", u.release);
        sscanf(u.release, "%d.%d.%d", &r->major, &r->minor, &r->patch);
    }
}

static void detect_perf(ProbeResult *r) {
    r->perf_event_paranoid = read_first_int("/proc/sys/kernel/perf_event_paranoid", 999);
#ifdef __linux__
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.type = PERF_TYPE_SOFTWARE;
    attr.size = sizeof(attr);
    attr.config = PERF_COUNT_SW_CPU_CLOCK;
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;

    int fd = (int)syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
    if (fd >= 0) {
        r->perf_event_open_ok = 1;
        close(fd);
    } else {
        r->perf_event_open_ok = 0;
        snprintf(r->perf_error, sizeof(r->perf_error), "%s", strerror(errno));
    }
#else
    r->perf_event_open_ok = 0;
    snprintf(r->perf_error, sizeof(r->perf_error), "not linux");
#endif
}

int probe_environment(ProbeResult *result) {
    memset(result, 0, sizeof(*result));
    detect_kernel(result);
    detect_tracefs(result);
    detect_perf(result);
    return result->tracefs_path[0] ? 0 : -1;
}

void probe_print(const ProbeResult *r, FILE *out) {
    fprintf(out, "kernel: %s\n", r->kernel_release[0] ? r->kernel_release : "unknown");
    fprintf(out, "tracefs: %s\n", r->tracefs_path[0] ? r->tracefs_path : "unavailable");
    fprintf(out, "perf_event_paranoid: %d\n", r->perf_event_paranoid);
    fprintf(out, "perf_event_open dry-run: %s", r->perf_event_open_ok ? "ok" : "unavailable");
    if (!r->perf_event_open_ok && r->perf_error[0]) {
        fprintf(out, " (%s)", r->perf_error);
    }
    fputc('\n', out);
}
