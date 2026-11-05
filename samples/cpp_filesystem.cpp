#include <filesystem>
#include <iostream>
namespace fs = std::filesystem;
int main() {
  std::error_code ec;
  fs::file_status s = fs::symlink_status("/etc/hosts", ec), t = fs::status("/tmp", ec), n = fs::status("/nonexistent/x", ec);
  std::cout << (int)s.type() << " " << (int)t.type() << " " << (int)n.type() << " " << std::oct << (int)s.permissions() << std::dec << "\n";
  std::cout << fs::exists("/etc") << fs::is_directory("/tmp") << fs::is_symlink("/etc") << fs::is_regular_file("/etc/hosts") << "\n";
  fs::space_info si = fs::space("/", ec); std::cout << (si.capacity > 0) << (si.free <= si.capacity) << "\n";
  std::cout << fs::file_size("/etc/hosts", ec) << " " << (fs::last_write_time("/etc/hosts", ec).time_since_epoch().count() > 0) << " " << fs::canonical("/etc/../etc/hosts", ec) << " " << fs::absolute("x", ec).has_root_path() << "\n";
}
