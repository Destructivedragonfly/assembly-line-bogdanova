#ifndef MEMBERS_H
#define MEMBERS_H

#include "params.h"
#include "queue.h"


/* Тип места транспортировки */
typedef enum {
    DEST_NONE,
    DEST_MACHINE,
    DEST_INSPECTOR
} DestinationType;

/* Состояние детали */
typedef enum {
    ST_NOT_CREATED,
    ST_CREATED,
    ST_WAIT_ROBOT,
    ST_IN_ROBOT,
    ST_MACHINE_QUEUE,
    ST_IN_MACHINE,
    ST_CONTROL_QUEUE,
    ST_IN_CONTROL,
    ST_WAIT_BUFFER,
    ST_IN_BUFFER,
    ST_IN_ASSEMBLY,
    ST_DONE,
    ST_SCRAPPED
} PartState;

/* Деталь */
typedef struct {
    int id;
    int ptype;
    int oper_idx; /* индекс текущей операции маршрута */

    PartState state;

    DestinationType dest_type;
    int dest_id;
} Part;

/* Робот */
typedef struct {
    int id;
    int busy;
    int part_id;

    DestinationType dest_type;
    int dest_id;
} Robot;

/* Станок */
typedef struct {
    int id;
    int busy;
    int part_id;

    IntQueue queue; /* очередь id деталей к станку */
} Machine;

/* Контролёр */
typedef struct {
    int id;
    int busy;
    int part_id;
    int blocked_part; /* проверенная, но не уместившаяся в накопитель деталь (-1 - нет вообще) */

    IntQueue queue; /* очередь id деталей на контроль */
} Inspector;

/* Пост сборки */
typedef struct {
    int id;
    int busy;

    int product_idx;
    int parts_used[NUM_PART_TYPES]; /* детали текущего изделия */
} AssemblyPost;

/* Промежуточный накопитель готовых деталей одного типа */
typedef struct {
    int ptype;
    int count;
    int cap;

    int items[BUFFER_CAP];
} Buffer;

#endif
