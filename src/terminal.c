#include "terminal.h"

#include <inttypes.h>
#include <stdio.h>

static void print_targets(const SchedSpyConfig *cfg) {
    if (cfg->all) {
        printf("targets=all");
        return;
    }
    printf("targets=");
    for (size_t i = 0; i < cfg->pid_count; i++) {
        printf("%s%d", i ? "," : "", (int)cfg->pids[i]);
    }
    for (size_t i = 0; i < cfg->comm_count; i++) {
        printf("%s%s", (cfg->pid_count || i) ? "," : "", cfg->comms[i]);
    }
}

void terminal_render(MetricsStore *store, const SchedSpyConfig *cfg) {
    printf("\033[H\033[J");
    printf("sched-spy  ");
    print_targets(cfg);
    printf("  threshold=%.3fms\n\n", (double)cfg->threshold_ns / 1000000.0);
    metrics_print_text(store, stdout);
    fflush(stdout);
}
