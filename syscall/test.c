#include <stdio.h>
#include "multiverse.h"

__attribute__((multiverse)) int config_A;

void __attribute__((multiverse)) foo()
{
    if (config_A) {
        printf("A\n");
    } else {
        printf("B\n");
    }
}

int main()
{
    multiverse_init();

    foo();

    config_A = 1;

    multiverse_commit_refs(&config_A);

    foo();

    return 0;
}
