#!/usr/bin/env python3
"""Generate a kei-wallpaper-host profile from a Wallpaper Engine Spine item.

Every one of these Blue Archive wallpapers ships the same `js/main.js` with its
own HITBOX rects, CHARACTER id and AUDIO_DETAIL table, so the per-character data
the host needs is already on disk — reading it beats transcribing it by hand.

    make-profile.py                 # every Spine item in the workshop dir
    make-profile.py <workshop id> ...  # only these ids
    make-profile.py --stdout ID     # print instead of writing

Profiles are written to ~/.config/kei-wallpaper-host/profiles/<id>.conf, which is
where the host looks them up by workshop id.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

WORKSHOP = Path.home() / ".steam/steam/steamapps/workshop/content/431960"
OUT_DIR = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "kei-wallpaper-host/profiles"

# Gesture -> (profile key, track 1 animation, track 2 animation). The reference
# JS hardcodes these names; a differently-rigged item can still be fixed up by
# hand afterwards, so unknown names are reported rather than silently dropped.
ANIMS = {
    "idle": ["Idle_01"],
    "pat": ["Pat_01_M"],
    "patEnd": ["PatEnd_01_M"],
    "patEndA": ["PatEnd_01_A"],
    "pinch": ["Pinch_02_M"],
    "pinchA": ["Pinch_01_M"],
    "pinchEnd": ["PinchEnd_01_A"],
    "pinchEndA": ["PinchEnd_01_M"],
    "look": ["Look_01_M"],
    "lookA": ["Look_01_A"],
    "lookEnd": ["LookEnd_01_M"],
    "lookEndA": ["LookEnd_01_A"],
    "handFollow": ["HandFollow_01_M"],
    "handFollowEnd": ["HandFollowEnd_01_M"],
}


def find_rect(js: str, name: str) -> tuple[float, float, float, float] | None:
    m = re.search(
        name + r"\s*:\s*\{\s*xMin\s*:\s*(-?[\d.]+)\s*,\s*xMax\s*:\s*(-?[\d.]+)\s*,"
        r"\s*yMin\s*:\s*(-?[\d.]+)\s*,\s*yMax\s*:\s*(-?[\d.]+)",
        js,
    )
    return tuple(float(g) for g in m.groups()) if m else None  # type: ignore[return-value]


def find_design(js: str) -> tuple[float, float]:
    m = re.search(r"x\s*:\s*\{\s*length\s*:\s*([\d.]+).*?y\s*:\s*\{\s*length\s*:\s*([\d.]+)", js, re.S)
    return (float(m.group(1)), float(m.group(2))) if m else (2560.0, 1600.0)


def find_voicelines(js: str) -> list[tuple[float, list[float]]]:
    """Pull `time` / `startTimes` out of AUDIO_DETAIL, in order."""
    start = js.find("const AUDIO_DETAIL")
    if start < 0:
        return []
    lines: list[tuple[float, list[float]]] = []
    for entry in re.finditer(r"time\s*:\s*(\d+)\s*,\s*count\s*:\s*(\d+)\s*,\s*startTimes\s*:\s*\[([^\]]*)\]",
                             js[start:]):
        total = float(entry.group(1)) / 1000.0
        starts = [float(v) / 1000.0 for v in re.findall(r"[\d.]+", entry.group(3))]
        lines.append((total, starts))
    return lines


def find_hidden_slots(js: str) -> list[str]:
    """Read the item's per-frame `slotsToHide` list.

    These are not optional visual preferences: the shipped renderer detaches
    them after every animation-state apply. Leaving them attached exposes raw
    reflection/light layers and can wash the entire character out.
    """
    match = re.search(r"\b(?:const|let|var)\s+slotsToHide\s*=\s*\[(.*?)\]\s*;", js, re.S)
    if not match:
        return []
    return [
        value
        for single, double in re.findall(r"'([^']*)'|\"([^\"]*)\"", match.group(1))
        if (value := single or double)
    ]


def skeleton_blob(item: Path) -> bytes:
    """Raw .skel bytes, searched for animation names.

    Spine writes its string table without separators, so tokenising the binary
    merges neighbouring names ("Pat_01_M" + "PatEnd_01_A") and makes exact-match
    lookups miss. A substring test is enough to tell a real name from a guess.
    """
    for skel in item.glob("assets/*/*.skel"):
        return skel.read_bytes()
    return b""


def profile_for(item: Path) -> str | None:
    js_path = item / "js/main.js"
    if not js_path.is_file():
        return None
    js = js_path.read_text(encoding="utf-8", errors="replace")

    title = item.name
    project = item / "project.json"
    scale = 0.8
    project_bgm = ""
    if project.is_file():
        try:
            data = json.loads(project.read_text(encoding="utf-8", errors="replace"))
            title = data.get("title", title)
            props = data.get("general", {}).get("properties", {})
            if "scale" in props:
                scale = float(props["scale"].get("value", scale))
            # Wallpaper Engine applies project property defaults after loading
            # main.js. The JS initializer can therefore name a different song
            # from the one users actually hear on Windows.
            bgm_value = props.get("bgmfile", {}).get("value", "")
            if isinstance(bgm_value, str) and bgm_value.lower().endswith((".ogg", ".mp3", ".wav")):
                project_bgm = Path(bgm_value).name
        except (json.JSONDecodeError, ValueError, AttributeError):
            pass

    character = re.search(r"const CHARACTER\s*=\s*'([^']+)'", js)
    bgm = re.search(r"bgmfile\s*=\s*'[^']*/([^/']+\.ogg)'", js)
    design = find_design(js)
    skel = skeleton_blob(item)

    def has_anim(name: str) -> bool:
        return not skel or name.encode() in skel

    out = [
        f"# generated by make-profile.py from {js_path}",
        f"name={title}",
        f"design={design[0]:g} {design[1]:g}",
        f"scale={scale:g}",
    ]

    for key, box in (
        ("headpat", "headpat"),
        ("pinch", "pinch"),
        ("voiceline", "voiceline"),
        ("handFollow", "handFollow"),
    ):
        rect = find_rect(js, box)
        if rect:
            out.append(f"hitbox.{key}={rect[0]:g} {rect[1]:g} {rect[2]:g} {rect[3]:g}")

    missing = []
    for key, candidates in ANIMS.items():
        for cand in candidates:
            if has_anim(cand):
                out.append(f"anim.{key}={cand}")
                break
        else:
            missing.append(candidates[0])
            out.append(f"anim.{key}=")

    lines = find_voicelines(js)
    # Talk animations are numbered per line; the host substitutes the index.
    out.append("anim.talk=" + ("Talk_0%d_M" if has_anim("Talk_01_M") else ""))
    out.append("anim.talkA=" + ("Talk_0%d_A" if has_anim("Talk_01_A") else ""))
    if b"HandFollow" in skel:
        out.append("bone.handFollow=HandFollow")
        # A bounded target offset, not a cursor attachment. At the default
        # camera scale this is about 64x48 logical pixels in either direction.
        out.append("handFollow.range=160 120")
        # The outer ring adds another 32x24px in each direction and progressively
        # slows movement as the target approaches the hard limit.
        out.append("handFollow.outerRange=240 180")
        # Full-speed / return / outer-edge speed.
        out.append("handFollow.speed=12 4 3")

    if character:
        # count == 1 items use a single clip; the host probes for _1/_2 and skips
        # what is not there, so the two-slot pattern is safe either way.
        out.append(f"voice.pattern={character.group(1)}_memoriallobby_%d_%d.ogg")
    if project_bgm:
        out.append(f"voice.bgm={project_bgm}")
    elif bgm:
        out.append(f"voice.bgm={bgm.group(1)}")
    for slot in find_hidden_slots(js):
        out.append(f"slot.hide={slot}")
    for total, starts in lines:
        starts = (starts + [0.0, 0.0])[:2]
        out.append(f"voice.line={total:g} {starts[0]:g} {starts[1]:g}")

    if missing:
        out.append("# animations not found in the skeleton: " + ", ".join(missing))
    return "\n".join(out) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ids", nargs="*", help="workshop ids (default: all Spine items found)")
    ap.add_argument("--stdout", action="store_true", help="print the profile instead of writing it")
    ap.add_argument("--workshop", type=Path, default=WORKSHOP)
    args = ap.parse_args()

    items = [args.workshop / i for i in args.ids] if args.ids else sorted(
        p.parent.parent for p in args.workshop.glob("*/js/main.js")
    )
    if not items:
        print(f"no Spine wallpapers under {args.workshop}", file=sys.stderr)
        return 1

    written = 0
    for item in items:
        text = profile_for(item)
        if text is None:
            print(f"skip {item.name}: no js/main.js", file=sys.stderr)
            continue
        if args.stdout:
            print(f"=== {item.name} ===")
            print(text, end="")
            continue
        OUT_DIR.mkdir(parents=True, exist_ok=True)
        dest = OUT_DIR / f"{item.name}.conf"
        dest.write_text(text, encoding="utf-8")
        name = next((l[5:] for l in text.splitlines() if l.startswith("name=")), item.name)
        print(f"wrote {dest}  ({name})")
        written += 1
    if written:
        print(f"\nrun with:  kei-wallpaper-host --assets <item>/assets/4k", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
