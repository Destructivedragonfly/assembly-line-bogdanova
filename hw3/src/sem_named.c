#include <fcntl.h>
#include <stdio.h>
#include <semaphore.h>
#include <sys/stat.h>

#include "sem.h"


static sem_t *named_create(sem_t *slot, const char *name, unsigned value)
{
    (void)slot;

    sem_unlink(name); /* остаток аварийно завершённого запуска помешал бы O_EXCL */
    sem_t *sem = sem_open(name, O_CREAT | O_EXCL | O_RDWR, 0600, value);

    if (sem == SEM_FAILED) {
        perror(name);
        return NULL;
    }

    return sem;
}

static sem_t *named_open(sem_t *slot, const char *name)
{
    (void)slot;

    sem_t *sem = sem_open(name, O_RDWR);
    return sem == SEM_FAILED ? NULL : sem;
}

static void named_close(sem_t *sem)
{
    sem_close(sem);
}

static void named_destroy(sem_t *sem, const char *name)
{
    sem_close(sem);
    sem_unlink(name);
}

/* Конкретная реализация named семафоров */
const SemKind sem_named = {
    .name = "named",
    .in_shared_memory = 0,
    .create = named_create,
    .open = named_open,
    .close = named_close,
    .destroy = named_destroy,
};
