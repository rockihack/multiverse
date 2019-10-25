#include <stdio.h>
#include "multiverse.h"

#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>

//#include "textdump.h"

__attribute__((multiverse)) int config_A;

void __attribute__((multiverse)) foo(void)
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

    //textdump("normal.dump");

    config_A = 1;
    multiverse_commit_refs(&config_A);
    config_A = 0;

    foo();

    long ret = syscall(1002, 1);
    printf("Migrate: %ld\n", ret);

    foo();

    return 0;
}

