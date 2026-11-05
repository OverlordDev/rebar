#!/usr/bin/env python3
"""A/B stand for JIT work.  python3 tools/jitbench.py [--rebar PATH] [--rounds N] [--env K=V ...] [--save FILE] [--base FILE]
Medians (ms, lower is better) of: Godot GDScript (loop, fib, arrays/dict), CPython (fib, loop, dict), Node (json, regex, class, map, typed arrays), a rustc compile.
Needs the arm64 programs (override paths with env GODOT, PY, HB, NODE, RUSTC); missing ones are skipped.  REBAR_JIT_CACHE=0 is set unless --env REBAR_JIT_CACHE=1 is given.
--save writes the medians as JSON, --base prints the ratio against such a file."""
import argparse, json, os, re, statistics, subprocess, sys, time
D = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
A = os.path.expanduser("~/Desktop/arm64-apps")
HB = os.environ.get("HB", A + "/homebrew")
P = {"godot": os.environ.get("GODOT", A + "/new-apps/godot/Godot.app/Contents/MacOS/Godot"), "py": os.environ.get("PY", HB + "/bin/python3.13"),
     "node": os.environ.get("NODE", A + "/cli-tools/x/node/node-v26.13.0-darwin-arm64/bin/node"), "rustc": os.environ.get("RUSTC", A + "/cli-tools/x/rust/sysroot/bin/rustc")}
ap = argparse.ArgumentParser(); ap.add_argument("--rebar", default=D + "/rebar"); ap.add_argument("--rounds", type=int, default=3); ap.add_argument("--env", action="append", default=[])
ap.add_argument("--save"); ap.add_argument("--base"); ap.add_argument("--only", default="")
a = ap.parse_args()
home = "/tmp/jb_home"; os.makedirs(home, exist_ok=True)
env = {"HOME": home, "PATH": "/usr/bin:/bin", "MallocNanoZone": "0", "REBAR_ALL_ARM64": "1", "REBAR_PATHMAP": "/opt/homebrew=" + HB, "REBAR_JIT_CACHE": "0"}
for kv in a.env: k, v = kv.split("=", 1); env[k] = v
def run(args):
    t = time.time(); o = subprocess.run([a.rebar] + args, env=env, capture_output=True, text=True, timeout=600); return o.stdout + o.stderr, (time.time() - t) * 1000
res = {}
def add(k, v): res.setdefault(k, []).append(v)
want = lambda n: not a.only or n in a.only.split(",")
for _ in range(a.rounds):
    if want("godot") and os.path.exists(P["godot"]):
        o, t = run([P["godot"], "--headless", "--path", D + "/tools/bench/godot"])
        for m in re.finditer(r"^(loop|fib|array/dict)\b.* (\d+) ms", o, re.M): add("godot-" + m.group(1), int(m.group(2)))
        add("godot-total", t)
        o, t = run([P["godot"], "--headless", "--fixed-fps", "6000", "--path", D + "/tools/bench/godot_phys"])
        for m in re.finditer(r"^(physics|transform3d)\b.* (\d+) ms", o, re.M): add("godot-" + m.group(1), int(m.group(2)))
    if want("py") and os.path.exists(P["py"]):
        o, t = run([P["py"], D + "/tools/bench/py.py"]); m = re.search(r"fib ([\d.]+) loop ([\d.]+) dict ([\d.]+)", o)
        if m: add("python-fib", float(m.group(1)) * 1000); add("python-loop", float(m.group(2)) * 1000); add("python-dict", float(m.group(3)) * 1000)
        add("python-total", t)
    if want("node") and os.path.exists(P["node"]):
        o, t = run([P["node"], D + "/tools/bench/node.js"])
        for m in re.finditer(r"(\w+) (\d+)", o.strip().split("\n")[-1]): add("node-" + m.group(1), int(m.group(2)))
        add("node-total", t)
    if want("rustc") and os.path.exists(P["rustc"]):
        o, t = run([P["rustc"], "--target", "x86_64-apple-darwin", "-O", D + "/tools/bench/hello.rs", "-o", home + "/hello"]); add("rustc", t)
med = {k: statistics.median(v) for k, v in res.items()}
base = json.load(open(a.base)) if a.base else {}
for k in sorted(med):
    line = "%-16s %8.0f" % (k, med[k])
    if k in base: line += "   x%.3f" % (med[k] / base[k])
    print(line)
if a.save: json.dump(med, open(a.save, "w"), indent=1)
