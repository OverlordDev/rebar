// A facet's string-returning virtual functions (numpunct::grouping and friends) called from the program: the result string comes back in memory (x8) in the program's string layout.
#include <cstdio>
#include <locale>
#include <string>
int main() {
    std::locale loc;
    const std::numpunct<char> &np = std::use_facet<std::numpunct<char>>(loc);
    std::string g = np.grouping(), t = np.truename(), f = np.falsename();
    std::printf("grouping %zu '%s' true '%s' false '%s' sep '%c' dec '%c'\n", g.size(), g.c_str(), t.c_str(), f.c_str(), np.thousands_sep(), np.decimal_point());
    std::locale us("en_US.UTF-8");
    const std::numpunct<char> &up = std::use_facet<std::numpunct<char>>(us);
    std::string ug = up.grouping();
    std::printf("us grouping size %zu first %d sep '%c'\n", ug.size(), ug.empty() ? -1 : (int)ug[0], up.thousands_sep());
    std::printf("moneypunct %s\n", std::use_facet<std::moneypunct<char>>(us).curr_symbol().c_str());
    return 0;
}
