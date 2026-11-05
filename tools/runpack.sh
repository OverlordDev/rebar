#!/bin/sh
# One line per app: sh tools/runpack.sh <seconds> <App.app>...   Starts each app in a private HOME (and Chromium profile) under a Dock-like minimal environment,
# reports the first window, the windows/processes alive at the end and the faults rebar logged. REBAR_ALL_ARM64=1: universal apps run their arm64 slice.
D="$(cd "$(dirname "$0")/.." && pwd)"
SECS="$1"; shift
for APP in "$@"; do
  N=$(basename "$APP" .app | tr ' ' '_')
  EXE="$APP/Contents/MacOS/$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$APP/Contents/Info.plist")"
  H=/tmp/rp/home_$N; if [ -n "$RP_KEEP_HOME" ]; then rm -rf /tmp/rp/$N; else rm -rf "$H" /tmp/rp/$N; fi; mkdir -p "$H"   # RP_KEEP_HOME=1: keep the private HOME (and the translation cache in it) of the earlier run
  ARGS=""
  if ls "$APP/Contents/Frameworks" 2>/dev/null | grep -q -E "Electron Framework|Framework.framework"; then
    if [ -d "$APP/Contents/Frameworks/Electron Framework.framework" ]; then ARGS="--no-sandbox --use-mock-keychain --disable-gpu-sandbox --use-angle=metal --ignore-gpu-blocklist --enable-gpu-rasterization --user-data-dir=/tmp/rp/$N"; fi
  fi
  rm -f /tmp/rp/faults_$N
  env -i HOME="$H" CFFIXED_USER_HOME="$H" USER="$USER" LOGNAME="$USER" TMPDIR="$TMPDIR" PATH=/usr/bin:/bin:/usr/sbin:/sbin MallocNanoZone=0 ANGLE_FEATURE_OVERRIDES_DISABLED=disableMetalOnNvidia REBAR_ALL_ARM64=1 $REBAR_EXTRA REBAR_FAULT_LOG=/tmp/rp/faults_$N \
    perl -e 'setpgrp(0,0); exec @ARGV' "$D/rebar" "$EXE" $ARGS > /tmp/rp/out_$N 2>&1 &
  PID=$!
  t=0; first=-; wins=0; state=running
  while [ $t -lt $SECS ]; do
    sleep 2; t=$((t+2))
    if ! kill -0 $PID 2>/dev/null; then wait $PID; state="EXITED rc=$? at ${t}s"; break; fi
    PIDS=$(pgrep -g $PID)
    w=$("$D/tools/winlist" | while read p rest; do echo "$PIDS" | grep -qx "$p" && echo w; done | wc -l | tr -d ' ')
    [ "$w" -gt 0 ] && [ "$first" = - ] && first=$t
    [ "$w" -gt "$wins" ] && wins=$w
  done
  np=$(pgrep -g $PID | wc -l | tr -d ' ')
  kill -KILL -$PID 2>/dev/null; pkill -9 -f "$APP/Contents/" 2>/dev/null; sleep 1
  nf=0; [ -s /tmp/rp/faults_$N ] && nf=$(grep -c '^=== pid' /tmp/rp/faults_$N)
  ff=""; [ -s /tmp/rp/faults_$N ] && ff=$(grep -m1 '^rebar: ' /tmp/rp/faults_$N | cut -c1-150)
  re=$(grep -a -m1 '^rebar: ' /tmp/rp/out_$N | cut -c1-150)
  printf '%s\tfirstwin=%s\twindows=%s\tprocs=%s\t%s\tfaults=%s\t%s\t%s\n' "$N" "$first" "$wins" "$np" "$state" "$nf" "$ff" "$re"
done
