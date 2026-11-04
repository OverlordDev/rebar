#!/usr/bin/env python3
"""ipa: look at and run iOS apps (.ipa or an unpacked .app) under rebar.

  ipa.py info <app.ipa|App.app>           bundle id, executable, architectures, minimum iOS, device family, frameworks it links, encryption
  ipa.py unpack <app.ipa> [dir]           Payload/*.app out of the zip (default: ~/Library/Caches/rebar-ipa/<hash>)
  ipa.py run <app.ipa|App.app> [args...]  unpack if needed and start the executable under rebar (rebar-ios when it needs UIKit)

What runs: iOS arm64 command-line style code (Foundation, libSystem, Network, CoreData, ... the frameworks macOS shares with iOS) runs as is. Anything that links UIKit needs
the Mac Catalyst copy of UIKit, which only loads into a process built for the macCatalyst platform and living in an .app: that is `rebar-ios` (build.sh makes it from rebar with vtool)
inside a generated host bundle (cache dir, see host_bundle).
Apps from the App Store are FairPlay-encrypted (LC_ENCRYPTION_INFO cryptid=1): the code is not readable and nothing here decrypts it; use an IPA you built or own the decrypted binary of.
"""
import hashlib, os, plistlib, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
REBAR = os.environ.get("REBAR", os.path.join(ROOT, "rebar"))
REBAR_IOS = os.environ.get("REBAR_IOS", os.path.join(ROOT, "rebar-ios"))
CACHE = os.path.expanduser("~/Library/Caches/rebar-ipa")
UI_FRAMEWORKS = ("UIKit", "UIKitCore", "SwiftUI", "AppKit")

def sh(*a):
    return subprocess.run(a, capture_output=True, text=True).stdout

def sha(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""): h.update(b)
    return h.hexdigest()[:16]

def unpack(ipa, dest=None):
    dest = dest or os.path.join(CACHE, sha(ipa))
    marker = os.path.join(dest, ".done")
    if not os.path.exists(marker):
        os.makedirs(dest, exist_ok=True)
        r = subprocess.run(["unzip", "-q", "-o", ipa, "-d", dest], capture_output=True, text=True)   # (unzip keeps symlinks and permissions)
        if r.returncode not in (0, 1): sys.exit("ipa: unzip failed: " + r.stderr.strip()[:200])
        open(marker, "w").close()
    return dest

def find_app(path):
    if path.endswith(".ipa"):
        root = unpack(path)
        pl = os.path.join(root, "Payload")
        apps = [d for d in os.listdir(pl) if d.endswith(".app")] if os.path.isdir(pl) else []
        if not apps: sys.exit("ipa: no Payload/*.app in " + path)
        return os.path.join(pl, apps[0])
    if path.endswith(".app") and os.path.isdir(path): return os.path.abspath(path)
    if os.path.isdir(os.path.join(path, "Payload")):
        apps = [d for d in os.listdir(os.path.join(path, "Payload")) if d.endswith(".app")]
        if apps: return os.path.join(path, "Payload", apps[0])
    sys.exit("ipa: not an .ipa or .app: " + path)

def info(app):
    with open(os.path.join(app, "Info.plist"), "rb") as f: p = plistlib.load(f)
    exe = os.path.join(app, p.get("CFBundleExecutable", ""))
    d = {"app": app, "bundle_id": p.get("CFBundleIdentifier"), "name": p.get("CFBundleDisplayName") or p.get("CFBundleName"), "version": p.get("CFBundleShortVersionString"),
         "executable": exe, "min_ios": p.get("MinimumOSVersion"), "device_family": p.get("UIDeviceFamily"), "capabilities": p.get("UIRequiredDeviceCapabilities")}
    archs = sh("lipo", "-archs", exe).split()
    d["archs"] = archs
    build = sh("vtool", "-show-build", exe)
    d["platform"] = next((l.split()[1] for l in build.split("\n") if l.strip().startswith("platform")), "?")
    linked = [l.strip().split(" (")[0] for l in sh("otool", "-arch", "arm64", "-L", exe).split("\n")[1:] if l.strip()]
    d["links"] = linked
    d["uses_ui"] = any(("/%s.framework/" % f) in l for l in linked for f in UI_FRAMEWORKS)
    enc = sh("otool", "-arch", "arm64", "-l", exe)
    d["encrypted"] = "cryptid 1" in enc
    fw = os.path.join(app, "Frameworks")
    d["embedded"] = sorted(os.listdir(fw)) if os.path.isdir(fw) else []
    return d

