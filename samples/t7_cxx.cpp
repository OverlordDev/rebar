#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <vector>
struct Shape { virtual ~Shape() {} virtual double area() const = 0; };
struct Sq : Shape { double s; Sq(double s) : s(s) {} double area() const override { return s * s; } };
struct Ci : Shape { double r; Ci(double r) : r(r) {} double area() const override { return 3.14159 * r * r; } };
int main() {
    std::vector<int> v = {5, 3, 9, 1, 7};
    std::sort(v.begin(), v.end());
    std::string s = "sorted:";
    for (int x : v) s += " " + std::to_string(x);
    std::cout << s << std::endl;
    std::map<std::string, int> m{{"b", 2}, {"a", 1}, {"c", 3}};
    for (auto &kv : m) std::cout << kv.first << "=" << kv.second << " ";
    std::cout << std::endl;
    Shape *shapes[] = {new Sq(2), new Ci(1)};
    double tot = 0; for (auto *p : shapes) { tot += p->area(); delete p; }
    std::cout << "area " << tot << std::endl;
    return 0;
}
