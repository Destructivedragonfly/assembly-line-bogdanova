#ifndef PROCESS_H
#define PROCESS_H

#include "members.h"
#include "queue.h"
#include "events.h"

/* Состояние полного процесса */
typedef struct Process {
    Part parts[MAX_PARTS]; /* массив всех деталей */
    int part_count; /* сколько создано деталей */

    Machine machines[NUM_MACHINES];
    Inspector inspectors[NUM_INSPECTORS];
    AssemblyPost assembly_posts[NUM_ASM];
    Robot robots[NUM_ROBOTS];
    Buffer buffers[NUM_PART_TYPES];

    struct EventQueue *events; /* очередь будущих событий */
    struct IntQueue *robot_queue; /* очередь деталей, ждущих робота */

    double now; /* текущее время */

    int products_started; /* сколько изделий уже поставлено на сборку */
    int products_done; /* сколько изделий полностью выпущено */
    int next_product_num; /* номер следующего изделия */

    int next_machine[NUM_MACHINE_TYPES]; /* round-robin по станкам каждого типа */
    int next_inspector; /* round-robin по контролёрам */

    unsigned seed;
    int total_events; /* кол-во обработанных событий */
} Process;

void process_init(Process *proc);
void process_run(Process *proc);
void process_print_stats(const Process *proc);

#endif
