/// @file SwanRuntime.cpp
/// @brief SwanStation CoreRuntime: brings up the renderer the settings name
/// (Vulkan on the core's own device, OpenGL, Zink or Software), boots the
/// game, and draws it under the overlay each frame.

#include "SwanRuntime.h"

#include "SwanBios.h"
#include "SwanCheats.h"
#include "SwanDiscs.h"
#include "SwanLibrary.h"
#include "SwanSaves.h"
#include "SwanShaders.h"
#include "TicoAudio.h"
#include "TicoChainload.h"
#include "TicoConfig.h"
#include "TicoCore.h"
#include "TicoGL.h"
#include "TicoLogger.h"
#include "TicoOverlayHost.h"
#include "TicoShaderChain.h"
#include "TicoVulkan.h"
#include "overlay/imgui_overlay.h"
#include "overlay/overlay_ui.h"
#include "overlay/tico_config.h"
#include "overlay/translation_manager.h"
#include "swan/SwanHost.h"

#include "common/vulkan/context.h"
#include "core/gpu_hw_opengl.h"
#include "core/gpu_hw_vulkan.h"
#include "vulkan_loader.h"

#include "deps/stb/stb_image.h"
#include "deps/stb/stb_image_write.h"
#include "imgui.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace OverlayUI = SwitchFrontend::OverlayUI;
namespace ImGuiOverlay = SwitchFrontend::ImGuiOverlay;
namespace OverlayConfig = SwitchFrontend::TicoConfig;

namespace
{
TicoCore *s_core = nullptr; // for the renderer hooks, which are plain functions

// The core's last software frame, kept for the picture saved with a save state.
struct
{
    std::vector<uint8_t> data;
    unsigned width = 0;
    unsigned height = 0;
    size_t pitch = 0;
    retro_pixel_format format = RETRO_PIXEL_FORMAT_RGB565;
} s_lastFrame;

// The OpenGL renderer's finished frame, in the shared EGL context: drawn
// straight from its texture (no shader chain on OpenGL).
struct
{
    uint32_t texture = 0;
    uint32_t width = 0;
    uint32_t height = 0;
} s_glFrame;

void SoftwareFrame(const void *data, unsigned width, unsigned height, size_t pitch, retro_pixel_format format)
{
    if (TicoShaderChain *chain = SwanShaders::Chain())
        chain->SetSourceFrame(data, width, height, pitch, format);
    if (data) // null repeats the previous frame
    {
        s_lastFrame.data.assign((const uint8_t *)data, (const uint8_t *)data + pitch * height);
        s_lastFrame.width = width;
        s_lastFrame.height = height;
        s_lastFrame.pitch = pitch;
        s_lastFrame.format = format;
    }
}

// The Vulkan renderer's finished frame, on the shared device: the shader
// chain copies it in this frame's command buffer (same queue, later
// submission). A null image keeps the last frame up.
void VulkanFrame(VkImage image, VkImageView, uint32_t width, uint32_t height)
{
    TicoShaderChain *chain = SwanShaders::Chain();
    if (!image || !chain)
        return;
    chain->SetSourceImage(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, width, height);
    if (s_core)
        s_core->SetFrameSize(static_cast<int>(width), static_cast<int>(height));
}

// Texture 0 keeps the last frame up.
void GLFrame(uint32_t texture, uint32_t width, uint32_t height)
{
    if (!texture)
        return;
    s_glFrame = {texture, width, height};
    if (s_core)
        s_core->SetFrameSize(static_cast<int>(width), static_cast<int>(height));
}
} // namespace

void (*g_tico_vulkan_frame)(VkImage, VkImageView, uint32_t, uint32_t) = VulkanFrame;
void (*g_tico_gl_frame)(uint32_t, uint32_t, uint32_t) = GLFrame;
void *(*g_tico_gl_proc_address)(const char *) = TicoGL::GetProcAddress;

namespace Tico
{

namespace
{

// One pixel of the last frame as RGBA.
void FramePixel(unsigned x, unsigned y, uint8_t *out)
{
    const uint8_t *row = s_lastFrame.data.data() + y * s_lastFrame.pitch;
    switch (s_lastFrame.format)
    {
    case RETRO_PIXEL_FORMAT_XRGB8888:
    {
        uint32_t p;
        memcpy(&p, row + x * 4, 4);
        out[0] = (p >> 16) & 0xFF;
        out[1] = (p >> 8) & 0xFF;
        out[2] = p & 0xFF;
        break;
    }
    case RETRO_PIXEL_FORMAT_0RGB1555:
    {
        uint16_t p;
        memcpy(&p, row + x * 2, 2);
        out[0] = ((p >> 10) & 0x1F) * 255 / 31;
        out[1] = ((p >> 5) & 0x1F) * 255 / 31;
        out[2] = (p & 0x1F) * 255 / 31;
        break;
    }
    default: // RGB565
    {
        uint16_t p;
        memcpy(&p, row + x * 2, 2);
        out[0] = ((p >> 11) & 0x1F) * 255 / 31;
        out[1] = ((p >> 5) & 0x3F) * 255 / 63;
        out[2] = (p & 0x1F) * 255 / 31;
        break;
    }
    }
    out[3] = 255;
}

// Saves the last frame as a PNG beside a save state, shrunk to fit 256x192
// (box filtered), just big enough for the Save/Load State panel.
void SaveStatePicture(const std::string &path)
{
    // The hardware renderers' frames never reach the CPU: read the picture
    // back from the shader chain's output or the OpenGL texture instead.
    TicoShaderChain *chain = SwanShaders::Chain();
    if (s_lastFrame.data.empty() && (chain || s_glFrame.texture))
    {
        std::vector<uint8_t> rgba;
        uint32_t width = 0, height = 0;
        bool read;
        if (TicoVulkan::IsOpenGL())
        {
            width = s_glFrame.width;
            height = s_glFrame.height;
            read = TicoGL::ReadTexture(s_glFrame.texture, width, height, rgba);
        }
        else
            read = chain->ReadOutput(rgba, width, height);
        if (read && width && height)
        {
            for (size_t i = 0; i + 3 < rgba.size(); i += 4)
                std::swap(rgba[i], rgba[i + 2]); // to the XRGB8888 FramePixel reads
            s_lastFrame.data = std::move(rgba);
            s_lastFrame.width = width;
            s_lastFrame.height = height;
            s_lastFrame.pitch = (size_t)width * 4;
            s_lastFrame.format = RETRO_PIXEL_FORMAT_XRGB8888;
            SaveStatePicture(path);
            s_lastFrame.data.clear();
        }
        return;
    }
    if (s_lastFrame.data.empty() || !s_lastFrame.width || !s_lastFrame.height)
        return;
    const unsigned srcW = s_lastFrame.width, srcH = s_lastFrame.height;
    const float fit = std::min({1.0f, 256.0f / srcW, 192.0f / srcH});
    const unsigned dstW = std::max(1u, (unsigned)(srcW * fit));
    const unsigned dstH = std::max(1u, (unsigned)(srcH * fit));
    std::vector<uint8_t> out(dstW * dstH * 4);
    for (unsigned y = 0; y < dstH; y++)
    {
        const unsigned y0 = y * srcH / dstH, y1 = std::max(y0 + 1, (y + 1) * srcH / dstH);
        for (unsigned x = 0; x < dstW; x++)
        {
            const unsigned x0 = x * srcW / dstW, x1 = std::max(x0 + 1, (x + 1) * srcW / dstW);
            unsigned sum[3] = {0, 0, 0}, n = 0;
            for (unsigned sy = y0; sy < y1; sy++)
                for (unsigned sx = x0; sx < x1; sx++)
                {
                    uint8_t px[4];
                    FramePixel(sx, sy, px);
                    sum[0] += px[0];
                    sum[1] += px[1];
                    sum[2] += px[2];
                    n++;
                }
            uint8_t *dst = &out[(y * dstW + x) * 4];
            dst[0] = sum[0] / n;
            dst[1] = sum[1] / n;
            dst[2] = sum[2] / n;
            dst[3] = 255;
        }
    }
    stbi_write_png(path.c_str(), (int)dstW, (int)dstH, 4, out.data(), (int)dstW * 4);
}

} // namespace

// Adapts TicoCore and the renderer to the overlay's IOverlayHost /
// IOverlayRAHost interfaces.
class SwanOverlayHost final : public IOverlayHost, public IOverlayRAHost
{
public:
    explicit SwanOverlayHost(TicoCore *core) : core_(core) {}

