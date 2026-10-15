#include "tico/overlay_hook.h"

/* Set by the overlay (imgui_overlay.cpp) for the active renderer */
TicoVkOverlayHook tico_vk_overlay_hook;
TicoGlOverlayHook tico_gl_overlay_hook;
TicoOverlayWantsFrame tico_overlay_wants_frame;
volatile int tico_overlay_hold;
