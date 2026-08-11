// Native Spine wallpaper host — Spine 4.2 + wlr-layer-shell, no CEF.
#include "spine-glfw.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glew.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wayland-egl.h>

#define class _class
#define namespace _namespace
#define static
extern "C" {
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
}
#undef class
#undef namespace
#undef static

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace spine;

struct App;

struct OutputCandidate {
	App* app = nullptr;
	wl_output* proxy = nullptr;
	uint32_t globalName = 0;
	std::string name;
	std::string description;
	int32_t scale = 1;
};

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
	// Not every character has every zone (Aris has no pinch), and an empty rect
	// must not become a live one near the design-space origin.
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
	// Maximum world-space movement of the hand target while held. Aris's camera
	// maps roughly 2.5 model units to one logical pixel, so single-digit values
	// are effectively immobile.
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

// `key=value` lines, `#` comments. Deliberately not JSON: the generator script
// does the messy parsing of the wallpaper's own JS, so the host only needs a
// format it can read without a dependency.
static bool loadProfile(Profile& p, float* modelScale, const std::string& path, bool reset = true) {
	FILE* f = fopen(path.c_str(), "r");
	if (!f)
		return false;
	// A profile is authoritative: keep the colours and the zone names, but drop
	// the built-in rects/animations so a character without a pinch zone does not
	// inherit another item's, which would fire an animation it has no keys for. An overlay
	// (`.local.conf`) merges on top instead, so hand-tuned zones survive the
	// generator being re-run.
	if (reset) {
		const Profile blank;
		p = Profile();
		p.headpat = {blank.headpat.name, 0, 0, 0, 0, blank.headpat.r, blank.headpat.g, blank.headpat.b};
		p.pinch = {blank.pinch.name, 0, 0, 0, 0, blank.pinch.r, blank.pinch.g, blank.pinch.b};
		p.voiceline = {blank.voiceline.name, 0, 0, 0, 0, blank.voiceline.r, blank.voiceline.g, blank.voiceline.b};
		p.handFollow = {blank.handFollow.name, 0, 0, 0, 0, blank.handFollow.r, blank.handFollow.g,
						blank.handFollow.b};
		p.idleAnim.clear();
		p.patAnim.clear();
		p.patEndAnim.clear();
		p.patEndAnimA.clear();
		p.pinchAnim.clear();
		p.pinchAnimA.clear();
		p.pinchEndAnim.clear();
		p.pinchEndAnimA.clear();
		p.lookAnim.clear();
		p.lookAnimA.clear();
		p.lookEndAnim.clear();
		p.lookEndAnimA.clear();
		p.handFollowAnim.clear();
		p.handFollowEndAnim.clear();
		p.handFollowBone.clear();
		p.talkAnim.clear();
		p.talkAnimA.clear();
		p.voicePattern.clear();
		p.bgmFile.clear();
		p.hiddenSlots.clear();
		p.voicelines.clear();
	}
	// After a reset the list is already empty, so `voice.line` only appends. In an
	// overlay the first one replaces the inherited set.
	bool clearedVoicelines = reset;
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		std::string s(line);
		const size_t hash = s.find('#');
		if (hash != std::string::npos)
			s.erase(hash);
		const size_t eq = s.find('=');
		if (eq == std::string::npos)
			continue;
		std::string key = s.substr(0, eq);
		std::string val = s.substr(eq + 1);
		const char* ws = " \t\r\n";
		key.erase(0, key.find_first_not_of(ws));
		key.erase(key.find_last_not_of(ws) + 1);
		val.erase(0, val.find_first_not_of(ws));
		val.erase(val.find_last_not_of(ws) + 1);
		if (key.empty())
			continue;

		auto rect = [&val](Hitbox& box) {
			std::sscanf(val.c_str(), "%f %f %f %f", &box.xMin, &box.xMax, &box.yMin, &box.yMax);
		};
		if (key == "name")
			p.name = val;
		else if (key == "design")
			std::sscanf(val.c_str(), "%f %f", &p.designW, &p.designH);
		else if (key == "scale")
			*modelScale = static_cast<float>(atof(val.c_str()));
		else if (key == "hitbox.headpat")
			rect(p.headpat);
		else if (key == "hitbox.pinch")
			rect(p.pinch);
		else if (key == "hitbox.voiceline")
			rect(p.voiceline);
		else if (key == "hitbox.handFollow")
			rect(p.handFollow);
		else if (key == "anim.idle")
			p.idleAnim = val;
		else if (key == "anim.pat")
			p.patAnim = val;
		else if (key == "anim.patEnd")
			p.patEndAnim = val;
		else if (key == "anim.patEndA")
			p.patEndAnimA = val;
		else if (key == "anim.pinch")
			p.pinchAnim = val;
		else if (key == "anim.pinchA")
			p.pinchAnimA = val;
		else if (key == "anim.pinchEnd")
			p.pinchEndAnim = val;
		else if (key == "anim.pinchEndA")
			p.pinchEndAnimA = val;
		else if (key == "anim.look")
			p.lookAnim = val;
		else if (key == "anim.lookA")
			p.lookAnimA = val;
		else if (key == "anim.lookEnd")
			p.lookEndAnim = val;
		else if (key == "anim.lookEndA")
			p.lookEndAnimA = val;
		else if (key == "anim.handFollow")
			p.handFollowAnim = val;
		else if (key == "anim.handFollowEnd")
			p.handFollowEndAnim = val;
		else if (key == "anim.talk")
			p.talkAnim = val;
		else if (key == "anim.talkA")
			p.talkAnimA = val;
		else if (key == "bone.eye")
			p.eyeBone = val;
		else if (key == "bone.point")
			p.pointBone = val;
		else if (key == "bone.handFollow")
			p.handFollowBone = val;
		else if (key == "handFollow.range")
			std::sscanf(val.c_str(), "%f %f", &p.handFollowRangeX, &p.handFollowRangeY);
		else if (key == "handFollow.outerRange")
			std::sscanf(val.c_str(), "%f %f", &p.handFollowOuterRangeX, &p.handFollowOuterRangeY);
		else if (key == "handFollow.speed") {
			float follow = p.handFollowRate;
			float returning = p.handFollowReturnRate;
			float edge = p.handFollowEdgeRate;
			const int count = std::sscanf(val.c_str(), "%f %f %f", &follow, &returning, &edge);
			if (count >= 1) p.handFollowRate = follow;
			if (count >= 2) p.handFollowReturnRate = returning;
			if (count >= 3) p.handFollowEdgeRate = std::min(follow, edge);
		}
		else if (key == "clickFx.color") {
			float r = p.clickFxR * 255.f;
			float g = p.clickFxG * 255.f;
			float b = p.clickFxB * 255.f;
			if (std::sscanf(val.c_str(), "%f %f %f", &r, &g, &b) == 3) {
				p.clickFxR = std::min(1.f, std::max(0.f, r / 255.f));
				p.clickFxG = std::min(1.f, std::max(0.f, g / 255.f));
				p.clickFxB = std::min(1.f, std::max(0.f, b / 255.f));
			}
		}
		else if (key == "clickFx.radius")
			p.clickFxRadius = std::min(240.f, std::max(20.f, static_cast<float>(atof(val.c_str()))));
		else if (key == "clickFx.duration")
			p.clickFxDuration = std::min(3.f, std::max(0.3f, static_cast<float>(atof(val.c_str()))));
		else if (key == "clickFx.particles")
			p.clickFxParticles = std::min(60, std::max(0, atoi(val.c_str())));
		else if (key == "voice.pattern")
			p.voicePattern = val;
		else if (key == "voice.bgm")
			p.bgmFile = val;
		else if (key == "slot.hide")
			p.hiddenSlots.push_back(val);
		else if (key == "voice.line") {
			if (!clearedVoicelines) {
				p.voicelines.clear();
				clearedVoicelines = true;
			}
			VoicelineTiming t {};
			if (std::sscanf(val.c_str(), "%lf %lf %lf", &t.total, &t.start[0], &t.start[1]) >= 1)
				p.voicelines.push_back(t);
		}
	}
	fclose(f);
	return true;
}

struct App {
	wl_display* display = nullptr;
	wl_registry* registry = nullptr;
	wl_compositor* compositor = nullptr;
	wl_shm* shm = nullptr;
	wl_seat* seat = nullptr;
	wl_pointer* pointer = nullptr;
	zwlr_layer_shell_v1* layerShell = nullptr;
	wl_output* output = nullptr;
	std::string outputName;
	std::string wantOutput;
	std::vector<OutputCandidate*> outputs;

	wl_surface* surface = nullptr;
	zwlr_layer_surface_v1* layerSurface = nullptr;
	wl_egl_window* eglWindow = nullptr;
	wl_callback* frameCallback = nullptr;

	EGLDisplay eglDisplay = EGL_NO_DISPLAY;
	EGLContext eglContext = EGL_NO_CONTEXT;
	EGLSurface eglSurface = EGL_NO_SURFACE;
	EGLConfig eglConfig {};

	int width = 0;
	int height = 0;
	int scale = 1;
	float modelScale = 0.8f; // project.json property "scale" default
	// resize() derives this alongside the ortho window; the hitbox mapping needs it.
	float transpose = 1.f;
	bool configured = false;
	bool running = true;
	int targetFps = 60;
	std::chrono::steady_clock::time_point nextFrameAt {};
	std::chrono::steady_clock::time_point lastFrameAt {};
	bool pointerIn = false;
	double mouseX = 0;
	double mouseY = 0;
	bool mouseDown = false;

	Atlas* atlas = nullptr;
	SkeletonData* skeletonData = nullptr;
	Skeleton* skeleton = nullptr;
	AnimationStateData* animData = nullptr;
	AnimationState* animState = nullptr;
	renderer_t* renderer = nullptr;
	std::vector<Slot*> hiddenSlotPtrs;

	Bone* touchEye = nullptr;
	Bone* touchPoint = nullptr;
	Bone* handFollowTarget = nullptr;
	float eyeRestX = 0, eyeRestY = 0;
	float pointRestX = 0, pointRestY = 0;
	// mouseSelect upstream: 1 headpat, 2 voiceline, 3 eye track, 4 pinch,
	// 5 character-specific hand-follow.
	int interactMode = 0;
	double lastPressAt = -1;
	bool voicePlaying = false;
	double voiceUntil = 0;
	std::vector<pid_t> voicePids;
	bool untracking = false;
	bool unpetting = false;
	double patEndAt = -1; // scheduled PatEnd; a new stroke cancels it
	// The hand-follow entry is allowed to finish before its End animation starts.
	// A boolean gate is deliberate: different characters have different lengths,
	// so a wall-clock deadline cannot describe completion.
	bool handFollowReleasePending = false;
	double stepAccum = 0; // upstream drives interaction from 20ms setInterval ticks
	double eyeTargetX = 0, eyeTargetY = 0;
	// Entering a monitor must not immediately turn the character's head. Keep the
	// last pose for three seconds, then grow the tracking range smoothly.
	double gazeReadyAt = -1;
	float gazeBlend = 0;
	float patTargetY = 0;
	double handPressX = 0, handPressY = 0;
	float handOffsetX = 0, handOffsetY = 0;
	float handTargetX = 0, handTargetY = 0;
	float handDragBaseX = 0, handDragBaseY = 0;
	// applyHandFollowOffset temporarily changes the authored local position.
	// Restore this exact base before the next AnimationState apply so an
	// animation without a key on every frame cannot accumulate the offset.
	bool handOffsetApplied = false;
	float handBaseLocalX = 0, handBaseLocalY = 0;
	// Pinch: how far the cheek is pulled, 0..1, while held. A horizontal drag
	// drives it directly; it creeps up on its own only while the pointer is
	// still. Upstream queues the whole chain on press, so it is always stuck at 1.
	float pinchAmount = 0;
	float pinchTarget = 0;
	float pinchPrevX = 0;
	int pinchDir = 1;
	bool pinchDragged = false;
	bool haveMousePrev = false;
	double prevMouseX = 0, prevMouseY = 0;

	Profile profile;
	int voiceline = 1; // 1..N, advances per playback like upstream
	float voiceVolume = 0.5f; // upstream `volume`
	float bgmVolume = 0.f;    // project.json bgmvolume is 20, but music unasked-for is worse
	// Set by the manager while another app is playing audio, a game is open, or
	// a fullscreen client is visible. Desired slider volumes remain untouched.
	bool audioMuted = false;
	// A fullscreen client on this host's output stops animation and GL drawing.
	// Frame callbacks remain armed so the tiny control file can resume us.
	bool renderPaused = false;
	pid_t bgmPid = -1;
	double bgmCheckAt = 0;
	// Avoid music jumping back in immediately after a game/video/fullscreen
	// session ends. Voicelines remain responsive to an intentional touch.
	double bgmReadyAt = -1;
	std::string controlPath;
	double controlCheckAt = 0;
	std::string audioDir;
	bool traceInput = false;
	struct PendingAudio {
		double at;
		std::string path;
	};
	std::vector<PendingAudio> pendingAudio;

	// The item's own click burst, drawn natively from what the Windows build
	// runs as js/fireworks.js. No invented mouse trail is layered on top.
	bool cursorFx = true;
	struct Ripple {
		double x = 0, y = 0, at = 0;
		double duration = 1.5;
		float radiusTo = 75;
		float r = 233.f / 255.f, g = 179.f / 255.f, b = 237.f / 255.f;
	};
	std::vector<Ripple> ripples;
	// Native draw of the click burst in the item's js/fireworks.js. The web
	// build uses 20 triangular particles plus a growing ring, no image assets.
	struct TapParticle {
		double x = 0, y = 0, endX = 0, endY = 0, at = 0, duration = 1;
		float radius = 10, angle = 0;
		float r = 0.4f, g = 0.65f, b = 0.87f, alpha = 0.5f;
	};
	std::vector<TapParticle> tapParticles;
	uint32_t fxRng = 0x6d2b79f5u;

	bool drawHitbox = false;
	bool hitboxEdit = false;
	int hitboxEditZone = -1;
	int hitboxDragMask = 0; // 1 left, 2 right, 4 top, 8 bottom, 16 move
	float hitboxDragX = 0, hitboxDragY = 0;
	float hitboxDragRect[4] = {0, 0, 0, 0};
	double hitboxFeedbackAt = 0;
	unsigned long long hitboxControlRevision = 0;
	bool autoIdle = false;
	float autoAngle = 0;
	double lastAutoPat = 0;
	double nextAutoPatIn = 20;
	bool userOverride = false;
	double userOverrideUntil = 0;

	float presentFps = 0;
	int presentCount = 0;
	std::chrono::steady_clock::time_point presentWindowStart {};

	// KEI_TRACE_FRAME=1: break down only the frames past the threshold. A hitch
	// on the first click never shows up in an average fps, so look per frame.
	bool traceFrame = false;
	double traceFrameMs = 8.0;
	// Verbose cursor-fx log. Folding it into KEI_TRACE_INPUT buries the presses.
	bool traceFx = false;
	double nextPointerTraceAt = 0;
};

static App g;

static void frameDone(void* data, wl_callback* cb, uint32_t /*time*/);
static void setCamera(App& a);

static const wl_callback_listener frameListener = {.done = frameDone};

static void armFrame(App& a) {
	if (!a.surface)
		return;
	if (a.frameCallback)
		wl_callback_destroy(a.frameCallback);
	a.frameCallback = wl_surface_frame(a.surface);
	wl_callback_add_listener(a.frameCallback, &frameListener, &a);
	wl_surface_commit(a.surface);
}

static void outputGeometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*,
						   const char*, int32_t) {}
static void outputMode(void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {}
static void outputDone(void*, wl_output*) {}
static void outputScale(void* data, wl_output*, int32_t factor) {
	static_cast<OutputCandidate*>(data)->scale = std::max(1, factor);
}
static void outputName(void* data, wl_output*, const char* name) {
	static_cast<OutputCandidate*>(data)->name = name ? name : "";
}
static void outputDescription(void* data, wl_output*, const char* description) {
	static_cast<OutputCandidate*>(data)->description = description ? description : "";
}

static const wl_output_listener outputListener = {
	.geometry = outputGeometry,
	.mode = outputMode,
	.done = outputDone,
	.scale = outputScale,
	.name = outputName,
	.description = outputDescription,
};

static void seatCapabilities(void* data, wl_seat* seat, uint32_t caps);
static void seatName(void*, wl_seat*, const char*);
static const wl_seat_listener seatListener = {
	.capabilities = seatCapabilities,
	.name = seatName,
};

static void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* iface, uint32_t version) {
	App* a = static_cast<App*>(data);
	if (strcmp(iface, "wl_compositor") == 0) {
		a->compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
	} else if (strcmp(iface, "wl_shm") == 0) {
		a->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
	} else if (strcmp(iface, "wl_seat") == 0) {
		a->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 5));
		// The compositor sends capabilities immediately after the bind. Adding
		// this listener later (after output-enumeration roundtrips) loses that
		// one-shot event, so wl_pointer is never created and every hitbox looks
		// correct but remains completely inert.
		wl_seat_add_listener(a->seat, &seatListener, a);
	} else if (strcmp(iface, "zwlr_layer_shell_v1") == 0) {
		a->layerShell = static_cast<zwlr_layer_shell_v1*>(
			wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, std::min(version, 4u)));
	} else if (strcmp(iface, "wl_output") == 0) {
		auto* candidate = new OutputCandidate;
		candidate->app = a;
		candidate->globalName = name;
		candidate->proxy = static_cast<wl_output*>(
			wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 4u)));
		wl_output_add_listener(candidate->proxy, &outputListener, candidate);
		a->outputs.push_back(candidate);
	}
}

