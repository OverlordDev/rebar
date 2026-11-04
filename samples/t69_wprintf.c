// wide printf: swprintf (variadic) and vswprintf (va_list), integers, doubles, strings and wide strings
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>
static int vw(wchar_t *buf, size_t n, const wchar_t *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vswprintf(buf, n, fmt, ap); va_end(ap); return r; }
int main() {
    wchar_t buf[128];
    int r = swprintf(buf, 128, L"%d-%ls-%.2f-%s-%lld", 42, L"wide", 3.14159, "narrow", 1234567890123LL);
    printf("%d %ls\n", r, buf);
    r = vw(buf, 128, L"[%5d][%-6ls][%x]", 7, L"ab", 255);
    printf("%d %ls\n", r, buf);
    return 0;
}
