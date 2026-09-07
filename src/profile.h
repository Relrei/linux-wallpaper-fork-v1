// Part of linux-wallpaper-fork. See docs/ARCHITECTURE.md for the module map.
#pragma once

// The wallpaper profile: the whole of what is per-item. Nothing else in src/
// knows a character's name, hit zones, animation names or voiceline timings --
// they arrive here from a `.conf` that scripts/make-profile.py generates on the
// user's machine out of an item they own. See docs/PROFILE_FORMAT.md.

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Wallpaper profile: everything that is per-character lives here so the host
// itself is generic. Values come from a `.conf` next to the wallpaper (see
// scripts/make-profile.py, which reads the workshop item's own js/main.js);
// the built-in defaults are empty, so an unprofiled run renders without reacting.
// ---------------------------------------------------------------------------

struct Hitbox {
	const char* name;
	float xMin, xMax, yMin, yMax;
	float r, g, b; // debug overlay colour
	// Not every item declares every zone -- several ship no pinch rect at all --
	// and an empty rect must not become a live one near the design-space origin.
	bool enabled() const { return xMax > xMin && yMax > yMin; }
};

struct VoicelineTiming {
	double total;
	double start[2];
};

// Every field below is item-specific and is meant to arrive from a profile that
// `scripts/make-profile.py` generates on the user's own machine, out of the
// workshop item they own (its `js/main.js` holds `HITBOX` / `CHARACTER` /
// `AUDIO_DETAIL`). Shipping a filled-in table here would mean redistributing
// one author's item data, so the built-in defaults are deliberately empty: with
// no profile the character still renders, it simply does not react.
struct Profile {
	std::string name;
	// The design space workshop items express their HITBOX rects and t() in.
	// 2560x1600 is what every item examined so far uses; make-profile writes
	// the real value, so this only matters before a profile is loaded.
	float designW = 2560.f, designH = 1600.f;
	// Design-space rects (y down from the top). An empty rect is a zone the
	// user cannot hit (see Hitbox::enabled).
	Hitbox headpat = {"headpat", 0.f, 0.f, 0.f, 0.f, 0.2f, 1.0f, 0.4f};
	Hitbox pinch = {"pinch", 0.f, 0.f, 0.f, 0.f, 1.0f, 0.85f, 0.2f};
	Hitbox voiceline = {"voiceline", 0.f, 0.f, 0.f, 0.f, 0.3f, 0.8f, 1.0f};
	Hitbox handFollow = {"handFollow", 0.f, 0.f, 0.f, 0.f, 0.95f, 0.35f, 1.0f};

	// Empty animation names are looked up through findAnimation and simply miss,
	// which leaves the track untouched instead of asserting.
	std::string idleAnim;
	std::string patAnim;
	std::string patEndAnim;
	std::string patEndAnimA;
	std::string pinchAnim;
	std::string pinchAnimA;
	std::string pinchEndAnim;
	std::string pinchEndAnimA;
	std::string lookAnim;
	std::string lookAnimA;
	std::string lookEndAnim;
	std::string lookEndAnimA;
	std::string handFollowAnim;
	std::string handFollowEndAnim;
	// "%d" is the 1-based line number; empty disables the talk layer.
	std::string talkAnim;
	std::string talkAnimA;

	std::string eyeBone;
	std::string pointBone;
	std::string handFollowBone;
	// Maximum world-space movement of the hand target while held, in model
	// units. At the default camera scale a logical pixel is a few model units
	// wide, so single-digit values here are effectively immobile.
	float handFollowRangeX = 160.f;
	float handFollowRangeY = 120.f;
	// A larger soft-limit ellipse. Movement remains possible here, but the
	// target eases down from handFollowRate to handFollowEdgeRate at its border.
	float handFollowOuterRangeX = 240.f;
	float handFollowOuterRangeY = 180.f;
	float handFollowRate = 12.f;
	float handFollowReturnRate = 4.f;
	float handFollowEdgeRate = 3.f;
	// Per-workshop native port of js/fireworks.js. Local profile overlays let
	// the manager tune this without modifying the Workshop item.
	float clickFxR = 252.f / 255.f;
	float clickFxG = 146.f / 255.f;
	float clickFxB = 174.f / 255.f;
	float clickFxRadius = 100.f;
	float clickFxDuration = 1.5f;
	int clickFxParticles = 20;

	// Two printf slots: line number, then clip number within the line.
	std::string voicePattern;
	std::string bgmFile;
	// Some workshop items deliberately detach raw lighting/FX slots every frame.
	// Without carrying that item metadata over, those slots cover the character
	// in an unintended white glow.
	std::vector<std::string> hiddenSlots;
	// Voiceline lengths / per-clip start offsets, read out of the item's
	// AUDIO_DETAIL by make-profile. Empty = no voice layer.
	std::vector<VoicelineTiming> voicelines;
};

// Reads `key=value` lines into `p`. `reset` clears the inherited values first --
// a profile is authoritative, an overlay (`.local.conf`) merges on top.
// Returns false if the file cannot be opened.
bool loadProfile(Profile& p, float* modelScale, const std::string& path, bool reset = true);
