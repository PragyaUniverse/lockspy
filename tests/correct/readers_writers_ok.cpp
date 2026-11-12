// readers_writers_ok.cpp - Readers-Writers corrected: every thread takes db_lock before
// stats_lock. Several readers hold db_lock (read) at the same time, and one reader re-enters
// its read lock; both are fine and must not be reported. Expected: no warnings.
#include <pthread.h>
#include <cstdio>

pthread_rwlock_t db_lock = PTHREAD_RWLOCK_INITIALIZER;
pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;
static int db[16], writes = 0, reads = 0;

void* writer(void* arg) {
    long id = (long)arg;
    for (int i = 0; i < 500; i++) {
        pthread_rwlock_wrlock(&db_lock);
        db[i % 16] = (int)id;
        pthread_mutex_lock(&stats_lock);
        writes++;
        pthread_mutex_unlock(&stats_lock);
        pthread_rwlock_unlock(&db_lock);
    }
    return nullptr;
}

void* reader(void*) {
    long sum = 0;
    for (int i = 0; i < 500; i++) {
        pthread_rwlock_rdlock(&db_lock);
        sum += db[i % 16];
        if (i % 50 == 0) {           // nested read of the same rwlock: allowed
            pthread_rwlock_rdlock(&db_lock);
            sum += db[0];
            pthread_rwlock_unlock(&db_lock);
        }
        pthread_rwlock_unlock(&db_lock);
    }
    pthread_rwlock_rdlock(&db_lock);  // same order as the writers: db_lock -> stats_lock
    pthread_mutex_lock(&stats_lock);
    reads++;
    pthread_mutex_unlock(&stats_lock);
    pthread_rwlock_unlock(&db_lock);
    return (void*)sum;
}

int main() {
    pthread_t w[2], r[3];
    for (long i = 0; i < 2; i++) pthread_create(&w[i], nullptr, writer, (void*)i);
    for (long i = 0; i < 3; i++) pthread_create(&r[i], nullptr, reader, nullptr);
    for (int i = 0; i < 2; i++) pthread_join(w[i], nullptr);
    for (int i = 0; i < 3; i++) pthread_join(r[i], nullptr);
    printf("%d writes, %d readers\n", writes, reads);
    return 0;
}
