// hello_threads.cpp - Phase 1 smoke test: two threads lock and unlock one mutex a few times.
// Run with LOCKSPY_TRACE=1 to see LockSpy's raw event trace. Expected: no findings.
#include <pthread.h>
#include <cstdio>

static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
static int counter = 0;

static void* worker(void*) {
    for (int i = 0; i < 3; i++) {
        pthread_mutex_lock(&counter_lock);
        counter++;
        pthread_mutex_unlock(&counter_lock);
    }
    return nullptr;
}

int main() {
    pthread_t a, b;
    pthread_create(&a, nullptr, worker, nullptr);
    pthread_create(&b, nullptr, worker, nullptr);
    pthread_join(a, nullptr);
    pthread_join(b, nullptr);
    printf("counter = %d\n", counter);
    return 0;
}
