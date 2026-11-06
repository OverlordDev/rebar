# Xcode 26.3 (arm64) under rebar: known issues

Run: `env -i HOME=$HOME PATH=/usr/bin:/bin MallocNanoZone=0 ./rebar "/Volumes/Новый том/Xcode.app/Contents/MacOS/Xcode" <project> -ApplePersistenceIgnoreState YES`

## Open

1. **Xcode crashes when a source file is opened in the editor** (reported by the user, 2026-11-06). Not investigated yet. Where to start: run with `REBAR_FAULT_LOG=<file>` and `REBAR_TRACE_SIGNALS=1`, open `ContentView.swift` of `~/Documents/app/cool/cool.xcodeproj`; the crash report now lists the guest frames (image + offset, resolve with `nm -n`), `REBAR_RECENT=40` shows the last imports, `REBAR_TRACE_MSG=<selector>` with `REBAR_TRACE_DESC=1` / `REBAR_TRACE_SELF=1` shows arguments. The source editor lives in SourceEditor.framework / IDESourceEditor.framework (Swift + a TextKit-like layout; text drawing goes through CoreText on the host).
2. Opening a project takes 15 s (native x86 Xcode: 8 s). What is left is spread out: GIL contention between threads, the static initializers of ~1000 images, dlopen / dlsym of host libraries, signals for block invocations, I/O on the exFAT disk.

## Fixed on 2026-11-06 (for the record)

- plug-in loading (arm64 NSBundle), `+bundleWithIdentifier:`, `+bundleForClass:` from host code, categories of Swift classes outside the class list, protocol mirroring, BOOL encoding, `__VaListBuilder`, tables that spun when full;
- Swift Strings returned as bridged NSStrings by host getters (indexing crash in SymbolCacheIndexing).
