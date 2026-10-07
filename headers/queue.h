#ifndef QUEUE_H
#define QUEUE_H

#include "params.h"


typedef struct IntQueue {
    int head;
    int tail;
    int data[MAX_QUEUE];
} IntQueue;

void queue_init(IntQueue *q);
int queue_size(const IntQueue *q);
int queue_empty(const IntQueue *q);

int queue_push(IntQueue *q, int value);
int queue_peek(const IntQueue *q, int *value);
int queue_pop(IntQueue *q, int *value);

#endif
