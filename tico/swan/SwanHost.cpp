/// @file SwanHost.cpp
/// @brief The HostInterface members SwanStation's core leaves to the
/// frontend, a software display and the audio stream, for the standalone
/// tico build (TICO_STANDALONE). What the libretro glue in src/libretro/ did
/// through retro_* callbacks is done here against SwanHost::Frontend.

#include "SwanHost.h"

#include "TicoLogger.h"

#include "common/align.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/analog_controller.h"
#include "core/digital_controller.h"
#include "core/gpu.h"
#include "core/gpu_hw_opengl.h"
#include "core/gpu_hw_vulkan.h"
#include "core/host_interface.h"
#include "core/settings.h"
#include "core/system.h"
#include "libretro/libretro_audio_stream.h"
#include "libretro/libretro_game_settings.h"
#include "libretro/libretro_settings_interface.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <type_traits>
#include <vector>

Log_SetChannel(SwanHost);

HostInterface g_host_interface_storage;
HostInterface *g_host_interface;

namespace SwanHost
{
namespace
{
Frontend s_frontend;
bool s_initialized = false;

uint32_t s_buttons[NUM_CONTROLLER_AND_CARD_PORTS] = {};
std::array<float, 4> s_axes[NUM_CONTROLLER_AND_CARD_PORTS] = {};
bool s_analog_combo_held = false;

// The setting value for a section/key, as libretro named core options.
const std::string *Option(const char *section, const char *key)
{
    if (!s_frontend.option)
        return nullptr;
    return s_frontend.option(StringUtil::StdStringFromFormat("swanstation_%s_%s", section, key));
}

void LogCallback(void *, const char *channel, const char *function, LogLevel level, const char *message)
{
    if (level <= LogLevel::Error)
        LOG_ERROR("SWAN", "%s: %s", function ? function : channel, message);
    else if (level <= LogLevel::Warning)
        LOG_WARN("SWAN", "%s: %s", channel, message);
    else
        LOG_INFO("SWAN", "%s: %s", channel, message);
}

std::string WithSlash(std::string dir)
{
    if (!dir.empty() && dir.back() != '/')
        dir += '/';
    return dir;
}
} // namespace

Frontend &Get()
{
    return s_frontend;
}

Renderer ParseRenderer(const std::string &value)
{
    if (value == "OpenGL")
        return Renderer::OpenGL;
    if (value == "Zink")
        return Renderer::Zink;
    if (value == "Software")
        return Renderer::Software;
    return Renderer::Vulkan; // "Vulkan", and "Auto"
}

bool Initialize()
{
    if (s_initialized)
        return true;
    Log::RegisterCallback(LogCallback, nullptr);
    s_initialized = g_host_interface->Initialize();
    return s_initialized;
}

void Shutdown()
{
    if (!s_initialized)
        return;
    g_host_interface->Shutdown();
    Log::UnregisterCallback(LogCallback, nullptr);
    s_initialized = false;
}

bool Boot(const std::string &path)
{
    // the renderer the frontend set up: settings are re-read for it
    g_host_interface->ReloadSettingsForBoot();
    auto params = std::make_shared<SystemBootParameters>();
    params->filename = path;
    params->force_software_renderer = s_frontend.renderer == Renderer::Software;
    return g_host_interface->BootSystem(std::move(params));
}

void Unload()
{
    g_host_interface->DestroySystem();
}

bool IsRunning()
{
    return System::IsValid();
}

void ReloadSettings()
{
    g_host_interface->ApplyFrontendSettings();
}

void SetPad(uint32_t port, uint32_t buttons, const std::array<float, 4> &axes)
{
    if (port >= NUM_CONTROLLER_AND_CARD_PORTS)
        return;
    s_buttons[port] = buttons;
    s_axes[port] = axes;
}

void GetRumble(uint32_t port, float &large, float &small)
{
    large = small = 0.0f;
    if (!System::IsValid() || g_settings.controller_types[port] != ControllerType::AnalogController)
        return;
    AnalogController *pad = static_cast<AnalogController *>(System::GetController(port));
    if (!pad)
        return;
    large = pad->GetVibrationMotorStrength(0);
    small = pad->GetVibrationMotorStrength(1);
}

namespace
{
template <typename T, size_t N>
void ApplyButtons(T *pad, uint32_t buttons, const std::array<std::pair<typename T::Button, Button>, N> &map)
{
    for (const auto &entry : map)
        pad->SetButtonState(entry.first, (buttons & (1u << entry.second)) != 0);
}

void UpdatePads()
{
    for (uint32_t port = 0; port < NUM_CONTROLLER_AND_CARD_PORTS; ++port)
    {
        const uint32_t buttons = s_buttons[port];
        switch (g_settings.controller_types[port])
        {
        case ControllerType::DigitalController:
        {
            static constexpr std::array<std::pair<DigitalController::Button, Button>, 14> map = {{
                {DigitalController::Button::Up, Up}, {DigitalController::Button::Down, Down},
                {DigitalController::Button::Left, Left}, {DigitalController::Button::Right, Right},
                {DigitalController::Button::Cross, Cross}, {DigitalController::Button::Circle, Circle},
                {DigitalController::Button::Square, Square}, {DigitalController::Button::Triangle, Triangle},
                {DigitalController::Button::L1, L1}, {DigitalController::Button::R1, R1},
                {DigitalController::Button::L2, L2}, {DigitalController::Button::R2, R2},
                {DigitalController::Button::Start, Start}, {DigitalController::Button::Select, Select},
            }};
            if (auto *pad = static_cast<DigitalController *>(System::GetController(port)))
                ApplyButtons(pad, buttons, map);
            break;
        }
        case ControllerType::AnalogController:
        {
            static constexpr std::array<std::pair<AnalogController::Button, Button>, 16> map = {{
                {AnalogController::Button::Up, Up}, {AnalogController::Button::Down, Down},
                {AnalogController::Button::Left, Left}, {AnalogController::Button::Right, Right},
                {AnalogController::Button::Cross, Cross}, {AnalogController::Button::Circle, Circle},
                {AnalogController::Button::Square, Square}, {AnalogController::Button::Triangle, Triangle},
                {AnalogController::Button::L1, L1}, {AnalogController::Button::R1, R1},
                {AnalogController::Button::L2, L2}, {AnalogController::Button::R2, R2},
                {AnalogController::Button::L3, L3}, {AnalogController::Button::R3, R3},
                {AnalogController::Button::Start, Start}, {AnalogController::Button::Select, Select},
            }};
            auto *pad = static_cast<AnalogController *>(System::GetController(port));
            if (!pad)
                break;
            ApplyButtons(pad, buttons, map);
            for (uint32_t axis = 0; axis < 4; ++axis)
                pad->SetAxisState(static_cast<int32_t>(axis), std::clamp(s_axes[port][axis], -1.0f, 1.0f));

            // The DualShock's Analog button, pressed with the combo picked in
            // settings (Controller > Analog combo), once per press.
            auto held = [&](Button b) { return (buttons & (1u << b)) != 0; };
            bool combo = false;
            switch (g_settings.controller_analog_combo)
            {
            case 1: combo = held(L1) && held(R1) && held(L3) && held(R3); break;
            case 2: combo = held(L1) && held(R1) && held(L2) && held(R2) && held(Start) && held(Select); break;
            case 3: combo = held(L1) && held(R1) && held(Select); break;
            case 4: combo = held(L1) && held(R1) && held(Start); break;
            case 5: combo = held(L1) && held(R1) && held(L3); break;
            case 6: combo = held(L1) && held(R1) && held(R3); break;
            case 7: combo = held(L2) && held(R2) && held(Select); break;
            case 8: combo = held(L2) && held(R2) && held(Start); break;
            case 9: combo = held(L2) && held(R2) && held(L3); break;
            case 10: combo = held(L2) && held(R2) && held(R3); break;
            case 11: combo = held(L3) && held(R3); break;
            default: break;
            }
            if (port == 0)
            {
                pad->SetButtonState(AnalogController::Button::Analog, combo && !s_analog_combo_held);
                s_analog_combo_held = combo;
            }
            break;
        }
        default:
            break;
        }
    }
}
} // namespace

void RunFrame()
{
    if (!System::IsValid())
        return;
    UpdatePads();
    System::RunFrame();
    if (HostDisplay *display = g_host_interface->GetDisplay())
        display->Render();
    if (g_settings.audio_fast_hook)
        g_host_interface->GetAudioStream()->UploadToFrontend();
}

float AspectRatio()
{
    if (HostDisplay *display = g_host_interface->GetDisplay())
        return display->GetDisplayAspectRatio();
    return g_settings.GetDisplayAspectRatioValue();
}

float FrameRate()
{
    return System::IsValid() ? System::GetVerticalFrequency() : 60.0f;
}

} // namespace SwanHost

