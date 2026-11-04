// std::any_cast / typeid comparison of a type the program defines: libc++ on arm64 compares the type name against a constant that carries the
// "non-unique" tag in bit 63 (clearing the tag in the typeinfo made every any_cast throw bad_any_cast: Blender could not start)
#include <any>
#include <cstdio>
#include <map>
#include <string>
#include <typeinfo>
struct Big { long a[8]; std::string s; };
struct Small { int v; };
int main() {
    std::any a = Small{41};
    std::any b = Big{{1, 2, 3, 4, 5, 6, 7, 8}, "payload"};
    std::printf("small %d\n", std::any_cast<Small>(a).v + 1);
    std::printf("big %ld %s\n", std::any_cast<Big &>(b).a[7], std::any_cast<Big &>(b).s.c_str());
    std::printf("typeid eq %d %d\n", (int)(a.type() == typeid(Small)), (int)(a.type() == typeid(Big)));
    try { (void)std::any_cast<Big>(a); } catch (const std::bad_any_cast &e) { std::printf("caught bad_any_cast\n"); }
    std::map<std::string, std::any> reg;
    reg["k"] = Big{{9}, "in map"};
    std::printf("map %s\n", std::any_cast<Big &>(reg["k"]).s.c_str());
    return 0;
}
