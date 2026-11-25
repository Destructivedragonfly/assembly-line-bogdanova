#ifndef LINK_H
#define LINK_H

#include "params.h"


/* Связь с рабочими процессами */
typedef struct {
    int fd[MAX_WORKERS]; /* файловые дескрипторы рабочих процессов */
    int stopped; /* индикатор, нужно ли работникам остановиться */
} Link;


int link_start_workers(Link *link);
void link_close_workers(Link *link);
void link_send_job(Link *link, int worker_id, long duration_ms);
int link_wait_done(Link *link, int timeout_ms, int *worker_id);
int link_wait_job(Link *link, int worker_id, int timeout_ms, long *duration_ms);
int link_report_done(Link *link, int worker_id);
int link_stopped(const Link *link);
void link_request_stop(Link *link);

#endif