static void registryGlobalRemove(void*, wl_registry*, uint32_t) {}

static const wl_registry_listener registryListener = {
	.global = registryGlobal,
	.global_remove = registryGlobalRemove,
};

static void layerConfigure(void* data, zwlr_layer_surface_v1* surface, uint32_t serial, uint32_t w, uint32_t h) {
	App* a = static_cast<App*>(data);
	zwlr_layer_surface_v1_ack_configure(surface, serial);
	a->width = static_cast<int>(w);
	a->height = static_cast<int>(h);
	a->configured = true;
	if (a->eglWindow)
		wl_egl_window_resize(a->eglWindow, a->width * a->scale, a->height * a->scale, 0, 0);
	setCamera(*a);
	wl_surface_set_buffer_scale(a->surface, a->scale);
	armFrame(*a);
}

static void layerClosed(void* data, zwlr_layer_surface_v1*) {
	static_cast<App*>(data)->running = false;
}

static const zwlr_layer_surface_v1_listener layerListener = {
	.configure = layerConfigure,
	.closed = layerClosed,
};

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
// i.e. she shuts her eyes, half-opens them, then shuts them again. How bad it
// looks depends on where idle's blink was when the press landed.
static const float kGestureMix = 0.06f;
// Petting is usually a run of short strokes, and upstream ends the gesture on
// every release: `PatEnd` opens the eyes, the next press restarts `Pat_01_M` from
// frame 0 and shuts them again, so a normal stroke rate flickers the eyes open
// and closed. Releases wait this long for the next stroke before ending the pat.
static const double kPatRelease = 0.25;
// Where `Pat_01_M` has the lids fully down. Resuming a pat starts here so the
// eyes do not travel back open through the animation's lead-in.
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

// Upstream t(): screen px → design space. The algebra collapses to
// L/2 + transpose * (n - mid) / scale, which is why `transpose` must exist.
static float toHitboxSpace(const App& a, double n, bool isX) {
	const float length = isX ? a.profile.designW : a.profile.designH;
	const float mid = (isX ? a.width : a.height) * 0.5f;
	return length * 0.5f + a.transpose * (static_cast<float>(n) - mid) / a.modelScale;
}

static float toScreenSpace(const App& a, float n, bool isX) {
	const float length = isX ? a.profile.designW : a.profile.designH;
	const float mid = (isX ? a.width : a.height) * 0.5f;
	return mid + a.modelScale * (n - length * 0.5f) / a.transpose;
}

// Spine world coordinates use the renderer camera (origin around the model),
// not the 2560x1600 design coordinates used by HITBOX. Keep this conversion
// beside setCamera's inverse so the hand-follow limit can be drawn truthfully.
static void modelWorldToScreen(const App& a, float worldX, float worldY, float& x, float& y) {
	float viewW = 2560.f / a.modelScale;
	float viewH = 1600.f / a.modelScale;
	const float wr = a.width / 2560.f;
	const float hr = a.height / 1600.f;
	if (wr < hr)
		viewW = viewH * (a.width / static_cast<float>(std::max(1, a.height)));
	else if (wr > hr)
		viewH = viewW * (a.height / static_cast<float>(std::max(1, a.width)));
	const float left = -viewW * 0.5f;
	const float top = 900.f + viewH * 0.5f;
	x = (worldX - left) / viewW * a.width;
	y = (top - worldY) / viewH * a.height;
}

static bool inHitbox(const Hitbox& box, float tx, float ty, float grace = 0.f) {
	return box.enabled() && tx > box.xMin - grace && tx < box.xMax + grace && ty > box.yMin - grace &&
		   ty < box.yMax + grace;
}

// Upstream's rects leave dead bands between the zones (headpat ends at y 450 and
// pinch starts at 500; pinch ends at 830 and voiceline starts at 870). A press
// there silently fell through to eye tracking, which is most of why interaction
// felt like it only worked sometimes. Retry the test with a margin.
static const float kHitboxGrace = 45.f;

// Returns the upstream mouseSelect value, or 3 (eye track) when nothing is hit.
static int zoneAt(const App& a, float tx, float ty, bool* usedGrace) {
	*usedGrace = false;
	if (inHitbox(a.profile.headpat, tx, ty))
		return 1;
	if (inHitbox(a.profile.pinch, tx, ty))
		return 4;
	if (inHitbox(a.profile.voiceline, tx, ty))
		return 2;
	if (inHitbox(a.profile.handFollow, tx, ty))
		return 5;
	*usedGrace = true;
	if (inHitbox(a.profile.headpat, tx, ty, kHitboxGrace))
		return 1;
	if (inHitbox(a.profile.pinch, tx, ty, kHitboxGrace))
		return 4;
	if (inHitbox(a.profile.voiceline, tx, ty, kHitboxGrace))
		return 2;
	if (inHitbox(a.profile.handFollow, tx, ty, kHitboxGrace))
		return 5;
	*usedGrace = false;
	return 3;
}

// Where `Touch_Point` should sit for a cursor at (x, y). Upstream stepped the
// bone by ±5 per motion event keyed off canvas thresholds (`y < 800`,
// `x >= 1440`) that are both always true inside this zone, so every direction
// pushed the same way and the stroke came out faceted. Position mapping keeps
// the diagonal ("clockwise") intent and eases smoothly instead.
static float patTargetFor(const App& a, double x, double y) {
	const float tx = toHitboxSpace(a, x, true);
	const float ty = toHitboxSpace(a, y, false);
	const Hitbox& box = a.profile.headpat;
	const float nx = (tx - (box.xMin + box.xMax) * 0.5f) / ((box.xMax - box.xMin) * 0.5f);
	const float ny = (ty - (box.yMin + box.yMax) * 0.5f) / ((box.yMax - box.yMin) * 0.5f);
	const float proj = std::min(std::max((nx - ny) * 0.5f, -1.f), 1.f);
	return a.pointRestY - proj * kHeadpatClamp;
}

// spine-cpp asserts on unknown animation names and dereferences null in release
// builds, so every play goes through a lookup. Not every item has `_A` variants.
static TrackEntry* setAnim(App& a, size_t track, const char* name, bool loop) {
	if (!a.animState || !a.skeletonData || !a.skeletonData->findAnimation(name))
		return nullptr;
	return a.animState->setAnimation(track, name, loop);
}

static TrackEntry* addAnim(App& a, size_t track, const char* name, bool loop, float delay) {
	if (!a.animState || !a.skeletonData || !a.skeletonData->findAnimation(name))
		return nullptr;
	return a.animState->addAnimation(track, name, loop, delay);
}

static bool currentAnimIs(const App& a, size_t track, const std::string& name) {
	if (!a.animState || name.empty())
		return false;
	TrackEntry* e = a.animState->getCurrent(track);
	return e && e->getAnimation() && name == e->getAnimation()->getName().buffer();
}

// Return the normalized time of the current entry when it is `name`.
// This is used when a release animation is interrupted: restarting from zero
// would visibly reset the character even though the pointer only paused briefly.
static float currentAnimProgress(const App& a, size_t track, const std::string& name) {
	if (!currentAnimIs(a, track, name))
		return -1.f;
	TrackEntry* e = a.animState->getCurrent(track);
	const float duration = e && e->getAnimation() ? e->getAnimation()->getDuration() : 0.f;
	if (duration <= 0.f)
		return -1.f;
	return std::min(1.f, std::max(0.f, e->getTrackTime() / duration));
}

static double animDuration(const App& a, const char* name) {
	if (!a.skeletonData)
		return 0.0;
	Animation* anim = a.skeletonData->findAnimation(name);
	return anim ? anim->getDuration() : 0.0;
}

static pid_t spawnVoiceClipImpl(const std::string& path, float volume) {
	const pid_t pid = fork();
	if (pid != 0)
		return pid;
	setsid();
	const int devnull = open("/dev/null", O_RDWR);
	if (devnull >= 0) {
		dup2(devnull, STDIN_FILENO);
		dup2(devnull, STDOUT_FILENO);
		dup2(devnull, STDERR_FILENO);
	}
	char vol[32];
	std::snprintf(vol, sizeof(vol), "--volume=%.3f", static_cast<double>(volume));
	// Tag the stream at creation time. The manager polls active streams once a
	// second; relying only on media.filename races with PipeWire property
	// publication and briefly classifies our own voice/BGM as external audio.
	execlp("pw-play", "pw-play", "--properties",
		   "application.name=NLE-WE-Wallpaper-Audio", vol, path.c_str(),
		   static_cast<char*>(nullptr));
	execlp("paplay", "paplay", path.c_str(), static_cast<char*>(nullptr));
	_exit(127);
}

static pid_t spawnVoiceClip(App& a, const std::string& path, float volume) {
	if (!a.traceInput)
		return spawnVoiceClipImpl(path, volume);
	const auto t0 = std::chrono::steady_clock::now();
	const pid_t pid = spawnVoiceClipImpl(path, volume);
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	std::fprintf(stderr, "voice: spawn %.2fms pid=%d\n", ms, pid);
	return pid;
}

static void stopVoiceline(App& a);
static void writeHitboxFeedback(App& a, bool force = false);

static void playVoiceline(App& a, double now) {
	const int count = static_cast<int>(a.profile.voicelines.size());
	if (count == 0)
		return;
	// Pressing again does not restart the same line; it stops the current one
	// and advances. Advancing on natural end lives in updateInteraction, so
	// only advance here when something was actually playing.
	if (a.voicePlaying) {
		stopVoiceline(a);
		a.voiceline = (a.voiceline >= count) ? 1 : a.voiceline + 1;
	}
	const int index = std::min(std::max(a.voiceline, 1), count) - 1;
	const VoicelineTiming& timing = a.profile.voicelines[static_cast<size_t>(index)];

	if (a.animState) {
		a.animState->setEmptyAnimation(1, 1.f);
		a.animState->setEmptyAnimation(2, 1.f);
	}
	char name[64];
	if (!a.profile.talkAnim.empty()) {
		std::snprintf(name, sizeof(name), a.profile.talkAnim.c_str(), index + 1);
		addAnim(a, 1, name, false, 0.f);
	}
	if (!a.profile.talkAnimA.empty()) {
		std::snprintf(name, sizeof(name), a.profile.talkAnimA.c_str(), index + 1);
		addAnim(a, 2, name, false, 0.f);
	}
	if (a.animState) {
		a.animState->addEmptyAnimation(1, 0.5f, 0.f);
		a.animState->addEmptyAnimation(2, 0.5f, 0.f);
	}

	if (!a.audioMuted && a.voiceVolume > 0.f && !a.audioDir.empty() && !a.profile.voicePattern.empty()) {
		for (int i = 0; i < 2; ++i) {
			char clip[128];
			std::snprintf(clip, sizeof(clip), a.profile.voicePattern.c_str(), index + 1, i + 1);
			const std::string file = a.audioDir + "/" + clip;
			if (access(file.c_str(), R_OK) == 0)
				a.pendingAudio.push_back({now + timing.start[i], file});
		}
	}

	a.voicePlaying = true;
	a.voiceUntil = now + timing.total;
	if (a.traceInput)
		std::fprintf(stderr, "voice: line %d/%d (%.1fs)\n", index + 1, count, timing.total);
	// Upstream advances the index from the end-of-line timer, so a cut-off line
	// must not consume it: advancing here made every interrupted click skip ahead.
}

// Upstream locks input out for the whole line (11–16s). The native host keeps
// input available; this is called only for another voiceline or an explicit
// automatic mute, never for head/cheek/hand movement.
static void stopVoiceline(App& a) {
	for (pid_t pid : a.voicePids)
		if (pid > 0)
			kill(pid, SIGTERM);
	a.voicePids.clear();
	a.pendingAudio.clear();
	a.voicePlaying = false;
}

// The manager writes this tiny file atomically so audio sliders can apply
// without recreating the EGL/Spine host (which would reset the character).
static void refreshRuntimeControl(App& a, double now) {
	if (a.controlPath.empty() || now < a.controlCheckAt)
		return;
	a.controlCheckAt = now + 0.2;
	FILE* file = std::fopen(a.controlPath.c_str(), "r");
	if (!file)
		return;
	float voice = a.voiceVolume;
	float bgm = a.bgmVolume;
	bool drawHitbox = a.drawHitbox;
	bool muted = a.audioMuted;
	bool renderPaused = a.renderPaused;
	bool hitboxEdit = a.hitboxEdit;
	int hitboxSelected = a.hitboxEditZone;
	float handRangeX = a.profile.handFollowRangeX;
	float handRangeY = a.profile.handFollowRangeY;
	float handOuterRangeX = a.profile.handFollowOuterRangeX;
	float handOuterRangeY = a.profile.handFollowOuterRangeY;
	float handFollowRate = a.profile.handFollowRate;
	float handReturnRate = a.profile.handFollowReturnRate;
	float handEdgeRate = a.profile.handFollowEdgeRate;
	float clickFxR = a.profile.clickFxR;
	float clickFxG = a.profile.clickFxG;
	float clickFxB = a.profile.clickFxB;
	float clickFxRadius = a.profile.clickFxRadius;
	float clickFxDuration = a.profile.clickFxDuration;
	int clickFxParticles = a.profile.clickFxParticles;
	unsigned long long hitboxRevision = a.hitboxControlRevision;
	float hitboxRects[4][4] {};
	bool haveHitboxRect[4] = {false, false, false, false};
	char line[256];
	while (std::fgets(line, sizeof(line), file)) {
		float value = 0.f;
		int enabled = 0;
		if (std::sscanf(line, "voice_volume=%f", &value) == 1)
			voice = std::min(1.f, std::max(0.f, value));
		else if (std::sscanf(line, "bgm_volume=%f", &value) == 1)
			bgm = std::min(1.f, std::max(0.f, value));
		else if (std::sscanf(line, "debug_hitboxes=%d", &enabled) == 1)
			drawHitbox = enabled != 0;
		else if (std::sscanf(line, "auto_muted=%d", &enabled) == 1)
			muted = enabled != 0;
		else if (std::sscanf(line, "render_paused=%d", &enabled) == 1)
			renderPaused = enabled != 0;
		else if (std::sscanf(line, "hitbox_edit=%d", &enabled) == 1)
			hitboxEdit = enabled != 0;
		else if (std::sscanf(line, "hitbox_selected=%d", &enabled) == 1)
			hitboxSelected = std::min(3, std::max(-1, enabled));
		else if (std::sscanf(line, "hitbox_revision=%llu", &hitboxRevision) == 1) {
		}
		else if (std::sscanf(line, "handFollow_range=%f %f", &handRangeX, &handRangeY) == 2) {
			handRangeX = std::max(0.f, handRangeX);
			handRangeY = std::max(0.f, handRangeY);
		}
		else if (std::sscanf(line, "handFollow_outerRange=%f %f",
							 &handOuterRangeX, &handOuterRangeY) == 2) {
			handOuterRangeX = std::max(handRangeX, handOuterRangeX);
			handOuterRangeY = std::max(handRangeY, handOuterRangeY);
		}
		else if (std::strncmp(line, "handFollow_speed=", 17) == 0) {
			float follow = handFollowRate;
			float returning = handReturnRate;
			float edge = handEdgeRate;
			const int count =
				std::sscanf(line, "handFollow_speed=%f %f %f", &follow, &returning, &edge);
			if (count >= 2) {
				handFollowRate = std::max(0.1f, follow);
				handReturnRate = std::max(0.1f, returning);
				if (count >= 3)
					handEdgeRate = std::min(handFollowRate, std::max(0.1f, edge));
			}
		}
		else if (std::sscanf(line, "clickFx_color=%f %f %f",
							 &clickFxR, &clickFxG, &clickFxB) == 3) {
			clickFxR = std::min(1.f, std::max(0.f, clickFxR));
			clickFxG = std::min(1.f, std::max(0.f, clickFxG));
			clickFxB = std::min(1.f, std::max(0.f, clickFxB));
		}
		else if (std::sscanf(line, "clickFx_radius=%f", &clickFxRadius) == 1)
			clickFxRadius = std::min(240.f, std::max(20.f, clickFxRadius));
		else if (std::sscanf(line, "clickFx_duration=%f", &clickFxDuration) == 1)
			clickFxDuration = std::min(3.f, std::max(0.3f, clickFxDuration));
		else if (std::sscanf(line, "clickFx_particles=%d", &clickFxParticles) == 1)
			clickFxParticles = std::min(60, std::max(0, clickFxParticles));
		else if (std::sscanf(line, "hitbox.headpat=%f %f %f %f", &hitboxRects[0][0],
							 &hitboxRects[0][1], &hitboxRects[0][2], &hitboxRects[0][3]) == 4)
			haveHitboxRect[0] = true;
		else if (std::sscanf(line, "hitbox.pinch=%f %f %f %f", &hitboxRects[1][0],
							 &hitboxRects[1][1], &hitboxRects[1][2], &hitboxRects[1][3]) == 4)
			haveHitboxRect[1] = true;
		else if (std::sscanf(line, "hitbox.voiceline=%f %f %f %f", &hitboxRects[2][0],
							 &hitboxRects[2][1], &hitboxRects[2][2], &hitboxRects[2][3]) == 4)
			haveHitboxRect[2] = true;
		else if (std::sscanf(line, "hitbox.handFollow=%f %f %f %f", &hitboxRects[3][0],
							 &hitboxRects[3][1], &hitboxRects[3][2], &hitboxRects[3][3]) == 4)
			haveHitboxRect[3] = true;
	}
	std::fclose(file);
	const bool editModeChanged = hitboxEdit != a.hitboxEdit;
	const bool controlRectsChanged = hitboxRevision != a.hitboxControlRevision;
	// During direct manipulation the host owns the rectangles; rereading the
	// unchanged control file every 200ms must not snap a dragged box backwards.
	// Numeric edits carry a new revision, so they still apply while edit mode is open.
	if (!a.hitboxEdit || editModeChanged || controlRectsChanged) {
		Hitbox* boxes[] = {&a.profile.headpat, &a.profile.pinch, &a.profile.voiceline,
						  &a.profile.handFollow};
		for (int i = 0; i < 4; ++i) {
			if (!haveHitboxRect[i])
				continue;
			boxes[i]->xMin = hitboxRects[i][0];
			boxes[i]->xMax = hitboxRects[i][1];
			boxes[i]->yMin = hitboxRects[i][2];
			boxes[i]->yMax = hitboxRects[i][3];
		}
	}
	a.hitboxControlRevision = hitboxRevision;
	a.hitboxEdit = hitboxEdit;
	a.profile.handFollowRangeX = handRangeX;
	a.profile.handFollowRangeY = handRangeY;
	a.profile.handFollowOuterRangeX = std::max(handRangeX, handOuterRangeX);
	a.profile.handFollowOuterRangeY = std::max(handRangeY, handOuterRangeY);
	a.profile.handFollowRate = handFollowRate;
	a.profile.handFollowReturnRate = handReturnRate;
	a.profile.handFollowEdgeRate = handEdgeRate;
	a.profile.clickFxR = clickFxR;
	a.profile.clickFxG = clickFxG;
	a.profile.clickFxB = clickFxB;
	a.profile.clickFxRadius = clickFxRadius;
	a.profile.clickFxDuration = clickFxDuration;
	a.profile.clickFxParticles = clickFxParticles;
	if (editModeChanged) {
		a.hitboxEditZone = hitboxSelected;
		a.hitboxDragMask = 0;
		a.interactMode = 0;
		a.mouseDown = false;
		if (hitboxEdit) {
			a.drawHitbox = true;
			writeHitboxFeedback(a, true);
		}
		std::fprintf(stderr, "hitbox editor: %s\n", hitboxEdit ? "direct mode" : "closed");
	} else if (a.hitboxDragMask == 0) {
		a.hitboxEditZone = hitboxSelected;
	}
	a.voiceVolume = voice;
	if (!a.hitboxEdit)
		a.drawHitbox = drawHitbox;
	if (renderPaused != a.renderPaused) {
		a.renderPaused = renderPaused;
		std::fprintf(stderr, "render: %s by fullscreen policy\n", renderPaused ? "paused" : "resumed");
	}
	if (muted != a.audioMuted) {
		a.audioMuted = muted;
		if (muted) {
			if (a.bgmPid > 0)
				kill(a.bgmPid, SIGTERM);
			a.bgmPid = -1;
			stopVoiceline(a);
		} else {
			// A voiceline that was interrupted is deliberately not replayed.
			// BGM waits so it does not collide with the tail of the other audio.
			a.bgmCheckAt = 0;
			a.bgmReadyAt = now + 15.0;
			std::fprintf(stderr, "audio: BGM resume queued in 15s\n");
		}
		std::fprintf(stderr, "audio: auto mute %s\n", muted ? "on" : "off");
	}
	if (std::fabs(bgm - a.bgmVolume) > 0.001f) {
		a.bgmVolume = bgm;
		if (a.bgmPid > 0)
			kill(a.bgmPid, SIGTERM);
		a.bgmPid = -1;
		a.bgmCheckAt = 0;
	}
}

