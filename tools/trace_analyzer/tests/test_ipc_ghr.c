#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

volatile int* global_ptr = NULL;

void __attribute__((noinline)) helper_push(int share) {
    volatile int* local_ptr = NULL;
    
    // We use inline assembly to do an explicit push/pop cycle.
    // We save RSP to local_ptr to publish the stack address.
    asm volatile (
        "push %%rax\n\t"
        "mov %%rsp, %0\n\t"
        : "=r" (local_ptr)
        :
        : "rax", "memory"
    );

    if (share) {
        // Publish the stack address
        global_ptr = (int*)local_ptr;
        // Wait for thread 2 to acknowledge the read
        while (global_ptr != NULL) {
            usleep(100);
        }
    }

    asm volatile (
        "pop %%rax\n\t"
        ::: "rax", "memory"
    );
}

// path_a loop control flow to ensure a distinct GHR history
void __attribute__((noinline)) path_a() {
    volatile int x = 0;
    for (int i = 0; i < 15; i++) {
        x += i;
    }
    helper_push(0);
}

// path_b conditional branch control flow to ensure a distinct GHR history
void __attribute__((noinline)) path_b() {
    volatile int x = 0;
    if (x == 0) {
        x = 99;
    } else {
        x = 100;
    }
    helper_push(1);
}

void* worker1(void* arg) {
    // Run path_a (private context) multiple times
    for (int i = 0; i < 10; i++) {
        path_a();
    }
    // Run path_b (shared context) once
    path_b();
    return NULL;
}

void* worker2(void* arg) {
    // Wait for the pointer to be published
    while (global_ptr == NULL) {
        usleep(100);
    }
    
    // Read the stack location (causes IPC)
    int val = *global_ptr;
    (void)val;
    
    // Acknowledge read
    global_ptr = NULL;
    
    return NULL;
}

int main() {
    pthread_t t1, t2;
    pthread_create(&t1, NULL, worker1, NULL);
    pthread_create(&t2, NULL, worker2, NULL);
    
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    
    printf("test_ipc_ghr finished\n");
    return 0;
}
