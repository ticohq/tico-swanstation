/// @file TicoCore.h
/// @brief SwanStation for the tico overlay, driven without libretro (SwanHost)
#pragma once

#include <string>
#include <map>
#include <cstdint>
#include <SDL.h>
#include <vector>
#include "libretro.h" // only for retro_pixel_format, which the shader chain takes
#include "imgui.h"
#include "swan/SwanHost.h"

#ifdef __SWITCH__
#include <switch.h>
#include <SDL_mixer.h>
#endif

#include <mutex>
#include <condition_variable>
#include <functional>
#include <deque>

struct rc_client_t;

struct TicoMemoryMap {
    uint32_t start;
    uint32_t length;
    uint8_t* ptr;
};

/// @brief Alert position for RA notifications
enum class RAAlertPosition {
    TopLeft = 0,
    TopRight,
    BottomLeft,
    BottomRight
};

/// @brief RA notification for the overlay
struct RANotification {
    std::string title;
    std::string description;
    std::string badge_name;     // badge identifier or "ra_icon" for session start
    ImTextureID textureId = ImTextureID_Invalid; // badge texture
    float timer = 0.0f;
    float duration = 4.0f; // total display time
    float slideIn = 0.4f;  // slide-in duration
    float slideOut = 0.4f; // slide-out duration
};

/// @brief SwanStation, as the overlay drives a core
class TicoCore
{
public:
    TicoCore();
    ~TicoCore();

    /// @brief Initialize the core
    bool Init();

    /// @brief Boot a disc image, EXE or PSF by path
    bool LoadGame(const std::string &path);
    bool GetVariable(const char *key, const char **value);


    /// @brief OSD notification accessors
    const std::string& GetOSDMessage() const { return m_osdMessage; }
    int GetOSDFrames() const { return m_osdFrames; }
    void DecrementOSD() { if (m_osdFrames > 0) m_osdFrames--; }
    void ShowOSD(const std::string &msg, int frames) { m_osdMessage = msg; m_osdFrames = frames; }

    /// @brief Unload current game
    void UnloadGame();

    /// @brief Run a single frame
    void RunFrame();

    /// @brief Reset the game
    void Reset();

    /// @brief Pause/Resume
    void Pause();
    void Resume();
    bool IsPaused() const { return m_paused; }
    bool IsGameLoaded() const { return m_gameLoaded; }

    /// @brief Input handling
    void SetInputState(unsigned port, unsigned id, bool pressed);
    void SetAnalogState(unsigned port, unsigned index, unsigned id, int16_t value);
    void ClearInputs();

    /// @brief Video/Audio info
    float GetAspectRatio() const { return m_aspectRatio; }
    int GetFrameWidth() const { return m_frameWidth; }
    int GetFrameHeight() const { return m_frameHeight; }
    double GetFPS() const { return m_fps; }
    double GetSampleRate() const { return m_sampleRate; }
    /// The size of the frame the GPU renderer last handed over
    void SetFrameSize(int width, int height) { m_frameWidth = width; m_frameHeight = height; }

    /// @brief Receives every new software frame from the core (the Vulkan
    /// renderer hands its image to g_tico_vulkan_frame instead)
    typedef void (*VideoCallback_t)(const void *data, unsigned width, unsigned height,
                                    size_t pitch, retro_pixel_format format);
    void SetVideoCallback(VideoCallback_t cb) { m_videoCallback = cb; }

    /// @brief Get current game path
    std::string GetGamePath() const { return m_gamePath; }

    /// @brief Cheats for the game, from sdmc:/tico/cheats/<slug>/<game>.cht
    /// (RetroArch's format, as in libretro's cheat database) or .cheats
    /// ("# Name" then its codes): GameShark lines ("80012345 0063"). Every
    /// cheat starts off; toggles last the session.
    struct Cheat {
        std::string name;
        std::vector<std::string> codes;
        bool enabled = false;
    };
    const std::vector<Cheat> &GetCheats() const { return m_cheats; }
    void ToggleCheat(size_t index);

    /// @brief Save states
    /// The state goes to `path`, rc_client's achievement progress beside it
    /// (`path` + ".ra"). Loading is refused while hardcore is active.
    bool SaveState(const std::string &path);
    bool LoadState(const std::string &path);

    /// True while rc_client runs the session in hardcore mode. Loading states
    /// (and rewind, cheats, slow motion) must stay unavailable then.
    bool IsHardcoreActive() const;

    /// Hardcore rate-limits pausing so it can't be used to slow the game
    /// down. False while a pause isn't allowed yet; `secondsRemaining` then
    /// says how long until it is. Always true outside hardcore.
    bool CanPause(int &secondsRemaining);

    /// Keeps the RetroAchievements session alive while emulation is paused
    /// (the quick menu is open): pings, server callbacks, badge uploads.
    void Idle();

