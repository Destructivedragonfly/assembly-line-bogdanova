#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "process.h"


/* Модельное время */
static double model_now(const Process *proc)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    double ms = (double)(t.tv_sec - proc->start_time.tv_sec) * 1000.0 +
                (double)(t.tv_nsec - proc->start_time.tv_nsec) / 1e6;
    /* реальные миллисекунды от старта, делённые на миллисекунды на единицу */

    return ms / proc->delay_ms;
}

/* Чтобы начать занятие, будим процесс участника, он поработает нужное время */
static void start_activity(Process *proc, Link *link, int worker_id, double duration)
{
    proc->pending++;
    link_send_job(link, worker_id, (long)(duration * proc->delay_ms));
}

/* Выбрать станок */
static int choose_machine(Process *proc, int cur_type)
{
    int possible[NUM_MACHINES];
    int count = 0;

    /* чтобы было равномерное распределение, выбираем из станков нужного типа какой-то конкретный
    методом round-robin */
    for (int i = 0; i < NUM_MACHINES; ++i) {
        if (machine_type[i] == cur_type) possible[count++] = i;
    }
    if (count == 0) return -1;

    int idx = proc->next_machine[cur_type] % count;
    proc->next_machine[cur_type]++;
    return possible[idx];
}

/* Выбрать контролёра */
static int choose_inspector(Process *proc)
{
    int idx = proc->next_inspector % NUM_INSPECTORS;
    proc->next_inspector++;
    return idx;
}

/* Попытка назначить робота на деталь */
static int try_assign_robot(Process *proc, Link *link, Part *part)
{
    for (int i = 0; i < NUM_ROBOTS; ++i) {
        Robot *robot = &proc->robots[i];
        if (robot->busy) continue;

        robot->busy = 1;
        robot->part_id = part->id;
        robot->dest_type = part->dest_type;
        robot->dest_id = part->dest_id;
        part->state = ST_IN_ROBOT;

        dprintf(1, "%6.2f robot %d takes part %d\n", proc->now, robot->id, part->id);
        /* планируем время доставки */
        start_activity(proc, link, WORKER_ROBOT + i, ROBOT_TRAVEL);
        return 1; /* свободный робот нашёлся */
    }
    return 0; /* свободных нет */
}

/* Вызвать робота или встать в очередь к роботам */
static void request_robot(Process *proc, Link *link, Part *part)
{
    part->state = ST_WAIT_ROBOT;
    if (try_assign_robot(proc, link, part)) return;

    if (!queue_push(&proc->robot_queue, part->id)) {
        dprintf(2, "error: robot queue full\n");
        proc->stop = 1;
        return;
    }
    dprintf(1, "%6.2f part %d waits robot\n", proc->now, part->id);
}

/* Деталь доставлена: в очередь станка или контролёра */
static void part_arrive(Process *proc, Part *part,
                        DestinationType dest_type, int dest_id)
{
    if (dest_type == DEST_MACHINE) {
        Machine *machine = &proc->machines[dest_id];
        
        if (!queue_push(&machine->queue, part->id)) {
            dprintf(2, "error: machine %d queue full\n", dest_id);
            proc->stop = 1;

            return;
        }
        part->state = ST_MACHINE_QUEUE;
        dprintf(1, "%6.2f part %d -> machine %d\n", proc->now, part->id, dest_id);
    } 
    else if (dest_type == DEST_INSPECTOR) {
        Inspector *inspector = &proc->inspectors[dest_id];
        if (!queue_push(&inspector->queue, part->id)) {
            dprintf(2, "error: inspector %d queue full\n", dest_id);
            proc->stop = 1;

            return;
        }
        part->state = ST_CONTROL_QUEUE;
        dprintf(1, "%6.2f part %d -> inspector %d\n", proc->now, part->id, dest_id);
    }
}

