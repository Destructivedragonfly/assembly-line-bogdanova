#ifndef PARAMS_H
#define PARAMS_H


/* План */
#define PLAN 4 /* план выпуска изделий */
#define EXTRA_PARTS 4 /* запас деталей каждого типа сверх плана (на случай брака) */

/* Максимальные значения массивов */
#define MAX_PARTS 500 /* макс число созданных деталей */
#define MAX_QUEUE 500 /* макс размер очереди (к станку, контролёру, роботу) */
#define MAX_EVENTS 50000 /* макс длина очереди будущих событий */

/* Вместимость накопителя для каждого типа детали */
#define BUFFER_CAP 2

/* Вероятности */
#define PROB_DEFECT 0.3 /* вероятность брака */
#define PROB_REDO 0.6 /* вероятность того, что брак пойдёт на переделку (иначе списание) */

/* Типы деталей, ресурсы */
#define NUM_PART_TYPES 2 /* видов деталей в изделии */
#define NUM_MACHINE_TYPES 2 /* видов станков (значения в machine_type / route) */
#define NUM_MACHINES 3 /* всего станков */
#define NUM_INSPECTORS 2 /* всего постов контроля */
#define NUM_ASM 1 /* всего постов сборки */
#define NUM_ROBOTS 2 /* всего транспортных роботов */

/* Длительности */
#define CONTROL_TIME 1.5 /* время контроля детали */
#define ASM_TIME 4.0 /* время сборки изделия */
#define ROBOT_TRAVEL 1.0 /* время перемещения робота */

/* Типы отдельных станков: станки 0 и 1 - типа 0, станок 2 - типа 1 */
static const int machine_type[NUM_MACHINES] = {0, 0, 1};

/* Маршруты */
#define MAX_ROUTE_LEN 4 /* макс длина маршрута */

static const int route_len[NUM_PART_TYPES] = {2, 1}; /* деталь типа 0 - 2 операции, типа 1 - 1 операция */

static const int route[NUM_PART_TYPES][MAX_ROUTE_LEN] = {
    {0, 1, -1, -1},
    {1, -1, -1, -1}
}; /* route[0][0] - первая операция детали типа 0 выполняется на станке типа 0 */

static const double oper_time[NUM_PART_TYPES][MAX_ROUTE_LEN] = {
    {2.0, 3.0, 0.0, 0.0},
    {2.5, 0.0, 0.0, 0.0}
}; /* длительность соответствующих операций */

#endif
