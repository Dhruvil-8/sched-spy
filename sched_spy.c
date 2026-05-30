#include "sched_spy.h"

#include "src/correlator.h"
#include "src/http_metrics.h"
#include "src/metrics.h"
#include "src/perf_reader.h"
#include "src/probe.h"
#include "src/terminal.h"
#include "src/tracefs_reader.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_dump_requested = 0;
static volatile sig_atomic_t g_reset_requested = 0;
static char g_tracefs_path[256];

static void on_signal(int sig) {
    if (sig == SIGUSR1) {
        g_dump_requested = 1;
    } else if (sig == SIGUSR2) {
        g_reset_requested = 1;
    } else {
        g_running = 0;
    }
}

static void cleanup_tracefs(void) {
    if (g_tracefs_path[0]) {
        char err[256];
        tracefs_enable_events(g_tracefs_path, 0, err, sizeof(err));
    }
}

static void usage(FILE *out) {
    fprintf(out,
            "USAGE:\n"
            "  sched-spy [OPTIONS]\n\n"
            "TARGET SELECTION:\n"
            "  --pid PID              Watch a PID, repeatable\n"
            "  --comm NAME            Watch a comm name, repeatable\n"
            "  --all                  Watch all processes\n\n"
            "FILTERING:\n"
            "  --threshold MS         Spike threshold in milliseconds (default 1.0)\n"
            "  --min-latency NS       Minimum latency to record\n\n"
            "OUTPUT:\n"
            "  --json FILE            Write spike events as NDJSON\n"
            "  --stats-json FILE      Write final stats JSON on exit\n"
            "  --no-terminal          Suppress live terminal output\n"
            "  --verbose              Print probe details\n\n"
            "SERVER:\n"
            "  --serve PORT           Expose Prometheus text metrics on /metrics\n\n"
            "BEHAVIOR:\n"
            "  --duration SEC         Run for SEC seconds\n"
            "  --force-tracefs        Use tracefs backend\n"
            "  --refresh MS           Terminal refresh interval (default 500)\n"
            "  --keep-alive SEC       Stale wakeup timeout (default 5)\n"
            "  --help                 Show this help\n");
}

