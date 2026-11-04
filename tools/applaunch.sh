#!/bin/sh
# Headless version of the launcher's "Запустить": sh tools/applaunch.sh <App.app> [seconds] [extra args...]
# Starts the app the way launcher/Sources/Model.swift does (own rebar copy, same flags, REBAR_APP_ID marker), waits, prints the process count
# and the windows that belong to the app's processes, then stops everything it started.
D="$(cd "$(dirname "$0")/.." && pwd)"
APP="$1"; SECS="${2:-45}"; shift; shift
R="${REBAR:-$D/rebar}"
EXE="$APP/Contents/MacOS/$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$APP/Contents/Info.plist")"
ARGS=""
if [ -d "$APP/Contents/Frameworks/Electron Framework.framework" ]; then ARGS="--no-sandbox --use-mock-keychain --disable-gpu-sandbox --use-angle=metal --ignore-gpu-blocklist --enable-gpu-rasterization"; fi
LOG="${LOG:-/tmp/applaunch.$(basename "$APP" .app).log}"
ID="applaunch$$"
REBAR_APP_ID="$ID" "$R" "$EXE" $ARGS "$@" > "$LOG" 2>&1 &
LEAD=$!
sleep "$SECS"
PIDS=$(ps -axo pid,command | grep -F "$APP/Contents/" | grep -v grep | awk '{print $1}')
echo "processes: $(echo "$PIDS" | grep -c .), leader alive: $(kill -0 $LEAD 2>/dev/null && echo yes || echo NO)"
"$D/tools/winlist" | while read p rest; do echo "$PIDS" | grep -qx "$p" && echo "window: $p $rest"; done
for p in $PIDS; do kill -9 $p 2>/dev/null; done
kill -9 $LEAD 2>/dev/null
pkill -9 -f "$APP/Contents/" 2>/dev/null
exit 0
