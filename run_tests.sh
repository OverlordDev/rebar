#!/bin/sh
# Runs every sample under rebar and compares with the native x86_64 build of the same source.
# Usage: ./run_tests.sh          (build the emulator first with ./build.sh)
cd "$(dirname "$0")/samples" || exit 1
unset CFProcessPath __CFBundleIdentifier   # (a launcher/IDE environment can make NSBundle point at its own bundle: the native oracle then sees the wrong name/bundle)
R=../rebar
pass=0; fail=0

# run_to SECONDS OUTFILE cmd...   (stdout+stderr split: OUTFILE and OUTFILE.err), prints exit status via $RC
run_to() {
    secs=$1; out=$2; shift 2
    "$@" > "$out" 2> "$out.err" &
    pid=$!; n=0
    while kill -0 $pid 2>/dev/null && [ $n -lt $((secs * 2)) ]; do sleep 0.5; n=$((n + 1)); done
    if kill -0 $pid 2>/dev/null; then kill $pid 2>/dev/null; echo "TIMEOUT" >> "$out"; fi
    wait $pid 2>/dev/null; RC=$?
}
check() {  # name, native-out, rebar-out, [sed script to normalize names]
    if [ -n "$4" ]; then sed "$4" "$2" > "$2.n"; sed "$4" "$3" > "$3.n"; else cp "$2" "$2.n"; cp "$3" "$3.n"; fi
    if diff -q "$2.n" "$3.n" >/dev/null; then echo "PASS $1"; pass=$((pass + 1)); else echo "FAIL $1"; diff "$2.n" "$3.n" | head -5; fail=$((fail + 1)); fi
}
T=/tmp/rebar_tests; mkdir -p $T

for t in t1_exit t2_hello t3_calc_O0 t3_calc_O2 hello_arm64; do
    run_to 20 $T/o $R $t; echo "ran $t exit=$RC"
