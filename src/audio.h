// Part of linux-wallpaper-fork. See docs/ARCHITECTURE.md for the module map.
#pragma once

// Voicelines and BGM. There is no decoder in-process: each clip is a forked
// `pw-play` (or `paplay`) at the offsets the item's own AUDIO_DETAIL gives, so
// a press can cut a line off by killing its players. The BGM player is
// respawned when it exits -- that is the loop -- and killed on the way out.

#include "app.h"

#include <sys/types.h>

// Forks a player for one clip. Returns its pid, or -1.
pid_t spawnVoiceClip(App& a, const std::string& path, float volume);
// Starts the current line (index advances only when it finishes, not when a
// press interrupts it) and queues its clips into a.pendingAudio.
void playVoiceline(App& a, double now);
// Kills the forked players and drops anything still queued.
void stopVoiceline(App& a);
