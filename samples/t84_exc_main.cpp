// An exception thrown in a dynamic library is caught in the program (and by a base class): the typeinfo of the type exists in both images, dyld coalesces them
#include <cstdio>
#include "t84_exc.h"
int main() {
    try { throw_my_err(7); } catch (const MyErr &e) { std::printf("caught MyErr %d\n", e.code); }
    try { throw_derived(8); } catch (const std::runtime_error &e) { std::printf("caught runtime_error %s\n", e.what()); }
    try { throw_derived(9); } catch (const MyErr &e) { std::printf("caught DerivedErr as MyErr %d\n", e.code); }
    try { throw_my_err(1); } catch (...) { std::printf("caught ...\n"); }
    return 0;
}
