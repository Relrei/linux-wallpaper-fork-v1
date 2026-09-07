// Part of linux-wallpaper-fork. See docs/ARCHITECTURE.md for the module map.
#include "wayland.h"

#include "interaction.h"
#include "renderer.h"
#include "shot.h"

static void frameDone(void* data, wl_callback* cb, uint32_t /*time*/);

const wl_callback_listener frameListener = {.done = frameDone};

void armFrame(App& a) {
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

const wl_registry_listener registryListener = {
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

const zwlr_layer_surface_v1_listener layerListener = {
	.configure = layerConfigure,
	.closed = layerClosed,
};

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
		// immediately started Look again, so one release visibly moved the
		// character's head twice. Workspace changes repeated the same
		// Leave/Enter pair.
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

bool initEgl(App& a) {
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
