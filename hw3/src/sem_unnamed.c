#include <stdio.h>
#include <semaphore.h>

#include "sem.h"


static sem_t *unnamed_create(sem_t *slot, const char *name, unsigned value)
{
    if (sem_init(slot, 1, value) == -1) {
        perror(name);
        return NULL;
    }

    return slot;
}

/* Семафор лежит в shared memory, которую процесс уже подключил, имени у него нет */
static sem_t *unnamed_open(sem_t *slot, const char *name)
{
    (void)name;
    return slot;
}

static void unnamed_close(sem_t *sem)
{
    (void)sem;
}

static void unnamed_destroy(sem_t *sem, const char *name)
{
    (void)name;
    if (sem_destroy(sem) == -1) perror("sem_destroy");
}

/* Конкретная реализация unnamed семафоров */
const SemKind sem_unnamed = {
    .name = "unnamed",
    .in_shared_memory = 1,
    .create = unnamed_create,
    .open = unnamed_open,
    .close = unnamed_close,
    .destroy = unnamed_destroy,
};
