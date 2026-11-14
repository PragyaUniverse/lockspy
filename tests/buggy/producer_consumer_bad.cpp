// producer_consumer_bad.cpp - bounded buffer (monitor: mutex + two condition variables) with a
// latent lock-order bug. The producer logs while holding buf_lock (buf_lock -> log_lock); the
// consumer's final report takes log_lock, then peeks at the buffer (log_lock -> buf_lock).
// In this run the report happens after the producer is done, so it never hangs, but the
// orders are inconsistent: expected POTENTIAL DEADLOCK.
#include <pthread.h>
#include <cstdio>

#define SIZE 8
#define ITEMS 2000
pthread_mutex_t buf_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t not_full = PTHREAD_COND_INITIALIZER, not_empty = PTHREAD_COND_INITIALIZER;
static int buf[SIZE], count = 0, head = 0, tail = 0, logged = 0;

static void log_event() {            // called with buf_lock held
    pthread_mutex_lock(&log_lock);
    logged++;
    pthread_mutex_unlock(&log_lock);
}

void* producer(void*) {
    for (int i = 0; i < ITEMS; i++) {
        pthread_mutex_lock(&buf_lock);
        while (count == SIZE) pthread_cond_wait(&not_full, &buf_lock);
        buf[tail] = i; tail = (tail + 1) % SIZE; count++;
        if (i % 100 == 0) log_event();
        pthread_cond_signal(&not_empty);
        pthread_mutex_unlock(&buf_lock);
    }
    return nullptr;
}

void* consumer(void*) {
    long sum = 0;
    for (int i = 0; i < ITEMS; i++) {
        pthread_mutex_lock(&buf_lock);
        while (count == 0) pthread_cond_wait(&not_empty, &buf_lock);
        sum += buf[head]; head = (head + 1) % SIZE; count--;
        pthread_cond_signal(&not_full);
        pthread_mutex_unlock(&buf_lock);
    }
    // progress report: log_lock first, then buf_lock -- the opposite order (bug)
    pthread_mutex_lock(&log_lock);
    pthread_mutex_lock(&buf_lock);
    printf("consumed sum=%ld, %d log entries, %d left\n", sum, logged, count);
    pthread_mutex_unlock(&buf_lock);
    pthread_mutex_unlock(&log_lock);
    return nullptr;
}

int main() {
    pthread_t p, c;
    pthread_create(&c, nullptr, consumer, nullptr);
    pthread_create(&p, nullptr, producer, nullptr);
    pthread_join(p, nullptr);
    pthread_join(c, nullptr);
    return 0;
}
