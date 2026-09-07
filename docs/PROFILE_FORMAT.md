# Profile format

A profile is everything the host knows about one wallpaper. Nothing in `src/`
carries a character's hit zones, animation names, bone names or voiceline
timings; they all arrive from here. The built-in defaults are empty on purpose,
so **with no profile an item renders but does not react** — the host can never
apply one item's numbers to another.

- Written by `scripts/make-profile.py` from the item's own `js/main.js` and its
  `.skel`, on your machine, from an item you own.
- Read by the host at start-up.
- Location: `~/.config/linux-wallpaper-fork/profiles/<workshop id>.conf`
  (`$XDG_CONFIG_HOME` is honoured).

## File syntax

```
key=value
# comment; also strips everything after a '#' on a value line
```

- No quoting, no escapes, no sections. Whitespace inside a value is significant
  (multi-number values are separated by spaces).
- Unknown keys are ignored — a newer generator does not break an older host.
- A malformed value leaves the field at whatever it already held. `sscanf` is
  used throughout, so a partial value fills the fields it could parse.
- Lines longer than 511 bytes are truncated.
- **A key that is absent is off**, not "inherit from somewhere". Absent zones
  are unclickable, absent animation names miss quietly (every play goes through
  `findAnimation`, which returns null rather than asserting).

## Lookup order

From the `--assets` path the host climbs to the nearest directory containing
`project.json`; that directory's name is the workshop id. Then, in order:

1. `--profile FILE` — if given, this and nothing else. Missing file = exit 1.
2. `<item>/wallpaper-profile.conf` — a profile kept next to the item.
3. `~/.config/linux-wallpaper-fork/profiles/<id>.conf` — what the generator writes.

Whichever of 1–3 loads first **replaces** the built-in defaults. Then:

4. `~/.config/linux-wallpaper-fork/profiles/<id>.local.conf` — an *overlay*. It
   merges on top, so hand-tuned values survive re-running the generator. This is
   where corrections belong: several items declare a `pinch` rect whose top edge
   sits on the eyebrow line, and nudging it down to the cheek here fixes the aim
   without touching the item.

`voice.line` is the one repeated key. On a reset load the lines append; in an
overlay the **first** `voice.line` clears the inherited set, so an overlay
either leaves the timings alone or replaces all of them.

Run with `LWF_TRACE_INPUT=1` to see which files loaded.

## Keys

Angle brackets mark the value shape. "Absent" is what happens with no such line.

### Identity and framing

| Key | Value | Default | Absent |
|---|---|---|---|
| `name` | `<text>` | *(empty)* | the id is used in log lines |
| `design` | `<width> <height>` floats | `2560 1600` | 2560×1600 assumed; every item examined so far uses it |
| `scale` | `<float>` | `0.8` | 0.8. This is `project.json`'s `general.properties.scale`; `--scale` on the command line wins over both |

`design` is the space the hit rects and the item's own `t()` are expressed in,
**y down from the top**. `setCamera()` derives `transpose` from it
(`designW/width` or `designH/height`, whichever axis fits) — without that the
zones land in the wrong place.

### Hit zones

| Key | Value | Default | Absent |
|---|---|---|---|
| `hitbox.headpat` | `<xMin> <xMax> <yMin> <yMax>` floats, design space | empty | zone is not clickable |
| `hitbox.pinch` | same | empty | same |
| `hitbox.voiceline` | same | empty | same |
| `hitbox.handFollow` | same | empty | same |

A rect counts as enabled only when `xMax > xMin && yMax > yMin`, so
`0 0 0 0` is the way an overlay switches a zone **off** without deleting the
line. Priority when rects overlap is headpat, pinch, voiceline, handFollow, then
eye tracking for everything else. The rects usually leave dead bands between
them, so the hit test runs a second pass with a 45-unit grace margin in the same
order; without it roughly one press in three lands on nothing.

