// C++ exceptions: types, hierarchy, rethrow, cleanups, nested, noexcept, catch(...).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <typeinfo>

struct Base { virtual ~Base() {} virtual const char *name() const { return "Base"; } int tag = 1; };
struct Derived : Base { const char *name() const override { return "Derived"; } int extra = 2; };
struct MyErr { int code; char msg[32]; };
static int g_dtors;
struct Guard { const char *n; Guard(const char *n) : n(n) { std::printf("  ctor %s\n", n); } ~Guard() { g_dtors++; std::printf("  dtor %s\n", n); } };

[[gnu::noinline]] static void thrower(int kind) {
    Guard g("thrower-guard");
    switch (kind) {
    case 0: throw 42;
    case 1: throw (long)-7;
    case 2: throw "c string";
    case 3: { MyErr e = {99, "my error"}; throw e; }
    case 4: throw Derived();
    case 5: throw std::runtime_error("runtime boom");
    case 6: throw std::out_of_range("range boom");
    case 7: throw 3.5;
    case 8: { static Base b; throw &b; }
    }
}
[[gnu::noinline]] static void middle(int kind) { Guard g("middle-guard"); thrower(kind); std::printf("unreachable\n"); }
[[gnu::noinline]] static void rethrower(int kind) {
    try { middle(kind); } catch (...) { std::printf("  rethrower caught, rethrowing\n"); throw; }
}
[[gnu::noinline]] static int nothrower(int x) noexcept { return x * 2; }

int main() {
    for (int k = 0; k <= 8; k++) {
        std::printf("kind %d:\n", k);
        try { middle(k); }
        catch (int v) { std::printf("  int %d\n", v); }
        catch (long v) { std::printf("  long %ld\n", v); }
        catch (const char *s) { std::printf("  cstr %s\n", s); }
        catch (const MyErr &e) { std::printf("  MyErr %d %s\n", e.code, e.msg); }
        catch (const Base &b) { std::printf("  Base& %s tag=%d\n", b.name(), b.tag); }
        catch (const std::out_of_range &e) { std::printf("  out_of_range %s\n", e.what()); }
        catch (const std::exception &e) { std::printf("  std::exception %s\n", e.what()); }
        catch (Base *p) { std::printf("  Base* %s\n", p->name()); }
        catch (...) { std::printf("  catch-all\n"); }
    }
    std::printf("rethrow:\n");
    try { rethrower(0); } catch (int v) { std::printf("  outer int %d\n", v); }
    std::printf("nested:\n");
    try {
        try { throw std::runtime_error("inner"); }
        catch (const std::exception &e) { std::printf("  inner caught %s\n", e.what()); throw 5; }
    } catch (int v) { std::printf("  outer got %d\n", v); }
    std::printf("current_exception:\n");
    try { throw Derived(); } catch (...) { std::exception_ptr p = std::current_exception(); std::printf("  has=%d\n", p != nullptr); }
    std::printf("dtors=%d nothrow=%d\n", g_dtors, nothrower(21));
    try { std::terminate_handler h = std::set_terminate([] { std::printf("terminate handler\n"); std::exit(7); }); (void)h; throw 1; } catch (float) {}
    std::printf("not reached\n");
    return 0;
}
