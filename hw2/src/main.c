#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "headers/params.h"
#include "headers/process.h"


int main(int argc, char **argv)
{
    unsigned int seed;
    if (argc > 1) {
        seed = (unsigned int)strtoul(argv[1], NULL, 10);
    }
    else {
        seed = (unsigned int)time(NULL);
    }

    srand(seed);

    static Process proc; /* static, потому что после выхода из main стек не годен */
    process_init(&proc);
    proc.seed = seed;

    /* необязательный второй аргумент: реальных мс на единицу модельного времени */
    if (argc > 2) {
        long delay = strtol(argv[2], NULL, 10);
        proc.delay_ms = (int)(delay < 1 ? 1 : delay > 10000 ? 10000 : delay);
    }

    dprintf(1, "Assembly line simulation\n");
    dprintf(1, "Plan: %d products, seed=%u\n\n", PLAN, seed);

    process_run(&proc);
    process_print_stats(&proc);

    return 0;
}
