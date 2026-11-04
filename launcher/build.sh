#!/bin/sh
# Builds dist/Rebar.app and dist/RebarLauncher.zip (rebar is built from ../rebar.c and packed inside).
set -e
cd "$(dirname "$0")"
NAME=RebarLauncher
OUT=dist
APP=$OUT/Rebar.app   # (the name the user sees and the readme uses; the executable inside is RebarLauncher)
rm -rf "$OUT/$NAME.app" "$APP" "$OUT/$NAME.zip" "$OUT/stage"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/bin"
clang -arch x86_64 -mmacosx-version-min=11.0 -std=gnu2x -O2 -Wno-everything \
    -Wl,-no_pie -Wl,-pagezero_size,0x1000 -Wl,-image_base,0x10000 ../rebar.c -lobjc -o "$APP/Contents/Resources/bin/rebar"
clang -arch x86_64 -mmacosx-version-min=11.0 -dynamiclib -O2 -Wno-everything ../execshim.c -o "$APP/Contents/Resources/bin/librebar_exec.dylib"   # (injected into the shells and tools that guests start: see execshim.c)
swiftc -O -swift-version 5 -parse-as-library -target x86_64-apple-macos11.0 Sources/App.swift Sources/Model.swift Sources/Support.swift Sources/FastEditor.swift -o "$APP/Contents/MacOS/$NAME"
cp Info.plist "$APP/Contents/Info.plist"
codesign --force --deep --sign - "$APP"
mkdir -p "$OUT/stage/Rebar"
cp -R "$APP" "$OUT/stage/Rebar/"
cp README_USER.txt "$OUT/stage/Rebar/README.txt"
ditto -c -k --keepParent "$OUT/stage/Rebar" "$OUT/$NAME.zip"
rm -rf "$OUT/stage"
ls -la "$OUT/$NAME.zip"
