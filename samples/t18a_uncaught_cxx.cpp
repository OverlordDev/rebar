#include <cstdio>
#include <stdexcept>
struct Custom { int v; };
struct Loud { ~Loud() { std::printf("Loud dtor (should run? no: terminate before unwinding)\n"); } };
static void f(int k) { Loud l; if (k == 0) throw Custom{3}; else throw std::runtime_error("uncaught runtime"); }
int main(int argc, char **) {
    std::printf("about to throw %d\n", argc);
    std::fflush(stdout);
    f(argc == 1 ? 0 : 1);
    return 0;
}