    std::string GetGamePath() override { return core_ ? core_->GetGamePath() : std::string(); }
    bool IsGameLoaded() override { return core_ && core_->IsGameLoaded(); }

    bool StateSlotExists(int slot) override
    {
        struct stat st;
        return stat(StatePath(slot).c_str(), &st) == 0;
    }
    void SaveStateSlot(int slot) override
    {
        if (!core_)
            return;
        const std::string path = StatePath(slot);
        if (core_->SaveState(path))
            SaveStatePicture(path + ".png");
    }

    std::string SlotStatePath(int slot) const { return StatePath(slot); }
    void LoadStateSlot(int slot) override
    {
        if (core_)
            core_->LoadState(StatePath(slot));
    }
    void SwapDisc(const std::string &path) override
    {
        if (core_)
            core_->SwapDiskByPath(path);
    }

    ImTextureID CreateTextureRGBA(const unsigned char *rgba, int width, int height) override
    {
        return TicoVulkan::CreateTextureRGBA(rgba, width, height);
    }
    void DestroyTexture(ImTextureID tex) override
    {
        if (tex)
            TicoVulkan::DestroyTexture(tex);
    }

    IOverlayRAHost *RA() override { return this; }

    // IOverlayRAHost, backed by TicoCore's RA state.
    std::mutex &Mutex() override { return core_->m_raCallbackMutex; }
    std::vector<RANotification> &Notifications() override { return core_->m_raNotifications; }
    RAAlertPosition AlertPosition() const override { return core_->m_raAlertPosition; }
    ImTextureID IconTexture() const override { return core_->m_raIconTexture; }
    void SetIconTexture(ImTextureID tex) override { core_->m_raIconTexture = tex; }
    ImTextureID BadgeTexture(const std::string &badge) const override
    {
        auto it = core_->m_raBadgeCache.find(badge);
        return it != core_->m_raBadgeCache.end() ? it->second : (ImTextureID)0;
    }

private:
    std::string StatePath(int slot) const
    {
        std::string name = core_ ? core_->GetGamePath() : std::string();
        const size_t slash = name.find_last_of("/\\");
        if (slash != std::string::npos)
            name = name.substr(slash + 1);
        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos)
            name = name.substr(0, dot);
        const std::string stateDir = TicoConfig::StatesPath();
        TicoConfig::MakeDirs(stateDir);
        return stateDir + name + ".state" + std::to_string(slot);
    }

    TicoCore *core_ = nullptr;
};

