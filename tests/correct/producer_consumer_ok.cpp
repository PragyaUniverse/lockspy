// producer_consumer_ok.cpp - the bounded buffer from producer_consumer_bad.cpp, corrected: the
// consumer's report takes buf_lock before log_lock, the same order as the producer. The
// condition-variable waits release buf_lock while sleeping; LockSpy mirrors that, so the
// sleeping consumer is never treated as the owner. Expected: no warnings.
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
    pthread_mutex_lock(&buf_lock);   // same order as the producer: buf_lock, then log_lock
    pthread_mutex_lock(&log_lock);
    printf("consumed sum=%ld, %d log entries, %d left\n", sum, logged, count);
    pthread_mutex_unlock(&log_lock);
    pthread_mutex_unlock(&buf_lock);
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
