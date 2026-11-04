// A library that calls the host's libc++ with wide strings (std::wstring concatenation and append through the library) loads and gives the right answer.
#include <dlfcn.h>
#include <stdio.h>
#include <wchar.h>
int main(int argc, char **argv) {
    void *h = dlopen(argv[1], RTLD_NOW);
    printf("dlopen %s\n", h ? "succeeded" : "refused");
    if (!h) return 0;
    size_t (*wide_len)(const wchar_t *, const wchar_t *) = (size_t (*)(const wchar_t *, const wchar_t *))dlsym(h, "wide_len");
    if (wide_len) printf("wide_len: %zu %zu\n", wide_len(L"abc", L"defghijk"), wide_len(L"", L""));
    return 0;
}