// Keep the character looking at the real pointer whenever it is over the
// wallpaper. Upstream only entered this state from a held click; the native
// host receives genuine Wayland motion events, so requiring a click is
// unnecessary and makes the feature feel broken.
//
// Persistent does not mean "highest priority": starting Look with
// setEmptyAnimation while PatEnd / PinchEnd / Talk owns tracks 1 and 2 erases
// that gesture on the first tiny pointer movement after release. That made the
// result depend on whether the mouse happened to move by a pixel at the wrong
// time. Gaze waits for those finite reactions and resumes automatically on the
// next motion/frame instead.
static bool reactionOwnsTracks(const App& a) {
	if (a.voicePlaying || a.patEndAt >= 0 || a.handFollowReleasePending)
		return true;
	return currentAnimIs(a, 1, a.profile.patAnim) ||
		   currentAnimIs(a, 1, a.profile.patEndAnim) ||
		   currentAnimIs(a, 1, a.profile.pinchEndAnim) ||
		   currentAnimIs(a, 2, a.profile.pinchEndAnimA) ||
		   currentAnimIs(a, 1, a.profile.handFollowAnim) ||
		   currentAnimIs(a, 1, a.profile.handFollowEndAnim);
}

static void beginGazeTracking(App& a, double x, double y) {
	if (a.interactMode == 3) {
		a.eyeTargetX = x;
		a.eyeTargetY = y;
		return;
	}
	if (a.interactMode != 0 || reactionOwnsTracks(a))
		return;
	if (a.animState) {
		a.animState->setEmptyAnimation(1, 0.f);
		a.animState->setEmptyAnimation(2, 0.f);
	}
	if (TrackEntry* e = addAnim(a, 1, a.profile.lookAnim.c_str(), false, 0.f))
		e->setMixDuration(1.0f);
	if (TrackEntry* e = addAnim(a, 2, a.profile.lookAnimA.c_str(), false, 0.f))
		e->setMixDuration(1.0f);
	a.interactMode = 3;
	a.untracking = false;
	a.gazeBlend = 0.f;
	a.eyeTargetX = x;
	a.eyeTargetY = y;
}

// Returns the zone selected for this exact press. The visual feedback must use
// this value, not infer it later from mutable interaction state.
static int pressedMouse(App& a, double x, double y, double now) {
	const float tx = toHitboxSpace(a, x, true);
	const float ty = toHitboxSpace(a, y, false);
	bool usedGrace = false;
	int zone = zoneAt(a, tx, ty, &usedGrace);
	if (a.traceInput && usedGrace)
		std::fprintf(stderr, "input: matched zone %d via grace margin\n", zone);
	// The second click of a double-click on the head lands a few pixels off and
	// resolves to eye tracking, which cuts the pat off and snaps the eyes open
	// mid-gesture. While a pat is winding down, a press that hit nothing in
	// particular near the head continues it. An explicit pinch/voiceline press is
	// never affected — only zone 3 is reconsidered.
	if (zone == 3 && a.patEndAt >= 0 && inHitbox(a.profile.headpat, tx, ty, kPatContinueGrace)) {
		zone = 1;
		if (a.traceInput)
			std::fprintf(stderr, "input: continuing headpat\n");
	}
	// Other gestures must not cut a line short. This used to stop playback on
	// every press ("cut off beats unresponsive"), which meant a head pat killed
	// the dialogue. The input lock is long gone, so the only place that needs to
	// stop a line is where a new one starts (inside playVoiceline).
	// Any other gesture takes track 1 over, so a pending PatEnd would only fire
	// into the middle of it.
	if (zone != 1)
		a.patEndAt = -1;
	if (zone != 5)
		a.handFollowReleasePending = false;

	if (zone == 1) {
		// A stroke that lands before the pending PatEnd continues the same pat.
		a.patEndAt = -1;
		const bool wasEnding = currentAnimIs(a, 1, a.profile.patEndAnim);
		if (a.traceInput && currentAnimIs(a, 1, a.profile.patAnim)) {
			TrackEntry* current = a.animState->getCurrent(1);
			std::fprintf(stderr, "headpat: preserve track at %.3fs\n",
						 current ? static_cast<double>(current->getTrackTime()) : 0.0);
		}
		// Keep a running pat exactly where it is. The old code rewound it to
		// kPatLidsDown on every press, so a normal sequence of short strokes looked
		// like the motion reset whenever the click timing happened to cross a frame.
		if (currentAnimIs(a, 1, a.profile.patAnim)) {
			// Nothing to restart. Pointer-driven bone motion below provides the
			// visible continuation even if the non-looping entry reached its end.
		} else if (TrackEntry* e = setAnim(a, 1, a.profile.patAnim.c_str(), false)) {
			e->setMixDuration(kGestureMix);
			// When PatEnd is interrupted, start after the open-eye lead-in. A truly
			// new gesture still starts at frame zero.
			if (wasEnding)
				e->setTrackTime(std::min(
					kPatLidsDown, static_cast<float>(animDuration(a, a.profile.patAnim.c_str()))));
			// The pat has no `_A` layer, so track 2 keeps whatever the last release
			// left there — and that entry's queued empty animation then fades out
			// mid-pat, which reads as the eyes drifting open and shutting again.
			if (a.animState->getCurrent(2))
				a.animState->setEmptyAnimation(2, kGestureMix);
		}
		a.interactMode = 1;
		a.unpetting = false;
		a.patTargetY = patTargetFor(a, x, y);
		return zone;
	}
	if (zone == 4) {
		// If the release is still returning the cheek to neutral, continue from
		// that visible amount rather than snapping the scrub animation back to 0.
		// PinchEnd runs 1 (pinched) -> 0 (neutral), hence `1 - progress`.
		const float endProgress = currentAnimProgress(a, 1, a.profile.pinchEndAnim);
		const float resumeAmount = endProgress >= 0.f ? 1.f - endProgress : 0.f;
		if (a.traceInput && endProgress >= 0.f)
			std::fprintf(stderr, "pinch: resume release at amount %.3f\n", static_cast<double>(resumeAmount));
		// Upstream queues PinchEnd right here, so the whole chain plays out no
		// matter how long the button is held — the cheek can only ever be clicked.
		// Holding the last frame instead just moved the problem: the pose was then
		// pinned at fully-pinched. Scrub the animation by hand instead — timeScale 0
		// and `setTrackTime` every frame — so the pinch amount follows the pointer.
		if (TrackEntry* e = setAnim(a, 1, a.profile.pinchAnim.c_str(), false)) {
			e->setMixDuration(kGestureMix);
			e->setTimeScale(0.f);
			const float duration = e->getAnimation() ? e->getAnimation()->getDuration() : 0.f;
			e->setTrackTime(std::min(resumeAmount * duration, std::max(0.f, duration - 1e-3f)));
		}
		if (TrackEntry* e = setAnim(a, 2, a.profile.pinchAnimA.c_str(), false)) {
			e->setMixDuration(kGestureMix);
			e->setTimeScale(0.f);
			const float duration = e->getAnimation() ? e->getAnimation()->getDuration() : 0.f;
			e->setTrackTime(std::min(resumeAmount * duration, std::max(0.f, duration - 1e-3f)));
		}
		a.interactMode = 4;
		a.pinchAmount = resumeAmount;
		a.pinchTarget = resumeAmount;
		a.pinchPrevX = tx;
		a.pinchDragged = false;
		// Which way pulls the cheek outward: +x if the rect sits right of centre.
		const float boxMid = 0.5f * (a.profile.pinch.xMin + a.profile.pinch.xMax);
		a.pinchDir = (boxMid >= 0.5f * a.profile.designW) ? 1 : -1;
		return zone;
	}
	if (zone == 2) {
		// Upstream only arms here; the line plays on release.
		a.interactMode = 2;
		return zone;
	}
	if (zone == 5) {
		if (TrackEntry* e = setAnim(a, 1, a.profile.handFollowAnim.c_str(), false))
			e->setMixDuration(kGestureMix);
		a.handFollowReleasePending = false;
		a.handPressX = x;
		a.handPressY = y;
		a.handDragBaseX = a.handOffsetX;
		a.handDragBaseY = a.handOffsetY;
		a.handTargetX = a.handOffsetX;
		a.handTargetY = a.handOffsetY;
		a.interactMode = 5;
		return zone;
	}

	// An outside press is still a valid gaze target. When Look is already
	// running, keep its TrackEntry and only update the target: clearing and
	// re-adding the same animation made every ordinary click visibly reset gaze.
	a.eyeTargetX = x;
	a.eyeTargetY = y;
	if (a.interactMode == 3) {
		if (a.traceInput)
			std::fprintf(stderr, "gaze: preserve current track on click\n");
		return zone;
	}
	if (now >= a.gazeReadyAt)
		beginGazeTracking(a, x, y);
	return zone;
}

static void movedMouse(App& a, double x, double y, double dx, double dy) {
	switch (a.interactMode) {
		case 1:
			if (!a.touchPoint)
				break;
			a.patTargetY = patTargetFor(a, x, y);
			(void)dx;
			(void)dy;
			break;
		case 2:
			// Dragging out of the voiceline zone cancels it.
			a.interactMode = 0;
			break;
		case 3:
			a.eyeTargetX = x;
			a.eyeTargetY = y;
			break;
		case 4: {
			// Accumulate relatively. Absolute positions jump when the user presses
			// again away from the original point.
			const float tx = toHitboxSpace(a, x, true);
			const float moved = (tx - a.pinchPrevX) * static_cast<float>(a.pinchDir);
			a.pinchPrevX = tx;
			if (std::fabs(moved) > 0.f) {
				a.pinchDragged = true;
				a.pinchTarget = std::min(1.f, std::max(0.f, a.pinchTarget + moved / kPinchDragRange));
			}
			break;
		}
		case 5: {
			// The HandFollow bone is a constrained target, not an unrestricted
			// cursor attachment. Clamp along one radial ellipse instead of X/Y
			// independently: rectangular limits make diagonal motion hit both
			// axes at once and feel as if it decelerates twice near corners.
			const float dx = toHitboxSpace(a, x, true) - toHitboxSpace(a, a.handPressX, true);
			const float dy = toHitboxSpace(a, a.handPressY, false) - toHitboxSpace(a, y, false);
			float targetX = a.handDragBaseX + dx;
			float targetY = a.handDragBaseY + dy;
			const float outerX = std::max(0.01f, a.profile.handFollowOuterRangeX);
			const float outerY = std::max(0.01f, a.profile.handFollowOuterRangeY);
			const float normalizedRadius =
				std::sqrt((targetX * targetX) / (outerX * outerX) +
						  (targetY * targetY) / (outerY * outerY));
			if (normalizedRadius > 1.f) {
				targetX /= normalizedRadius;
				targetY /= normalizedRadius;
			}
			a.handTargetX = targetX;
			a.handTargetY = targetY;
			break;
		}
		default:
			break;
	}
}

static void releasedMouse(App& a, double now) {
	switch (a.interactMode) {
		case 1:
			// The hand comes home immediately, but the expression waits in case this
			// is the gap between two strokes rather than the end of the petting.
			a.unpetting = true;
			a.patEndAt = now + kPatRelease;
			break;
		case 2:
			playVoiceline(a, now);
			break;
		case 4: {
			// Releasing part-way disagrees with PinchEnd's first pose (fully
			// pinched), so mix in proportion to how far the pinch got: the
			// deeper it is, the shorter the mix.
			const float mix = kGestureMix + (1.f - a.pinchAmount) * kPinchReleaseMix;
			if (TrackEntry* e = setAnim(a, 1, a.profile.pinchEndAnim.c_str(), false))
				e->setMixDuration(mix);
			if (TrackEntry* e = setAnim(a, 2, a.profile.pinchEndAnimA.c_str(), false))
				e->setMixDuration(mix);
			if (a.animState) {
				a.animState->addEmptyAnimation(1, 0.5f, 0.f);
				a.animState->addEmptyAnimation(2, 0.5f, 0.f);
			}
			a.pinchAmount = 0.f;
			a.pinchTarget = 0.f;
			a.pinchDragged = false;
			break;
		}
		case 5:
			// Do not guess the animation length. The frame loop waits until the
			// actual non-looping TrackEntry reports completion, then starts End.
			a.handFollowReleasePending = true;
			a.handTargetX = 0.f;
			a.handTargetY = 0.f;
			break;
		case 3: {
			a.untracking = true;
			TrackEntry* e1 = setAnim(a, 1, a.profile.lookEndAnim.c_str(), false);
			TrackEntry* e2 = setAnim(a, 2, a.profile.lookEndAnimA.c_str(), false);
			if (e1)
				e1->setMixDuration(0.f);
			if (e2)
				e2->setMixDuration(0.f);
			if (a.animState) {
				a.animState->addEmptyAnimation(1, 0.5f, 0.f);
				a.animState->addEmptyAnimation(2, 0.5f, 0.f);
			}
			break;
		}
		default:
			break;
	}
	a.interactMode = 0;
}

