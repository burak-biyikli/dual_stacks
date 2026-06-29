#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// Global pointer to point to a stack variable
volatile int* global_ptr = NULL;

void* worker1(void* arg) {
    int secret = 42;
    global_ptr = &secret;
    
    // Wait for thread 2 to read it
    while (global_ptr != NULL) {
        usleep(100);
    }
    
    return NULL;
}

void* worker2(void* arg) {
    // Wait for thread 1 to publish the pointer
    while (global_ptr == NULL) {
        usleep(100);
    }
    
    // Read the stack variable belonging to thread 1 (causes IPC)
    int val = *global_ptr;
    
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
    
    printf("test_ipc finished\n");
    return 0;
}
