// contention_heavy.cpp - 8 threads share three locks with very different contention:
//   hot_lock  : taken every iteration and held while doing ~2 us of work  -> heavy waiting
//   warm_lock : taken every 4th iteration, very short hold                 -> some waiting
//   cold_lock : taken every 64th iteration                                 -> almost none
// plus a counting semaphore that limits 4 threads at a time in a "connection pool".
// The contention report should rank hot_lock > warm_lock > cold_lock.
// usage: contention_heavy [iterations per thread, default 4000]
#include <pthread.h>
#include <semaphore.h>
#include <cstdio>
#include <cstdlib>

pthread_mutex_t hot_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t warm_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t cold_lock = PTHREAD_MUTEX_INITIALIZER;
static sem_t pool;
static int iters = 4000;
static volatile long hot = 0, warm = 0, cold = 0;

static void spin(int n) { for (volatile int i = 0; i < n; i++) {} }

void* worker(void*) {
    for (int i = 0; i < iters; i++) {
        pthread_mutex_lock(&hot_lock);
        hot++;
        spin(400);
        pthread_mutex_unlock(&hot_lock);
        if (i % 4 == 0) { pthread_mutex_lock(&warm_lock); warm++; pthread_mutex_unlock(&warm_lock); }
        if (i % 64 == 0) { pthread_mutex_lock(&cold_lock); cold++; pthread_mutex_unlock(&cold_lock); }
        if (i % 16 == 0) { sem_wait(&pool); spin(200); sem_post(&pool); }
        spin(100);
    }
    return nullptr;
}

int main(int argc, char** argv) {
    if (argc > 1) iters = atoi(argv[1]);
    sem_init(&pool, 0, 4);
    pthread_t t[8];
    for (int i = 0; i < 8; i++) pthread_create(&t[i], nullptr, worker, nullptr);
    for (int i = 0; i < 8; i++) pthread_join(t[i], nullptr);
    printf("hot=%ld warm=%ld cold=%ld\n", hot, warm, cold);
    return 0;
}
