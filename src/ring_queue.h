#ifndef SCHED_SPY_RING_QUEUE_H
#define SCHED_SPY_RING_QUEUE_H

#include "sched_spy.h"
#include <stdatomic.h>
#include <stddef.h>

#define RING_QUEUE_SIZE 4096

typedef struct {
    RawEvent entries[RING_QUEUE_SIZE];
    atomic_size_t head;
    atomic_size_t tail;
    atomic_uint_fast64_t dropped;
} RingQueue;

void ring_queue_init(RingQueue *q);
int ring_queue_push(RingQueue *q, const RawEvent *ev);
int ring_queue_pop(RingQueue *q, RawEvent *ev);
uint64_t ring_queue_dropped(const RingQueue *q);

#endif