/* Робот довёз деталь */
static void handle_robot_drop(Process *proc, Link *link, int id)
{
    Robot *robot = &proc->robots[id];
    Part *part = &proc->parts[robot->part_id];

    part_arrive(proc, part, robot->dest_type, robot->dest_id);

    robot->busy = 0;
    robot->part_id = -1;
    robot->dest_type = DEST_NONE;
    robot->dest_id = -1;

    /* освободившийся робот сразу берёт ожидающую деталь */
    int part_id;
    if (queue_pop(&proc->robot_queue, &part_id)) {
        try_assign_robot(proc, link, &proc->parts[part_id]);
    }
}

/* Положить годную деталь в накопитель, 0 - накопитель полон */
static int try_place_buffer(Process *proc, Part *part)
{
    Buffer *buffer = &proc->buffers[part->ptype];
    if (buffer->count >= buffer->cap) return 0;

    buffer->items[buffer->count++] = part->id;
    part->state = ST_IN_BUFFER;
    dprintf(1, "%6.2f part %d in buffer %d\n", proc->now, part->id, part->ptype);

    return 1;
}

/* Есть ли в каждом накопителе хотя бы одна деталь */
static int kit_ready(const Process *proc)
{
    for (int i = 0; i < NUM_PART_TYPES; ++i) {
        if (proc->buffers[i].count == 0) return 0;
    }

    return 1;
}

/* Достать самую старую деталь из накопителя */
static int buffer_pop(Buffer *buffer)
{
    int part_id = buffer->items[0];
    for (int i = 1; i < buffer->count; ++i) {
        buffer->items[i - 1] = buffer->items[i];
    }
    buffer->count--;

    return part_id;
}

/* Запустить станок, если он свободен и есть деталь в очереди */
static void try_start_machine(Process *proc, Link *link, int machine_id)
{
    Machine *machine = &proc->machines[machine_id];
    int part_id;
    if (machine->busy || !queue_pop(&machine->queue, &part_id)) return;

    Part *part = &proc->parts[part_id];
    machine->busy = 1;
    machine->part_id = part_id;
    part->state = ST_IN_MACHINE;

    dprintf(1, "%6.2f machine %d starts part %d\n", proc->now, machine_id, part_id);

    start_activity(proc, link, WORKER_MACHINE + machine_id,
                   oper_time[part->ptype][part->oper_idx]);
}

/* Запустить контролёра, если он свободен и есть деталь в очереди */
static void try_start_control(Process *proc, Link *link, int inspector_id)
{
    Inspector *inspector = &proc->inspectors[inspector_id];
    int part_id;
    if (inspector->busy || inspector->blocked_part >= 0 ||
        !queue_pop(&inspector->queue, &part_id)) return;

    Part *part = &proc->parts[part_id];
    inspector->busy = 1;
    inspector->part_id = part_id;
    part->state = ST_IN_CONTROL;

    dprintf(1, "%6.2f inspector %d checks part %d\n", proc->now, inspector_id, part_id);

    start_activity(proc, link, WORKER_INSPECTOR + inspector_id, CONTROL_TIME);
}

/* Начать сборку, если пост свободен и есть полный комплект */
static int try_start_assembly(Process *proc, Link *link)
{
    for (int a = 0; a < NUM_ASM; ++a) {
        AssemblyPost *post = &proc->assembly_posts[a];
        if (post->busy || proc->products_started >= PLAN || !kit_ready(proc)) continue;

        for (int type = 0; type < NUM_PART_TYPES; ++type) {
            int part_id = buffer_pop(&proc->buffers[type]);
            proc->parts[part_id].state = ST_IN_ASSEMBLY;
            post->parts_used[type] = part_id;
        }

        post->busy = 1;
        proc->products_started++;
        post->product_idx = proc->next_product_num++;

        dprintf(1, "%6.2f assembly %d starts product %d\n",
                proc->now, a, post->product_idx);

        start_activity(proc, link, WORKER_ASSEMBLY + a, ASM_TIME);
        return 1;
    }
    return 0;
}

