// Part of linux-wallpaper-fork. See docs/ARCHITECTURE.md for the module map.
#pragma once

// Wayland: the layer-shell background surface, the EGL context on it, and the
// protocol listeners. Everything here is glue -- the pointer callbacks convert
// wl_fixed_t into doubles and hand over to interaction.h, and the frame callback
// drives renderer.h. No drawing and no gesture logic lives in this file.

#include "app.h"

// Ask the compositor for the next frame callback and commit. Must be called
// before eglSwapBuffers, or the surface settles at half the refresh rate.
void armFrame(App& a);

// Create the EGL display/context for `a.surface`. Returns false if the
// compositor offers no config this host can use.
bool initEgl(App& a);

extern const wl_registry_listener registryListener;
extern const zwlr_layer_surface_v1_listener layerListener;
// renderFrame() re-arms the frame callback inline: arming it after the swap
// misses every other vsync on a 100Hz panel (an effective 50fps lock).
extern const wl_callback_listener frameListener;
