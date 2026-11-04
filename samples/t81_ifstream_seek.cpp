// ifstream seekg / tellg and ofstream tellp / seekp: seekoff / seekpos of a stream buffer return and take a 136-byte std::fpos by value (the host's istream calls the guest's
// filebuf, the guest's ostream calls the host's)
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
    std::ofstream o("/tmp/rebar_t81o.txt");
    o << "abcdef";
    std::cout << "tellp " << (long long)o.tellp() << "\n";
    o.seekp(2);
    std::streampos at = o.tellp();
    o << "XY";
    o.seekp(at);
    o << "Z";
    o.close();
    std::ifstream back("/tmp/rebar_t81o.txt");
    std::string text((std::istreambuf_iterator<char>(back)), std::istreambuf_iterator<char>());
    std::cout << "seekp " << (long long)at << " " << text << "\n";
    return 0;
}
