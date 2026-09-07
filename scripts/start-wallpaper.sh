#!/usr/bin/env bash
# Launch a Spine wallpaper: prefer the native host (60–100fps), fall back to
# patched LWE.  usage: start-wallpaper.sh [OUTPUT] [host flags...]
#   LWF_WALLPAPER=<workshop id>  which item to display (required: there is no
#                                default item, and none ships with this repo)
set -euo pipefail

STATE_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/wallpaperctl"
mkdir -p "$STATE_DIR"

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LWF_HOST="${LWF_HOST:-$REPO_DIR/build/linux-wallpaper-fork}"
LWE_BIN="${LWE_BIN:-$HOME/linux-wallpaperengine/build/output/linux-wallpaperengine}"
LWE_DIR="$(dirname "$LWE_BIN")"
OUTPUT="${1:-DP-1}"
shift || true
LWF_ARGS=("$@") # extra host flags, e.g. --hitbox-debug / --voice-volume 0.5
MODE="${WALLPAPER_MODE:-native}" # native | lwe
SCALE="${LWE_WEB_RENDER_SCALE:-0.7}"
WALLPAPER="${LWF_WALLPAPER:-}"
if [[ -z "$WALLPAPER" ]]; then
  echo "set LWF_WALLPAPER=<workshop id> (an item you own) -- none is bundled" >&2
  exit 2
fi
RES="${LWF_RESOLUTION:-4k}"
WORKSHOP="$HOME/.steam/steam/steamapps/workshop/content/431960"
ASSETS="${LWF_ASSETS:-$WORKSHOP/$WALLPAPER/assets/$RES}"
PROFILE="${XDG_CONFIG_HOME:-$HOME/.config}/linux-wallpaper-fork/profiles/$WALLPAPER.conf"

# Interaction data (hitboxes, animation names, voiceline timings) is read from the
# wallpaper's own JS, so a character that has never been played still gets one.
if [[ ! -f "$PROFILE" && -f "$WORKSHOP/$WALLPAPER/js/main.js" ]]; then
  python3 "$(dirname "$0")/make-profile.py" "$WALLPAPER" >/dev/null 2>&1 || true
fi

kill_wallpaper_backends() {
  python3 - <<'PY'
import os, signal, time
names = ("linux-wallpaper-fork", "linux-wallpaperengine")
for sig in (signal.SIGTERM, signal.SIGKILL):
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            exe = os.readlink(f"/proc/{pid}/exe")
            base = os.path.basename(exe.replace(" (deleted)", ""))
        except Exception:
            continue
        if not any(base.startswith(n[:15]) or base == n for n in names):
            # also match truncated proc names
            if base not in names and not base.startswith("linux-wallpaper") and base != "linux-wallpaperengine":
                continue
        try:
            os.kill(int(pid), sig)
        except Exception:
            pass
    time.sleep(0.4)
PY
}

kill_wallpaper_backends

if [[ "$MODE" == "native" && -x "$LWF_HOST" ]]; then
  setsid --fork "$LWF_HOST" --output "$OUTPUT" --assets "$ASSETS" "${LWF_ARGS[@]}" \
    >>"${XDG_CACHE_HOME:-$HOME/.cache}/linux-wallpaper-fork.log" 2>&1
  # remember for wallpaperctl
  cat >"$STATE_DIR/backend.state" <<EOF
BACKEND=lwf-native
ARGS=$(printf '%q' "--output $OUTPUT --assets $ASSETS ${LWF_ARGS[*]}")
WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-}
XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-}
EOF
  echo "wallpaper: started linux-wallpaper-fork on $OUTPUT (item $WALLPAPER)"
  exit 0
fi

if [[ ! -x "$LWE_BIN" ]]; then
  echo "neither lwf-host nor LWE binary found" >&2
  exit 1
fi

cd "$LWE_DIR"
setsid --fork env LWE_WEB_RENDER_SCALE="$SCALE" "$LWE_BIN" \
  --screen-root "$OUTPUT" \
  --bg "$WALLPAPER" \
  --silent \
  --no-audio-processing \
  --fps 60 \
  --set-property mouseactions=true \
  --set-property modelresolution=4k \
  --set-property targetfps=60 \
  --set-property fireworks=false \
  --set-property introanimation=false \
  >>"${XDG_CACHE_HOME:-$HOME/.cache}/lwe-fork.log" 2>&1

cat >"$STATE_DIR/backend.state" <<EOF
BACKEND=lwe
ARGS=$(printf '%q' "--screen-root $OUTPUT --bg $WALLPAPER --silent --no-audio-processing --fps 60 --set-property mouseactions=true --set-property modelresolution=4k --set-property targetfps=60 --set-property fireworks=false --set-property introanimation=false")
LWE_WEB_RENDER_SCALE=$SCALE
WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-}
XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-}
EOF
echo "wallpaper: started LWE item $WALLPAPER on $OUTPUT (scale=$SCALE)"
