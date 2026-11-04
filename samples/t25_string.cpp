// std::string and friends: the arm64 and x86_64 libc++ lay strings out differently, rebar converts at the boundary.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

static void show(const char *tag, const std::string &s) { std::printf("%-10s [%zu] %s\n", tag, s.size(), s.c_str()); }

int main() {
    std::string a = "short";
    show("short", a);
    a += " string";
    a.append(" and more text to make it long enough");
    show("grown", a);
    std::string b = a + "!" + std::to_string(42) + std::to_string(3.5);
    show("concat", b);
    b.insert(0, ">> ");
    b.insert(3, 2, '#');
    show("insert", b);
    show("substr", b.substr(5, 12));
    show("find", std::to_string(b.find("long")) + "/" + std::to_string(b.rfind("s")) + "/" + std::to_string(b.find_first_of("xyz")));
    b.replace(0, 5, "<<<<<<");
    b.erase(b.size() - 3);
    show("replace", b);
    std::string e;
    show("empty", e);
    e.assign(22, 'x');
    show("22", e);
    e.push_back('y');
    show("23", e);
    e.append(e);
    show("self", e);
    e.resize(5);
    e.shrink_to_fit();
    show("resized", e);
    std::string m = std::move(b);
    show("moved", m);
    show("moved-from", b);
    std::swap(a, e);
    show("swap-a", a);
    show("swap-e", e);
    std::printf("cmp %d %d %d eq %d\n", a.compare(e) < 0, a.compare("abc") > 0, std::string("x").compare("x"), a == e);

    std::vector<std::string> v = {"pear", "apple", "kiwi", "banana, a rather long fruit name", "fig"};
    std::sort(v.begin(), v.end());
    for (auto &s : v) std::cout << s << "|";
    std::cout << std::endl;
    std::map<std::string, int> counts;
    for (auto &s : v) counts[s + s] += (int)s.size();
    for (auto &kv : counts) std::cout << kv.first << "=" << kv.second << ";";
    std::cout << std::endl;

    std::ostringstream os;
    os << "n=" << 12 << " pi=" << 3.25 << " s=" << a;
    show("ostream", os.str());
    std::istringstream is("alpha beta\ngamma delta epsilon\nlast");
    std::string line, word;
    while (std::getline(is, line)) show("line", line);
    std::istringstream is2("one two three");
    while (is2 >> word) show("word", word);
    std::stringstream ss;
    ss << 7 << ' ' << "seven";
    int n; std::string name;
    ss >> n >> name;
    std::printf("ss %d %s\n", n, name.c_str());

    show("stoi", std::to_string(std::stoi("1234") + 1));
    show("stod", std::to_string(std::stod("2.5") * 2));
    show("stol", std::to_string(std::stol("-99999999999")));
    try { std::stoi("not a number"); } catch (const std::exception &x) { show("stoi-exc", "caught"); }
    try { throw std::runtime_error(std::string("runtime ") + "error with a long message exceeding the small string size"); }
    catch (const std::exception &x) { show("what", x.what()); }
    try { show("range", a.substr(1000)); } catch (const std::out_of_range &) { show("range", "out_of_range"); }
    try { (void)a.at(1000); } catch (const std::out_of_range &) { show("at", "out_of_range"); }

    std::filesystem::path p = "/usr/local/lib/libfoo.dylib";
    show("path", p.string());
    show("filename", p.filename().string());
    show("stem", p.stem().string());
    show("ext", p.extension().string());
    show("parent", p.parent_path().string());
    p /= "sub";
    show("append", p.string());
    std::error_code ec = std::make_error_code(std::errc::no_such_file_or_directory);
    show("errc", ec.message());

    try { throw std::system_error(ec, std::string("while testing ") + "system_error"); }
    catch (const std::system_error &x) { show("syserr", x.what()); std::printf("code %d\n", x.code().value()); }
    ss.str("reset 123");
    ss.clear();
    ss >> name >> n;
    std::printf("ss2 %s %d [%s]\n", name.c_str(), n, ss.str().c_str());
    std::string line2;
    std::istringstream csv("a,bb,ccc,,last");
    while (std::getline(csv, line2, ',')) show("csv", line2);

    std::string total;
    for (int i = 0; i < 200; i++) total += std::to_string(i) + ",";
    show("total", total.substr(0, 40));
    std::printf("hash %zu\n", total.size() + std::hash<std::string>()("x") * 0);
    return 0;
}
