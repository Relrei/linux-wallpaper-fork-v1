#!/usr/bin/env bash
# Fetch the Spine Runtimes and apply this project's local changes.
#
# The Spine Runtimes are NOT redistributed with this repository. They are
# licensed by Esoteric Software, and integrating them into software requires a
# valid Spine Editor license held at the time of integration:
#
#   https://esotericsoftware.com/spine-editor-license
#   https://esotericsoftware.com/spine-runtimes-license
#
# This script clones the official repository at a pinned commit and applies the
# small patches in third_party/spine-gl-patches/ to the spine-glfw sample that
# this host builds on. Nothing here removes your obligation to hold a license.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRD="$ROOT/third_party"
SPINE_DIR="$THIRD/spine-runtimes"
SPINE_URL="https://github.com/EsotericSoftware/spine-runtimes.git"
# spine-runtimes 4.2 branch, pinned. Change deliberately: the renderer follows
# spine-cpp 4.2 semantics (two-colour tint, per-slot blend, physics clamp).
SPINE_COMMIT="b81e5a58ed38704aee4f866f0e0ac672623ce914"

echo "This project builds against the Spine Runtimes."
echo "You need your own Spine Editor license to integrate them. See:"
echo "  https://esotericsoftware.com/spine-editor-license"
echo

if [ ! -d "$SPINE_DIR/.git" ]; then
	echo "==> cloning spine-runtimes"
	git clone --filter=blob:none "$SPINE_URL" "$SPINE_DIR"
fi

echo "==> checking out $SPINE_COMMIT"
git -C "$SPINE_DIR" fetch --quiet origin "$SPINE_COMMIT" || git -C "$SPINE_DIR" fetch --quiet origin
git -C "$SPINE_DIR" checkout --quiet "$SPINE_COMMIT"

echo "==> staging spine-gl from spine-glfw"
mkdir -p "$THIRD/spine-gl"
for f in spine-glfw.cpp spine-glfw.h stb_image.h; do
	cp "$SPINE_DIR/spine-glfw/src/$f" "$THIRD/spine-gl/$f"
done

echo "==> applying local patches"
for p in "$THIRD/spine-gl-patches"/*.patch; do
	[ -e "$p" ] || continue
	target="$THIRD/spine-gl/$(basename "${p%.patch}")"
	patch --forward --silent "$target" < "$p" || {
		echo "patch failed for $target -- the pinned commit may have moved" >&2
		exit 1
	}
done

echo "done. now: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build"