`--hitbox-debug` (or `LWF_DRAW_HITBOX=1`) draws them, and works in `--shot`, so
placement can be checked without touching the desktop.

### Animations

All values are Spine animation names, matched exactly. Empty (or absent) means
that step of the gesture does nothing.

| Key | Used for |
|---|---|
| `anim.idle` | track 0, looping, under everything else |
| `anim.pat` / `anim.patEnd` / `anim.patEndA` | head pat: hold, wind-down, and the wind-down's eye layer |
| `anim.pinch` / `anim.pinchA` | cheek pinch while held (tracks 1 and 2) |
| `anim.pinchEnd` / `anim.pinchEndA` | the release pair |
| `anim.look` / `anim.lookA` | gaze toward the cursor |
| `anim.lookEnd` / `anim.lookEndA` | gaze return |
| `anim.handFollow` / `anim.handFollowEnd` | hand-follow hold and release |
| `anim.talk` / `anim.talkA` | voiceline. **Contains one `%d`**, substituted with the 1-based line number (`Talk_0%d_M` → `Talk_03_M`) |

The `*A` variants are the second track — items that lack them simply do not get
that layer. The host prints, at load, every name that the skeleton does not
have (`profile: pat animation 'X' not in skeleton`) plus a summary
(`profile: animations 9 present, 7 unset, 0 not in skeleton`).

### Bones

