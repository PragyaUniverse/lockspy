// single_thread_both_orders.cpp - one thread takes A then B, and later B then A. A deadlock
// needs at least two threads waiting for each other, and only this thread ever uses A and B,
// so it is safe. A naive detector reports the cycle; LockSpy needs edges from at least two
// different threads. Expected: no warnings.
#include <pthread.h>
#include <cstdio>

pthread_mutex_t A = PTHREAD_MUTEX_INITIALIZER, B = PTHREAD_MUTEX_INITIALIZER;

void* only_user(void*) {
    int x = 0;
    for (int i = 0; i < 100; i++) {
        pthread_mutex_lock(&A); pthread_mutex_lock(&B); x++; pthread_mutex_unlock(&B); pthread_mutex_unlock(&A);
        pthread_mutex_lock(&B); pthread_mutex_lock(&A); x++; pthread_mutex_unlock(&A); pthread_mutex_unlock(&B);
    }
    printf("x = %d\n", x);
    return nullptr;
}

int main() {
    pthread_t t;
    pthread_create(&t, nullptr, only_user, nullptr);
    pthread_join(t, nullptr);
    return 0;
}
