#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void* worker1(void* arg) {
    volatile int x = 123;
    (void)x;
    return NULL;
}

void* worker2(void* arg) {
    volatile int x = 456;
    (void)x;
    return NULL;
}

int main() {
    pthread_t t1;
    if (pthread_create(&t1, NULL, worker1, NULL) != 0) {
        fprintf(stderr, "Failed to create thread 1\n");
        return 1;
    }
    pthread_join(t1, NULL);
    
    // Sleep briefly to ensure thread cleanup is fully processed by the OS
    usleep(1000);
    
    // Create second thread immediately after first one exits
    // This makes it highly likely the stack memory is reused.
    pthread_t t2;
    if (pthread_create(&t2, NULL, worker2, NULL) != 0) {
        fprintf(stderr, "Failed to create thread 2\n");
        return 1;
    }
    pthread_join(t2, NULL);
    
    printf("test_stack_reuse finished\n");
    return 0;
}
