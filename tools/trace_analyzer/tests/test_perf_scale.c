#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#define CHURN_ITERATIONS 50000

__attribute__((noinline)) void dummy_churn() {
    volatile int a = 1;
    volatile int b = 2;
    volatile int c = a + b;
    (void)c;
}

__attribute__((noinline)) void deep_churn(int depth) {
    if (depth <= 0) return;
    volatile int local = depth;
    dummy_churn();
    deep_churn(depth - 1);
    local = local + 1;
}

void* worker(void* arg) {
    for (int i = 0; i < CHURN_ITERATIONS; i++) {
        deep_churn(5);
    }
    return NULL;
}

double get_time() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

int main(int argc, char* argv[]) {
    int num_threads = 1;
    if (argc > 1) {
        num_threads = atoi(argv[1]);
    }
    
    pthread_t* threads = malloc(num_threads * sizeof(pthread_t));
    if (!threads) {
        fprintf(stderr, "Failed to allocate memory for threads\n");
        return 1;
    }
    
    double start = get_time();
    
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&threads[i], NULL, worker, NULL) != 0) {
            fprintf(stderr, "Failed to create thread %d\n", i);
            return 1;
        }
    }
    
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    
    double end = get_time();
    printf("%.4f\n", end - start);
    
    free(threads);
    return 0;
}
