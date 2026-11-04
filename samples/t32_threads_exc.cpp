// Exceptions thrown by the host's libc++ (stoi) and caught in a guest thread must not leave that thread holding the
// bridge lock: another thread that needs a handler (printf) must keep making progress meanwhile.
#include <atomic>
#include <cstdio>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <unistd.h>
static std::atomic<int> g_done{0}, g_caught{0};
static void *thrower(void *) {
    for (int i = 0; i < 3000; i++) {
        try { (void)std::stoi("not a number"); } catch (const std::invalid_argument &) { g_caught++; }
    }
    g_done = 1;
    return nullptr;
}
int main() {
    FILE *null = fopen("/dev/null", "w");
    pthread_t t;
    pthread_create(&t, nullptr, thrower, nullptr);
    int during = 0;
    while (!g_done) { fprintf(null, "tick %d\n", during); during++; }
    pthread_join(t, nullptr);
    std::printf("caught %d\n", g_caught.load());
    std::printf("main thread kept running while the other threw: %s\n", during >= 20 ? "yes" : "no");
    return 0;
}
