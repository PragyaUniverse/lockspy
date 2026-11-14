// lock_held_at_exit.cpp - a worker returns on an error path without unlocking config_lock.
// Any later locker would block forever. Expected: HELD_AT_EXIT.
#include <pthread.h>
#include <cstdio>

pthread_mutex_t config_lock = PTHREAD_MUTEX_INITIALIZER;
static int config_version = 0;

static bool load_config(int v) { return v != 3; }   // version 3 "fails to load"

void* reload_config(void* arg) {
    long v = (long)arg;
    pthread_mutex_lock(&config_lock);
    if (!load_config((int)v)) return nullptr;        // bug: returns with config_lock held
    config_version = (int)v;
    pthread_mutex_unlock(&config_lock);
    return nullptr;
}

int main() {
    pthread_t t;
    for (long v = 1; v <= 3; v++) {
        pthread_create(&t, nullptr, reload_config, (void*)v);
        pthread_join(t, nullptr);
    }
    printf("config version %d\n", config_version);
    return 0;
}
