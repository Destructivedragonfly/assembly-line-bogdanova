#ifndef WORKER_H
#define WORKER_H

#include <signal.h>

#include "link.h"


/* Рабочий процесс */
void worker_run(Link *link, int worker_id, volatile sig_atomic_t *interrupted);

#endif
