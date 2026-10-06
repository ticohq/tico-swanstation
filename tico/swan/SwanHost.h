/// @file SwanHost.h
/// @brief SwanStation driven directly by the tico frontend, without libretro.
///
/// The core's HostInterface (settings, paths, OSD, audio, display, input) is
/// implemented in SwanHost.cpp on top of what the frontend registers here.
/// Everything runs on the frontend's thread: Boot, RunFrame and the state
/// calls all go through System, as the libretro glue used to.
#pragma once

#include "core/host_display.h"
#include "core/types.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace SwanHost
{

/// The renderer, picked in settings and fixed for the run: Vulkan shares the
/// frontend's NVK device; OpenGL and Zink render in the frontend's EGL
/// context (native nvc0 or Zink over NVK); Software uploads frames.
enum class Renderer
{
    Vulkan,
    OpenGL,
    Zink,
    Software,
};

/// The setting's value ("Vulkan", "OpenGL", "Zink", "Software", "Auto").
Renderer ParseRenderer(const std::string &value);

/// PlayStation pad buttons, one bit each in a pad's button mask.
enum Button : uint32_t
{
    Up, Down, Left, Right, Cross, Circle, Square, Triangle,
    L1, R1, L2, R2, L3, R3, Start, Select, ButtonCount
};

/// The pad plugged into a port.
enum class PadType
{
    None,
    Digital,
    DualShock,
};

struct Frontend
{
    /// A setting by its full key (swanstation_<Section>_<Key>); null when unset.
    std::function<const std::string *(const std::string &key)> option;
    /// An on-screen message from the core.
    std::function<void(const std::string &message, float seconds)> osd;
    /// Stereo 44.1 kHz samples, once per frame.
    std::function<void(const int16_t *samples, uint32_t frames)> audio;
    /// A software-rendered frame (RGB565 or BGRA8), or null to repeat the last.
    std::function<void(const void *data, uint32_t width, uint32_t height, uint32_t pitch,
                       HostDisplayPixelFormat format)>
        softwareFrame;

    std::string systemDir; ///< BIOS images
    std::string savesDir;  ///< memory cards
    std::string cacheDir;  ///< the GPU shader cache
    Renderer renderer = Renderer::Software;
    std::array<PadType, 2> pads = {PadType::DualShock, PadType::DualShock};
};

Frontend &Get();

/// Brings up the host (logging, settings). Call once, before Boot.
bool Initialize();
void Shutdown();

/// Boots a disc image, EXE or PSF; empty boots the BIOS.
bool Boot(const std::string &path);
/// Shuts the system down (memory cards are flushed).
void Unload();
bool IsRunning();

/// Settings changed: re-read them; the core applies what it can live.
void ReloadSettings();

/// One port's buttons (a Button bit mask) and sticks (LeftX, LeftY, RightX,
/// RightY, -1..1), applied before the next frame.
void SetPad(uint32_t port, uint32_t buttons, const std::array<float, 4> &axes);
/// The pad's vibration motors (large, small), 0..1, after a frame.
void GetRumble(uint32_t port, float &large, float &small);

/// Runs one frame: input, emulation, display, audio.
void RunFrame();

/// The picture: its aspect ratio, the core's frame rate.
float AspectRatio();
float FrameRate();

} // namespace SwanHost
