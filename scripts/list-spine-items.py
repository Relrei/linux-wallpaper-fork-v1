#!/usr/bin/env python3
"""List the Workshop items on this machine that this host can actually run.

Wallpaper Engine items come in several flavours (`video`, `scene`, `web`), and
only a subset of the `web` ones are Spine-based. This host draws Spine
skeletons directly, so it can run *those* and nothing else; the rest belong to
upstream `linux-wallpaperengine`.

The test is on-disk, not by name:

  * `js/main.js` exists                -- a web item with a script
  * a `.skel` exists under `assets/`   -- it is Spine-rigged
  * a `.atlas` sits next to that `.skel` -- the host can load it

    list-spine-items.py              # table of every item found
    list-spine-items.py --all        # include non-Spine items (with a reason)
    list-spine-items.py --json       # machine-readable
    list-spine-items.py --workshop DIR

Nothing here talks to Steam. It reads the directory Steam already wrote, so it
works with Steam closed, and it only ever looks at items you have subscribed to
and downloaded yourself.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

# Wallpaper Engine's Steam appid. Workshop content lands under this tree.
WORKSHOP = Path.home() / ".steam/steam/steamapps/workshop/content/431960"
PROFILE_DIR = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "linux-wallpaper-fork/profiles"


def asset_dirs(item: Path) -> list[Path]:
    """Directories that hold a `.skel` + `.atlas` pair, best resolution first.

    Two layouts are in the wild:
      assets/<res>/name.skel   -- most items ship 2k/4k/8k side by side
      assets/name.skel         -- single-resolution items put them in assets/
    Both are returned as candidates for `--assets`.
    """
    found: list[Path] = []
    assets = item / "assets"
    if not assets.is_dir():
        return found
    for cand in [assets, *sorted(p for p in assets.iterdir() if p.is_dir())]:
        skels = sorted(cand.glob("*.skel"))
        if skels and (cand / (skels[0].stem + ".atlas")).is_file():
            found.append(cand)
    # Prefer the largest: 8k > 4k > 2k reads better as a number than as a name.
    def key(p: Path) -> tuple[int, str]:
        digits = "".join(c for c in p.name if c.isdigit())
        return (int(digits) if digits else 0, p.name)
    return sorted(found, key=key, reverse=True)


# What this build of the host links. A skeleton is only readable by the runtime
# generation it was exported from, so this is the line between "unsupported item"
# and "broken host".
RUNTIME = "4.2"


def skeleton_version(skel: Path) -> str:
    """The Spine version in the .skel header, or "" if unrecognisable.

    The header layout moved between generations (4.x writes an 8-byte hash,
    3.8 a length-prefixed one), so match the first "N.N.N" instead of counting.
    """
    with skel.open("rb") as fh:
        head = fh.read(128)
    m = re.search(rb"\d+\.\d+\.\d+", head)
    return m.group(0).decode("ascii") if m else ""


def project_meta(item: Path) -> tuple[str, str]:
    """(title, type) from project.json; missing fields degrade to the item id."""
    path = item / "project.json"
    if not path.is_file():
        return (item.name, "")
    try:
        data = json.loads(path.read_text(encoding="utf-8", errors="replace"))
    except (json.JSONDecodeError, ValueError):
        return (item.name, "")
    return (str(data.get("title", item.name)), str(data.get("type", "")).lower())


def survey(workshop: Path) -> list[dict]:
    rows = []
    for item in sorted(p for p in workshop.iterdir() if p.is_dir()):
        title, kind = project_meta(item)
        dirs = asset_dirs(item)
        has_js = (item / "js/main.js").is_file()
        version = skeleton_version(sorted(dirs[0].glob("*.skel"))[0]) if dirs else ""
        if not dirs:
            reason = "no .skel/.atlas under assets/ (%s item)" % (kind or "unknown")
        elif version and not version.startswith(RUNTIME + "."):
            reason = f"Spine {version} skeleton; this host links spine-cpp {RUNTIME}"
        elif not has_js:
            # Renders, but every hit zone and animation name would have to be
            # written by hand: make-profile.py reads them out of js/main.js.
            reason = "no js/main.js -- make-profile.py has nothing to read"
        else:
            reason = ""
        rows.append({
            "id": item.name,
            "title": title,
            "type": kind,
            "spine": bool(dirs),
            "assets": [str(d) for d in dirs],
            "resolutions": [d.name for d in dirs],
            "spine_version": version,
            "runnable": bool(dirs) and (not version or version.startswith(RUNTIME + ".")),
            "profile": str(PROFILE_DIR / f"{item.name}.conf") if (PROFILE_DIR / f"{item.name}.conf").is_file() else "",
            "overlay": str(PROFILE_DIR / f"{item.name}.local.conf") if (PROFILE_DIR / f"{item.name}.local.conf").is_file() else "",
            "reason": reason,
        })
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--workshop", type=Path, default=WORKSHOP)
    ap.add_argument("--all", action="store_true", help="list non-Spine items too")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    args = ap.parse_args()

    if not args.workshop.is_dir():
        print(f"no workshop directory at {args.workshop}", file=sys.stderr)
        print("(subscribe to an item in Wallpaper Engine first, or pass --workshop)", file=sys.stderr)
        return 1

    rows = survey(args.workshop)
    shown = rows if args.all else [r for r in rows if r["spine"]]

    if args.json:
        json.dump(shown, sys.stdout, indent=2, ensure_ascii=False)
        print()
        return 0

    if not shown:
        print(f"no Spine items under {args.workshop}", file=sys.stderr)
        return 1

    width = max(len(r["title"]) for r in shown)
    print(f"{'id':<11} {'title':<{width}} {'res':<12} {'spine':<7} {'profile':<9} note")
    for r in shown:
        res = ",".join(r["resolutions"]) if r["spine"] else "-"
        prof = "yes" + ("+local" if r["overlay"] else "") if r["profile"] else "-"
        note = r["reason"] or ""
        ver = r["spine_version"] or "-"
        print(f"{r['id']:<11} {r['title']:<{width}} {res:<12} {ver:<7} {prof:<9} {note}")

    old_gen = [r for r in shown if r["spine"] and not r["runnable"]]
    if old_gen:
        print(f"\n{len(old_gen)} item(s) exported from a Spine generation this host cannot read "
              f"(runtime {RUNTIME}): " + ", ".join(r["id"] for r in old_gen), file=sys.stderr)

    missing = [r for r in shown if r["spine"] and not r["profile"]]
    if missing:
        print(f"\n{len(missing)} Spine item(s) without a profile. Generate them with:", file=sys.stderr)
        print("  python3 scripts/make-profile.py", file=sys.stderr)

    stale = sorted(
        p.stem for p in PROFILE_DIR.glob("*.conf")
        if not p.name.endswith(".local.conf") and not (args.workshop / p.stem).is_dir()
    )
    if stale:
        print(f"\n{len(stale)} profile(s) with no item installed (unsubscribed?): "
              + ", ".join(stale), file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
