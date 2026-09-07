// Part of linux-wallpaper-fork. See docs/ARCHITECTURE.md for the module map.
#pragma once

// The gesture model: which zone a press landed in, and the state machine that
// turns presses, drags and releases into Spine track changes. It is a port of
// what a web-hosted item does in its own js/main.js, with the differences
// listed in the README under "Interaction". The numbers it works on come from
// the profile, never from this file.

#include "app.h"

// ---------------------------------------------------------------------------
// Interaction: faithful port of js/main.js (HITBOX, t(), pressed/moved/released)
// ---------------------------------------------------------------------------

static const float kHeadpatClamp = 30.f;
static const float kEyeClampX = 200.f;
static const float kEyeClampY = kEyeClampX * (9.f / 16.f);
static const float kHeadpatStep = 5.f;
static const float kEyeStep = 10.f;
static const double kStepInterval = 0.02; // upstream setInterval(..., 20)
static const double kGazeEnterDelay = 3.0;
static const float kGazeBlendSeconds = 1.5f;
// Petting follows the cursor and is smoothed instead of accumulating ±5 jumps
// per motion event, which is what made the stroke look faceted.
static const float kHeadpatFollowRate = 14.f;
// Only guards against a doubled press event, not against the user being quick.
static const double kPressDebounce = 0.08;
// A gesture must take the eyelids over from `Idle_01` at once. With the 0.2s
// default mix the two eyelid tracks blend, and because idle's own blink keeps
// running the blend is not monotone: measured openness ran 0.20 → 0.28 → closed,
// i.e. the eyes shut, half-open, then shut again. How bad it looks depends on
// where idle's blink was when the press landed.
static const float kGestureMix = 0.06f;
// Petting is usually a run of short strokes, and upstream ends the gesture on
// every release: `anim.patEnd` opens the eyes, the next press restarts
// `anim.pat` from frame 0 and shuts them again, so a normal stroke rate flickers
// the eyes open and closed. Releases wait this long for the next stroke first.
static const double kPatRelease = 0.25;
// How far into `anim.pat` the lids are fully down on the items measured here.
// Resuming a pat starts at this offset so the eyes do not travel back open
// through the animation's lead-in. An item whose lead-in is longer wants a
// larger value; it is a look choice, not a correctness one.
static const float kPatLidsDown = 0.18f;
// How far outside the headpat rect a press still counts as continuing a pat that
// is winding down. This is deliberately only a pointer-jitter allowance. 200px
// swallowed most of the upper face for 250ms after every stroke, making a quick
// cheek/miss press look like another head pat.
static const float kPatContinueGrace = 70.f;
// Horizontal drag (design px) needed to take the pinch from 0 to 1.
static const float kPinchDragRange = 120.f;
// Seconds to reach 1 while held without moving. Starting a drag stops it.
static const double kPinchHoldSeconds = 1.5;
// Follow rate for the pinch amount (exponential). Raw drag values stutter.
static const float kPinchFollow = 14.f;
// Extra mix (seconds) into PinchEnd when released at a shallow pinch.
static const float kPinchReleaseMix = 0.14f;

// Upstream's rects leave dead bands between the zones (headpat ends at y 450 and
// pinch starts at 500; pinch ends at 830 and voiceline starts at 870). A press
// there silently fell through to eye tracking, which is most of why interaction
// felt like it only worked sometimes. Retry the test with a margin.
static const float kHitboxGrace = 45.f;

// design space <-> screen pixels, using the transpose derived in setCamera().
float toHitboxSpace(const App& a, double n, bool isX);
float toScreenSpace(const App& a, float n, bool isX);
void modelWorldToScreen(const App& a, float worldX, float worldY, float& x, float& y);

bool inHitbox(const Hitbox& box, float tx, float ty, float grace = 0.f);
// 1 headpat, 2 voiceline, 3 eye track, 4 pinch, 5 hand-follow, 0 nothing.
int zoneAt(const App& a, float tx, float ty, bool* usedGrace);
float patTargetFor(const App& a, double x, double y);

// Track helpers. Every play goes through findAnimation: spine-cpp asserts on an
// unknown name, so a profile entry the rig lacks must miss quietly.
TrackEntry* setAnim(App& a, size_t track, const char* name, bool loop);
TrackEntry* addAnim(App& a, size_t track, const char* name, bool loop, float delay);
bool currentAnimIs(const App& a, size_t track, const std::string& name);
float currentAnimProgress(const App& a, size_t track, const std::string& name);
double animDuration(const App& a, const char* name);
bool reactionOwnsTracks(const App& a);
void beginGazeTracking(App& a, double x, double y);

// The three pointer entry points, in screen pixels. Returns the zone that won.
int pressedMouse(App& a, double x, double y, double now);
void movedMouse(App& a, double x, double y, double dx, double dy);
void releasedMouse(App& a, double now);

// Per-frame easing, the 20ms interaction tick, voiceline scheduling and BGM.
void updateInteraction(App& a, float dt, double now);
// Polls --control for volume / mute / pause / hitbox-editor changes.
void refreshRuntimeControl(App& a, double now);

// The hand-follow bone is moved by writing its local position, so the authored
// base has to be restored before the next AnimationState apply.
void applyHandFollowOffset(App& a);
void restoreHandFollowBase(App& a);

// Live hitbox editing (--hitbox-debug plus the control file).
Hitbox* editBox(App& a, int index);
void writeHitboxFeedback(App& a, bool force = false);
bool beginHitboxDrag(App& a, double x, double y);
void moveHitboxDrag(App& a, double x, double y);