using SwanHost::Renderer;

//==============================================================================
// Settings (the core reads them through LibretroSettingsInterface)
//==============================================================================

template <typename T, typename DefaultValueType>
static T GetVariable(const char *section, const char *key, DefaultValueType default_value)
{
    const std::string *value = SwanHost::Option(section, key);
    if (!value)
        return T(default_value);

    std::string text = *value;
    // Zink is OpenGL to the core; the frontend picked the driver already
    if (!std::strcmp(section, "GPU") && !std::strcmp(key, "Renderer") && text == "Zink")
        text = "OpenGL";

    if constexpr (std::is_same_v<T, std::string>)
        return text;
    else if constexpr (std::is_same_v<T, bool>)
        return StringUtil::Strcasecmp(text.c_str(), "true") == 0 || StringUtil::Strcasecmp(text.c_str(), "1") == 0 ||
               StringUtil::Strcasecmp(text.c_str(), "enabled") == 0;
    else if constexpr (std::is_same_v<T, float>)
        return std::strtof(text.c_str(), nullptr);
    else
        return StringUtil::FromChars<T>(text).value_or(T(default_value));
}

int LibretroSettingsInterface::GetIntValue(const char *section, const char *key, int default_value)
{
    return GetVariable<int>(section, key, default_value);
}

