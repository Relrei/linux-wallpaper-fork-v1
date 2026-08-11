# What this repository does and does not contain

Read this before building or redistributing.

## This repository contains

Original source for a wallpaper host: the Wayland/EGL surface, the renderer,
the interaction model (hit zones, pat/pinch/voice gestures, cursor effects),
the profile system, and the tooling around them. Licensed under the MIT terms
in [LICENSE](LICENSE).

## This repository does not contain

### The Spine Runtimes

Not included, not vendored, not redistributed. `scripts/fetch-third-party.sh`
clones them from Esoteric Software at a pinned commit and applies the patches
in `third_party/spine-gl-patches/`.

The Spine Runtimes are proprietary. Integrating them into software requires a
valid Spine Editor license held **at the time of integration**:

- [Spine Runtimes License](https://esotericsoftware.com/spine-runtimes-license)
- [Spine Editor License](https://esotericsoftware.com/spine-editor-license)

Cloning this repository does not give you that license, and this project cannot
grant one. If you intend to distribute anything built from this source, read
Section 2 of the Editor License first.

### Any wallpaper content

No skeletons, atlases, textures, audio, or profiles for any specific wallpaper
are part of this project. Those belong to the people who made them.

The host is content-agnostic: what a character's hit zones are, which
animations exist, and how its voice lines are timed all arrive from a profile
that `scripts/make-profile.py` generates **on your machine, from an item you
already own**. The built-in defaults are deliberately empty — with no profile,
an item renders but does not react.

### Any affiliation

This project is not affiliated with, endorsed by, or derived from Wallpaper
Engine or its developers, and it is not a fork of any of them. It shares no
code with `linux-wallpaperengine` (GPL-3.0); it is an independent
implementation that reads the same on-disk layout.

Names of other software are used here only to describe interoperability.