namespace
{

#ifdef __SWITCH__
u8 s_lastOperationMode = 255;

// Sizes the window to the mode and crops from the top-left, so the swapchain
// always matches what is on screen.
void UpdateScreenMode()
{
    const u8 op = appletGetOperationMode();
    if (op == s_lastOperationMode)
        return;
    const bool handheld = op == AppletOperationMode_Handheld;
    const u32 w = handheld ? 1280 : 1920, h = handheld ? 720 : 1080;
    nwindowSetDimensions(nwindowGetDefault(), w, h);
    nwindowSetCrop(nwindowGetDefault(), 0, 0, w, h);
    TicoVulkan::Resize(w, h);
    LOG_INFO("DISPLAY", "Mode -> %s (%ux%u)", handheld ? "Handheld" : "Docked", w, h);
    s_lastOperationMode = op;
}

void DisplaySize(uint32_t &w, uint32_t &h)
{
    const bool handheld = appletGetOperationMode() == AppletOperationMode_Handheld;
    w = handheld ? 1280 : 1920;
    h = handheld ? 720 : 1080;
}
#else
void UpdateScreenMode() {}
void DisplaySize(uint32_t &w, uint32_t &h)
{
    w = 1280;
    h = 720;
}
#endif

// TicoCore::SetAudioCallbacks takes plain function pointers, so route them
// through a file-static audio sink set up in Initialize().
TicoAudio *s_audio = nullptr;

void AudioSampleCallback(int16_t left, int16_t right)
{
    if (s_audio)
        s_audio->PushSample(left, right);
}

size_t AudioSampleBatchCallback(const int16_t *data, size_t frames)
{
    return s_audio ? s_audio->PushSamples(data, frames) : frames;
}

void AudioFlushCallback()
{
    if (s_audio)
        s_audio->Flush();
}

// The overlay's message for key in the language tico is set to, with one %d
// filled in.
std::string TrFormat(const char *key, int value)
{
    const std::string format = SwitchFrontend::OverlayTranslation::tr(key);
    char text[256];
    std::snprintf(text, sizeof(text), format.c_str(), value);
    return text;
}

// The same with two %s, filled in order (not snprintf: a translation with
// another % sequence must not read off the stack).
std::string TrFormat2(const char *key, const std::string &first, const std::string &second)
{
    std::string text = SwitchFrontend::OverlayTranslation::tr(key);
    for (const std::string *value : {&first, &second})
    {
        const size_t at = text.find("%s");
        if (at == std::string::npos)
            break;
        text.replace(at, 2, *value);
    }
    return text;
}

#ifdef __SWITCH__
bool PromptKeyboard(const char *header, const std::string &initial, size_t maxLength, std::string &output)
{
    SwkbdConfig keyboard;
    if (R_FAILED(swkbdCreate(&keyboard, 0)))
        return false;
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetHeaderText(&keyboard, header);
    if (!initial.empty())
        swkbdConfigSetInitialText(&keyboard, initial.c_str());
    swkbdConfigSetStringLenMax(&keyboard, static_cast<u32>(maxLength));
    std::vector<char> text(maxLength + 1);
    const Result result = swkbdShow(&keyboard, text.data(), text.size());
    swkbdClose(&keyboard);
    if (R_FAILED(result))
        return false;
    output = text.data();
    return true;
}
#endif

// The renderer picked in Settings > Video, fixed for the run.
SwanHost::Renderer ConfiguredRenderer()
{
    return SwanHost::ParseRenderer(OverlayConfig::GetConfigValue("swanstation_GPU_Renderer", "Vulkan"));
}

// SwanStation's Vulkan renderer creates the device (with what it needs) on
// the frontend's instance and surface; the frontend draws on it too.
void ShareVulkanDevice()
{
    TicoVulkan::DeviceFactory factory;
    factory.create = [](VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface, VkDevice &device,
                        VkQueue &queue, uint32_t &family) {
        if (!Vulkan::LoadVulkanLibrary() || !Vulkan::LoadVulkanInstanceFunctions(instance))
            return false;
        if (!Vulkan::Context::CreateFromExistingInstance(instance, gpu, surface, false, false, false))
            return false;
        device = g_vulkan_context->GetDevice();
        queue = g_vulkan_context->GetGraphicsQueue();
        family = g_vulkan_context->GetGraphicsQueueFamilyIndex();
        return true;
    };
    factory.destroy = [] {
        Vulkan::Context::Destroy();
        Vulkan::UnloadVulkanLibrary();
    };
    TicoVulkan::SetDeviceFactory(std::move(factory));
}

// The game's on-screen rectangle, from the Display tab: Integer scales the
// frame by 1x, 2x or the largest that fits ("Auto"); Display fits an aspect
// ratio (4:3, 16:9, the core's own "Original") or stretches.
ImVec4 ComputeGameRect(const TicoCore *core, ImVec2 displaySize)
{
    const int width = core ? core->GetFrameWidth() : 320;
    const int height = core ? core->GetFrameHeight() : 240;
    const float aspectRatio = core ? core->GetAspectRatio() : 4.0f / 3.0f;
    const std::string mode = OverlayConfig::GetConfigValue("display_mode", "Display");
    const std::string size = OverlayConfig::GetConfigValue("display_size", "4:3");

    const float baseW = width > 0 ? static_cast<float>(width) : 320.0f;
    const float baseH = height > 0 ? static_cast<float>(height) : 240.0f;
    float dstWidth = displaySize.x;
    float dstHeight = displaySize.y;
    if (mode == "Integer")
    {
        int scale;
        if (size == "1x")
            scale = 1;
        else if (size == "2x")
            scale = 2;
        else
            scale = std::max(1, std::min(static_cast<int>(displaySize.x / baseW),
                                         static_cast<int>(displaySize.y / baseH)));
        dstWidth = std::min(displaySize.x, baseW * scale);
        dstHeight = std::min(displaySize.y, baseH * scale);
    }
    else if (size != "Stretch")
    {
        float ar = aspectRatio > 0.0f ? aspectRatio : baseW / baseH;
        if (size == "4:3")
            ar = 4.0f / 3.0f;
        else if (size == "16:9")
            ar = 16.0f / 9.0f;
        if (ar > displaySize.x / displaySize.y)
        {
            dstWidth = displaySize.x;
            dstHeight = displaySize.x / ar;
        }
        else
        {
            dstHeight = displaySize.y;
            dstWidth = displaySize.y * ar;
        }
    }
    dstWidth = std::floor(dstWidth);
    dstHeight = std::floor(dstHeight);
    return ImVec4(std::floor((displaySize.x - dstWidth) / 2.0f), std::floor((displaySize.y - dstHeight) / 2.0f),
                  dstWidth, dstHeight);
}

std::string GameTitleFromPath(const std::string &path)
{
    std::string title = path;
    const size_t slash = title.find_last_of("/\\");
    if (slash != std::string::npos)
        title = title.substr(slash + 1);
    const size_t dot = title.find_last_of('.');
    if (dot != std::string::npos)
        title = title.substr(0, dot);
    return title.empty() ? "SwanStation" : title;
}

// A Switch button by the name settings.json stores, as tico's positional
// PadButton bit; 0 for "None".
uint64_t PadBitFor(const std::string &name)
{
    static const std::pair<const char *, uint64_t> kNames[] = {
        {"A", Pad_B},         {"B", Pad_A},        {"X", Pad_Y},         {"Y", Pad_X},
        {"L", Pad_L},         {"R", Pad_R},        {"ZL", Pad_L2},       {"ZR", Pad_R2},
        {"Plus", Pad_Start},  {"Minus", Pad_Select}, {"StickL", Pad_L3}, {"StickR", Pad_R3},
        {"Up", Pad_Up},       {"Down", Pad_Down},  {"Left", Pad_Left},   {"Right", Pad_Right},
    };
    for (const auto &entry : kNames)
        if (name == entry.first)
            return entry.second;
    return 0;
}

// PlayStation buttons, with the Switch button each sits on by default: the
// face buttons by position (Circle east on A, Cross south on B, Triangle on
// X, Square on Y), the shoulders on their namesakes and L3/R3 on the sticks.
struct ButtonMapping
{
    const char *key;
    const char *fallback;
    unsigned button; // SwanHost::Button
};
const ButtonMapping kButtonMappings[] = {
    {"map_circle", "A", SwanHost::Circle},     {"map_cross", "B", SwanHost::Cross},
    {"map_triangle", "X", SwanHost::Triangle}, {"map_square", "Y", SwanHost::Square},
    {"map_l1", "L", SwanHost::L1},             {"map_r1", "R", SwanHost::R1},
    {"map_l2", "ZL", SwanHost::L2},            {"map_r2", "ZR", SwanHost::R2},
    {"map_l3", "StickL", SwanHost::L3},        {"map_r3", "StickR", SwanHost::R3},
    {"map_start", "Plus", SwanHost::Start},    {"map_select", "Minus", SwanHost::Select},
    {"map_up", "Up", SwanHost::Up},            {"map_down", "Down", SwanHost::Down},
    {"map_left", "Left", SwanHost::Left},      {"map_right", "Right", SwanHost::Right},
};

// The PlayStation has two pad ports (no multitap here).
constexpr unsigned kPorts = 2;

// The display refreshes at 60 Hz and vsync paces the loop.
constexpr double kDisplayHz = 60.0;

} // namespace

