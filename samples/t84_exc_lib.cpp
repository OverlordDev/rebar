// The library half of t84: throws exception types that the program catches; every image has its own copy of those types' typeinfo
#include "t84_exc.h"
void throw_my_err(int code) { throw MyErr(code); }
void throw_derived(int code) { throw DerivedErr(code); }