/* Запустить всё, что можно, пока что-то запускается */
static void try_start_all(Process *proc, Link *link)
{
    int progress = 1;
    while (progress && proc->products_done < PLAN && !proc->stop) {
        progress = 0;

        /* разблокировка контролёров */
        for (int i = 0; i < NUM_INSPECTORS; ++i) {
            Inspector *inspector = &proc->inspectors[i];
            if (inspector->blocked_part >= 0 &&
                try_place_buffer(proc, &proc->parts[inspector->blocked_part])) {
                dprintf(1, "%6.2f inspector %d unblocked\n", proc->now, i);
                inspector->blocked_part = -1;
                inspector->busy = 0;
                inspector->part_id = -1;
                progress = 1;
            }
        }

        /* свободные контролёры */
        for (int i = 0; i < NUM_INSPECTORS; ++i) {
            Inspector *inspector = &proc->inspectors[i];
            if (!inspector->busy && inspector->blocked_part < 0 &&
                !queue_empty(&inspector->queue)) {
                try_start_control(proc, link, i);
                progress = 1;
            }
        }

        /* свободные станки */
        for (int i = 0; i < NUM_MACHINES; ++i) {
            Machine *machine = &proc->machines[i];
            if (!machine->busy && !queue_empty(&machine->queue)) {
                try_start_machine(proc, link, i);
                progress = 1;
            }
        }

        /* сборка */
        if (try_start_assembly(proc, link)) progress = 1;
    }
}

/* Деталь появилась на линии */
static void handle_create(Process *proc, Link *link, int id)
{
    Part *part = &proc->parts[id];
    part->oper_idx = 0;
    part->state = ST_CREATED;

    dprintf(1, "%6.2f part %d created\n", proc->now, part->id);

    int machine_id = choose_machine(proc, route[part->ptype][0]);
    if (machine_id < 0) {
        dprintf(1, "%6.2f part %d: no machine\n", proc->now, part->id);
        part->state = ST_SCRAPPED;
        return;
    }

    part->dest_type = DEST_MACHINE;
    part->dest_id = machine_id;

    request_robot(proc, link, part);
}

/* Станок закончил операцию */
static void handle_oper_done(Process *proc, Link *link, int id)
{
    Machine *machine = &proc->machines[id];
    Part *part = &proc->parts[machine->part_id];
    dprintf(1, "%6.2f machine %d ends part %d\n", proc->now, machine->id, part->id);

    machine->busy = 0;
    machine->part_id = -1;
    part->oper_idx++;

    if (part->oper_idx < route_len[part->ptype]) {
        int machine_id = choose_machine(proc, route[part->ptype][part->oper_idx]);
        if (machine_id < 0) {
            dprintf(1, "%6.2f part %d: no machine\n", proc->now, part->id);
            part->state = ST_SCRAPPED;
            return;
        }
        part->dest_type = DEST_MACHINE;
        part->dest_id = machine_id;
    } 
    else {
        part->dest_type = DEST_INSPECTOR;
        part->dest_id = choose_inspector(proc);
    }
    
    request_robot(proc, link, part);
}

/* Контролёр закончил проверку: годна, блокировка, переделка или списание */
static void handle_control_done(Process *proc, Link *link, int id)
{
    Inspector *inspector = &proc->inspectors[id];
    Part *part = &proc->parts[inspector->part_id];

    if ((double)rand() / RAND_MAX >= PROB_DEFECT) {
        dprintf(1, "%6.2f part %d ok\n", proc->now, part->id);
        part->state = ST_WAIT_BUFFER;

        if (try_place_buffer(proc, part)) {
            inspector->busy = 0;
            inspector->part_id = -1;
        } 
        else {
            dprintf(1, "%6.2f inspector %d blocked\n", proc->now, inspector->id);
            /* контролёр остаётся занят и держит деталь */
            inspector->blocked_part = part->id;
        }
        return;
    }

    dprintf(1, "%6.2f part %d bad\n", proc->now, part->id);
    inspector->busy = 0;
    inspector->part_id = -1;

    if ((double)rand() / RAND_MAX < PROB_REDO) {
        part->oper_idx--; /* назад на последнюю операцию маршрута */
        dprintf(1, "%6.2f part %d rework\n", proc->now, part->id);
        int machine_id = choose_machine(proc, route[part->ptype][part->oper_idx]);
        
        if (machine_id < 0) {
            dprintf(1, "%6.2f part %d: no machine\n", proc->now, part->id);
            part->state = ST_SCRAPPED;
            return;
        }
        part->dest_type = DEST_MACHINE;
        part->dest_id = machine_id;
        request_robot(proc, link, part);
    } 
    else {
        dprintf(1, "%6.2f part %d scrapped\n", proc->now, part->id);
        part->state = ST_SCRAPPED;
    }
}

