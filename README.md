# rebar — it's revolution, Johnny: arm64 to x86_64

Runs arm64 macOS programs on Intel/AMD (x86_64) Macs.

rebar is an ARM64 interpreter with an x86-64 JIT, a Mach-O loader that does the job of dyld, and a bridge to the host's own system libraries.

## Build

Needs macOS on x86_64 and the Xcode command line tools.

```sh
./build.sh
```

This produces `./rebar`.

## Run

```sh
./rebar path/to/arm64/binary [args...]
```

### Apps that start by themselves (experimental)

An Intel Mac refuses an arm64-only app ("Bad CPU type") before anything can be hooked, so `tools/rebar-wrap` changes the app's main executable instead: it becomes a universal
file, the original arm64 slice next to a tiny x86_64 program (`tools/rebar_wrap_shim.c`) that starts `rebar` on the same file. A double click, the Dock, `open` and file
associations then work as for any app; the bundle, its Info.plist and the pid are unchanged.

```sh
tools/rebar-wrap status /Applications        # which apps are arm64-only / already wrapped
tools/rebar-wrap enable "/Applications/Some App.app"
tools/rebar-wrap disable "/Applications/Some App.app"   # puts the original executable back, byte for byte
tools/rebar-wrap agent install                # a LaunchAgent: wrap new arm64-only apps in /Applications and ~/Applications as they appear
```

The original executable is kept in `~/Library/Application Support/rebar/wrapped/`. The wrapped file is signed ad hoc. An app that updates itself replaces the file; the agent
(or `tools/rebar-wrap auto`) wraps it again. More folders to watch: one per line in `~/Library/Application Support/rebar/autowrap-folders`.

## License

MIT, see [LICENSE](LICENSE).