def show(d):
    print("%s  (%s)" % (d["name"], d["bundle_id"]))
    print("  executable   %s" % os.path.relpath(d["executable"], d["app"]))
    print("  arch         %s   platform %s   min iOS %s   device family %s" % (",".join(d["archs"]), d["platform"], d["min_ios"], d["device_family"]))
    print("  encrypted    %s" % ("YES (FairPlay: cannot run)" if d["encrypted"] else "no"))
    print("  UI           %s" % ("UIKit/SwiftUI: needs rebar-ios (Mac Catalyst UIKit)" if d["uses_ui"] else "no UI framework: command-line style, runs with rebar"))
    sysfw = [l for l in d["links"] if l.startswith("/System/Library/Frameworks/")]
    print("  frameworks   %s" % ", ".join(sorted({l.split("/")[4].replace(".framework", "") for l in sysfw})))
    if d["embedded"]: print("  embedded     %s" % ", ".join(d["embedded"]))

LSREGISTER = "/System/Library/Frameworks/CoreServices.framework/Versions/A/Frameworks/LaunchServices.framework/Versions/A/Support/lsregister"

def host_bundle(app, d):
    """UIKit (Mac Catalyst) only runs a process that lives in an .app: its scene server looks the client up by bundle. So the iOS app is run from a small host bundle
    <cache>/host/<Name>.app: Contents/MacOS/<Name> is rebar-ios, Contents/Resources is the iOS .app folder (nibs, assets, Info.plist), Contents/Info.plist is the app's with the
    executable renamed. rebar-ios then loads the arm64 iOS executable from the original folder."""
    name = os.path.basename(d["executable"])
    host = os.path.join(os.path.dirname(os.path.dirname(app)), "host", name + ".app")
    exe = os.path.join(host, "Contents", "MacOS", name)
    stamp = os.path.join(host, ".stamp")
    want = "%s %s %s" % (os.path.getmtime(REBAR_IOS), os.path.realpath(app), os.environ.get("REBAR_IOS_FAMILY", ""))
    if not (os.path.exists(stamp) and open(stamp).read() == want):
        shutil.rmtree(host, ignore_errors=True)
        os.makedirs(os.path.dirname(exe))
        shutil.copy2(REBAR_IOS, exe)
        os.symlink(app, os.path.join(host, "Contents", "Resources"))
        with open(os.path.join(app, "Info.plist"), "rb") as f: p = plistlib.load(f)
        p["CFBundleExecutable"] = name
        p.pop("NSPrincipalClass", None)
        p["CFBundlePackageType"] = "APPL"
        p["LSMinimumSystemVersion"] = "12.0"
        if os.environ.get("REBAR_IOS_FAMILY"): p["UIDeviceFamily"] = [int(x) for x in os.environ["REBAR_IOS_FAMILY"].split(",")]
        p.setdefault("CFBundleVersion", "1")
        with open(os.path.join(host, "Contents", "Info.plist"), "wb") as f: plistlib.dump(p, f)
        subprocess.run(["codesign", "--force", "--sign", "-", host], capture_output=True)
        subprocess.run([LSREGISTER, "-f", host], capture_output=True)
        with open(stamp, "w") as f: f.write(want)
    return exe

def run(path, args):
    app = find_app(path); d = info(app)
    if "arm64" not in d["archs"] and "arm64e" not in d["archs"]: sys.exit("ipa: no arm64 slice (%s)" % ",".join(d["archs"]))
    if d["encrypted"]: sys.exit("ipa: the binary is FairPlay-encrypted (cryptid 1); it has to be decrypted first")
    exe = d["executable"]
    env = dict(os.environ); env["MallocNanoZone"] = "0"
    if d["uses_ui"]:
        if not os.path.exists(REBAR_IOS): sys.exit("ipa: %s missing (run build.sh; it needs vtool from Xcode)" % REBAR_IOS)
        host = host_bundle(app, d)
        env["REBAR_IOS"] = "1"; env["REBAR_IOS_UI"] = "1"; env["CFProcessPath"] = host; env["REBAR_CFPROCESSPATH"] = host; env["REBAR_LIB_DIR"] = os.path.dirname(REBAR_IOS)
        os.execve(host, [host, exe] + args, env)
    env["CFProcessPath"] = exe; env["REBAR_IOS"] = "1"
    os.execve(REBAR, [REBAR, exe] + args, env)

def main():
    a = sys.argv[1:]
    if len(a) < 2: print(__doc__); return
    cmd, target = a[0], a[1]
    if cmd == "info": show(info(find_app(target)))
    elif cmd == "unpack": print(find_app(target) if not target.endswith(".ipa") else unpack(target, a[2] if len(a) > 2 else None))
    elif cmd == "run": run(target, a[2:])
    else: sys.exit("unknown command " + cmd)

main()
