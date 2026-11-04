// std::wstring across the bridge (wchar_t is 4 bytes: the short form holds 4 characters, in a different place than std::string's): growth past the short size, append, replace, find, at, compare.
#include <string>
#include <cstdio>
#include <stdexcept>
static void show(const char *what, const std::wstring &s) {
    std::printf("%s: size=%zu [", what, s.size());
    for (wchar_t c : s) std::printf("%lc", (wint_t)c);
    std::printf("]\n");
}
int main() {
    std::wstring a = L"abc";                      // short
    show("short", a);
    a.append(L"defghij");                         // grows into the long form through the library
    show("append", a);
    std::wstring b = L"héllo wörld, ünïcode";
    show("unicode", b);
    b.replace(0, 5, L"HELLO");
    show("replace", b);
    std::printf("find: %zu %zu\n", b.find(L"wör"), b.find(L'z') == std::wstring::npos ? 999u : 0u);
    try { std::printf("at: %lc\n", (wint_t)b.at(1)); b.at(500); } catch (const std::out_of_range &) { std::printf("out_of_range caught\n"); }
    std::wstring c;
    for (int i = 0; i < 12; i++) c.push_back(L'a' + i);
    show("push_back", c);
    std::printf("compare: %d %d %d\n", c.compare(L"abcdefghijkl") == 0, c < a, a.substr(2, 4) == L"cdef");
    std::wstring d = std::to_wstring(1234567890);
    show("to_wstring", d);
    d.assign(L"xy"); d.insert(1, L"-+-"); show("assign/insert", d);
    return 0;
}
