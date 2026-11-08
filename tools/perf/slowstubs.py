#!/usr/bin/env python3
"""Summarise the log of REBAR_SLOW_STUB=<ms>: which import calls (a call from the guest into the host: a driver call, a lock, a file read...)
took longer than the threshold, how often, how long in total, and from which guest address.

    REBAR_SLOW_STUB=15 ./rebar game ... 2> /tmp/slow.log        (the lines go to stderr)
    python3 tools/perf/slowstubs.py /tmp/slow.log [from_second to_second]

A call that blocks one frame for 15 ms or more is a stutter; the name says who is to blame: waits on a fence / semaphore / lock = the GPU or another
thread (driver side), file reads = streaming, objc_msgSend [Class selector] = the Objective-C method that was slow.
"""
import re, sys, collections
path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/slow.log"
lo = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
hi = float(sys.argv[3]) if len(sys.argv) > 3 else 1e18
pat = re.compile(r"\[slow (\d+) ms at ([\d.]+)s\] (.*?) from guest (0x[0-9a-f]+) thread (0x[0-9a-f]+)")
by_name = collections.defaultdict(lambda: [0, 0, 0, collections.Counter(), set()])
times = []
for ln in open(path, errors="replace"):
    m = pat.search(ln)
    if not m:
        continue
    ms, at, what, caller, thr = int(m.group(1)), float(m.group(2)), m.group(3), m.group(4), m.group(5)
    if not (lo <= at <= hi):
        continue
    name = re.sub(r"\(0x.*$", "", what)          # import name, or "[Class selector]"
    r = by_name[name]
    r[0] += 1; r[1] += ms; r[2] = max(r[2], ms); r[3][caller] += 1; r[4].add(thr)
    times.append(at)
if not times:
    sys.exit("no slow calls in %s (threshold reached never, or the log has no [slow ...] lines)" % path)
span = max(times) - min(times) or 1.0
print("%d slow calls in %.0f s (%.1f per second), %d ms in total" % (len(times), span, len(times) / span, sum(r[1] for r in by_name.values())))
print("%6s %8s %6s  %-5s %s" % ("calls", "total ms", "max", "thrds", "import / message   (top callers)"))
for name, r in sorted(by_name.items(), key=lambda kv: -kv[1][1])[:20]:
    top = ", ".join("%s x%d" % (c, n) for c, n in r[3].most_common(2))
    print("%6d %8d %6d  %-5d %s   (%s)" % (r[0], r[1], r[2], len(r[4]), name[:70], top))
