#include "sched_spy.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

uint64_t sched_spy_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void sched_spy_default_config(SchedSpyConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->threshold_ns = 1000000ULL;
    cfg->keep_alive_ns = 5000000000ULL;
    cfg->refresh_ms = 500;
    cfg->serve_port = 0;
}

int sched_spy_config_add_pid(SchedSpyConfig *cfg, pid_t pid) {
    if (cfg->pid_count >= SCHED_SPY_MAX_TARGETS) {
        return -1;
    }
    cfg->pids[cfg->pid_count++] = pid;
    return 0;
}

int sched_spy_config_add_comm(SchedSpyConfig *cfg, const char *comm) {
    if (cfg->comm_count >= SCHED_SPY_MAX_TARGETS) {
        return -1;
    }
    snprintf(cfg->comms[cfg->comm_count++], SCHED_SPY_COMM_LEN, "%s", comm);
    return 0;
}

int sched_spy_target_matches(const SchedSpyConfig *cfg, pid_t pid, const char *comm) {
    if (cfg->all) {
        return 1;
    }
    for (size_t i = 0; i < cfg->pid_count; i++) {
        if (cfg->pids[i] == pid) {
            return 1;
        }
    }
    for (size_t i = 0; i < cfg->comm_count; i++) {
        if (strncmp(cfg->comms[i], comm ? comm : "", SCHED_SPY_COMM_LEN) == 0) {
            return 1;
        }
    }
    return 0;
}

static int output_path_is_safe(const char *path) {
    if (!path) {
        return 1;
    }
    if (strstr(path, "..")) {
        return 0;
    }
    if (strncmp(path, "/sys", 4) == 0 || strncmp(path, "/proc", 5) == 0 || strncmp(path, "/dev", 4) == 0) {
        return 0;
    }
    return 1;
}

int sched_spy_validate_config(const SchedSpyConfig *cfg, char *err, size_t err_len) {
    if (!cfg->all && cfg->pid_count == 0 && cfg->comm_count == 0) {
        snprintf(err, err_len, "select at least one target with --pid, --comm, or --all");
        return -1;
    }
#ifndef _WIN32
    for (size_t i = 0; i < cfg->pid_count; i++) {
        if (kill(cfg->pids[i], 0) != 0 && errno == ESRCH) {
            snprintf(err, err_len, "PID %d not found", (int)cfg->pids[i]);
            return -1;
        }
    }
#endif
    if (!output_path_is_safe(cfg->json_path) || !output_path_is_safe(cfg->stats_json_path)) {
        snprintf(err, err_len, "refusing unsafe output path");
        return -1;
    }
    return 0;
}