    /// @brief Core options. LoadConfig reads swanstation.jsonc (once); SetOption
    /// changes one, which the core re-reads before the next frame.
    void EnsureConfigLoaded() { LoadConfig(); }
    void SetOption(const std::string &key, const std::string &value);

    /// @brief Audio callback types
    typedef void (*AudioSampleCallback_t)(int16_t left, int16_t right);
    typedef size_t (*AudioSampleBatchCallback_t)(const int16_t *data, size_t frames);
    typedef void (*AudioFlushCallback_t)();

    void SetAudioCallbacks(AudioSampleCallback_t sampleCb, AudioSampleBatchCallback_t batchCb, AudioFlushCallback_t flushCb = nullptr)
    {
        m_audioSampleCallback = sampleCb;
        m_audioSampleBatchCallback = batchCb;
        m_audioFlushCallback = flushCb;
    }

    void SetAudioFlushCallback(AudioFlushCallback_t flushCb)
    {
        m_audioFlushCallback = flushCb;
    }

private:
    void InitializeCore();
    void SetupCallbacks();

    void LoadSaveData();
    void SaveSaveData();
    void LoadCheats();
    void ApplyCheats();
    std::vector<Cheat> m_cheats;

    /// Memory card 1 as last read or written, to skip unchanged writes.
    std::vector<uint8_t> m_savedCard;
    int m_saveCheckFrames = 0;

    void SetRumble(unsigned port, float large, float small);
    static SwanHost::PadType ParsePadType(const std::string &value);

    bool m_initialized = false;
    bool m_gameLoaded = false;
    bool m_paused = false;
    bool m_variablesUpdated = true;
    enum retro_pixel_format m_pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;

    int m_frameWidth = 256;
    int m_frameHeight = 224;
    float m_aspectRatio = 4.0f / 3.0f;
    double m_fps = 60.0;
    double m_sampleRate = 44100.0;
    VideoCallback_t m_videoCallback = nullptr;

    AudioSampleCallback_t m_audioSampleCallback = nullptr;
    AudioSampleBatchCallback_t m_audioSampleBatchCallback = nullptr;
    AudioFlushCallback_t m_audioFlushCallback = nullptr;

    bool m_inputState[4][16] = {};
    int16_t m_analogState[4][2][2] = {};

    std::string m_systemDir;
    std::string m_saveDir;
    std::string m_gamePath;

    std::string GetConfigValue(const std::string &key, const std::string &defaultVal = "");
    void LoadConfig();
    std::map<std::string, std::string> m_configOptions;
    bool m_configLoaded = false;

    std::string m_osdMessage = "";
    int m_osdFrames = 0;

    // RetroAchievements Client
    rc_client_t* m_rcClient = nullptr;
    bool m_raEnabled = false;
    std::string m_raUsername = "";
    std::string m_raToken = "";
    std::string m_raPassword = "";
    bool m_raHardcore = false;
    void LoadRAConfig();
    void SaveRAToken(const std::string& token);
    static void RAIdentifyGame(rc_client_t* c, TicoCore* core);

    Mix_Chunk* m_trophySound = nullptr;
    static void RALoginWithPassword(rc_client_t* c, TicoCore* core);

public:
    // RA notifications queue (public for overlay access)
    std::vector<RANotification> m_raNotifications;
    RAAlertPosition m_raAlertPosition = RAAlertPosition::TopRight;
    void PushRANotification(const std::string& title, const std::string& desc,
                           const std::string& badge = "");
    
    // RA badge cache (badge_name -> texture)
    std::map<std::string, ImTextureID> m_raBadgeCache;
    ImTextureID m_raIconTexture = ImTextureID_Invalid; // ra.svg icon
    void LoadRAIcon();                        // load ra.svg as texture
    ImTextureID GetRABadgeTexture(const std::string& badge_name);
    void DownloadAndCacheBadge(const std::string& badge_name); // runs on worker
    void PreloadRABadges();                   // called after game identification
    std::vector<std::pair<std::string, std::vector<unsigned char>>> m_raPendingBadgeUploads;
    std::mutex m_raBadgeUploadMutex;
    void ProcessPendingBadgeUploads();        // called from main thread (RunFrame)

public:
    // RA Worker Thread (persistent, proper libnx lifecycle)
    struct RAJob {
        std::string url;
        std::string post_data;
        void* callback;       // rc_client_server_callback_t (cast in .cpp)
        void* callback_data;
    };
    std::mutex m_raJobMutex;
    std::condition_variable m_raJobCond;
    std::deque<RAJob> m_raJobQueue;
    bool m_raWorkerRunning = false;

    std::mutex m_raCallbackMutex;
    std::vector<std::function<void()>> m_raPendingCallbacks;

#ifdef __SWITCH__
    Thread m_raThread;
    bool m_raThreadCreated = false;
#endif
    void StartRAWorker();
    void StopRAWorker();
    static void RAWorkerEntry(void* arg);

    std::vector<TicoMemoryMap> m_memoryMaps;
};
