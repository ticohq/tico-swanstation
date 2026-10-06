/// @file TicoConfig.h
/// @brief Minimal hardcoded configuration for tico overlay
#pragma once

#include <string>

namespace TicoConfig {
    // Hardcoded test ROM for easy testing
    constexpr const char* TEST_ROM = "sdmc:/tico/roms/psx/game.cue";
    
    // Asset paths
    constexpr const char* FONT_PATH = "romfs:/fonts/font.ttf";
    constexpr const char* IMAGES_PATH = "romfs:/images/";
    /// Console being booted: psx. Tico passes it as argv[3] ("{slug}" in
    /// module.json's launch template); older launchers omit it, so it is
    /// inferred from the ROM's parent directory (roms/<slug>/), else psx.
    void SetSlug(const std::string &launchSlug, const std::string &romPath);
    const std::string &Slug();

    /// Content directories. Tico's per-module Paths tab stores custom roots as
    /// tico_{system,saves,states}_path in swanstation.jsonc; empty or missing
    /// keys fall back to sdmc:/tico/<kind>/. Like tico's {saves}/{states}/
    /// {system}, the slug is appended to each root: the BIOS images go in
    /// <system root>/psx/.
    std::string SystemPath();
    std::string SavesPath();
    std::string StatesPath();

    /// Create a directory and any missing parents.
    void MakeDirs(const std::string &path);

    /// The GPU renderer's shader cache (pipelines for Vulkan and OpenGL).
    inline std::string CachePath() { return "sdmc:/tico/cache/swanstation/"; }

    /// RetroAchievements console ID
    inline int GetRcConsoleId() { return 12; } // RC_CONSOLE_PLAYSTATION
    
    // Window settings
    constexpr int WINDOW_WIDTH = 1280;
    constexpr int WINDOW_HEIGHT = 720;
    constexpr float FONT_SIZE = 32.0f;

    // Audio backend configuration
    // true  = SDL_QueueAudio (Push model), which a busy frame cannot starve
    //         the way it does SDL_mixer's pull callback. The RA trophy chime is
    //         mixed into this queue by TicoAudio (no second device, which the
    //         Switch can't do).
    // false = Mix_HookMusic + RingBuffer (Callback model, used by lighter cores).
    constexpr bool USE_SDLQUEUEAUDIO = true;
}

// Emulator-agnostic paths consumed by the generic Tico layer (Tico::Main,
// Tico::ChainloadLauncher). The core's own subpaths stay in TicoConfig above.
namespace Tico { namespace Paths {
    constexpr const char* Root              = "sdmc:/tico";
    constexpr const char* Debug             = "sdmc:/tico/debug";
    constexpr const char* Assets            = "sdmc:/tico/assets";
    constexpr const char* Lang              = "sdmc:/tico/lang";
    constexpr const char* System            = "sdmc:/tico/system";
    constexpr const char* SavesRoot         = "sdmc:/tico/saves";
    constexpr const char* StatesRoot        = "sdmc:/tico/states";
    constexpr const char* CoreConfigDir     = "sdmc:/tico/config/cores";
    constexpr const char* LauncherNro       = "sdmc:/switch/tico.nro";
    constexpr const char* LauncherNroFallback = "sdmc:/switch/tico/tico.nro";
    constexpr const char* DefaultTitleFont       = "romfs:/fonts/font.ttf";
    constexpr const char* DefaultDescriptionFont = "romfs:/fonts/description.ttf";
}}  // namespace Tico::Paths

// UI Actions for HelpersBar
enum UIActions {
    ACTION_CONFIRM,
    ACTION_BACK,
    ACTION_DETAILS,
    ACTION_MENU,
    ACTION_EDIT,
    ACTION_DELETE
};
