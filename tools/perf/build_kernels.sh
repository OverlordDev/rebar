#!/bin/sh
# Builds tools/perf/kernels.c for x86_64 (native oracle) and arm64 (what rebar runs). Both -O2 -fno-fast-math, matmul/saxpy vectorise at -O2 too (clang vectorises loops at -O2).
set -e
cd "$(dirname "$0")/../.."
OUT=${OUT:-/tmp/perf}; mkdir -p "$OUT"
clang -arch x86_64 -O2 -ffp-contract=off -o "$OUT/kernels_x86" tools/perf/kernels.c -lm
clang -arch arm64 -O2 -ffp-contract=off -o "$OUT/kernels_arm" tools/perf/kernels.c -lm
codesign --force -s - "$OUT/kernels_x86" "$OUT/kernels_arm" 2>/dev/null
echo "built $OUT/kernels_x86 $OUT/kernels_arm"