static int parse_args(int argc, char **argv, SchedSpyConfig *cfg) {
    static const struct option opts[] = {
        {"pid", required_argument, NULL, 'p'},
        {"comm", required_argument, NULL, 'c'},
        {"all", no_argument, NULL, 'a'},
        {"threshold", required_argument, NULL, 't'},
        {"min-latency", required_argument, NULL, 1000},
        {"json", required_argument, NULL, 'j'},
        {"stats-json", required_argument, NULL, 1001},
        {"no-terminal", no_argument, NULL, 'n'},
        {"verbose", no_argument, NULL, 'v'},
        {"serve", required_argument, NULL, 's'},
        {"duration", required_argument, NULL, 'd'},
        {"force-tracefs", no_argument, NULL, 1002},
        {"refresh", required_argument, NULL, 'r'},
        {"keep-alive", required_argument, NULL, 1003},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int ch;
    while ((ch = getopt_long(argc, argv, "p:c:at:j:nvs:d:r:h", opts, NULL)) != -1) {
        switch (ch) {
        case 'p':
            if (sched_spy_config_add_pid(cfg, (pid_t)strtol(optarg, NULL, 10)) != 0) {
                return -1;
            }
            break;
        case 'c':
            if (sched_spy_config_add_comm(cfg, optarg) != 0) {
                return -1;
            }
            break;
        case 'a':
            cfg->all = 1;
            break;
        case 't':
            cfg->threshold_ns = (uint64_t)(strtod(optarg, NULL) * 1000000.0);
            break;
        case 'j':
            cfg->json_path = optarg;
            break;
        case 'n':
            cfg->no_terminal = 1;
            break;
        case 'v':
            cfg->verbose = 1;
            break;
        case 's':
            cfg->serve_port = atoi(optarg);
            break;
        case 'd':
            cfg->duration_sec = atoi(optarg);
            break;
        case 'r':
            cfg->refresh_ms = atoi(optarg);
            break;
        case 'h':
            usage(stdout);
            exit(0);
        case 1000:
            cfg->min_latency_ns = (uint64_t)strtoull(optarg, NULL, 10);
            break;
        case 1001:
            cfg->stats_json_path = optarg;
            break;
        case 1002:
            cfg->force_tracefs = 1;
            break;
        case 1003:
            cfg->keep_alive_ns = (uint64_t)(strtod(optarg, NULL) * 1000000000.0);
            break;
        default:
            usage(stderr);
            return -1;
        }
    }
    return 0;
}

static void install_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

int main(int argc, char **argv) {
    SchedSpyConfig cfg;
    sched_spy_default_config(&cfg);
    if (parse_args(argc, argv, &cfg) != 0) {
        return 2;
    }

    char err[512];
    if (sched_spy_validate_config(&cfg, err, sizeof(err)) != 0) {
        fprintf(stderr, "sched-spy: %s\n", err);
        usage(stderr);
        return 2;
    }

    ProbeResult probe;
    if (probe_environment(&probe) != 0) {
        fprintf(stderr, "sched-spy: no readable tracefs mount found.\n");
        fprintf(stderr, "  Try: sudo mount -t tracefs tracefs /sys/kernel/tracing\n");
        return 1;
    }
    if (cfg.verbose) {
        probe_print(&probe, stderr);
        if (!cfg.force_tracefs && !perf_reader_available(&probe)) {
            fprintf(stderr, "%s\n", perf_reader_status());
        }
    }

    MetricsStore metrics;
    if (metrics_init(&metrics, cfg.threshold_ns) != 0) {
        fprintf(stderr, "sched-spy: metrics initialization failed\n");
        return 1;
    }

    JsonWriter json;
    if (json_writer_open(&json, cfg.json_path) != 0) {
        fprintf(stderr, "sched-spy: could not open JSON output %s: %s\n", cfg.json_path, strerror(errno));
        metrics_destroy(&metrics);
        return 1;
    }

    install_signal_handlers();
    atexit(cleanup_tracefs);

    snprintf(g_tracefs_path, sizeof(g_tracefs_path), "%s", probe.tracefs_path);
    if (tracefs_enable_events(g_tracefs_path, 1, err, sizeof(err)) != 0) {
        fprintf(stderr, "sched-spy: cannot enable tracepoints: %s\n", err);
        json_writer_close(&json);
        metrics_destroy(&metrics);
        return 1;
    }

    TracefsReader reader;
    if (tracefs_reader_open(&reader, g_tracefs_path, err, sizeof(err)) != 0) {
        fprintf(stderr, "sched-spy: cannot open trace_pipe: %s\n", err);
        cleanup_tracefs();
        json_writer_close(&json);
        metrics_destroy(&metrics);
        return 1;
    }

    HttpMetricsServer http;
    memset(&http, 0, sizeof(http));
    http.server_fd = -1;
    if (cfg.serve_port > 0 && http_metrics_start(&http, cfg.serve_port, &g_running, &metrics) != 0) {
        fprintf(stderr, "sched-spy: could not start HTTP server on port %d: %s\n", cfg.serve_port, strerror(errno));
    }

    Correlator correlator;
    correlator_init(&correlator, &cfg, &metrics, &json);

    uint64_t start_ns = sched_spy_now_ns();
    uint64_t last_render_ns = 0;
    uint64_t last_cleanup_ns = start_ns;

    while (g_running) {
        RawEvent ev;
        int rc = tracefs_reader_next(&reader, &ev, 100);
        if (rc < 0) {
            fprintf(stderr, "sched-spy: trace_pipe read failed: %s\n", strerror(errno));
            break;
        }
        if (rc > 0) {
            correlator_process(&correlator, &ev);
        }

        uint64_t now = sched_spy_now_ns();
        if (now - last_cleanup_ns > 1000000000ULL) {
            correlator_cleanup_stale(&correlator, now);
            last_cleanup_ns = now;
        }
        if (!cfg.no_terminal && now - last_render_ns >= (uint64_t)cfg.refresh_ms * 1000000ULL) {
            terminal_render(&metrics, &cfg);
            last_render_ns = now;
        }
        if (cfg.duration_sec > 0 && now - start_ns >= (uint64_t)cfg.duration_sec * 1000000000ULL) {
            break;
        }
        if (g_dump_requested) {
            metrics_write_json(&metrics, stdout);
            g_dump_requested = 0;
        }
        if (g_reset_requested) {
            metrics_reset(&metrics, cfg.threshold_ns);
            correlator_init(&correlator, &cfg, &metrics, &json);
            g_reset_requested = 0;
        }
    }

    if (!cfg.no_terminal) {
        printf("\nfinal statistics:\n");
        metrics_print_text(&metrics, stdout);
    }
    if (json_write_stats_file(cfg.stats_json_path, &metrics) != 0) {
        fprintf(stderr, "sched-spy: failed writing stats JSON %s: %s\n", cfg.stats_json_path, strerror(errno));
    }

    http_metrics_stop(&http);
    tracefs_reader_close(&reader);
    cleanup_tracefs();
    g_tracefs_path[0] = '\0';
    json_writer_close(&json);
    metrics_destroy(&metrics);
    return 0;
}
