// philosophers_naive.cpp - Dining Philosophers, naive version: every philosopher picks up the
// left fork, then the right fork. If all five hold their left fork at once, nobody can get a
// right fork: circular wait, deadlock.
//   ./philosophers_naive            a barrier after the first fork forces that interleaving,
//                                   so it deadlocks for sure (LIVE DEADLOCK, all 5 threads)
//   ./philosophers_naive --no-hang  philosophers eat one at a time: no hang, but the
//                                   lock-order cycle fork[0]->fork[1]->...->fork[0] is reported
#include <pthread.h>
#include <cstdio>
#include <cstring>

#define N 5
pthread_mutex_t forks[N];
static pthread_barrier_t all_hold_left;
static bool force_deadlock = true;
static int meals[N];

void* philosopher(void* arg) {
    long i = (long)arg;
    char name[16];
    snprintf(name, sizeof name, "phil-%ld", i);
    pthread_setname_np(pthread_self(), name);
    int left = i, right = (i + 1) % N;
    pthread_mutex_lock(&forks[left]);
    if (force_deadlock) pthread_barrier_wait(&all_hold_left);
    pthread_mutex_lock(&forks[right]);
    meals[i]++;
    pthread_mutex_unlock(&forks[right]);
    pthread_mutex_unlock(&forks[left]);
    return nullptr;
}

int main(int argc, char** argv) {
    force_deadlock = !(argc > 1 && strcmp(argv[1], "--no-hang") == 0);
    for (int i = 0; i < N; i++) pthread_mutex_init(&forks[i], nullptr);
    pthread_barrier_init(&all_hold_left, nullptr, N);
    pthread_t t[N];
    for (long i = 0; i < N; i++) {
        pthread_create(&t[i], nullptr, philosopher, (void*)i);
        if (!force_deadlock) pthread_join(t[i], nullptr);   // one at a time
    }
    if (force_deadlock) for (int i = 0; i < N; i++) pthread_join(t[i], nullptr);
    printf("everyone ate\n");
    return 0;
}
