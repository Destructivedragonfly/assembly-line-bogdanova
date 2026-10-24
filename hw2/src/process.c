#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "headers/params.h"
#include "headers/process.h"


/* static, чтобы не было проблемы переполнения стека */
static IntQueue g_robot_queue;
static Process *g_proc; /* процесс, с которым работают потоки */

/* Модельное время */
static double model_now(const Process *proc)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    double ms = (double)(time.tv_sec - proc->start_time.tv_sec) * 1000.0 +
                (double)(time.tv_nsec - proc->start_time.tv_nsec) / 1e6;
    /* реальные миллисекунды от старта, делённые на миллисекунды на единицу */

    return ms / proc->delay_ms;
}

/* Поток работает, но по сути спит */
static void work(const Process *proc, double units)
{
    if (units <= 0) { return; }

    long long ns = (long long)(units * proc->delay_ms * 1e6);
    struct timespec ts = {(time_t)(ns / 1000000000LL), (long)(ns % 1000000000LL)};

    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

/* Чтобы начать занятие, будим поток участника, он поработает нужное время */
static void start_activity(Process *proc, sem_t *go)
{
    proc->pending++;
    sem_post(go);
}

/* Выбрать станок */
static int choose_machine(Process *proc, int cur_type)
{
    int possible[NUM_MACHINES], cnt = 0;

    /* чтобы было равномерное распределение, выбираем из станков нужного типа какой-то конкретный
    методом round-robin */
    for (int i = 0; i < NUM_MACHINES; ++i) {
        if (machine_type[i] == cur_type) {
            possible[cnt++] = i;
        }
    }
    if (!cnt) {
        return -1;
    }

    int idx = proc->next_machine[cur_type] % cnt;
    proc->next_machine[cur_type]++;

    return possible[idx];
}

/* Выбрать контролёра */
static int choose_inspector(Process *proc)
{
    int idx = proc->next_inspector % NUM_INSPECTORS; /* выбираем по кругу */
    proc->next_inspector++;

    return idx;
}

/* Попытка назначить робота на деталь */
static int try_assign_robot(Process *proc, Part *part)
{
    for (int i = 0; i < NUM_ROBOTS; ++i) {
        Robot *robot = &proc->robots[i];

        if (robot->busy) {
            continue;
        }

        robot->busy = 1;
        robot->part_id = part->id;
        robot->dest_type = part->dest_type;
        robot->dest_id = part->dest_id;

        part->state = ST_IN_ROBOT;

        dprintf(1, "%6.2f robot %d takes part %d\n", proc->now, robot->id, part->id);

        /* планируем время доставки */
        start_activity(proc, &robot->go);

        return 1; /* свободный робот нашёлся */
    }

    return 0; /* свободных нет */
}

/* Вызвать робота или встать в очередь к роботам */
static void request_robot(Process *proc, Part *part)
{
    part->state = ST_WAIT_ROBOT;

    if (try_assign_robot(proc, part)) {
        return;
    }

    if (!queue_push(proc->robot_queue, part->id)) {
        dprintf(2, "error: robot queue full\n");
        exit(1);
    }

    dprintf(1, "%6.2f part %d waits robot\n", proc->now, part->id);
}

/* Деталь доставлена: в очередь станка или контролёра */
static void part_arrive(Process *proc, Part *part, DestinationType dest_type, int dest_id)
{
    if (dest_type == DEST_MACHINE) {
        Machine *machine = &proc->machines[dest_id];

        if (!queue_push(&machine->queue, part->id)) {
            dprintf(2, "error: machine %d queue full\n", dest_id);
            exit(1);
        }
        part->state = ST_MACHINE_QUEUE;

        dprintf(1, "%6.2f part %d -> machine %d\n", proc->now, part->id, dest_id);
    } else if (dest_type == DEST_INSPECTOR) {
        Inspector *inspector = &proc->inspectors[dest_id];

        if (!queue_push(&inspector->queue, part->id)) {
            dprintf(2, "error: inspector %d queue full\n", dest_id);
            exit(1);
        }
        part->state = ST_CONTROL_QUEUE;

        dprintf(1, "%6.2f part %d -> inspector %d\n", proc->now, part->id, dest_id);
    }
}

/* Робот довёз деталь */
static void handle_robot_drop(Process *proc, int id)
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
    if (queue_pop(proc->robot_queue, &part_id)) {
        try_assign_robot(proc, &proc->parts[part_id]);
    }
}

