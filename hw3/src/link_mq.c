#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "link.h"


/* Сообщение фабрики работнику: задание */
typedef struct {
    long duration_ms;
} JobMessage;

/* Сообщение работника фабрике: задание выполнено */
typedef struct {
    int worker_id;
} DoneMessage;

typedef struct {
    const SemKind *sems;

    char prefix[48];
    int owner; /* создатель удаляет ресурсы */

    sem_t *stop; /* значение > 0 - пора завершаться */
    sem_t *pid; /* значение - pid фабрики */
    mqd_t jobs[MAX_WORKERS]; /* очередь заданий каждого работника */
    mqd_t done; /* общая очередь сообщений об окончании работы */
} MqLink;


static void sem_name(const MqLink *mq, const char *what, char *name, size_t size)
{
    snprintf(name, size, "%s_%s", mq->prefix, what);
}

static void queue_name(const MqLink *mq, int worker_id, char *name, size_t size)
{
    if (worker_id < 0) snprintf(name, size, "%s_done", mq->prefix);
    else snprintf(name, size, "%s_job%d", mq->prefix, worker_id);
}

/* Закрыть свои дескрипторы */
static void release(MqLink *mq)
{
    for (int i = 0; i < MAX_WORKERS; ++i) {
        if (mq->jobs[i] != (mqd_t)-1) mq_close(mq->jobs[i]);
        mq->jobs[i] = (mqd_t)-1;
    }
    if (mq->done != (mqd_t)-1) mq_close(mq->done);
    mq->done = (mqd_t)-1;

    if (mq->stop != NULL) mq->sems->close(mq->stop);
    if (mq->pid != NULL) mq->sems->close(mq->pid);
    mq->stop = NULL;
    mq->pid = NULL;
}

static void mq_close_link(Link *link)
{
    MqLink *mq = link->impl;
    if (mq == NULL) return;

    if (mq->owner) {
        char name[64];
        for (int i = -1; i < MAX_WORKERS; ++i) {
            if ((i < 0 ? mq->done : mq->jobs[i]) == (mqd_t)-1) continue;

            queue_name(mq, i, name, sizeof(name));
            mq_unlink(name);
        }
        if (mq->stop != NULL) {
            sem_name(mq, "stop", name, sizeof(name));
            mq->sems->destroy(mq->stop, name);
            mq->stop = NULL;
        }
        if (mq->pid != NULL) {
            sem_name(mq, "pid", name, sizeof(name));
            mq->sems->destroy(mq->pid, name);
            mq->pid = NULL;
        }
    }

    release(mq);
    free(mq);
    link->impl = NULL;
}

static MqLink *mq_alloc(Link *link, const SemKind *sems, const char *prefix, int owner)
{
    /* семафоры нужны без shared memory, у очередей её нет */
    if (sems->in_shared_memory) {
        dprintf(2, "message queues need named semaphores\n");
        return NULL;
    }

    MqLink *mq = calloc(1, sizeof(*mq));
    if (mq == NULL) {
        perror("calloc");
        return NULL;
    }
    mq->sems = sems;
    mq->owner = owner;

    snprintf(mq->prefix, sizeof(mq->prefix), "%s", prefix);
    for (int i = 0; i < MAX_WORKERS; ++i) { 
        mq->jobs[i] = (mqd_t)-1;
    }
    mq->done = (mqd_t)-1;
    link->impl = mq;

    return mq;
}

/* Лимит ядра на число сообщений в очереди */
static int msg_max(void)
{
    int value = 10;
    FILE *file = fopen("/proc/sys/fs/mqueue/msg_max", "r");

    if (file != NULL) {
        if (fscanf(file, "%d", &value) != 1) value = 10;
        fclose(file);
    }

    return value > 0 ? value : 10;
}

static mqd_t create_queue(const char *name, long max_messages, long message_size)
{
    struct mq_attr attr = {0};
    attr.mq_maxmsg = max_messages;
    attr.mq_msgsize = message_size;

    mq_unlink(name); /* остаток аварийно завершённого запуска */
    mqd_t queue = mq_open(name, O_CREAT | O_EXCL | O_RDWR, 0600, &attr);
    if (queue == (mqd_t)-1) perror(name);

    return queue;
}

/* Создать все ресурсы */
static int create_all(MqLink *mq)
{
    char name[64];

    sem_name(mq, "stop", name, sizeof(name));
    mq->stop = mq->sems->create(NULL, name, 0);
    if (mq->stop == NULL) return 0;

    for (int i = 0; i < MAX_WORKERS; ++i) {
        queue_name(mq, i, name, sizeof(name));
        mq->jobs[i] = create_queue(name, 2, sizeof(JobMessage));

        /* задание у работника одно, а завершений одновременно не больше, чем работников (но не больше лимита ядра) */
        if (mq->jobs[i] == (mqd_t) - 1) return 0;
    }

    int limit = msg_max();
    queue_name(mq, -1, name, sizeof(name));
    mq->done = create_queue(name, MAX_WORKERS < limit ? MAX_WORKERS : limit, sizeof(DoneMessage));
    if (mq->done == (mqd_t) - 1) return 0;

    sem_name(mq, "pid", name, sizeof(name));
    mq->pid = mq->sems->create(NULL, name, (unsigned)getpid());

    return mq->pid != NULL;
}