static void cursorFxPress(App& a, double x, double y, double now, int zone);

static Hitbox* editBox(App& a, int index) {
	switch (index) {
		case 0: return &a.profile.headpat;
		case 1: return &a.profile.pinch;
		case 2: return &a.profile.voiceline;
		case 3: return &a.profile.handFollow;
		default: return nullptr;
	}
}

static void writeHitboxFeedback(App& a, bool force) {
	if (a.controlPath.empty())
		return;
	const double now =
		std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	if (!force && now - a.hitboxFeedbackAt < 0.05)
		return;
	a.hitboxFeedbackAt = now;
	const std::string path = a.controlPath + ".hitboxes";
	const std::string temp = path + ".tmp";
	FILE* file = std::fopen(temp.c_str(), "w");
	if (!file)
		return;
	const Hitbox* boxes[] = {&a.profile.headpat, &a.profile.pinch, &a.profile.voiceline,
							 &a.profile.handFollow};
	const char* names[] = {"headpat", "pinch", "voiceline", "handFollow"};
	for (int i = 0; i < 4; ++i)
		std::fprintf(file, "hitbox.%s=%.0f %.0f %.0f %.0f\n", names[i], boxes[i]->xMin,
					 boxes[i]->xMax, boxes[i]->yMin, boxes[i]->yMax);
	std::fclose(file);
	std::rename(temp.c_str(), path.c_str());
}

static bool beginHitboxDrag(App& a, double x, double y) {
	const float tx = toHitboxSpace(a, x, true);
	const float ty = toHitboxSpace(a, y, false);
	// A 14px screen-space grab band makes thin borders usable at any model scale.
	const float threshold = 14.f * a.transpose / std::max(0.1f, a.modelScale);
	for (int pass = 0; pass < 4; ++pass) {
		const int i = a.hitboxEditZone >= 0 ? a.hitboxEditZone : pass;
		if (a.hitboxEditZone >= 0 && pass > 0)
			break;
		Hitbox* box = editBox(a, i);
		if (!box || !box->enabled())
			continue;
		if (tx < box->xMin - threshold || tx > box->xMax + threshold ||
			ty < box->yMin - threshold || ty > box->yMax + threshold)
			continue;
		int mask = 0;
		if (std::abs(tx - box->xMin) <= threshold)
			mask |= 1;
		if (std::abs(tx - box->xMax) <= threshold)
			mask |= 2;
		if (std::abs(ty - box->yMin) <= threshold)
			mask |= 4;
		if (std::abs(ty - box->yMax) <= threshold)
			mask |= 8;
		if (mask == 0 && tx > box->xMin && tx < box->xMax && ty > box->yMin && ty < box->yMax)
			mask = 16;
		if (mask == 0)
			continue;
		a.hitboxEditZone = i;
		a.hitboxDragMask = mask;
		a.hitboxDragX = tx;
		a.hitboxDragY = ty;
		a.hitboxDragRect[0] = box->xMin;
		a.hitboxDragRect[1] = box->xMax;
		a.hitboxDragRect[2] = box->yMin;
		a.hitboxDragRect[3] = box->yMax;
		return true;
	}
	a.hitboxEditZone = -1;
	a.hitboxDragMask = 0;
	return false;
}

static void moveHitboxDrag(App& a, double x, double y) {
	Hitbox* box = editBox(a, a.hitboxEditZone);
	if (!box || a.hitboxDragMask == 0)
		return;
	const float dx = toHitboxSpace(a, x, true) - a.hitboxDragX;
	const float dy = toHitboxSpace(a, y, false) - a.hitboxDragY;
	float x0 = a.hitboxDragRect[0], x1 = a.hitboxDragRect[1];
	float y0 = a.hitboxDragRect[2], y1 = a.hitboxDragRect[3];
	if (a.hitboxDragMask & 16) {
		x0 += dx;
		x1 += dx;
		y0 += dy;
		y1 += dy;
	} else {
		if (a.hitboxDragMask & 1) x0 += dx;
		if (a.hitboxDragMask & 2) x1 += dx;
		if (a.hitboxDragMask & 4) y0 += dy;
		if (a.hitboxDragMask & 8) y1 += dy;
	}
	constexpr float minimum = 20.f;
	if (x1 - x0 < minimum) {
		if (a.hitboxDragMask & 1) x0 = x1 - minimum;
		else x1 = x0 + minimum;
	}
	if (y1 - y0 < minimum) {
		if (a.hitboxDragMask & 4) y0 = y1 - minimum;
		else y1 = y0 + minimum;
	}
	box->xMin = x0;
	box->xMax = x1;
	box->yMin = y0;
	box->yMax = y1;
	writeHitboxFeedback(a);
}

static void pointerEnter(void* data, wl_pointer*, uint32_t serial, wl_surface*, wl_fixed_t sx, wl_fixed_t sy) {
	App* a = static_cast<App*>(data);
	const double now =
		std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	a->pointerIn = true;
	a->mouseX = wl_fixed_to_double(sx);
	a->mouseY = wl_fixed_to_double(sy);
	a->prevMouseX = a->mouseX;
	a->prevMouseY = a->mouseY;
	a->haveMousePrev = true;
	a->eyeTargetX = a->mouseX;
	a->eyeTargetY = a->mouseY;
	a->gazeReadyAt = now + kGazeEnterDelay;
	a->gazeBlend = 0.f;
	if (a->traceInput || a->drawHitbox)
		std::fprintf(stderr, "input: pointer entered %.0f,%.0f (gaze in %.1fs)\n",
					 a->mouseX, a->mouseY, kGazeEnterDelay);
	(void)serial;
}

static void pointerLeave(void* data, wl_pointer*, uint32_t, wl_surface*) {
	App* a = static_cast<App*>(data);
	a->pointerIn = false;
	a->haveMousePrev = false;
	// A window can take the pointer mid-drag and the release never arrives;
	// without this the gesture stays latched and clicks stop being accepted.
	if (a->mouseDown) {
		a->mouseDown = false;
		if (a->hitboxEdit) {
			a->hitboxDragMask = 0;
			writeHitboxFeedback(*a, true);
		} else {
			releasedMouse(
				*a,
				std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
		}
	}
}

static void pointerMotion(void* data, wl_pointer*, uint32_t, wl_fixed_t sx, wl_fixed_t sy) {
	App* a = static_cast<App*>(data);
	const double x = wl_fixed_to_double(sx);
	const double y = wl_fixed_to_double(sy);
	const double dx = a->haveMousePrev ? x - a->prevMouseX : 0.0;
	const double dy = a->haveMousePrev ? y - a->prevMouseY : 0.0;
	a->prevMouseX = x;
	a->prevMouseY = y;
	a->haveMousePrev = true;
	a->mouseX = x;
	a->mouseY = y;
	const double traceNow =
		std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	if ((a->traceInput || a->drawHitbox) && traceNow >= a->nextPointerTraceAt) {
		std::fprintf(stderr, "input: pointer motion %.0f,%.0f\n", x, y);
		a->nextPointerTraceAt = traceNow + 0.25;
	}
	if (a->hitboxEdit) {
		if (a->mouseDown)
			moveHitboxDrag(*a, x, y);
		return;
	}
	if (!a->mouseDown && a->interactMode == 0 && traceNow >= a->gazeReadyAt)
		beginGazeTracking(*a, x, y);

	// CEF injected a move every frame; a real pointer does not, so only genuine
	// motion counts as "the user is driving" (which pauses --auto-idle).
	if (std::abs(dx) + std::abs(dy) > 0.5) {
		a->userOverride = true;
		a->userOverrideUntil =
			std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() + 4.0;
	}
	movedMouse(*a, x, y, dx, dy);
}

static void pointerButton(void* data, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t state) {
	App* a = static_cast<App*>(data);
	if (button != 0x110) // BTN_LEFT
		return;
	const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	a->mouseDown = state == WL_POINTER_BUTTON_STATE_PRESSED;
	if (a->hitboxEdit) {
		if (a->mouseDown)
			beginHitboxDrag(*a, a->mouseX, a->mouseY);
		else {
			a->hitboxDragMask = 0;
			writeHitboxFeedback(*a, true);
		}
		return;
	}
	a->userOverride = true;
	a->userOverrideUntil = now + 4.0;
	if (a->mouseDown) {
		// Upstream gates presses on `acceptingClick` plus a 500ms settle timer and
		// locks input for the whole voiceline. Ported faithfully that swallowed 42%
		// of real presses ("sometimes it works, sometimes it doesn't"), so the only
		// gate left is a debounce against a doubled press event; a new press
		// otherwise replaces whatever gesture was running.
		const bool blocked = a->lastPressAt >= 0 && (now - a->lastPressAt) < kPressDebounce;
		if (a->traceInput || a->drawHitbox) {
			std::fprintf(stderr, "input: press %.0f,%.0f -> hitbox %.0f,%.0f%s\n", a->mouseX, a->mouseY,
						 toHitboxSpace(*a, a->mouseX, true), toHitboxSpace(*a, a->mouseY, false),
						 blocked ? " [ignored: debounce]" : "");
		}
		if (blocked)
			return;
		a->lastPressAt = now;
		const int zone = pressedMouse(*a, a->mouseX, a->mouseY, now);
		cursorFxPress(*a, a->mouseX, a->mouseY, now, zone);
		if (a->traceInput || a->drawHitbox)
			std::fprintf(stderr, "input: zone=%d mode=%d\n", zone, a->interactMode);
	} else {
		if (a->traceInput || a->drawHitbox)
			std::fprintf(stderr, "input: release mode=%d\n", a->interactMode);
		const int releasedMode = a->interactMode;
		// Gaze is persistent. The old path started LookEnd here and then
		// immediately started Look again, so one release visibly moved Aris's
		// head twice. Workspace changes repeated the same Leave/Enter pair.
		// Keep mode 3 alive until an explicit character action replaces it.
		if (releasedMode != 3)
			releasedMouse(*a, now);
	}
}

static void pointerAxis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {}
static void pointerFrame(void*, wl_pointer*) {}
static void pointerAxisSource(void*, wl_pointer*, uint32_t) {}
static void pointerAxisStop(void*, wl_pointer*, uint32_t, uint32_t) {}
static void pointerAxisDiscrete(void*, wl_pointer*, uint32_t, int32_t) {}
static void pointerAxisValue120(void*, wl_pointer*, uint32_t, int32_t) {}
static void pointerAxisRelativeDirection(void*, wl_pointer*, uint32_t, uint32_t) {}

static const wl_pointer_listener pointerListener = {
	.enter = pointerEnter,
	.leave = pointerLeave,
	.motion = pointerMotion,
	.button = pointerButton,
	.axis = pointerAxis,
	.frame = pointerFrame,
	.axis_source = pointerAxisSource,
	.axis_stop = pointerAxisStop,
	.axis_discrete = pointerAxisDiscrete,
	.axis_value120 = pointerAxisValue120,
	.axis_relative_direction = pointerAxisRelativeDirection,
};

static void seatCapabilities(void* data, wl_seat* seat, uint32_t caps) {
	App* a = static_cast<App*>(data);
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !a->pointer) {
		a->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(a->pointer, &pointerListener, a);
	}
}

static void seatName(void*, wl_seat*, const char*) {}

static bool initEgl(App& a) {
	a.eglDisplay = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(a.display));
	if (a.eglDisplay == EGL_NO_DISPLAY)
		return false;
	if (!eglInitialize(a.eglDisplay, nullptr, nullptr))
		return false;
	EGLint attrs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_NONE};
	EGLint n = 0;
	if (!eglChooseConfig(a.eglDisplay, attrs, &a.eglConfig, 1, &n) || n < 1)
		return false;
	if (!eglBindAPI(EGL_OPENGL_API))
		return false;
	EGLint ctx[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
					EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
	a.eglContext = eglCreateContext(a.eglDisplay, a.eglConfig, EGL_NO_CONTEXT, ctx);
	return a.eglContext != EGL_NO_CONTEXT;
}

// The workshop wallpapers share one template, only the character id differs
// (it differs per item). Discover it instead of hardcoding.
static std::string findSkeletonBase(const std::string& assetDir) {
	DIR* d = opendir(assetDir.c_str());
	if (!d)
		return std::string();
	std::string base;
	while (dirent* e = readdir(d)) {
		const char* dot = strrchr(e->d_name, '.');
		if (!dot || strcmp(dot, ".atlas") != 0)
			continue;
		base.assign(e->d_name, static_cast<size_t>(dot - e->d_name));
		break;
	}
	closedir(d);
	return base;
}

static bool loadSpine(App& a, const std::string& assetDir) {
	// The reference renderer leaves Skeleton.yDown at false and uses a plain
	// y-up ortho2d. Flipping it here put the whole skeleton at negative y, i.e.
	// completely outside the camera window — only the sun flare clipped in.
	GlTextureLoader loader;
	const std::string base = findSkeletonBase(assetDir);
	if (base.empty()) {
		std::fprintf(stderr, "no .atlas found in %s\n", assetDir.c_str());
		return false;
	}
	const std::string atlasPath = assetDir + "/" + base + ".atlas";
	const std::string skelPath = assetDir + "/" + base + ".skel";
	a.atlas = new Atlas(atlasPath.c_str(), &loader);
	if (a.atlas->getPages().size() == 0) {
		std::fprintf(stderr, "failed to load atlas: %s\n", atlasPath.c_str());
		return false;
	}
	SkeletonBinary binary(a.atlas);
	binary.setScale(1.f);
	a.skeletonData = binary.readSkeletonDataFile(skelPath.c_str());
	if (!a.skeletonData) {
		std::fprintf(stderr, "failed to load skel: %s\n", skelPath.c_str());
		return false;
	}
	a.skeleton = new Skeleton(a.skeletonData);
	if (getenv("KEI_DEBUG_BONES")) {
		Vector<BoneData*>& bones = a.skeletonData->getBones();
		for (size_t i = 0; i < bones.size(); ++i) {
			BoneData* bone = bones[i];
			std::fprintf(stderr, "bone %3d %-28s parent=%-24s xy=(%8.2f,%8.2f) rot=%7.2f\n",
						 bone->getIndex(), bone->getName().buffer(),
						 bone->getParent() ? bone->getParent()->getName().buffer() : "-",
						 static_cast<double>(bone->getX()), static_cast<double>(bone->getY()),
						 static_cast<double>(bone->getRotation()));
		}
		Vector<IkConstraintData*>& iks = a.skeletonData->getIkConstraints();
		for (size_t i = 0; i < iks.size(); ++i) {
			IkConstraintData* c = iks[i];
			std::fprintf(stderr, "ik %-28s target=%s bones=", c->getName().buffer(),
						 c->getTarget() ? c->getTarget()->getName().buffer() : "-");
			for (size_t j = 0; j < c->getBones().size(); ++j)
				std::fprintf(stderr, "%s%s", j ? "," : "", c->getBones()[j]->getName().buffer());
			std::fprintf(stderr, "\n");
		}
		Vector<TransformConstraintData*>& transforms = a.skeletonData->getTransformConstraints();
		for (size_t i = 0; i < transforms.size(); ++i) {
			TransformConstraintData* c = transforms[i];
			std::fprintf(stderr, "transform %-21s target=%s bones=", c->getName().buffer(),
						 c->getTarget() ? c->getTarget()->getName().buffer() : "-");
			for (size_t j = 0; j < c->getBones().size(); ++j)
				std::fprintf(stderr, "%s%s", j ? "," : "", c->getBones()[j]->getName().buffer());
			std::fprintf(stderr, "\n");
		}
	}
	for (const std::string& name : a.profile.hiddenSlots) {
		if (Slot* slot = a.skeleton->findSlot(name.c_str()))
			a.hiddenSlotPtrs.push_back(slot);
		else
			std::fprintf(stderr, "profile: hidden slot '%s' not in skeleton\n", name.c_str());
	}
	if (!a.hiddenSlotPtrs.empty())
		std::fprintf(stderr, "profile: hiding %zu item FX slots\n", a.hiddenSlotPtrs.size());
	a.animData = new AnimationStateData(a.skeletonData);
	a.animData->setDefaultMix(0.2f);
	a.animState = new AnimationState(a.animData);
	if (a.skeletonData->findAnimation(a.profile.idleAnim.c_str()))
		a.animState->setAnimation(0, a.profile.idleAnim.c_str(), true);
	else
		std::fprintf(stderr, "profile: idle animation '%s' not in skeleton\n", a.profile.idleAnim.c_str());
	a.touchEye = a.skeleton->findBone(a.profile.eyeBone.c_str());
	a.touchPoint = a.skeleton->findBone(a.profile.pointBone.c_str());
	a.handFollowTarget = a.profile.handFollowBone.empty()
		? nullptr
		: a.skeleton->findBone(a.profile.handFollowBone.c_str());
	if (a.touchEye) {
		a.eyeRestX = a.touchEye->getX();
		a.eyeRestY = a.touchEye->getY();
	}
	if (a.touchPoint) {
		a.pointRestX = a.touchPoint->getX();
		a.pointRestY = a.touchPoint->getY();
	}
	if (!a.profile.handFollowBone.empty() && !a.handFollowTarget)
		std::fprintf(stderr, "profile: hand-follow bone '%s' not in skeleton\n",
					 a.profile.handFollowBone.c_str());
	if (getenv("KEI_DEBUG_ANIMS")) {
		Vector<Animation*>& anims = a.skeletonData->getAnimations();
		for (size_t i = 0; i < anims.size(); ++i)
			std::fprintf(stderr, "anim %-18s %.3fs\n", anims[i]->getName().buffer(), anims[i]->getDuration());
	}
	a.renderer = renderer_create();
	return true;
}

