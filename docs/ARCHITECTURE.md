# Architecture

One process, one thread, one `App` struct. It opens a `wlr-layer-shell`
background surface, puts an EGL context on it, and draws a Spine skeleton into
it every frame. Everything that is specific to a wallpaper arrives from a
profile; nothing in `src/` knows which character it is drawing.

```
                            ┌──────────────┐
   Workshop item on disk    │   main.cpp   │  argv, start-up, --shot vs run
   (.skel .atlas .png .ogg) │              │
              │             └──────┬───────┘
              │                    │ owns  App g
              ▼                    ▼
        ┌───────────┐        ┌───────────┐        ┌───────────┐
        │ profile.* │───────▶│   app.h   │◀───────│ wayland.* │ layer-shell,
        │  <id>.conf│  fills │  struct   │ drives │           │ EGL, pointer,
        └───────────┘        │    App    │        └─────┬─────┘ frame callback
                             └─────┬─────┘              │
                  ┌────────────────┼────────────────┐   │ wl_pointer
                  ▼                ▼                ▼   ▼
            ┌───────────┐    ┌───────────┐    ┌─────────────┐
            │ renderer.*│◀───│   shot.*  │    │interaction.*│
            │ GL + Spine│    │ FBO + PPM │    │ zones, state│
            └───────────┘    │  + replay │    │   machine   │
                             └───────────┘    └──────┬──────┘
                                                     ▼
                                               ┌───────────┐
                                               │  audio.*  │ forked pw-play
                                               └───────────┘
```

## Files

| File | Holds | Does **not** hold |
|---|---|---|
| `app.h` | `struct App` — the whole mutable state, plus the third-party includes and `using namespace spine` | any logic |
| `profile.{h,cpp}` | `Hitbox`, `Profile`, and the `key=value` reader. The only place that knows the key names | anything about GL, Wayland or Spine |
| `wayland.{h,cpp}` | registry/output/seat/layer-surface listeners, `initEgl`, `armFrame`, the pointer callbacks, `frameDone` | drawing, gesture logic |
| `renderer.{h,cpp}` | `loadSpine`, `setCamera`, `renderFrame`, per-slot blend, `--hitbox-debug` overlay, the click burst | which zone was hit |
| `interaction.{h,cpp}` | `zoneAt`, `pressedMouse`/`movedMouse`/`releasedMouse`, `updateInteraction`, the 20 ms tick, hitbox editing, `refreshRuntimeControl` | any GL call |
| `audio.{h,cpp}` | `playVoiceline`, `stopVoiceline`, the forked `pw-play` players | when a line should start |
| `shot.{h,cpp}` | `runShot` (FBO → PPM), `parseShotScript`, `applyShotEvent`, `runLiveScript` | a second copy of the render path — it calls the same one |
| `main.cpp` | argv, profile lookup, signal handling, the run loop | anything else |

Two deliberate exceptions to the table, both because splitting them would change
behaviour: `renderFrame` re-arms the frame callback itself (arming it after
`eglSwapBuffers` halves the rate on a 100 Hz panel), and `updateInteraction`
owns the BGM respawn (it is the only per-frame tick that already has `now`).

## Profile keys

Full reference with types, units and defaults: [PROFILE_FORMAT.md](PROFILE_FORMAT.md).

| Group | Keys | Meaning |
|---|---|---|
| identity | `name` `design` `scale` | title, the design space rects are expressed in, camera scale |
| zones | `hitbox.headpat` `hitbox.pinch` `hitbox.voiceline` `hitbox.handFollow` | `xMin xMax yMin yMax` in design space, y down. Absent = not clickable |
| animations | `anim.idle` `anim.pat` `anim.patEnd(A)` `anim.pinch(A)` `anim.pinchEnd(A)` `anim.look(A)` `anim.lookEnd(A)` `anim.handFollow(End)` `anim.talk(A)` | Spine animation names. Absent = that gesture does nothing |
| bones | `bone.eye` `bone.point` `bone.handFollow` | bones the gaze / pat / hand target write to |
| hand follow | `handFollow.range` `handFollow.outerRange` `handFollow.speed` | bounded offset and easing rates |
| click burst | `clickFx.color` `clickFx.radius` `clickFx.duration` `clickFx.particles` | the native port of the item's `js/fireworks.js` |
| audio | `voice.pattern` `voice.bgm` `voice.line` | clip filename pattern, BGM file, per-line timings |
| slots | `slot.hide` | slots the item detaches after every apply |

