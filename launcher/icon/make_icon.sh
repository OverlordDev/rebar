#!/bin/sh
# Makes AppIcon.icns (the light icon: what Finder and the Desktop show) and icon-light.png / icon-dark.png (what the running launcher puts in the Dock, whichever matches the appearance)
# from the two SVGs. Run it after the SVGs change; build.sh only copies the results.
set -e
cd "$(dirname "$0")"
T=$(mktemp -d)
swiftc -O render.swift -o "$T/render"
SET="$T/AppIcon.iconset"; mkdir -p "$SET"
for s in 16 32 64 128 256 512 1024; do "$T/render" rebar-icon-light.svg "$T/light-$s.png" $s; done
for pair in 16:icon_16x16 32:icon_16x16@2x 32:icon_32x32 64:icon_32x32@2x 128:icon_128x128 256:icon_128x128@2x 256:icon_256x256 512:icon_256x256@2x 512:icon_512x512 1024:icon_512x512@2x; do
  cp "$T/light-${pair%%:*}.png" "$SET/${pair#*:}.png"
done
iconutil -c icns "$SET" -o AppIcon.icns
"$T/render" rebar-icon-light.svg icon-light.png 512
"$T/render" rebar-icon-dark.svg icon-dark.png 512
rm -rf "$T"
ls -la AppIcon.icns icon-light.png icon-dark.png