static void hideProfileSlots(App& a) {
	// AnimationState can restore attachments, which is why the workshop's own JS
	// repeats this after every apply rather than doing it just once at load time.
	for (Slot* slot : a.hiddenSlotPtrs)
		slot->setAttachment(nullptr);
}

static void applyHandFollowOffset(App& a) {
	if (!a.handFollowTarget ||
		(std::abs(a.handOffsetX) < 0.01f && std::abs(a.handOffsetY) < 0.01f))
		return;
	a.handBaseLocalX = a.handFollowTarget->getX();
	a.handBaseLocalY = a.handFollowTarget->getY();
	a.handOffsetApplied = true;
	Bone* parent = a.handFollowTarget->getParent();
	if (!parent) {
		a.handFollowTarget->setX(a.handFollowTarget->getX() + a.handOffsetX);
		a.handFollowTarget->setY(a.handFollowTarget->getY() + a.handOffsetY);
		return;
	}
	// Convert the screen-aligned world offset through the target's parent. PC is
	// rotated in Aris's rig, so adding dx/dy directly to local X/Y swaps axes.
	float worldX = 0.f, worldY = 0.f;
	parent->localToWorld(a.handFollowTarget->getX(), a.handFollowTarget->getY(), worldX, worldY);
	float localX = 0.f, localY = 0.f;
	parent->worldToLocal(worldX + a.handOffsetX, worldY + a.handOffsetY, localX, localY);
	a.handFollowTarget->setX(localX);
	a.handFollowTarget->setY(localY);
}

static void restoreHandFollowBase(App& a) {
	if (!a.handFollowTarget || !a.handOffsetApplied)
		return;
	a.handFollowTarget->setX(a.handBaseLocalX);
	a.handFollowTarget->setY(a.handBaseLocalY);
	a.handOffsetApplied = false;
}

static void setCamera(App& a) {
	if (!a.renderer || a.width <= 0 || a.height <= 0)
		return;
	// Faithful port of workshop js/main.js resize(): a 2560×1600 design frame
	// divided by the `scale` property (default 0.8), fitted to the panel aspect,
	// then mvp.ortho2d(centerX - w/2, centerY - h/2, w, h). Y is up.
	const float centerX = 0.f;
	const float centerY = 900.f;
	float viewW = 2560.f / a.modelScale;
	float viewH = 1600.f / a.modelScale;
	const float wr = a.width / 2560.f;
	const float hr = a.height / 1600.f;
	if (wr < hr) {
		viewW = viewH * (a.width / static_cast<float>(a.height));
		a.transpose = 1600.f / a.height;
	} else if (wr > hr) {
		viewH = viewW * (a.height / static_cast<float>(a.width));
		a.transpose = 2560.f / a.width;
	} else {
		a.transpose = 1600.f / a.height;
	}

	float matrix[16];
	memset(matrix, 0, sizeof(matrix));
	const float left = centerX - viewW * 0.5f;
	const float right = centerX + viewW * 0.5f;
	const float bottom = centerY - viewH * 0.5f;
	const float top = centerY + viewH * 0.5f;
	matrix[0] = 2.f / (right - left);
	matrix[5] = 2.f / (top - bottom);
	matrix[10] = -1.f;
	matrix[12] = -(right + left) / (right - left);
	matrix[13] = -(top + bottom) / (top - bottom);
	matrix[15] = 1.f;
	shader_use(a.renderer->shader);
	shader_set_matrix4(a.renderer->shader, "uMatrix", matrix);
}

// Upstream trackMouse(): steps the bone toward the cursor and clamps, rather
// than snapping. The clamp is what fixes the steady state; the step is the ramp.
static void trackMouseStep(App& a) {
	if (!a.touchEye)
		return;
	const float blend = std::min(1.f, std::max(0.f, a.gazeBlend));
	if (blend <= 0.f)
		return;
	const float adjX = static_cast<float>(a.eyeTargetX / std::max(1, a.width) - 0.5);
	const float adjY = static_cast<float>(a.eyeTargetY / std::max(1, a.height) - 0.5);
	const float signX = (adjX > 0.f) - (adjX < 0.f);
	const float signY = (adjY > 0.f) - (adjY < 0.f);
	const float step = kEyeStep * blend;
	float ey = a.touchEye->getY() - signX * step;
	float ex = a.touchEye->getX() - signY * step;
	// The allowed range grows with the blend as well as the per-tick step. This
	// makes the first movement additive instead of snapping to a distant clamp.
	const float spanY = std::abs(adjX) * kEyeClampX * blend;
	const float spanX = std::abs(adjY) * kEyeClampY * blend;
	a.touchEye->setY(std::min(std::max(ey, a.eyeRestY - spanY), a.eyeRestY + spanY));
	a.touchEye->setX(std::min(std::max(ex, a.eyeRestX - spanX), a.eyeRestX + spanX));
}

// Upstream untrackMouse(): ease the eye home. Its 500ms "then re-arm clicks"
// timer is dropped — a press during the ease just starts the next gesture.
static void untrackStep(App& a) {
	if (!a.touchEye) {
		a.untracking = false;
		return;
	}
	const float dy = a.touchEye->getY() - a.eyeRestY;
	const float dx = a.touchEye->getX() - a.eyeRestX;
	if (std::abs(dy) <= kEyeStep && std::abs(dx) <= kEyeStep) {
		a.touchEye->setY(a.eyeRestY);
		a.touchEye->setX(a.eyeRestX);
		a.untracking = false;
		return;
	}
	a.touchEye->setY(a.touchEye->getY() - (dy > 0 ? kEyeStep : -kEyeStep));
	a.touchEye->setX(a.touchEye->getX() - (dx > 0 ? kEyeStep : -kEyeStep));
}

// Release ease. Upstream jumps ±5 every 20ms; that reads as a staircase on the
// way home too, so this eases with the same curve the stroke uses.
static void unpetEase(App& a, float dt) {
	if (!a.touchPoint) {
		a.unpetting = false;
		return;
	}
	const float k = std::min(1.f, dt * kHeadpatFollowRate);
	a.touchPoint->setY(a.touchPoint->getY() + (a.pointRestY - a.touchPoint->getY()) * k);
	a.touchPoint->setX(a.touchPoint->getX() + (a.pointRestX - a.touchPoint->getX()) * k);
	if (std::abs(a.touchPoint->getY() - a.pointRestY) <= 0.5f &&
		std::abs(a.touchPoint->getX() - a.pointRestX) <= 0.5f) {
		a.touchPoint->setY(a.pointRestY);
		a.touchPoint->setX(a.pointRestX);
		a.unpetting = false;
	}
}

// Map the 0..1 pinch amount onto the two held tracks. Their durations differ,
// so normalise per track or one of them lands short of its end at 1.
static void applyPinchScrub(App& a) {
	if (!a.animState)
		return;
	const float amount = std::min(1.f, std::max(0.f, a.pinchAmount));
	for (size_t track = 1; track <= 2; ++track) {
		TrackEntry* e = a.animState->getCurrent(track);
		if (!e || !e->getAnimation())
			continue;
		const float dur = e->getAnimation()->getDuration();
		if (dur <= 0.f)
			continue;
		// Exactly at the end marks a non-looping entry complete, so stop just shy.
		e->setTrackTime(std::min(amount * dur, dur - 1e-3f));
	}
}

static void updateInteraction(App& a, float dt, double now) {
	refreshRuntimeControl(a, now);

	if (a.userOverride && now > a.userOverrideUntil)
		a.userOverride = false;

	if (a.voicePlaying && now >= a.voiceUntil) {
		a.voicePlaying = false;
		a.voicePids.clear();
		const int count = std::max<int>(1, static_cast<int>(a.profile.voicelines.size()));
		a.voiceline = (a.voiceline >= count) ? 1 : a.voiceline + 1;
	}

	for (size_t i = 0; i < a.pendingAudio.size();) {
		if (now >= a.pendingAudio[i].at) {
			if (!a.audioMuted)
				a.voicePids.push_back(spawnVoiceClip(a, a.pendingAudio[i].path, a.voiceVolume));
			a.pendingAudio.erase(a.pendingAudio.begin() + static_cast<long>(i));
		} else {
			++i;
		}
	}

	// While the cheek is held: creep toward a pinch when the pointer is still,
	// follow the drag when it is not. Ease toward the value to kill stutter.
	if (a.interactMode == 4) {
		if (!a.pinchDragged)
			a.pinchTarget = std::min(1.f, a.pinchTarget + static_cast<float>(dt / kPinchHoldSeconds));
		const float k = std::min(1.f, dt * kPinchFollow);
		a.pinchAmount += (a.pinchTarget - a.pinchAmount) * k;
		applyPinchScrub(a);
		if (a.traceFx)
			std::fprintf(stderr, "pinch: amount %.2f (target %.2f, drag=%d)\n",
						 static_cast<double>(a.pinchAmount), static_cast<double>(a.pinchTarget),
						 a.pinchDragged ? 1 : 0);
	}

	// Ease both toward the held target and back to zero after release. This keeps
	// the constrained hand from jumping when the pointer or button state changes.
	if (a.handFollowTarget) {
		float rate = a.profile.handFollowReturnRate;
		if (a.interactMode == 5) {
			// Find the intersections of the current direction with the inner and
			// outer ellipses. One radial value drives easing, so diagonal motion
			// has exactly the same slowdown as horizontal/vertical motion.
			const float radius = std::hypot(a.handOffsetX, a.handOffsetY);
			float edge = 0.f;
			if (radius > 0.001f) {
				const float ux = a.handOffsetX / radius;
				const float uy = a.handOffsetY / radius;
				auto ellipseRadius = [ux, uy](float rangeX, float rangeY) {
					rangeX = std::max(0.01f, rangeX);
					rangeY = std::max(0.01f, rangeY);
					return 1.f / std::sqrt((ux * ux) / (rangeX * rangeX) +
										  (uy * uy) / (rangeY * rangeY));
				};
				const float innerRadius =
					ellipseRadius(a.profile.handFollowRangeX, a.profile.handFollowRangeY);
				const float outerRadius =
					ellipseRadius(a.profile.handFollowOuterRangeX,
								  a.profile.handFollowOuterRangeY);
				if (outerRadius <= innerRadius + 0.01f)
					edge = radius >= innerRadius ? 1.f : 0.f;
				else
					edge = std::min(1.f, std::max(0.f,
						(radius - innerRadius) / (outerRadius - innerRadius)));
			}
			// Smoothstep removes the speed kink exactly where the inner ellipse ends.
			edge = edge * edge * (3.f - 2.f * edge);
			rate = a.profile.handFollowRate +
				   (a.profile.handFollowEdgeRate - a.profile.handFollowRate) * edge;
		}
		const float k = std::min(1.f, dt * rate);
		a.handOffsetX += (a.handTargetX - a.handOffsetX) * k;
		a.handOffsetY += (a.handTargetY - a.handOffsetY) * k;
	}

	// BGM loops by respawning the player. SIGCHLD is ignored so the child is
	// reaped for us, which makes kill(pid, 0) a valid liveness probe.
	if (a.bgmReadyAt < 0)
		a.bgmReadyAt = now + 15.0;
	if (!a.audioMuted && now >= a.bgmReadyAt && a.bgmVolume > 0.f && !a.audioDir.empty() &&
		!a.profile.bgmFile.empty() && now >= a.bgmCheckAt) {
		a.bgmCheckAt = now + 1.0;
		if (a.bgmPid <= 0 || kill(a.bgmPid, 0) != 0) {
			const std::string path = a.audioDir + "/" + a.profile.bgmFile;
			if (access(path.c_str(), R_OK) == 0)
				a.bgmPid = spawnVoiceClip(a, path, a.bgmVolume);
		}
	}

	// Auto idle (eye wander + occasional self-pat) is OFF by default: it was a
	// stand-in for interaction back when the CEF path was too slow to touch, and
	// a wallpaper that moves on its own reads as a glitch. `--auto-idle` restores it.
	const bool autoOwnsEye =
		a.autoIdle && !a.userOverride && a.interactMode == 0 && !a.untracking && !reactionOwnsTracks(a);
	if (autoOwnsEye && a.touchEye) {
		a.autoAngle += dt * 0.7f;
		// Idle wander over a third of the reference range.
		a.touchEye->setY(a.eyeRestY - std::cos(a.autoAngle) * kEyeClampX * 0.3f);
		a.touchEye->setX(a.eyeRestX - std::sin(a.autoAngle * 0.73f) * kEyeClampY * 0.3f);
	}

	if (a.autoIdle && !a.userOverride && a.interactMode == 0 && !reactionOwnsTracks(a)) {
		if (a.lastAutoPat == 0)
			a.lastAutoPat = now;
		if (now - a.lastAutoPat > a.nextAutoPatIn) {
			a.lastAutoPat = now;
			a.nextAutoPatIn = 18.0 + (std::rand() % 20);
			setAnim(a, 1, a.profile.patAnim.c_str(), false);
			a.patEndAt = now + 1.0;
		}
	}

	if (a.patEndAt >= 0 && now >= a.patEndAt) {
		a.patEndAt = -1;
		if (TrackEntry* e = setAnim(a, 1, a.profile.patEndAnim.c_str(), false))
			e->setMixDuration(kGestureMix);
		setAnim(a, 2, a.profile.patEndAnimA.c_str(), false);
		if (a.animState) {
			// delay=0 queues after the preceding animation's real completion.
			// A fixed one second cut long PatEnd animations off on other characters.
			a.animState->addEmptyAnimation(1, 0.5f, 0.f);
			a.animState->addEmptyAnimation(2, 0.5f, 0.f);
		}
	}
	if (a.handFollowReleasePending && a.animState) {
		TrackEntry* hand = a.animState->getCurrent(1);
		if (!hand || !currentAnimIs(a, 1, a.profile.handFollowAnim)) {
			// Another explicit action replaced HandFollow; never inject a stale End.
			a.handFollowReleasePending = false;
		} else if (hand->isComplete()) {
			a.handFollowReleasePending = false;
			if (TrackEntry* e = setAnim(a, 1, a.profile.handFollowEndAnim.c_str(), false)) {
				e->setMixDuration(kGestureMix);
				// addEmptyAnimation(delay=0) starts its fade `mixDuration`
				// before the preceding animation ends. For this character that
				// overrides the authored last frames of HandFollowEnd and makes
				// the head kick for roughly four displayed frames. Start the
				// fade at the actual end instead; no character-specific timer.
				const float endAt =
					e->getAnimation() ? e->getAnimation()->getDuration() : 0.f;
				a.animState->addEmptyAnimation(1, 0.5f, endAt);
			} else {
				a.animState->setEmptyAnimation(1, 0.5f);
			}
		}
	}

	// If the pointer stayed perfectly still while a reaction finished there is no
	// new Wayland motion event to restart persistent gaze. Re-evaluate once per
	// frame; beginGazeTracking is a no-op until the gesture has released its tracks.
	if (a.pointerIn && !a.mouseDown && a.interactMode == 0 && now >= a.gazeReadyAt)
		beginGazeTracking(a, a.mouseX, a.mouseY);

	if (a.interactMode == 3 && a.pointerIn && now >= a.gazeReadyAt)
		a.gazeBlend = std::min(1.f, a.gazeBlend + dt / kGazeBlendSeconds);

	// Petting eases per frame (not on the 20ms grid) so the stroke stays smooth at
	// 100Hz and keeps moving between pointer events.
	if (a.interactMode == 1 && a.touchPoint) {
		const float k = std::min(1.f, dt * kHeadpatFollowRate);
		const float y = a.touchPoint->getY();
		a.touchPoint->setY(y + (a.patTargetY - y) * k);
	}
	if (a.unpetting)
		unpetEase(a, dt);

	// Everything below is driven by upstream's 20ms intervals, so run it on a
	// fixed step instead of per-frame — otherwise 100Hz moves the bones 1.7x fast.
	a.stepAccum += dt;
	int steps = 0;
	while (a.stepAccum >= kStepInterval && steps < 8) {
		a.stepAccum -= kStepInterval;
		++steps;
		if (a.interactMode == 3 && a.pointerIn && now >= a.gazeReadyAt)
			trackMouseStep(a);
		if (a.untracking)
			untrackStep(a);
	}
}

