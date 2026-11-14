// bad_unlock.cpp - unlocking a mutex the thread does not hold.
//  1. main unlocks never_locked, which nobody ever locked.
//  2. main unlocks owned_by_worker while the worker thread owns it.
// Expected: BAD_UNLOCK (twice). The program keeps running afterwards.
#include <pthread.h>
#include <cstdio>

pthread_mutex_t never_locked = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t owned_by_worker = PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t locked, unlocked_by_main;

void* worker(void*) {
    pthread_mutex_lock(&owned_by_worker);
    pthread_barrier_wait(&locked);
    pthread_barrier_wait(&unlocked_by_main);
    pthread_mutex_unlock(&owned_by_worker);
    return nullptr;
}

int main() {
    pthread_mutex_unlock(&never_locked);          // bug 1
    pthread_barrier_init(&locked, nullptr, 2);
    pthread_barrier_init(&unlocked_by_main, nullptr, 2);
    pthread_t t;
    pthread_create(&t, nullptr, worker, nullptr);
    pthread_barrier_wait(&locked);
    pthread_mutex_unlock(&owned_by_worker);       // bug 2: not the owner
    pthread_barrier_wait(&unlocked_by_main);
    pthread_join(t, nullptr);
    printf("done\n");
    return 0;
}
