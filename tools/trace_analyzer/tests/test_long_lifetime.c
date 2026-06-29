#include <stdio.h>

void __attribute__((noinline)) do_long() {
    asm volatile (
        "push %%rbx\n\t"
        "mov $5000, %%rcx\n"
        "1:\n\t"
        "add $1, %%rbx\n\t"
        "sub $1, %%rcx\n\t"
        "jnz 1b\n\t"
        "pop %%rbx\n\t"
        ::: "rcx", "rbx", "memory", "cc"
    );
}

int main() {
    do_long();
    printf("test_long_lifetime finished\n");
    return 0;
}
