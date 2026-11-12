// recursive_lock.cpp - relocking a mutex the thread already holds.
//  1. A PTHREAD_MUTEX_RECURSIVE mutex locked twice: legal, must NOT be reported.
//  2. An error-checking mutex locked twice: reported as RELOCK; the real call returns EDEADLK.
//  3. A normal mutex locked twice: reported as RELOCK, then the thread waits for itself
//     forever, and the watchdog reports a LIVE DEADLOCK that the RELOCK report predicted.
#include <pthread.h>
#include <cstdio>

pthread_mutex_t rec_lock, check_lock, plain_lock;

static void inner(pthread_mutex_t* m) { pthread_mutex_lock(m); pthread_mutex_unlock(m); }

static void outer(pthread_mutex_t* m) {
    pthread_mutex_lock(m);
    inner(m);                       // second lock of the same mutex
    pthread_mutex_unlock(m);
}

int main() {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&rec_lock, &a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&check_lock, &a);
    pthread_mutex_init(&plain_lock, nullptr);

    outer(&rec_lock);
    printf("recursive mutex: ok\n");
    pthread_mutex_lock(&check_lock);
    int rc = pthread_mutex_lock(&check_lock);
    printf("error-checking mutex relock returned %d (EDEADLK)\n", rc);
    pthread_mutex_unlock(&check_lock);
    printf("normal mutex: about to self-deadlock\n");
    fflush(stdout);
    outer(&plain_lock);             // hangs forever
    printf("unreachable\n");
    return 0;
}