SwanRuntime::SwanRuntime(LogCallback log) : log_(std::move(log)) {}

SwanRuntime::~SwanRuntime() = default;

bool SwanRuntime::Configure(const LaunchInfo &launch)
{
    // Without a game (e.g. from the homebrew menu) the library lists the ROM
    // folders; a game picked there runs in a fresh launch of this NRO, marked
    // --from-library so Exit Game comes back to the list.
    romPath_ = launch.contentPath;
    standalone_ = romPath_.empty();
    argv0_ = launch.argc > 0 && launch.argv[0] ? launch.argv[0] : "";
    for (int i = 1; i < launch.argc && launch.argv[i]; ++i)
        if (std::string(launch.argv[i]) == "--from-library")
            fromLibrary_ = true;
    titleArg_ = launch.title;
    TicoConfig::SetSlug(launch.argc > 3 && launch.argv[3] ? launch.argv[3] : "", romPath_);
    return true;
}

bool SwanRuntime::InitAudio()
{
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = TicoAudio::SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = TicoAudio::CHANNELS;
    want.samples = 2048;
    want.callback = nullptr;
    audioDevice_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (audioDevice_ == 0)
    {
        LOG_ERROR("AUDIO", "SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return false;
    }
    LOG_INFO("AUDIO", "SDL_QueueAudio initialized (deviceID=%u, freq=%d)", audioDevice_, have.freq);
    return true;
}

bool SwanRuntime::Initialize(const LaunchInfo &)
{
    // Tico::Main is SDL-free, so the runtime brings up what it needs.
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0)
        LOG_WARN("HOME", "SDL_Init(AUDIO|TIMER) failed: %s", SDL_GetError());

    InitAudio();

    TicoConfig::MakeDirs(TicoConfig::SavesPath());
    TicoConfig::MakeDirs(TicoConfig::StatesPath());
    TicoConfig::MakeDirs(TicoConfig::SystemPath());
    TicoConfig::MakeDirs(TicoConfig::CachePath()); // the renderer's pipeline cache

    core_ = std::make_unique<TicoCore>();
    s_core = core_.get();
    core_->SetAudioCallbacks(AudioSampleCallback, AudioSampleBatchCallback, AudioFlushCallback);
    core_->SetVideoCallback(SoftwareFrame);
    // settings.json is the one settings definition: every option it lists
    // reaches the core with its default when the config file does not set it.
    // The overlay reads the file after the core, which writes the defaults
    // (renderer included) on a first run, so its saves keep them.
    core_->EnsureConfigLoaded();
    OverlayConfig::ReloadConfig();
    // this game's own settings (Settings > This Game), if it has them, over
    // the core's; none in the library
    OverlayConfig::SetGame(romPath_);
    ApplySettingsToCore();

    // The renderer is fixed for the run: Vulkan shares its device with the
    // frontend, OpenGL and Zink run the frontend on OpenGL too (the two APIs
    // cannot share images), Software uploads frames to the frontend's own
    // Vulkan device.
    const SwanHost::Renderer renderer = ConfiguredRenderer();
    if (renderer == SwanHost::Renderer::Vulkan)
        ShareVulkanDevice();
    else if (renderer == SwanHost::Renderer::OpenGL || renderer == SwanHost::Renderer::Zink)
        TicoVulkan::UseOpenGL(renderer == SwanHost::Renderer::Zink);

    uint32_t w = 0, h = 0;
    DisplaySize(w, h);
#ifdef __SWITCH__
    nwindowSetDimensions(nwindowGetDefault(), w, h);
#endif
    if (!TicoVulkan::Init(nullptr, w, h))
    {
        LOG_ERROR("HOME", "Renderer initialization failed");
        return false;
    }
    UpdateScreenMode();

    audio_ = std::make_unique<TicoAudio>();
    s_audio = audio_.get();
    if (!audio_->Init(audioDevice_))
        LOG_WARN("HOME", "TicoAudio init failed");

    if (!core_->Init())
    {
        LOG_ERROR("HOME", "TicoCore::Init failed");
        return false;
    }
    return true;
}

bool SwanRuntime::LoadContent(const std::string &path)
{
    romPath_ = path.empty() ? romPath_ : path;
    if (standalone_)
    {
        if (InitOverlay(std::string()))
        {
            SwanLibrary::Register([this](const std::string &game, const std::string &slug) {
                LaunchSelf(argv0_.c_str(), {game, std::string(), slug, "--from-library"}, log_);
                exitRequested_ = true;
            });
            OverlayUI::SetGameTitle("SwanStation");
            OverlayUI::SetLibraryMode(true);
            OpenMenu();
        }
        lastTicks_ = SDL_GetTicks();
        return true;
    }
    LOG_INFO("HOME", "Loading ROM: %s", romPath_.c_str());

    if (!InitOverlay(romPath_))
        LOG_WARN("HOME", "Overlay init failed; continuing without Tico overlay");

    // Without a BIOS it knows the core boots its own OpenBIOS, which some
    // games do not work with: say what is missing and where it goes first,
    // unless Settings > System says to use OpenBIOS (tico's own check before
    // launching waives the BIOS then too, see module.json).
    const SwanBios::Status bios = SwanBios::Check();
    if (!bios.ok && overlayReady_ && OverlayConfig::GetConfigValue("bios_openbios", "false") != "true")
    {
        biosNotice_ = true;
        OpenMenu();
        const std::string text = bios.unknown.empty()
            ? TrFormat2("emulator_bios_missing", bios.file, bios.folder)
            : TrFormat2("emulator_bios_unknown", bios.unknown, bios.folder);
        OverlayUI::ShowNotice(text, {SwitchFrontend::OverlayTranslation::tr("emulator_bios_use_openbios"),
                                     SwitchFrontend::OverlayTranslation::tr("emulator_exit_game")});
    }
    else
        BootGame();

    lastTicks_ = SDL_GetTicks();
    return true; // idle even if the game failed to load
}

