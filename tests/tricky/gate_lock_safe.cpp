// gate_lock_safe.cpp - looks like AB-BA, but is safe. Both threads first take the gate lock G,
// so only one of them can be between "lock G" and "unlock G" at a time; the opposite orders of
// A and B can never overlap. A naive lock-order detector reports A->B->A; LockSpy sees that
// both edges were taken while holding G and suppresses it. Expected: no warnings.
#include <pthread.h>
#include <cstdio>

pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER, A = PTHREAD_MUTEX_INITIALIZER, B = PTHREAD_MUTEX_INITIALIZER;
static int x = 0;

void* forward(void*) {
    for (int i = 0; i < 1000; i++) {
        pthread_mutex_lock(&G);
        pthread_mutex_lock(&A);
        pthread_mutex_lock(&B);
        x++;
        pthread_mutex_unlock(&B);
        pthread_mutex_unlock(&A);
        pthread_mutex_unlock(&G);
    }
    return nullptr;
}

void* backward(void*) {
    for (int i = 0; i < 1000; i++) {
        pthread_mutex_lock(&G);
        pthread_mutex_lock(&B);
        pthread_mutex_lock(&A);
        x--;
        pthread_mutex_unlock(&A);
        pthread_mutex_unlock(&B);
        pthread_mutex_unlock(&G);
    }
    return nullptr;
}

int main() {
    pthread_t a, b;
    pthread_create(&a, nullptr, forward, nullptr);
    pthread_create(&b, nullptr, backward, nullptr);
    pthread_join(a, nullptr);
    pthread_join(b, nullptr);
    printf("x = %d\n", x);
    return 0;
}