/* Положить годную деталь в накопитель, 0 - накопитель полон */
static int try_place_buffer(Process *proc, Part *part)
{
    Buffer *buffer = &proc->buffers[part->ptype];

    if (buffer->count >= buffer->cap) {
        return 0;
    }

    buffer->items[buffer->count++] = part->id;
    part->state = ST_IN_BUFFER;

    dprintf(1, "%6.2f part %d in buffer %d\n", proc->now, part->id, part->ptype);

    return 1;
}

/* Есть ли в каждом накопителе хотя бы одна деталь */
static int kit_ready(const Process *proc)
{
    for (int i = 0; i < NUM_PART_TYPES; ++i) {
        if (proc->buffers[i].count == 0) {
            return 0;
        }
    }
    return 1;
}

/* Достать самую старую деталь из накопителя */
static int buffer_pop(Buffer *buffer)
{
    int part_id = buffer->items[0];

    if (buffer->count > 1) {
        memmove(
            &buffer->items[0],
            &buffer->items[1],
            (size_t)(buffer->count - 1) * sizeof(int)
        );
    }
    buffer->count--;

    return part_id;
}

/* Запустить станок, если он свободен и есть деталь в очереди */
static void try_start_machine(Process *proc, int machine_id)
{
    Machine *machine = &proc->machines[machine_id];
    int part_id;

    if (machine->busy) {
        return;
    }
    if (!queue_pop(&machine->queue, &part_id)) {
        return;
    }

    Part *part = &proc->parts[part_id];

    machine->busy = 1;
    machine->part_id = part_id;
    part->state = ST_IN_MACHINE;

    dprintf(1, "%6.2f machine %d starts part %d\n", proc->now, machine_id, part_id);

    start_activity(proc, &machine->go);
}

/* Запустить контролёра, если он свободен и есть деталь в очереди */
static void try_start_control(Process *proc, int inspector_id)
{
    Inspector *inspector = &proc->inspectors[inspector_id];
    int part_id;

    if (inspector->busy || inspector->blocked_part >= 0) {
        return;
    }
    if (!queue_pop(&inspector->queue, &part_id)) {
        return;
    }

    Part *part = &proc->parts[part_id];

    inspector->busy = 1;
    inspector->part_id = part_id;
    part->state = ST_IN_CONTROL;

    dprintf(1, "%6.2f inspector %d checks part %d\n", proc->now, inspector_id, part_id);

    start_activity(proc, &inspector->go);
}

/* Начать сборку, если пост свободен и есть полный комплект, 1 - начали */
static int try_start_assembly(Process *proc)
{
    for (int a = 0; a < NUM_ASM; ++a) {
        AssemblyPost *post = &proc->assembly_posts[a];

        if (post->busy) {
            continue;
        }
        if (proc->products_started >= PLAN) {
            continue;
        }
        if (!kit_ready(proc)) {
            continue;
        }

        for (int type = 0; type < NUM_PART_TYPES; ++type) {
            int part_id = buffer_pop(&proc->buffers[type]);

            proc->parts[part_id].state = ST_IN_ASSEMBLY;
            post->parts_used[type] = part_id;
        }

        post->busy = 1;
        proc->products_started++;
        post->product_idx = proc->next_product_num;

        dprintf(1, "%6.2f assembly %d starts product %d\n", proc->now, a, post->product_idx);

        start_activity(proc, &post->go);

        return 1;
    }

    return 0;
}

/* Запустить всё, что можно, пока что-то запускается */
static void try_start_all(Process *proc)
{
    int progress = 1;

    while (progress && proc->products_done < PLAN) {
        progress = 0;

        /* разблокировка контролёров */
        for (int i = 0; i < NUM_INSPECTORS; ++i) {
            Inspector *inspector = &proc->inspectors[i];

            if (inspector->blocked_part < 0) {
                continue;
            }
            if (try_place_buffer(proc, &proc->parts[inspector->blocked_part])) {
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

            if (inspector->busy || inspector->blocked_part >= 0 ||
                queue_empty(&inspector->queue)) {
                continue;
            }
            try_start_control(proc, i);
            progress = 1;
        }

        /* свободные станки */
        for (int i = 0; i < NUM_MACHINES; ++i) {
            Machine *machine = &proc->machines[i];

            if (machine->busy || queue_empty(&machine->queue)) {
                continue;
            }
            try_start_machine(proc, i);
            progress = 1;
        }

        /* сборка */
        if (try_start_assembly(proc)) {
            progress = 1;
        }
    }
}

/* Деталь появилась на линии */
static void handle_create(Process *proc, int id)
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

    request_robot(proc, part);
}

/* Станок закончил операцию */
static void handle_oper_done(Process *proc, int id)
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
    } else {
        part->dest_type = DEST_INSPECTOR;
        part->dest_id = choose_inspector(proc);
    }

    request_robot(proc, part);
}