float LibretroSettingsInterface::GetFloatValue(const char *section, const char *key, float default_value)
{
    return GetVariable<float>(section, key, default_value);
}

bool LibretroSettingsInterface::GetBoolValue(const char *section, const char *key, bool default_value)
{
    return GetVariable<bool>(section, key, default_value);
}

std::string LibretroSettingsInterface::GetStringValue(const char *section, const char *key,
                                                      const char *default_value)
{
    return GetVariable<std::string>(section, key, default_value);
}

//==============================================================================
// Audio: the SPU's FIFO, drained into the frontend once per frame
//==============================================================================

void LibretroAudioStream::BeginWrite(SampleType **buffer_ptr, uint32_t *num_frames)
{
    *buffer_ptr = m_buffer.GetWritePointer();
    *num_frames = std::min(BUFFER_SIZE, m_buffer.GetContiguousSpace() / CHANNELS);
}

void LibretroAudioStream::EndWrite(uint32_t num_frames)
{
    m_buffer.AdvanceTail(num_frames * CHANNELS);
    if (m_silent_mode)
    {
        m_buffer.Remove(m_buffer.GetSize());
        return;
    }
    if (!g_settings.audio_fast_hook)
        UploadToFrontend();
}

void LibretroAudioStream::UploadToFrontend()
{
    // the FIFO is circular: up to two contiguous runs
    while (const uint32_t num_samples = m_buffer.GetContiguousSize())
    {
        if (SwanHost::Get().audio)
            SwanHost::Get().audio(m_buffer.GetReadPointer(), num_samples / CHANNELS);
        m_buffer.Remove(num_samples);
    }
}

