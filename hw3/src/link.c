#include <errno.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include "link.h"


int link_create(Link *link, const LinkOps *ops, const SemKind *sems, const char *prefix)
{
    memset(link, 0, sizeof(*link));
    link->ops = ops;

    return ops->create(link, sems, prefix);
}

int link_attach(Link *link, const LinkOps *ops, const SemKind *sems, const char *prefix, int timeout_ms)
{
    memset(link, 0, sizeof(*link));
    link->ops = ops;

    return ops->attach(link, sems, prefix, timeout_ms);
}

void link_close(Link *link) { link->ops->close(link); }

int link_send_job(Link *link, int worker_id, long duration_ms) { return link->ops->send_job(link, worker_id, duration_ms); }
int link_wait_done(Link *link, int timeout_ms, int *worker_id) { return link->ops->wait_done(link, timeout_ms, worker_id); }
void link_request_stop(Link *link) { link->ops->request_stop(link); }

int link_wait_job(Link *link, int worker_id, int timeout_ms, long *duration_ms) { return link->ops->wait_job(link, worker_id, timeout_ms, duration_ms); }
int link_report_done(Link *link, int worker_id) { return link->ops->report_done(link, worker_id); }
int link_stopped(Link *link) { return link->ops->stopped(link); }

void link_deadline(struct timespec *deadline, int timeout_ms)
{
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += timeout_ms / 1000;
    deadline->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;

    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_nsec -= 1000000000L;
        deadline->tv_sec++;
    }
}

/* Процесс жив? Проверяем сигналом 0 */
int link_pid_alive(int pid)
{
    return pid > 0 && !(kill(pid, 0) == -1 && errno == ESRCH);
}
