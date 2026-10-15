// Copyright 2026 Azahar Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>

#include "tico/overlay/overlay_ui.h"

namespace SwitchFrontend::ImGuiOverlay {

// Creates the ImGui context and registers the draw hook of the active
// renderer (Vulkan, or OpenGL for both NVC0 and Zink). The renderer backend
// itself is created the first time a frame is drawn.
bool Init(bool vulkan);
void Shutdown();

// Shows or hides the quick menu (the HUD and toasts are drawn either way).
void SetVisible(bool visible);
bool IsVisible();

// Edge-triggered menu navigation for the next drawn frame.
void FeedNav(const OverlayUI::NavInput& nav);

// The touchscreen, for the menu.
void FeedTouch(const OverlayUI::TouchInput& touch);

// A texture from RGBA pixels, for ImGui to draw (0 on failure). Only while the
// overlay draws: the Save/Load State picture callback runs then. Textures are
// destroyed at the start of the next drawn frame.
unsigned long long CreateTexture(const unsigned char* rgba, int width, int height);
void DestroyTexture(unsigned long long texture);

// Held while the core's thread draws the overlay: take it around any
// OverlayUI call made from another thread.
std::recursive_mutex& Mutex();

// The action the menu returned while drawing, once; None when there was none.
OverlayUI::Action ConsumeAction();

} // namespace SwitchFrontend::ImGuiOverlay
