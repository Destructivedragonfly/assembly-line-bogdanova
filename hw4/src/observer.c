#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "observer.h"


#define MAX_CLIENTS 32 /* макс клиентов */
#define HISTORY_SIZE 300 /* количество сохранённых событий */
#define LINE_SIZE 512 /* размер буфера */
#define HISTORY_SEND_TIMEOUT_SEC 1 /* сколько ждать клиента при отправке истории */

static int listen_fd = -1;
static int clients[MAX_CLIENTS]; /* сокеты клиентов, -1 - свободное место */
static char history[HISTORY_SIZE][LINE_SIZE]; /* кольцо последних событий, каждое с '\n' на конце */
static int history_count;
static int history_pos;
static atomic_int running;
static pthread_t thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;


/* Отправить данные целиком */
static int send_all(int fd, const char *data, size_t len, int flags)
{
    return send(fd, data, len, flags | MSG_NOSIGNAL) == (ssize_t)len; /* 1 - всё ушло*/
}

/* Отправить новому клиенту последние события, чтобы он видел контекст */
static int send_history(int fd)
{
    /* клиент, который не читает, не должен надолго остановить моделирование */
    struct timeval tv = { HISTORY_SEND_TIMEOUT_SEC, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int start = (history_pos - history_count + HISTORY_SIZE) % HISTORY_SIZE;

    for (int i = 0; i < history_count; ++i) {
        const char *s = history[(start + i) % HISTORY_SIZE];
        if (!send_all(fd, s, strlen(s), 0)) return 0;
    }

    return 1;
}

/* Поток принимает новых наблюдателей */
static void *accept_clients(void *unused)
{
    (void)unused;

    while (running) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (!running) break; /* сервер останавливается */
            continue;
        }

        pthread_mutex_lock(&lock);

        /* ищем свободное место для клиента */
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (clients[i] < 0) {
                slot = i;
                break;
            }
        }

        if (slot < 0) {
            send_all(fd, "SERVER_BUSY\n", 12, MSG_DONTWAIT);
            close(fd);
        }
        else if (send_history(fd)) {
            clients[slot] = fd;
        }
        else {
            close(fd); /* клиент пропал, пока ему отправляли историю */
        }

        pthread_mutex_unlock(&lock);
    }

    return NULL;
}

/* Запустить TCP-сервер наблюдения */
int observer_start(const char *ip, const char *port)
{
    struct addrinfo hints, *res = NULL, *p;

    for (int i = 0; i < MAX_CLIENTS; ++i) clients[i] = -1;
    history_count = 0;
    history_pos = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    /* 1 - успешно, 0 - ошибка */
    if (getaddrinfo(ip, port, &hints, &res) != 0) {
        errno = EINVAL;
        return 0;
    }

    /* берём первый адрес, на котором получилось занять порт */
    for (p = res; p; p = p->ai_next) {
        listen_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (listen_fd < 0) continue;

        int yes = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        if (bind(listen_fd, p->ai_addr, p->ai_addrlen) == 0 && listen(listen_fd, 16) == 0) {
            break;
        }

        close(listen_fd);
        listen_fd = -1;
    }

    freeaddrinfo(res);

    if (listen_fd < 0) return 0;

    running = 1;
    if (pthread_create(&thread, NULL, accept_clients, NULL) != 0) {
        running = 0;
        close(listen_fd);
        listen_fd = -1;
        return 0;
    }

    return 1;
}

/* Запомнить событие в истории и разослать его всем клиентам */
void observer_log(const char *line)
{
    if (!running) return;

    pthread_mutex_lock(&lock);

    char *entry = history[history_pos];
    int len = snprintf(entry, LINE_SIZE, "%s\n", line);
    
    if (len >= LINE_SIZE) {
        len = LINE_SIZE - 1;
        entry[len - 1] = '\n'; /* слишком длинную строку обрезаем, но конец строки сохраняем */
    }

    history_pos = (history_pos + 1) % HISTORY_SIZE;
    if (history_count < HISTORY_SIZE) history_count++;

    /* медленного или отключившегося клиента отсоединяем, остальные продолжают получать события */
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i] < 0) continue;

        if (!send_all(clients[i], entry, (size_t)len, MSG_DONTWAIT)) {
            close(clients[i]);
            clients[i] = -1;
        }
    }

    pthread_mutex_unlock(&lock);
}

/* Остановить приём клиентов и сообщить им о завершении сервера */
void observer_stop(void)
{
    if (!running) return;
    running = 0;

    /* shutdown будит поток, ждущий в accept, сокет закрываем только после завершения потока */
    shutdown(listen_fd, SHUT_RDWR);
    pthread_join(thread, NULL);
    close(listen_fd);
    listen_fd = -1;

    pthread_mutex_lock(&lock);

    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i] < 0) continue;

        send_all(clients[i], "SERVER_STOP\n", 12, MSG_DONTWAIT);
        shutdown(clients[i], SHUT_RDWR);
        close(clients[i]);
        clients[i] = -1;
    }

    pthread_mutex_unlock(&lock);
}
