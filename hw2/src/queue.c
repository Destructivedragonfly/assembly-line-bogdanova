#include <string.h>

#include "headers/queue.h"


void queue_init(IntQueue *q) {
    q->head = 0;
    q->tail = 0;
}

int queue_size(const IntQueue *q) {
    return q->tail - q->head;
}

int queue_empty(const IntQueue *q) {
    return q->head >= q->tail;
}

int queue_push(IntQueue *q, int val) {
    if (q->tail >= MAX_QUEUE) {
        /* сдвигаем влево */
        if (q->head == 0) {
            return 0;
        }
        int n = q->tail - q->head;
        memmove(&q->data[0], &q->data[q->head], (size_t)n * sizeof(int));
        q->head = 0;
        q->tail = n;
    }
    q->data[q->tail++] = val;
    return 1;
}

int queue_peek(const IntQueue *q, int *val) {
    if (queue_empty(q)) {
        return 0;
    }
    *val = q->data[q->head];
    return 1;
}

int queue_pop(IntQueue *q, int *val) {
    if (queue_empty(q)) {
        return 0;
    }
    *val = q->data[q->head++];
    return 1;
}
