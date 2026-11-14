// trylock_backoff.cpp - opposite lock orders made safe with trylock: the second lock is only
// TRIED; if it is busy, the thread releases the first lock and retries. Nobody ever blocks while
// holding a lock, so no deadlock is possible. LockSpy adds no lock-order edge for a trylock
// (it cannot block). Expected: no warnings.
#include <pthread.h>
#include <sched.h>
#include <cstdio>

pthread_mutex_t A = PTHREAD_MUTEX_INITIALIZER, B = PTHREAD_MUTEX_INITIALIZER;
static int work = 0;

static void take_both(pthread_mutex_t* first, pthread_mutex_t* second) {
    for (;;) {
        pthread_mutex_lock(first);
        if (pthread_mutex_trylock(second) == 0) return;
        pthread_mutex_unlock(first);    // back off and retry
        sched_yield();
    }
}

void* t1(void*) {
    for (int i = 0; i < 2000; i++) { take_both(&A, &B); work++; pthread_mutex_unlock(&B); pthread_mutex_unlock(&A); }
    return nullptr;
}

void* t2(void*) {
    for (int i = 0; i < 2000; i++) { take_both(&B, &A); work++; pthread_mutex_unlock(&A); pthread_mutex_unlock(&B); }
    return nullptr;
}

int main() {
    pthread_t a, b;
    pthread_create(&a, nullptr, t1, nullptr);
    pthread_create(&b, nullptr, t2, nullptr);
    pthread_join(a, nullptr);
    pthread_join(b, nullptr);
    printf("work = %d\n", work);
    return 0;
}
