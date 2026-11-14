// ab_ba.cpp - the classic AB-BA bug from the synopsis (Fig. IV), as a bank transfer.
// Thread P locks account A then B; thread Q locks B then A.
//   ./ab_ba          P and Q run one after the other, so the run never hangs. LockSpy still
//                    reports a POTENTIAL DEADLOCK, because the lock-order graph has a cycle.
//   ./ab_ba --hang   the same, then a second round where a barrier makes both threads hold
//                    their first lock at the same moment: a real deadlock. The watchdog reports
//                    a LIVE DEADLOCK that "was predicted" by the first report.
#include <pthread.h>
#include <cstdio>
#include <cstring>

pthread_mutex_t A, B;              // the two account locks
static long balance_a = 100, balance_b = 100;
static pthread_barrier_t both_hold_first_lock;
static bool force_overlap = false;

void* worker_p(void*) {            // transfer A -> B
    pthread_setname_np(pthread_self(), "P");
    pthread_mutex_lock(&A);
    if (force_overlap) pthread_barrier_wait(&both_hold_first_lock);
    pthread_mutex_lock(&B);
    balance_a -= 10; balance_b += 10;
    pthread_mutex_unlock(&B);
    pthread_mutex_unlock(&A);
    return nullptr;
}

void* worker_q(void*) {            // transfer B -> A: takes the locks in the opposite order (bug)
    pthread_setname_np(pthread_self(), "Q");
    pthread_mutex_lock(&B);
    if (force_overlap) pthread_barrier_wait(&both_hold_first_lock);
    pthread_mutex_lock(&A);
    balance_b -= 5; balance_a += 5;
    pthread_mutex_unlock(&A);
    pthread_mutex_unlock(&B);
    return nullptr;
}

static void run(bool overlap) {
    force_overlap = overlap;
    pthread_t p, q;
    pthread_create(&p, nullptr, worker_p, nullptr);
    if (!overlap) pthread_join(p, nullptr);     // sequential: no overlap, no hang
    pthread_create(&q, nullptr, worker_q, nullptr);
    if (overlap) pthread_join(p, nullptr);
    pthread_join(q, nullptr);
}

int main(int argc, char** argv) {
    pthread_mutex_init(&A, nullptr);
    pthread_mutex_init(&B, nullptr);
    pthread_barrier_init(&both_hold_first_lock, nullptr, 2);
    run(false);
    if (argc > 1 && strcmp(argv[1], "--hang") == 0) run(true);  // deadlocks here
    printf("balances: A=%ld B=%ld\n", balance_a, balance_b);
    return 0;
}
