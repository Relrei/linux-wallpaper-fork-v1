// Part of linux-wallpaper-fork. See docs/ARCHITECTURE.md for the module map.
#include "shot.h"

#include "audio.h"
#include "interaction.h"
#include "renderer.h"

std::vector<ShotEvent> parseShotScript(const std::string& spec) {
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

void applyShotEvent(App& a, const ShotEvent& e, double now) {
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

// LWF_LIVE_SCRIPT replays the same script format on the *live* layer surface
// (same syntax as LWF_SHOT_SCRIPT, times are seconds after the first frame).
// Offscreen shots miss anything that only happens with a real window — the
// first-click hitch is exactly that kind of bug, so it needs a repro that keeps
// the compositor, EGL surface and swap chain in the loop.
std::vector<ShotEvent> gLiveScript;
static size_t gLiveNext = 0;
static double gLiveEpoch = 0.0;

void runLiveScript(App& a, double now) {
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

// Offscreen render to a pbuffer, dumped as binary PPM. Lets us A/B the look
// without putting anything on the user's monitors.
int runShot(const std::string& assets, const std::string& path, int w, int h, double seconds) {
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

	// LWF_DEBUG_PHYSICS=1 lists the physics constraints (spine 4.2 catches up
	// `skeleton.time - constraint.lastTime` in fixed `step` increments, and it
	// only advances lastTime while mix > 0 — so a constraint that idles at mix 0
	// pays for every second it was off the moment an animation turns it back on).
	const bool debugPhysics = getenv("LWF_DEBUG_PHYSICS") != nullptr;
	if (debugPhysics) {
		auto& pcs = g.skeleton->getPhysicsConstraints();
		std::fprintf(stderr, "physics: %d constraints\n", (int)pcs.size());
		for (size_t i = 0; i < pcs.size(); ++i)
			std::fprintf(stderr, "physics[%zu] bone=%s step=%.4f mix=%.3f (setup %.3f)\n", i,
						 pcs[i]->getBone()->getData().getName().buffer(),
						 pcs[i]->getData().getStep(), pcs[i]->getMix(),
						 pcs[i]->getData().getMix());
	}
	// LWF_TEST_TIME_JUMP=SEC[:AT] fast-forwards skeleton time by SEC at shot time
	// AT (default 1.0) to stand in for hours of idle without simulating them.
	double jumpSec = 0.0, jumpAt = 1.0;
	if (const char* v = getenv("LWF_TEST_TIME_JUMP")) {
		jumpSec = atof(v);
		if (const char* c = strchr(v, ':'))
			jumpAt = atof(c + 1);
	}
	bool jumped = false;

	// Lets a shot judge an interaction pose (e.g. the held pinch) rather than idle.
	if (const char* anim = getenv("LWF_SHOT_ANIM"))
		setAnim(g, 1, anim, false);

	// LWF_SHOT_SCRIPT replays a gesture offscreen through the *live* input
	// handlers, so what a shot shows is what the wallpaper does — guessing which
	// animation calls a gesture makes is how the pinch/pat bugs stayed hidden.
	// Format: ';'-separated "TIME:EVENT[:ARG]" where EVENT is
	//   press:X,Y | move:X,Y | release | anim:TRACK,NAME
	// X,Y are screen pixels for this shot size.
	std::vector<ShotEvent> script;
	if (const char* spec = getenv("LWF_SHOT_SCRIPT"))
		script = parseShotScript(spec);

	// One process renders the whole series: --shot-series DIR turns `path` into
	// DIR/f%04d.ppm sampled at --shot-fps, which is what makes frame-by-frame
	// inspection cheap enough to actually do.
	const char* seriesDir = getenv("LWF_SHOT_SERIES");
	const double seriesFps = getenv("LWF_SHOT_FPS") ? atof(getenv("LWF_SHOT_FPS")) : 10.0;
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
		// LWF_NO_PHYSICS_CLAMP=1 restores the pre-fix behaviour (for A/B).
		if (!getenv("LWF_NO_PHYSICS_CLAMP"))
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
