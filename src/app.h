// Part of linux-wallpaper-fork. See docs/ARCHITECTURE.md for the module map.
#pragma once

// The one piece of shared state: every module operates on this App. It is a
// plain struct on purpose -- the host is a single-threaded event loop, and the
// alternative (passing a dozen subsystems around) obscured which frame a value
// belonged to. `using namespace spine` is inherited from the original single
// file and kept so the module bodies are unchanged.

// Native Spine wallpaper host — Spine 4.2 + wlr-layer-shell, no CEF.
#include "spine-glfw.h"
#include <spine/Version.h>

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

#include <ctype.h>
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
#include <utility>
#include <vector>

using namespace spine;

#include "profile.h"

struct App;

struct OutputCandidate {
	App* app = nullptr;
	wl_output* proxy = nullptr;
	uint32_t globalName = 0;
	std::string name;
	std::string description;
	int32_t scale = 1;
};

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

	// LWF_TRACE_FRAME=1: break down only the frames past the threshold. A hitch
	// on the first click never shows up in an average fps, so look per frame.
	bool traceFrame = false;
	double traceFrameMs = 8.0;
	// Verbose cursor-fx log. Folding it into LWF_TRACE_INPUT buries the presses.
	bool traceFx = false;
	double nextPointerTraceAt = 0;
};

// The single instance. Defined in main.cpp.
extern App g;
