#include <stdexcept>
struct MyErr { int code; explicit MyErr(int c) : code(c) {} virtual ~MyErr() {} };
struct DerivedErr : MyErr, std::runtime_error { explicit DerivedErr(int c) : MyErr(c), std::runtime_error("derived") {} };
void throw_my_err(int code);
void throw_derived(int code);
