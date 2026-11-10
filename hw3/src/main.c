#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "process.h"
#include "worker.h"


#define ATTACH_TIMEOUT_MS 30000 /* сколько независимый работник ждёт появления фабрики */

/* Четыре режима собираются из трёх опций */
typedef struct {
    const char *name;
    const SemKind *sems; /* какие семафоры */
    const LinkOps *link; /* как фабрика и работники обмениваются данными */
    int independent; /* работников запускают отдельно (иначе их создаёт fork) */
} Mode;

static const Mode modes[] = {
    {"named", &sem_named, &link_shm, 0},
    {"unnamed", &sem_unnamed, &link_shm, 0},
    {"shm", &sem_named, &link_shm, 1},
    {"mq", &sem_named, &link_mq, 1},
};

#define MODE_COUNT ((int)(sizeof(modes) / sizeof(modes[0])))

static volatile sig_atomic_t interrupted = 0;


static void handle_signal(int signal_number)
{
    (void)signal_number;
    interrupted = 1;
}

static int install_signals(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0; /* без SA_RESTART блокирующие вызовы вернутся с EINTR и увидят флаг */

    if (sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1) {
        perror("sigaction");
        return 0;
    }

    return 1;
}

static void wait_for_child(pid_t pid)
{
    if (pid <= 0) return;

    while (waitpid(pid, NULL, 0) == -1) {
        if (errno == EINTR) continue;
        perror("waitpid");
        break;
    }
}

/* Префиксы имён ресурсов (для опций, когда они нужны) */
static void resource_prefix(const Mode *mode, char *prefix, size_t size)
{
    if (mode->independent) snprintf(prefix, size, "/line");
    else snprintf(prefix, size, "/line_%d", (int)getpid());
}

/* Независимо запущенный работник подключается к фабрике и выполняет её задания */
static int run_worker(const Mode *mode, int worker_id)
{
    char prefix[48];
    resource_prefix(mode, prefix, sizeof(prefix));

    /* пока не подключились, сигналы действуют по умолчанию */
    Link link;
    if (!link_attach(&link, mode->link, mode->sems, prefix, ATTACH_TIMEOUT_MS)) return 1;
    if (!install_signals()) {
        link_close(&link);
        return 1;
    }

    dprintf(1, "worker %d connected\n", worker_id);
    worker_run(&link, worker_id, &interrupted);
    dprintf(1, "worker %d finished\n", worker_id);

    link_close(&link);
    return 0;
}

/* Фабрика создаёт связь, запускает работников (или ждёт независимых), ведёт модель, убирает за собой */
static int run_factory(const Mode *mode, unsigned int seed, int delay_ms)
{
    if (!install_signals()) return 1;

    char prefix[48];
    resource_prefix(mode, prefix, sizeof(prefix));

    Link link;
    if (!link_create(&link, mode->link, mode->sems, prefix)) return 1;

    static Process proc; /* static, потому что после выхода из main стек не годен */
    process_init(&proc, seed, delay_ms);
    srand(seed);

    dprintf(1, "Assembly line simulation\n");
    dprintf(1, "Plan: %d products, seed=%u, mode=%s\n", PLAN, seed, mode->name);

    pid_t children[MAX_WORKERS] = {0};
    int child_count = 0;

    if (mode->independent) {
        dprintf(1, "Workers: waiting for %d independent processes (--mode %s --role worker --id 0..%d)\n\n",
                MAX_WORKERS, mode->name, MAX_WORKERS - 1);
    } 
    
    else {
        dprintf(1, "Workers: %d child processes\n\n", MAX_WORKERS);

        for (int worker_id = 0; worker_id < MAX_WORKERS; ++worker_id) {
            pid_t pid = fork();
            if (pid == -1) {
                perror("fork");
                break;
            }
            if (pid == 0) {
                /* остановкой дочерних процессов управляет только родитель */
                signal(SIGINT, SIG_IGN);
                signal(SIGTERM, SIG_IGN);
                worker_run(&link, worker_id, NULL);
                _exit(0);
            }
            children[worker_id] = pid;
            child_count++;
        }

        if (child_count != MAX_WORKERS) {
            process_stop_workers(&proc, &link);
            for (int i = 0; i < child_count; ++i) wait_for_child(children[i]);
            link_close(&link);
            return 1;
        }
    }

    process_run(&proc, &link, &interrupted);
    process_stop_workers(&proc, &link);

    for (int i = 0; i < MAX_WORKERS; ++i) wait_for_child(children[i]);

    process_print_stats(&proc);
    link_close(&link);
    return 0;
}

static int usage(const char *prog)
{
    dprintf(2, "usage: %s [--mode ", prog);

    for (int i = 0; i < MODE_COUNT; ++i) {
        dprintf(2, "%s%s", i ? "|" : "", modes[i].name);
    }

    dprintf(2, "] [--seed N] [--delay MS]\n%s --mode shm|mq --role worker --id 0..%d\n", prog, MAX_WORKERS - 1);
    return 2;
}

static const Mode *find_mode(const char *name)
{
    for (int i = 0; i < MODE_COUNT; ++i) {
        if (strcmp(modes[i].name, name) == 0) return &modes[i];
    }

    return NULL;
}

int main(int argc, char **argv)
{
    const Mode *mode = &modes[0];
    unsigned int seed = (unsigned int)time(NULL);
    int delay_ms = 100; /* реальных мс на единицу модельного времени */
    int is_worker = 0;
    int worker_id = -1;

    for (int i = 1; i < argc; i += 2) {
        const char *value = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (value == NULL) return usage(argv[0]);

        if (strcmp(argv[i], "--mode") == 0) {
            mode = find_mode(value);
            if (mode == NULL) return usage(argv[0]);
        }
        else if (strcmp(argv[i], "--role") == 0) {
            if (strcmp(value, "worker") != 0 && strcmp(value, "factory") != 0) return usage(argv[0]);
            is_worker = strcmp(value, "worker") == 0;
        }
        else if (strcmp(argv[i], "--id") == 0) {
            worker_id = atoi(value);
        }
        else if (strcmp(argv[i], "--seed") == 0) {
            seed = (unsigned int)strtoul(value, NULL, 10);
        }
        else if (strcmp(argv[i], "--delay") == 0) {
            long delay = strtol(value, NULL, 10);
            delay_ms = (int)(delay < 1 ? 1 : delay > 10000 ? 10000 : delay);
        }
        else {
            return usage(argv[0]);
        }
    }

    if (!is_worker) return run_factory(mode, seed, delay_ms);

    if (!mode->independent || worker_id < 0 || worker_id >= MAX_WORKERS) return usage(argv[0]);

    return run_worker(mode, worker_id);
}
