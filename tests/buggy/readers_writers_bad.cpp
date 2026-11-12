// readers_writers_bad.cpp - Readers-Writers with a pthread rwlock (db_lock) and a statistics
// mutex. Writers update the database and then the stats: db_lock(write) -> stats_lock.
// The audit reader takes stats_lock first, then reads the database: stats_lock -> db_lock(read).
// A reader waiting to read is blocked by a writer, so this cycle is real. In this run the
// audit happens after the writers finish, so it never hangs: expected POTENTIAL DEADLOCK.
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
        pthread_rwlock_unlock(&db_lock);
    }
    pthread_mutex_lock(&stats_lock);
    reads++;
    pthread_mutex_unlock(&stats_lock);
    return (void*)sum;
}

void* auditor(void*) {               // opposite order: stats_lock -> db_lock (bug)
    pthread_mutex_lock(&stats_lock);
    pthread_rwlock_rdlock(&db_lock);
    printf("audit: %d writes, %d readers done, db[0]=%d\n", writes, reads, db[0]);
    pthread_rwlock_unlock(&db_lock);
    pthread_mutex_unlock(&stats_lock);
    return nullptr;
}

int main() {
    pthread_t w[2], r[3], a;
    for (long i = 0; i < 2; i++) pthread_create(&w[i], nullptr, writer, (void*)i);
    for (long i = 0; i < 3; i++) pthread_create(&r[i], nullptr, reader, nullptr);
    for (int i = 0; i < 2; i++) pthread_join(w[i], nullptr);
    for (int i = 0; i < 3; i++) pthread_join(r[i], nullptr);
    pthread_create(&a, nullptr, auditor, nullptr);
    pthread_join(a, nullptr);
    return 0;
}