/* Сборка закончена, изделие выпущено */
static void handle_asm_done(Process *proc, int id)
{
    AssemblyPost *post = &proc->assembly_posts[id];
    dprintf(1, "%6.2f product %d done\n", proc->now, post->product_idx);

    for (int type = 0; type < NUM_PART_TYPES; ++type) {
        proc->parts[post->parts_used[type]].state = ST_DONE;
    }

    post->busy = 0;
    proc->products_done++;
}

/* Конец, запустить всё, что можно, и обновить число незавершённых занятий */
static void activity_done(Process *proc, Link *link)
{
    proc->total_events++;
    try_start_all(proc, link);
    proc->pending--;
}

/* Определить, какой работник завершил задачу, и позвать соответствующего handler */
static void handle_worker_done(Process *proc, Link *link, int worker_id)
{
    if (worker_id >= WORKER_MACHINE && worker_id < WORKER_INSPECTOR) {
        handle_oper_done(proc, link, worker_id - WORKER_MACHINE);
    } 
    else if (worker_id >= WORKER_INSPECTOR && worker_id < WORKER_ROBOT) {
        handle_control_done(proc, link, worker_id - WORKER_INSPECTOR);
    }
    else if (worker_id >= WORKER_ROBOT && worker_id < WORKER_ASSEMBLY) {
        handle_robot_drop(proc, link, worker_id - WORKER_ROBOT);
    }
    else if (worker_id >= WORKER_ASSEMBLY && worker_id < MAX_WORKERS) {
        handle_asm_done(proc, worker_id - WORKER_ASSEMBLY);
    }

    activity_done(proc, link);
}

/* Поставщик создаёт детали в те же моменты, что были у событий создания */
static double create_time(int part_id)
{
    return (part_id / NUM_PART_TYPES) * 0.6 +
           (part_id % NUM_PART_TYPES) * 0.25;
}

/* Перевести модельное время в секунды */
static int wait_time_ms(double model_units, int delay_ms)
{
    double ms = model_units * delay_ms;
    if (ms < 1.0) return 1;
    if (ms > 2147483000.0) return 2147483000;

    return (int)ms;
}

/* Начальное состояние модели */
void process_init(Process *proc, unsigned seed, int delay_ms)
{
    proc->part_count = 0;
    proc->now = 0.0;
    proc->delay_ms = delay_ms;
    proc->seed = seed;
    proc->products_started = 0;
    proc->products_done = 0;
    proc->next_product_num = 1;
    proc->next_inspector = 0;
    proc->total_events = 0;
    proc->pending = 0;
    proc->stop = 0;

    for (int i = 0; i < NUM_MACHINE_TYPES; ++i) proc->next_machine[i] = 0;
    queue_init(&proc->robot_queue);

    for (int i = 0; i < NUM_MACHINES; ++i) {
        Machine *machine = &proc->machines[i];
        machine->id = i;
        machine->busy = 0;
        machine->part_id = -1;
        queue_init(&machine->queue);
    }
    for (int i = 0; i < NUM_INSPECTORS; ++i) {
        Inspector *inspector = &proc->inspectors[i];
        inspector->id = i;
        inspector->busy = 0;
        inspector->part_id = -1;
        inspector->blocked_part = -1;
        queue_init(&inspector->queue);
    }
    for (int i = 0; i < NUM_ASM; ++i) {
        AssemblyPost *post = &proc->assembly_posts[i];
        post->id = i;
        post->busy = 0;
        post->product_idx = 0;
        for (int type = 0; type < NUM_PART_TYPES; ++type) post->parts_used[type] = -1;
    }
    for (int i = 0; i < NUM_ROBOTS; ++i) {
        Robot *robot = &proc->robots[i];
        robot->id = i;
        robot->busy = 0;
        robot->part_id = -1;
        robot->dest_type = DEST_NONE;
        robot->dest_id = -1;
    }
    for (int type = 0; type < NUM_PART_TYPES; ++type) {
        Buffer *buffer = &proc->buffers[type];
        buffer->ptype = type;
        buffer->count = 0;
        buffer->cap = BUFFER_CAP;
    }

    int total_per_type = PLAN + EXTRA_PARTS;
    if (total_per_type * NUM_PART_TYPES > MAX_PARTS) {
        dprintf(2, "error: too many parts\n");
        return;
    }
    for (int i = 0; i < total_per_type; ++i) {
        for (int type = 0; type < NUM_PART_TYPES; ++type) {
            int id = proc->part_count++;
            Part *part = &proc->parts[id];
            part->id = id;
            part->ptype = type;
            part->oper_idx = 0;
            part->state = ST_NOT_CREATED;
            part->dest_type = DEST_NONE;
            part->dest_id = -1;
        }
    }
    proc->pending = proc->part_count; 
}

