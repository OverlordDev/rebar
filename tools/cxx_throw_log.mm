// DYLD_INSERT_LIBRARIES=cxx_throw_log.dylib ./rebar app: logs the type of every C++ / Objective-C exception that is thrown (host code and the guest's own throws go through the same
// function), the NSException's name and reason, and the host frames
// build: clang++ -arch x86_64 -dynamiclib -framework Foundation -o cxx_throw_log.dylib tools/cxx_throw_log.mm
#import <Foundation/Foundation.h>
#include <cxxabi.h>
#include <execinfo.h>
#include <typeinfo>

static void logging_throw(void *obj, std::type_info *ti, void (*dtor)(void *)) {
    int st = 0;
    char *dem = abi::__cxa_demangle(ti->name(), nullptr, nullptr, &st);
    fprintf(stderr, "[throw] %s\n", dem ? dem : ti->name());
    if (!strcmp(ti->name(), "NSException") || !strcmp(ti->name(), "10NSException")) {
        @autoreleasepool {
            id exc = *(id *)obj;
            fprintf(stderr, "  %s: %s\n", [[exc name] UTF8String], [[exc reason] UTF8String]);
        }
    }
    void *bt[16]; int n = backtrace(bt, 16);
    backtrace_symbols_fd(bt + 1, n - 1, 2);
    __cxxabiv1::__cxa_throw(obj, ti, dtor);
}
__attribute__((used)) static struct { void *replacement, *original; } interposers[] __attribute__((section("__DATA,__interpose"))) = {
    { (void *)logging_throw, (void *)__cxxabiv1::__cxa_throw },
};
