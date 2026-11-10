#ifndef LINK_H
#define LINK_H

#include <time.h>

#include "params.h"
#include "sem.h"


/* Связь фабрики с работниками: фабрика выдаёт работнику задание, работник сообщает о завершении. */
/* Фабрика и работники не знают, как устроена связь - через shared memory или очереди сообщений. */
struct Link;
typedef struct LinkOps {
    const char *name;

    /* Жизненный цикл */
    int (*create)(struct Link *link, const SemKind *sems, const char *prefix); /* фабрика создаёт ресурсы */
    int (*attach)(struct Link *link, const SemKind *sems, const char *prefix, int timeout_ms); /* независимый работник подключается */
    void (*close)(struct Link *link); /* отключиться, создатель ещё удаляет ресурсы */

    /* Со стороны фабрики */
    int (*send_job)(struct Link *link, int worker_id, long duration_ms);
    int (*wait_done)(struct Link *link, int timeout_ms, int *worker_id);
    void (*request_stop)(struct Link *link);

    /* Со стороны работника */
    int (*wait_job)(struct Link *link, int worker_id, int timeout_ms, long *duration_ms);
    int (*report_done)(struct Link *link, int worker_id);
    int (*stopped)(struct Link *link); /* остановка запрошена или фабрика погибла */
} LinkOps;

typedef struct Link {
    const LinkOps *ops;
    void *impl; /* данные конкретной реализации */
} Link;


extern const LinkOps link_shm; /* POSIX shared memory */
extern const LinkOps link_mq; /* POSIX очереди */

int link_create(Link *link, const LinkOps *ops, const SemKind *sems, const char *prefix); /* prefix - начало имён всех ресурсов */
int link_attach(Link *link, const LinkOps *ops, const SemKind *sems, const char *prefix, int timeout_ms);
void link_close(Link *link);

int link_send_job(Link *link, int worker_id, long duration_ms);
int link_wait_done(Link *link, int timeout_ms, int *worker_id);
void link_request_stop(Link *link);

int link_wait_job(Link *link, int worker_id, int timeout_ms, long *duration_ms);
int link_report_done(Link *link, int worker_id);
int link_stopped(Link *link);

/* Хелперы */
void link_deadline(struct timespec *deadline, int timeout_ms); /* абсолютное время через timeout_ms */
int link_pid_alive(int pid);

#endif