bool SwanRuntime::BootGame()
{
    if (!core_->LoadGame(romPath_))
    {
        LOG_ERROR("HOME", "LoadGame failed; idling");
        if (overlayReady_)
            OverlayUI::ShowToast(SwitchFrontend::OverlayTranslation::tr("emulator_load_game_failed"),
                                 OverlayUI::ToastCorner::TopRight);
        return false;
    }
    SwanSaves::BackupForSession();
    SwanCheats::Load(core_->CurrentDiscPath(), TicoConfig::Slug());
    if (overlayReady_ && !TicoVulkan::IsOpenGL() && !SwanShaders::Chain())
        SwanShaders::Init();
    pacedFps_ = 0.0; // UpdateFramePacing sets the audio rate for this game
    offerResume_ = overlayReady_;
    return true;
}

bool SwanRuntime::InitOverlay(const std::string &romPath)
{
    if (!TicoVulkan::IsReady())
        return false;

    overlayHost_ = std::make_unique<SwanOverlayHost>(core_.get());
    if (!ImGuiOverlay::Init(overlayHost_.get()))
    {
        overlayHost_.reset();
        return false;
    }
    ImGuiOverlay::SetBackgroundDraw([this](ImDrawList *list, ImVec2 size) { DrawGame(list, size); });

    // Prefer the launcher-supplied title; fall back to the rom filename.
    OverlayUI::SetGameTitle(titleArg_.empty() ? GameTitleFromPath(romPath) : titleArg_);
    // the menu's slots 1..6 are the state files .state0 .. .state5
    SwanOverlayHost *host = overlayHost_.get();
    OverlayUI::SetSlotOccupiedCallback([host](int slot) { return slot >= 1 && host->StateSlotExists(slot - 1); });
    // Save/Load State show each slot's picture and when it was saved.
    slotPictures_ = {};
    OverlayUI::SetSlotPreviewCallback([this, host](int slot) {
        OverlayUI::SlotPreview preview;
        if (slot < 1 || slot > (int)slotPictures_.size())
            return preview;
        ImTextureID &picture = slotPictures_[slot - 1];
        host->DestroyTexture(picture); // the slot may have been saved again
        picture = 0;
        const std::string path = host->SlotStatePath(slot - 1);
        struct stat st;
        if (stat(path.c_str(), &st) != 0)
            return preview;
        char when[32];
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", std::localtime(&st.st_mtime));
        preview.saved_at = when;
        int w = 0, h = 0, channels = 0;
        if (unsigned char *rgba = stbi_load((path + ".png").c_str(), &w, &h, &channels, 4))
        {
            picture = host->CreateTextureRGBA(rgba, w, h);
            stbi_image_free(rgba);
        }
        preview.texture = (unsigned long long)picture;
        if (core_ && core_->GetAspectRatio() > 0.1f)
            preview.aspect = core_->GetAspectRatio();
        return preview;
    });
    OverlayUI::SetDiscCallback([this] {
        std::vector<OverlayUI::DiscMenuEntry> entries;
        discPaths_.clear();
        if (!core_ || !core_->IsGameLoaded())
            return entries;
        // the discs are found from the launched game (an .m3u lists them all);
        // the current one is whatever is in the drive now
        const std::string current = NormalizeDiscPath(core_->CurrentDiscPath());
        for (const DiscEntry &disc : ScanDiscs(NormalizeDiscPath(core_->GetGamePath())))
        {
            entries.push_back({disc.displayName, disc.romPath == current});
            discPaths_.push_back(disc.romPath);
        }
        return entries;
    });
    // Cheats from the game's .cht/.cheats file; the menu hides them in hardcore.
    OverlayUI::SetCheatCallbacks(
        [this] {
            std::vector<OverlayUI::CheatMenuEntry> entries;
            // another disc has its own cheats file
            if (core_ && core_->IsGameLoaded() && SwanCheats::NeedsReload(core_->CurrentDiscPath()))
                SwanCheats::Load(core_->CurrentDiscPath(), TicoConfig::Slug());
            const auto &cheats = SwanCheats::List();
            for (size_t i = 0; i < cheats.size(); ++i)
                entries.push_back({cheats[i].name, cheats[i].enabled, true, (int)i, false});
            return entries;
        },
        [this](int index) {
            if (index < 0 || (core_ && core_->IsHardcoreActive()))
                return false;
            SwanCheats::Toggle((size_t)index);
            return true;
        });
    // Settings > Players: what each port has, and the system's screen to
    // choose who is which player
    OverlayUI::PlayerCallbacks players;
    players.ports = [] { return ControllerNames(); };
    OverlayUI::SetPlayerCallbacks(std::move(players));
    OverlayUI::ReloadSettings();
    overlayReady_ = true;
    return true;
}

void SwanRuntime::ShutdownOverlay()
{
    if (overlayReady_)
    {
        OverlayUI::SetSlotOccupiedCallback(nullptr);
        OverlayUI::SetSlotPreviewCallback(nullptr);
        slotPictures_ = {}; // freed with the overlay's textures below
        OverlayUI::SetDiscCallback(nullptr);
        OverlayUI::SetCheatCallbacks(nullptr, nullptr);
        OverlayUI::SetPlayerCallbacks({});
        ImGuiOverlay::SetBackgroundDraw(nullptr);
        SwanLibrary::Unregister();
        ImGuiOverlay::Shutdown(); // frees textures via overlayHost_ (still alive)
    }
    overlayHost_.reset();
    overlayReady_ = false;
    menuOpen_ = false;
}

