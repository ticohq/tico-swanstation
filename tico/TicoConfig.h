/// @file TicoConfig.h
/// @brief Minimal hardcoded configuration for tico overlay
#pragma once

#include <string>

namespace TicoConfig {
    // Hardcoded test ROM for easy testing
    constexpr const char* TEST_ROM = "sdmc:/tico/roms/psx/game.cue";
    
    // Asset paths
    constexpr const char* FONT_PATH = "romfs:/fonts/font.ttf";
    constexpr const char* SYSTEM_PATH = "sdmc:/tico/system/psx/";
    constexpr const char* SAVES_PATH = "sdmc:/tico/saves/psx/";
    constexpr const char* STATES_PATH = "sdmc:/tico/states/psx/";
    
    // Window settings
    constexpr int WINDOW_WIDTH = 1280;
    constexpr int WINDOW_HEIGHT = 720;
    constexpr float FONT_SIZE = 32.0f;

    // Audio backend configuration
    // true = Use SDL_QueueAudio (Push model, better for swanstation)
    // false = Use Mix_HookMusic + RingBuffer (Callback model, better for other cores)
    constexpr bool USE_SDLQUEUEAUDIO = true;
}

// UI Actions for HelpersBar
enum UIActions {
    ACTION_CONFIRM,
    ACTION_BACK,
    ACTION_DETAILS,
    ACTION_MENU,
    ACTION_EDIT,
    ACTION_DELETE
};
