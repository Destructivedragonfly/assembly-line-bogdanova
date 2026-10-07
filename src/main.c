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

    Process proc;
    process_init(&proc);
    proc.seed = seed;

    printf("Assembly line simulation\n");
    printf("Plan: %d products, seed=%u\n\n", PLAN, seed);

    process_run(&proc);
    process_print_stats(&proc);

    return 0;
}
