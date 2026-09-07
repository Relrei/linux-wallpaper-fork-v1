// Part of linux-wallpaper-fork. See docs/ARCHITECTURE.md for the module map.
#include "interaction.h"

#include "audio.h"
#include "renderer.h"

// Upstream t(): screen px → design space. The algebra collapses to
// L/2 + transpose * (n - mid) / scale, which is why `transpose` must exist.
float toHitboxSpace(const App& a, double n, bool isX) {
	const float length = isX ? a.profile.designW : a.profile.designH;
	const float mid = (isX ? a.width : a.height) * 0.5f;
	return length * 0.5f + a.transpose * (static_cast<float>(n) - mid) / a.modelScale;
}

float toScreenSpace(const App& a, float n, bool isX) {
	const float length = isX ? a.profile.designW : a.profile.designH;
	const float mid = (isX ? a.width : a.height) * 0.5f;
	return mid + a.modelScale * (n - length * 0.5f) / a.transpose;
}

// Spine world coordinates use the renderer camera (origin around the model),
// not the 2560x1600 design coordinates used by HITBOX. Keep this conversion
// beside setCamera's inverse so the hand-follow limit can be drawn truthfully.
void modelWorldToScreen(const App& a, float worldX, float worldY, float& x, float& y) {
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

bool inHitbox(const Hitbox& box, float tx, float ty, float grace) {
	return box.enabled() && tx > box.xMin - grace && tx < box.xMax + grace && ty > box.yMin - grace &&
		   ty < box.yMax + grace;
}

// Returns the upstream mouseSelect value, or 3 (eye track) when nothing is hit.
int zoneAt(const App& a, float tx, float ty, bool* usedGrace) {
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

// Where the pat bone (`bone.point`) should sit for a cursor at (x, y). Upstream stepped the
// bone by ±5 per motion event keyed off canvas thresholds (`y < 800`,
// `x >= 1440`) that are both always true inside this zone, so every direction
// pushed the same way and the stroke came out faceted. Position mapping keeps
// the diagonal ("clockwise") intent and eases smoothly instead.
float patTargetFor(const App& a, double x, double y) {
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
TrackEntry* setAnim(App& a, size_t track, const char* name, bool loop) {
	if (!a.animState || !a.skeletonData || !a.skeletonData->findAnimation(name))
		return nullptr;
	return a.animState->setAnimation(track, name, loop);
}

TrackEntry* addAnim(App& a, size_t track, const char* name, bool loop, float delay) {
	if (!a.animState || !a.skeletonData || !a.skeletonData->findAnimation(name))
		return nullptr;
	return a.animState->addAnimation(track, name, loop, delay);
}

bool currentAnimIs(const App& a, size_t track, const std::string& name) {
	if (!a.animState || name.empty())
		return false;
	TrackEntry* e = a.animState->getCurrent(track);
	return e && e->getAnimation() && name == e->getAnimation()->getName().buffer();
}

// Return the normalized time of the current entry when it is `name`.
// This is used when a release animation is interrupted: restarting from zero
// would visibly reset the character even though the pointer only paused briefly.
float currentAnimProgress(const App& a, size_t track, const std::string& name) {
	if (!currentAnimIs(a, track, name))
		return -1.f;
	TrackEntry* e = a.animState->getCurrent(track);
	const float duration = e && e->getAnimation() ? e->getAnimation()->getDuration() : 0.f;
	if (duration <= 0.f)
		return -1.f;
	return std::min(1.f, std::max(0.f, e->getTrackTime() / duration));
}

double animDuration(const App& a, const char* name) {
	if (!a.skeletonData)
		return 0.0;
	Animation* anim = a.skeletonData->findAnimation(name);
	return anim ? anim->getDuration() : 0.0;
}

// The manager writes this tiny file atomically so audio sliders can apply
// without recreating the EGL/Spine host (which would reset the character).
void refreshRuntimeControl(App& a, double now) {
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
bool reactionOwnsTracks(const App& a) {
	if (a.voicePlaying || a.patEndAt >= 0 || a.handFollowReleasePending)
		return true;
	return currentAnimIs(a, 1, a.profile.patAnim) ||
		   currentAnimIs(a, 1, a.profile.patEndAnim) ||
		   currentAnimIs(a, 1, a.profile.pinchEndAnim) ||
		   currentAnimIs(a, 2, a.profile.pinchEndAnimA) ||
		   currentAnimIs(a, 1, a.profile.handFollowAnim) ||
		   currentAnimIs(a, 1, a.profile.handFollowEndAnim);
}

void beginGazeTracking(App& a, double x, double y) {
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
int pressedMouse(App& a, double x, double y, double now) {
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

void movedMouse(App& a, double x, double y, double dx, double dy) {
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

void releasedMouse(App& a, double now) {
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

Hitbox* editBox(App& a, int index) {
	switch (index) {
		case 0: return &a.profile.headpat;
		case 1: return &a.profile.pinch;
		case 2: return &a.profile.voiceline;
		case 3: return &a.profile.handFollow;
		default: return nullptr;
	}
}

void writeHitboxFeedback(App& a, bool force) {
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

bool beginHitboxDrag(App& a, double x, double y) {
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

void moveHitboxDrag(App& a, double x, double y) {
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

void applyHandFollowOffset(App& a) {
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
	// Convert the screen-aligned world offset through the target's parent. That
	// parent is rotated in some rigs, and then adding dx/dy straight to the
	// local X/Y swaps the axes -- so go through world space rather than assume.
	float worldX = 0.f, worldY = 0.f;
	parent->localToWorld(a.handFollowTarget->getX(), a.handFollowTarget->getY(), worldX, worldY);
	float localX = 0.f, localY = 0.f;
	parent->worldToLocal(worldX + a.handOffsetX, worldY + a.handOffsetY, localX, localY);
	a.handFollowTarget->setX(localX);
	a.handFollowTarget->setY(localY);
}

void restoreHandFollowBase(App& a) {
	if (!a.handFollowTarget || !a.handOffsetApplied)
		return;
	a.handFollowTarget->setX(a.handBaseLocalX);
	a.handFollowTarget->setY(a.handBaseLocalY);
	a.handOffsetApplied = false;
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

void updateInteraction(App& a, float dt, double now) {
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
