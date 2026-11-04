// Thread-local variables: per-thread values in the main thread and four others (initialised, zero-initialised and static ones, a counter hammered in a loop), and the speed of the access.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
thread_local int tl_counter = 5;
thread_local const char *tl_name = "unnamed";
thread_local long tl_zero_init = 0;
thread_local long tl_big[64];
static thread_local int tl_static_zero;
__attribute__((noinline)) int *counter_addr() { return &tl_counter; }   // one TLV access per call
static int *(*volatile counter_fn)() = counter_addr;   // (called through a pointer so that the compiler cannot hoist the access out of the loop)
__attribute__((noinline)) void bump_slow(int n) { for (int i = 0; i < n; i++) (*counter_fn())++; }
__attribute__((noinline)) void bump(int n) { for (int i = 0; i < n; i++) { tl_counter++; tl_big[i & 63] += i; tl_static_zero ^= i; } }
int main() {
    std::vector<long> results(5);
    auto work = [&](int idx) {
        static const char *names[] = {"w0", "worker-1", "worker-2", "worker-3", "worker-4"}; tl_name = names[idx]; tl_zero_init = idx * 7;
        bump(200000 + idx); bump_slow(1000000);
        long sum = 0; for (long v : tl_big) sum += v;
        results[idx] = tl_counter * 1000003L + sum + (long)std::string(tl_name).size() + tl_static_zero + tl_zero_init;
    };
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> th;
    for (int k = 1; k < 5; k++) th.emplace_back(work, k);
    work(0);
    for (auto &t : th) t.join();
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    for (int k = 0; k < 5; k++) std::printf("thread %d result %ld\n", k, results[k]);
    std::printf("main thread name still %s, counter %d\n", tl_name, tl_counter);
    std::fprintf(stderr, "time %.3f s\n", secs);
    return 0;
}