done
run_to 60 $T/neon $R t5_neon; tail -1 $T/neon
run_to 30 $T/a ./t4_fp_x86; run_to 30 $T/b $R t4_fp_arm;             check t4_fp $T/a $T/b
run_to 30 $T/a ./t6_libc_x86; run_to 30 $T/b $R t6_libc_arm;         check t6_libc $T/a $T/b
run_to 30 $T/b $R t6_libc_classic;                                   check t6_libc_classic $T/a $T/b
run_to 30 $T/a ./t9_objc_x86; run_to 30 $T/b $R t9_objc_arm;         check t9_objc $T/a $T/b 's/t9_objc_[a-z0-9]*/NAME/'
run_to 30 $T/a ./t10_x86; run_to 30 $T/b $R t10_arm;                 check t10_structs $T/a $T/b
run_to 30 $T/a ./t12_x86; run_to 30 $T/b $R t12_arm;                 check t12_threads $T/a $T/b
run_to 40 $T/a ./t11_x86; run_to 40 $T/b $R t11_arm;                 check t11_gui $T/a $T/b
run_to 40 $T/a ./t14_x86; run_to 40 $T/b $R t14_arm;                 check t14_ui $T/a $T/b
run_to 30 $T/a ./t15_x86 t15_nib.nib; run_to 30 $T/b $R t15_arm t15_nib.nib; check t15_nib $T/a $T/b
T13_HOLD=1 run_to 40 $T/a ./T13X86.app/Contents/MacOS/t13; T13_HOLD=1 run_to 40 $T/b $R T13Arm.app/Contents/MacOS/t13; check t13_bundle $T/a $T/b 's/T13[A-Za-z0-9]*\.app/APP/'
run_to 30 $T/a ./t16_x86; ra=$RC; run_to 30 $T/b $R t16_arm; rb=$RC; check t16_cxx_exceptions $T/a $T/b; [ $ra = $rb ] || { echo "FAIL t16 exit $ra vs $rb"; fail=$((fail + 1)); }
run_to 40 $T/a ./t17_x86; run_to 40 $T/b $R t17_arm;                 check t17_objc_exceptions $T/a $T/b
run_to 30 $T/a ./t18a_x86; ra=$RC; run_to 30 $T/b $R t18a_arm; rb=$RC; check t18a_uncaught_cxx $T/a $T/b; [ $ra = $rb ] || { echo "FAIL t18a exit $ra vs $rb"; fail=$((fail + 1)); }
run_to 30 $T/a ./t18b_x86; ra=$RC; run_to 30 $T/b $R t18b_arm; rb=$RC; check t18b_uncaught_objc $T/a $T/b; [ $ra = $rb ] || { echo "FAIL t18b exit $ra vs $rb"; fail=$((fail + 1)); }
run_to 30 $T/a ./t7_x86; run_to 30 $T/b $R t7_cxx_arm;               check t7_cxx_stl $T/a $T/b
run_to 30 $T/a ./t25_x86; run_to 60 $T/b $R t25_arm;                 check t25_string $T/a $T/b
run_to 60 $T/b $R t25_arm_O0;                                        check t25_string_O0 $T/a $T/b
run_to 30 $T/b $R t28_wide_arm ./t28_wide_lib.dylib;                if [ "$(cat $T/b)" = "dlopen succeeded
wide_len: 13 2" ]; then echo "PASS t28_wide_string_dlopen"; pass=$((pass + 1)); else echo "FAIL t28_wide_string_dlopen"; cat $T/b; fail=$((fail + 1)); fi
run_to 30 $T/a ./t29_x86; run_to 60 $T/b $R t29_arm;                 check t29_fp16 $T/a $T/b
# FP comparison flags: every ARM condition, ordered and unordered, through CSEL / FCSEL / B.cond (the JIT's flag kind 4)
rm -f t95_arm
clang -arch arm64 -O1 t95_fp_flags.c -o t95_arm || { echo "FAIL t95_fp_flags_build"; fail=$((fail + 1)); }
printf 'all ok\n' > $T/a; run_to 30 $T/b $R t95_arm;                              grep -v '^nvrm' $T/b > $T/b2; check t95_fp_flags $T/a $T/b2
printf 'all ok\n' > $T/a; run_to 30 $T/b env REBAR_JIT=0 $R t95_arm;               grep -v '^nvrm' $T/b > $T/b2; check t95_fp_flags_interp $T/a $T/b2
printf 'all ok\n' > $T/a; run_to 30 $T/b env REBAR_JIT_NOFPFLAGS=1 $R t95_arm;    grep -v '^nvrm' $T/b > $T/b2; check t95_fp_flags_oldpath $T/a $T/b2
# flags handed back by an outlined function that restores only x30 (`cmp w0, #1; ldr x30, [sp], #16; ret`): the JIT's flag liveness must keep them
rm -f t96_arm
clang -arch arm64 -O1 t96_flags_returned.c -o t96_arm || { echo "FAIL t96_flags_returned_build"; fail=$((fail + 1)); }
printf 'all ok\n' > $T/a; run_to 30 $T/b env REBAR_JIT_FLAGPOISON=1 $R t96_arm;                              grep -v '^nvrm' $T/b > $T/b2; check t96_flags_returned $T/a $T/b2
printf 'all ok\n' > $T/a; run_to 30 $T/b env REBAR_JIT=0 $R t96_arm;               grep -v '^nvrm' $T/b > $T/b2; check t96_flags_returned_interp $T/a $T/b2
run_to 60 $T/b $R t30_arm;                                           if [ "$(tail -1 $T/b)" = "all ok" ]; then echo "PASS t30_sha512_addhn"; pass=$((pass + 1)); else echo "FAIL t30_sha512_addhn"; cat $T/b; fail=$((fail + 1)); fi
run_to 60 $T/b $R t33_arm;                                           if [ "$(tail -1 $T/b)" = "blend ok" ]; then echo "PASS t33_neon_blend"; pass=$((pass + 1)); else echo "FAIL t33_neon_blend"; cat $T/b; fail=$((fail + 1)); fi
run_to 60 $T/b2 env REBAR_NO_SIMD_FAST=1 $R t33_arm;                if cmp -s $T/b $T/b2; then echo "PASS t33_neon_blend_general_path"; pass=$((pass + 1)); else echo "FAIL t33_neon_blend_general_path"; diff $T/b $T/b2 | head -3; fail=$((fail + 1)); fi
run_to 30 $T/a ./t34_x86; run_to 60 $T/b $R t34_arm;                 grep -v '^nvrm' $T/a > $T/a2; grep -v '^nvrm' $T/b > $T/b2; check t34_system_constants $T/a2 $T/b2
run_to 30 $T/a ./t35_x86; run_to 60 $T/b $R t35_arm;                 grep -v '^nvrm' $T/a > $T/a2; grep -v '^nvrm' $T/b > $T/b2; check t35_ssl_cipher_suite_width $T/a2 $T/b2
run_to 30 $T/a ./t36_x86; run_to 60 $T/b $R t36_arm;                 grep -v '^nvrm' $T/a > $T/a2; grep -v '^nvrm' $T/b > $T/b2; check t36_swift_calling_convention $T/a2 $T/b2
run_to 30 $T/a ./t37_x86; run_to 60 $T/b $R t37_arm;                 grep -v '^nvrm' $T/a > $T/a2; grep -v '^nvrm' $T/b > $T/b2; check t37_swift_runtime_metadata $T/a2 $T/b2
run_to 30 $T/a ./t38_x86; run_to 60 $T/b $R t38_arm;                 grep -v '^nvrm' $T/a > $T/a2; grep -v '^nvrm' $T/b > $T/b2; check t38_swift_errors_and_codable $T/a2 $T/b2
run_to 30 $T/a ./t39_x86; run_to 60 $T/b $R t39_arm;                 grep -v '^nvrm' $T/a > $T/a2; grep -v '^nvrm' $T/b > $T/b2; check t39_exception_unwind_d_registers $T/a2 $T/b2
run_to 30 $T/a ./t40_x86; run_to 60 $T/b $R t40_arm;                 grep -v '^nvrm' $T/a > $T/a2; grep -v '^nvrm' $T/b > $T/b2; check t40_chkstk_keeps_registers $T/a2 $T/b2
run_to 30 $T/a ./t41_x86; run_to 60 $T/b $R t41_arm;                 grep -v '^nvrm' $T/a > $T/a2; grep -v '^nvrm' $T/b > $T/b2; check t41_locale_returned_by_value $T/a2 $T/b2
run_to 30 $T/a ./t42_x86_dir/main; run_to 60 $T/b $R t42_arm_dir/main;      grep -v '^nvrm' $T/a > $T/a2; grep -v '^nvrm' $T/b > $T/b2; check t42_bundle_for_class $T/a2 $T/b2
run_to 30 $T/a ./t43_x86; run_to 60 $T/b $R t43_arm;                 grep -v '^nvrm' $T/a > $T/a2; grep -v '^nvrm' $T/b > $T/b2; check t43_dwarf_unwind $T/a2 $T/b2
run_to 30 $T/b $R t44_arm;                                           if grep -q "hw.optional.neon: yes" $T/b && grep -q "hw.optional.armv8_crc32: yes" $T/b && grep -q "hw.ncpu: yes" $T/b; then echo "PASS t44_cpu_features"; pass=$((pass + 1)); else echo "FAIL t44_cpu_features"; cat $T/b; fail=$((fail + 1)); fi
run_to 30 $T/a ./t45_x86; run_to 60 $T/b $R t45_arm;                 grep -v '^nvrm' $T/a > $T/a2; grep -v '^nvrm' $T/b > $T/b2; check t45_dlsym_dlerror $T/a2 $T/b2
run_to 30 $T/a ./t46_x86; run_to 60 $T/b $R t46_arm;                 grep -v '^nvrm' $T/a > $T/a2; grep -v '^nvrm' $T/b > $T/b2; check t46_filesystem_status $T/a2 $T/b2
run_to 30 $T/a ./t47_x86; run_to 60 $T/b $R t47_arm;                 grep -v '^nvrm' $T/a > $T/a2; grep -v '^nvrm' $T/b > $T/b2; check t47_wstring $T/a2 $T/b2
run_to 60 $T/a ./t48_x86; run_to 90 $T/b $R t48_arm;                 check t48_thread_local_variables $T/a $T/b
run_to 60 $T/a ./t49_x86; run_to 120 $T/b $R t49_arm;                check t49_atomics_ordered_lse $T/a $T/b
run_to 60 $T/a ./t51_x86; run_to 120 $T/b env REBAR_JIT_THRESHOLD=1 $R t51_arm;  check t51_leaf_imports_div $T/a $T/b
run_to 120 $T/b env REBAR_JIT_NOLEAF=1 REBAR_JIT_THRESHOLD=1 $R t51_arm;      check t51_leaf_imports_div_noleaf $T/a $T/b
run_to 30 $T/a ./t52_x86; run_to 60 $T/b $R t52_arm;                 grep -v '^nvrm' $T/b > $T/b2; check t52_nsstring_va_list $T/a $T/b2
run_to 30 $T/a ./t55_x86; run_to 30 $T/b $R t55_arm;                                  grep -v '^nvrm' $T/b > $T/b2; check t55_open_mode $T/a $T/b2
printf 'children failed: 0\n' > $T/a; run_to 120 $T/b env REBAR_JIT_THRESHOLD=1 $R t56_arm;       grep -v '^nvrm' $T/b > $T/b2; check t56_fork_child $T/a $T/b2
printf 'answer localhost. 127.0.0.1 rrclass 1\nok\n' > $T/a; run_to 60 $T/b $R t57_arm;                          grep -v '^nvrm' $T/b > $T/b2; check t57_callback_stack_args $T/a $T/b2
printf 'sqdmull indexed: 16400 checks, mismatches 0\n' > $T/a; run_to 60 $T/b $R t59_arm;                  grep -v '^nvrm' $T/b > $T/b2; check t59_sqdmull_indexed $T/a $T/b2
run_to 30 $T/a ./t60_x86; run_to 30 $T/b $R t60_arm;                                  grep -v '^nvrm' $T/b > $T/b2; check t60_tsd $T/a $T/b2
printf 'main direct 1234\nworker direct 7\n' > $T/a; run_to 30 $T/b $R t61_arm;       grep -v '^nvrm' $T/b > $T/b2; check t61_tsd_direct $T/a $T/b2
run_to 30 $T/a ./t62_x86; run_to 60 $T/b $R t62_arm;                                  grep -v '^nvrm' $T/b > $T/b2; check t62_promise_future $T/a $T/b2
printf 'sigaction 0 survived\n' > $T/a; run_to 30 $T/b $R t63_arm;                              grep -v '^nvrm' $T/b > $T/b2; check t63_sigurg $T/a $T/b2
printf 'child: hello, arm64\nstatus 0\n' > $T/a; run_to 60 $T/b $R t64_arm "$PWD/t2_hello";   grep -v '^nvrm' $T/b > $T/b2; check t64_fork_exec_pipe $T/a $T/b2
printf 'status pipe: read 0 err 0\nchild: hello, arm64\nexit 0\n' > $T/a; run_to 60 $T/b $R t65_arm "$PWD/t2_hello"; grep -v '^nvrm' $T/b > $T/b2; check t65_exec_status_pipe $T/a $T/b2
printf 'sum 20100\nafter iallu 5\n' > $T/a; run_to 30 $T/b $R t66_arm;                          grep -v '^nvrm' $T/b > $T/b2; check t66_icache_sys $T/a $T/b2
printf 'sum 20100\nafter iallu 5\n' > $T/a; run_to 30 $T/b env REBAR_JIT_THRESHOLD=1 $R t66_arm; grep -v '^nvrm' $T/b > $T/b2; check t66_icache_sys_jit $T/a $T/b2
printf 'fetch_add 1600000 cas-lock 1600000 ticket 1600000 mutex 1600000 (expected 1600000)\n' > $T/a; run_to 90 $T/b $R t75_lse;  grep -v '^nvrm' $T/b > $T/b2; check t75_atomics_lse $T/a $T/b2
printf 'fetch_add 1600000 cas-lock 1600000 ticket 1600000 mutex 1600000 (expected 1600000)\n' > $T/a; run_to 90 $T/b $R t75_llsc; grep -v '^nvrm' $T/b > $T/b2; check t75_atomics_llsc $T/a $T/b2
printf 'ulock32 ok 0\nulock64 ok 0\n' > $T/a; run_to 60 $T/b $R t76_arm; grep -v '^nvrm' $T/b > $T/b2; check t76_ulock $T/a $T/b2
printf 'read 17: through the fifo\ndone\n' > $T/a; run_to 30 $T/b $R t77_arm; grep -v '^nvrm' $T/b > $T/b2; check t77_fifo_open $T/a $T/b2
run_to 30 $T/a ./t78_x86; run_to 30 $T/b $R t78_arm; grep -v '^nvrm' $T/b > $T/b2; check t78_sigaltstack $T/a $T/b2
run_to 30 $T/a ./t79_x86; run_to 30 $T/b $R t79_arm; grep -v '^nvrm' $T/b > $T/b2; check t79_spawnp_path $T/a $T/b2
run_to 30 $T/a ./t81_x86; run_to 30 $T/b $R t81_arm; grep -v '^nvrm' $T/b > $T/b2; check t81_ifstream_seek $T/a $T/b2
run_to 30 $T/a ./t82_x86; run_to 30 $T/b $R t82_arm; grep -v '^nvrm' $T/b > $T/b2; check t82_any_cast $T/a $T/b2
run_to 30 $T/a ./t83_x86; run_to 30 $T/b $R t83_arm; grep -v '^nvrm' $T/b > $T/b2; check t83_fs_current_path $T/a $T/b2
run_to 30 $T/a ./t84_x86; run_to 30 $T/b $R t84_arm; grep -v '^nvrm' $T/b > $T/b2; check t84_exc_across_dylibs $T/a $T/b2
printf 'SIGSEGV 1 at 0x10\nrecovered after fault 1\n' > $T/a; run_to 30 $T/b $R t85_arm; grep -v '^nvrm' $T/b > $T/b2; check t85_guest_segv_handler $T/a $T/b2
run_to 30 $T/a ./t86_x86; run_to 30 $T/b $R t86_arm; grep -v '^nvrm' $T/b > $T/b2; check t86_string_iter $T/a $T/b2
run_to 30 $T/a ./t87_x86; run_to 30 $T/b $R t87_arm; grep -v '^nvrm' $T/b > $T/b2; check t87_nsrange_stack $T/a $T/b2
printf '389 89 a rXaher long string 1234567890123456789z\nown allocations used\n' > $T/a; run_to 30 $T/b $R t88_arm; grep -v '^nvrm' $T/b > $T/b2; check t88_replaced_allocator_strings $T/a $T/b2
printf '/private/tmp duplicate 1 42-formatted\nfreed ok, own allocations used\n' > $T/a; run_to 30 $T/b $R t89_arm; grep -v '^nvrm' $T/b > $T/b2; check t89_replaced_allocator_host_results $T/a $T/b2
run_to 30 $T/a ./t90_x86; run_to 30 $T/b $R t90_arm; grep -v '^nvrm' $T/b > $T/b2; check t90_guest_zone_size $T/a $T/b2
printf 'spawn 0\nshell said: hello, arm64\nstatus 0\n' > $T/a; run_to 60 $T/b $R t67_arm "$PWD/t2_hello"; grep -v '^nvrm' $T/b > $T/b2; check t67_shell_runs_arm64 $T/a $T/b2
run_to 30 $T/a ./t68_x86; run_to 30 $T/b $R t68_arm;                                  grep -v '^nvrm' $T/b > $T/b2; check t68_variadic_ipc $T/a $T/b2
run_to 30 $T/a ./t69_x86; run_to 30 $T/b $R t69_arm;                                  grep -v '^nvrm' $T/b > $T/b2; check t69_wprintf $T/a $T/b2
run_to 60 $T/a ./t70_x86 x; run_to 60 $T/b $R t70_arm x;                              grep -v '^nvrm' $T/b > $T/b2; check t70_tsearch_callbacks $T/a $T/b2
run_to 60 $T/a ./t71_x86; run_to 60 $T/b $R t71_arm;                                  grep -v '^nvrm' $T/b > $T/b2; check t71_malloc_leaf $T/a $T/b2
run_to 60 $T/b env REBAR_JIT_THRESHOLD=1 $R t71_arm;                                      grep -v '^nvrm' $T/b > $T/b2; check t71_malloc_leaf_jit1 $T/a $T/b2
run_to 60 $T/a ./t72_x86; run_to 60 $T/b $R t72_arm;                                  grep -v '^nvrm' $T/b > $T/b2; check t72_directory_iterator $T/a $T/b2
run_to 60 $T/a ./t73_x86; run_to 60 $T/b $R t73_arm;                                  grep -v '^nvrm' $T/b > $T/b2; check t73_numpunct_sret $T/a $T/b2
printf 'dczid dzp=0\nctr dminline=4 iminline=4\n' > $T/a; run_to 30 $T/b $R t54_arm;                       grep -v '^nvrm' $T/b > $T/b2; check t54_sysregs $T/a $T/b2
printf 'child: hello, arm64\nstatus 0\n' > $T/a; run_to 60 $T/b $R t53_arm "$PWD/t2_hello";  grep -v '^nvrm' $T/b > $T/b2; check t53_nstask_arm64 $T/a $T/b2
run_to 60 $T/b env REBAR_JIT_THRESHOLD=1 $R t50_arm;                 if grep -q "^t50 flags across calls: bad 0 sum 1200006" $T/b; then echo "PASS t50_flags_across_calls"; pass=$((pass + 1)); else echo "FAIL t50_flags_across_calls"; cat $T/b | head -3; fail=$((fail + 1)); fi
$R --simd-selftest 60000 > $T/st 2>&1;                               if tail -1 $T/st | grep -q " 0 mismatches"; then echo "PASS simd_selftest"; pass=$((pass + 1)); else echo "FAIL simd_selftest"; tail -5 $T/st; fail=$((fail + 1)); fi
run_to 120 $T/jt $R --jit-selftest 60000 1;                          if grep -q "jit-selftest" $T/jt.err && [ "$(grep -c '^MISMATCH' $T/jt.err)" -le "$(grep -c '^MISMATCH (died: interpreter 0, jit 1)' $T/jt.err)" ]; then echo "PASS jit_selftest"; pass=$((pass + 1)); else echo "FAIL jit_selftest"; grep '^MISMATCH' $T/jt.err | head -3; fail=$((fail + 1)); fi
for tpl in dot-vec dot-idx fcvtl fcvtn ld1-multi v3-fmax v3-fmaxnm v3-faddp v3-ushl4s trn m2-fredv m2-addv4s m2-fsqrt tbl m2-fcm0 ld1-lane m2-fcvtns m2-scvtf-fixed; do
    run_to 120 $T/jt $R --jit-selftest 30000 5 $tpl
    if grep -q " 0 mismatches" $T/jt.err || { grep -q "^MISMATCH" $T/jt.err && [ "$(grep -c '^MISMATCH' $T/jt.err)" -le "$(grep -c '^MISMATCH (died: interpreter 0, jit 1)' $T/jt.err)" ]; }; then echo "PASS jit_selftest_$tpl"; pass=$((pass + 1)); else echo "FAIL jit_selftest_$tpl"; grep -a "jit-selftest" $T/jt.err | head -2; fail=$((fail + 1)); fi
done
run_to 60 $T/mixa $R bench/bench_mix_arm 1; run_to 60 $T/mixn ./bench/bench_mix_x86 1;  check bench_mix_jit $T/mixn $T/mixa
run_to 60 $T/mixb env REBAR_JIT=0 $R bench/bench_mix_arm 1;           check bench_mix_interpreter_only $T/mixn $T/mixb
run_to 30 $T/a ./t31_x86; run_to 60 $T/b $R t31_arm;                 grep -v '^nvrm' $T/a > $T/a2; grep -v '^nvrm' $T/b > $T/b2; check t31_super_structs $T/a2 $T/b2
run_to 30 $T/a ./t32_x86; run_to 60 $T/b $R t32_arm;                 check t32_threads_exceptions $T/a $T/b
run_to 30 $T/a ./t26_x86; run_to 60 $T/b $R t26_arm;                 check t26_sstream_O0 $T/a $T/b
run_to 60 $T/b $R t26_arm_O2;                                        check t26_sstream_O2 $T/a $T/b
run_to 40 $T/a ./t19_x86; run_to 40 $T/b $R t19_arm;                 check t19_host_catch $T/a $T/b
# guest dylibs: @rpath, per-image TLS/ObjC/C++ unwinding, dlopen/dlsym of guest plugins
run_to 30 $T/a ./dylib/out_x86_64/t20; run_to 30 $T/b $R ./dylib/out_arm64/t20; check t20_dylibs $T/a $T/b
# ARMv8 crypto/CRC instructions against standard test vectors
run_to 30 $T/b $R t21_arm
if grep -q "^aes128 69c4e0d86a7b0430d8cdb78070b4c55a" $T/b && grep -q "^sha256 ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" $T/b && grep -q "^sha1   a9993e364706816aba3e25717850c26c9cd0d89d" $T/b && grep -q "^crc32  cbf43926" $T/b && grep -q "^crc32c e3069283" $T/b; then echo "PASS t21_crypto"; pass=$((pass + 1)); else echo "FAIL t21_crypto"; fail=$((fail + 1)); fi
# an arm64 program starting other arm64 programs (posix_spawn, posix_spawnp, fork+execvp)
run_to 30 $T/b $R ./t24_arm
if grep -q "^posix_spawn r=0 exit=7" $T/b && grep -q "^posix_spawnp r=0 exit=7" $T/b && grep -q "^fork+execvp exit=7" $T/b \
   && [ "$(grep -c '^child ok' $T/b)" = 3 ]; then echo "PASS t24_spawn"; pass=$((pass + 1)); else echo "FAIL t24_spawn"; cat $T/b; fail=$((fail + 1)); fi
# the arm64-only Rebar Notes app against its x86_64 build
export REBAR_NOTES_DIR=$T/notes; rm -rf $REBAR_NOTES_DIR
REBAR_SELFTEST=1 run_to 60 $T/a ./app/RebarNotes-x86_64.app/Contents/MacOS/RebarNotes
REBAR_SELFTEST=1 run_to 90 $T/b $R ./app/RebarNotes-arm64.app/Contents/MacOS/RebarNotes; check rebar_notes_app $T/a $T/b
# the real VS Code CLI (arm64 Rust binary), if it is around
VSC=${REBAR_VSCODE_CLI:-}   # path of the arm64 `code` CLI
if [ -n "$VSC" ]; then run_to 60 $T/b $R "$VSC" --version; grep -q "^code " $T/b && { echo "PASS vscode_cli --version: $(cat $T/b)"; pass=$((pass + 1)); } || { echo "FAIL vscode_cli"; fail=$((fail + 1)); }; fi
# the arm64 Electron binary of VS Code running JavaScript (V8 + Node, ELECTRON_RUN_AS_NODE), if it is around
APP=${REBAR_VSCODE_APP:-}   # path of an arm64 "Visual Studio Code.app"
# the translator against the interpreter on real instruction runs taken from the Electron framework, if it is around.
# ('died: interpreter 0, jit 1' is expected: a load into wzr/xzr, V8's null-check idiom, is dropped by the interpreter and made by the translator, so a random bad pointer faults only there.)
EFW="$APP/Contents/Frameworks/Electron Framework.framework/Versions/A/Electron Framework"
if [ -n "$APP" ] && [ -f "$EFW" ]; then
    # (the real-code sequences store through whatever addresses their registers hold; about one run in ten damages the harness's own heap and aborts: try again)
    for try in 1 2 3; do
        run_to 500 $T/js $R --jit-selftest 50000 1 - scan "$EFW"
        grep -q "jit-selftest" $T/js.err && break
    done
    if grep -q "jit-selftest" $T/js.err && [ "$(grep '^MISMATCH' $T/js.err | grep -vc 'died: interpreter 0, jit 1')" -le 3 ]; then echo "PASS jit_scan_electron_framework: $(grep jit-selftest $T/js.err | cut -c1-70)"; pass=$((pass + 1)); else echo "FAIL jit_scan_electron_framework"; grep -A4 '^MISMATCH' $T/js.err | grep -v 'died: interpreter 0, jit 1' | head -12; fail=$((fail + 1)); fi
fi
if [ -n "$APP" ]; then
    ELECTRON_RUN_AS_NODE=1 run_to 120 $T/b $R "$APP/Contents/MacOS/Code" ./t23_node.js
    if grep -q "^arch arm64 darwin true" $T/b && grep -q "^fib 6765" $T/b && grep -q "^sha256 6112b742a77f125f4e953b48d94dd8f624fb0937690d0038caa353d2f80f56f5" $T/b \
       && grep -q "^gunzip 300" $T/b && grep -q "^sha512 ddaf35a193617abacc417349ae204131" $T/b && grep -q "^wasm 5" $T/b && grep -q "^regex 1|144" $T/b && [ "$(tail -2 $T/b | tr '\n' ' ')" = "microtask timer " ]; then
        echo "PASS electron_node"; pass=$((pass + 1))
    else echo "FAIL electron_node"; cat $T/b $T/b.err | head -10; fail=$((fail + 1)); fi
    # VS Code's own JavaScript command line (ESM with top-level await) on that Electron
    ELECTRON_RUN_AS_NODE=1 run_to 120 $T/b $R "$APP/Contents/MacOS/Code" "$APP/Contents/Resources/app/out/cli.js" --version
    if [ "$(sed -n 1p $T/b)" = "1.144.0" ] && [ "$(sed -n 3p $T/b)" = "arm64" ]; then echo "PASS electron_vscode_cli_js: $(sed -n 1p $T/b) $(sed -n 3p $T/b)"; pass=$((pass + 1))
    else echo "FAIL electron_vscode_cli_js"; cat $T/b $T/b.err | head -10; fail=$((fail + 1)); fi
    # a native Node addon of VS Code (arm64 C++ using libc++ strings, CFPreferences, FSEvents)
    ELECTRON_RUN_AS_NODE=1 run_to 240 $T/b $R "$APP/Contents/MacOS/Code" ./t27_policy_watcher.js "$APP/Contents/Resources/app/node_modules.asar.unpacked/@vscode/policy-watcher/build/Release/vscode-policy-watcher.node"
    if [ "$(tr '\n' '|' < $T/b)" = "loaded function|created dispose|update {}|disposed|error: Unknown policy type 'bogus'|" ]; then echo "PASS electron_policy_watcher_addon"; pass=$((pass + 1))
    else echo "FAIL electron_policy_watcher_addon"; cat $T/b $T/b.err | head -10; fail=$((fail + 1)); fi
fi
# neovim (LuaJIT: its errors are thrown through the personality routine of the VM's own unwind info), when the tarball of the pack is there
NVT=$HOME/Desktop/arm64-apps/nvim-macos-arm64.tar.gz
if [ -f "$NVT" ]; then
    mkdir -p $T/nvim && tar -xzf "$NVT" -C $T/nvim 2>/dev/null
    NV=$(ls -d $T/nvim/*/bin/nvim 2>/dev/null | head -1)
    if [ -n "$NV" ]; then
        printf 'local ok, e = pcall(error, "boom")\nprint(ok, e)\nprint(select(2, pcall(function() local x = nil; return x.y end)))\nlocal s = 0 for i = 1, 100000 do s = s + i end print(s)\n' > $T/nv.lua
        env HOME=$T TERM=xterm $R "$NV" --headless -u NONE -l $T/nv.lua > $T/b 2> $T/b.err
        printf 'false boom\n%s/nv.lua:3: attempt to index local '"'"'x'"'"' (a nil value)\n5000050000\n' "$T" > $T/a
        cat $T/b $T/b.err | grep -v '^nvrm' > $T/b2; if cmp -s $T/a $T/b2; then echo "PASS nvim_luajit_errors"; pass=$((pass + 1)); else echo "FAIL nvim_luajit_errors"; cat $T/b $T/b.err | head -6; fail=$((fail + 1)); fi
    fi
fi
# Framework tests (samples/fw_*.swift): built for arm64 and x86_64 with swiftc, output of the arm64 build under rebar must equal the native x86_64 one
for src in fw_*.swift; do
    fwn=${src%.swift}
    flags=""; [ "$fwn" = fw_swift_concurrency ] && flags="-parse-as-library"
    for a in arm64 x86_64; do
        sfx=arm; [ $a = x86_64 ] && sfx=x86
        [ -x ${fwn}_$sfx ] && [ ${fwn}_$sfx -nt $src ] || swiftc $flags -O -target $a-apple-macos15.0 -o ${fwn}_$sfx $src 2>/dev/null
    done
    if [ -x ${fwn}_arm ] && [ -x ${fwn}_x86 ]; then
        run_to 60 $T/a ./${fwn}_x86; run_to 120 $T/b $R ${fwn}_arm; grep -v '^nvrm' $T/b | grep -v '^rebar: warning' > $T/b2; check $fwn $T/a $T/b2
    else echo "SKIP $fwn (no swiftc?)"; fi
done

# std::filesystem: path iteration (begin / end / ++ hand out iterators over the guest's path), status / symlink_status / space returned in memory
for cn in cpp_path_iter cpp_filesystem; do
    for a in arm x86; do ar=arm64; [ $a = x86 ] && ar=x86_64; [ -x ${cn}_$a ] && [ ${cn}_$a -nt $cn.cpp ] || clang++ -std=c++20 -O1 -arch $ar $cn.cpp -o ${cn}_$a 2>/dev/null; done
    run_to 30 $T/a ./${cn}_x86; run_to 60 $T/b $R ${cn}_arm; grep -v '^nvrm' $T/b > $T/b2; check $cn $T/a $T/b2
done
# Objective-C runtime and Foundation in one program: struct returns, runtime classes, swizzling, forwarding, blocks, weak, exceptions, GCD, operations, KVC, predicates, formatters
for a in arm x86; do ar=arm64; [ $a = x86 ] && ar=x86_64; [ -x objc_torture_$a ] && [ objc_torture_$a -nt objc_torture.m ] || clang -fobjc-arc -arch $ar -framework Foundation -framework CoreGraphics objc_torture.m -o objc_torture_$a 2>/dev/null; done
run_to 60 $T/a ./objc_torture_x86; run_to 120 $T/b $R objc_torture_arm; grep -v '^nvrm' $T/b > $T/b2; check objc_torture $T/a $T/b2
# arm64e (pointer authentication; the signing instructions are no-ops, the chained fixups carry signed pointers): the same programs built for arm64e must give the x86_64 output
[ -x objc_torture_arme ] && [ objc_torture_arme -nt objc_torture.m ] || clang -fobjc-arc -arch arm64e -framework Foundation -framework CoreGraphics objc_torture.m -o objc_torture_arme 2>/dev/null
run_to 60 $T/a ./objc_torture_x86; run_to 120 $T/b $R objc_torture_arme; grep -v '^nvrm' $T/b > $T/b2; check arm64e_objc $T/a $T/b2
[ -x cpp_stdlib_arme ] && [ cpp_stdlib_arme -nt cpp_stdlib.cpp ] || clang++ -std=c++20 -O1 -arch arm64e cpp_stdlib.cpp -o cpp_stdlib_arme 2>/dev/null
run_to 60 $T/a ./cpp_stdlib_x86; run_to 120 $T/b $R cpp_stdlib_arme; grep -v '^nvrm' $T/b > $T/b2; check arm64e_cpp $T/a $T/b2
[ -x fw_swift_foundation_arme ] && [ fw_swift_foundation_arme -nt fw_swift_foundation.swift ] || swiftc -O -target arm64e-apple-macos15.0 -o fw_swift_foundation_arme fw_swift_foundation.swift 2>/dev/null
run_to 60 $T/a ./fw_swift_foundation_x86; run_to 120 $T/b $R fw_swift_foundation_arme; grep -v '^nvrm' $T/b | grep -v '^rebar: warning' > $T/b2; check arm64e_swift $T/a $T/b2
# FEAT_FRINTTS (FRINT32Z/32X/64Z/64X): the expected output follows the architecture manual's pseudocode (no arm64 machine to compare with)
clang -arch arm64 -march=armv8.5-a -O1 frintts.c -o frintts_arm 2>/dev/null; run_to 30 $T/b $R frintts_arm; grep -v '^nvrm' $T/b > $T/b2; check frintts frintts.expected $T/b2
# ARMv8.2-8.6 extensions (FCMA, FHM, I8MM, BF16, RDM, FlagM, LDTR, LDRAA, PACGA): program compares every instruction with a C model, expected output is all zeros
clang -arch arm64 -march=armv8.6-a+fp16+fp16fml+i8mm+bf16+rcpc+rdm+fcma+flagm+pauth -O1 isa_ext.c -o isa_ext_arm 2>/dev/null; run_to 60 $T/b $R isa_ext_arm; grep -v '^nvrm' $T/b > $T/b2; check isa_ext isa_ext.expected $T/b2
# newer extensions (CSSC, HBC, CPA, CASP, LSE128, THE/RCW, LRCPC2/3, MOPS, MTE arithmetic) and the saturating shift / SQDMLAL / USQADD / URECPE / FRECPX forms: C model vs inline asm, expected all zeros
clang -arch arm64 -march=armv9.5-a+fp16+rcpc3+cssc+lse128+mops+hbc+the+cpa+memtag+pauth-lr+d128+lse -O1 isa_ext2.c -o isa_ext2_arm 2>/dev/null; run_to 60 $T/b $R isa_ext2_arm; grep -v '^nvrm' $T/b > $T/b2; check isa_ext2 isa_ext2.expected $T/b2
# translation cache: the second run of a program maps the code snapshot (and block list) it finds in REBAR_JIT_CACHE_DIR, restores its blocks from it and prints the same output
CD=$(mktemp -d); clang -arch arm64 -march=armv8.6-a+fp16+rcpc3+cssc+lse128+mops+hbc+the+cpa+memtag+pauth-lr+d128+lse -O1 isa_ext2.c -o isa_ext2_c 2>/dev/null
REBAR_JIT_CACHE_DIR=$CD $R isa_ext2_c > $T/c1 2>&1; REBAR_JIT_CACHE_DIR=$CD REBAR_JIT_CACHE_DEBUG=1 $R isa_ext2_c > $T/c2 2>&1
grep -q "[1-9][0-9]* restored from the snapshot" $T/c2 && grep -v "^\[jitcache\]" $T/c2 | grep -v '^nvrm' > $T/c2b && grep -v '^nvrm' $T/c1 > $T/c1b; check jit_cache $T/c1b $T/c2b; rm -rf $CD
# the JIT's NEON templates against the interpreter: ~630 forms (widening, pairwise, halving, shifts, multiplies by element, conversions) on random data, JIT on and off must print the same checksums
python3 ../tools/neonforms.py > $T/nf.c 2>/dev/null && clang -arch arm64 -O0 -march=armv8.2-a+fp16 $T/nf.c -o $T/nf_arm 2>/dev/null
REBAR_JIT_CACHE=0 REBAR_JIT=0 $R $T/nf_arm 2>&1 | grep -v '^nvrm' > $T/nf0; REBAR_JIT_CACHE=0 REBAR_JIT_THRESHOLD=1 $R $T/nf_arm 2>&1 | grep -v '^nvrm' > $T/nf1; [ -s $T/nf0 ] && check neon_forms_jit $T/nf0 $T/nf1
# libc++ (C++20) in one program: streams, filesystem, regex, threads, futures, ranges, format, locale, containers
for a in arm x86; do ar=arm64; [ $a = x86 ] && ar=x86_64; [ -x cpp_stdlib_$a ] && [ cpp_stdlib_$a -nt cpp_stdlib.cpp ] || clang++ -std=c++20 -O1 -arch $ar cpp_stdlib.cpp -o cpp_stdlib_$a 2>/dev/null; done
run_to 60 $T/a ./cpp_stdlib_x86; run_to 120 $T/b $R cpp_stdlib_arm; grep -v '^nvrm' $T/b > $T/b2; check cpp_stdlib $T/a $T/b2

printf '8e4da1bc9fdc589d01010101c6c6c6c6\nroundtrip ok\n' > $T/a; run_to 30 $T/b $R t92_arm; grep -v '^nvrm' $T/b > $T/b2; check t92_aesmc_rd $T/a $T/b2
run_to 30 $T/a ./t93_x86; run_to 30 $T/b $R t93_arm; grep -v '^nvrm' $T/b > $T/b2; check t93_environ_live $T/a $T/b2
[ -x t94_arm ] || clang -arch arm64 -o t94_arm t94_pagesize.c; printf 'same 1\npages fit in memory 1\n' > $T/a; run_to 30 $T/b $R t94_arm; grep -v '^nvrm' $T/b > $T/b2; check t94_pagesize $T/a $T/b2

# Homebrew arm64 bottles (tools/rebrew.py install jq sqlite lua ripgrep tmux): checked when the prefix exists
HB=${REBREW_PREFIX:-$HOME/Desktop/arm64-apps/homebrew}
hbcheck() {  # name, expected, tool, args...
    nm=$1; ex=$2; shift 2; tool=$1; shift
    [ -x "$HB/bin/$tool" ] || { echo "SKIP $nm (not installed)"; return; }
    printf '%s\n' "$ex" > $T/a; run_to 30 $T/b $R "$HB/bin/$tool" "$@"; grep -v '^nvrm' $T/b | head -1 > $T/b2; check $nm $T/a $T/b2
}
hbcheck brew_sqlite 42 sqlite3 :memory: "select 6*7"
hbcheck brew_lua 42 lua -e 'print(6*7)'
[ -x "$HB/bin/rg" ] && { echo hello-rebar > $T/rg.txt; printf '1:hello-rebar\n' > $T/a; run_to 30 $T/b $R "$HB/bin/rg" -n hello $T/rg.txt; grep -v '^nvrm' $T/b > $T/b2; check brew_rg $T/a $T/b2; }
hbcheck brew_tmux "tmux 3.8" tmux -V
# a shell script run as `rebar script`: the host shell runs it, the arm64 programs it starts run under rebar (the exec shim)
if [ -x "$HB/bin/python3.13" ]; then
    printf '#!/bin/sh\nexec "%s/bin/python3.13" -c "import sys; print(\\"hostscript\\", 6 * 7)"\n' "$HB" > $T/host.sh; chmod +x $T/host.sh
    echo "hostscript 42" > $T/a; REBAR_PATHMAP=/opt/homebrew=$HB run_to 30 $T/b $R $T/host.sh; grep -v '^nvrm' $T/b | head -1 > $T/b2; check host_shell_script $T/a $T/b2
fi
# GObject signals without a marshaller go through libffi (glib's generic closure marshal), GVariant, GMainLoop, GError, UTF-8
if [ -f "$HB/lib/libgobject-2.0.dylib" ]; then
    clang -arch arm64 -I$HB/include/glib-2.0 -I$HB/lib/glib-2.0/include -L$HB/lib -lgobject-2.0 -lglib-2.0 -Wl,-rpath,$HB/lib glib_gobject.c -o glib_gobject_arm 2>/dev/null
    printf "changed 42 hello ud\ncompute 11.0\ntransform 7.500000\nvariant {'a': <[1, 2, 3]>, 'b': <('x', 2.5)>}\nhash 5\nloop done\ngerror 1\nutf8 5 STRASSE\n" > $T/a
    REBAR_PATHMAP=/opt/homebrew=$HB run_to 60 $T/b $R glib_gobject_arm; grep -v '^nvrm' $T/b > $T/b2; check glib_gobject $T/a $T/b2
fi
# a script whose #! interpreter is an arm64 program is run through that interpreter
if [ -x "$HB/bin/python3.13" ]; then
    printf '#!%s/bin/python3.13 -u\nimport sys\nprint("shebang", len(sys.argv), sys.argv[1:])\n' "$HB" > $T/she.py; chmod +x $T/she.py
    echo "shebang 3 ['p', 'q']" > $T/a; REBAR_PATHMAP=/opt/homebrew=$HB run_to 30 $T/b $R $T/she.py p q; grep -v '^nvrm' $T/b | head -1 > $T/b2; check shebang_script $T/a $T/b2
fi
# Node (V8, workers, crypto, zlib, http, fs including recursive rm through std::filesystem, wasm, Intl)
if [ -x "$HB/bin/node" ]; then
    printf 'fs hello 5\nwatch ok\nzlib 300 brotli\ncrypto 2d711642 8 2b90e4\ned25519 true\naesgcm secret\nhttp echo:ping\nnet tcp!\nchild hi 3\nworker 42\natomics 5\nwasm 42\nintl 1.234.567,891 1970/1/1 SS 1\nregex 11 a_b_\nbigint 265252859812191058636308480000000\njit 28665 true\nfetch hi\ndns ::1\ntimers ok\nhrtime true darwin arm64\n' > $T/a
    REBAR_PATHMAP=/opt/homebrew=$HB run_to 120 $T/b $R "$HB/bin/node" node_features.js; grep -v '^nvrm' $T/b > $T/b2; check node_features $T/a $T/b2 's/^dns ::1/dns 127.0.0.1/'
fi
# Rust (rustc built for arm64): panics unwind through guest frames running drop glue (a guest personality routine asks for the LSDA and the IP), catch_unwind, threads, std
if [ -x "$HB/bin/rustc" ]; then
    REBAR_PATHMAP=/opt/homebrew=$HB $R "$HB/bin/rustc" -O rust_panics.rs -o $T/rust_panics >/dev/null 2>&1
    printf 'drop inner-a\ndrop inner-b\ndrop inner-a\ndrop inner-b\ndrop inner-a\ndrop outer\nr err true\nmsg Some("boom 0")\ndrop thread\njoin err true\ndrop nested\ninner caught true\nresumed Some(7)\ntrue\ndrop end\n' > $T/a
    run_to 60 $T/b $R $T/rust_panics; grep -v '^nvrm' $T/b > $T/b2; check rust_panics $T/a $T/b2
    REBAR_PATHMAP=/opt/homebrew=$HB $R "$HB/bin/rustc" -O rust_std.rs -o $T/rust_std >/dev/null 2>&1
    printf 'sum 6000\nchan [0, 1, 4, 9, 16]\n[("a", 3), ("b", 2), ("c", 1)]\npanic caught true\ntrue\nOk("hi")\nf64 1.4142 340282366920938463463374607431768211455\n' > $T/a
    run_to 60 $T/b $R $T/rust_std; grep -v '^nvrm' $T/b > $T/b2; check rust_std $T/a $T/b2
fi
# Go: runtime panics from faults (nil dereference, nil map, index, divide by zero) are recovered: the guest's SIGSEGV handler has to run on its alternate signal stack
if [ -x "$HB/bin/go" ]; then
    gd=$T/go_panics; rm -rf $gd; mkdir -p $gd; cp go_panics.go $gd/main.go; printf 'module gopanics\ngo 1.21\n' > $gd/go.mod
    RABS=$(cd .. && pwd)/rebar; ( cd $gd && REBAR_PATHMAP=/opt/homebrew=$HB GOCACHE=$T/gocache "$RABS" "$HB/bin/go" build -o gopanics . >/dev/null 2>&1 )
    printf 'nil deref: runtime error: invalid memory address or nil pointer dereference\nnil map: assignment to entry in nil map\nindex: runtime error: index out of range [3] with length 0\ndiv: runtime error: integer divide by zero\nnil iface: runtime error: invalid memory address or nil pointer dereference\nwrite nil: runtime error: invalid memory address or nil pointer dereference\nhot nil derefs: 3000\n' > $T/a
    run_to 60 $T/b $R $gd/gopanics; grep -v '^nvrm' $T/b > $T/b2; check go_panics $T/a $T/b2
fi
hbcheck brew_python_ctypes "ctypes 5 [1, 2, 3, 4, 5] 1024.0 42 42-x" python3.13 ctypes_basic.py
hbcheck brew_python_ctypes_guest "guest 7 3 4 0 0 21.0 21.0" python3.13 ctypes_guest.py   # (calls into an arm64 library: ffi_call is made by rebar with the arm64 convention)
# libffi closures from a C program (the host libffi is x86_64: the arm64 ABI constant and the closure size are translated)
for a in arm x86; do ar=arm64; [ $a = x86 ] && ar=x86_64; [ -x ffi_closure_$a ] && [ ffi_closure_$a -nt ffi_closure.c ] || clang -arch $ar ffi_closure.c -lffi -lm -o ffi_closure_$a 2>/dev/null; done
# libxml2's SAX2 startElementNs has nine arguments: the ninth is on the arm64 callee's stack
for a in arm x86; do ar=arm64; [ $a = x86 ] && ar=x86_64; [ -x libxml2_sax_$a ] && [ libxml2_sax_$a -nt libxml2_sax.c ] || clang -arch $ar -I$(xcrun --show-sdk-path)/usr/include/libxml2 libxml2_sax.c -lxml2 -o libxml2_sax_$a 2>/dev/null; done
run_to 30 $T/a ./libxml2_sax_x86; run_to 60 $T/b $R libxml2_sax_arm; grep -v '^nvrm' $T/b > $T/b2; check libxml2_sax $T/a $T/b2
run_to 30 $T/a ./ffi_closure_x86; run_to 60 $T/b $R ffi_closure_arm; grep -v '^nvrm' $T/b > $T/b2; check ffi_closure $T/a $T/b2

# iOS apps from IPAs (tools/ipa.py): iOS-platform arm64 programs packed as .ipa; the UIKit and SwiftUI ones run with rebar-ios from a generated Mac Catalyst host bundle (needs a window server session)
iosipa() {  # name, source, expected-output-file, swiftc|clang, extra args to the compiler; the app is IosApp, family iPhone, runs 60 s at most
    nm=$1; src=$2; ex=$3; cc=$4; shift 4
    rm -rf $T/$nm && mkdir -p $T/$nm/Payload/IosApp.app
    if [ $cc = swiftc ]; then xcrun -sdk iphoneos swiftc -parse-as-library -target arm64-apple-ios17.0 -O "$@" $src -o $T/$nm/Payload/IosApp.app/IosApp 2>/dev/null
    else xcrun -sdk iphoneos clang -arch arm64 -target arm64-apple-ios15.0 -fobjc-arc -w "$@" $src -o $T/$nm/Payload/IosApp.app/IosApp 2>/dev/null; fi
    printf '<?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><dict><key>CFBundleExecutable</key><string>IosApp</string><key>CFBundleIdentifier</key><string>dev.rebar.%s</string><key>CFBundleName</key><string>%s</string><key>MinimumOSVersion</key><string>15.0</string><key>UIDeviceFamily</key><array><integer>1</integer></array></dict></plist>' $nm $nm > $T/$nm/Payload/IosApp.app/Info.plist
    (cd $T/$nm && zip -qr ../$nm.ipa Payload)
    printf "$ex" > $T/a
    run_to 60 $T/b python3 -I ../tools/ipa.py run $T/$nm.ipa x y; grep -v '^nvrm' $T/b > $T/b2; check $nm $T/a $T/b2
}
if xcrun -sdk iphoneos --show-sdk-path >/dev/null 2>&1; then
    iosipa ios_cli_ipa ios_cli.m 'json {"k":[1,2,3]}\nbundle dev.rebar.ios_cli_ipa\nhw.machine arm64\nargs 2\n' clang -framework Foundation
    if [ -x ../rebar-ios ]; then
        iosipa ios_uikit_ipa ios_ui.m 'launched idiom 0 window key 1 label UIKit under rebar\nbye\n' clang -framework UIKit -framework Foundation
        iosipa ios_uikit2_ipa ios_uikit2.m 'late: window {{0, 0}, {400, 800}} root.view {{0, 0}, {400, 800}} scene 1\nautolayout box {{20, 100}, {200, 100}}\nstack {{0, 300}, {100, 170}}\napp state 0 scene state 0\nnav depth 2 top TVC rows 5\ncell2 text row 2 table frame {{0, 0}, {400, 800}} visible 5\nimage 4x4 scale 1 pixel ff0000ff\ntext width ok 1 height ok 1\ndefaults v\nidiom 0\nalert actions 1\nanimation finished 1 alpha 0\n' clang -framework UIKit -framework Foundation -framework CoreGraphics
        iosipa ios_frameworks_ipa ios_frameworks.m 'webkit js hi2\nmetal device yes queue 1\navaudiosession category ok 1\ncoremotion accelerometer 0\ncorelocation services 1 status 0\npasteboard pb-rebar\nkeychain add 0 get 0 value secret\ncoredata fetched 1 n=42\nurlsession local file via URLSession\n' clang -framework UIKit -framework Foundation -framework WebKit -framework Metal -framework AVFoundation -framework CoreMotion -framework CoreLocation -framework CoreData -framework Security -framework UserNotifications
        iosipa ios_metal_ipa ios_metal.m 'frames 5 status 4\n' clang -framework UIKit -framework Foundation -framework Metal -framework MetalKit
        iosipa ios_swiftui_ipa ios_swiftui.swift 'appeared count 0\nbye\n' swiftc
        iosipa ios_swiftui2_ipa ios_swiftui2.swift 'done ticks 16 items 22 tab 0 level 0.9\n' swiftc
        # NavigationStack path pushes while the previous push still animates (UIKit's objc_opt_respondsToSelector(obj, BOOL) call poisoned a method cache), Layout, Grid, Canvas, TimelineView, Combine, Stepper (needs the iPad idiom: SwiftUI's default Stepper style recurses forever for the iPhone idiom, natively too)
        REBAR_IOS_FAMILY=2; export REBAR_IOS_FAMILY
        iosipa ios_swiftui3_ipa ios_swiftui3.swift 'combine [0, 1, 2, 3, 4, 5, 6] render 40x40 stored 41\nui path 0 tab 0 step 5 text hello expanded true drag 12\n' swiftc
        iosipa ios_swiftui4_ipa ios_swiftui4.swift 'notes 2 fetch 2 settings x 7 sel one\n' swiftc
        unset REBAR_IOS_FAMILY
        # a store-style app: compiled storyboard, asset catalog, UIWindowSceneDelegate from the scene manifest, resources, localization
        if xcrun -f ibtool >/dev/null 2>&1 && xcrun -f actool >/dev/null 2>&1; then
            sb=$T/ios_sb_ipa; rm -rf $sb && mkdir -p $sb/Payload/SbApp.app
            xcrun -sdk iphoneos clang -arch arm64 -target arm64-apple-ios15.0 -fobjc-arc -framework UIKit -framework Foundation -w ios_sb/main.m -o $sb/Payload/SbApp.app/SbApp 2>/dev/null
            xcrun ibtool --compile $sb/Payload/SbApp.app/Main.storyboardc ios_sb/Main.storyboard --target-device iphone --minimum-deployment-target 15.0 >/dev/null 2>&1
            xcrun actool ios_sb/Assets.xcassets --compile $sb/Payload/SbApp.app --platform iphoneos --minimum-deployment-target 15.0 --target-device iphone --output-partial-info-plist $T/ap.plist >/dev/null 2>&1
            cp ios_sb/Info.plist ios_sb/data.txt $sb/Payload/SbApp.app/; cp -R ios_sb/en.lproj $sb/Payload/SbApp.app/
            (cd $sb && zip -qr ../ios_sb.ipa Payload)
            printf "didFinishLaunching\nscene willConnect role UIWindowSceneSessionRoleApplication config Default Configuration\nstoryboard label 'Storyboard OK' image found 16x16\nmain bundle id dev.rebar.sbapp\nresource hello from a bundle resource\n\nstring Hello strings\nbye\n" > $T/a
            run_to 60 $T/b python3 -I ../tools/ipa.py run $T/ios_sb.ipa; grep -v '^nvrm' $T/b > $T/b2; check ios_storyboard_scene_ipa $T/a $T/b2
        fi
    fi
else echo "SKIP iOS IPA tests (no iphoneos SDK)"; fi

echo "passed $pass, failed $fail"
[ $fail = 0 ]
