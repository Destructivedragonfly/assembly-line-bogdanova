#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "process.h"
#include "observer.h"


/* Записать событие в консоль и передать его наблюдателям */
void event_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    /* убрать лишние символы переноса строки */
    size_t n = strlen(line);
    while (n > 0 && line[n - 1] == '\n') {
        line[--n] = '\0';
    }

    dprintf(1, "%s\n", line);

    observer_log(line);
}
