#!/usr/bin/env python3
"""Hottest translated blocks with the guest instructions next to the host code that was generated for each:
  env REBAR_JITPROF=/tmp/perf/jp REBAR_JITDUMP=/tmp/perf/jd REBAR_JIT_CACHE=0 ./rebar prog ...   (run for more than 2 s)
  python3 tools/perf/hotblocks.py /tmp/perf/jp.<pid> /tmp/perf/jd.<pid> [N]
Needs capstone (pip3 install capstone). Guest words and host bytes come from the dump; every guest instruction's host code is shown after it (the dump records where each ends)."""
import sys, re, collections
import capstone
prof, dump = sys.argv[1], sys.argv[2]; top = int(sys.argv[3]) if len(sys.argv) > 3 else 5
samples = {}; total = 0
for l in open(prof):
    p = l.split()
    if p[0] == "samples": total = int(p[1])
    if p[0] == "block": samples[int(p[1], 16)] = int(p[2])
blocks = {}
for l in open(dump):
    m = re.match(r"(\w+) (\d+) (\d+) ((?:[0-9a-f]{8} )+)@([\d,]*) \|([0-9a-f]*)", l)
    if not m: continue
    pc = int(m.group(1), 16); n = int(m.group(2)); words = [int(w, 16) for w in m.group(4).split()]; offs = [int(x) for x in m.group(5).split(",") if x]
    blocks[pc] = (n, words, offs, bytes.fromhex(m.group(6)))
ga = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM); hx = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for pc, cnt in sorted(samples.items(), key=lambda x: -x[1])[:top]:
    if pc not in blocks: print("block %x: %d samples (no dump)" % (pc, cnt)); continue
    n, words, offs, code = blocks[pc]
    print("=== block %x  %d samples (%.1f%% of %d)  %d guest insns, %d host bytes (%.1f bytes/insn)" % (pc, cnt, 100.0 * cnt / max(total, 1), total, n, len(code), len(code) / max(n, 1)))
    ends = offs + [len(code)]; prev = 0
    for k in range(n):
        g = list(ga.disasm(words[k].to_bytes(4, "little"), pc + 4 * k)); print("  %-34s" % ("%s %s" % (g[0].mnemonic, g[0].op_str) if g else "?%08x" % words[k]))
        end = offs[k] if k < len(offs) else len(code)
        for i in hx.disasm(code[prev:end], 0): print("        %-6s %s" % (i.mnemonic, i.op_str))
        prev = end
    for i in hx.disasm(code[prev:], 0): print("      + %-6s %s" % (i.mnemonic, i.op_str))
