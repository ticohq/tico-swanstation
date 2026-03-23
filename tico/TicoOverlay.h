/// @file TicoOverlay.h
/// @brief Overlay UI for tico-integrated swanstation
#pragma once

#include "imgui.h"
#include <SDL.h>
#include <string>
#include <vector>
#include <memory>

// Forward declaration
class TicoCore;

/// @brief Overlay menu types
enum class OverlayMenu
{
    None,
    QuickMenu,
    SaveStates,
    Settings,
    DiscSelect
};

/// @brief Display mode for the emulator viewport
enum class swanstationDisplayMode
{
    Integer = 0, // Integer pixel scaling
    Display = 1, // Aspect-ratio based display
    COUNT = 2
};

/// @brief Display size (context-dependent on swanstationDisplayMode)
///   Integer → 1x, 2x, Auto
///   Display → Stretch, 4:3, 16:9, Original
enum class swanstationDisplaySize
{
    // Display sizes (0-3)
    Stretch = 0,
    _4_3 = 1,
    _16_9 = 2,
    Original = 3,
    // Integer sizes (4-6)
    _1x = 4,
    _2x = 5,
    Auto = 6
};

/// @brief Overlay UI for swanstation with tico styling
class TicoOverlay
{
public:
    TicoOverlay();

    /// @brief Update overlay animation
    void Update(float deltaTime);

    /// @brief Render the overlay
    void Render(ImVec2 displaySize, unsigned int gameTexture, float aspectRatio,
                int frameWidth, int frameHeight, int fboWidth = 0, int fboHeight = 0);

    /// @brief Handle input
    /// @return true if input was consumed by overlay
    bool HandleInput(SDL_GameController *controller);

    /// @brief Show/hide overlay
    void Show();
    void Hide();
    bool IsVisible() const { return m_currentMenu != OverlayMenu::None; }

    /// @brief Set game title for title card
    void SetGameTitle(const std::string &title) { m_gameTitle = title; }

    /// @brief Set core reference for save states
    void SetCore(TicoCore *core) { m_core = core; }

    /// @brief Check if user wants to exit
    bool ShouldExit() const { return m_shouldExit; }
    void ClearExit() { m_shouldExit = false; }

    /// @brief Check if user wants to reset
    bool ShouldReset() const { return m_shouldReset; }
    void ClearReset() { m_shouldReset = false; }

private:
    void RenderGame(ImDrawList *dl, ImVec2 displaySize, unsigned int texture,
                    float aspectRatio, int width, int height,
                    int fboWidth, int fboHeight);
    void RenderOverlayBackground(ImDrawList *dl, ImVec2 displaySize);
    void RenderTitleCard(ImDrawList *dl, ImVec2 displaySize);
    void RenderQuickMenu(ImDrawList *dl, ImVec2 displaySize);
    void RenderSaveStatesMenu(ImDrawList *dl, ImVec2 displaySize);
    void RenderSettingsMenu(ImDrawList *dl, ImVec2 displaySize);
    void RenderDiscMenu(ImDrawList *dl, ImVec2 displaySize);
    void RenderHelpersBar(ImDrawList *dl, ImVec2 displaySize);
    void RenderStatusBar(ImDrawList *dl, ImVec2 displaySize);
    void RenderRAAlerts(ImDrawList *dl, ImVec2 displaySize, float deltaTime);

    OverlayMenu m_currentMenu = OverlayMenu::None;
    std::string m_gameTitle;
    TicoCore *m_core = nullptr;

    // Animation
    float m_animTimer = 0.0f;

    // Menu state
    int m_quickMenuSelection = 0;
    int m_saveStateSlot = 0;
    bool m_isSaveMode = true;
    int m_settingsSelection = 0;
    
    // Disc Select
    int m_discSelection = 0;
    float m_discScrollY = 0.0f;
    float m_discTargetScrollY = 0.0f;
    struct DiscEntry {
        std::string displayName;
        std::string romPath;
    };
    std::vector<DiscEntry> m_discs;
    void ScanForDiscs();
    swanstationDisplayMode m_displayMode = swanstationDisplayMode::Display;
    swanstationDisplaySize m_displaySize = swanstationDisplaySize::_4_3;

    // Settings persistence
    void LoadCoreSettings();
    void SaveCoreSettings();
    void ApplyScalingSettings(bool save = true);

    // Triangle texture
    unsigned int m_triangleTexture = 0;
    int m_triangleWidth = 0;
    int m_triangleHeight = 0;

    // Input debounce
    bool m_upHeld = false;
    bool m_downHeld = false;
    bool m_leftHeld = false;
    bool m_rightHeld = false;
    bool m_confirmHeld = false;
    bool m_backHeld = false;
    bool m_toggleHeld = false;
    bool m_xHeld = false;
    uint32_t m_lastInputTime = 0;
    static constexpr uint32_t DEBOUNCE_MS = 200;

    // Exit/Reset flags
    bool m_shouldExit = false;
    bool m_shouldReset = false;

    // Battery Status
    uint32_t m_batteryLevel = 100;
    bool m_isCharging = false;
    float m_batteryTimer = 0.0f;
    float m_chargingStateProgress = 0.0f;
    unsigned int m_boltTexture = 0;
    int m_boltWidth = 0;
    int m_boltHeight = 0;

    // Config
    bool m_isDarkMode = true;
    bool m_showNickname = false; // Added
    std::string m_hourFormat = "24h";
    void LoadConfig();
    void LoadGeneralConfig();
    void LoadSVGIcon();

    // Social Area
    unsigned int m_avatarTexture = 0;
    std::string m_nickname;
    void LoadAccountData();
    void RenderSocialArea(ImDrawList *dl, ImVec2 displaySize);
};
