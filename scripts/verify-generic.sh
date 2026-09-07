#!/usr/bin/env bash
# Render every profiled Workshop item offscreen and check the host is generic.
#
# The host is meant to carry no per-item knowledge: hit zones, animation names,
# bone names and voiceline timings all arrive from a profile. That claim is only
# worth making if it has been run against more than one item, so this walks every
# profile on the machine, takes a `--shot` (nothing reaches any monitor), and
# reports per item:
#
#   spine      Spine generation the .skel was exported from
#   stand      the process exited 0 and wrote a readable PPM
#   nonblack   share of pixels that are not the clear colour -- catches the
#              "loads fine, draws nothing" failure, which exits 0
#   zones      hit zones the profile enables (0 = the item renders but is inert)
#   warn       profile entries the skeleton does not have ("not in skeleton")
#
# Exit status is 0 only when every *supported* installed item stands and clears
# --min-nonblack. Three things are reported rather than failed, because none of
# them is the host carrying item-specific knowledge:
#
#   * zones and warnings -- an item may legitimately have no pinch rect, and its
#     own js/main.js may name animations its rig no longer has
#   * a profile whose item is no longer installed
#   * a skeleton from another Spine generation: spine-cpp reads only its own, so
#     such an item needs a host built against that runtime. --strict fails on it.
#
#   scripts/verify-generic.sh                 # every profile found
#   scripts/verify-generic.sh 3650874083 ...  # only these ids
#   scripts/verify-generic.sh --strict        # skips count as failures
#   LWF_RESOLUTION=2k scripts/verify-generic.sh
#   scripts/verify-generic.sh --keep-shots DIR
set -uo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${LWF_HOST:-$REPO_DIR/build/linux-wallpaper-fork}"
WORKSHOP="${LWF_WORKSHOP:-$HOME/.steam/steam/steamapps/workshop/content/431960}"
PROFILE_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/linux-wallpaper-fork/profiles"
RES="${LWF_RESOLUTION:-4k}"
SIZE="${LWF_SHOT_SIZE:-960x540}"
SHOT_TIME="${LWF_SHOT_TIME:-2.0}"
MIN_NONBLACK="${LWF_MIN_NONBLACK:-50}"
TIMEOUT="${LWF_SHOT_TIMEOUT:-120}"
KEEP=""
STRICT=0

ids=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --strict) STRICT=1; shift ;;
    --keep-shots) KEEP="$2"; shift 2 ;;
    --min-nonblack) MIN_NONBLACK="$2"; shift 2 ;;
    --resolution) RES="$2"; shift 2 ;;
    -h|--help) sed -n '2,36p' "$0"; exit 0 ;;
    *) ids+=("$1"); shift ;;
  esac
done

if [[ ! -x "$HOST" ]]; then
  echo "no host binary at $HOST (cmake --build build)" >&2
  exit 2
fi

OUT="${KEEP:-$(mktemp -d)}"
mkdir -p "$OUT"
cleanup() { [[ -z "$KEEP" ]] && rm -rf "$OUT"; }
trap cleanup EXIT

