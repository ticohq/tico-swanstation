/// @file SwanRuntime.h
/// @brief SwanStation implementation of Tico::CoreRuntime.
///
/// Owns the core (TicoCore over SwanHost), the renderer it was started with
/// (TicoVulkan on the core's Vulkan device, or TicoGL), SDL audio (TicoAudio)
/// and the overlay, behind the agnostic CoreRuntime interface that Tico::Main
/// drives.
#pragma once

#include "TicoMain.h"

#include <SDL.h>

#include <array>

#include "imgui.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class TicoCore;
class TicoAudio;

namespace Tico
{

class SwanOverlayHost; // IOverlayHost adapter (defined in the .cpp)

class SwanRuntime final : public CoreRuntime
{
public:
    explicit SwanRuntime(LogCallback log = {});
    ~SwanRuntime() override;

    const char *Name() const override { return "swanstation"; }
    bool Configure(const LaunchInfo &launch) override;
    bool Initialize(const LaunchInfo &launch) override;
    bool LoadContent(const std::string &path) override;
    void HandleInput(const FrameInput &input) override;
    void RunFrame() override;
    void RenderFrame() override;
    bool ShouldExit() const override { return exitRequested_; }
    bool ShouldChainloadLauncher() const override { return chainload_; }
    bool ShouldRelaunch() const override { return relaunch_; }
    void RequestExit() override { exitRequested_ = true; }
    void Shutdown() override;

private:
    bool InitAudio();
    bool InitOverlay(const std::string &romPath);
    void ShutdownOverlay();
    /// Boots the game; false (with a toast) when it could not be loaded.
    bool BootGame();
    void ApplyCoreInput(const FrameInput &input);
    void UpdateFastForward(const FrameInput &input);
    void UpdateFramePacing();
    int FramesThisRefresh();
    void OpenMenu();
    void CloseMenu();
    void OfferResume();
    std::string RestartMarkerPath() const;
    /// Feeds the open menu; returns false when the menu is closed.
    bool FeedMenu(const FrameInput &input);
    /// Carries out what the menu chose on the last built frame.
    void RunMenuAction();
    void ApplySettingsToCore();
    void UpdateHud(float deltaTime);
    void DrawGame(ImDrawList *list, ImVec2 size);
    void DrawOsd();

    LogCallback log_;
    std::unique_ptr<TicoCore> core_;
    std::unique_ptr<SwanOverlayHost> overlayHost_;
    std::unique_ptr<TicoAudio> audio_;
    SDL_AudioDeviceID audioDevice_ = 0;

    bool overlayReady_ = false;
    bool exitRequested_ = false;
    bool chainload_ = false;
    bool relaunch_ = false;    // Restart: start this NRO again on exit
    bool biosNotice_ = false;  // no BIOS: the notice runs before the game boots
    bool standalone_ = false;  // started without a game: the library is the menu
    bool fromLibrary_ = false; // launched from the library: Exit goes back to it
    std::string argv0_;
    bool offerResume_ = false; // ask to continue from the auto save after the first frame
    void *frameCmd_ = nullptr; // this frame's command buffer (VkCommandBuffer), null when skipped
    bool frameInFlight_ = false;
    std::string romPath_;
    std::string titleArg_; // display title from the launcher
    bool pad2Connected_ = true; // as LoadGame leaves it
    uint32_t lastTicks_ = 0;

    // Fast forward (player 1's hotkey) and pacing the core onto the 60 Hz display
    uint64_t ffButton_ = 0;
    bool ffHotkeyHeld_ = false;
    bool ffLatched_ = false;
    float ffFrameBudget_ = 0.0f;
    bool vsyncOff_ = false;
    double pacedFps_ = 0.0;
    double frameStep_ = 1.0;
    double frameAccum_ = 0.0;

    // Quick menu
    bool menuOpen_ = false;
    std::vector<std::string> discPaths_;        // Change Disc rows, in menu order
    std::array<ImTextureID, 6> slotPictures_{}; // Save/Load State pictures, slots 1-6
    uint64_t navHeldPrev_ = 0;                  // directional hold-repeat
    int navRepeatFrames_ = 0;
    static constexpr int kNavInitialDelayFrames = 14;
    static constexpr int kNavRepeatFrames = 6;

    // HUD frame counter
    int hudFrames_ = 0;
    float hudSeconds_ = 0.0f;
    float hudFps_ = 0.0f;
};

} // namespace Tico