//==============================================================================
// Software display: frames go to the frontend as pixels
//==============================================================================

namespace
{
class TicoSoftwareDisplay final : public HostDisplay
{
public:
    RenderAPI GetRenderAPI() const override { return RenderAPI::None; }
    void *GetRenderDevice() const override { return nullptr; }
    void *GetRenderContext() const override { return nullptr; }

    bool CreateRenderDevice(const WindowInfo &wi, std::string_view, bool, bool) override
    {
        m_window_info = wi;
        return true;
    }
    bool InitializeRenderDevice(std::string_view, bool, bool) override { return true; }
    void DestroyRenderDevice() override { ClearSoftwareCursor(); }
    bool ChangeRenderWindow(const WindowInfo &wi) override
    {
        m_window_info = wi;
        return true;
    }
    void ResizeRenderWindow(int32_t width, int32_t height) override
    {
        m_window_info.surface_width = width;
        m_window_info.surface_height = height;
    }
    bool CreateResources() override { return true; }
    void DestroyResources() override {}

    std::unique_ptr<HostDisplayTexture> CreateTexture(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                                      HostDisplayPixelFormat, const void *, uint32_t,
                                                      bool) override
    {
        return nullptr;
    }

    // what the frontend's shader chain takes: RGB565 and BGRA8 (XRGB8888)
    bool SupportsDisplayPixelFormat(HostDisplayPixelFormat format) const override
    {
        return format == HostDisplayPixelFormat::RGB565 || format == HostDisplayPixelFormat::BGRA8;
    }

    bool BeginSetDisplayPixels(HostDisplayPixelFormat format, uint32_t width, uint32_t height, void **out_buffer,
                               uint32_t *out_pitch) override
    {
        if (!SupportsDisplayPixelFormat(format))
            return false;
        // the active picture inside the full display area, borders black
        const uint32_t left = static_cast<uint32_t>(m_display_active_left);
        const uint32_t top = static_cast<uint32_t>(m_display_active_top);
        const uint32_t full_width = left + static_cast<uint32_t>(m_display_width - m_display_active_left -
                                                                 m_display_active_width) + width;
        const uint32_t full_height = top + static_cast<uint32_t>(m_display_height - m_display_active_top -
                                                                 m_display_active_height) + height;
        const uint32_t pixel_size = GetDisplayPixelFormatSize(format);
        const uint32_t pitch = Common::AlignUpPow2(full_width * pixel_size, 4);
        const size_t size = static_cast<size_t>(full_height) * pitch;
        if (m_frame.size() != size || m_format != format)
        {
            m_frame.assign(size, 0);
            m_format = format;
        }
        m_pitch = pitch;
        SetDisplayTexture(m_frame.data(), format, full_width, full_height, 0, 0, full_width, full_height);
        *out_buffer = m_frame.data() + top * pitch + left * pixel_size;
        *out_pitch = pitch;
        return true;
    }
    void EndSetDisplayPixels() override {}

    bool Render() override
    {
        const auto &frame = SwanHost::Get().softwareFrame;
        if (!frame)
            return true;
        if (HasDisplayTexture())
            frame(m_display_texture_handle, m_display_texture_view_width, m_display_texture_view_height, m_pitch,
                  m_display_texture_format);
        else
            frame(nullptr, 0, 0, 0, HostDisplayPixelFormat::Unknown);
        return true;
    }

private:
    std::vector<uint8_t> m_frame;
    uint32_t m_pitch = 0;
    HostDisplayPixelFormat m_format = HostDisplayPixelFormat::Unknown;
};
} // namespace

//==============================================================================
// HostInterface: the frontend half
//==============================================================================

HostInterface::HostInterface()
{
    g_host_interface = this;
}

HostInterface::~HostInterface()
{
    if (System::IsValid())
        DestroySystem();
    g_host_interface = nullptr;
}

