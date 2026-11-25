#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


/* Вывод формата запуска программы */
static void usage(const char *name)
{
    fprintf(stderr, "Usage: %s -a SERVER_IP -p PORT\n", name);
}

int main(int argc, char **argv)
{
    const char *ip = NULL;
    const char *port = NULL;

    /* Разбираем аргументы командной строки */
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-a") && i + 1 < argc) {
            ip = argv[++i];
        }
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            port = argv[++i];
        }
        else {
            usage(argv[0]);
            return 1;
        }
    }

    if (!ip || !port) {
        usage(argv[0]);
        return 1;
    }

    /* Получаем адрес сервера */
    struct addrinfo hints, *res = NULL, *p;
    memset(&hints, 0, sizeof(hints));

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int err = getaddrinfo(ip, port, &hints, &res);
    if (err != 0) {
        fprintf(stderr, "Invalid server address: %s\n", gai_strerror(err));
        return 1;
    }

    /* Попытка подключиться к серверу */
    int fd = -1;

    for (p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;

        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            break;
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);

    if (fd < 0) {
        perror("Connection failed");
        return 1;
    }

    printf("Connected to %s:%s. Waiting for events...\n", ip, port);

    /* Получаем события от сервера */
    char buf[1024];
    char line[1024];
    size_t pos = 0;
    int stop = 0;

    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);

        if (n == 0) {
            printf("Server disconnected\n");
            break;
        }

        if (n < 0) {
            if (errno == EINTR) continue;

            perror("Connection error");
            break;
        }

        /* Разбиваем поток байтов на строки по символу переноса */
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                line[pos] = '\0';

                if (pos > 0 && line[pos - 1] == '\r') {
                    line[--pos] = '\0'; /* уборка символа возврата каретки */
                }

                printf("%s\n", line);
                fflush(stdout);

                /* Завершаем работу после остановки сервера */
                if (!strcmp(line, "SERVER_STOP")) {
                    stop = 1;
                    break;
                }

                pos = 0;
            }
            else {
                if (pos >= sizeof(line) - 1) {
                    fprintf(stderr, "Received message is too long\n");
                    stop = 1;
                    break;
                }

                line[pos++] = buf[i];
            }
        }

        if (stop) break;
    }

    close(fd);
    return 0;
}
