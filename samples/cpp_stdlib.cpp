#include <iostream>
#include <unistd.h>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <locale>
#include <regex>
#include <filesystem>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <unordered_map>
#include <map>
#include <set>
#include <deque>
#include <list>
#include <memory>
#include <variant>
#include <optional>
#include <any>
#include <string_view>
#include <numeric>
#include <algorithm>
#include <ranges>
#include <format>
#include <bitset>
#include <random>
#include <complex>
#include <valarray>
#include <stdexcept>
#include <system_error>
#include <codecvt>
#include <charconv>
#include <span>
#include <atomic>
#include <semaphore>
#include <latch>
#include <barrier>
#include <stop_token>
namespace fs = std::filesystem;
struct Shape { virtual ~Shape() = default; virtual double area() const = 0; };
struct Sq : Shape { double s; Sq(double s) : s(s) {} double area() const override { return s * s; } };
int main() {
  std::ostringstream os; os << std::setw(8) << std::setfill('*') << std::hex << 255 << ' ' << std::fixed << std::setprecision(3) << 3.14159 << ' ' << std::boolalpha << true;
  std::cout << "sstream " << os.str() << "\n";
  std::istringstream is("12 3.5 word"); int i; double d; std::string w; is >> i >> d >> w; std::cout << "parse " << i << ' ' << d << ' ' << w << "\n";
  std::stringstream ss; ss << "a,b,,c"; std::string tok; std::vector<std::string> v; while (std::getline(ss, tok, ',')) v.push_back(tok); std::cout << "split " << v.size() << "\n";
  { auto p = fs::temp_directory_path() / ("rebar_cpp_" + std::to_string(getpid())); fs::create_directories(p / "a/b"); std::ofstream(p / "a/b/f.txt") << "hello\nworld\n";
    std::ifstream in(p / "a/b/f.txt"); std::string l1, l2; std::getline(in, l1); std::getline(in, l2); std::cout << "fstream " << l1 << l2 << ' ' << fs::file_size(p / "a/b/f.txt") << "\n";
    int n = 0; for (auto& e : fs::recursive_directory_iterator(p)) { (void)e; n++; } std::cout << "fs entries " << n << ' ' << (p / "a/b/f.txt").extension() << ' ' << (p / "a" / ".." / "a").lexically_normal().filename() << "\n"; fs::remove_all(p); }
  std::regex re("(\\w+)@(\\w+)\\.com"); std::smatch m; std::string em = "mail bob@example.com now"; std::cout << "regex " << std::regex_search(em, m, re) << ' ' << m[1] << ' ' << m[2] << ' ' << std::regex_replace(em, re, "$2:$1") << "\n";
  std::mutex mu; std::condition_variable cv; bool ready = false; int shared = 0; std::vector<std::thread> th; for (int t = 0; t < 4; t++) th.emplace_back([&] { std::unique_lock lk(mu); cv.wait(lk, [&] { return ready; }); shared += 10; });
  { std::lock_guard lk(mu); ready = true; } cv.notify_all(); for (auto& t : th) t.join(); std::cout << "threads " << shared << "\n";
  auto fut = std::async(std::launch::async, [] { return 6 * 7; }); std::promise<std::string> pr; auto f2 = pr.get_future(); std::thread([&] { pr.set_value("promised"); }).detach(); std::cout << "future " << fut.get() << ' ' << f2.get() << "\n";
  std::unordered_map<std::string, int> um; for (int k = 0; k < 1000; k++) um["k" + std::to_string(k % 37)] += k; std::map<int, std::string> mp{{3, "c"}, {1, "a"}, {2, "b"}}; std::cout << "maps " << um.size() << ' ' << um["k5"] << ' ' << mp.begin()->second << "\n";
  std::variant<int, std::string, double> var = std::string("hi"); std::optional<int> op; std::any an = 42; std::cout << "variant " << var.index() << ' ' << op.has_value() << ' ' << std::any_cast<int>(an) << ' ' << std::visit([](auto&& x) { std::ostringstream o; o << x; return o.str(); }, var) << "\n";
  auto sp = std::make_shared<Sq>(3); std::shared_ptr<Shape> sh = sp; std::weak_ptr<Shape> wk = sh; std::cout << "smart " << sh->area() << ' ' << sh.use_count() << ' ' << wk.expired() << "\n";
  std::vector<int> nums(20); std::iota(nums.begin(), nums.end(), 1); auto ev = nums | std::views::filter([](int x) { return x % 2 == 0; }) | std::views::transform([](int x) { return x * x; }); int sum = 0; for (int x : ev) sum += x; std::cout << "ranges " << sum << ' ' << std::ranges::max(nums) << "\n";
  std::cout << "format " << std::format("{:>6}|{:<5.1f}|{:#x}|{}", "ab", 2.55, 255, std::vector<int>{1, 2}.size()) << "\n";
  std::mt19937 g(42); std::uniform_int_distribution<int> ud(0, 99); std::normal_distribution<double> nd(0, 1); std::cout << "random " << ud(g) << ' ' << ud(g) << ' ' << std::fixed << std::setprecision(4) << nd(g) << "\n";
  std::complex<double> c(1, 2); std::valarray<double> va{1, 2, 3}; std::cout << "complex " << std::abs(c * c) << ' ' << va.sum() << ' ' << std::bitset<8>(0xA5) << "\n";
  try { throw std::system_error(std::make_error_code(std::errc::no_such_file_or_directory), "x"); } catch (const std::exception& e) { std::cout << "except " << e.what() << "\n"; }
  try { std::vector<int> e; (void)e.at(3); } catch (const std::out_of_range& e) { std::cout << "oor " << (std::string(e.what()).size() > 0) << "\n"; }
  char buf[32]; auto r = std::to_chars(buf, buf + 32, 3.25); double back; back = 3.25; std::cout << "chars " << std::string(buf, r.ptr) << ' ' << back << "\n";
  auto t0 = std::chrono::steady_clock::now(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(); std::cout << "chrono " << (dt >= 20) << ' ' << std::chrono::duration_cast<std::chrono::seconds>(std::chrono::hours(2)).count() << "\n";
  std::latch lt(3); std::vector<std::jthread> jt; for (int k = 0; k < 3; k++) jt.emplace_back([&] { lt.count_down(); }); lt.wait(); std::counting_semaphore<4> sem(2); sem.acquire(); std::cout << "sync " << sem.try_acquire() << sem.try_acquire() << "\n";
  std::locale::global(std::locale("en_US.UTF-8")); std::cout.imbue(std::locale()); std::cout << "locale " << 1234567 << ' ' << std::use_facet<std::numpunct<char>>(std::locale()).thousands_sep() << "\n";
  std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> conv; std::cout << "utf8 " << conv.from_bytes("héllo").size() << "\n";
  std::deque<int> dq{1, 2, 3}; dq.push_front(0); std::list<int> ls{3, 1, 2}; ls.sort(); std::set<std::string> st{"b", "a"}; std::cout << "containers " << dq.size() << ls.front() << *st.begin() << "\n";
}
