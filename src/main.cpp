// Part of linux-wallpaper-fork. See docs/ARCHITECTURE.md for the module map.
// Native Spine wallpaper host -- Spine 4.2 + wlr-layer-shell, no CEF.
//
// This file is argument parsing and start-up only. The work is in:
//   profile.*      what is per-item (hit zones, animation names, timings)
//   wayland.*      layer-shell surface, EGL, protocol listeners
//   renderer.*     skeleton loading, camera, drawing, overlays
//   interaction.*  zone hit test and the gesture state machine
//   audio.*        voicelines and BGM
//   shot.*         offscreen rendering and scripted replay
// docs/ARCHITECTURE.md has the map.

#include "app.h"
#include "audio.h"
#include "interaction.h"
#include "renderer.h"
#include "shot.h"
#include "wayland.h"

App g;

static void usage(const char* argv0) {
	std::fprintf(stderr,
				 "usage: %s --assets DIR [--output NAME] [--fps N]\n"
				 "       %s --assets DIR --shot FILE.ppm [--size WxH] [--shot-time SECONDS]\n"
				 "  --shot renders one frame offscreen (no layer surface, nothing on screen)\n"
				 "  --hitbox-debug   draw all interaction zones (also LWF_DRAW_HITBOX=1)\n"
				 "  --no-cursor-fx   turn off the item's click fireworks\n"
				 "  --voice-volume V voiceline audio 0..1 (default 0.5, 0 = silent)\n"
				 "  --bgm-volume V   looping the item's configured BGM at 0..1 (default 0 = off)\n"
				 "  --control FILE   live audio/debug control file\n"
				 "  --auto-idle      let the character move on its own between interactions\n"
				 "  LWF_TRACE_INPUT=1 log every press/release with its hitbox coords\n"
				 "  --assets is required; it must be a wallpaper you own, e.g.\n"
				 "  ~/.steam/.../workshop/content/431960/<item id>/assets/4k\n",
				 argv0, argv0);
}

int main(int argc, char** argv) {
	// No default: this host does not ship or point at any specific wallpaper.
	// --assets must name a directory the user already owns.
	std::string assets;
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
	if (assets.empty()) {
		std::fprintf(stderr, "error: --assets DIR is required (a wallpaper you own)\n\n");
		usage(argv[0]);
		return 1;
	}
	if (getenv("LWF_DRAW_HITBOX"))
		g.drawHitbox = true;
	if (getenv("LWF_NO_CURSOR_FX"))
		g.cursorFx = false;
	if (getenv("LWF_TRACE_FX"))
		g.traceFx = true;
	if (const char* spec = getenv("LWF_LIVE_SCRIPT"))
		gLiveScript = parseShotScript(spec);
	if (getenv("LWF_TRACE_INPUT"))
		g.traceInput = true;
	if (const char* v = getenv("LWF_TRACE_FRAME")) {
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
		// Workshop items are not all shaped the same: most ship
		// assets/<res>/x.skel, some ship assets/x.skel. Walking up a fixed two
		// levels turned the second layout into the appid directory, so the
		// profile was looked up under the id "431960" and never found. Climb
		// until project.json appears instead -- that file marks the item root in
		// every layout -- and keep the old two-level guess as the fallback.
		std::string itemDir = assets;
		std::string probe = assets;
		bool foundItem = false;
		for (int up = 0; up < 4 && !probe.empty(); ++up) {
			const size_t cut = probe.find_last_of('/');
			if (cut == std::string::npos || cut == 0)
				break;
			probe.erase(cut);
			if (access((probe + "/project.json").c_str(), R_OK) == 0) {
				itemDir = probe;
				foundItem = true;
				break;
			}
		}
		if (!foundItem) {
			itemDir = assets;
			for (int up = 0; up < 2; ++up) {
				const size_t cut = itemDir.find_last_of('/');
				if (cut != std::string::npos)
					itemDir.erase(cut);
			}
		}
		std::string id = itemDir.substr(itemDir.find_last_of('/') + 1);
		std::vector<std::string> candidates;
		if (!profilePath.empty())
			candidates.push_back(profilePath);
		else {
			candidates.push_back(itemDir + "/wallpaper-profile.conf");
			candidates.push_back(home + "/.config/linux-wallpaper-fork/profiles/" + id + ".conf");
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
		const std::string local = home + "/.config/linux-wallpaper-fork/profiles/" + id + ".local.conf";
		if (loadProfile(g.profile, &scale, local, false)) {
			if (!scaleFromCli)
				g.modelScale = scale;
			std::fprintf(stderr, "profile: overlay %s\n", local.c_str());
		}
	}
	// assets/audio/ is a sibling of assets/<res>/ in the common layout and a
	// child of assets/ in the single-resolution one. Probe rather than assume:
	// guessing wrong is silent, because voicelines only fail when clicked.
	{
		std::vector<std::string> tries;
		tries.push_back(assets + "/audio");
		const size_t cut = assets.find_last_of('/');
		if (cut != std::string::npos)
			tries.push_back(assets.substr(0, cut) + "/audio");
		for (const std::string& dir : tries) {
			if (access(dir.c_str(), R_OK | X_OK) == 0) {
				g.audioDir = dir;
				break;
			}
		}
		if (g.audioDir.empty() && !tries.empty())
			g.audioDir = tries.back();
	}
	// Voiceline clips are fire-and-forget players; don't leave zombies behind.
	signal(SIGCHLD, SIG_IGN);
	// The BGM player is detached (setsid), so it would outlive us and keep playing
	// after `pkill linux-wallpaper-fork` unless we take it down on the way out.
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
		g.layerShell, g.surface, g.output, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "linux-wallpaper-fork");
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
