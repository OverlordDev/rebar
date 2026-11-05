#include <filesystem>
#include <iostream>
#include <cstring>
namespace fs = std::filesystem;
int main() {
  fs::path p("/usr/local/lib/libfoo.dylib");
  auto it = p.begin();
  std::cout << "sizeof iterator " << sizeof(it) << " path " << sizeof(p) << "\n";
  int n = 0; for (auto e : p) { std::cout << "[" << e.string() << "]"; n++; } std::cout << " " << n << "\n";
  auto e = p.end(); --e; std::cout << "last " << *e << " " << (it == p.begin()) << " " << std::distance(p.begin(), p.end()) << "\n";
  fs::path q("rel/file.txt"); for (auto e : q) std::cout << "<" << e.string() << ">"; std::cout << "\n";
  fs::path longp("/aaaaaaaaaaaaaaaaaaaa/bbbbbbbbbbbbbbbbbbbbbbbbbbb/c"); for (auto e : longp) std::cout << "(" << e.string() << ")"; std::cout << "\n";
  fs::path sp("a/b"); auto i2 = sp.begin(); ++i2; std::cout << "second " << *i2 << (i2 != sp.end()) << (++i2 == sp.end()) << "\n";
  std::cout << p.filename() << p.parent_path() << p.stem() << p.extension() << p.root_path() << p.relative_path() << "\n";
}
