# rebar JIT: where the time goes and how far we are from native (2026-11-07 night)

Host: macOS 26.7.2, AMD Ryzen 7 5800H. Everything below is measured on this machine; A/B pairs are medians of 3 rounds run back to back
(spread between sessions is ±3 %, the machine was not always idle: absolute numbers drift, ratios hold).

## How to measure (all in the repo)

| What | Command |
|---|---|
| Ratio to native on 14 compute kernels (results of both builds are compared) | `tools/perf/build_kernels.sh && python3 tools/perf/compare.py --rounds 3 [--env K=V] [--only a,b] [--save f.json] [--base f.json]` |
| Real programs (Godot GDScript + physics, CPython, Node, rustc) | `python3 tools/jitbench.py --rounds 3 [--env K=V] [--save f.json] [--base f.json]` |
| Micro benchmarks (call, indirect call, alu, loads, FP, NEON, memcpy, malloc) | `REBAR_JIT_CACHE=0 ./rebar samples/bench/micro_arm 1` (native: `samples/bench/micro_x86 1`) |
| The hottest translated blocks with the guest code and the host code generated for each | `REBAR_JITPROF=/tmp/jp REBAR_JITDUMP=/tmp/jd ./rebar prog ...` (run longer than 2 s), then `python3 tools/perf/hotblocks.py /tmp/jp.<pid> /tmp/jd.<pid> 5` (needs `pip3 install capstone`, run with `python3`, not `-I`) |
| Whole-process host profile | `sample <pid> 3 1 -file out.txt`, `REBAR_PROFILE`, `REBAR_JITPROF` (host side of the same file) |
| Register cache pressure at translation time | `REBAR_JITSTATS_RC=1` (fills / evictions / dirty write-backs when the process exits) |
| JIT correctness | `./rebar --jit-selftest 100000 <seed>` (only `died: interpreter 0, jit 1` is allowed), `... 40000 5 - tails`, `REBAR_JIT_FLAGPOISON=1` on real programs (a wrong "flags are dead" answer traps), `sh run_tests.sh` (never two at once: they share /tmp/rebar_tests) |

The host changed to macOS 26.7.2 in the middle of the project: `--jit-selftest` used to die in its own set-up (the page at 0x1000 is rebar's
own RX `__TEXT` now); fixed, 19 of the 24 tests that failed there came back. `run_tests.sh`: **170 pass, 5 fail** (`t11_gui`, `t15_nib`,
`rebar_notes_app`, `fw_appkit`: 4-pixel layout differences of this OS; `jit_cache`: the 2000-block snapshot threshold), the same five as in the lab notes.

## Where we stand: x native (rebar JIT / native x86_64, same C source, `-O2 -ffp-contract=off`)

| kernel | what it stresses | before | after round 1 |
|---|---|---:|---:|
| bytes (CRC table) | byte loads, table lookups | 1.02 | 1.02 |
| chase | memory latency | 1.03 | 1.03 |
| virt | indirect calls | 1.02 | 0.94-1.01 |
| fannkuch | integer + small arrays | 1.41 | 1.42 |
| hash | open-addressing table | 1.77 | 1.75 |
| spectral | FP divide, scalar | 2.69 | 2.67 |
| strings | strlen / memcpy / byte loop | 3.07 | 3.07 |
| fib | call / return, prologue / epilogue | 3.74 | 3.55 |
| sha256 | 15 live integer registers, rotates | 3.80 | 3.88 |
| mandel | scalar FP chain, compare | 3.99 | 3.99 |
| nbody | scalar FP | 4.47 | 4.29 |
| matmul | NEON (auto-vectorised) | 6.20 | 6.20 |
| saxpy | NEON | 6.41 | 6.14 |
| qsort | callbacks from libc into the guest | 14.15 | 8.84 |
| **geometric mean** | | **2.92** | **2.88** |