void SwanRuntime::ApplySettingsToCore()
{
    if (!core_)
        return;
    OverlayConfig::ApplyToCore([this](const std::string &key, const std::string &value) {
        core_->SetOption(key, value);
    });
}

void SwanRuntime::UpdateHud(float deltaTime)
{
    hudFrames_++;
    hudSeconds_ += deltaTime;
    if (hudSeconds_ >= 0.5f)
    {
        hudFps_ = static_cast<float>(hudFrames_) / hudSeconds_;
        hudFrames_ = 0;
        hudSeconds_ = 0.0f;
    }
    OverlayUI::HudStats stats;
    stats.fps = hudFps_;
    stats.fast_forward = audio_ && audio_->IsFastForwarding();
    if (core_)
    {
        stats.rendered_width = core_->GetFrameWidth();
        stats.rendered_height = core_->GetFrameHeight();
    }
    OverlayUI::SetHudStats(stats);
}

void SwanRuntime::OpenMenu()
{
    if (!overlayReady_ || menuOpen_)
        return;
    menuOpen_ = true;
    TicoCore::StopRumble(); // the game is paused
    ffLatched_ = false;
    navHeldPrev_ = 0;
    navRepeatFrames_ = 0;
    OverlayUI::SetHardcoreMode(core_ && core_->IsHardcoreActive());
    ImGuiOverlay::SetVisible(true);
}

void SwanRuntime::CloseMenu()
{
    if (!menuOpen_)
        return;
    menuOpen_ = false;
    ImGuiOverlay::SetVisible(false);
}

bool SwanRuntime::FeedMenu(const FrameInput &input)
{
    if (!menuOpen_)
        return false;

    // Directional navigation: D-pad + left stick, edge plus hold-repeat.
    const uint64_t buttons = input.buttons;
    uint64_t dirHeld = 0;
    if ((buttons & Pad_Up) || input.leftStickY < -16000)
        dirHeld |= Pad_Up;
    if ((buttons & Pad_Down) || input.leftStickY > 16000)
        dirHeld |= Pad_Down;
    if ((buttons & Pad_Left) || input.leftStickX < -16000)
        dirHeld |= Pad_Left;
    if ((buttons & Pad_Right) || input.leftStickX > 16000)
        dirHeld |= Pad_Right;

    uint64_t dirFire = dirHeld & ~navHeldPrev_; // new presses fire instantly
    if (dirHeld != 0 && dirHeld == navHeldPrev_)
    {
        if (--navRepeatFrames_ <= 0)
        {
            dirFire |= dirHeld;
            navRepeatFrames_ = kNavRepeatFrames;
        }
    }
    else if (dirFire != 0)
    {
        navRepeatFrames_ = kNavInitialDelayFrames;
    }
    navHeldPrev_ = dirHeld;

    // Pad_B is the Switch A button (east), Pad_A the Switch B button (south).
    ImGuiOverlay::FeedNav({
        .up = (dirFire & Pad_Up) != 0,
        .down = (dirFire & Pad_Down) != 0,
        .left = (dirFire & Pad_Left) != 0,
        .right = (dirFire & Pad_Right) != 0,
        .accept = (input.pressed & Pad_B) != 0,
        .cancel = (input.pressed & Pad_A) != 0,
    });
    ImGuiOverlay::FeedTouch({input.touchDown, input.touchX, input.touchY});
    return true;
}

void SwanRuntime::RunMenuAction()
{
    using OverlayUI::Action;
    const Action action = ImGuiOverlay::ConsumeAction();
    if (OverlayUI::ConsumeSettingsChanged())
        ApplySettingsToCore();

    switch (action)
    {
    case Action::None:
        return;
    case Action::Resume:
        if (!biosNotice_)
            CloseMenu();
        return;
    case Action::Exit:
        LOG_INFO("OVERLAY", "Exit requested");
        CloseMenu();
        if (fromLibrary_)
            LaunchSelf(argv0_.c_str(), {}, log_); // back to the library
        else if (!standalone_)
            chainload_ = true; // the library itself just quits
        exitRequested_ = true;
        return;
    case Action::Restart:
        // The NRO starts itself again with the same arguments (a renderer
        // change takes effect then); the game is saved on the way out.
        LOG_INFO("OVERLAY", "Restart requested");
        CloseMenu();
        // the relaunched NRO finds this and skips the resume prompt: Restart
        // means from the start
        if (overlayHost_ && core_ && core_->IsGameLoaded())
        {
            if (FILE *marker = std::fopen(RestartMarkerPath().c_str(), "wb"))
                std::fclose(marker);
        }
        relaunch_ = true;
        exitRequested_ = true;
        return;
    case Action::Reset:
        LOG_INFO("OVERLAY", "Reset requested");
        if (core_)
            core_->Reset();
        CloseMenu();
        return;
    case Action::ControllerOrder:
        if (!ShowControllerOrder())
            OverlayUI::ShowToast(SwitchFrontend::OverlayTranslation::tr("emulator_controllers_failed"),
                                 OverlayUI::ToastCorner::TopRight);
        return;
    case Action::NoticeChoice:
    {
        // the missing-BIOS notice: OpenBIOS from now on (the setting tico
        // reads too), or leave
        const int choice = OverlayUI::ConsumeNoticeChoice();
        biosNotice_ = false;
        CloseMenu();
        if (choice == 0)
        {
            OverlayConfig::SetConfigValue("bios_openbios", "true");
            OverlayConfig::SaveConfig();
            BootGame();
            return;
        }
        if (fromLibrary_)
            LaunchSelf(argv0_.c_str(), {}, log_);
        else if (!standalone_)
            chainload_ = true;
        exitRequested_ = true;
        return;
    }
    case Action::SwapDisc:
    {
        const int index = OverlayUI::ConsumeDiscIndex();
        if (core_ && index >= 0 && index < static_cast<int>(discPaths_.size()))
            core_->SwapDiskByPath(discPaths_[static_cast<size_t>(index)]);
        CloseMenu();
        return;
    }
    case Action::EditText:
    {
        const OverlayConfig::OptionDef *option = OverlayUI::ConsumeTextEditOption();
#ifdef __SWITCH__
        std::string value;
        const size_t length = option && option->max_length > 0 ? option->max_length : 64;
        if (option && PromptKeyboard(SwitchFrontend::OverlayTranslation::tr(option->label_key).c_str(),
                                     OverlayConfig::GetOptionValue(*option), length, value))
        {
            OverlayConfig::SetOptionValue(*option, value);
            OverlayUI::NotifyOptionEdited(*option);
        }
#else
        (void)option;
#endif
        return;
    }
    default:
        break;
    }

    if (!core_ || !core_->IsGameLoaded())
        return;
    if (OverlayUI::IsSaveStateAction(action) && overlayHost_)
    {
        const int slot = OverlayUI::GetStateSlotForAction(action);
        overlayHost_->SaveStateSlot(slot - 1);
        OverlayUI::ShowToast(TrFormat("emulator_state_saved", slot));
        CloseMenu();
    }
    else if (OverlayUI::IsLoadStateAction(action) && overlayHost_)
    {
        const int slot = OverlayUI::GetStateSlotForAction(action);
        overlayHost_->LoadStateSlot(slot - 1);
        if (slot == OverlayUI::kAutoStateSlot)
            OverlayUI::ShowToast(SwitchFrontend::OverlayTranslation::tr("emulator_auto_loaded"));
        else
            OverlayUI::ShowToast(TrFormat("emulator_state_loaded", slot));
        CloseMenu();
    }
}

