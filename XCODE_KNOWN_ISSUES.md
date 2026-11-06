# Xcode 26.3 (arm64) under rebar: known issues

Run: `env -i HOME=$HOME PATH=/usr/bin:/bin MallocNanoZone=0 ./rebar "/Volumes/Новый том/Xcode.app/Contents/MacOS/Xcode" <project> -ApplePersistenceIgnoreState YES`

## Open

1. **The editor stays on "No Selection" after a source file is clicked in the navigator** (2026-11-06; the file is selected, the editor pane shows nothing, Xcode burns 99 % CPU on a background thread and its window stops answering accessibility queries). The *crash* that used to follow (~40 s after the click) is fixed, see below. Next step: sample the busy thread (`sample <pid> 3`) and see what the SourceEditor / IDESourceKit loading waits for (the log has `IDESourceKit warning: Response error ... 'CloseIndex': Connection invalid` and `ERROR SETTING UP TOOLCHAINS`: the SourceKit XPC service of the toolchain is an arm64 process that a host process cannot start, `sourcekitd` would have to be started through rebar as `posix_spawn` does for other arm64 helpers).
2. Opening a project takes 15 s (native x86 Xcode: 8 s). What is left is spread out: GIL contention between threads, the static initializers of ~1000 images, dlopen / dlsym of host libraries, signals for block invocations, I/O on the exFAT disk.

## Fixed on 2026-11-06 (for the record)

- **The crash after a file was clicked**: a Swift function that returns `Bool` leaves the rest of rax undefined on the x86_64 host, and arm64 callers test the whole w0 (`bl swift_isUniquelyReferenced_nonNull; mov x8, x0; cbz w8, ...`): a shared array buffer looked uniquely referenced and was mutated in place (`_SliceBuffer.replaceSubrange` moved the tail of a shared buffer with a negative length). Bool results of the runtime's functions and of mangled Swift functions whose demangled result type is `Swift.Bool` are now zero-extended (`swift_returns_bool` in bridge.h, `REBAR_NO_SWIFT_BOOL=1` turns it off). Found with `REBAR_WATCH=<pc>` (interpreter: `REBAR_JIT=0 REBAR_JIT_CACHE=0`) and by reading the guest code around the call.

- **The crash when a source file is opened from the command line** (`Xcode <project> <file>`): Combine's `CompactMap` handed IDEDelta's `workspaceHolder` closure an `Any?` whose type word was a small number (a "no payload" encoding of a nested Optional; the other words were stack leftovers). The closure tests only for 0, took it for a value and cast it; the host's `swift_dynamicCast` read a vtable at `type-8` and died. `swift_dynamicCast` of an `Any` with a type word below 0x1000 now fails the cast without touching the source (`h_swift_dynamicCast` in bridge.h; `REBAR_NO_CAST_GUARD=1` turns it off, `REBAR_TRACE_CAST=1` reports every cast of an `Any`). The real cause (why the host has such a value there, probably the extra-inhabitant numbering of a nested Optional that differs between the arm64 guest and the x86_64 host) is not found. Not seen on screen yet: Xcode ran 2 min without a fault, the editor itself was not checked because the screen was locked.

- plug-in loading (arm64 NSBundle), `+bundleWithIdentifier:`, `+bundleForClass:` from host code, categories of Swift classes outside the class list, protocol mirroring, BOOL encoding, `__VaListBuilder`, tables that spun when full;
- Swift Strings returned as bridged NSStrings by host getters (indexing crash in SymbolCacheIndexing).
