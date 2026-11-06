# Xcode 26.3 (arm64) under rebar: known issues

Run: `env -i HOME=$HOME PATH=/usr/bin:/bin MallocNanoZone=0 ./rebar "/Volumes/Новый том/Xcode.app/Contents/MacOS/Xcode" <project> -ApplePersistenceIgnoreState YES`

## Open

1. **Xcode crashes ~40 s after a source file is clicked in the navigator** (reported by the user, reproduced 2026-11-06 with a real mouse click on `ContentView.swift` of `~/Documents/app/cool/cool.xcodeproj`; an AX press does not open the editor, use computer-use `left_click`). Diagnosis so far:
   - the fault is in `SymbolCacheIndexing`: `Swift._ArrayBufferProtocol.replaceSubrange(_:with:elementsOf:)` specialised for `_ContiguousArrayBuffer<SMSourceModelItem>` with `EmptyCollection` (frames `SymbolCacheIndexing+0x159a48 <- 0xa19b8 <- 0x9fe6c ...`);
   - the call that dies is the `memmove` that shifts the tail of the array after the removed range (guest `bl memmove` at +0xbc of that function): its length is **negative** (`(count - upperBound) * 8` = -0x600020), i.e. the subrange handed to `replaceSubrange` ends 0xc0004 elements past the end of the array. Swift's own bounds check is in the caller, so the range is computed wrongly further up;
   - just before it the guest asked `-[... lineRangeForCharacterRange:]`, `-[NSString substringWithRange:]` and bridged an NSString with `String._unconditionallyBridgeFromObjectiveC`: the suspicion is an NSRange / line range that reaches the code in the wrong form (a by-value struct through a host or guest ObjC method, or a Swift String/Substring layout mismatch like the bridged-NSString one fixed the same day).
   - not the cause: `swift_arrayDestroy` (the host's loop is fine; the ring only shows it as the last *recorded* import because memmove is not logged), the hot-IMP thunks, the lock-free send cache.
   - next step: log the arguments of `lineRangeForCharacterRange:` and its result (`REBAR_TRACE_MSG=lineRangeForCharacterRange,substringWithRange` + `REBAR_TRACE_SELF=1`), compare with what the caller does with the returned NSRange (struct return in x0:x1), and check whether the guest method returning the range is entered with the struct encoding (`imp_register`) it needs.
2. Opening a project takes 15 s (native x86 Xcode: 8 s). What is left is spread out: GIL contention between threads, the static initializers of ~1000 images, dlopen / dlsym of host libraries, signals for block invocations, I/O on the exFAT disk.

## Fixed on 2026-11-06 (for the record)

- plug-in loading (arm64 NSBundle), `+bundleWithIdentifier:`, `+bundleForClass:` from host code, categories of Swift classes outside the class list, protocol mirroring, BOOL encoding, `__VaListBuilder`, tables that spun when full;
- Swift Strings returned as bridged NSStrings by host getters (indexing crash in SymbolCacheIndexing).