/* Создать детали, запустить процессы и ждать, пока не выполнен план или не кончились занятия */
void process_run(Process *proc, Link *link, volatile sig_atomic_t *interrupted)
{
    int next_part = 0;
    clock_gettime(CLOCK_MONOTONIC, &proc->start_time);

    while (!proc->stop) {
        if (interrupted != NULL && *interrupted) {
            dprintf(1, "\ninterrupted by user\n");
            break;
        }
        if (link_stopped(link)) {
            dprintf(1, "\nstopped: a worker was interrupted\n");
            break;
        }

        proc->now = model_now(proc);

        /* план выполнен, события кончились либо вышло время */
        if (proc->products_done >= PLAN || proc->now >= MAX_MODEL_TIME ||
            (next_part >= proc->part_count && proc->pending <= 0)) {
            break;
        }

        if (next_part < proc->part_count && proc->now >= create_time(next_part)) {
            handle_create(proc, link, next_part++);
            activity_done(proc, link);
            continue;
        }

        double until_next = (next_part < proc->part_count)
                          ? create_time(next_part) - proc->now
                          : MAX_MODEL_TIME - proc->now;
        int timeout = wait_time_ms(until_next, proc->delay_ms);
        if (timeout > 100) timeout = 100; /* короткие интервалы: вовремя замечаем остановку со стороны работника */

        int worker_id;
        int result = link_wait_done(link, timeout, &worker_id);
        if (result < 0) break;
        if (result == 0) continue;

        proc->now = model_now(proc);
        handle_worker_done(proc, link, worker_id);
    }
}

/* Остановить работников и разбудить работников, которые ждут */
void process_stop_workers(Process *proc, Link *link)
{
    proc->stop = 1;
    link_request_stop(link);
}

/* Итог: время, выполнение плана, число событий и судьба деталей */
void process_print_stats(const Process *proc)
{
    dprintf(1, "\nend at %.2f\n", proc->now);
    dprintf(1, "products %d/%d\n", proc->products_done, PLAN);

    if (proc->products_done < PLAN) {
        if (proc->now >= MAX_MODEL_TIME) dprintf(1, "time limit reached\n");
        /* застрявший контролёр - признак взаимной блокировки */
        for (int i = 0; i < NUM_INSPECTORS; ++i) {
            if (proc->inspectors[i].blocked_part >= 0) {
                dprintf(1, "inspector %d stuck with part %d\n",
                        i, proc->inspectors[i].blocked_part);
            }
        }
    }

    dprintf(1, "events %d\n", proc->total_events);
    int done = 0, scrapped = 0, in_buffer = 0, other = 0;
    for (int i = 0; i < proc->part_count; ++i) {
        switch (proc->parts[i].state) {
        case ST_DONE:
        case ST_IN_ASSEMBLY: done++; break;
        case ST_SCRAPPED: scrapped++; break;
        case ST_IN_BUFFER: in_buffer++; break;
        default: other++; break;
        }
    }
    dprintf(1, "parts: used %d, scrapped %d, buffered %d, other %d\n",
            done, scrapped, in_buffer, other);
}