// Debug overlay: the upstream `drawHitboxes` option, ported so a "missing"
// zone is visible instead of guessed at. Enable with --hitbox-debug.
static void drawHitboxOverlay(App& a, int viewportW, int viewportH) {
	if (!a.drawHitbox)
		return;
	static shader_t prog = 0;
	static GLuint vao = 0, vbo = 0;
	if (!prog) {
		prog = shader_create("#version 330 core\n"
							 "layout(location = 0) in vec2 aPos;\n"
							 "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n",
							 "#version 330 core\n"
							 "uniform vec4 uColor;\n"
							 "out vec4 fragColor;\n"
							 "void main() { fragColor = uColor; }\n");
		glGenVertexArrays(1, &vao);
		glGenBuffers(1, &vbo);
		glBindVertexArray(vao);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 8, nullptr, GL_DYNAMIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 2, nullptr);
	}
	if (!prog)
		return;

	shader_use(prog);
	const GLint colorLoc = glGetUniformLocation(prog, "uColor");
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	const Hitbox* boxes[] = {&a.profile.headpat, &a.profile.pinch, &a.profile.voiceline,
							 &a.profile.handFollow};
	for (int boxIndex = 0; boxIndex < 4; ++boxIndex) {
		const Hitbox* box = boxes[boxIndex];
		if (!box->enabled())
			continue;
		const float x0 = toScreenSpace(a, box->xMin, true) / std::max(1, a.width) * 2.f - 1.f;
		const float x1 = toScreenSpace(a, box->xMax, true) / std::max(1, a.width) * 2.f - 1.f;
		// Design space is y-down, GL clip space is y-up.
		const float y0 = 1.f - toScreenSpace(a, box->yMin, false) / std::max(1, a.height) * 2.f;
		const float y1 = 1.f - toScreenSpace(a, box->yMax, false) / std::max(1, a.height) * 2.f;
		const float quad[8] = {x0, y0, x1, y0, x1, y1, x0, y1};
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
		const bool selected = a.hitboxEdit && a.hitboxEditZone == boxIndex;
		glUniform4f(colorLoc, box->r, box->g, box->b, selected ? 0.34f : 0.20f);
		glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
		glLineWidth(selected ? 4.f : 2.f);
		glUniform4f(colorLoc, box->r, box->g, box->b, 0.9f);
		glDrawArrays(GL_LINE_LOOP, 0, 4);
		if (a.hitboxEdit) {
			// Four solid corner handles make it clear that the overlay itself is
			// draggable. The whole interior moves; borders/corners resize.
			const float halfX = 7.f / std::max(1, a.width) * 2.f;
			const float halfY = 7.f / std::max(1, a.height) * 2.f;
			const float corners[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
			for (const auto& corner : corners) {
				const float handle[8] = {
					corner[0] - halfX, corner[1] - halfY,
					corner[0] + halfX, corner[1] - halfY,
					corner[0] + halfX, corner[1] + halfY,
					corner[0] - halfX, corner[1] + halfY,
				};
				glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(handle), handle);
				glUniform4f(colorLoc, box->r, box->g, box->b, 1.f);
				glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
			}
		}

		// Stable 1..4 labels connect the overlay to the compact inspector. A
		// seven-segment glyph avoids pulling a font renderer into the GL host.
		const float badgeX0 = x0 + 5.f / std::max(1, a.width) * 2.f;
		const float badgeY0 = y0 - 5.f / std::max(1, a.height) * 2.f;
		const float badgeW = 24.f / std::max(1, a.width) * 2.f;
		const float badgeH = 28.f / std::max(1, a.height) * 2.f;
		const float badge[8] = {
			badgeX0, badgeY0, badgeX0 + badgeW, badgeY0,
			badgeX0 + badgeW, badgeY0 - badgeH, badgeX0, badgeY0 - badgeH,
		};
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(badge), badge);
		glUniform4f(colorLoc, 0.02f, 0.025f, 0.035f, 0.86f);
		glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
		static const unsigned char digitSegments[4] = {
			(1u << 1) | (1u << 2),
			(1u << 0) | (1u << 1) | (1u << 6) | (1u << 4) | (1u << 3),
			(1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 6),
			(1u << 5) | (1u << 6) | (1u << 1) | (1u << 2),
		};
		const float px = badgeX0 + 8.f / std::max(1, a.width) * 2.f;
		const float py = badgeY0 - 5.f / std::max(1, a.height) * 2.f;
		const float dw = 8.f / std::max(1, a.width) * 2.f;
		const float dh = 17.f / std::max(1, a.height) * 2.f;
		const float segment[7][4] = {
			{px, py, px + dw, py},
			{px + dw, py, px + dw, py - dh * 0.5f},
			{px + dw, py - dh * 0.5f, px + dw, py - dh},
			{px, py - dh, px + dw, py - dh},
			{px, py - dh * 0.5f, px, py - dh},
			{px, py, px, py - dh * 0.5f},
			{px, py - dh * 0.5f, px + dw, py - dh * 0.5f},
		};
		float lines[28];
		int lineCount = 0;
		for (int segmentIndex = 0; segmentIndex < 7; ++segmentIndex) {
			if (!(digitSegments[boxIndex] & (1u << segmentIndex)))
				continue;
			for (int valueIndex = 0; valueIndex < 4; ++valueIndex)
				lines[lineCount++] = segment[segmentIndex][valueIndex];
		}
		glBufferData(GL_ARRAY_BUFFER, sizeof(float) * lineCount, lines, GL_DYNAMIC_DRAW);
		glLineWidth(3.f);
		glUniform4f(colorLoc, selected ? 1.f : box->r, selected ? 0.94f : box->g,
					selected ? 0.25f : box->b, 1.f);
		glDrawArrays(GL_LINES, 0, lineCount / 2);
	}

	// Hand-follow uses a second coordinate system: the magenta input rectangle
	// above says where the gesture can start, while the cyan ellipse is the
	// normal-speed range. The larger orange ellipse is the soft-limit ring:
	// follow speed falls toward its border. The centre cross is
	// the authored rest position; the bright square is the live target.
	if (a.handFollowTarget && a.profile.handFollowRangeX > 0.f &&
		a.profile.handFollowRangeY > 0.f) {
		const float currentWorldX = a.handFollowTarget->getWorldX();
		const float currentWorldY = a.handFollowTarget->getWorldY();
		const float restWorldX = currentWorldX - a.handOffsetX;
		const float restWorldY = currentWorldY - a.handOffsetY;
		float restX, restY, currentX, currentY;
		modelWorldToScreen(a, restWorldX, restWorldY, restX, restY);
		modelWorldToScreen(a, currentWorldX, currentWorldY, currentX, currentY);
		auto clipX = [&](float px) {
			return px / std::max(1, a.width) * 2.f - 1.f;
		};
		auto clipY = [&](float py) {
			return 1.f - py / std::max(1, a.height) * 2.f;
		};
		auto drawMotionEllipse = [&](float rangeX, float rangeY, float r, float g,
									 float b, float alpha, float width) {
			constexpr int kSegments = 64;
			float ellipse[kSegments * 2];
			for (int i = 0; i < kSegments; ++i) {
				const float angle =
					6.28318530718f * static_cast<float>(i) / static_cast<float>(kSegments);
				float screenX, screenY;
				modelWorldToScreen(a, restWorldX + std::cos(angle) * rangeX,
								   restWorldY + std::sin(angle) * rangeY,
								   screenX, screenY);
				ellipse[i * 2] = clipX(screenX);
				ellipse[i * 2 + 1] = clipY(screenY);
			}
			glBufferData(GL_ARRAY_BUFFER, sizeof(ellipse), ellipse, GL_DYNAMIC_DRAW);
			glLineWidth(width);
			glUniform4f(colorLoc, r, g, b, alpha);
			glDrawArrays(GL_LINE_LOOP, 0, kSegments);
		};
		drawMotionEllipse(a.profile.handFollowOuterRangeX,
						  a.profile.handFollowOuterRangeY,
						  1.f, 0.52f, 0.18f, 0.9f, 3.f);
		drawMotionEllipse(a.profile.handFollowRangeX,
						  a.profile.handFollowRangeY,
						  0.15f, 0.9f, 1.f, 0.95f, 2.f);

		const float crossHalfX = 8.f / std::max(1, a.width) * 2.f;
		const float crossHalfY = 8.f / std::max(1, a.height) * 2.f;
		const float cross[8] = {
			clipX(restX) - crossHalfX, clipY(restY),
			clipX(restX) + crossHalfX, clipY(restY),
			clipX(restX), clipY(restY) - crossHalfY,
			clipX(restX), clipY(restY) + crossHalfY,
		};
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(cross), cross);
		glUniform4f(colorLoc, 0.15f, 0.9f, 1.f, 1.f);
		glDrawArrays(GL_LINES, 0, 4);

		const float markerHalfX = 5.f / std::max(1, a.width) * 2.f;
		const float markerHalfY = 5.f / std::max(1, a.height) * 2.f;
		const float marker[8] = {
			clipX(currentX) - markerHalfX, clipY(currentY) - markerHalfY,
			clipX(currentX) + markerHalfX, clipY(currentY) - markerHalfY,
			clipX(currentX) + markerHalfX, clipY(currentY) + markerHalfY,
			clipX(currentX) - markerHalfX, clipY(currentY) + markerHalfY,
		};
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(marker), marker);
		glUniform4f(colorLoc, 1.f, 0.95f, 0.2f, 1.f);
		glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	}

	glLineWidth(1.f);
	glBindVertexArray(0);
	(void)viewportW;
	(void)viewportH;
}

static float fxRandom01(App& a) {
	// Deterministic xorshift: a click effect does not need <random>'s heavier
	// machinery, but particles must not all leave in the same direction.
	uint32_t x = a.fxRng;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	a.fxRng = x;
	return static_cast<float>(x & 0x00ffffffu) / static_cast<float>(0x01000000u);
}

static float fxRandomRange(App& a, float low, float high) {
	return low + (high - low) * fxRandom01(a);
}

// Native port of the item's `js/fireworks.js`. The Windows build gets this for
// free by running the HTML; this host runs no JS, so it has to be explicit.
static void cursorFxPress(App& a, double x, double y, double now, int zone) {
	if (!a.cursorFx)
		return;
	(void)zone;
	App::Ripple rp;
	rp.x = x;
	rp.y = y;
	rp.at = now;
	rp.duration = a.profile.clickFxDuration * fxRandomRange(a, 0.8f, 1.2f);
	rp.radiusTo = a.profile.clickFxRadius * fxRandomRange(a, 0.5f, 1.f);
	rp.r = std::min(1.f, a.profile.clickFxR * 0.75f + 0.25f);
	rp.g = std::min(1.f, a.profile.clickFxG * 0.75f + 0.25f);
	rp.b = std::min(1.f, a.profile.clickFxB * 0.75f + 0.25f);
	a.ripples.push_back(rp);
	if (a.ripples.size() > 12)
		a.ripples.erase(a.ripples.begin());

	const float base[3] = {
		a.profile.clickFxR, a.profile.clickFxG, a.profile.clickFxB
	};
	const float palette[3][3] = {
		{base[0], base[1], base[2]},
		{base[0] * 0.65f + 0.27f, base[1] * 0.65f + 0.27f,
		 base[2] * 0.65f + 0.27f},
		{base[0] * 0.65f + 0.35f, base[1] * 0.65f + 0.35f,
		 base[2] * 0.65f + 0.35f},
	};
	for (int i = 0; i < a.profile.clickFxParticles; ++i) {
		App::TapParticle p;
		p.x = x;
		p.y = y;
		p.at = now;
		p.duration = a.profile.clickFxDuration * fxRandomRange(a, 0.6f, 1.f);
		const float scale = a.profile.clickFxRadius / 100.f;
		p.radius = fxRandomRange(a, 10.f, 20.f) * scale;
		p.angle = fxRandomRange(a, 0.f, 6.2831853f);
		const float direction = fxRandomRange(a, 0.f, 6.2831853f);
		const float distance = a.profile.clickFxRadius * fxRandomRange(a, 0.5f, 1.f);
		p.endX = x + std::cos(direction) * distance;
		p.endY = y + std::sin(direction) * distance;
		const int color = std::min(2, static_cast<int>(fxRandom01(a) * 3.f));
		p.r = palette[color][0];
		p.g = palette[color][1];
		p.b = palette[color][2];
		p.alpha = fxRandomRange(a, 0.2f, 0.8f);
		a.tapParticles.push_back(p);
	}
	const size_t particleLimit =
		static_cast<size_t>(std::max(20, a.profile.clickFxParticles * 6));
	if (a.tapParticles.size() > particleLimit) {
		const size_t overflow = a.tapParticles.size() - particleLimit;
		a.tapParticles.erase(a.tapParticles.begin(),
							 a.tapParticles.begin() + static_cast<long>(overflow));
	}
}

// Draw a screen-pixel circle in NDC. NDC is not square, so divide per axis.
static void fxCircle(int w, int h, GLint colorLoc, double cx, double cy, float radius, float r, float g,
					 float b, float alpha, bool filled) {
	if (radius <= 0.f || alpha <= 0.003f)
		return;
	constexpr int kSegments = 28;
	float verts[(kSegments + 2) * 2];
	int n = 0;
	if (filled) {
		verts[n++] = static_cast<float>(cx / w * 2.0 - 1.0);
		verts[n++] = static_cast<float>(1.0 - cy / h * 2.0);
	}
	for (int i = 0; i <= kSegments; ++i) {
		const float t = 6.2831853f * static_cast<float>(i) / kSegments;
		const double px = cx + std::cos(t) * radius;
		const double py = cy + std::sin(t) * radius;
		verts[n++] = static_cast<float>(px / w * 2.0 - 1.0);
		verts[n++] = static_cast<float>(1.0 - py / h * 2.0);
	}
	glBufferData(GL_ARRAY_BUFFER, sizeof(float) * static_cast<size_t>(n), verts, GL_DYNAMIC_DRAW);
	glUniform4f(colorLoc, r, g, b, alpha);
	glDrawArrays(filled ? GL_TRIANGLE_FAN : GL_LINE_STRIP, 0, n / 2);
}

static void fxTriangle(int w, int h, GLint colorLoc, const double px[3], const double py[3], float r, float g,
					   float b, float alpha) {
	float verts[6];
	for (int i = 0; i < 3; ++i) {
		verts[i * 2] = static_cast<float>(px[i] / w * 2.0 - 1.0);
		verts[i * 2 + 1] = static_cast<float>(1.0 - py[i] / h * 2.0);
	}
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
	glUniform4f(colorLoc, r, g, b, alpha);
	glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void drawCursorFx(App& a, double now) {
	if (!a.cursorFx)
		return;
	// Drop the item's click bursts that have finished.
	a.ripples.erase(std::remove_if(a.ripples.begin(), a.ripples.end(),
								   [&](const App::Ripple& r) { return now - r.at > r.duration; }),
					a.ripples.end());
	a.tapParticles.erase(std::remove_if(a.tapParticles.begin(), a.tapParticles.end(),
									   [&](const App::TapParticle& p) { return now - p.at > p.duration; }),
						 a.tapParticles.end());

	static shader_t prog = 0;
	static GLuint vao = 0, vbo = 0;
	if (!prog) {
		prog = shader_create("#version 330 core\n"
							 "layout(location = 0) in vec2 aPos;\n"
							 "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n",
							 "#version 330 core\n"
							 "uniform vec4 uColor;\n"
							 "out vec4 fragColor;\n"
							 "void main() { fragColor = uColor; }\n");
		glGenVertexArrays(1, &vao);
		glGenBuffers(1, &vbo);
		glBindVertexArray(vao);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 2, nullptr);
	}
	if (!prog)
		return;
	// Compile/create the tiny FX pipeline on the first ordinary render, before
	// the first click. Lazy creation here used to add a one-off hitch exactly
	// when the user first touched the wallpaper.
	if (a.ripples.empty() && a.tapParticles.empty())
		return;
	if (a.traceFx)
		std::fprintf(stderr, "fx: ripples=%zu particles=%zu size=%dx%d\n", a.ripples.size(),
					 a.tapParticles.size(), a.width, a.height);

	const int w = std::max(1, a.width);
	const int h = std::max(1, a.height);
	shader_use(prog);
	const GLint colorLoc = glGetUniformLocation(prog, "uColor");
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glEnable(GL_BLEND);
	glLineWidth(2.f);

	// The item's triangular burst. Same shape as anime.js easeOutExpo: a large
	// initial throw that settles quietly over the remaining time.
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	for (const App::TapParticle& p : a.tapParticles) {
		const float t = static_cast<float>((now - p.at) / p.duration);
		if (t < 0.f || t > 1.f)
			continue;
		const float ease = t >= 1.f ? 1.f : 1.f - std::pow(2.f, -10.f * t);
		const double cx = p.x + (p.endX - p.x) * ease;
		const double cy = p.y + (p.endY - p.y) * ease;
		const double radius = p.radius * (1.f - ease);
		if (radius < 0.25)
			continue;
		const double c = std::cos(p.angle);
		const double s = std::sin(p.angle);
		const double localX[3] = {0.0, radius * 0.8660254, -radius * 0.8660254};
		const double localY[3] = {-radius, radius * 0.5, radius * 0.5};
		double px[3], py[3];
		for (int i = 0; i < 3; ++i) {
			px[i] = cx + localX[i] * c - localY[i] * s;
			py[i] = cy + localX[i] * s + localY[i] * c;
		}
		fxTriangle(w, h, colorLoc, px, py, p.r, p.g, p.b, p.alpha * (1.f - t));
	}

	// The growing ring from the same JS. The click point is shown with the
	// item's own effect; hit-zone colours are not mixed in.
	for (const App::Ripple& rp : a.ripples) {
		const float t = static_cast<float>((now - rp.at) / rp.duration);
		if (t < 0.f || t > 1.f)
			continue;
		const float ease = t >= 1.f ? 1.f : 1.f - std::pow(2.f, -10.f * t);
		const float radius = 0.1f + (rp.radiusTo - 0.1f) * ease;
		const float alpha = 0.5f * (1.f - t);
		glLineWidth(std::max(1.f, 6.f * (1.f - ease)));
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		fxCircle(w, h, colorLoc, rp.x, rp.y, radius + 1.5f, 0.f, 0.f, 0.08f, alpha * 0.45f, false);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
		fxCircle(w, h, colorLoc, rp.x, rp.y, radius, rp.r, rp.g, rp.b, alpha, false);
	}
	glLineWidth(2.f);

	glLineWidth(1.f);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBindVertexArray(0);
}