/* Контролёр закончил проверку: годна, блокировка, переделка или списание */
static void handle_control_done(Process *proc, int id)
{
    Inspector *inspector = &proc->inspectors[id];
    Part *part = &proc->parts[inspector->part_id];

    if ((double)rand() / RAND_MAX >= PROB_DEFECT) {
        dprintf(1, "%6.2f part %d ok\n", proc->now, part->id);

        part->state = ST_WAIT_BUFFER;

        if (try_place_buffer(proc, part)) {
            inspector->busy = 0;
            inspector->part_id = -1;
        } else {
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

        request_robot(proc, part);
    } else {
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
    proc->next_product_num++;
}

/* Конец, запустить всё, что можно, и проверить, не пора ли заканчивать */
static void activity_done(Process *proc)
{
    proc->total_events++;

    try_start_all(proc);

    proc->pending--;

    /* план выполнен, события кончились либо вышло время */
    if (proc->products_done >= PLAN || proc->pending == 0 || proc->now >= MAX_MODEL_TIME) {
        sem_post(&proc->finished);
    }
}

/* Поставщик создаёт детали в те же моменты, что были у событий создания */
static void *supplier_thread(void *arg)
{
    Process *proc = arg;

    for (int id = 0; id < proc->part_count; ++id) {
        work(proc, (id / NUM_PART_TYPES) * 0.6 + (id % NUM_PART_TYPES) * 0.25 - model_now(proc));

        pthread_mutex_lock(&proc->lock);
        proc->now = model_now(proc);
        handle_create(proc, id);
        activity_done(proc);
        pthread_mutex_unlock(&proc->lock);
    }
    return NULL;
}

/* Робот */
static void *robot_thread(void *arg)
{
    Robot *robot = arg;
    Process *proc = g_proc;

    /* ждёт деталь, везёт её ROBOT_TRAVEL */
    for (;;) {
        sem_wait(&robot->go);
        work(proc, ROBOT_TRAVEL);

        pthread_mutex_lock(&proc->lock);
        proc->now = model_now(proc);
        handle_robot_drop(proc, robot->id);
        activity_done(proc);
        pthread_mutex_unlock(&proc->lock);
    }
    return NULL;
}

/* Станок */
static void *machine_thread(void *arg)
{
    Machine *machine = arg;
    Process *proc = g_proc;

    /* ждёт, пока ему назначат деталь, и выполняет операцию */
    for (;;) {
        sem_wait(&machine->go);

        /* деталь уже занята этим станком, другие потоки её не трогают */
        Part *part = &proc->parts[machine->part_id];

        work(proc, oper_time[part->ptype][part->oper_idx]);

        pthread_mutex_lock(&proc->lock);
        proc->now = model_now(proc);
        handle_oper_done(proc, machine->id);
        activity_done(proc);
        pthread_mutex_unlock(&proc->lock);
    }
    return NULL;
}

/* Контролёр */
static void *inspector_thread(void *arg)
{
    Inspector *inspector = arg;
    Process *proc = g_proc;

    /* ждёт деталь и проверяет её CONTROL_TIME */
    for (;;) {
        sem_wait(&inspector->go);
        work(proc, CONTROL_TIME);

        pthread_mutex_lock(&proc->lock);
        proc->now = model_now(proc);
        handle_control_done(proc, inspector->id);
        activity_done(proc);
        pthread_mutex_unlock(&proc->lock);
    }
    return NULL;
}

/* Пост сборки */
static void *assembly_thread(void *arg)
{
    AssemblyPost *post = arg;
    Process *proc = g_proc;

    /* ждёт комплект и собирает изделие ASM_TIME  */
    for (;;) {
        sem_wait(&post->go);
        work(proc, ASM_TIME);

        pthread_mutex_lock(&proc->lock);
        proc->now = model_now(proc);
        handle_asm_done(proc, post->id);
        activity_done(proc);
        pthread_mutex_unlock(&proc->lock);
    }
    return NULL;
}

/* Начальное состояние модели */
void process_init(Process *proc)
{
    proc->part_count = 0;
    proc->now = 0.0;

    proc->products_started = 0;
    proc->products_done = 0;
    proc->next_product_num = 1;

    proc->next_inspector = 0;
    proc->seed = 0;
    proc->total_events = 0;

    for (int i = 0; i < NUM_MACHINE_TYPES; ++i) {
        proc->next_machine[i] = 0;
    }

    proc->robot_queue = &g_robot_queue;
    queue_init(proc->robot_queue);

    proc->delay_ms = 100;
    proc->pending = 0;
    pthread_mutex_init(&proc->lock, NULL);
    sem_init(&proc->finished, 0, 0);

    for (int i = 0; i < NUM_MACHINES; ++i) {
        Machine *machine = &proc->machines[i];

        machine->id = i;
        machine->busy = 0;
        machine->part_id = -1;
        queue_init(&machine->queue);
        sem_init(&machine->go, 0, 0);
    }

    for (int i = 0; i < NUM_INSPECTORS; ++i) {
        Inspector *inspector = &proc->inspectors[i];

        inspector->id = i;
        inspector->busy = 0;
        inspector->part_id = -1;
        inspector->blocked_part = -1;
        queue_init(&inspector->queue);
        sem_init(&inspector->go, 0, 0);
    }

    for (int i = 0; i < NUM_ASM; ++i) {
        AssemblyPost *post = &proc->assembly_posts[i];

        post->id = i;
        post->busy = 0;
        sem_init(&post->go, 0, 0);
        post->product_idx = 0;
        for (int type = 0; type < NUM_PART_TYPES; ++type) {
            post->parts_used[type] = -1;
        }
    }

    for (int i = 0; i < NUM_ROBOTS; ++i) {
        Robot *robot = &proc->robots[i];

        robot->id = i;
        robot->busy = 0;
        sem_init(&robot->go, 0, 0);
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
}

static void start_thread(pthread_t *thread, void *(*fn)(void *), void *arg)
{
    if (pthread_create(thread, NULL, fn, arg) != 0) {
        dprintf(2, "error: cannot create a thread\n");
        exit(1);
    }
    pthread_detach(*thread); /* потоки можно не ждать, ведь они завершаются вместе с программой */
}

/* Создать детали, запустить потоки и ждать, пока не выполнен план или не кончились занятия */
void process_run(Process *proc)
{
    int total_per_type = PLAN + EXTRA_PARTS;

    if (total_per_type * NUM_PART_TYPES > MAX_PARTS) {
        dprintf(2, "error: too many parts\n");
        exit(1);
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

    proc->pending = proc->part_count; /* впереди создание каждой детали */
    g_proc = proc;
    clock_gettime(CLOCK_MONOTONIC, &proc->start_time);

    pthread_t supplier;

    for (int i = 0; i < NUM_ROBOTS; ++i) {
        start_thread(&proc->robots[i].thread, robot_thread, &proc->robots[i]);
    }
    for (int i = 0; i < NUM_MACHINES; ++i) {
        start_thread(&proc->machines[i].thread, machine_thread, &proc->machines[i]);
    }
    for (int i = 0; i < NUM_INSPECTORS; ++i) {
        start_thread(&proc->inspectors[i].thread, inspector_thread, &proc->inspectors[i]);
    }
    for (int i = 0; i < NUM_ASM; ++i) {
        start_thread(&proc->assembly_posts[i].thread, assembly_thread, &proc->assembly_posts[i]);
    }
    start_thread(&supplier, supplier_thread, proc);

    while (sem_wait(&proc->finished) == -1 && errno == EINTR) {
    }

    /* берём мьютекс и не отпускаем */
    /* остальные потоки встанут на следующем обращении к модели, поэтому статистика печатается без помех */
    pthread_mutex_lock(&proc->lock);
}

/* Итог: время, выполнение плана, число событий и судьба деталей */
void process_print_stats(const Process *proc)
{
    dprintf(1, "\nend at %.2f\n", proc->now);
    dprintf(1, "products %d/%d\n", proc->products_done, PLAN);

    if (proc->products_done < PLAN) {
        if (proc->now >= MAX_MODEL_TIME) {
            dprintf(1, "time limit reached\n");
        }
        /* застрявший контролёр - признак взаимной блокировки */
        for (int i = 0; i < NUM_INSPECTORS; ++i) {
            if (proc->inspectors[i].blocked_part >= 0) {
                dprintf(1, "inspector %d stuck with part %d\n", i, proc->inspectors[i].blocked_part);
            }
        }
    }

    dprintf(1, "events %d\n", proc->total_events);

    int done = 0, scrapped = 0, in_buffer = 0, other = 0;

    for (int i = 0; i < proc->part_count; ++i) {
        switch (proc->parts[i].state) {
        case ST_DONE:
        case ST_IN_ASSEMBLY:
            done++;
            break;
        case ST_SCRAPPED:
            scrapped++;
            break;
        case ST_IN_BUFFER:
            in_buffer++;
            break;
        default:
            other++;
            break;
        }
    }

    dprintf(1, "parts: used %d, scrapped %d, buffered %d, other %d\n", done, scrapped, in_buffer, other);
}
