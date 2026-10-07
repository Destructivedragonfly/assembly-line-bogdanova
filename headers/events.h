#ifndef EVENTS_H
#define EVENTS_H

#include "params.h"


typedef enum {
    EV_CREATE_PART, /* object_id = id детали */
    EV_ROBOT_DROP, /* object_id = id робота */
    EV_OPER_DONE, /* object_id = id станка */
    EV_CONTROL_DONE, /* object_id = id контролёра */
    EV_ASM_DONE /* object_id = id сборочного поста */
} EventType;

typedef struct {
    double time;
    EventType type; /* тип события */
    int object_id; /* объект, с которым работает событие (смысл зависит от type) */
} Event;

typedef struct EventQueue {
    int size;
    Event data[MAX_EVENTS];
} EventQueue;

void event_init(EventQueue *queue);
int event_empty(const EventQueue *queue);

/* Вставляет событие с сохранением порядка по времени. Возвращает 0 при переполнении. */
int event_new(
    double time,
    EventQueue *queue,
    EventType type,
    int object_id
);

/* Извлекает ближайшее событие. Возвращает 0, если очередь пуста. */
int event_pop(EventQueue *queue, Event *event);

#endif