// Updates fast forward from player 1's hotkey, whose Switch button then stays
// out of the game.
void SwanRuntime::UpdateFastForward(const FrameInput &input)
{
    ffButton_ = PadBitFor(OverlayConfig::GetConfigValue("fast_forward_hotkey", "None"));
    const bool down = ffButton_ && (input.players[0].buttons & ffButton_);
    bool active;
    if (OverlayConfig::GetConfigValue("fast_forward_mode", "hold") == "toggle")
    {
        if (down && !ffHotkeyHeld_)
            ffLatched_ = !ffLatched_;
        active = ffLatched_;
    }
    else
        active = down;
    ffHotkeyHeld_ = down;
    if (!active)
        ffFrameBudget_ = 0.0f;
    if (audio_)
        audio_->SetFastForward(active);
}

// Core frames to run before the next present.
int SwanRuntime::FramesThisRefresh()
{
    if (!audio_ || !audio_->IsFastForwarding())
        return 1;
    const std::string speed = OverlayConfig::GetConfigValue("fast_forward_speed", "200");
    if (speed == "unlimited")
        return 1; // vsync is off instead
    const float rate = std::max(1.0f, std::atoi(speed.c_str()) / 100.0f);
    ffFrameBudget_ += rate;
    const int frames = static_cast<int>(ffFrameBudget_);
    ffFrameBudget_ -= frames;
    return std::max(1, frames);
}

// Content near 60 Hz (NTSC, 59.94 fps) runs one core frame per vsync, with
// audio stretched to absorb the small difference. Anything else (PAL, 50 fps)
// is paced by an accumulator so it runs at real speed instead of 60/50 = 120%.
void SwanRuntime::UpdateFramePacing()
{
    const double fps = core_->GetFPS();
    if (fps <= 0.0 || fps == pacedFps_)
        return;
    pacedFps_ = fps;
    const double effectiveFps = std::fabs(fps - kDisplayHz) < 1.0 ? kDisplayHz : fps;
    frameStep_ = effectiveFps / kDisplayHz;
    frameAccum_ = 0.0;
    if (audio_)
        audio_->SetCoreSampleRate(core_->GetSampleRate() * (effectiveFps / fps));
    LOG_INFO("AUDIO", "Core %.3f fps, %.0f Hz: running %.3f core frames per vsync", fps, core_->GetSampleRate(),
             frameStep_);
}

void SwanRuntime::ApplyCoreInput(const FrameInput &input)
{
    core_->ClearInputs();
    UpdateFastForward(input);
    const bool analogDpad = OverlayConfig::GetConfigValue("analog_dpad", "false") == "true";

    // Resolve the mapping once per frame; both ports share it.
    uint64_t bits[std::size(kButtonMappings)];
    for (size_t i = 0; i < std::size(kButtonMappings); ++i)
        bits[i] = PadBitFor(OverlayConfig::GetConfigValue(kButtonMappings[i].key, kButtonMappings[i].fallback));

    for (unsigned port = 0; port < kPorts; ++port)
    {
        const PlayerInput &player = input.players[port];
        uint64_t held = player.buttons;
        if (port == 0)
            held &= ~ffButton_;
        if (analogDpad)
        {
            if (player.leftStickY < -16000)
                held |= Pad_Up;
            if (player.leftStickY > 16000)
                held |= Pad_Down;
            if (player.leftStickX < -16000)
                held |= Pad_Left;
            if (player.leftStickX > 16000)
                held |= Pad_Right;
        }
        // Several PlayStation buttons may share a Switch button, so only ever press.
        for (size_t i = 0; i < std::size(kButtonMappings); ++i)
            if (held & bits[i])
                core_->SetInputState(port, kButtonMappings[i].button, true);

        // both sticks, for DualShock games
        core_->SetAnalogState(port, 0, 0, player.leftStickX);
        core_->SetAnalogState(port, 0, 1, player.leftStickY);
        core_->SetAnalogState(port, 1, 0, player.rightStickX);
        core_->SetAnalogState(port, 1, 1, player.rightStickY);
    }
}

void SwanRuntime::HandleInput(const FrameInput &input)
{
    RunMenuAction();
    if (exitRequested_)
        return;

    // Player 2's pad is plugged in while a second controller is connected.
    if (core_ && core_->IsGameLoaded() && input.players[1].connected != pad2Connected_)
    {
        pad2Connected_ = input.players[1].connected;
        core_->SetPortConnected(1, pad2Connected_);
    }

    // Start+Select only ever opens the menu; B closes it. While the combo is
    // held it is kept from the game.
    const bool comboDown = (input.buttons & Pad_Start) && (input.buttons & Pad_Select);
    if (comboDown && !menuOpen_)
        OpenMenu();
    const bool consumed = FeedMenu(input) || comboDown;

    if (!core_ || !core_->IsGameLoaded())
        return;
    if (menuOpen_)
    {
        core_->ClearInputs();
        core_->Pause();
        if (audio_)
            audio_->SetFastForward(false);
    }
    else
    {
        core_->Resume();
        if (consumed)
            core_->ClearInputs();
        else
            ApplyCoreInput(input);
    }
}

