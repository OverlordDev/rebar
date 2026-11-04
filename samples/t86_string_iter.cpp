// std::string::insert / erase with an iterator argument: the iterator points into the arm64-layout characters, the host's libc++ works on a copy in its own layout (OpenAL's
// device-name formatting crashed Minecraft)
#include <iostream>
#include <string>
int main() {
    std::string s = std::to_string(7u);
    s.insert(s.begin(), '!');
    auto it = s.insert(s.begin() + 1, '#');
    std::cout << s << " " << (it - s.begin()) << " " << *it << "\n";
    std::string l(60, 'a');
    auto at = l.insert(l.begin() + 30, 'Z');
    std::cout << l.size() << " " << (at - l.begin()) << " " << *at << "\n";
    l.erase(l.begin() + 10);
    auto e = l.erase(l.begin() + 5, l.begin() + 9);
    std::cout << l.size() << " " << (e - l.begin()) << " " << l.find('Z') << "\n";
    std::string t = "hello world";
    t.insert(t.cbegin() + 5, 3, ',');
    t.insert(t.end(), '.');
    std::cout << t << "\n";
    return 0;
}
