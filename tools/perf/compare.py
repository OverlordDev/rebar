#!/usr/bin/env python3
"""How far from native: python3 tools/perf/compare.py [--rebar PATH] [--rounds N] [--env K=V ...] [--only a,b] [--save F] [--base F]
Runs tools/perf/kernels.c natively (x86_64) and under rebar (arm64), median of N rounds, prints ms, the ratio to native and a check of the results.
Build the two binaries first: tools/perf/build_kernels.sh.  --save keeps the rebar medians, --base prints the ratio against such a file (A/B of a JIT change)."""
import argparse, json, os, statistics, subprocess, sys
D = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ap = argparse.ArgumentParser(); ap.add_argument("--rebar", default=D + "/rebar"); ap.add_argument("--rounds", type=int, default=3); ap.add_argument("--env", action="append", default=[])
ap.add_argument("--only", default=""); ap.add_argument("--save"); ap.add_argument("--base"); ap.add_argument("--dir", default="/tmp/perf"); ap.add_argument("--native-only", action="store_true")
a = ap.parse_args()
env = {"HOME": "/tmp/perf_home", "PATH": "/usr/bin:/bin", "MallocNanoZone": "0", "REBAR_JIT_CACHE": "0"}
for kv in a.env: k, v = kv.split("=", 1); env[k] = v
os.makedirs(env["HOME"], exist_ok=True)
names = subprocess.run([a.dir + "/kernels_x86", "list"], capture_output=True, text=True).stdout.split()
if a.only: names = [n for n in names if n in a.only.split(",")]
def one(cmd, name):
    o = subprocess.run(cmd + [name, "1"], env=env if cmd[0] == a.rebar else None, capture_output=True, text=True, timeout=900).stdout.split()
    return (float(o[1]), o[2]) if len(o) >= 3 and o[0] == name else (float("nan"), "?")
nat, reb, bad = {}, {}, []
for n in names:
    nv, rv = [], []
    for _ in range(a.rounds):
        t, cn = one([a.dir + "/kernels_x86"], n); nv.append(t)
        if not a.native_only:
            t2, cr = one([a.rebar, a.dir + "/kernels_arm"], n); rv.append(t2)
            if cn != cr: bad.append("%s: native %s rebar %s" % (n, cn, cr))
    nat[n] = statistics.median(nv); reb[n] = statistics.median(rv) if rv else float("nan")
base = json.load(open(a.base)) if a.base else {}
print("%-10s %9s %9s %7s" % ("kernel", "native ms", "rebar ms", "x native") + ("   vs base" if base else ""))
logs = []
for n in names:
    r = reb[n] / nat[n]; logs.append(r)
    line = "%-10s %9.1f %9.1f %7.2f" % (n, nat[n], reb[n], r)
    if n in base: line += "   x%.3f" % (reb[n] / base[n])
    print(line)
if logs and not a.native_only:
    import math; print("%-10s %9s %9s %7.2f   (geometric mean)" % ("all", "", "", math.exp(sum(math.log(x) for x in logs if x == x) / len(logs))))
for b in bad: print("RESULT MISMATCH", b)
if a.save: json.dump(reb, open(a.save, "w"), indent=1)
