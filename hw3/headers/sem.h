#ifndef SEM_H
#define SEM_H

#include <semaphore.h>


/* Семафоры */
typedef struct {
    const char *name;
    int in_shared_memory; /* семафор лежит в переданной ему shared memory (иначе у него есть имя в системе) */

    /* Создатель делает семафор */
    sem_t *(*create)(sem_t *slot, const char *name, unsigned value); /* slot - место под него в shared memory для неименованных */
    /* Другой процесс получает уже созданный семафор, NULL - его пока нет */
    sem_t *(*open)(sem_t *slot, const char *name);
    /* Закрыть семафор, не уничтожая его */
    void (*close)(sem_t *sem);
    /* Создатель уничтожает семафор */
    void (*destroy)(sem_t *sem, const char *name);
    
} SemKind;

extern const SemKind sem_named; /* sem_open: имя в системе, shared memory не нужна */
extern const SemKind sem_unnamed; /* sem_init(pshared = 1) в переданной shared memory */

#endif
