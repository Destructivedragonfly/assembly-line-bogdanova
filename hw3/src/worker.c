#include <errno.h>
#include <time.h>

#include "worker.h"


/* Рабочий процесс выполняет назначенную работу, но по сути спит */
static void sleep_ms(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;

    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

static int was_interrupted(volatile sig_atomic_t *interrupted)
{
    return interrupted != NULL && *interrupted;
}

/* Работа короткими интервалами, чтобы между ними можно было бы заметить остановку */
static int do_job(Link *link, long remaining_ms, volatile sig_atomic_t *interrupted)
{
    while (remaining_ms > 0) {
        long piece = remaining_ms > 50 ? 50 : remaining_ms;
        sleep_ms(piece);
        remaining_ms -= piece;

        if (was_interrupted(interrupted) || link_stopped(link)) return 0;
    }

    return 1;
}

void worker_run(Link *link, int worker_id, volatile sig_atomic_t *interrupted)
{
    while (true) {
        long duration_ms = 0;
        int result = link_wait_job(link, worker_id, 100, &duration_ms);

        if (was_interrupted(interrupted)) break;
        if (result < 0 || link_stopped(link)) break;
        if (result == 0) continue;

        if (!do_job(link, duration_ms, interrupted)) break;
        if (!link_report_done(link, worker_id)) break;
    }

    if (was_interrupted(interrupted)) link_request_stop(link); /* прервали вручную, остановить всю модель */
}