Lookup order, from `--assets`: `--profile FILE`, else
`<item>/wallpaper-profile.conf`, else
`~/.config/linux-wallpaper-fork/profiles/<id>.conf`. `<id>.local.conf` then
merges on top as an overlay. The item directory is found by climbing to the
nearest `project.json`, so both Workshop layouts (`assets/<res>/` and `assets/`)
resolve to the same id.

## Environment variables

| Variable | Effect |
|---|---|
| `LWF_DRAW_HITBOX=1` | same as `--hitbox-debug`; works in `--shot` |
| `LWF_NO_CURSOR_FX=1` | same as `--no-cursor-fx` |
| `LWF_TRACE_INPUT=1` | log every press/release with design-space coords and the zone it resolved to |
| `LWF_TRACE_FX=1` | log the click burst separately from presses |
| `LWF_TRACE_FRAME=<ms>` | per-frame breakdown for frames over `<ms>` (default 8) |
| `LWF_DEBUG_BONES=1` / `LWF_DEBUG_ANIMS=1` | dump the skeleton's bones / animation names at load |
| `KEI_DEBUG_BLEND=1` | one-shot census of the first frame's draw commands (see note) |
| `KEI_FORCE_NORMAL_BLEND=1` | collapse every slot to Normal blending (A/B for a blend regression) |
| `LWF_SHOT_ANIM=<name>` | pose a single animation in `--shot` |
| `LWF_SHOT_SCRIPT=<spec>` | replay `TIME:press:X,Y;TIME:move:X,Y;TIME:release;TIME:anim:TRACK,NAME` |
| `LWF_SHOT_SERIES=<dir>` / `LWF_SHOT_FPS=<n>` | write the whole timeline, not one frame |
| `LWF_LIVE_SCRIPT=<spec>` | the same replay against a real on-screen surface |
| `LWF_DEBUG_PHYSICS=1` / `LWF_NO_PHYSICS_CLAMP=1` / `LWF_TEST_TIME_JUMP=<s>` | physics catch-up clamp diagnostics |
| `LWF_WALLPAPER` / `LWF_ASSETS` / `LWF_RESOLUTION` / `LWF_HOST` | read by `scripts/start-wallpaper.sh` |
| `LWF_WORKSHOP` / `LWF_SHOT_SIZE` / `LWF_SHOT_TIME` / `LWF_MIN_NONBLACK` / `LWF_SHOT_TIMEOUT` | read by `scripts/verify-generic.sh` |

Note: the two blend switches live in `third_party/spine-gl-patches/`, which the
rename to `LWF_*` did not reach, so they still answer to `KEI_*`. The README
lists them under the new prefix; the names above are the ones that work
(verified: `LWF_DEBUG_BLEND=1` prints nothing, `KEI_DEBUG_BLEND=1` prints the
census). Renaming them means editing the patch, which is left for whoever
re-pins the runtime.

## Gates

Both run offscreen. Nothing appears on any monitor, so they are safe to run
while a wallpaper is live.

**Does it work on more than one item?**

```bash
cmake --build build -j"$(nproc)"
./scripts/verify-generic.sh                # every profile on the machine
./scripts/verify-generic.sh --keep-shots /tmp/gen   # keep the PPMs and logs
```

Exit 0 means every supported installed item rendered and painted more than half
the frame. Items whose skeleton comes from another Spine generation, and
profiles whose item is no longer installed, are reported and skipped;
`--strict` fails on those too.

**Did a refactor change any pixel?**

Take the shots before the change, repeat after, compare. `--shot` is
deterministic at a fixed `--shot-time`, so this is a byte comparison, not a
tolerance:

```bash
shots() {  # $1 = output dir
  for id in 3650874083 3532339978 3734379503; do
    A=~/.steam/steam/steamapps/workshop/content/431960/$id/assets/4k
    ./build/linux-wallpaper-fork --shot "$1/${id}_plain.ppm"  --size 960x540 --shot-time 2.0 --assets "$A"
    LWF_DRAW_HITBOX=1 ./build/linux-wallpaper-fork --shot "$1/${id}_hitbox.ppm" --size 960x540 --shot-time 2.0 --assets "$A"
    LWF_SHOT_SCRIPT="0.2:press:590,100;0.6:move:610,120;1.7:release" \
      ./build/linux-wallpaper-fork --shot "$1/${id}_pat.ppm" --size 960x540 --shot-time 4.5 --assets "$A"
  done
}
mkdir -p /tmp/a /tmp/b && shots /tmp/a          # before
#  ... make the change, rebuild ...
shots /tmp/b && diff <(cd /tmp/a && md5sum *.ppm) <(cd /tmp/b && md5sum *.ppm)
```

Three items × three paths covers the renderer, the profile reader, the overlay
and the gesture state machine. That is the gate the `main.cpp` split was held
to: all nine hashes unchanged.
