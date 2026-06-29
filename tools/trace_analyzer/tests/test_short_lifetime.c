#include <stdio.h>

void __attribute__((noinline)) do_short() {
    asm volatile (
        "push %%rbx\n\t"
        "pop %%rbx\n\t"
        ::: "memory"
    );
}

int main() {
    do_short();
    printf("test_short_lifetime finished\n");
    return 0;
}
