#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "link.h"


/* Семафоры связи */
enum {
    SEM_LOCK, /* мьютекс сообщает, что только один процесс работает с shared memory */
    /* Изначально = 1 */
    SEM_DONE, /* оповещает фабрику, что работник закончил */
    /* Изначально = 0 */
    SEM_JOB, /* будит работника, когда для него есть задание */
    /* Изначально = 0 */
    SEM_COUNT = SEM_JOB + MAX_WORKERS
};

/* Всё, что фабрика и работники делят через shared memory */
typedef struct {
    int ready; /* флаг инициализации */
    int factory_pid; /* pid для определения состояния фабрики */
    int stop; /* флаг завершения */

    long job_ms[MAX_WORKERS]; /* сколько реальных мс работнику работать */

    int done[MAX_WORKERS]; /* очередь работников, закончивших задание */
    int done_head;
    int done_count;

    sem_t slots[SEM_COUNT]; /* место под семафоры, если они неименованные */
} Shared;

typedef struct {
    const SemKind *sems;

    char prefix[48];
    int owner; /* создатель удаляет ресурсы */

    Shared *shared;
    sem_t *sem[SEM_COUNT];
} ShmLink;


static void shm_name(const ShmLink *shm, char *name, size_t size)
{
    snprintf(name, size, "%s_shm", shm->prefix);
}

static void sem_name(const ShmLink *shm, int index, char *name, size_t size)
{
    snprintf(name, size, "%s_sem%d", shm->prefix, index);
}

static void shm_close(Link *link)
{
    ShmLink *shm = link->impl;
    if (shm == NULL) return;

    char name[64];
    for (int i = 0; i < SEM_COUNT; ++i) {
        if (shm->sem[i] == NULL) continue;

        sem_name(shm, i, name, sizeof(name));
        if (shm->owner) shm->sems->destroy(shm->sem[i], name);
        else shm->sems->close(shm->sem[i]);
    }

    if (shm->shared != NULL) munmap(shm->shared, sizeof(Shared));
    if (shm->owner) {
        shm_name(shm, name, sizeof(name));
        shm_unlink(name);
    }

    free(shm);
    link->impl = NULL;
}

static ShmLink *shm_alloc(Link *link, const SemKind *sems, const char *prefix, int owner)
{
    ShmLink *shm = calloc(1, sizeof(*shm));
    if (shm == NULL) {
        perror("calloc");
        return NULL;
    }
    shm->sems = sems;
    shm->owner = owner;
    snprintf(shm->prefix, sizeof(shm->prefix), "%s", prefix);
    link->impl = shm;

    return shm;
}

static Shared *map_shared(int fd)
{
    void *shared = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    return shared == MAP_FAILED ? NULL : shared;
}

/* Фабрика создаёт shared memory и семафоры */
static int shm_create(Link *link, const SemKind *sems, const char *prefix)
{
    ShmLink *shm = shm_alloc(link, sems, prefix, 1);
    if (shm == NULL) return 0;

    char name[64];
    shm_name(shm, name, sizeof(name));
    shm_unlink(name); /* остаток аварийно завершённого запуска */

    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1) {
        perror("shm_open");
        shm_close(link);
        return 0;
    }
    if (ftruncate(fd, (off_t)sizeof(Shared)) == -1) {
        perror("ftruncate");
        close(fd);
        shm_close(link);
        return 0;
    }

    shm->shared = map_shared(fd); /* страницы нулевые */
    if (shm->shared == NULL) {
        perror("mmap");
        shm_close(link);
        return 0;
    }
    shm->shared->factory_pid = (int)getpid();

    for (int i = 0; i < SEM_COUNT; ++i) {
        sem_name(shm, i, name, sizeof(name));
        shm->sem[i] = sems->create(&shm->shared->slots[i], name, i == SEM_LOCK ? 1 : 0);

        if (shm->sem[i] == NULL) {
            shm_close(link);
            return 0;
        }
    }

    __atomic_store_n(&shm->shared->ready, 1, __ATOMIC_RELEASE);

    return 1;
}

/* Попытка подключения  */
static int try_attach(ShmLink *shm)
{
    char name[64];
    shm_name(shm, name, sizeof(name));

    int fd = shm_open(name, O_RDWR, 0600);
    if (fd == -1) return 0;

    /* создатель мог ещё не успеть задать размер */
    struct stat st;
    if (fstat(fd, &st) == -1 || st.st_size != (off_t)sizeof(Shared)) {
        close(fd);
        return 0;
    }

    Shared *shared = map_shared(fd);
    if (shared == NULL) return 0;

    if (!__atomic_load_n(&shared->ready, __ATOMIC_ACQUIRE) || !link_pid_alive(shared->factory_pid)) {
        munmap(shared, sizeof(Shared));
        return 0;
    }

    for (int i = 0; i < SEM_COUNT; ++i) {
        sem_name(shm, i, name, sizeof(name));
        shm->sem[i] = shm->sems->open(&shared->slots[i], name);

        if (shm->sem[i] == NULL) {
            for (int j = 0; j < i; ++j) {
                shm->sems->close(shm->sem[j]);
            } 
            memset(shm->sem, 0, sizeof(shm->sem));
            munmap(shared, sizeof(Shared));

            return 0;
        }
    }
    shm->shared = shared;

    return 1;
}

