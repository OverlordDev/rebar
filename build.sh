#!/bin/sh
# Link the emulator at a low address so the guest can use 0x100000000.
cd "$(dirname "$0")" && clang -arch x86_64 -std=gnu2x -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-nonliteral -Wno-format-security -Wno-deprecated-declarations \
    -Wl,-no_pie -Wl,-pagezero_size,0x1000 -Wl,-image_base,0x10000 \
    rebar.c -lobjc -o rebar
# ad-hoc signature: on macOS 26 an unsigned executable can hang for good in the kernel's code-signing path when it is started (and every release asset is signed the same way)
codesign --force -s - rebar >/dev/null 2>&1
# rebar-ios: the same emulator marked as a Mac Catalyst process, so the iOS-support UIKit (/System/iOSSupport) loads into it (used by tools/ipa.py for apps that link UIKit)
if xcrun -f vtool >/dev/null 2>&1; then
    xcrun vtool -set-build-version maccatalyst 15.0 26.0 -replace -output rebar-ios rebar >/dev/null 2>&1 && codesign --force -s - rebar-ios >/dev/null 2>&1
fi
# the keychain iOS apps use (see ios_sec.m); rebar-ios and the host bundles find it next to rebar
clang -arch x86_64 -dynamiclib -fobjc-arc -O2 -w -framework Foundation -framework Security ios_sec.m -o librebar_sec.dylib
codesign --force -s - librebar_sec.dylib >/dev/null 2>&1