bool HostInterface::Initialize()
{
    LoadSettings();
    FixIncompatibleSettings(true);
    UpdateLogging();
    return true;
}

void HostInterface::Shutdown()
{
    if (!System::IsShutdown())
        System::Shutdown();
}

void HostInterface::ReportError(const char *message)
{
    AddFormattedOSDMessage(10.0f, "ERROR: %s", message);
    Log_ErrorPrint(message);
}

void HostInterface::ReportMessage(const char *message)
{
    AddOSDMessage(message, 5.0f);
    Log_InfoPrint(message);
}

bool HostInterface::ConfirmMessage(const char *message)
{
    Log_InfoPrintf("Confirm: %s", message);
    return false;
}

void HostInterface::GetGameInfo(const char *path, CDImage *image, std::string *code, std::string *title)
{
    *title = FileSystem::GetFileTitleFromPath(path);
    if (image)
        *code = System::GetGameCodeForImage(image, true);
    else
        code->clear();
}

std::string HostInterface::GetSharedMemoryCardPath(uint32_t slot) const
{
    return StringUtil::StdStringFromFormat("%sshared_card_%u.mcd", SwanHost::WithSlash(SwanHost::Get().savesDir).c_str(),
                                           slot + 1);
}

std::string HostInterface::GetGameMemoryCardPath(const char *game_code, uint32_t slot) const
{
    return StringUtil::StdStringFromFormat("%s%s_%u.mcd", SwanHost::WithSlash(SwanHost::Get().savesDir).c_str(),
                                           game_code, slot + 1);
}

std::string HostInterface::GetShaderCacheBasePath() const
{
    return SwanHost::WithSlash(SwanHost::Get().cacheDir);
}

std::string HostInterface::GetStringSettingValue(const char *section, const char *key, const char *default_value)
{
    LibretroSettingsInterface si;
    return si.GetStringValue(section, key, default_value);
}

void HostInterface::DisplayLoadingScreen(const char *message, int progress_min, int progress_max, int progress_value)
{
    if (!message || !*message || !SwanHost::Get().osd)
        return;
    if (progress_max > 0 && progress_value >= 0)
        SwanHost::Get().osd(StringUtil::StdStringFromFormat("%s %d/%d", message, progress_value, progress_max), 1.0f);
    else
        SwanHost::Get().osd(message, 1.0f);
}

void HostInterface::AddOSDMessage(std::string message, float duration)
{
    if (!g_settings.display_show_osd_messages || !SwanHost::Get().osd)
        return;
    SwanHost::Get().osd(message, duration);
}

std::string HostInterface::GetBIOSDirectory()
{
    return SwanHost::Get().systemDir;
}

void HostInterface::UpdateLogging()
{
    Log::SetFilterLevel(g_settings.log_level);
}

bool HostInterface::UpdateGameSettings()
{
    std::unique_ptr<GameSettings::Entry> new_game_settings;
    if (!System::IsShutdown() && !System::GetRunningCode().empty())
        new_game_settings = GetSettingsForGame(System::GetRunningCode());
    if (new_game_settings == m_game_settings)
        return false;
    m_game_settings = std::move(new_game_settings);
    return true;
}

void HostInterface::ApplyGameSettings()
{
    if (!g_settings.apply_game_settings || !m_game_settings)
        return;
    m_game_settings->ApplySettings(System::GetState() == System::State::Starting);
}

