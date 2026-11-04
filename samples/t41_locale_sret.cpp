// std::locale returned by value from the library (locale::global, ios_base::getloc / imbue): the result address travels in x8 on arm64.
#include <locale>
#include <iostream>
#include <sstream>
int main() {
    std::locale prev = std::locale::global(std::locale::classic());
    std::cout << "global previous: " << prev.name() << "\n";
    std::ostringstream os;
    std::locale old = os.imbue(std::locale::classic());
    std::locale cur = os.getloc();
    std::cout << "imbue/getloc: " << old.name() << " " << cur.name() << " " << (cur == std::locale::classic()) << "\n";
    os << 1234567;
    std::cout << os.str() << "\n";
    return 0;
}
