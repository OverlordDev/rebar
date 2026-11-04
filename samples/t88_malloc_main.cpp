// std::string growth through the host's libc++ when the program has its own malloc: the buffer the host frees must be one the host allocated, and the one it hands back must be freeable by
// the program's free
#include <cstdio>
#include <string>
extern "C" int g_own_allocs, g_own_frees;
int main() {
    std::string s = "a rather long string, longer than the 22 characters of the short form";
    for (int i = 0; i < 20; i++) s.append("0123456789abcdef", 16);
    s.insert(s.begin() + 3, 'X');
    std::string t = std::to_string(1234567890123456789ll) + std::string(40, 'z');
    t.append(s, 0, 30);
    s.erase(s.begin() + 5);
    std::printf("%zu %zu %.20s %.20s\n", s.size(), t.size(), s.c_str(), t.c_str());
    s.clear(); s.shrink_to_fit();
    std::printf("own allocations %s\n", g_own_allocs > 0 ? "used" : "unused");
    return 0;
}
