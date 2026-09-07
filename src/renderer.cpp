// Part of linux-wallpaper-fork. See docs/ARCHITECTURE.md for the module map.
#include "renderer.h"

#include "interaction.h"
#include "wayland.h"

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

// One stderr block saying how much of the profile this skeleton can actually
// honour. A profile is generated from the item's own js/main.js, but nothing
// guarantees the rig has the animations that JS names, and a key that is simply
// absent means "this item has no such gesture". Both are legitimate and both are
// invisible at runtime, because every play goes through findAnimation and a miss
// leaves the track untouched. Printing them is what turns "the character does
// not react" from a mystery into a line of output.
//
// The wording is deliberately greppable: scripts/verify-generic.sh counts
// "not in skeleton" across every installed item.
void reportProfileCoverage(App& a) {
	const Profile& p = a.profile;
	// Talk names carry a "%d" line index; probe with the first line.
	char talk[128] = {0}, talkA[128] = {0};
	if (!p.talkAnim.empty())
		std::snprintf(talk, sizeof(talk), p.talkAnim.c_str(), 1);
	if (!p.talkAnimA.empty())
		std::snprintf(talkA, sizeof(talkA), p.talkAnimA.c_str(), 1);
	const std::pair<const char*, std::string> anims[] = {
		{"idle", p.idleAnim},             {"pat", p.patAnim},
		{"patEnd", p.patEndAnim},         {"patEndA", p.patEndAnimA},
		{"pinch", p.pinchAnim},           {"pinchA", p.pinchAnimA},
		{"pinchEnd", p.pinchEndAnim},     {"pinchEndA", p.pinchEndAnimA},
		{"look", p.lookAnim},             {"lookA", p.lookAnimA},
		{"lookEnd", p.lookEndAnim},       {"lookEndA", p.lookEndAnimA},
		{"handFollow", p.handFollowAnim}, {"handFollowEnd", p.handFollowEndAnim},
		{"talk", std::string(talk)},      {"talkA", std::string(talkA)},
	};
	int present = 0, unset = 0, absent = 0;
	for (const std::pair<const char*, std::string>& entry : anims) {
		if (entry.second.empty())
			++unset;
		else if (a.skeletonData->findAnimation(entry.second.c_str()))
			++present;
		else {
			++absent;
			std::fprintf(stderr, "profile: %s animation '%s' not in skeleton\n",
						 entry.first, entry.second.c_str());
		}
	}
	std::fprintf(stderr, "profile: animations %d present, %d unset, %d not in skeleton\n",
				 present, unset, absent);

	const Hitbox* zones[] = {&p.headpat, &p.pinch, &p.voiceline, &p.handFollow};
	std::string enabled;
	int live = 0;
	for (const Hitbox* z : zones) {
		if (!z->enabled())
			continue;
		++live;
		enabled += (enabled.empty() ? "" : ", ");
		enabled += z->name;
	}
	if (live == 0)
		std::fprintf(stderr, "profile: no hit zones -- nothing on this item is clickable\n");
	else
		std::fprintf(stderr, "profile: hit zones %d of 4 (%s)\n", live, enabled.c_str());
}

// The Spine version a .skel was exported from, or "" if the header is not
// recognisable. The runtime only accepts a skeleton whose major.minor matches
// its own, and its own diagnostic reads the version at a fixed offset that
// moved between generations -- against a 3.8 file it prints binary garbage. The
// header always contains the version as plain "N.N.N" text, so scan for that.
static std::string skeletonVersion(const std::string& skelPath) {
	FILE* f = fopen(skelPath.c_str(), "rb");
	if (!f)
		return std::string();
	unsigned char head[128] = {0};
	const size_t n = fread(head, 1, sizeof(head), f);
	fclose(f);
	for (size_t i = 0; i + 4 < n; ++i) {
		if (!isdigit(head[i]) || head[i + 1] != '.')
			continue;
		size_t j = i;
		int dots = 0;
		while (j < n && (isdigit(head[j]) || head[j] == '.')) {
			if (head[j] == '.')
				++dots;
			++j;
		}
		if (dots >= 2)
			return std::string(reinterpret_cast<const char*>(head + i), j - i);
	}
	return std::string();
}

bool loadSpine(App& a, const std::string& assetDir) {
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
		// The usual cause is generation drift: the item was exported from an
		// older Spine than this build links. Say so in those words -- "failed to
		// load skel" alone reads as a bug in the host.
		const std::string ver = skeletonVersion(skelPath);
		if (!ver.empty() && ver.compare(0, strlen(SPINE_VERSION_STRING), SPINE_VERSION_STRING) != 0)
			std::fprintf(stderr,
						 "failed to load skel: %s\n"
						 "  exported from Spine %s; this build links spine-cpp " SPINE_VERSION_STRING
						 ". Spine skeletons are not readable across generations --\n"
						 "  re-export the item from Spine " SPINE_VERSION_STRING
						 ", or build a host against the matching runtime.\n",
						 skelPath.c_str(), ver.c_str());
		else
			std::fprintf(stderr, "failed to load skel: %s: %s\n", skelPath.c_str(),
						 binary.getError().buffer());
		return false;
	}
	a.skeleton = new Skeleton(a.skeletonData);
	if (getenv("LWF_DEBUG_BONES")) {
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
	reportProfileCoverage(a);
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
	if (getenv("LWF_DEBUG_ANIMS")) {
		Vector<Animation*>& anims = a.skeletonData->getAnimations();
		for (size_t i = 0; i < anims.size(); ++i)
			std::fprintf(stderr, "anim %-18s %.3fs\n", anims[i]->getName().buffer(), anims[i]->getDuration());
	}
	a.renderer = renderer_create();
	return true;
}

void hideProfileSlots(App& a) {
	// AnimationState can restore attachments, which is why the workshop's own JS
	// repeats this after every apply rather than doing it just once at load time.
	for (Slot* slot : a.hiddenSlotPtrs)
		slot->setAttachment(nullptr);
}

void setCamera(App& a) {
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

// Debug overlay: the upstream `drawHitboxes` option, ported so a "missing"
// zone is visible instead of guessed at. Enable with --hitbox-debug.
void drawHitboxOverlay(App& a, int viewportW, int viewportH) {
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
void cursorFxPress(App& a, double x, double y, double now, int zone) {
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

void drawCursorFx(App& a, double now) {
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
void clampPhysicsCatchUp(App& a) {
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

void renderFrame(App& a, float dt, double now) {
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