void HostInterface::LoadSettings()
{
    LibretroSettingsInterface si;
    g_settings.Load(si);

    // the renderer is the frontend's: its display is already set up for it
    switch (SwanHost::Get().renderer)
    {
    case Renderer::Vulkan: g_settings.gpu_renderer = GPURenderer::HardwareVulkan; break;
    case Renderer::OpenGL:
    case Renderer::Zink: g_settings.gpu_renderer = GPURenderer::HardwareOpenGL; break;
    case Renderer::Software: g_settings.gpu_renderer = GPURenderer::Software; break;
    }

    // overclock as a percentage, MSAA as "<samples>[-ssaa]", as the libretro options spell them
    const uint32_t overclock_percent = static_cast<uint32_t>(std::max(si.GetIntValue("CPU", "Overclock", 100), 1));
    Settings::CPUOverclockPercentToFraction(overclock_percent, &g_settings.cpu_overclock_numerator,
                                            &g_settings.cpu_overclock_denominator);
    g_settings.cpu_overclock_enable = (overclock_percent != 100);
    g_settings.UpdateOverclockActive();
    const std::string msaa = si.GetStringValue("GPU", "MSAA", "1");
    g_settings.gpu_multisamples = StringUtil::FromChars<uint32_t>(msaa).value_or(1);
    g_settings.gpu_per_sample_shading = StringUtil::EndsWith(msaa, "-ssaa");

    for (uint32_t i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; ++i)
    {
        const SwanHost::PadType type = i < 2 ? SwanHost::Get().pads[i] : SwanHost::PadType::None;
        g_settings.controller_types[i] = type == SwanHost::PadType::DualShock ? ControllerType::AnalogController
                                         : type == SwanHost::PadType::Digital ? ControllerType::DigitalController
                                                                              : ControllerType::None;
        g_settings.memory_card_paths[i] = GetSharedMemoryCardPath(i);
    }
}

void HostInterface::ReloadSettingsForBoot()
{
    LoadSettings();
    FixIncompatibleSettings(false);
}

void HostInterface::ApplyFrontendSettings()
{
    Settings old_settings(std::move(g_settings));
    LoadSettings();
    ApplyGameSettings();
    FixIncompatibleSettings(false);

    if (System::IsValid())
    {
        // these only take effect when the game starts again
        g_settings.gpu_renderer = old_settings.gpu_renderer;
        g_settings.memory_card_types[0] = old_settings.memory_card_types[0];
        g_settings.gpu_use_software_renderer_for_readbacks = old_settings.gpu_use_software_renderer_for_readbacks;
        g_settings.audio_fast_hook = old_settings.audio_fast_hook;
    }
    CheckForSettingsChanges(old_settings);
}

void HostInterface::OnRunningGameChanged(const std::string &path, CDImage *image, const std::string &game_code,
                                         const std::string &game_title)
{
    if (UpdateGameSettings())
        ApplyFrontendSettings();
}

void HostInterface::AcquireHostDisplay()
{
    WindowInfo wi;
    wi.surface_width = VRAM_WIDTH;
    wi.surface_height = VRAM_HEIGHT;

    std::unique_ptr<HostDisplay> display;
    switch (SwanHost::Get().renderer)
    {
    case Renderer::Vulkan:
        display = std::make_unique<LibretroVulkanHostDisplay>();
        break;
    case Renderer::OpenGL:
    case Renderer::Zink:
        // the frontend's EGL context is current on this thread
        display = std::make_unique<LibretroOpenGLHostDisplay>();
        break;
    case Renderer::Software:
        display = std::make_unique<TicoSoftwareDisplay>();
        break;
    }
    if (!display->CreateRenderDevice(wi, {}, false, false) ||
        !display->InitializeRenderDevice(GetShaderCacheBasePath(), false, false))
    {
        Log_ErrorPrintf("Failed to create the %s display, falling back to software",
                        Settings::GetRendererName(g_settings.gpu_renderer));
        display = std::make_unique<TicoSoftwareDisplay>();
        display->CreateRenderDevice(wi, {}, false, false);
        g_settings.gpu_renderer = GPURenderer::Software;
    }
    m_display = std::move(display);
}

void HostInterface::ReleaseHostDisplay()
{
    if (!m_display)
        return;
    m_display->DestroyRenderDevice();
    m_display.reset();
}

void HostInterface::OnControllerTypeChanged(uint32_t slot) {}

// libretro had to be told the picture's new size; the frontend reads the
// display's aspect ratio every frame instead.
void HostInterface::UpdateGeometry() {}
