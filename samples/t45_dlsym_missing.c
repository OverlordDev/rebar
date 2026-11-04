// dlsym of a symbol that does not exist must say so (GTK 3 looks for GTK 2 symbols to refuse a mixed process), through RTLD_DEFAULT, a dlopen(NULL) handle and a library handle.
#include <dlfcn.h>
#include <stdio.h>
int main(void) {
    void *self = dlopen(NULL, RTLD_LAZY);
    void *lib = dlopen("/usr/lib/libSystem.B.dylib", RTLD_LAZY);
    printf("self handle: %s\n", self ? "yes" : "no");
    printf("RTLD_DEFAULT missing: %s\n", dlsym(RTLD_DEFAULT, "gtk_progress_get_type") ? "FOUND" : "null");
    printf("self missing: %s\n", self && dlsym(self, "gtk_progress_get_type") ? "FOUND" : "null");
    printf("RTLD_DEFAULT present: %s\n", dlsym(RTLD_DEFAULT, "malloc") ? "found" : "NULL");
    printf("self present: %s\n", self && dlsym(self, "printf") ? "found" : "NULL");
    printf("lib present/missing: %s %s\n", lib && dlsym(lib, "strlen") ? "found" : "NULL", lib && dlsym(lib, "no_such_symbol_xyz") ? "FOUND" : "null");
    printf("RTLD_NEXT missing: %s\n", dlsym(RTLD_NEXT, "no_such_symbol_xyz") ? "FOUND" : "null");
    dlerror(); dlsym(RTLD_DEFAULT, "no_such_symbol_xyz"); printf("dlerror after a missing symbol: %s\n", dlerror() ? "set" : "NULL");
    dlerror(); dlsym(RTLD_DEFAULT, "malloc"); printf("dlerror after a found symbol: %s\n", dlerror() ? "set" : "NULL");
    void *s2 = dlopen(NULL, RTLD_LAZY); dlerror(); dlsym(s2, "no_such_symbol_xyz"); printf("dlerror after a missing symbol, dlopen(NULL) handle: %s\n", dlerror() ? "set" : "NULL");
    return 0;
}
