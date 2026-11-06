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
if xcrun -f vtool >/dev/null 2>&1; then   # rebar as a Mac Catalyst process: the UIKit of iOS apps loads only into one (see ../tools/ipa.py)
    xcrun vtool -set-build-version maccatalyst 15.0 26.0 -replace -output "$APP/Contents/Resources/bin/rebar-ios" "$APP/Contents/Resources/bin/rebar" >/dev/null 2>&1 && codesign --force -s - "$APP/Contents/Resources/bin/rebar-ios" >/dev/null 2>&1
fi
clang -arch x86_64 -mmacosx-version-min=11.0 -dynamiclib -O2 -Wno-everything ../execshim.c -o "$APP/Contents/Resources/bin/librebar_exec.dylib"   # (injected into the shells and tools that guests start: see execshim.c)
clang -arch x86_64 -mmacosx-version-min=11.0 -dynamiclib -fobjc-arc -O2 -w -framework Foundation -framework Security ../ios_sec.m -o "$APP/Contents/Resources/bin/librebar_sec.dylib"   # (the keychain of iOS apps)
swiftc -O -swift-version 5 -parse-as-library -target x86_64-apple-macos11.0 Sources/App.swift Sources/Model.swift Sources/Support.swift Sources/FastEditor.swift -o "$APP/Contents/MacOS/$NAME"
cp Info.plist "$APP/Contents/Info.plist"
cp icon/AppIcon.icns icon/icon-light.png icon/icon-dark.png "$APP/Contents/Resources/"   # (made from the SVGs by icon/make_icon.sh)
codesign --force --deep --sign - "$APP"
mkdir -p "$OUT/stage/Rebar"
cp -R "$APP" "$OUT/stage/Rebar/"
cp README_USER.txt "$OUT/stage/Rebar/README.txt"
ditto -c -k --keepParent "$OUT/stage/Rebar" "$OUT/$NAME.zip"
rm -rf "$OUT/stage"
ls -la "$OUT/$NAME.zip"
