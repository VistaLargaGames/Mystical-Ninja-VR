#pragma once

#include <cstdint>
#include <string>

// Quest storage and the OpenXR loader handshake. Both run on the UI thread before the renderer.
void goemon_android_prepare_storage();
extern "C" void goemon_quest_boot();

// Makes sure a valid ROM is stored before the game starts: imports any ROM file
// left in the game folder (any name), otherwise asks the player to pick their
// own backup in a 2D panel. Returns false if they chose to quit.
bool goemon_android_ensure_rom(std::u8string game_id);

// VR hooks for the game, the input code and the renderer context (openxr_quest.cpp).
#include "ultramodern/ultra64.h"
namespace RT64 { struct Application; }
extern "C" void goemon_vr_overlay_n64_input(int player_index, uint16_t* buttons, float* x, float* y);
// Returns true when the VR code submitted the display list itself.
bool goemon_vr_display_list(RT64::Application* app, const OSTask* task);
