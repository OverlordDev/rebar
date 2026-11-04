// std::stringstream objects are shared between the guest's inline code and the host's libc++: growth, reset and parsing, built at -O0 and -O2.
#include <sstream>
#include <string>
#include <cstdio>
int main() {

    std::ostringstream os;
    for (int i = 0; i < 60; i++) os << i << ",";
    std::string r = os.str();
    std::printf("A [%zu] %s\n", r.size(), r.substr(0, 50).c_str());
    os.str("");
    os << "after reset " << 42 << " and some more text to be long enough";
    std::printf("B %s\n", os.str().c_str());
    std::stringstream ss("12 34 56 foo bar");
    int a, b, c; std::string w1, w2;
    ss >> a >> b >> c >> w1 >> w2;
    std::printf("C %d %d %d %s %s\n", a, b, c, w1.c_str(), w2.c_str());
    ss.clear(); ss.str("x y z");
    std::string x, y, z; ss >> x >> y >> z;
    std::printf("D %s %s %s | %s\n", x.c_str(), y.c_str(), z.c_str(), ss.str().c_str());
    return 0;
}