| Key | Value | Absent |
|---|---|---|
| `bone.point` | bone the head pat drives (upstream's `findBone('Touch_Point')`) | the pat animation plays but nothing follows the cursor — the failure reads as "touch only" |
| `bone.eye` | bone the gaze drives (`Touch_Eye`) | same for the eyes |
| `bone.handFollow` | bone the hand target writes to | hand follow does not move |

### Hand follow tuning

| Key | Value | Default |
|---|---|---|
| `handFollow.range` | `<x> <y>` model units — the hard limit of the target offset | `160 120` |
| `handFollow.outerRange` | `<x> <y>` — a larger soft-limit ellipse; movement slows toward its border | `240 180` |
| `handFollow.speed` | `<follow> <return> <edge>` easing rates per second; `edge` is clamped to ≤ `follow` | `12 4 3` |

Units are model units, not pixels. At the default camera scale a logical pixel
is a few model units wide, so single-digit ranges are effectively immobile.
`handFollow.speed` accepts 1, 2 or 3 numbers; the ones you omit keep their value.

### Click burst

The native port of the item's `js/fireworks.js`. No image assets.

| Key | Value | Default | Clamp |
|---|---|---|---|
| `clickFx.color` | `<r> <g> <b>` 0–255 | `252 146 174` | 0–255 |
| `clickFx.radius` | `<float>` px | `100` | 20–240 |
| `clickFx.duration` | `<float>` s | `1.5` | 0.3–3.0 |
| `clickFx.particles` | `<int>` | `20` | 0–60 |

All three numbers of `clickFx.color` must parse or the line is ignored.

### Audio

| Key | Value | Default | Absent |
|---|---|---|---|
| `voice.pattern` | clip filename with **two** `%d`: line number, then clip number within the line (`CH0334_memoriallobby_%d_%d.ogg`) | *(empty)* | no voicelines |
| `voice.bgm` | filename inside the item's `assets/audio/` | *(empty)* | no BGM |
| `voice.line` | `<total> <start1> <start2>` seconds, one line per voiceline, **in order** | *(empty)* | no voicelines |

`total` is how long the line occupies (the index advances only when it reaches
that time — an interrupted press must not consume a line). `start1`/`start2` are
the offsets at which each clip is fired; a line with one clip has `start2` as
`0` and the second file simply does not exist, which is checked with `access()`.

BGM is off unless `--bgm-volume` is given, even when the item's `project.json`
asks for it: music nobody asked for is worse than silence. The player is
`pw-play` (falling back to `paplay`), forked per clip; there is no decoder in
this process and subtitles are not ported.

### Slots

| Key | Value | Repeats |
|---|---|---|
| `slot.hide` | slot name to detach after every animation apply | yes, one per line |

These are not a visual preference. The item's own renderer detaches them after
every apply; leaving them attached exposes raw reflection/light layers and can
wash the character out. A name the skeleton does not have is reported at load.

## What `make-profile.py` reads

Every Spine Workshop item ships its own `js/main.js`, so the numbers already
exist on disk and do not have to be transcribed.

| Profile output | Source |
|---|---|
| `name`, `scale`, `voice.bgm` | `project.json` — `title`, `general.properties.scale.value`, `general.properties.bgmfile.value`. The property default wins over the JS initializer, because Wallpaper Engine applies it after loading `main.js` |
| `design` | the `t()` helper's `d = { x: { length: 2560, ... }, y: { length: 1600, ... } }` |
| `hitbox.*` | `const HITBOX = { headpat: { xMin: 1200, xMax: 1500, yMin: 220, yMax: 450 }, ... }` — matched per zone name, so a missing zone stays absent |
| `voice.line` | `const AUDIO_DETAIL = [ { time: 15000, count: 2, startTimes: [800, 7000], ... } ]`, milliseconds → seconds, in array order |
| `voice.pattern` | `const CHARACTER = 'CH0334'` → `CH0334_memoriallobby_%d_%d.ogg` |
| `slot.hide` | the item's `slotsToHide` array |
| `bone.point` / `bone.eye` / `bone.handFollow` | every `findBone('...')` in the JS, kept only if the name is present in the `.skel` |
| `anim.*` | probed against the `.skel`: the name is written only if it appears in the binary. Names that do not are listed in a trailing comment |

The `.skel` probe is a substring test on the raw bytes, not a parse: Spine
writes its string table without separators, so neighbouring names merge
("Pat_01_M" + "PatEnd_01_A") and exact-match lookups miss.

Both on-disk layouts are searched for the skeleton — `assets/<res>/x.skel` and
`assets/x.skel`. That matters: with no `.skel` found the probe answers "yes" to
every name, and the profile then claims animations the rig does not have.

```bash
python3 scripts/make-profile.py               # every Spine item found
python3 scripts/make-profile.py <id> ...      # only these
python3 scripts/make-profile.py --stdout <id> # print instead of writing
```

Re-running is safe: it rewrites `<id>.conf` only, and `<id>.local.conf` is
merged on top afterwards.

## Worked example

```
# generated by make-profile.py from .../3650874083/js/main.js
name=Blue Archive | Tendou Aris (Battle)
design=2560 1600
scale=0.8
hitbox.headpat=1200 1500 220 450
hitbox.voiceline=1180 1490 770 1300
hitbox.handFollow=550 850 700 1000
anim.idle=Idle_01
anim.pat=Pat_01_M
anim.patEnd=PatEnd_01_M
anim.pinch=                      # this rig has no pinch keys, so the zone is inert
anim.look=Look_01_M
anim.lookEnd=LookEnd_01_M
anim.handFollow=HandFollow_01_M
anim.handFollowEnd=HandFollowEnd_01_M
anim.talk=Talk_0%d_M
anim.talkA=Talk_0%d_A
bone.point=Touch_Point
bone.eye=Touch_Eye
bone.handFollow=HandFollow
handFollow.range=160 120
handFollow.outerRange=240 180
handFollow.speed=12 4 3
voice.pattern=CH0334_memoriallobby_%d_%d.ogg
voice.bgm=Theme_298.ogg
voice.line=15 0.8 7
voice.line=18 2.3 8.1
# animations not found in the skeleton: Pinch_02_M, Pinch_01_M, ...
```

and an overlay next to it, `3650874083.local.conf`:

```
# Hand-tuned. The generated profile is left untouched.
hitbox.pinch=0 0 0 0             # switch the zone off explicitly
hitbox.handFollow=570 874 714 944
```
