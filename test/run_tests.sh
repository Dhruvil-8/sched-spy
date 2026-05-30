#!/bin/sh
set -eu

cc=${CC:-gcc}
cflags="${CFLAGS:--Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE}"

$cc $cflags -I. test/test_tracefs_parser.c src/tracefs_reader.c -o test/test_tracefs_parser
./test/test_tracefs_parser

$cc $cflags -I. test/test_ring_queue.c src/ring_queue.c -o test/test_ring_queue
./test/test_ring_queue

$cc $cflags -I. test/test_metrics.c src/metrics.c -o test/test_metrics -pthread -lm
./test/test_metrics

$cc $cflags -I. test/test_correlator.c src/config.c src/correlator.c src/metrics.c src/output_json.c -o test/test_correlator -pthread -lm
./test/test_correlator
