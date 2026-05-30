#include "src/ring_queue.h"

#include <assert.h>
#include <stdio.h>

static void test_wraparound(void) {
    RingQueue q;
    ring_queue_init(&q);
    RawEvent ev = {.type = RAW_SCHED_WAKEUP, .pid = 1};
    for (int i = 0; i < RING_QUEUE_SIZE - 1; i++) {
        ev.pid = i;
        assert(ring_queue_push(&q, &ev) == 1);
    }
    assert(ring_queue_push(&q, &ev) == 0);
    for (int i = 0; i < RING_QUEUE_SIZE - 1; i++) {
        RawEvent out;
        assert(ring_queue_pop(&q, &out) == 1);
        assert(out.pid == i);
    }
    assert(ring_queue_pop(&q, &ev) == 0);
}

int main(void) {
    test_wraparound();
    puts("PASS: test_ring_queue");
    return 0;
}
