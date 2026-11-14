// consistent_order.cpp - corrected AB-BA, written with C++ std::mutex and std::lock_guard
// (std::mutex is a pthread mutex underneath, so LockSpy sees it). Every thread takes the
// locks in the same global order A < B < C. Expected: no warnings.
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

std::mutex A, B, C;
static long total = 0;

static void transfer(int id) {
    for (int i = 0; i < 1000; i++) {
        std::lock_guard<std::mutex> la(A);
        std::lock_guard<std::mutex> lb(B);
        if (id % 2) { std::lock_guard<std::mutex> lc(C); total += 2; }
        else total += 1;
    }
}

int main() {
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; i++) ts.emplace_back(transfer, i);
    for (auto& t : ts) t.join();
    printf("total = %ld\n", total);
    return 0;
}
