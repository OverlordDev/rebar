#!/usr/bin/env python3
"""rebrew: install Homebrew's arm64 bottles on an Intel Mac and run them under rebar.

Homebrew itself refuses to pour arm64 bottles on x86_64, so this does the part of `brew install` that matters:
  * looks the formula and its runtime dependencies up in the official formula API (formulae.brew.sh),
  * downloads the bottle blob from ghcr.io (anonymous pull token) and checks its SHA-256 against the API,
  * unpacks it into <prefix>/Cellar, makes <prefix>/opt/<name> and links the binaries into <prefix>/bin,
  * relocates it the way brew does: @@HOMEBREW_PREFIX@@ / @@HOMEBREW_CELLAR@@ in Mach-O load commands (install_name_tool),
    in text files (plain replace), and re-signs the changed binaries ad hoc.
Programs are then started with `rebrew run <tool> [args]` (or `rebar <prefix>/bin/<tool>`).

  rebrew install jq ripgrep      rebrew run jq --version      rebrew list      rebrew env
"""
import hashlib, json, os, shutil, subprocess, sys, tarfile, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REBAR = os.environ.get("REBAR", os.path.join(os.path.dirname(HERE), "rebar"))
PREFIX = os.environ.get("REBREW_PREFIX", os.path.expanduser("~/Desktop/arm64-apps/homebrew"))
TAGS = os.environ.get("REBREW_TAGS", "arm64_sequoia,arm64_tahoe,arm64_sonoma,arm64_golden_gate").split(",")
API = "https://formulae.brew.sh/api/formula/%s.json"
MAGICS = (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca")

def curl(url, headers=None, out=None):
    cmd = ["curl", "-sSfL", "--retry", "3", "-A", "rebrew"]
    for k, v in (headers or {}).items(): cmd += ["-H", "%s: %s" % (k, v)]
    if out: cmd += ["-o", out]
    cmd.append(url)
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0: raise RuntimeError("curl %s failed: %s" % (url, r.stderr.decode()[:200]))
    return r.stdout

def formula(name):
    try:
        return json.loads(curl(API % name))
    except Exception as e:
        sys.exit("rebrew: formula %s not found (%s)" % (name, e))

def bottle_for(f):
    files = f["bottle"]["stable"]["files"]
    for t in TAGS:
        if t in files: return t, files[t]
    for t, v in files.items():
        if t.startswith("arm64_") and "linux" not in t: return t, v
    if "all" in files: return "all", files["all"]   # architecture-independent bottle (ca-certificates, scripts)
    return None, None

def token(repo):
    url = "https://ghcr.io/token?service=ghcr.io&scope=repository:%s:pull" % repo
    return json.loads(curl(url))["token"]

def download(f, tag, info, dest):
    repo = "homebrew/core/" + f["name"].replace("@", "/")   # (ghcr names openssl@3 as openssl/3)
    tok = token(repo)
    curl(info["url"], {"Authorization": "Bearer " + tok}, out=dest)
    h = hashlib.sha256()
    with open(dest, "rb") as fh:
        for b in iter(lambda: fh.read(1 << 20), b""): h.update(b)
    if h.hexdigest() != info["sha256"]:
        os.remove(dest); sys.exit("rebrew: SHA-256 mismatch for %s (got %s, expected %s)" % (f["name"], h.hexdigest(), info["sha256"]))

def is_macho(path):
    try:
        with open(path, "rb") as fh: return fh.read(4) in MAGICS
    except OSError: return False

def run(*a):
    return subprocess.run(a, capture_output=True, text=True)

def subst(s):
    return s.replace("@@HOMEBREW_PREFIX@@", PREFIX).replace("@@HOMEBREW_CELLAR@@", PREFIX + "/Cellar").replace("@@HOMEBREW_REPOSITORY@@", PREFIX + "/Homebrew").replace("@@HOMEBREW_LIBRARY@@", PREFIX + "/Library")

def relocate_macho(path):
    mode = os.stat(path).st_mode; os.chmod(path, mode | 0o200)   # (bottle files are read-only)
    out = run("otool", "-l", path).stdout.split("\n")
    changed = False; cmd = None; names = []
    for i, l in enumerate(out):
        l = l.strip()
        if l.startswith("cmd "): cmd = l.split()[1]
        elif l.startswith("name ") and cmd in ("LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB", "LC_ID_DYLIB", "LC_LOAD_UPWARD_DYLIB"): names.append((cmd, l.split(" (offset")[0][5:]))
        elif l.startswith("path ") and cmd == "LC_RPATH": names.append((cmd, l.split(" (offset")[0][5:]))
    args = []
    for c, n in names:
        if "@@HOMEBREW" not in n: continue
        new = subst(n)
        if c == "LC_ID_DYLIB": args += ["-id", new]
        elif c == "LC_RPATH": args += ["-rpath", n, new]
        else: args += ["-change", n, new]
    if args:
        r = run("install_name_tool", *args, path)
        if r.returncode != 0: print("rebrew: install_name_tool failed on %s: %s" % (path, r.stderr.strip()[:200]))
        changed = True
    if changed or True:   # (an arm64 binary needs a valid signature for a real Mac; rebar does not check it, brew always re-signs)
        run("codesign", "--force", "--sign", "-", path)
    os.chmod(path, mode)
    return changed

def relocate_tree(root):
    n = 0
    for d, _, fs in os.walk(root):
        for fn in fs:
            p = os.path.join(d, fn)
            if os.path.islink(p) or not os.path.isfile(p): continue
            if is_macho(p):
                n += relocate_macho(p)
            else:
                try:
                    with open(p, "rb") as fh: data = fh.read()
                except OSError: continue
                if data.startswith(b"#!") and b"@@HOMEBREW" in data.split(b"\n", 1)[0] and b"\0" in data[:4096]:   # a bytecode program (OCaml's ocaml, ocamlc, ...): only its #! line holds a placeholder
                    first, rest = data.split(b"\n", 1)
                    mode = os.stat(p).st_mode; os.chmod(p, mode | 0o200)
                    with open(p, "wb") as fh: fh.write(subst(first.decode("utf-8", "surrogateescape")).encode("utf-8", "surrogateescape") + b"\n" + rest)
                    os.chmod(p, mode)
                elif b"@@HOMEBREW" in data and b"\0" not in data[:4096]:
                    mode = os.stat(p).st_mode; os.chmod(p, mode | 0o200)
                    with open(p, "wb") as fh: fh.write(subst(data.decode("utf-8", "surrogateescape")).encode("utf-8", "surrogateescape"))
                    os.chmod(p, mode)
    return n

def installed(name):
    return os.path.isdir(os.path.join(PREFIX, "Cellar", name))

def install(name, seen):
    if name in seen: return
    seen.add(name)
    f = formula(name)
    for dep in f.get("dependencies", []): install(dep, seen)
    if installed(f["name"]): print("rebrew: %s already installed" % f["name"]); return
    tag, info = bottle_for(f)
    if not info: print("rebrew: no arm64 bottle for %s (build from source is not supported)" % name); return
    print("rebrew: %s %s (%s)" % (f["name"], f["versions"]["stable"], tag))
    os.makedirs(PREFIX + "/Cellar", exist_ok=True)
    with tempfile.TemporaryDirectory() as td:
        blob = os.path.join(td, "b.tar.gz"); download(f, tag, info, blob)
        with tarfile.open(blob) as t:
            top = sorted({m.name.split("/")[0] for m in t.getmembers()})
            t.extractall(PREFIX + "/Cellar", filter="tar") if sys.version_info >= (3, 12) else t.extractall(PREFIX + "/Cellar")
    kegdir = os.path.join(PREFIX, "Cellar", f["name"]); vers = sorted(os.listdir(kegdir)); keg = os.path.join(kegdir, vers[-1])
    nm = relocate_tree(keg)
    os.makedirs(PREFIX + "/opt", exist_ok=True)
    opt = os.path.join(PREFIX, "opt", f["name"])
    if os.path.lexists(opt): os.remove(opt)
    os.symlink("../Cellar/%s/%s" % (f["name"], vers[-1]), opt)
    for sub in ("bin", "sbin", "lib", "include", "share", "etc", "libexec"):
        src = os.path.join(keg, sub)
        if not os.path.isdir(src): continue
        if sub in ("bin", "sbin"):
            os.makedirs(os.path.join(PREFIX, sub), exist_ok=True)
            for e in os.listdir(src):
                dst = os.path.join(PREFIX, sub, e)
                if os.path.lexists(dst): os.remove(dst)
                os.symlink(os.path.relpath(os.path.join(src, e), os.path.join(PREFIX, sub)), dst)
        elif sub in ("lib", "include", "share", "etc"):   # (programs built for /opt/homebrew look for their files in <prefix>/lib/<name>, <prefix>/share/<name>: OCaml's stdlib, git's templates, ...); first formula wins
            os.makedirs(os.path.join(PREFIX, sub), exist_ok=True)
            for e in os.listdir(src):
                dst = os.path.join(PREFIX, sub, e)
                if os.path.lexists(dst): continue
                os.symlink(os.path.relpath(os.path.join(src, e), os.path.join(PREFIX, sub)), dst)
    print("rebrew: installed %s (%d Mach-O files relocated)" % (f["name"], nm))

def rebar_env():
    e = dict(os.environ)
    e["PATH"] = PREFIX + "/bin:" + e.get("PATH", "")
    e["HOMEBREW_PREFIX"] = PREFIX
    e["SSL_CERT_FILE"] = e.get("SSL_CERT_FILE", "/etc/ssl/cert.pem")
    e["MallocNanoZone"] = "0"
    e["REBAR_PATHMAP"] = "/opt/homebrew=" + PREFIX + (":" + e["REBAR_PATHMAP"] if e.get("REBAR_PATHMAP") else "")   # bottles are built for /opt/homebrew: git's exec path, templates, openssl's certificate dir
    return e

def main():
    a = sys.argv[1:]
    if not a or a[0] in ("-h", "--help"): print(__doc__); return
    cmd, rest = a[0], a[1:]
    if cmd == "install":
        seen = set()
        for n in rest: install(n, seen)
    elif cmd == "list":
        for n in sorted(os.listdir(PREFIX + "/Cellar")) if os.path.isdir(PREFIX + "/Cellar") else []: print(n, " ".join(sorted(os.listdir(PREFIX + "/Cellar/" + n))))
    elif cmd == "env":
        print("export PATH=%s/bin:$PATH HOMEBREW_PREFIX=%s" % (PREFIX, PREFIX))
    elif cmd == "run":
        if not rest: sys.exit("usage: rebrew run <tool> [args]")
        exe = rest[0] if "/" in rest[0] else os.path.join(PREFIX, "bin", rest[0])
        os.execve(REBAR, [REBAR, os.path.realpath(exe)] + rest[1:], rebar_env())
    else: sys.exit("unknown command " + cmd)

main()
