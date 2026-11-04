#include <cstdio>
#include <cstdint>
#include <typeinfo>
#include <stdexcept>
struct Base { virtual ~Base() {} };
struct Derived : Base {};
static void show(const char *n, const std::type_info &t) {
    const uint64_t *w = (const uint64_t *)&t;
    std::printf("%s: vptr=%llx name_word=%llx\n", n, (unsigned long long)w[0], (unsigned long long)w[1]);
}
int main() { show("Base", typeid(Base)); show("Derived", typeid(Derived)); show("int", typeid(int)); show("runtime_error", typeid(std::runtime_error)); return 0; }
