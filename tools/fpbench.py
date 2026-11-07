#!/usr/bin/env python3
"""fpbench: the scalar-FP path of the JIT, measured on samples/bench/micro_arm.

  python3 tools/fpbench.py [--rounds N]

Prints the median ns/iter for:
  native x86_64 (micro_x86), rebar JIT, rebar interpreter (REBAR_JIT=0),
  rebar JIT with the old FCMP flag path (REBAR_JIT_NOFPFLAGS=1).
The test is the "fp scalar (mul/add/cmp)" line of samples/bench/micro.c: a chain of
FMADD / FCMP / FCSEL / B.NE, the same shape as a hot FP loop in a game or a numeric app.
"""
import argparse, os, re, statistics, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REBAR = os.environ.get("REBAR", os.path.join(ROOT, "rebar"))
ARM = os.path.join(ROOT, "samples", "bench", "micro_arm")
X86 = os.path.join(ROOT, "samples", "bench", "micro_x86")
LINE = re.compile(r"fp scalar.*?([0-9]+(?:\.[0-9]+)?) ns/iter")

def one(cmd, env=None):
    e = dict(os.environ)
    e.setdefault("REBAR_JIT_CACHE", "0")
    e.update(env or {})
    out = subprocess.run(cmd, env=e, capture_output=True, text=True, timeout=120)
    m = LINE.search(out.stdout)
    if not m:
        sys.exit("fpbench: no fp line in the output of %s:\n%s\n%s" % (cmd[0], out.stdout[-500:], out.stderr[-500:]))
    return float(m.group(1))

def med(cmd, env=None, rounds=5):
    return statistics.median(one(cmd, env) for _ in range(rounds))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=5)
    a = ap.parse_args()
    if not os.access(REBAR, os.X_OK): sys.exit("fpbench: no rebar at " + REBAR)
    if not os.path.exists(ARM): sys.exit("fpbench: no " + ARM)
    rows = [("rebar JIT", med([REBAR, ARM, "1"], None, a.rounds)),
            ("rebar JIT, old FCMP flags", med([REBAR, ARM, "1"], {"REBAR_JIT_NOFPFLAGS": "1"}, a.rounds)),
            ("rebar interpreter", med([REBAR, ARM, "1"], {"REBAR_JIT": "0"}, a.rounds))]
    if os.path.exists(X86): rows.insert(0, ("native x86_64", med([X86, "1"], None, a.rounds)))
    print("%-28s %10s" % ("scalar FP (ns/iter)", "median"))
    for name, v in rows: print("%-28s %10.2f" % (name, v))
    if len(rows) >= 2:
        native = rows[0][1] if rows[0][0] == "native x86_64" else None
        jit = rows[1][1] if native is not None else rows[0][1]
        if native is not None: print("\nJIT vs native: %.2fx slower" % (jit / native))

if __name__ == "__main__":
    main()
