// std::promise / std::future (libc++'s <future> lives in the dylib): promise<void> and promise<int>, set from another thread.
#include <future>
#include <thread>
#include <cstdio>
int main() {
    std::promise<void> pv;
    std::future<void> fv = pv.get_future();
    std::thread t([&] { pv.set_value(); });
    fv.get(); t.join();
    std::promise<int> pi; std::future<int> fi = pi.get_future();
    std::thread t2([&] { pi.set_value(42); });
    printf("int %d\n", fi.get()); t2.join();
    std::future<int> fa = std::async(std::launch::async, [] { return 7; });
    printf("async %d\n", fa.get());
    puts("ok");
    return 0;
}
