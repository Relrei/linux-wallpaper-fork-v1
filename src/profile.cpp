// Part of linux-wallpaper-fork. See docs/ARCHITECTURE.md for the module map.
#include "profile.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// `key=value` lines, `#` comments. Deliberately not JSON: the generator script
// does the messy parsing of the wallpaper's own JS, so the host only needs a
// format it can read without a dependency.
bool loadProfile(Profile& p, float* modelScale, const std::string& path, bool reset) {
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
