// std::filesystem across the bridge: status (a file_status comes back through the hidden result pointer), exists, sizes, path pieces, canonical, space, create / remove.
// (directory_iterator / directory_entry are not covered: their cached entry holds a path in the host's string layout.)
#include <filesystem>
#include <fstream>
#include <iostream>
namespace fs = std::filesystem;
int main() {
    fs::path root = fs::temp_directory_path() / "rebar_t46";
    fs::remove_all(root);
    std::cout << "created: " << fs::create_directories(root / "a" / "b") << "\n";
    { std::ofstream(root / "a" / "one.txt") << "12345"; std::ofstream(root / "a" / "b" / "two.txt") << "abcdefghij"; }
    std::cout << "exists: " << fs::exists(root / "a") << fs::exists(root / "zzz") << " is_dir: " << fs::is_directory(root / "a") << " is_file: " << fs::is_regular_file(root / "a" / "one.txt") << "\n";
    fs::file_status st = fs::status(root / "a" / "one.txt");
    std::cout << "status type regular: " << (st.type() == fs::file_type::regular) << " size: " << fs::file_size(root / "a" / "one.txt") << "\n";
    fs::path p = "/usr/local/lib/libfoo.so.1.2";
    std::cout << "pieces: " << p.filename() << " " << p.stem() << " " << p.extension() << " " << p.parent_path() << " " << p.root_name().empty() << "\n";
    std::cout << "canonical ok: " << (fs::canonical(root / "a" / ".." / "a").filename() == "a") << "\n";
    std::error_code ec; auto missing = fs::file_size(root / "nope", ec);
    std::cout << "error_code: " << (ec ? "set" : "clear") << " " << (missing == static_cast<std::uintmax_t>(-1)) << "\n";
    auto sp = fs::space(root); std::cout << "space ok: " << (sp.capacity > 0 && sp.free <= sp.capacity) << "\n";
    std::cout << "removed: " << fs::remove_all(root) << "\n";
    return 0;
}
