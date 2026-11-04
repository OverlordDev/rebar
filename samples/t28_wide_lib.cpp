#include <string>
extern "C" size_t wide_len(const wchar_t *a, const wchar_t *b) {
    std::wstring s = std::wstring(a) + std::wstring(b);
    s.append(L"!!");
    return s.size();
}
