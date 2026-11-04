#!/bin/sh
# Seconds until an app opens its first window: sh tools/timetowindow.sh <App.app> [max seconds] [extra args...]   (env like a Dock-opened launcher)
D="$(cd "$(dirname "$0")/.." && pwd)"
APP="$1"; MAX="${2:-120}"; shift; shift
EXE="$APP/Contents/MacOS/$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$APP/Contents/Info.plist")"
ARGS=""; [ -d "$APP/Contents/Frameworks/Electron Framework.framework" ] && ARGS="--no-sandbox --use-mock-keychain --disable-gpu-sandbox --use-angle=metal --ignore-gpu-blocklist --enable-gpu-rasterization"
env -i HOME="$HOME" USER="$USER" LOGNAME="$USER" TMPDIR="$TMPDIR" PATH=/usr/bin:/bin:/usr/sbin:/sbin MallocNanoZone=0 ANGLE_FEATURE_OVERRIDES_DISABLED=disableMetalOnNvidia $TTW_ENV "$D/rebar" "$EXE" $ARGS "$@" > /tmp/ttw.log 2>&1 &
T0=$(python3 -I -c 'import time;print(time.time())')
i=0
while [ $i -lt $((MAX*2)) ]; do
  sleep 0.5; i=$((i+1))
  PIDS=$(ps -axo pid,command | grep -F "$APP/Contents/" | grep -v grep | awk '{print $1}')
  W=$("$D/tools/winlist" | while read p rest; do echo "$PIDS" | grep -qx "$p" && echo "$p $rest"; done | head -1)
  if [ -n "$W" ]; then T1=$(python3 -I -c 'import time;print(time.time())'); python3 -I -c "print('window after %.1f s: $W' % ($T1-$T0))"; break; fi
done
[ -z "$W" ] && echo "no window in $MAX s"
for p in $PIDS; do kill -9 $p 2>/dev/null; done; pkill -9 -f "$APP/Contents/" 2>/dev/null
exit 0
