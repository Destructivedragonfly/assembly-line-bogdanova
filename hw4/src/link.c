#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "link.h"
#include "worker.h"


/* Сообщение между фабрикой и работником */
typedef struct {
    int command; /* команда*/
    long duration; /* длительность */
} Message;

/* Команды: задание, задание выполнено, остановка */
enum { MSG_JOB = 1, MSG_DONE, MSG_STOP };


/* Отправить сообщение работнику или фабрике */
static int send_message(int fd, int command, long duration)
{
    Message m;
    memset(&m, 0, sizeof(m)); /* чтобы в сокет не попал мусор из выравнивания */
    m.command = command;
    m.duration = duration;

    return write(fd, &m, sizeof(m)) == (ssize_t)sizeof(m); /* 1 - отправлено целиком */
}

/* Создать процессы работников, каждый связан с фабрикой парой сокетов */
int link_start_workers(Link *link)
{
    for (int i = 0; i < MAX_WORKERS; ++i) link->fd[i] = -1;
    link->stopped = 0;

    for (int i = 0; i < MAX_WORKERS; ++i) {
        int pair[2];

        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) {
            link_request_stop(link);
            link_close_workers(link);
            return 0;
        }

        pid_t pid = fork();
        if (pid < 0) {
            close(pair[0]);
            close(pair[1]);
            link_request_stop(link);
            link_close_workers(link);
            return 0;
        }

        if (pid == 0) {
            /* дочерний процесс: оставляем только свой конец сокета */
            close(pair[0]);
            for (int j = 0; j < i; ++j) {
                if (link->fd[j] >= 0) close(link->fd[j]);
            }

            Link child;
            for (int j = 0; j < MAX_WORKERS; ++j) child.fd[j] = -1;
            child.fd[i] = pair[1];
            child.stopped = 0;

            worker_run(&child, i, NULL);
            close(pair[1]);
            _exit(0);
        }

        /* родительский процесс: запоминаем свой конец сокета */
        close(pair[1]);
        link->fd[i] = pair[0];
    }
    return 1;
}

/* Фабрика назначает работнику задание */
void link_send_job(Link *link, int worker_id, long duration_ms)
{
    if (worker_id < 0 || worker_id >= MAX_WORKERS || link->fd[worker_id] < 0) return;

    if (!send_message(link->fd[worker_id], MSG_JOB, duration_ms)) {
        link->stopped = 1;
    }
}

/* Фабрика ждёт, пока какой-нибудь работник закончит */
int link_wait_done(Link *link, int timeout_ms, int *worker_id)
{
    struct pollfd p[MAX_WORKERS];

    for (int i = 0; i < MAX_WORKERS; ++i) {
        p[i].fd = link->fd[i];
        p[i].events = POLLIN | POLLHUP;
        p[i].revents = 0;
    }

    int n = poll(p, MAX_WORKERS, timeout_ms);
    if (n < 0) return errno == EINTR ? 0 : -1;
    if (n == 0) return 0;

    /* 1 - есть, 0 - таймаут, -1 - ошибка */
    for (int i = 0; i < MAX_WORKERS; ++i) {
        if (!p[i].revents) continue;

        Message m;
        if (read(link->fd[i], &m, sizeof(m)) != (ssize_t)sizeof(m) || m.command != MSG_DONE) {
            link->stopped = 1; /* работник умер или прислал не то */
            return -1;
        }

        *worker_id = i;
        return 1;
    }
    return 0;
}

/* Работник ждёт задание: 1 - получено, 0 - таймаут, -1 - остановка или ошибка */
int link_wait_job(Link *link, int worker_id, int timeout_ms, long *duration_ms)
{
    struct pollfd p = { link->fd[worker_id], POLLIN | POLLHUP, 0 };

    int n = poll(&p, 1, timeout_ms);
    if (n < 0) return errno == EINTR ? 0 : -1;
    if (n == 0) return 0;

    Message m;
    if (read(link->fd[worker_id], &m, sizeof(m)) != (ssize_t)sizeof(m)) return -1;
    if (m.command != MSG_JOB) return -1; /* MSG_STOP или неизвестная команда */

    *duration_ms = m.duration;
    return 1;
}

/* Работник сообщает фабрике, что закончил задание */
int link_report_done(Link *link, int worker_id)
{
    return send_message(link->fd[worker_id], MSG_DONE, 0);
}

/* Нужно ли остановиться */
int link_stopped(const Link *link)
{
    return link->stopped;
}

/* Попросить всех работников остановиться */
void link_request_stop(Link *link)
{
    for (int i = 0; i < MAX_WORKERS; ++i) {
        if (link->fd[i] >= 0) send_message(link->fd[i], MSG_STOP, 0);
    }
}

/* Закрыть связь с работниками и дождаться их завершения */
void link_close_workers(Link *link)
{
    for (int i = 0; i < MAX_WORKERS; ++i) {
        if (link->fd[i] >= 0) {
            close(link->fd[i]);
            link->fd[i] = -1;
        }
    }

    while (waitpid(-1, NULL, 0) > 0) {}
}
