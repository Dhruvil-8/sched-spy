# sched-spy

`sched-spy` measures Linux scheduler latency: the time between a task becoming runnable and the scheduler actually running it.

This implementation is intentionally dependency-light C. It currently ships a complete tracefs reader, parser, correlator, metrics store, terminal view, JSON output, a small `/metrics` HTTP endpoint, and unit tests. `perf_event_open` probing is present, but the binary perf ring-buffer reader falls back to tracefs until that backend is completed.

## Development

The project ideation and planning documented in sched_spy_plan.md is done by Claude Sonnet 4.6. The full codebase was written by ChatGPT Codex 5.5; the project was developed to test LLM low-level model capability out of curiosity.

## Build

```sh
make
```

## Usage

At least one target selector is required:

```sh
sudo ./sched-spy --pid 1234
sudo ./sched-spy --comm nginx --threshold 2
sudo ./sched-spy --all --duration 10 --stats-json /tmp/sched_stats.json
sudo ./sched-spy --pid 1234 --json /tmp/spikes.ndjson --no-terminal
sudo ./sched-spy --pid 1234 --serve 9090
```

Useful options:

```text
--pid PID              Watch a PID, repeatable
--comm NAME            Watch Linux comm name, repeatable and truncated to 15 chars
--all                  Watch all processes
--threshold MS         Spike threshold in milliseconds, default 1.0
--min-latency NS       Minimum latency to record, default 0
--duration SEC         Stop after SEC seconds
--json FILE            Write spike events as NDJSON
--stats-json FILE      Write final aggregate stats as JSON
--no-terminal          Disable live terminal output
--verbose              Print environment and reader details
--force-tracefs        Use tracefs backend
--serve PORT           Serve metrics JSON on /metrics
```

## Requirements

Linux with tracefs mounted at `/sys/kernel/tracing` or `/sys/kernel/debug/tracing`. Reading tracefs and enabling scheduler tracepoints usually requires root.

`sched-spy` enables these tracepoints while it runs:

- `sched:sched_wakeup`
- `sched:sched_wakeup_new`
- `sched:sched_switch`
- `sched:sched_migrate_task`

They are disabled during normal shutdown and signal handling.

## Test

```sh
make test
```