// Fixes "the first touch after hours of idling freezes".
// spine 4.2's PhysicsConstraint integrates `skeleton.time - lastTime` in 1/60s
// steps, but returns at the top of the function while mix == 0 -- so lastTime
// never advances. Items commonly keep every physics constraint at mix 0 during
// idle (55 of them in the item measured here), so the moment a pat/pinch
// animation raises mix from 0 to 1, a single frame runs
// `idle seconds / (1/60) * 55` steps. Measured at 0.87ms of freeze per idle
// second (10 min -> 0.63s, 1 h -> 3.3s, 6 h -> 18.6s). While mix == 0 physics
// contributes nothing visually, so dropping the backlog leaves the image
// unchanged.
static void clampPhysicsCatchUp(App& a) {
	if (!a.skeleton)
		return;
	spine::Vector<spine::PhysicsConstraint*>& pcs = a.skeleton->getPhysicsConstraints();
	const float t = a.skeleton->getTime();
	for (size_t i = 0; i < pcs.size(); ++i) {
		spine::PhysicsConstraint* pc = pcs[i];
		const float maxLag = 4.f * std::max(1.f / 240.f, pc->getData().getStep());
		if (t - pc->getLastTime() > maxLag)
			pc->setLastTime(t - maxLag);
		if (pc->getRemaining() > maxLag)
			pc->setRemaining(maxLag);
	}
}

static void renderFrame(App& a, float dt, double now) {
	if (!a.configured || !a.skeleton || !a.renderer)
		return;

	using clock = std::chrono::steady_clock;
	const auto tFrame0 = clock::now();
	auto since = [&](clock::time_point from) {
		return std::chrono::duration<double, std::milli>(clock::now() - from).count();
	};

	eglMakeCurrent(a.eglDisplay, a.eglSurface, a.eglSurface, a.eglContext);
	updateInteraction(a, dt, now);
	const double msInteract = a.traceFrame ? since(tFrame0) : 0.0;

	const auto tStateUpdate = clock::now();
	restoreHandFollowBase(a);
	a.animState->update(dt);
	const double msStateUpdate = a.traceFrame ? since(tStateUpdate) : 0.0;
	const auto tApply = clock::now();
	a.animState->apply(*a.skeleton);
	hideProfileSlots(a);
	applyHandFollowOffset(a);
	const double msApply = a.traceFrame ? since(tApply) : 0.0;
	const auto tSkeletonUpdate = clock::now();
	a.skeleton->update(dt);
	clampPhysicsCatchUp(a);
	const double msSkeletonUpdate = a.traceFrame ? since(tSkeletonUpdate) : 0.0;
	const auto tWorld = clock::now();
	a.skeleton->updateWorldTransform(Physics_Update);
	const double msWorld = a.traceFrame ? since(tWorld) : 0.0;
	const double msAnim = msStateUpdate + msApply + msSkeletonUpdate + msWorld;
	const auto tDraw = clock::now();

	glViewport(0, 0, a.width * a.scale, a.height * a.scale);
	// project.json schemecolor = "0 0 0"; the reference clears to that with alpha 1.
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glClear(GL_COLOR_BUFFER_BIT);
	// The web canvas is created with `alpha: false`. Multiply/Screen slots would
	// otherwise drag the buffer alpha below 1 and let the compositor blend us.
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
	setCamera(a);
	renderer_draw(a.renderer, a.skeleton, false);
	drawCursorFx(a, now);
	drawHitboxOverlay(a, a.width * a.scale, a.height * a.scale);

	// Request the next frame BEFORE swap — arming after swap misses every
	// other vsync on 100Hz panels (effective lock at 50fps).
	if (a.frameCallback) {
		wl_callback_destroy(a.frameCallback);
		a.frameCallback = nullptr;
	}
	a.frameCallback = wl_surface_frame(a.surface);
	wl_callback_add_listener(a.frameCallback, &frameListener, &a);

	const double msDraw = a.traceFrame ? since(tDraw) : 0.0;
	const auto tSwap = clock::now();
	eglSwapBuffers(a.eglDisplay, a.eglSurface);
	wl_surface_set_buffer_scale(a.surface, a.scale);
	wl_surface_damage_buffer(a.surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(a.surface);

	if (a.traceFrame) {
		const double msSwap = since(tSwap);
		const double msTotal = since(tFrame0);
		if (msTotal >= a.traceFrameMs)
			std::fprintf(stderr,
						 "frame %.1fms (interact %.1f / anim %.1f [state %.1f apply %.1f skeleton %.1f "
						 "world %.1f] / draw %.1f / swap %.1f)\n",
						 msTotal, msInteract, msAnim, msStateUpdate, msApply, msSkeletonUpdate, msWorld,
						 msDraw, msSwap);
	}

	const auto t = clock::now();
	if (a.presentWindowStart.time_since_epoch().count() == 0)
		a.presentWindowStart = t;
	a.presentCount++;
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t - a.presentWindowStart).count();
	if (ms >= 500) {
		a.presentFps = (a.presentCount * 1000.f) / static_cast<float>(ms);
		a.presentCount = 0;
		a.presentWindowStart = t;
		static int n = 0;
		if ((++n % 4) == 0)
			std::fprintf(stderr, "wallpaper-host present fps: %.1f (target %d)\n",
						 a.presentFps, a.targetFps);
	}
}

struct ShotEvent {
	double at = 0;
	std::string kind; // press | move | release | anim
	double x = 0, y = 0;
	int track = 1;
	std::string anim;
};

static std::vector<ShotEvent> parseShotScript(const std::string& spec) {
	std::vector<ShotEvent> out;
	size_t pos = 0;
	while (pos < spec.size()) {
		const size_t end = std::min(spec.find(';', pos), spec.size());
		std::string item = spec.substr(pos, end - pos);
		pos = end + 1;
		const size_t c1 = item.find(':');
		if (c1 == std::string::npos)
			continue;
		ShotEvent e;
		e.at = atof(item.substr(0, c1).c_str());
		const size_t c2 = item.find(':', c1 + 1);
		e.kind = item.substr(c1 + 1, (c2 == std::string::npos ? item.size() : c2) - c1 - 1);
		const std::string arg = c2 == std::string::npos ? std::string() : item.substr(c2 + 1);
		const size_t comma = arg.find(',');
		if (e.kind == "anim") {
			e.track = atoi(arg.c_str());
			e.anim = comma == std::string::npos ? arg : arg.substr(comma + 1);
		} else if (comma != std::string::npos) {
			e.x = atof(arg.substr(0, comma).c_str());
			e.y = atof(arg.substr(comma + 1).c_str());
		}
		out.push_back(e);
	}
	return out;
}

static void applyShotEvent(App& a, const ShotEvent& e, double now) {
	if (e.kind == "press") {
		a.mouseX = e.x;
		a.mouseY = e.y;
		a.lastPressAt = now;
		a.userOverride = true;
		a.userOverrideUntil = now + 4.0;
		const int zone = pressedMouse(a, e.x, e.y, now);
		cursorFxPress(a, e.x, e.y, now, zone);
		std::fprintf(stderr, "script %.2f press %.0f,%.0f -> zone=%d mode=%d\n", e.at, e.x, e.y, zone,
					 a.interactMode);
	} else if (e.kind == "move") {
		a.mouseX = e.x;
		a.mouseY = e.y;
		if (a.interactMode == 0)
			beginGazeTracking(a, e.x, e.y);
		movedMouse(a, e.x, e.y, e.x - a.prevMouseX, e.y - a.prevMouseY);
		a.prevMouseX = e.x;
		a.prevMouseY = e.y;
	} else if (e.kind == "editpress") {
		a.mouseDown = true;
		beginHitboxDrag(a, e.x, e.y);
	} else if (e.kind == "editmove") {
		moveHitboxDrag(a, e.x, e.y);
	} else if (e.kind == "editrelease") {
		a.mouseDown = false;
		a.hitboxDragMask = 0;
		writeHitboxFeedback(a, true);
	} else if (e.kind == "release") {
		std::fprintf(stderr, "script %.2f release mode=%d\n", e.at, a.interactMode);
		if (a.interactMode != 3)
			releasedMouse(a, now);
	} else if (e.kind == "anim") {
		if (e.anim == "empty")
			a.animState->setEmptyAnimation(static_cast<size_t>(e.track), 0.5f);
		else
			setAnim(a, static_cast<size_t>(e.track), e.anim.c_str(), false);
	}
}

// KEI_LIVE_SCRIPT replays the same script format on the *live* layer surface
// (same syntax as KEI_SHOT_SCRIPT, times are seconds after the first frame).
// Offscreen shots miss anything that only happens with a real window — the
// first-click hitch is exactly that kind of bug, so it needs a repro that keeps
// the compositor, EGL surface and swap chain in the loop.
static std::vector<ShotEvent> gLiveScript;
static size_t gLiveNext = 0;
static double gLiveEpoch = 0.0;

static void runLiveScript(App& a, double now) {
	if (gLiveNext >= gLiveScript.size())
		return;
	if (gLiveEpoch == 0.0)
		gLiveEpoch = now;
	while (gLiveNext < gLiveScript.size() && gLiveScript[gLiveNext].at <= now - gLiveEpoch) {
		const auto t0 = std::chrono::steady_clock::now();
		applyShotEvent(a, gLiveScript[gLiveNext++], now);
		std::fprintf(stderr, "live script event took %.1fms\n",
					 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
	}
}

static void frameDone(void* data, wl_callback* cb, uint32_t /*time*/) {
	App* a = static_cast<App*>(data);
	wl_callback_destroy(cb);
	a->frameCallback = nullptr;

	const auto nowTp = std::chrono::steady_clock::now();
	const double now = std::chrono::duration<double>(nowTp.time_since_epoch()).count();
	runLiveScript(*a, now);
	// Keep only the Wayland frame/control heartbeat alive while another client
	// is fullscreen on this output. No Spine update, GL draw, swap or audio runs.
	refreshRuntimeControl(*a, now);
	if (a->renderPaused) {
		// A frame callback is also our resume heartbeat. Throttle that heartbeat
		// to 10Hz so fullscreen really stops animation/GPU work without making
		// resume feel delayed.
		usleep(100000);
		armFrame(*a);
		a->nextFrameAt = {};
		a->lastFrameAt = {};
		return;
	}
	// Frame callbacks arrive at the output refresh rate. Keep a fractional
	// deadline so 60 fps on a 100 Hz output becomes a stable 6-of-10 cadence
	// instead of the naive every-other-callback 50 fps.
	const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
		std::chrono::duration<double>(1.0 / std::max(1, a->targetFps)));
	if (a->nextFrameAt.time_since_epoch().count() != 0 && nowTp < a->nextFrameAt) {
		armFrame(*a);
		return;
	}
	if (a->nextFrameAt.time_since_epoch().count() == 0)
		a->nextFrameAt = nowTp;
	do {
		a->nextFrameAt += interval;
	} while (a->nextFrameAt <= nowTp);

	float dt = a->lastFrameAt.time_since_epoch().count() == 0
		? 1.f / static_cast<float>(std::max(1, a->targetFps))
		: std::chrono::duration<float>(nowTp - a->lastFrameAt).count();
	a->lastFrameAt = nowTp;
	// A deliberately low target FPS still needs wall-clock animation speed.
	// Keep the old 50 ms stall guard for normal rates, but widen it in
	// proportion to the requested interval instead of slowing the character.
	dt = std::min(dt, std::max(0.05f, 1.5f / static_cast<float>(a->targetFps)));

	renderFrame(*a, dt, now);
}

static void usage(const char* argv0) {
	std::fprintf(stderr,
				 "usage: %s [--output NAME] [--assets DIR] [--fps N]\n"
				 "       %s --shot FILE.ppm [--size WxH] [--shot-time SECONDS]\n"
				 "  --shot renders one frame offscreen (no layer surface, nothing on screen)\n"
				 "  --hitbox-debug   draw all interaction zones (also KEI_DRAW_HITBOX=1)\n"
				 "  --no-cursor-fx   turn off the item's click fireworks\n"
				 "  --voice-volume V voiceline audio 0..1 (default 0.5, 0 = silent)\n"
				 "  --bgm-volume V   looping the item's configured BGM at 0..1 (default 0 = off)\n"
				 "  --control FILE   live audio/debug control file\n"
				 "  --auto-idle      let her move on her own between interactions\n"
				 "  KEI_TRACE_INPUT=1 log every press/release with its hitbox coords\n"
				 "  default assets: ~/.steam/.../3650880224/assets/4k\n",
				 argv0, argv0);
}

