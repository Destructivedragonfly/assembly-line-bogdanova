#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "process.h"
#include "link.h"
#include "observer.h"


static volatile sig_atomic_t interrupted;

/* Ctrl+C или SIGTERM: только поднимаем флаг, остальное делает основной цикл */
static void on_signal(int sig)
{
    (void)sig;
    interrupted = 1;
}

/* Вывод формата запуска программы */
static void usage(const char *name)
{
    fprintf(stderr, "Usage: %s -a IP -p PORT [-d DELAY_MS]\n", name);
}

int main(int argc, char **argv)
{
    const char *ip = "0.0.0.0";
    const char *port = NULL;
    int delay = 100; /* реальных миллисекунд на единицу модельного времени */

    /* Разбираем аргументы командной строки */
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-a") && i + 1 < argc) {
            ip = argv[++i];
        }
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            port = argv[++i];
        }
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) {
            delay = atoi(argv[++i]);
        }
        else {
            usage(argv[0]);
            return 1;
        }
    }

    if (!port || delay < 1) {
        usage(argv[0]);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN); /* оборванное соединение не должно убивать сервер */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Работники создаются до TCP-сервера, чтобы не наследовать его сокеты */
    Link link;
    if (!link_start_workers(&link)) {
        fprintf(stderr, "Failed to start workers\n");
        return 1;
    }

    if (!observer_start(ip, port)) {
        perror("Failed to start TCP server");
        link_request_stop(&link);
        link_close_workers(&link);
        return 1;
    }

    printf("Server started: %s:%s\n", ip, port);

    /* Моделирование линии */
    Process proc;
    process_init(&proc, (unsigned)time(NULL), delay);
    event_log("Line started; plan: %d products", PLAN);

    process_run(&proc, &link, &interrupted);

    /* Завершение: останавливаем работников, выводим итог, прощаемся с клиентами */
    process_stop_workers(&proc, &link);
    link_close_workers(&link);
    process_print_stats(&proc);
    event_log("Line finished: produced %d of %d", proc.products_done, PLAN);

    observer_stop();

    return proc.products_done == PLAN ? 0 : 2;
}