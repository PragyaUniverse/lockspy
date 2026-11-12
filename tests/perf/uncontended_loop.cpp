// uncontended_loop.cpp - 4 threads, each locks and unlocks its OWN mutex in a tight loop.
// There is no contention, so the run time measures the pure per-call cost of interception.
// usage: uncontended_loop [lock/unlock pairs per thread, default 1000000]
#include <pthread.h>
#include <cstdio>
#include <cstdlib>

#define T 4
pthread_mutex_t own_lock[T];
static long iters = 1000000;
static long counts[T];

void* worker(void* arg) {
    long id = (long)arg;
    for (long i = 0; i < iters; i++) {
        pthread_mutex_lock(&own_lock[id]);
        counts[id]++;
        pthread_mutex_unlock(&own_lock[id]);
    }
    return nullptr;
}

int main(int argc, char** argv) {
    if (argc > 1) iters = atol(argv[1]);
    for (int i = 0; i < T; i++) pthread_mutex_init(&own_lock[i], nullptr);
    pthread_t t[T];
    for (long i = 0; i < T; i++) pthread_create(&t[i], nullptr, worker, (void*)i);
    for (int i = 0; i < T; i++) pthread_join(t[i], nullptr);
    printf("%ld lock/unlock pairs\n", iters * T);
    return 0;
}
