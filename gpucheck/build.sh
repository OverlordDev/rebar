#!/bin/sh
# Builds dist/RebarGPUCheck.app and dist/RebarGPUCheck.zip (the thing to send to somebody).
#   sh build.sh                              small build: VS Code is downloaded by the app (~350 MB) or chosen by the user
#   sh build.sh --with-vscode "<VS Code arm64 .app>"   offline build: that app is packed inside (the zip gets ~350 MB bigger)
set -e
cd "$(dirname "$0")"
ROOT=..
NAME=RebarGPUCheck
OUT=dist
APP=$OUT/$NAME.app
WITH=""
[ "$1" = "--with-vscode" ] && WITH="$2"
[ -n "$WITH" ] && [ ! -d "$WITH" ] && { echo "no such app: $WITH"; exit 1; }
SUFFIX=""; [ -n "$WITH" ] && SUFFIX="-offline"

rm -rf "$APP" "$OUT/$NAME$SUFFIX.zip" "$OUT/stage"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/bin"
BIN="$APP/Contents/Resources/bin"

# rebar itself, built for macOS 11 and later (the development build targets the OS it was built on)
clang -arch x86_64 -mmacosx-version-min=11.0 -std=gnu2x -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-nonliteral -Wno-format-security -Wno-deprecated-declarations \
    -Wl,-no_pie -Wl,-pagezero_size,0x1000 -Wl,-image_base,0x10000 "$ROOT/rebar.c" -lobjc -o "$BIN/rebar"

# the small test programs: the same source as arm64 (through rebar) and as native x86_64
for a in arm64 x86_64; do
    clang -arch $a -mmacosx-version-min=11.0 -fobjc-arc -O1 -framework Cocoa -framework Metal -framework QuartzCore -framework IOSurface tests/ios.m -o "$BIN/ios_$a"
done
cp "$ROOT/samples/hello_arm64" "$ROOT/samples/bench/bench_mix_arm" "$ROOT/samples/bench/bench_mix_x86" "$BIN/"

swiftc -O -swift-version 5 -parse-as-library -target x86_64-apple-macos11.0 Sources/App.swift Sources/Runner.swift Sources/Support.swift Sources/WindowGrab.swift \
    -o "$APP/Contents/MacOS/$NAME"
cp Info.plist "$APP/Contents/Info.plist"

if [ -n "$WITH" ]; then
    mkdir -p "$APP/Contents/Resources/payload"
    ditto -c -k --sequesterRsrc --keepParent "$WITH" "$APP/Contents/Resources/payload/VSCode-darwin-arm64.zip"
fi
codesign --force --deep --sign - "$APP"

mkdir -p "$OUT/stage/Rebar GPU Check"
cp -R "$APP" "$OUT/stage/Rebar GPU Check/"
cp README_FOR_TESTER.txt "$OUT/stage/Rebar GPU Check/ПРОЧТИ.txt"
ditto -c -k --keepParent "$OUT/stage/Rebar GPU Check" "$OUT/$NAME$SUFFIX.zip"
rm -rf "$OUT/stage"
ls -la "$OUT/$NAME$SUFFIX.zip"