/* Независимо запущенный работник подключается к ресурсам фабрики. Фабрику можно запустить и позже */
static int shm_attach(Link *link, const SemKind *sems, const char *prefix, int timeout_ms)
{
    ShmLink *shm = shm_alloc(link, sems, prefix, 0);
    if (shm == NULL) return 0;

    for (int waited = 0; !try_attach(shm); waited += 50) {
        if (waited >= timeout_ms) {
            dprintf(2, "cannot attach to '%s': is the factory running?\n", prefix);
            shm_close(link);

            return 0;
        }

        struct timespec delay = {0, 50000000L};
        nanosleep(&delay, NULL);
    }

    return 1;
}

static void lock(ShmLink *shm)
{
    while (sem_wait(shm->sem[SEM_LOCK]) == -1 && errno == EINTR) {}
}

static void unlock(ShmLink *shm)
{
    sem_post(shm->sem[SEM_LOCK]);
}

/* Ожидание семафора с таймаутом */
static int wait_sem(sem_t *sem, int timeout_ms)
{
    struct timespec deadline;
    link_deadline(&deadline, timeout_ms);

    while (sem_timedwait(sem, &deadline) == -1) {
        if (errno == ETIMEDOUT || errno == EINTR) return 0;

        perror("sem_timedwait");
        return -1;
    }

    return 1;
}

static int shm_send_job(Link *link, int worker_id, long duration_ms)
{
    ShmLink *shm = link->impl;

    lock(shm);
    shm->shared->job_ms[worker_id] = duration_ms;
    unlock(shm);

    return sem_post(shm->sem[SEM_JOB + worker_id]) == 0;
}

static int shm_wait_job(Link *link, int worker_id, int timeout_ms, long *duration_ms)
{
    ShmLink *shm = link->impl;

    int result = wait_sem(shm->sem[SEM_JOB + worker_id], timeout_ms);
    if (result <= 0) return result;

    lock(shm);
    *duration_ms = shm->shared->job_ms[worker_id];
    unlock(shm);

    return 1;
}

static int shm_report_done(Link *link, int worker_id)
{
    ShmLink *shm = link->impl;
    Shared *shared = shm->shared;

    lock(shm);
    int queued = shared->done_count < MAX_WORKERS;

    if (queued) {
        shared->done[(shared->done_head + shared->done_count) % MAX_WORKERS] = worker_id;
        shared->done_count++;
    }
    unlock(shm);

    if (!queued) {
        dprintf(2, "error: completion queue full\n");
        return 0;
    }

    return sem_post(shm->sem[SEM_DONE]) == 0;
}

static int shm_wait_done(Link *link, int timeout_ms, int *worker_id)
{
    ShmLink *shm = link->impl;
    Shared *shared = shm->shared;

    int result = wait_sem(shm->sem[SEM_DONE], timeout_ms);
    if (result <= 0) return result;

    lock(shm);
    int popped = shared->done_count > 0;

    if (popped) {
        *worker_id = shared->done[shared->done_head];
        shared->done_head = (shared->done_head + 1) % MAX_WORKERS;
        shared->done_count--;
    }
    unlock(shm);

    return popped ? 1 : -1;
}

static void shm_request_stop(Link *link)
{
    ShmLink *shm = link->impl;

    lock(shm);
    shm->shared->stop = 1;
    unlock(shm);

    /* разбудить работников, которые ждут задание */
    for (int i = 0; i < MAX_WORKERS; ++i) sem_post(shm->sem[SEM_JOB + i]);
}

static int shm_stopped(Link *link)
{
    ShmLink *shm = link->impl;

    lock(shm);
    int stop = shm->shared->stop;
    unlock(shm);

    return stop || !link_pid_alive(shm->shared->factory_pid);
}

/* Функции конкретно для реализации shared memory */
const LinkOps link_shm = {
    .name = "shm",
    .create = shm_create,
    .attach = shm_attach,
    .close = shm_close,
    .send_job = shm_send_job,
    .wait_done = shm_wait_done,
    .request_stop = shm_request_stop,
    .wait_job = shm_wait_job,
    .report_done = shm_report_done,
    .stopped = shm_stopped,
};
