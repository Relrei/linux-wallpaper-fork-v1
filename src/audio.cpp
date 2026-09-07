// Part of linux-wallpaper-fork. See docs/ARCHITECTURE.md for the module map.
#include "audio.h"

#include "interaction.h"

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

pid_t spawnVoiceClip(App& a, const std::string& path, float volume) {
	if (!a.traceInput)
		return spawnVoiceClipImpl(path, volume);
	const auto t0 = std::chrono::steady_clock::now();
	const pid_t pid = spawnVoiceClipImpl(path, volume);
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	std::fprintf(stderr, "voice: spawn %.2fms pid=%d\n", ms, pid);
	return pid;
}

void playVoiceline(App& a, double now) {
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
void stopVoiceline(App& a) {
	for (pid_t pid : a.voicePids)
		if (pid > 0)
			kill(pid, SIGTERM);
	a.voicePids.clear();
	a.pendingAudio.clear();
	a.voicePlaying = false;
}