/* Фабрика создаёт очереди и семафоры */
static int mq_create(Link *link, const SemKind *sems, const char *prefix)
{
    MqLink *mq = mq_alloc(link, sems, prefix, 1);
    if (mq == NULL) return 0;

    if (!create_all(mq)) {
        mq_close_link(link);
        return 0;
    }

    return 1;
}

/* Попытка подключения */
static int try_attach(MqLink *mq)
{
    char name[64];
    sem_name(mq, "pid", name, sizeof(name));
    mq->pid = mq->sems->open(NULL, name);
    if (mq->pid == NULL) return 0;

    int pid = 0; 
    sem_getvalue(mq->pid, &pid); 

    if (link_pid_alive(pid)) {
        sem_name(mq, "stop", name, sizeof(name));
        mq->stop = mq->sems->open(NULL, name);

        int opened = mq->stop != NULL;
        for (int i = -1; i < MAX_WORKERS && opened; ++i) {
            queue_name(mq, i, name, sizeof(name));
            mqd_t queue = mq_open(name, O_RDWR);

            if (queue == (mqd_t)-1) opened = 0;
            else if (i < 0) mq->done = queue;
            else mq->jobs[i] = queue;
        }
        if (opened) return 1;
    }

    release(mq); /* фабрики, создавшей ресурсы, уже нет - ждём новую */
    return 0;
}

/* Независимо запущенный работник подключается к ресурсам фабрики */
/* Фабрику можно запустить и позже */
static int mq_attach(Link *link, const SemKind *sems, const char *prefix, int timeout_ms)
{
    MqLink *mq = mq_alloc(link, sems, prefix, 0);
    if (mq == NULL) return 0;

    for (int waited = 0; !try_attach(mq); waited += 50) {
        if (waited >= timeout_ms) {
            dprintf(2, "cannot attach to '%s': is the factory running?\n", prefix);
            mq_close_link(link);
            return 0;
        }

        struct timespec delay = {0, 50000000L};
        nanosleep(&delay, NULL);
    }

    return 1;
}

static int mq_send_job(Link *link, int worker_id, long duration_ms)
{
    MqLink *mq = link->impl;
    JobMessage message = {duration_ms};

    struct timespec deadline;
    link_deadline(&deadline, 1000);

    if (mq_timedsend(mq->jobs[worker_id], (const char *)&message, sizeof(message), 0, &deadline) == -1) {
        perror("mq_send job");
        return 0;
    }

    return 1;
}

static int mq_wait_job(Link *link, int worker_id, int timeout_ms, long *duration_ms)
{
    MqLink *mq = link->impl;
    JobMessage message;

    struct timespec deadline;
    link_deadline(&deadline, timeout_ms);

    if (mq_timedreceive(mq->jobs[worker_id], (char *)&message, sizeof(message), NULL, &deadline) == -1) {
        if (errno == ETIMEDOUT || errno == EINTR) return 0;

        perror("mq_receive job");
        return -1;
    }
    *duration_ms = message.duration_ms;

    return 1;
}

static int mq_report_done(Link *link, int worker_id)
{
    MqLink *mq = link->impl;
    DoneMessage message = {worker_id};

    /* очередь может быть короче числа работников (лимит ядра): ждём, пока фабрика разберёт сообщения */
    for (;;) {
        struct timespec deadline;
        link_deadline(&deadline, 100);

        if (mq_timedsend(mq->done, (const char *)&message, sizeof(message), 0, &deadline) == 0) return 1;
        if (errno != ETIMEDOUT && errno != EINTR) {
            perror("mq_send done");
            return 0;
        }
        if (link->ops->stopped(link)) return 0;
    }
}

static int mq_wait_done(Link *link, int timeout_ms, int *worker_id)
{
    MqLink *mq = link->impl;
    DoneMessage message;

    struct timespec deadline;
    link_deadline(&deadline, timeout_ms);

    if (mq_timedreceive(mq->done, (char *)&message, sizeof(message), NULL, &deadline) == -1) {
        if (errno == ETIMEDOUT || errno == EINTR) return 0;

        perror("mq_receive done");
        return -1;
    }
    *worker_id = message.worker_id;

    return 1;
}

static void mq_request_stop(Link *link)
{
    MqLink *mq = link->impl;
    sem_post(mq->stop);
}

static int mq_stopped(Link *link)
{
    MqLink *mq = link->impl;

    int stop = 0, pid = 0;
    sem_getvalue(mq->stop, &stop);
    sem_getvalue(mq->pid, &pid);

    return stop > 0 || !link_pid_alive(pid);
}

/* Функции конкретно для реализации message queue */
const LinkOps link_mq = {
    .name = "mq",
    .create = mq_create,
    .attach = mq_attach,
    .close = mq_close_link,
    .send_job = mq_send_job,
    .wait_done = mq_wait_done,
    .request_stop = mq_request_stop,
    .wait_job = mq_wait_job,
    .report_done = mq_report_done,
    .stopped = mq_stopped,
};
