// philosophers_ordered.cpp - Dining Philosophers, corrected by resource ordering: each
// philosopher picks up the LOWER-numbered fork first. The lock-order graph is then acyclic
// (fork[i] -> fork[j] only for i < j), so no deadlock is possible. Expected: no warnings.
#include <pthread.h>
#include <cstdio>

#define N 5
pthread_mutex_t forks[N];
static int meals[N];

void* philosopher(void* arg) {
    long i = (long)arg;
    int left = i, right = (i + 1) % N;
    int first = left < right ? left : right, second = left < right ? right : left;
    for (int k = 0; k < 2000; k++) {
        pthread_mutex_lock(&forks[first]);
        pthread_mutex_lock(&forks[second]);
        meals[i]++;
        pthread_mutex_unlock(&forks[second]);
        pthread_mutex_unlock(&forks[first]);
    }
    return nullptr;
}

int main() {
    for (int i = 0; i < N; i++) pthread_mutex_init(&forks[i], nullptr);
    pthread_t t[N];
    for (long i = 0; i < N; i++) pthread_create(&t[i], nullptr, philosopher, (void*)i);
    for (int i = 0; i < N; i++) pthread_join(t[i], nullptr);
    for (int i = 0; i < N; i++) pthread_mutex_destroy(&forks[i]);
    int total = 0;
    for (int i = 0; i < N; i++) total += meals[i];
    printf("meals eaten: %d\n", total);
    return 0;
}