Micro benchmarks (ns/iteration, native / before / after): scalar FP 1.7 / 9.2 / **7.2** (the lab's "FP flags stay in the x86 flags" change is in now),
NEON 0.9 / 3.5 / 3.5, call+ret 1.3 / 2.0 / 2.0, array loads 1.2-1.4 / 1.8 / 1.7, alu chain 1.5 / 1.5 / 1.5, malloc+free 34 / 35 / 35.

Real programs (ms, lower is better; before -> after round 1): node-json 611 -> 586, node-map 447 -> 430, node-typed 676 -> 635 (node total x0.96), godot loop/fib/array x0.96-0.98,
python total x0.96, rustc x1.00.

## What was changed (commit c94a91f)

1. **FP compare flags stay in x86 flags** (lab patch `LAB_CHANGES.patch`, applied): `fcmp` + `b.cond` / `fcsel` no longer write and re-read four flag bytes.
2. **`canon_nan` keeps the register caches.** The NaN fix-up after every scalar FP operation used a forward branch helper that flushed and cleared the integer register cache and
   switched the vector cache off for the following stores; now a plain jump over two instructions that touch only rax / xmm0.
3. **Flag liveness knows the calling convention.** `fl_dead_from` treated every `ret` / `br` / `blr` as a reader of NZCV (outlined functions can return flags: `cmp x10, #0x3f; ret`),
   so every `cmp` in a function body kept all four flags stored for the epilogue's `ret`. Now a return after the frame was restored (a load into x30) and an indirect call are dead ends;
   the outlined leaf (no frame) stays live. `REBAR_JIT_NOFLRETDEAD=1` turns it off; `REBAR_JIT_FLAGPOISON=1` runs of Node, CPython, Godot and rustc trap nowhere. (The selftest switches
   it off itself: it compares the flags at the end of random sequences, where this is not a valid assumption.)
4. **`call_guest_short`** for the comparison function of `qsort` / `qsort_r` / `bsearch`: no interpreter-lock release, saves only what the guest still needs (x19..x30, sp, pc,
   counters, the return-address-stack bounds, d8..d15, flags) instead of copying 850 bytes twice. 14.2 x -> 8.8 x native. (`REBAR_SLOW_CALLBACK=1` = old path.)

## Where the time goes (translated code, from `hotblocks.py` of the kernels and of Subway Surfers)

* **Memory traffic of the guest register file.** The CPU structure is the home of every guest register; the register cache has 8 host registers (3 of them pinned: x8, x9, x0).
  sha256's round loop (15 live values): 65 % of the register-cache fills evict a live register; Node: 22 % of the instructions evict, 12 % write a dirty register back.
  A function prologue (`stp x26,x25,[sp,#-0x50]!` x5) is 4 host instructions per `stp`, an epilogue the same plus the write-backs of evicted registers: 40-50 host instructions for 10 guest ones.
  `REBAR_JIT_PIN` shows the trade: `0,8,9,19,20` makes fib 3.6 -> 2.5 x but sha256 worse; `none` makes sha256 3.9 -> 2.6 x and fib 4.3 x. One static set cannot fit both.
* **Scalar FP / NEON: 5-14 host instructions per guest operation.** Load operands (a 16-byte value is read as two 8-byte halves to avoid store-forwarding stalls), the operation, the NaN fix-up
  (`ucomisd/jnp` scalar; `movaps + cmpunordps + movmskps + test + jne` vector), a 16-byte write-through store, a copy into the xmm cache. mandel's loop is 15 guest instructions -> 110 host ones.
  The NaN fix-up is there for the sign of the default NaN (arm64 gives +NaN, x86 -NaN: `printf` shows "nan" vs "-nan").
* **Flags** are cheap where the consumer follows the `cmp`; a `mov` in between (`cmp; mov; b.hs`) makes the JIT store four flag bytes and read one back (fib).
* **Calls from the host into the guest** (qsort comparators, ObjC methods, blocks): a signal (10-20 us) or `call_guest` (~75 ns before, ~45 ns now: setjmp, thread-local lookups, jit_try).
* **Subway Surfers, 40 s profile:** 44 % of the samples are in translated code, 5 % in the enter/exit thunks and indirect-branch stubs, 11 % `swtch_pri` (Unity's own worker threads spinning),
  2 % signal delivery for host -> guest entries, ~2 % each for `jit_try`, `_tlv_get_addr`, `memmove`; the translated part is flat (the hottest block is 1.9 % of all samples; the top block is a
  function prologue, the second its epilogue).

## What to do next, by expected value

| # | Idea | Expected | Risk |
|---|---|---|---|
| 1 | **Move-aliasing in the register cache** (`mov x19, x5` makes x19 a second name of x5's host register until one of them is written): about 8 % of the host instructions in hot code are register copies, and each one also allocates / evicts a slot | 3-6 % integer code | medium (the cache's invariant is one guest register per slot; side-exit records) |
| 2 | **More cache registers** (r8, r9, rsi are free of the cache but used as scratch by the indirect-branch stubs, the FP flag code, LDSET templates, call arguments): 8 -> 11 slots | sha256-like code 15-25 %, Node ~2-3 % | medium (audit ~60 uses; rc_dirty is a byte) |
| 3 | **Per-process pinned set chosen from the first seconds' register usage** (dynamic counts of x0-x30/sp per translated block) instead of the fixed 8, 9, 0 | fib-like calls 20-30 %, others +-0 | low-medium |
| 4 | **Vector cache with write-back** for runs of cache-aware FP / NEON templates (flush before any other instruction touches V memory, at side exits and block end) | scalar FP / NEON loops 15-25 % | high (hundreds of direct `GVR` accesses; the selftest is a good net) |
| 5 | **Batch NaN checks** over a straight run of vector ops (OR of the unordered masks, one branch to a safe re-execution path) | NEON 20-30 % | high |
| 6 | `cmp; <mov / movk / adr>; b.cond` fusion (flag-neutral instructions between the compare and the branch) | fib-like 5 % | low-medium (needs a verified whitelist; memory instructions must stay out: a fault needs the flags in memory) |
| 7 | Guest-resident `qsort` / `bsearch` (arm64 machine code in the guest address space; the comparator call becomes a guest -> guest call) | qsort 8.8 x -> ~2 x | medium (must keep Apple's tie order: port FreeBSD's algorithm exactly) |
| 8 | Cheaper host -> guest callbacks in general (`call_guest`: thread-local lookups once per call, no `setjmp` when nothing can fault, direct entry of the JIT thunk) | ObjC blocks, dispatch | medium |

Already tried by earlier sessions and not worth repeating (see PLAN_SPEEDUP.md, the lab notes): block-length changes, following `b` / `bl` inside blocks, register liveness at block exits,
trace unrolling without hot-loop selection, loop warm entries, 2 MB pages, V8 flags.
