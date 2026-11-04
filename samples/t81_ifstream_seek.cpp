// ifstream seekg / tellg: the host's istream calls the virtual seekoff / seekpos of the guest's own filebuf, which return and take a 136-byte std::fpos by value
#include <fstream>
#include <iostream>
#include <cstdio>
#include <string>
int main() {
    { FILE *w = fopen("/tmp/rebar_t81.txt", "w"); fputs("0123456789abcdefghij", w); fclose(w); }
    std::ifstream f("/tmp/rebar_t81.txt");
    f.seekg(0, std::ios::end);
    std::streampos end = f.tellg();
    std::cout << "size " << (long long)end << "\n";
    f.seekg(5);
    std::streampos mid = f.tellg();
    char buf[6] = {0};
    f.read(buf, 5);
    std::cout << "read " << buf << " at " << (long long)mid << "\n";
    f.seekg(mid);
    f.read(buf, 3); buf[3] = 0;
    std::cout << "again " << buf << " now " << (long long)f.tellg() << "\n";
    f.seekg(0, std::ios::beg);
    std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::cout << "all " << all.size() << "\n";
    return 0;
}
