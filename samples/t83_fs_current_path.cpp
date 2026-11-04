// std::filesystem functions that return a path by value or take an error_code: current_path() (llama-bench started with it), absolute, exists, error_code::message
#include <filesystem>
#include <iostream>
#include <system_error>
namespace fs = std::filesystem;
int main() {
    std::error_code ec;
    fs::path p = fs::current_path(ec);
    std::cout << "current_path(ec) " << !ec << " " << (p.string().size() > 1) << "\n";
    fs::path q = fs::current_path();
    std::cout << "current_path() " << (q == p) << "\n";
    std::cout << "absolute " << (fs::absolute("x/y", ec).string().size() > 4) << "\n";
    std::cout << "exists " << fs::exists("/tmp", ec) << " " << fs::is_directory("/tmp", ec) << " " << fs::exists("/no/such/path/here", ec) << "\n";
    std::cout << "message " << std::make_error_code(std::errc::no_such_file_or_directory).message() << "\n";
    fs::path f("/tmp/a/b/c.txt");
    std::cout << f.filename() << " " << f.parent_path() << " " << f.extension() << " " << f.stem() << "\n";
    return 0;
}
