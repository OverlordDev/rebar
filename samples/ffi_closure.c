#include <ffi/ffi.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
static void cmpfn(ffi_cif *cif, void *ret, void **args, void *ud) { int *a = *(int **)args[0], *b = *(int **)args[1]; *(ffi_arg *)ret = *a - *b; }
int main(void) {
  ffi_cif cif; ffi_type *at[2] = {&ffi_type_pointer, &ffi_type_pointer};
  printf("prep %d\n", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 2, &ffi_type_sint, at));
  void *code; ffi_closure *cl = ffi_closure_alloc(sizeof(ffi_closure), &code);
  printf("alloc %d\n", cl != NULL);
  printf("closure %d\n", ffi_prep_closure_loc(cl, &cif, cmpfn, NULL, code));
  int arr[5] = {5, 1, 4, 2, 3}; qsort(arr, 5, 4, (int (*)(const void *, const void *))code);
  printf("sorted %d %d %d %d %d\n", arr[0], arr[1], arr[2], arr[3], arr[4]);
  ffi_closure_free(cl); printf("freed\n");
  ffi_cif c2; ffi_type *a2[1] = {&ffi_type_double}; double x = 2.0, r; void *av[1] = {&x}; ffi_prep_cif(&c2, FFI_DEFAULT_ABI, 1, &ffi_type_double, a2); ffi_call(&c2, FFI_FN(sqrt), &r, av); printf("sqrt %.3f\n", r);
  return 0;
}
