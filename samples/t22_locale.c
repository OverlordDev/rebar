#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    printf("LANG=%s LC_ALL=%s\n", getenv("LANG") ? getenv("LANG") : "(unset)", getenv("LC_ALL") ? getenv("LC_ALL") : "(unset)");
    char *r = setlocale(LC_ALL, "");
    printf("setlocale -> %s\n", r ? r : "(null)");
    return 0;
}
