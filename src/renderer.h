// Part of linux-wallpaper-fork. See docs/ARCHITECTURE.md for the module map.
#pragma once

// Loading the skeleton and drawing a frame: the camera, the per-slot blend, the
// hit-zone overlay and the item's click burst. The render contract this has to
// satisfy (yDown, premultipliedAlpha, two-colour tint, per-slot blend, clear
// colour) is tabulated in the README; every row of it was wrong at some point.

#include "app.h"

// Finds the .atlas/.skel in `assetDir`, loads them, applies the profile's hidden
// slots and reports profile coverage. False on any failure, with the reason on
// stderr -- including the case where the item was exported from another Spine
// generation, which spine-cpp cannot read.
bool loadSpine(App& a, const std::string& assetDir);
// The item's own js/main.js detaches these slots after every apply, so this is
// repeated per frame rather than done once at load.
void hideProfileSlots(App& a);
// ortho2d over the design space divided by modelScale; also derives a.transpose,
// which the hit-zone mapping depends on.
void setCamera(App& a);
// Advance the animation state by dt and draw. Does not swap buffers.
void renderFrame(App& a, float dt, double now);
void drawHitboxOverlay(App& a, int viewportW, int viewportH);
void drawCursorFx(App& a, double now);
// Spawns the click burst (the native port of the item's js/fireworks.js).
void cursorFxPress(App& a, double x, double y, double now, int zone);
void clampPhysicsCatchUp(App& a);
// One stderr block: profile entries the skeleton lacks, and live hit zones.
void reportProfileCoverage(App& a);
