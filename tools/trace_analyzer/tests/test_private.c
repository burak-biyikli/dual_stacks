#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void* worker(void* arg) {
    int local_var = 0;
    for (int i = 0; i < 1000; i++) {
        local_var += i;
    }
    return NULL;
}

int main() {
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, worker, NULL);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }
    printf("test_private finished\n");
    return 0;
}
