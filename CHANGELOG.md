# Changelog

##  0.8.0

### arm64 apps start by themselves (experimental)
- **`tools/rebar-wrap`**: an Intel Mac refuses an arm64-only app before anything can be hooked, so the app's main executable becomes a universal file: the original arm64 slice plus a tiny x86_64 program that starts `rebar` on the same file. A double click, the Dock, `open`, file associations and Steam then start the app through rebar; bundle, Info.plist and pid are unchanged. `enable` / `disable` (puts the original back byte for byte) / `status` / `list` / `env` (per-app environment) / `seal` / `update`, and a LaunchAgent that wraps new arm64-only apps as they appear.
- **Rebar launcher**: a checkbox **"Start apps by themselves"** installs / removes that agent; the gear next to it opens a window with watched folders, a switch per app, per-app environment variables, the agent log and "Allow changes" for root-owned App Store apps.
- Downloaded (quarantined) apps get their bundle signed ad hoc after wrapping (otherwise Gatekeeper calls them "damaged"); removing the quarantine attribute stays your decision.
- Tried by double click: Cyberpunk 2077, ChatGPT, Gemini, MainStage, Xcode-beta.
- `WRAP=1 ./run_tests.sh` runs the whole regression suite through the wrapper (174 pass, the same 5 known failures as the direct run).

### Correctness
- **JIT**: a `ret` after `ldr x30` is no longer treated as a frame restore (the rule kept stale NZCV flags: `Optional<URL> ==` crashed after a dialog). 0.7.0 has this bug: update.
- Swift extra inhabitants: arm64 writes index `i` as `i`, the host's x86_64 runtime as `2*(i+1)`/`i<<1`; guards for the copy / destroy of such values (Xcode's SwiftUI canvas no longer crashes when a view opens), and `State` / `Binding` `wrappedValue` of an `Optional<any P>` is converted at the call. **Device Hub** (Xcode's device app) starts and shows its window (there is no iOS Simulator runtime for an Intel Mac, so no simulator).
- `xcodebuild` of a SwiftUI project builds under rebar: `vm_allocate` blocks are 16 KB aligned inside Apple's linker tools; arm64-only programs started through `sandbox-exec` / `arch` / `open` / NSWorkspace are started through rebar.
- Logic Pro reaches its project chooser (system_error, CFBundle lookups, sqlite3 / CFCalendar variadics, `swift_bridgeObject*` guard).
- Raw syscalls: `syscall(n, ...)` indirection, `access` / `stat` / `sysctl` and friends.
- A thread that suspends itself no longer holds the bridge lock (TikTok's crash reporter froze the process).
- iOS: stand-in class for a missing OpenGLES class, Swift `@objc` protocols registered with the host runtime, `+[UIAppearance appearanceWhenContainedIn:]` varargs. TikTok 47.6 now starts and reaches the network but shows no window yet; The Binding of Isaac needs OpenGL ES (not available on this Mac).

### Profilers and debugging
- `REBAR_FRAMEPROF=<prefix>`: a frame profiler (every frame, per-frame interval, drawable wait, CPU cores busy, time in imports, GPU time with `REBAR_FRAMEPROF_GPU=1`; OpenGL swaps counted too), `REBAR_TIMEPROF=<prefix>`: where the time of any program goes (per import / selector, host → guest entries, main run loop hangs). `tools/perf/gameprof.py`, `tools/perf/slowstubs.py`.
- `REBAR_NO_METAL_RT=1` (a driver that says YES to ray tracing makes Cyberpunk 2077 build all its path tracing kernels and its watchdog kills it).
- Smaller aids: `REBAR_TRACE_CTX`, `REBAR_WATCH_VAL`, `REBAR_TRACE_CASTFAIL`, `REBAR_TRACE_CASTLR`, `REBAR_TEST_MANGLED`, `REBAR_DUMP_WORDS_ON_TRAP`, `REBAR_TRACE_MACHEXC_BT`.

### Install
- `rebar-0.8.0-macos-x86_64.zip`: `rebar`, `rebar-ios`, the support libraries and `tools/` (`ipa.py`, `rebar-wrap`).
- `Rebar-launcher-0.8.0-macos-x86_64.zip`: the launcher app with rebar inside.
