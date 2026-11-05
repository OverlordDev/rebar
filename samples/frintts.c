// FEAT_FRINTTS: FRINT32Z / FRINT32X / FRINT64Z / FRINT64X, scalar and vector. Results are specified by the architecture (no native arm64 here to compare with): a value that does not fit the
// integer width, a NaN or an infinity gives the most negative integer.
#include <stdio.h>
#include <math.h>
#define S(name, T, reg, v) ({ T r_ = v; __asm__ volatile(name " %" reg "0, %" reg "0" : "+w"(r_)); r_; })
int main(void) {
  double d[] = {2.7, -2.7, 2.5, 3.5, 3e9, -3e9, 1e19, -1e19, NAN, INFINITY, -0.0};
  for (int k = 0; k < 11; k++) {
    double a = S("frint32z", double, "d", d[k]), b = S("frint32x", double, "d", d[k]), c = S("frint64z", double, "d", d[k]), e = S("frint64x", double, "d", d[k]);
    printf("%g: %.1f %.1f %.1f %.1f\n", d[k], a, b, c, e);
  }
  float f[] = {1.5f, -1.5f, 3e9f, NAN};
  for (int k = 0; k < 4; k++) printf("s %g: %.1f %.1f\n", f[k], S("frint32z", float, "s", f[k]), S("frint64x", float, "s", f[k]));
  typedef float f4 __attribute__((vector_size(16)));
  f4 v = {2.5f, -2.5f, 3e9f, 7.9f}, r1 = v, r2 = v, r3 = v, r4 = v;
  __asm__ volatile("frint32z %0.4s, %0.4s" : "+w"(r1)); __asm__ volatile("frint32x %0.4s, %0.4s" : "+w"(r2));
  __asm__ volatile("frint64z %0.4s, %0.4s" : "+w"(r3)); __asm__ volatile("frint64x %0.4s, %0.4s" : "+w"(r4));
  printf("v32z %.1f %.1f %.1f %.1f\n", r1[0], r1[1], r1[2], r1[3]); printf("v32x %.1f %.1f %.1f %.1f\n", r2[0], r2[1], r2[2], r2[3]);
  printf("v64z %.1f %.1f %.1f %.1f\n", r3[0], r3[1], r3[2], r3[3]); printf("v64x %.1f %.1f %.1f %.1f\n", r4[0], r4[1], r4[2], r4[3]);
  typedef double d2 __attribute__((vector_size(16)));
  d2 w = {2.5, 1e19}, q = w; __asm__ volatile("frint64z %0.2d, %0.2d" : "+w"(q)); printf("v64z.2d %.1f %.1f\n", q[0], q[1]);
  return 0;
}
