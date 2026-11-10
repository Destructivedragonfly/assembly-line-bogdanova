#ifndef PROCESS_H
#define PROCESS_H

#include <signal.h>
#include <time.h>

#include "link.h"
#include "members.h"


/* Состояние полного процесса */
typedef struct Process {
    Part parts[MAX_PARTS]; /* массив всех деталей */
    int part_count; /* сколько создано деталей */

    Machine machines[NUM_MACHINES];
    Inspector inspectors[NUM_INSPECTORS];
    AssemblyPost assembly_posts[NUM_ASM];
    Robot robots[NUM_ROBOTS];
    Buffer buffers[NUM_PART_TYPES];

    IntQueue robot_queue; /* очередь деталей, ждущих робота */

    double now; /* текущее модельное время */
    struct timespec start_time; /* реальный момент начала моделирования */
    int delay_ms; /* реальных миллисекунд на единицу модельного времени */

    int pending; /* сколько занятий ещё впереди или идёт */

    int products_started; /* сколько изделий уже поставлено на сборку */
    int products_done; /* сколько изделий полностью выпущено */
    int next_product_num; /* номер следующего изделия */

    int next_machine[NUM_MACHINE_TYPES]; /* round-robin по станкам каждого типа */
    int next_inspector; /* round-robin по контролёрам */

    unsigned seed;
    int total_events; /* кол-во завершённых занятий (бывших событий) */

    int stop;
} Process;

void process_init(Process *proc, unsigned seed, int delay_ms);
void process_run(Process *proc, Link *link, volatile sig_atomic_t *interrupted);
void process_stop_workers(Process *proc, Link *link);
void process_print_stats(const Process *proc);

#endif