// Offscreen render to a pbuffer, dumped as binary PPM. Lets us A/B the look
// without putting anything on the user's monitors.
static int runShot(const std::string& assets, const std::string& path, int w, int h, double seconds) {
	g.display = wl_display_connect(nullptr);
	g.eglDisplay = g.display ? eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(g.display))
							 : eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (g.eglDisplay == EGL_NO_DISPLAY || !eglInitialize(g.eglDisplay, nullptr, nullptr)) {
		std::fprintf(stderr, "shot: eglInitialize failed\n");
		return 1;
	}
	EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
					  EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
					  EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
	EGLint n = 0;
	if (!eglChooseConfig(g.eglDisplay, attrs, &g.eglConfig, 1, &n) || n < 1 || !eglBindAPI(EGL_OPENGL_API)) {
		std::fprintf(stderr, "shot: no pbuffer config\n");
		return 1;
	}
	EGLint ctx[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
					EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
	g.eglContext = eglCreateContext(g.eglDisplay, g.eglConfig, EGL_NO_CONTEXT, ctx);
	// Tiny pbuffer only to make the context current; the real target is an FBO,
	// whose size limit is GL_MAX_RENDERBUFFER_SIZE rather than EGL's pbuffer cap.
	EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
	g.eglSurface = eglCreatePbufferSurface(g.eglDisplay, g.eglConfig, pb);
	if (g.eglContext == EGL_NO_CONTEXT || g.eglSurface == EGL_NO_SURFACE) {
		std::fprintf(stderr, "shot: context/pbuffer creation failed\n");
		return 1;
	}
	eglMakeCurrent(g.eglDisplay, g.eglSurface, g.eglSurface, g.eglContext);
	glewExperimental = GL_TRUE;
	glewInit();

	GLint maxRb = 0;
	glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxRb);
	if (w > maxRb || h > maxRb) {
		std::fprintf(stderr, "shot: %dx%d exceeds GL_MAX_RENDERBUFFER_SIZE=%d\n", w, h, maxRb);
		return 1;
	}
	GLuint fbo = 0, rb = 0;
	glGenFramebuffers(1, &fbo);
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		std::fprintf(stderr, "shot: framebuffer incomplete\n");
		return 1;
	}

	g.width = w;
	g.height = h;
	g.configured = true;
	if (!loadSpine(g, assets))
		return 1;
	setCamera(g);

	// KEI_DEBUG_PHYSICS=1 lists the physics constraints (spine 4.2 catches up
	// `skeleton.time - constraint.lastTime` in fixed `step` increments, and it
	// only advances lastTime while mix > 0 — so a constraint that idles at mix 0
	// pays for every second it was off the moment an animation turns it back on).
	const bool debugPhysics = getenv("KEI_DEBUG_PHYSICS") != nullptr;
	if (debugPhysics) {
		auto& pcs = g.skeleton->getPhysicsConstraints();
		std::fprintf(stderr, "physics: %d constraints\n", (int)pcs.size());
		for (size_t i = 0; i < pcs.size(); ++i)
			std::fprintf(stderr, "physics[%zu] bone=%s step=%.4f mix=%.3f (setup %.3f)\n", i,
						 pcs[i]->getBone()->getData().getName().buffer(),
						 pcs[i]->getData().getStep(), pcs[i]->getMix(),
						 pcs[i]->getData().getMix());
	}
	// KEI_TEST_TIME_JUMP=SEC[:AT] fast-forwards skeleton time by SEC at shot time
	// AT (default 1.0) to stand in for hours of idle without simulating them.
	double jumpSec = 0.0, jumpAt = 1.0;
	if (const char* v = getenv("KEI_TEST_TIME_JUMP")) {
		jumpSec = atof(v);
		if (const char* c = strchr(v, ':'))
			jumpAt = atof(c + 1);
	}
	bool jumped = false;

	// Lets a shot judge an interaction pose (e.g. the held pinch) rather than idle.
	if (const char* anim = getenv("KEI_SHOT_ANIM"))
		setAnim(g, 1, anim, false);

	// KEI_SHOT_SCRIPT replays a gesture offscreen through the *live* input
	// handlers, so what a shot shows is what the wallpaper does — guessing which
	// animation calls a gesture makes is how the pinch/pat bugs stayed hidden.
	// Format: ';'-separated "TIME:EVENT[:ARG]" where EVENT is
	//   press:X,Y | move:X,Y | release | anim:TRACK,NAME
	// X,Y are screen pixels for this shot size.
	std::vector<ShotEvent> script;
	if (const char* spec = getenv("KEI_SHOT_SCRIPT"))
		script = parseShotScript(spec);

	// One process renders the whole series: --shot-series DIR turns `path` into
	// DIR/f%04d.ppm sampled at --shot-fps, which is what makes frame-by-frame
	// inspection cheap enough to actually do.
	const char* seriesDir = getenv("KEI_SHOT_SERIES");
	const double seriesFps = getenv("KEI_SHOT_FPS") ? atof(getenv("KEI_SHOT_FPS")) : 10.0;
	int seriesIndex = 0;
	double nextCapture = seriesDir ? 0.0 : seconds;

	const float step = 1.f / 60.f;
	size_t next = 0;
	// updateInteraction reads absolute times; any stable epoch works offscreen.
	const double epoch = 1000.0;
	const long frames = static_cast<long>(seconds * 60.0 + 0.5);
	for (long i = 0; i <= frames; ++i) {
		const double t = i * static_cast<double>(step);
		while (next < script.size() && script[next].at <= t) {
			applyShotEvent(g, script[next++], epoch + t);
		}
		const auto advanceStart = std::chrono::steady_clock::now();
		updateInteraction(g, step, epoch + t);
		const auto stateStart = std::chrono::steady_clock::now();
		restoreHandFollowBase(g);
		g.animState->update(step);
		const auto applyStart = std::chrono::steady_clock::now();
		g.animState->apply(*g.skeleton);
		hideProfileSlots(g);
		applyHandFollowOffset(g);
		const auto skeletonStart = std::chrono::steady_clock::now();
		g.skeleton->update(step);
		if (jumpSec > 0.0 && !jumped && t >= jumpAt) {
			jumped = true;
			g.skeleton->setTime(g.skeleton->getTime() + (float)jumpSec);
			std::fprintf(stderr, "test: skeleton time += %.0fs at t=%.2f\n", jumpSec, t);
		}
		// KEI_NO_PHYSICS_CLAMP=1 restores the pre-fix behaviour (for A/B).
		if (!getenv("KEI_NO_PHYSICS_CLAMP"))
			clampPhysicsCatchUp(g);
		const auto worldStart = std::chrono::steady_clock::now();
		g.skeleton->updateWorldTransform(Physics_Update);
		const auto advanceEnd = std::chrono::steady_clock::now();
		if (debugPhysics) {
			auto& pcs = g.skeleton->getPhysicsConstraints();
			std::string mixes;
			for (size_t k = 0; k < pcs.size(); ++k) {
				char buf[32];
				std::snprintf(buf, sizeof(buf), "%.2f ", pcs[k]->getMix());
				mixes += buf;
			}
			const double worldMs =
				std::chrono::duration<double, std::milli>(advanceEnd - worldStart).count();
			if (i % 15 == 0 || worldMs >= 5.0)
				std::fprintf(stderr, "physics t=%.2f world=%.1fms mix: %s\n", t, worldMs,
							 mixes.c_str());
		}
		if (g.traceFrame) {
			auto elapsed = [](auto from, auto to) {
				return std::chrono::duration<double, std::milli>(to - from).count();
			};
			const double total = elapsed(advanceStart, advanceEnd);
			if (total >= g.traceFrameMs)
				std::fprintf(stderr,
							 "shot frame t=%.3f %.1fms (interact %.1f / state %.1f / apply %.1f / "
							 "skeleton %.1f / world %.1f)\n",
							 t, total, elapsed(advanceStart, stateStart), elapsed(stateStart, applyStart),
							 elapsed(applyStart, skeletonStart), elapsed(skeletonStart, worldStart),
							 elapsed(worldStart, advanceEnd));
		}

		if (seriesDir ? (t + 1e-9 < nextCapture) : (i != frames))
			continue;
		nextCapture += 1.0 / seriesFps;

		glViewport(0, 0, w, h);
		glClearColor(0.f, 0.f, 0.f, 1.f);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glClear(GL_COLOR_BUFFER_BIT);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
		setCamera(g);
		renderer_draw(g.renderer, g.skeleton, false);
		drawCursorFx(g, epoch + t);
		drawHitboxOverlay(g, w, h);
		glFinish();

		std::string buf(static_cast<size_t>(w) * h * 3, '\0');
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, &buf[0]);

		std::string out = path;
		if (seriesDir) {
			char name[64];
			std::snprintf(name, sizeof(name), "/f%04d.ppm", seriesIndex++);
			out = std::string(seriesDir) + name;
		}
		FILE* f = fopen(out.c_str(), "wb");
		if (!f) {
			std::fprintf(stderr, "shot: cannot write %s\n", out.c_str());
			return 1;
		}
		std::fprintf(f, "P6\n%d %d\n255\n", w, h);
		for (int y = h - 1; y >= 0; --y) // GL origin is bottom-left
			fwrite(&buf[static_cast<size_t>(y) * w * 3], 1, static_cast<size_t>(w) * 3, f);
		fclose(f);
		if (!seriesDir) {
			std::fprintf(stderr, "shot: wrote %s (%dx%d, t=%.2fs)\n", out.c_str(), w, h, seconds);
			return 0;
		}
	}
	if (seriesDir)
		std::fprintf(stderr, "shot: wrote %d frames to %s (%.0f fps)\n", seriesIndex, seriesDir, seriesFps);
	return 0;
}

int main(int argc, char** argv) {
	std::string assets =
		std::string(getenv("HOME") ? getenv("HOME") : "") +
		"/.steam/steam/steamapps/workshop/content/431960/3650880224/assets/4k";
	std::string profilePath;
	bool scaleFromCli = false;
	std::string shotPath;
	int shotW = 3440, shotH = 1440;
	double shotTime = 2.0;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--output") == 0 && i + 1 < argc)
			g.wantOutput = argv[++i];
		else if (strcmp(argv[i], "--assets") == 0 && i + 1 < argc)
			assets = argv[++i];
		else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc)
			g.targetFps = std::clamp(atoi(argv[++i]), 1, 240);
		else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc)
			shotPath = argv[++i];
		else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc)
			sscanf(argv[++i], "%dx%d", &shotW, &shotH);
		else if (strcmp(argv[i], "--shot-time") == 0 && i + 1 < argc)
			shotTime = atof(argv[++i]);
		else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
			g.modelScale = std::max(0.1f, static_cast<float>(atof(argv[++i])));
			scaleFromCli = true;
		} else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc)
			profilePath = argv[++i];
		else if (strcmp(argv[i], "--auto-idle") == 0)
			g.autoIdle = true;
		else if (strcmp(argv[i], "--hitbox-debug") == 0)
			g.drawHitbox = true;
		else if (strcmp(argv[i], "--no-cursor-fx") == 0)
			g.cursorFx = false;
		else if (strcmp(argv[i], "--voice-volume") == 0 && i + 1 < argc)
			g.voiceVolume = std::min(1.f, std::max(0.f, static_cast<float>(atof(argv[++i]))));
		else if (strcmp(argv[i], "--bgm-volume") == 0 && i + 1 < argc)
			g.bgmVolume = std::min(1.f, std::max(0.f, static_cast<float>(atof(argv[++i]))));
		else if (strcmp(argv[i], "--control") == 0 && i + 1 < argc)
			g.controlPath = argv[++i];
		else if (strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 0;
		}
	}
	if (getenv("KEI_DRAW_HITBOX"))
		g.drawHitbox = true;
	if (getenv("KEI_NO_CURSOR_FX"))
		g.cursorFx = false;
	if (getenv("KEI_TRACE_FX"))
		g.traceFx = true;
	if (const char* spec = getenv("KEI_LIVE_SCRIPT"))
		gLiveScript = parseShotScript(spec);
	if (getenv("KEI_TRACE_INPUT"))
		g.traceInput = true;
	if (const char* v = getenv("KEI_TRACE_FRAME")) {
		g.traceFrame = true;
		const double ms = atof(v);
		if (ms > 1.0)
			g.traceFrameMs = ms;
	}

	// A wallpaper is only a profile plus its assets, so find the profile the same
	// way: next to the item, then by workshop id in the config dir. Without one
	// the empty built-in profile applies: the item renders but does not react.
	{
		const std::string home = getenv("HOME") ? getenv("HOME") : "";
		std::string itemDir = assets;
		for (int up = 0; up < 2; ++up) {
			const size_t cut = itemDir.find_last_of('/');
			if (cut != std::string::npos)
				itemDir.erase(cut);
		}
		std::string id = itemDir.substr(itemDir.find_last_of('/') + 1);
		std::vector<std::string> candidates;
		if (!profilePath.empty())
			candidates.push_back(profilePath);
		else {
			candidates.push_back(itemDir + "/kei-profile.conf");
			candidates.push_back(home + "/.config/kei-wallpaper-host/profiles/" + id + ".conf");
		}
		bool loaded = false;
		float scale = g.modelScale;
		for (const std::string& c : candidates) {
			if (loadProfile(g.profile, &scale, c)) {
				if (!scaleFromCli)
					g.modelScale = scale;
				std::fprintf(stderr, "profile: %s (%s)\n", g.profile.name.c_str(), c.c_str());
				loaded = true;
				break;
			}
		}
		if (!loaded && !profilePath.empty()) {
			std::fprintf(stderr, "profile: cannot read %s\n", profilePath.c_str());
			return 1;
		}
		if (!loaded)
			std::fprintf(stderr, "profile: none found for %s; rendering without interaction "
							 "(run scripts/make-profile.py on the item to generate one)\n", id.c_str());

		// Hand-tuned zones live here so `make-profile.py` can be re-run at will.
		const std::string local = home + "/.config/kei-wallpaper-host/profiles/" + id + ".local.conf";
		if (loadProfile(g.profile, &scale, local, false)) {
			if (!scaleFromCli)
				g.modelScale = scale;
			std::fprintf(stderr, "profile: overlay %s\n", local.c_str());
		}
	}
	// assets/<res>/ sits next to assets/audio/ in every workshop item.
	{
		const size_t cut = assets.find_last_of('/');
		if (cut != std::string::npos)
			g.audioDir = assets.substr(0, cut) + "/audio";
	}
	// Voiceline clips are fire-and-forget players; don't leave zombies behind.
	signal(SIGCHLD, SIG_IGN);
	// The BGM player is detached (setsid), so it would outlive us and keep playing
	// after `pkill kei-wallpaper-host` unless we take it down on the way out.
	struct BgmReaper {
		static void handle(int sig) {
			if (g.bgmPid > 0)
				kill(g.bgmPid, SIGTERM);
			_exit(sig == 0 ? 0 : 128 + sig);
		}
	};
	signal(SIGTERM, &BgmReaper::handle);
	signal(SIGINT, &BgmReaper::handle);

	if (!shotPath.empty())
		return runShot(assets, shotPath, shotW, shotH, shotTime);

	g.display = wl_display_connect(nullptr);
	if (!g.display) {
		std::fprintf(stderr, "wl_display_connect failed\n");
		return 1;
	}
	g.registry = wl_display_get_registry(g.display);
	wl_registry_add_listener(g.registry, &registryListener, &g);
	// First roundtrip discovers globals. wl_output listeners are attached while
	// dispatching those globals, so their name events need a second roundtrip.
	wl_display_roundtrip(g.display);
	wl_display_roundtrip(g.display);

	OutputCandidate* selected = nullptr;
	for (OutputCandidate* candidate : g.outputs) {
		if (g.wantOutput.empty() || candidate->name == g.wantOutput) {
			selected = candidate;
			if (!g.wantOutput.empty())
				break;
		}
	}
	if (!selected) {
		std::fprintf(stderr, "output '%s' not found; available:", g.wantOutput.c_str());
		for (OutputCandidate* candidate : g.outputs)
			std::fprintf(stderr, " %s", candidate->name.empty() ? "(unnamed)" : candidate->name.c_str());
		std::fprintf(stderr, "\n");
		return 1;
	}
	g.output = selected->proxy;
	g.outputName = selected->name;
	g.scale = selected->scale;
	std::fprintf(stderr, "output: selected %s (scale %d)\n", g.outputName.c_str(), g.scale);

	if (!g.compositor || !g.layerShell || !g.output) {
		std::fprintf(stderr, "missing wayland globals (compositor/layerShell/output)\n");
		return 1;
	}
	if (!initEgl(g)) {
		std::fprintf(stderr, "EGL init failed\n");
		return 1;
	}

	g.surface = wl_compositor_create_surface(g.compositor);
	g.layerSurface = zwlr_layer_shell_v1_get_layer_surface(
		g.layerShell, g.surface, g.output, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "kei-wallpaper-host");
	zwlr_layer_surface_v1_set_anchor(g.layerSurface,
									 ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
										 ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(g.layerSurface, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(g.layerSurface, false);
	zwlr_layer_surface_v1_add_listener(g.layerSurface, &layerListener, &g);
	// Full-screen wallpaper is opaque — avoids compositor treating straight-alpha
	// EGL buffers as premultiplied (which blows out colors).
	wl_region* opaque = wl_compositor_create_region(g.compositor);
	wl_region_add(opaque, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_set_opaque_region(g.surface, opaque);
	wl_region_destroy(opaque);
	wl_region* input = wl_compositor_create_region(g.compositor);
	wl_region_add(input, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_set_input_region(g.surface, input);
	wl_region_destroy(input);
	wl_surface_commit(g.surface);
	wl_display_roundtrip(g.display);

	if (!g.configured || g.width <= 0 || g.height <= 0) {
		std::fprintf(stderr, "layer surface not configured\n");
		return 1;
	}

	g.eglWindow = wl_egl_window_create(g.surface, g.width * g.scale, g.height * g.scale);
	auto createWindowSurface = reinterpret_cast<PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC>(
		eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT"));
	if (createWindowSurface)
		g.eglSurface = createWindowSurface(g.eglDisplay, g.eglConfig, g.eglWindow, nullptr);
	else
		g.eglSurface = eglCreateWindowSurface(g.eglDisplay, g.eglConfig,
											  reinterpret_cast<EGLNativeWindowType>(g.eglWindow), nullptr);
	if (g.eglSurface == EGL_NO_SURFACE) {
		std::fprintf(stderr, "eglCreateWindowSurface failed\n");
		return 1;
	}
	eglMakeCurrent(g.eglDisplay, g.eglSurface, g.eglSurface, g.eglContext);
	eglSwapInterval(g.eglDisplay, 0); // uncapped; we pace via wl_surface_frame / target fps
	glewExperimental = GL_TRUE;
	GLenum glewErr = glewInit();
	if (glewErr != GLEW_OK && glewErr != GLEW_ERROR_NO_GLX_DISPLAY) {
		std::fprintf(stderr, "glewInit failed: %s\n", reinterpret_cast<const char*>(glewGetErrorString(glewErr)));
	} else {
		std::fprintf(stderr, "glew ok (err=%u)\n", static_cast<unsigned>(glewErr));
	}

	if (!loadSpine(g, assets))
		return 1;
	setCamera(g);

	std::fprintf(stderr, "wallpaper-host ready %dx%d target=%dfps assets=%s\n",
				 g.width, g.height, g.targetFps, assets.c_str());
	{
		const auto nowTp = std::chrono::steady_clock::now();
		const double now = std::chrono::duration<double>(nowTp.time_since_epoch()).count();
		renderFrame(g, 1.f / 60.f, now);
	}
	armFrame(g);

	// A stall can also live outside renderFrame — in a pointer handler or in the
	// compositor round trip. Timing the dispatch itself is what separates
	// "our frame got slow" from "we were not called for N seconds".
	while (g.running) {
		const auto t0 = std::chrono::steady_clock::now();
		if (wl_display_dispatch(g.display) == -1)
			break;
		if (g.traceFrame) {
			const double ms =
				std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			if (ms >= std::max(50.0, g.traceFrameMs))
				std::fprintf(stderr, "dispatch %.1fms\n", ms);
		}
	}

	return 0;
}
