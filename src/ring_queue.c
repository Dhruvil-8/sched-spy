#include "ring_queue.h"

void ring_queue_init(RingQueue *q) {
    atomic_store(&q->head, 0);
    atomic_store(&q->tail, 0);
    atomic_store(&q->dropped, 0);
}

int ring_queue_push(RingQueue *q, const RawEvent *ev) {
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t next = (head + 1) % RING_QUEUE_SIZE;
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (next == tail) {
        atomic_fetch_add_explicit(&q->dropped, 1, memory_order_relaxed);
        return 0;
    }

    q->entries[head] = *ev;
    atomic_store_explicit(&q->head, next, memory_order_release);
    return 1;
}

int ring_queue_pop(RingQueue *q, RawEvent *ev) {
    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    if (tail == head) {
        return 0;
    }

    *ev = q->entries[tail];
    atomic_store_explicit(&q->tail, (tail + 1) % RING_QUEUE_SIZE, memory_order_release);
    return 1;
}

uint64_t ring_queue_dropped(const RingQueue *q) {
    return atomic_load_explicit(&q->dropped, memory_order_relaxed);
}
