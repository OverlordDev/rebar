// std::filesystem::directory_iterator and recursive_directory_iterator: entry.path(), is_directory(), filename, a sorted listing of a small tree
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
namespace fs = std::filesystem;
int main() {
    fs::path root = fs::temp_directory_path() / "rebar_t72";
    fs::remove_all(root);
    fs::create_directories(root / "alpha" / "inner");
    fs::create_directories(root / "beta");
    for (const char *f : {"a.txt", "alpha/b_a_long_file_name_to_leave_the_small_string_buffer.txt", "alpha/inner/c.dat", "beta/d"}) { FILE *fp = fopen((root / f).c_str(), "w"); fputs("x", fp); fclose(fp); }
    std::vector<std::string> names;
    for (const auto &e : fs::directory_iterator(root)) names.push_back(e.path().filename().string() + (e.is_directory() ? "/" : ""));
    std::sort(names.begin(), names.end());
    for (auto &n : names) std::printf("top %s\n", n.c_str());
    names.clear();
    int maxdepth = 0;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        names.push_back(fs::relative(it->path(), root).string() + (it->is_regular_file() ? " file" : " dir"));
        maxdepth = std::max(maxdepth, it.depth());
    }
    std::sort(names.begin(), names.end());
    for (auto &n : names) std::printf("rec %s\n", n.c_str());
    std::printf("depth %d\n", maxdepth);
    fs::remove_all(root);
    return 0;
}
