CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE
LDFLAGS ?= -pthread -lm

SRC = sched_spy.c \
      src/config.c \
      src/probe.c \
      src/tracefs_reader.c \
      src/perf_reader.c \
      src/correlator.c \
      src/metrics.c \
      src/terminal.c \
      src/output_json.c \
      src/http_metrics.c \
      src/ring_queue.c

.PHONY: all clean test debug

all: sched-spy

sched-spy: $(SRC) sched_spy.h
	$(CC) $(CFLAGS) -I. $(SRC) -o $@ $(LDFLAGS)

debug: CFLAGS += -g -fsanitize=address,undefined -fno-omit-frame-pointer
debug: LDFLAGS += -fsanitize=address,undefined
debug: sched-spy-debug

sched-spy-debug: $(SRC) sched_spy.h
	$(CC) $(CFLAGS) -I. $(SRC) -o $@ $(LDFLAGS)

test:
	sh test/run_tests.sh

clean:
	rm -f sched-spy sched-spy-debug test/test_correlator test/test_metrics test/test_ring_queue test/test_tracefs_parser
