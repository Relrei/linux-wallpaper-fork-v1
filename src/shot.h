// Part of linux-wallpaper-fork. See docs/ARCHITECTURE.md for the module map.
#pragma once

// Offscreen rendering and scripted replay. Reasoning about which Spine calls a
// gesture makes is how interaction bugs survive; --shot plus LWF_SHOT_SCRIPT
// replays one through the live pressedMouse/movedMouse/releasedMouse instead,
// with no compositor and no window involved.

#include "app.h"

struct ShotEvent {
	double at = 0;
	std::string kind; // press | move | release | anim
	double x = 0, y = 0;
	int track = 1;
	std::string anim;
};

// "TIME:press:X,Y;TIME:move:X,Y;TIME:release;TIME:anim:TRACK,NAME"
std::vector<ShotEvent> parseShotScript(const std::string& spec);
void applyShotEvent(App& a, const ShotEvent& e, double now);

// LWF_LIVE_SCRIPT: the same events, replayed against a real on-screen surface.
extern std::vector<ShotEvent> gLiveScript;
void runLiveScript(App& a, double now);

// Renders one frame to an FBO and writes a PPM. Returns a process exit code.
int runShot(const std::string& assets, const std::string& path, int w, int h, double seconds);
