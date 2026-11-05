#!/usr/bin/env python3
"""Writes a C program that runs ~750 forms of the AdvSIMD instructions the JIT has templates for (widening, pairwise, halving, shifts, multiplies by element, conversions, ...) on random data
and prints a checksum per form.  The test in run_tests.sh runs it with the JIT and without and compares the two outputs: any difference is a template that disagrees with the interpreter.
Usage: python3 tools/neonforms.py > neon_forms.c ; clang -arch arm64 -O0 -march=armv8.2-a+fp16 neon_forms.c -o neon_forms_arm"""
forms = []
add = forms.append
for a, b in [("8b", "4h"), ("16b", "8h"), ("4h", "2s"), ("8h", "4s"), ("2s", "1d"), ("4s", "2d")]:
    for m in ["uaddlp", "saddlp", "uadalp", "sadalp"]: add(f"{m} v0.{b}, v1.{a}")
for t in ["2s", "4s", "2d"]:
    for m in ["fmul", "fmla", "fmls", "fmulx"]: add(f"{m} v0.{t}, v1.{t}, v2.{'d' if t == '2d' else 's'}[1]")
for m in ["fmul", "fmla", "fmls", "fmulx"]:
    add(f"{m} s0, s1, v2.s[1]"); add(f"{m} d0, d1, v2.d[1]")
for t, es in [("8b", 8), ("16b", 8), ("4h", 16), ("8h", 16), ("2s", 32), ("4s", 32), ("2d", 64)]:
    for m in ["srshr", "urshr", "srsra", "ursra", "ssra", "usra", "sshr", "ushr"]:
        for n in sorted({1, 2, es // 2, es - 1, es}): add(f"{m} v0.{t}, v1.{t}, #{n}")
    for n in sorted({1, 2, es // 2, es - 1}): add(f"shl v0.{t}, v1.{t}, #{n}")
for t in ["8b", "16b", "4h", "8h", "2s", "4s"]:
    for m in ["urhadd", "srhadd", "uhadd", "shadd", "uhsub", "shsub", "mul", "mla", "mls"]: add(f"{m} v0.{t}, v1.{t}, v2.{t}")
for t, es in [("4h", "h"), ("8h", "h")]:
    for m in ["mul", "mla", "mls"]:
        for idx in [0, 3, 7]: add(f"{m} v0.{t}, v1.{t}, v2.h[{idx}]")
for t in ["2s", "4s"]:
    for m in ["mul", "mla", "mls"]:
        for idx in [0, 1, 3]: add(f"{m} v0.{t}, v1.{t}, v2.s[{idx}]")
    for m in ["fcvtzs", "fcvtzu"]:
        for n in [1, 8, 31, 32]: add(f"{m} v0.{t}, v1.{t}, #{n}")
    for m in ["fcvtns", "fcvtnu", "fcvtms", "fcvtmu", "fcvtps", "fcvtpu", "fcvtzs", "fcvtzu", "scvtf", "ucvtf", "fsqrt", "fabs", "fneg"]: add(f"{m} v0.{t}, v1.{t}")
for n, w in [("8b", "8h"), ("4h", "4s"), ("2s", "2d")]:
    n2 = {"8b": "16b", "4h": "8h", "2s": "4s"}[n]
    for m in ["uaddl", "saddl", "usubl", "ssubl", "uaddw", "saddw", "usubw", "ssubw", "umull", "smull", "umlal", "smlal", "umlsl", "smlsl", "uabdl", "sabdl", "uabal", "sabal"]:
        if m.endswith("w"): add(f"{m} v0.{w}, v1.{w}, v2.{n}"); add(f"{m}2 v0.{w}, v1.{w}, v2.{n2}")
        else: add(f"{m} v0.{w}, v1.{n}, v2.{n}"); add(f"{m}2 v0.{w}, v1.{n2}, v2.{n2}")
for t in ["2s", "4s", "2d"]:
    for m in ["fmaxnm", "fminnm", "fmax", "fmin", "fabd", "fadd", "fmul", "fdiv", "fsub", "fmaxnmp", "fminnmp", "fmaxp", "fminp", "faddp"]: add(f"{m} v0.{t}, v1.{t}, v2.{t}")
src = r'''#include <stdio.h>
#include <stdint.h>
#include <string.h>
static uint64_t rs = 88172645463325252ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static void fill(uint8_t *p, int mode) {
  for (int k = 0; k < 16; k++) p[k] = (uint8_t)rnd();
  if (mode == 1) { float f[4]; for (int k = 0; k < 4; k++) f[k] = (float)((int)(rnd() % 20001) - 10000) / (float)(1 + rnd() % 50); memcpy(p, f, 16); }
  else if (mode == 2) { double d[2]; for (int k = 0; k < 2; k++) d[k] = (double)((int)(rnd() % 2000001) - 1000000) / (double)(1 + rnd() % 500); memcpy(p, d, 16); }
  else if (mode == 3) { for (int k = 0; k < 16; k++) p[k] = (uint8_t)(rnd() % 3 == 0 ? 0xff : rnd() % 3 == 0 ? 0 : p[k]); }
}
static uint64_t fold(uint64_t h, const uint8_t *p, int n) { for (int k = 0; k < n; k++) h = (h ^ p[k]) * 1099511628211ull; return h; }
'''
for k, f in enumerate(forms):
    src += f'''
static uint64_t form{k}(void) {{ uint64_t h = 1469598103934665603ull;
  for (int it = 0; it < 96; it++) {{ uint8_t a[16], b[16], c[16], o[16]; fill(a, it & 3); fill(b, (it >> 1) & 3); fill(c, it & 3); uint64_t x0 = 0;
    __asm__ volatile("ldr q0, [%1]\\n ldr q1, [%2]\\n ldr q2, [%3]\\n mov x0, #0\\n {f}\\n str q0, [%4]\\n mov %0, x0" : "=r"(x0) : "r"(a), "r"(b), "r"(c), "r"(o) : "x0", "v0", "v1", "v2", "memory");
    h = fold(h, o, 16); h = fold(h, (const uint8_t *)&x0, 8); }}
  return h; }}'''
src += '\nint main(void) {\n'
for k, f in enumerate(forms): src += f'  printf("%016llx {f}\\n", (unsigned long long)form{k}());\n'
src += '  return 0; }\n'
print(src)