if [[ ${#ids[@]} -eq 0 ]]; then
  while IFS= read -r f; do ids+=("$(basename "$f" .conf)"); done < <(
    find "$PROFILE_DIR" -maxdepth 1 -name '*.conf' ! -name '*.local.conf' 2>/dev/null | sort)
fi
if [[ ${#ids[@]} -eq 0 ]]; then
  echo "no profiles in $PROFILE_DIR -- run scripts/make-profile.py first" >&2
  exit 2
fi

# Share of pixels that are not the clear colour. A host that loads an item and
# then draws nothing still exits 0, so the shot has to be looked at.
nonblack() {
  python3 - "$1" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
fields, i = [], 0
while len(fields) < 4:                     # P6 <w> <h> <max>, '#' comments
    while i < len(data) and data[i:i+1].isspace(): i += 1
    if data[i:i+1] == b"#":
        while i < len(data) and data[i:i+1] != b"\n": i += 1
        continue
    j = i
    while j < len(data) and not data[j:j+1].isspace(): j += 1
    fields.append(data[i:j]); i = j
if fields[0] != b"P6":
    print("-1"); raise SystemExit
w, h = int(fields[1]), int(fields[2])
px = data[i+1:i+1+w*h*3]
if len(px) < w*h*3:
    print("-1"); raise SystemExit
try:
    import numpy as np
    a = np.frombuffer(px, dtype=np.uint8).reshape(-1, 3)
    lit = int((a.max(axis=1) > 8).sum())
except ImportError:
    lit = sum(1 for k in range(0, w*h*3, 3) if max(px[k], px[k+1], px[k+2]) > 8)
print("%.2f" % (100.0 * lit / (w * h)))
PY
}

# The Spine generation a .skel was exported from. The header layout moved
# between generations, so match the first "N.N.N" rather than counting bytes.
skel_version() {
  python3 - "$1" <<'SKELVER'
import re, sys
head = open(sys.argv[1], "rb").read(128)
m = re.search(rb"\d+\.\d+\.\d+", head)
print(m.group(0).decode() if m else "?")
SKELVER
}

printf '%-11s %-40s %-7s %-6s %-9s %-6s %-5s %s\n' id name spine stand nonblack zones warn note
fail=0; ran=0; skipped=0; inert=0; warned=0; unsupported=0
for id in "${ids[@]}"; do
  item="$WORKSHOP/$id"
  name=$(sed -n 's/^name=//p' "$PROFILE_DIR/$id.conf" 2>/dev/null | head -1)
  name=${name:-$id}
  if [[ ! -d "$item" ]]; then
    printf '%-11s %-40.40s %-7s %-6s %-9s %-6s %-5s %s\n' "$id" "$name" - skip - - - "item not installed"
    skipped=$((skipped + 1)); continue
  fi
  # Two layouts in the wild: assets/<res>/x.skel and assets/x.skel.
  assets=""
  for cand in "$item/assets/$RES" "$item/assets"; do
    if compgen -G "$cand/*.atlas" >/dev/null 2>&1; then assets="$cand"; break; fi
  done
  if [[ -z "$assets" ]]; then
    for cand in "$item"/assets/*/; do
      if compgen -G "$cand/*.atlas" >/dev/null 2>&1; then assets="${cand%/}"; break; fi
    done
  fi
  if [[ -z "$assets" ]]; then
    printf '%-11s %-40.40s %-7s %-6s %-9s %-6s %-5s %s\n' "$id" "$name" - skip - - - "no .atlas under assets/"
    skipped=$((skipped + 1)); continue
  fi
  skel=$(compgen -G "$assets/*.skel" | head -1)
  ver=$([[ -n "$skel" ]] && skel_version "$skel" || echo "?")

  log="$OUT/$id.log"; ppm="$OUT/$id.ppm"
  timeout "$TIMEOUT" "$HOST" --shot "$ppm" --size "$SIZE" --shot-time "$SHOT_TIME" \
    --assets "$assets" >"$log" 2>&1
  rc=$?
  ran=$((ran + 1))
  note=""
  if [[ $rc -ne 0 ]] && grep -q 'exported from Spine' "$log"; then
    # Not a genericity failure: spine-cpp reads one generation of skeletons.
    printf '%-11s %-40.40s %-7s %-6s %-9s %-6s %-5s %s\n' "$id" "$name" "$ver" skip - - - \
      "skeleton predates this runtime"
    unsupported=$((unsupported + 1)); ran=$((ran - 1))
    [[ $STRICT -eq 1 ]] && fail=$((fail + 1))
    continue
  fi
  if [[ $rc -ne 0 ]]; then
    stand=no; nb="-"
    note="rc=$rc: $(grep -m1 -iE 'error|failed|no \.atlas|cannot' "$log" | cut -c1-60)"
  elif [[ ! -s "$ppm" ]]; then
    stand=no; nb="-"; note="no shot written"
  else
    nb=$(nonblack "$ppm")
    stand=yes
    if [[ "$nb" == "-1" ]]; then stand=no; nb="-"; note="unreadable PPM"
    elif (( $(echo "$nb < $MIN_NONBLACK" | bc -l) )); then stand=no; note="nonblack < $MIN_NONBLACK%"
    fi
  fi
  [[ "$stand" != yes ]] && fail=$((fail + 1))

  # The coverage summary line also ends in "0 not in skeleton"; only the
  # per-entry warnings quote a name, so match the closing quote.
  warn=$(grep -c "' not in skeleton" "$log")
  [[ "$warn" -gt 0 ]] && warned=$((warned + 1))
  zones=$(sed -n 's/^profile: hit zones \([0-9]*\) of 4.*/\1/p' "$log" | head -1)
  if grep -q '^profile: no hit zones' "$log"; then zones=0; fi
  zones=${zones:-?}
  [[ "$zones" == "0" ]] && inert=$((inert + 1))
  if [[ -z "$note" ]] && grep -q '^profile: none found' "$log"; then note="no profile loaded"; fi

  printf '%-11s %-40.40s %-7s %-6s %-9s %-6s %-5s %s\n' \
    "$id" "$name" "$ver" "$stand" "$nb" "$zones" "$warn" "$note"
done

echo
echo "items run $ran, skipped $skipped, unsupported skeleton $unsupported, failed $fail"
echo "items with 0 hit zones (render but are inert): $inert"
echo "items with a profile entry the skeleton lacks: $warned"
[[ -n "$KEEP" ]] && echo "shots and logs kept in $OUT"
if [[ $fail -ne 0 ]]; then
  echo "FAIL: $fail item(s) did not stand up" >&2
  exit 1
fi
echo "PASS: all $ran supported item(s) rendered"
