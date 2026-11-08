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

## License

MIT, see [LICENSE](LICENSE).
