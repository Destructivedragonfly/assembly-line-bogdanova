#include <string.h>

#include "headers/events.h"


void event_init(EventQueue *queue) {
    queue->size = 0;
}

int event_empty(const EventQueue *queue) {
    return queue->size == 0;
}

int event_new(
    double time,
    EventQueue *queue,
    EventType type,
    int object_id
) {
    if (queue->size >= MAX_EVENTS) {
        return 0;
    }

    int i = queue->size++;

    /* вставка в отсортированный по времени массив, события с равным
       временем остаются в порядке добавления */
    while (i > 0 && queue->data[i - 1].time > time) {
        queue->data[i] = queue->data[i - 1];
        --i;
    }

    queue->data[i].time = time;
    queue->data[i].type = type;
    queue->data[i].object_id = object_id;

    return 1;
}

int event_pop(EventQueue *queue, Event *event)
{
    if (event_empty(queue)) {
        return 0;
    }

    *event = queue->data[0];
    memmove( /* сдвигает все элементы data на 1 влево */
        &queue->data[0],
        &queue->data[1],
        (size_t)(queue->size - 1) * sizeof(Event)
    );

    --queue->size;

    return 1;
}