void SwanRuntime::RunFrame()
{
    UpdateScreenMode();
    SwanShaders::Update(); // a preset compiles outside the frame

    // While fast forward runs uncapped the swapchain presents without vsync.
    const bool uncapped = audio_ && audio_->IsFastForwarding() &&
                          OverlayConfig::GetConfigValue("fast_forward_speed", "200") == "unlimited";
    if (uncapped != vsyncOff_)
    {
        TicoVulkan::SetVsync(!uncapped);
        vsyncOff_ = uncapped;
    }

    // Waits for this frame slot's previous submission, so everything below
    // may reuse per-frame resources. A skipped frame (swapchain being
    // recreated) still runs the core so emulation keeps its pace.
    frameCmd_ = TicoVulkan::BeginFrame();
    frameInFlight_ = frameCmd_ != nullptr;

    if (!core_ || !core_->IsGameLoaded())
        return;
    if (menuOpen_)
    {
        core_->Idle(); // paused: keep the RetroAchievements session alive
        return;
    }
    UpdateFramePacing();
    frameAccum_ += frameStep_ * FramesThisRefresh();
    while (frameAccum_ >= 1.0)
    {
        frameAccum_ -= 1.0;
        core_->RunFrame();
    }
    if (offerResume_)
        OfferResume();
}

// Once the game's first frame has run, the menu asks whether to continue from
// the auto save, if there is one (not after Restart, nor in hardcore).
void SwanRuntime::OfferResume()
{
    offerResume_ = false;
    if (!overlayHost_ || !core_)
        return;
    if (std::remove(RestartMarkerPath().c_str()) == 0)
        return;
    if (core_->IsHardcoreActive() || !overlayHost_->StateSlotExists(OverlayUI::kAutoStateSlot - 1))
        return;
    // tico's General > Continue Last Game
    const std::string mode = OverlayConfig::ResumeOnLaunch();
    if (mode == "never")
        return;
    if (mode == "always")
    {
        overlayHost_->LoadStateSlot(OverlayUI::kAutoStateSlot - 1);
        OverlayUI::ShowToast(SwitchFrontend::OverlayTranslation::tr("emulator_auto_loaded"));
        return;
    }
    OpenMenu();
    if (menuOpen_)
        OverlayUI::ShowResumePrompt();
}

std::string SwanRuntime::RestartMarkerPath() const
{
    return overlayHost_->SlotStatePath(OverlayUI::kAutoStateSlot - 1) + ".restart";
}

// Runs the shader chain at the game's on-screen size and draws its output
// (or the OpenGL renderer's texture) under the overlay.
void SwanRuntime::DrawGame(ImDrawList *list, ImVec2 size)
{
    list->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 255));
    if (!core_ || !core_->IsGameLoaded())
        return; // the library or the BIOS notice: no game, and no stale frame
    const ImVec4 rect = ComputeGameRect(core_.get(), size);
    if (rect.z < 1.0f || rect.w < 1.0f)
        return;
    if (TicoVulkan::IsOpenGL())
    {
        // bottom-left origin: flipped
        if (s_glFrame.texture)
            list->AddImage(static_cast<ImTextureID>(s_glFrame.texture), ImVec2(rect.x, rect.y),
                           ImVec2(rect.x + rect.z, rect.y + rect.w), ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
    }
    else if (TicoShaderChain *chain = SwanShaders::Chain(); chain && frameCmd_)
    {
        const ImTextureID tex = chain->Process(static_cast<VkCommandBuffer>(frameCmd_), (uint32_t)rect.z,
                                               (uint32_t)rect.w, core_->GetAspectRatio(), core_->GetFPS());
        if (tex != ImTextureID_Invalid)
            list->AddImage(tex, ImVec2(rect.x, rect.y), ImVec2(rect.x + rect.z, rect.y + rect.w));
    }
    DrawOsd();
}

// The core's own messages (disc changes, memory cards), as a fading pill.
void SwanRuntime::DrawOsd()
{
    const int frames = core_->GetOSDFrames();
    if (frames <= 0)
        return;
    const float alpha = frames < 30 ? frames / 30.0f : 1.0f;
    const std::string &msg = core_->GetOSDMessage();
    const ImVec2 textSize = ImGui::CalcTextSize(msg.c_str());
    const float padX = 16.0f, padY = 8.0f;
    const ImVec2 pos(24.0f, 16.0f);
    ImDrawList *fg = ImGui::GetForegroundDrawList();
    fg->AddRectFilled(pos, ImVec2(pos.x + textSize.x + padX * 2, pos.y + textSize.y + padY * 2),
                      IM_COL32(0, 0, 0, (int)(alpha * 153)), 14.0f);
    fg->AddText(ImVec2(pos.x + padX, pos.y + padY), IM_COL32(255, 255, 255, (int)(alpha * 240)), msg.c_str());
    core_->DecrementOSD();
}

void SwanRuntime::RenderFrame()
{
    if (!frameInFlight_)
        return;

    const uint32_t now = SDL_GetTicks();
    float deltaTime = static_cast<float>(now - lastTicks_) / 1000.0f;
    lastTicks_ = now;
    if (deltaTime <= 0.0f || deltaTime > 0.25f)
        deltaTime = 1.0f / 60.0f;

    ImDrawData *draw = nullptr;
    if (overlayReady_)
    {
        uint32_t width = 0, height = 0;
        TicoVulkan::GetSwapExtent(width, height);
        UpdateHud(deltaTime);
        draw = ImGuiOverlay::BuildFrame(static_cast<float>(width), static_cast<float>(height), deltaTime);
    }
    TicoVulkan::EndFrame(draw);
    frameInFlight_ = false;
    frameCmd_ = nullptr;
}

void SwanRuntime::Shutdown()
{
    LOG_INFO("HOME", "Shutting down");
    // the state the game is left in goes to the auto slot (listed first in
    // Load State), whatever closed it: Exit, Restart or HOME
    if (overlayHost_ && overlayHost_->IsGameLoaded())
        overlayHost_->SaveStateSlot(OverlayUI::kAutoStateSlot - 1);
    TicoVulkan::WaitIdle();
    SwanShaders::Shutdown();
    ShutdownOverlay();
    s_core = nullptr;
    core_.reset();
    TicoCore::StopRumble();
    TicoVulkan::Shutdown();

    if (audio_)
        audio_->Shutdown();
    s_audio = nullptr;
    SDL_Quit();
}

} // namespace Tico
