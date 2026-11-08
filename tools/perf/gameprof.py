#!/usr/bin/env python3
"""Where does a running program spend its time: translated guest code, rebar itself, or the host (driver, libc)?

Start the program with REBAR_JITPROF=<prefix> (rebar then samples every running thread every 4 ms and rewrites <prefix>.<pid> every 2 s),
then, while the scene you care about is on screen:

    python3 tools/perf/gameprof.py /tmp/cpjp [seconds=20] [pid]

The two snapshots <seconds> apart are subtracted, so only that window is shown.  Threads that wait (a GPU fence, a lock, sleep) are not
sampled at all: "running threads" is the average number of threads that were really executing.  Reading the result:
  * translated guest code is the game's own arm64 code running through the JIT: rebar's side (ratio to native: tools/perf/compare.py)
  * rebar bridge = import stubs, host calls, signal entries, the interpreter: rebar's side too
  * driver / Vulkan / Metal = the GPU driver's CPU work (shader compiler, command building): the driver's side
  * libsystem = memcpy, malloc, locks: either
If the render thread is mostly waiting (few running threads, small share everywhere) the frame is GPU-bound: look at the Metal HUD's GPU time.
"""
import glob, os, re, sys, time, collections

prefix = sys.argv[1] if len(sys.argv) > 1 else "/tmp/jitprof"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
pid = sys.argv[3] if len(sys.argv) > 3 else None


def newest():
    c = glob.glob(prefix + ".*")
    c = [p for p in c if re.search(r"\.\d+$", p) and (pid is None or p.endswith("." + pid))]
    return max(c, key=os.path.getmtime) if c else None


def read(path):
    total = jit = 0
    blk = collections.Counter()
    host = {}
    for ln in open(path, errors="replace"):
        p = ln.split()
        if not p:
            continue
        if p[0] == "samples":
            total, jit = int(p[1]), int(p[5])
        elif p[0] == "block":
            blk[int(p[1], 16)] = (int(p[2]), p[3] if len(p) > 3 else "")
        elif p[0] == "host":
            host[int(p[1], 16)] = (int(p[2]), p[3] if len(p) > 3 else "?")
    return total, jit, blk, host


def group(name):
    n = name
    if n == "?" or n.startswith("0x"):
        return "unknown host code (JIT'd or stripped)"
    if re.match(r"^(swtch_pri|cthread_yield|sched_yield|__ulock_wait|__psynch|semaphore_wait|mach_msg|__semwait|usleep|nanosleep|__workq|kevent|__select|poll|_dispatch_(worker|workloop|root_queue))", n):
        return "idle: spin / yield / wait (a thread with nothing to do)"
    if re.match(r"^(run_until|stub_call|host_call|host_invoke|rebar_|h_|jit_|call_guest|exec_|on_segv|interp|step|fetch|gil_|simd_|msgsend_fast|urg_|_sigtramp|longcopy|fp_|vec_|neon_|cpu_)", n):
        return "rebar: bridge, interpreter helpers (simd_* = NEON the JIT did not translate)"
    if "nak" in n or n.startswith("_RN") or n.startswith("_RIN") or "nvk_" in n or "vk_" in n or "spirv" in n or "nir_" in n or "mesa" in n or "compiler" in n:
        return "driver: Vulkan / shader compiler (CPU side)"
    if "nvmtl" in n.lower() or "NVMTL" in n or "MTL" in n:
        return "driver: Metal layer (CPU side)"
    if re.match(r"^(_?(platform|malloc|free|bzero|memcpy|memmove|memset|os_|pthread|_pthread|_os|tiny_|small_|szone|rack_|nano_))", n) or "malloc" in n:
        return "libsystem (memcpy / malloc / locks)"
    if re.match(r"^(objc_|_objc|CF|NS|_CF|_NS|weak_|_tlv|icu|_ZN3icu|_ZN4objc)", n):
        return "host frameworks (Objective-C runtime, CoreFoundation, ICU)"
    return "other host: " + n[:40]


a = newest()
if not a:
    sys.exit("no profile file %s.<pid> yet: start the program with REBAR_JITPROF=%s and wait 2 s" % (prefix, prefix))
t0, j0, b0, h0 = read(a)
time.sleep(secs)
a = newest()
t1, j1, b1, h1 = read(a)
dt, dj = t1 - t0, j1 - j0
if dt <= 0:
    sys.exit("no new samples in %.0f s: is the program running?" % secs)
period_ms = 4.0
print("window %.0f s, %d samples = %.1f running threads on average" % (secs, dt, dt * period_ms / 1000.0 / secs))
cats = collections.Counter()
cats["translated guest code"] = dj
for k, (n, fn) in h1.items():
    d = n - h0.get(k, (0, ""))[0]
    if d > 0:
        cats[group(fn)] += d
rest = dt - dj - sum(v for c, v in cats.items() if c != "translated guest code")
if rest > 0:
    cats["host code outside the top 400 addresses"] += rest
for c, v in cats.most_common():
    print("  %5.1f %%  %s" % (100.0 * v / dt, c))
print("\nhottest host functions")
agg = collections.Counter()
for k, (n, fn) in h1.items():
    d = n - h0.get(k, (0, ""))[0]
    if d > 0:
        agg[fn] += d
for fn, v in agg.most_common(12):
    print("  %5.1f %%  %s" % (100.0 * v / dt, fn[:90]))
print("\nhottest translated guest blocks (guest address, offset into the main binary at 0x100000000)")
rows = []
for k, (n, ex) in b1.items():
    d = n - b0.get(k, (0, ""))[0]
    if d > 0:
        rows.append((d, k, ex))
for d, k, ex in sorted(rows, reverse=True)[:12]:
    off = ("+0x%x" % (k - 0x100000000)) if 0x100000000 <= k < 0x110000000 else ""
    print("  %5.1f %%  %#x %s %s" % (100.0 * d / dt, k, off, ex))
