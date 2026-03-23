/// @file TicoCore.cpp
/// @brief Simplified libretro frontend for swanstation with tico overlay
/// Based on LibretroCoreStatic.cpp but stripped of multi-core switching

#include "TicoCore.h"
#include "TicoConfig.h"
#include <json.hpp>
#include <SDL.h>
#include <SDL_mixer.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <glad/glad.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h> // Keep this from original
#include "TicoLogger.h"

// RetroAchievements
#include "rc_client.h"
#include <curl/curl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>
#include "deps/stb/stb_image.h"

#ifdef __SWITCH__
#include <switch.h>

// Global vibration state for Switch
// Index: 0=Handheld, 1=P1, 2=P2, 3=P3, 4=P4
static HidVibrationDeviceHandle s_vibrationHandles[5][2] = {};
static HidVibrationValue s_currentVibration[5][2] = {};
static bool s_vibrationInitialized = false;

#else
#include <glad/glad.h>
#endif

//==============================================================================
// SRAM Handling
//==============================================================================

void TicoCore::LoadSRAM()
{
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!size)
        return;

    void *data = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!data)
        return;

    // Extract ROM name
    std::string filename = m_gamePath;
    size_t lastSlash = filename.find_last_of("/\\");
    if (lastSlash != std::string::npos)
        filename = filename.substr(lastSlash + 1);
    size_t lastDot = filename.find_last_of(".");
    if (lastDot != std::string::npos)
        filename = filename.substr(0, lastDot);

    std::string savePathMCD = std::string(TicoConfig::SAVES_PATH) + filename + ".mcd";
    std::string savePathMCR = std::string(TicoConfig::SAVES_PATH) + filename + ".mcr";
    std::string savePathSRM = std::string(TicoConfig::SAVES_PATH) + filename + ".srm";

    std::ifstream file(savePathMCD, std::ios::binary);
    if (file)
    {
        file.read((char *)data, size);
        LOG_CORE("Loaded SRAM from %s", savePathMCD.c_str());
        return;
    }

    file.open(savePathMCR, std::ios::binary);
    if (file)
    {
        file.read((char *)data, size);
        LOG_CORE("Loaded SRAM from %s", savePathMCR.c_str());
        return;
    }

    file.open(savePathSRM, std::ios::binary);
    if (file)
    {
        file.read((char *)data, size);
        LOG_CORE("Loaded SRAM from %s", savePathSRM.c_str());
        return;
    }

    LOG_WARN("CORE", "No SRAM file found (.mcd, .mcr, or .srm) at %s", TicoConfig::SAVES_PATH);
}

void TicoCore::SaveSRAM()
{
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!size)
        return;

    void *data = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!data)
        return;

    // Extract ROM name
    std::string filename = m_gamePath;
    size_t lastSlash = filename.find_last_of("/\\");
    if (lastSlash != std::string::npos)
        filename = filename.substr(lastSlash + 1);
    size_t lastDot = filename.find_last_of(".");
    if (lastDot != std::string::npos)
        filename = filename.substr(0, lastDot);

    // Ensure directory exists
    struct stat st = {0};
    if (stat(TicoConfig::SAVES_PATH, &st) == -1)
    {
#ifdef __SWITCH__
        mkdir(TicoConfig::SAVES_PATH, 0777);
#else
        mkdir(TicoConfig::SAVES_PATH, 0777);
#endif
    }

    std::string savePath = std::string(TicoConfig::SAVES_PATH) + filename + ".mcd";

    std::ofstream file(savePath, std::ios::binary);
    if (file)
    {
        file.write((const char *)data, size);
        LOG_CORE("Saved SRAM to %s", savePath.c_str());
    }
    else
    {
        LOG_ERROR("CORE", "Failed to save SRAM to %s", savePath.c_str());
    }
}

// Include libretro API
#include "libretro.h"

// Forward declarations for swanstation core functions
extern "C"
{
    void retro_init(void);
    void retro_deinit(void);
    void retro_set_environment(retro_environment_t);
    void retro_set_video_refresh(retro_video_refresh_t);
    void retro_set_audio_sample(retro_audio_sample_t);
    void retro_set_audio_sample_batch(retro_audio_sample_batch_t);
    void retro_set_input_poll(retro_input_poll_t);
    void retro_set_input_state(retro_input_state_t);
    void retro_get_system_info(struct retro_system_info *info);
    void retro_get_system_av_info(struct retro_system_av_info *info);
    void retro_set_controller_port_device(unsigned port, unsigned device);
    void retro_reset(void);
    void retro_run(void);
    bool retro_load_game(const struct retro_game_info *game);
    void retro_unload_game(void);
    size_t retro_serialize_size(void);
    bool retro_serialize(void *data, size_t size);
    bool retro_unserialize(const void *data, size_t size);
    void *retro_get_memory_data(unsigned id);
    size_t retro_get_memory_size(unsigned id);
}



// Static instance for callbacks
static TicoCore *s_instance = nullptr;

// HW render callback storage
static retro_hw_render_callback s_hwRenderCallback = {};

//==============================================================================
// RetroAchievements Callbacks
//==============================================================================
static uint32_t RC_CCONV RAReadMemory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client)
{
    if (!s_instance) return 0;
    
    // Check memory maps provided by RETRO_ENVIRONMENT_SET_MEMORY_MAPS
    if (!s_instance->m_memoryMaps.empty()) {
        for (const auto& map : s_instance->m_memoryMaps) {
            if (address >= map.start && address + num_bytes <= map.start + map.length) {
                memcpy(buffer, map.ptr + (address - map.start), num_bytes);
                return num_bytes;
            }
        }
        return 0;
    }
    
    // Fallback if no memory maps
    uint8_t* wram = (uint8_t*)retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    size_t wram_size = retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if (wram && address + num_bytes <= wram_size) {
        memcpy(buffer, wram + address, num_bytes);
        return num_bytes;
    }
    
    return 0;
}

static size_t CurlWriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

void TicoCore::RAWorkerEntry(void* arg) {
    TicoCore* self = (TicoCore*)arg;
    while (true) {
        RAJob job;
        {
            std::unique_lock<std::mutex> lock(self->m_raJobMutex);
            self->m_raJobCond.wait(lock, [self]() {
                return !self->m_raJobQueue.empty() || !self->m_raWorkerRunning;
            });
            if (!self->m_raWorkerRunning && self->m_raJobQueue.empty()) break;
            job = std::move(self->m_raJobQueue.front());
            self->m_raJobQueue.pop_front();
        }
        if (job.url == "__badge__") {
            self->DownloadAndCacheBadge(job.post_data);
            continue;
        }
        CURL *curl = curl_easy_init();
        std::string readBuffer;
        long http_code = 0;
        std::string requestUrl = job.url;
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, job.url.c_str());
            if (!job.post_data.empty()) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, job.post_data.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            else http_code = 500;
            curl_easy_cleanup(curl);
        }
        {
            std::lock_guard<std::mutex> lock(self->m_raCallbackMutex);
            self->m_raPendingCallbacks.push_back([job, http_code, readBuffer]() {
                rc_api_server_response_t response;
                memset(&response, 0, sizeof(response));
                response.body = readBuffer.c_str();
                response.body_length = readBuffer.size();
                response.http_status_code = http_code;
                rc_client_server_callback_t cb = (rc_client_server_callback_t)job.callback;
                if (cb) cb(&response, job.callback_data);
            });
        }
    }
}

void TicoCore::StartRAWorker() {
#ifdef __SWITCH__
    m_raWorkerRunning = true;
    memset(&m_raThread, 0, sizeof(m_raThread));
    Result rc = threadCreate(&m_raThread, RAWorkerEntry, this, NULL, 0x40000, 0x2C, 0);
    if (R_SUCCEEDED(rc)) {
        if (R_SUCCEEDED(threadStart(&m_raThread))) {
            m_raThreadCreated = true;
        } else {
            threadClose(&m_raThread);
            m_raWorkerRunning = false;
        }
    } else {
        m_raWorkerRunning = false;
    }
#endif
}

void TicoCore::StopRAWorker() {
#ifdef __SWITCH__
    if (!m_raThreadCreated) return;
    {
        std::lock_guard<std::mutex> lock(m_raJobMutex);
        m_raWorkerRunning = false;
    }
    m_raJobCond.notify_one();
    threadWaitForExit(&m_raThread);
    threadClose(&m_raThread);
    m_raThreadCreated = false;
#endif
}

static void RC_CCONV RAServerCall(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callback_data, rc_client_t* client)
{
    if (!s_instance) return;
    TicoCore::RAJob job;
    job.url = request->url;
    if (request->post_data) job.post_data = request->post_data;
    job.callback = (void*)callback;
    job.callback_data = callback_data;
    
#ifdef __SWITCH__
    if (s_instance->m_raWorkerRunning) {
        std::lock_guard<std::mutex> lock(s_instance->m_raJobMutex);
        s_instance->m_raJobQueue.push_back(std::move(job));
        s_instance->m_raJobCond.notify_one();
    } else {
        CURL *curl = curl_easy_init();
        std::string readBuffer;
        long http_code = 0;
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, request->url);
            if (request->post_data) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->post_data);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            else http_code = 500;
            curl_easy_cleanup(curl);
        }
        rc_api_server_response_t response;
        memset(&response, 0, sizeof(response));
        response.body = readBuffer.c_str();
        response.body_length = readBuffer.size();
        response.http_status_code = http_code;
        if (callback) callback(&response, callback_data);
    }
#else
    CURL *curl = curl_easy_init();
    std::string readBuffer;
    long http_code = 0;
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, request->url);
        if (request->post_data) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->post_data);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        else http_code = 500;
        curl_easy_cleanup(curl);
    }
    rc_api_server_response_t response;
    memset(&response, 0, sizeof(response));
    response.body = readBuffer.c_str();
    response.body_length = readBuffer.size();
    response.http_status_code = http_code;
    if (callback) callback(&response, callback_data);
#endif
}

//==============================================================================
// Construction
//==============================================================================

TicoCore::TicoCore()
{
    memset(m_inputState, 0, sizeof(m_inputState));
    memset(m_analogState, 0, sizeof(m_analogState));

    m_systemDir = TicoConfig::SYSTEM_PATH;
    m_saveDir = TicoConfig::SAVES_PATH;

}

TicoCore::~TicoCore()
{
    UnloadGame();

    if (m_initialized)
    {
        retro_deinit();
        m_initialized = false;
    }

    if (s_instance == this)
    {
        s_instance = nullptr;
    }

    StopRAWorker();
    
    if (m_trophySound) {
        Mix_FreeChunk(m_trophySound);
        m_trophySound = nullptr;
    }

    if (m_rcClient) {
        rc_client_destroy(m_rcClient);
        m_rcClient = nullptr;
    }
}

//==============================================================================
// Initialization
//==============================================================================

bool TicoCore::Init()
{
    if (m_initialized)
        return true;

    s_instance = this;

#ifdef __SWITCH__
    if (!s_vibrationInitialized)
    {
        memset(s_currentVibration, 0, sizeof(s_currentVibration));
        for(int i = 0; i < 5; i++) {
            for(int j = 0; j < 2; j++) {
                s_currentVibration[i][j].freq_low = 160.0f;
                s_currentVibration[i][j].freq_high = 320.0f;
            }
        }
        
        // Initialize Handheld and Player 1-4
        hidInitializeVibrationDevices(s_vibrationHandles[0], 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
        hidInitializeVibrationDevices(s_vibrationHandles[1], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
        hidInitializeVibrationDevices(s_vibrationHandles[2], 2, HidNpadIdType_No2, HidNpadStyleTag_NpadJoyDual);
        hidInitializeVibrationDevices(s_vibrationHandles[3], 2, HidNpadIdType_No3, HidNpadStyleTag_NpadJoyDual);
        hidInitializeVibrationDevices(s_vibrationHandles[4], 2, HidNpadIdType_No4, HidNpadStyleTag_NpadJoyDual);
        
        s_vibrationInitialized = true;
        LOG_CORE("Vibration devices initialized for P1-P4");
    }
#endif

    // Set environment callback before init
    retro_set_environment(EnvironmentCallback);

    // Load configuration to ensure variables are ready for init
    LoadConfig();
    LoadRAConfig();

    bool soundEnabled = false;
#ifdef __SWITCH__
    std::string audioConfigPath = "sdmc:/tico/config/audio.jsonc";
#else
    std::string audioConfigPath = "tico/config/audio.jsonc";
#endif
    std::ifstream audioIn(audioConfigPath);
    if (audioIn.is_open()) {
        nlohmann::json j = nlohmann::json::parse(audioIn, nullptr, false, true); // allow_exceptions = false, allow_comments = true
        if (!j.is_discarded() && j.contains("sound_enabled")) {
            if (j["sound_enabled"].is_boolean()) {
                soundEnabled = j["sound_enabled"].get<bool>();
            }
        }
        audioIn.close();
    }
    if (soundEnabled) {
#ifdef __SWITCH__
        m_trophySound = Mix_LoadWAV("romfs:/assets/trophy.mp3");
#else
        m_trophySound = Mix_LoadWAV("tico/assets/trophy.mp3");
#endif
    }

    // Initialize the core
    retro_init();

    // Set all callbacks
    retro_set_video_refresh(VideoRefreshCallback);
    retro_set_audio_sample(AudioSampleCallback);
    retro_set_audio_sample_batch(AudioSampleBatchCallback);
    retro_set_input_poll(InputPollCallback);
    retro_set_input_state(InputStateCallback);

    // Initialize RetroAchievements
    m_rcClient = rc_client_create(RAReadMemory, RAServerCall);
    if (m_rcClient) {
        rc_client_set_event_handler(m_rcClient, [](const rc_client_event_t* event, rc_client_t* client) {
            if (!s_instance) return;
            switch (event->type) {
                case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
                    if (event->achievement) {
                        s_instance->PushRANotification(event->achievement->title, event->achievement->description, event->achievement->badge_name);
                        if (s_instance->m_trophySound) Mix_PlayChannel(-1, s_instance->m_trophySound, 0);
                    }
                    break;
                case RC_CLIENT_EVENT_GAME_COMPLETED:
                    s_instance->PushRANotification("Game Mastered!", "All achievements unlocked!", "ra_icon");
                    if (s_instance->m_trophySound) Mix_PlayChannel(-1, s_instance->m_trophySound, 0);
                    break;
                case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
                    if (event->leaderboard) {
                        s_instance->PushRANotification("Leaderboard", event->leaderboard->title, "ra_icon");
                    }
                    break;
                default:
                    break;
            }
        });
        rc_client_set_hardcore_enabled(m_rcClient, m_raHardcore);

        StartRAWorker();

        if (!m_raUsername.empty() && !m_raToken.empty()) {
            LOG_CORE("RA: Existent token found. Auto login as %s...", m_raUsername.c_str());
            rc_client_begin_login_with_token(m_rcClient, m_raUsername.c_str(), m_raToken.c_str(),
                [](int res, const char* err, rc_client_t* c, void* ud) {
                    TicoCore* self = (TicoCore*)ud;
                    if (res == RC_OK) {
                        LOG_CORE("RA login success with token!");
                        // Token valid, let's identify the game
                        if (self->m_gameLoaded && !self->m_gamePath.empty()) {
                            RAIdentifyGame(c, self);
                        }
                    } else if (res == RC_INVALID_CREDENTIALS && !self->m_raPassword.empty()) {
                        LOG_CORE("RA token invalid or expired. Trying password...");
                        RALoginWithPassword(c, self);
                    } else {
                        LOG_CORE("RA login failed -> %s", err ? err : "Unknown");
                        self->PushRANotification("Login Failed", "Check your credentials.", "ra_icon");
                    }
                }, this);
        } else if (!m_raUsername.empty() && !m_raPassword.empty()) {
            LOG_CORE("RA: Auto login using password...");
            RALoginWithPassword(m_rcClient, this);
        }
    }

    // Get core info
    struct retro_system_info sysInfo = {};
    retro_get_system_info(&sysInfo);

    LOG_CORE("Initialized: %s %s",
             sysInfo.library_name ? sysInfo.library_name : "Unknown",
             sysInfo.library_version ? sysInfo.library_version : "");

    m_initialized = true;
    return true;
}

void TicoCore::SetHWRenderContext(SDL_Window *window, EGLContext mainCtx, EGLContext hwCtx)
{
    m_window = window;
    m_mainContext = mainCtx;
    m_hwContext = hwCtx;
    m_eglDisplay = eglGetCurrentDisplay();
    m_eglSurface = eglGetCurrentSurface(EGL_DRAW);
}

bool TicoCore::InitEGLDualContext()
{
    m_eglDisplay = eglGetCurrentDisplay();
    EGLContext currentCtx = eglGetCurrentContext();

    if (m_eglDisplay == EGL_NO_DISPLAY || currentCtx == EGL_NO_CONTEXT)
    {
        LOG_ERROR("CORE", "Failed to get current EGL context");
        return false;
    }

    m_mainContext = currentCtx;
    m_eglSurface = eglGetCurrentSurface(EGL_DRAW);
    m_hwContext = m_mainContext; // Single context mode

    // Use FBO dimensions (max resolution) so the core's internal resolution fits
    int fboW = m_fboWidth > 0 ? m_fboWidth : m_frameWidth;
    int fboH = m_fboHeight > 0 ? m_fboHeight : m_frameHeight;

    // Create HW render texture at max resolution
    glGenTextures(1, &m_frameTexture);
    glBindTexture(GL_TEXTURE_2D, m_frameTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fboW, fboH, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Create FBO if not exists
    if (m_fbo == 0)
    {
        glGenFramebuffers(1, &m_fbo);
        glGenRenderbuffers(1, &m_fbo_rbo);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);

    // Setup RBO (Depth24 Stencil8) at max resolution
    glBindRenderbuffer(GL_RENDERBUFFER, m_fbo_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, fboW, fboH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_fbo_rbo);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_frameTexture, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        LOG_ERROR("CORE", "FBO incomplete: 0x%x", status);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }

    // Set viewport to FBO dimensions before clearing, otherwise we don't clear the whole texture!
    glViewport(0, 0, fboW, fboH);

    // Clear FBO to black
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    LOG_CORE("Created HW render texture: %u (%dx%d) FBO: %u",
             m_frameTexture, fboW, fboH, m_fbo);

    return true;
}

void TicoCore::BindHWContext(bool enable)
{
    (void)enable; // Single context mode - no-op
}

//==============================================================================
// Game Loading
//==============================================================================

// Debug logging helper removed - using TicoLogger.h

bool TicoCore::LoadGame(const std::string &path)
{
    LOG_CORE("LoadGame: Enter: %s", path.c_str());

    m_gamePath = path;

    if (!m_initialized)
    {
        LOG_CORE("LoadGame: Not initialized, calling Init()");
        if (!Init())
        {
            LOG_ERROR("CORE", "LoadGame: Init() failed");
            return false;
        }
    }

    LOG_CORE("LoadGame: Opening file...");

    // SwanStation libretro core supports `need_fullpath = true`.
    // We send only the path instead of reading up to 700MB into RAM synchronously.
    struct retro_game_info gameInfo = {};
    gameInfo.path = path.c_str();
    gameInfo.data = nullptr;
    gameInfo.size = 0;

    LOG_CORE("LoadGame: Calling retro_load_game...");
    LOG_CORE("  gameInfo.path = %s", gameInfo.path);
    LOG_CORE("  gameInfo.size = %zu", gameInfo.size);
    LOG_CORE("  gameInfo.data = %p", gameInfo.data);

    if (!retro_load_game(&gameInfo))
    {
        LOG_ERROR("CORE", "LoadGame: retro_load_game failed");
        return false;
    }
    LOG_CORE("LoadGame: retro_load_game succeeded");

    // Get AV info
    LOG_CORE("LoadGame: Getting AV info...");
    struct retro_system_av_info avInfo = {};
    retro_get_system_av_info(&avInfo);

    m_frameWidth = avInfo.geometry.base_width;
    m_frameHeight = avInfo.geometry.base_height;
    m_aspectRatio = avInfo.geometry.aspect_ratio > 0
                        ? avInfo.geometry.aspect_ratio
                        : (float)m_frameWidth / m_frameHeight;
    m_fps = avInfo.timing.fps > 0 ? avInfo.timing.fps : 60.0;

    // Match LibretroCoreStatic: FBO sized to frame dimensions, not max VRAM
    m_fboWidth = m_frameWidth;
    m_fboHeight = m_frameHeight;

    LOG_CORE("LoadGame: Game loaded: %dx%d @ %.2f fps",
             m_frameWidth, m_frameHeight, m_fps);

    // Initialize HW rendering if needed
    if (m_hwRender)
    {
        LOG_CORE("LoadGame: Initializing HW render context...");
        if (InitEGLDualContext())
        {
            if (s_hwRenderCallback.context_reset)
            {
                LOG_CORE("LoadGame: Calling context_reset...");
                s_hwRenderCallback.context_reset();
                LOG_CORE("LoadGame: context_reset done");
            }
        }
    }

    // Set controller
    LOG_CORE("LoadGame: Setting controller port device...");

#ifndef RETRO_DEVICE_PS_DUALSHOCK
#define RETRO_DEVICE_PS_DUALSHOCK 261
#endif

    unsigned device = RETRO_DEVICE_JOYPAD;
    // Default to true for analog on PSX if not present, unless user specified false.
    if (GetConfigValue("analog", "true") == "true")
    {
        device = RETRO_DEVICE_PS_DUALSHOCK;
        LOG_CORE("LoadGame: PSX Analog Mode: Using RETRO_DEVICE_PS_DUALSHOCK (%u)", device);
    }
    
    // Set for ports 0 and 1 (Player 1 and 2)
    retro_set_controller_port_device(0, device);
    retro_set_controller_port_device(1, device);

    m_gameLoaded = true;
    m_paused = false;
    LOG_CORE("LoadGame: Complete!");

    // Load SRAM
    LoadSRAM();

    return true;
}

void TicoCore::UnloadGame()
{
    if (!m_gameLoaded)
        return;

    // Save SRAM before unloading
    SaveSRAM();

    retro_unload_game();
    m_gameLoaded = false;

    if (m_frameTexture != 0)
    {
        glDeleteTextures(1, &m_frameTexture);
        m_frameTexture = 0;
    }

    if (m_fbo != 0)
    {
        glDeleteFramebuffers(1, &m_fbo);
        m_fbo = 0;
    }

    if (m_fbo_rbo != 0)
    {
        glDeleteRenderbuffers(1, &m_fbo_rbo);
        m_fbo_rbo = 0;
    }

    // Reset GL state
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

//==============================================================================
// Frame execution
//==============================================================================

void TicoCore::RunFrame()
{
    if (!m_gameLoaded || m_paused)
        return;

    if (m_swapPending && m_swapDelayFrames > 0)
    {
        m_swapDelayFrames--;
        if (m_swapDelayFrames == 0)
        {
            retro_game_info info = {m_pendingSwapPath.c_str(), nullptr, 0, ""};
            if (m_diskControl.replace_image_index(0, &info) && m_diskControl.set_image_index(0))
            {
                m_diskControl.set_eject_state(false);
                LOG_CORE("Delayed SwapDisk executed successfully.");
            }
            else
            {
                LOG_ERROR("CORE", "Delayed SwapDisk failed during replace/set index.");
                m_diskControl.set_eject_state(false);
            }
            m_swapPending = false;
        }
    }

    if (m_rcClient) {
        rc_client_do_frame(m_rcClient);
    }
    
    // We no longer process timers here; TicoOverlay::RenderRAAlerts does it
    // using deltaTime to stay speed-independent.
    
    // Process queued callbacks
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(m_raCallbackMutex);
        std::swap(callbacks, m_raPendingCallbacks);
    }
    for (auto& cb : callbacks) cb();
    
    ProcessPendingBadgeUploads();

    retro_run();

    // CRITICAL: Unbind core's FBO so subsequent rendering
    // (glClear, ImGui) targets the default framebuffer (FBO 0)
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void TicoCore::ResizeFBO(int width, int height)
{
    if (m_frameTexture == 0 || m_fbo == 0)
        return;

    LOG_CORE("ResizeFBO: %dx%d", width, height);

    glBindTexture(GL_TEXTURE_2D, m_frameTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindRenderbuffer(GL_RENDERBUFFER, m_fbo_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_frameTexture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_fbo_rbo);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        LOG_ERROR("CORE", "ResizeFBO incomplete: 0x%x", status);
    }
    else
    {
        glViewport(0, 0, width, height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void TicoCore::Reset()
{
    if (m_gameLoaded)
    {
        retro_reset();
    }
}

void TicoCore::Pause() { m_paused = true; }
void TicoCore::Resume() { m_paused = false; }

//==============================================================================
// Input
//==============================================================================

void TicoCore::SetInputState(unsigned port, unsigned id, bool pressed)
{
    if (port < 4 && id < 16)
    {
        m_inputState[port][id] = pressed;
    }
}

void TicoCore::SetAnalogState(unsigned port, unsigned index, unsigned id, int16_t value)
{
    if (port < 4 && index < 2 && id < 2)
    {
        m_analogState[port][index][id] = value;
    }
}

void TicoCore::ClearInputs()
{
    memset(m_inputState, 0, sizeof(m_inputState));
    memset(m_analogState, 0, sizeof(m_analogState));
}

//==============================================================================
// Save States
//==============================================================================

void TicoCore::SaveState(const std::string &path)
{
    if (!m_gameLoaded)
        return;

    // CRITICAL: Bind HW context for GPU state access
    BindHWContext(true);
    // Sync point: Ensure prior UI commands are done
    glFinish();

    // Ensure no FBO is bound so core doesn't read from wrong buffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    size_t size = retro_serialize_size();
    if (size == 0)
    {
        LOG_WARN("CORE", "SaveState: size 0");
        BindHWContext(false);
        return;
    }

    std::vector<uint8_t> data(size);
    bool success = retro_serialize(data.data(), size);

    // Reset GL state after core operations to protect frontend
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Sync point: Ensure core commands are done before UI resumes
    glFinish();

    BindHWContext(false);

    if (success)
    {
        FILE *fp = fopen(path.c_str(), "wb");
        if (fp)
        {
            fwrite(data.data(), 1, size, fp);
            fclose(fp);
            LOG_CORE("Saved state to %s", path.c_str());
        }
        else
        {
            LOG_ERROR("CORE", "Failed to open file for save state: %s", path.c_str());
        }
    }
    else
    {
        LOG_ERROR("CORE", "retro_serialize failed");
    }
}

void TicoCore::LoadState(const std::string &path)
{
    if (!m_gameLoaded)
        return;

    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp)
    {
        LOG_WARN("CORE", "LoadState: File not found: %s", path.c_str());
        return;
    }

    fseek(fp, 0, SEEK_END);
    size_t fileSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fileSize == 0)
    {
        fclose(fp);
        return;
    }

    std::vector<uint8_t> data(fileSize);
    if (fread(data.data(), 1, fileSize, fp) != fileSize)
    {
        fclose(fp);
        return;
    }
    fclose(fp);

    // CRITICAL: Bind HW context for GPU state restoration
    BindHWContext(true);
    // Sync point: Wait for UI
    glFinish();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // CRITICAL: Flush BOTH swanstation's internal audio buffer AND SDL queue
    // This must happen BEFORE unserialize to prevent stale audio playback
    // 1. Flush swanstation's internal audio buffer (not applicable here directly, so skip)

    // 2. Flush SDL audio queue
    if (m_audioFlushCallback)
    {
        LOG_CORE("Resetting SDL audio device...");
        m_audioFlushCallback();
    }

    bool success = retro_unserialize(data.data(), fileSize);

    // Reset GL state after core operations
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Sync point: Wait for core
    glFinish();

    BindHWContext(false);

    if (success)
    {
        LOG_CORE("Loaded state from %s", path.c_str());

        // CRITICAL FIX: Run one frame to force display update
        // The core's DoState does NOT call UpdateDisplay.
        // This means the GPU's VRAM is restored but the FBO output is stale.
        LOG_CORE("Running one frame to force display update...");
        retro_run();
    }
    else
    {
        LOG_ERROR("CORE", "retro_unserialize failed");
    }
}

//==============================================================================
// Libretro Callbacks
//==============================================================================

bool TicoCore::EnvironmentCallback(unsigned cmd, void *data)
{
    if (!s_instance)
        return false;
    return s_instance->HandleEnvironment(cmd, data);
}

void TicoCore::VideoRefreshCallback(const void *data, unsigned width,
                                    unsigned height, size_t pitch)
{
    if (!s_instance)
        return;
    s_instance->HandleVideoRefresh(data, width, height, pitch);
}

void TicoCore::AudioSampleCallback(int16_t left, int16_t right)
{
    if (s_instance && s_instance->m_audioSampleCallback)
    {
        s_instance->m_audioSampleCallback(left, right);
    }
}

size_t TicoCore::AudioSampleBatchCallback(const int16_t *data, size_t frames)
{
    if (s_instance && s_instance->m_audioSampleBatchCallback)
    {
        return s_instance->m_audioSampleBatchCallback(data, frames);
    }
    return frames;
}

void TicoCore::InputPollCallback()
{
    // Input is polled externally
}

int16_t TicoCore::InputStateCallback(unsigned port, unsigned device,
                                     unsigned index, unsigned id)
{
    if (!s_instance)
        return 0;
    return s_instance->HandleInputState(port, device, index, id);
}

void TicoCore::LogCallback(enum retro_log_level level, const char *fmt, ...)
{
    // Map retro level to Tico level
    const char *levelStr = "CORE";
    if (level == RETRO_LOG_ERROR)
        levelStr = "CORE_ERR";
    else if (level == RETRO_LOG_WARN)
        levelStr = "CORE_WARN";
    else if (level == RETRO_LOG_INFO)
        levelStr = "CORE_INFO";
    else if (level == RETRO_LOG_DEBUG)
        levelStr = "CORE_DBG";

    // Use our logger
    va_list args;
    va_start(args, fmt);

    // Format string locally first
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    // Remove trailing newline if present since logger adds one
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '\n')
        buffer[len - 1] = '\0';

    Logger::Instance().Log(Logger::Level::DEBUG, levelStr, Logger::None, "%s", buffer);
}

bool TicoCore::SetRumbleStateCallback(unsigned port, enum retro_rumble_effect effect, uint16_t strength)
{
#ifdef __SWITCH__
    if (!s_vibrationInitialized || port >= 4) 
        return false;
        
    if (s_instance && s_instance->GetConfigValue("swanstation_Controller_EnableRumble", "true") != "true")
        return true;
    
    float amplitude = (float)strength / 65535.0f;
    
    int target_device = 1;
    if (port == 0) {
        // P1 takes Handheld or No1 based on dock state
        u8 opMode = appletGetOperationMode();
        target_device = (opMode == AppletOperationMode_Handheld) ? 0 : 1;
    } else {
        // P2 is index 2 (No2), P3 is index 3 (No3), P4 is index 4 (No4)
        target_device = port + 1;
    }

    HidVibrationValue *v = s_currentVibration[target_device];

    if (effect == RETRO_RUMBLE_STRONG) {
        v[0].amp_low = amplitude;
        v[1].amp_low = amplitude;
    } else if (effect == RETRO_RUMBLE_WEAK) {
        v[0].amp_high = amplitude;
        v[1].amp_high = amplitude;
    }

    hidSendVibrationValues(s_vibrationHandles[target_device], v, 2);
    
    return true;
#else
    return false;
#endif
}

//==============================================================================
// Instance Callbacks
//==============================================================================

bool TicoCore::HandleEnvironment(unsigned cmd, void *data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
    {
        auto *cb = (struct retro_log_callback *)data;
        cb->log = LogCallback;
        return true;
    }

    case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
    {
        auto *cb = (retro_rumble_interface *)data;
        if (cb) {
            cb->set_rumble_state = SetRumbleStateCallback;
            LOG_CORE("Provided Rumble Interface");
            return true;
        }
        return false;
    }

    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    {
        *(const char **)data = m_systemDir.c_str();
        return true;
    }

    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    {
        *(const char **)data = m_saveDir.c_str();
        return true;
    }

    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    {
        // Accept any format
        return true;
    }

    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    {
        auto *maps = (const struct retro_memory_map *)data;
        if (maps && maps->descriptors) {
            m_memoryMaps.clear();
            for (unsigned i = 0; i < maps->num_descriptors; i++) {
                const auto& desc = maps->descriptors[i];
                if (desc.ptr) {
                    TicoMemoryMap map;
                    map.start = desc.start;
                    map.length = desc.len;
                    map.ptr = (uint8_t*)desc.ptr;
                    // Optional: handle desc.offset if supported
                    m_memoryMaps.push_back(map);
                }
            }
        }
        return true;
    }

    case RETRO_ENVIRONMENT_SET_HW_RENDER:
    {
        auto *hw = (struct retro_hw_render_callback *)data;
        s_hwRenderCallback = *hw;
        m_hwRender = true;

        // Provide get_current_framebuffer and get_proc_address
        hw->get_current_framebuffer = []() -> uintptr_t
        {
            if (s_instance)
                return s_instance->m_fbo;
            return 0;
        };
        hw->get_proc_address = [](const char *sym) -> retro_proc_address_t
        {
            return (retro_proc_address_t)eglGetProcAddress(sym);
        };

        LOG_CORE("HW render requested: context_type=%d", hw->context_type);
        return true;
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        auto *var = (struct retro_variable *)data;
        if (!var || !var->key)
            return false;

        // Ensure config is loaded
        if (!m_configLoaded)
            LoadConfig();

        // Handle swanstation_ options
        if (strncmp(var->key, "swanstation_", 12) == 0)
        {
            auto it = m_configOptions.find(var->key);
            if (it != m_configOptions.end())
            {
                var->value = it->second.c_str();
                return true;
            }
            return false;
        }

        var->value = nullptr;
        return false;
    }

    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    {
        auto *avInfo = (struct retro_system_av_info *)data;
        m_frameWidth = avInfo->geometry.base_width;
        m_frameHeight = avInfo->geometry.base_height;
        if (avInfo->geometry.aspect_ratio > 0)
        {
            m_aspectRatio = avInfo->geometry.aspect_ratio;
        }
        m_fps = avInfo->timing.fps > 0 ? avInfo->timing.fps : 60.0;
        // Resize FBO if max dimensions changed
        int newMaxW = avInfo->geometry.max_width > 0 ? (int)avInfo->geometry.max_width : m_frameWidth;
        int newMaxH = avInfo->geometry.max_height > 0 ? (int)avInfo->geometry.max_height : m_frameHeight;
        if (newMaxW != m_fboWidth || newMaxH != m_fboHeight)
        {
            m_fboWidth = newMaxW;
            m_fboHeight = newMaxH;
            ResizeFBO(m_fboWidth, m_fboHeight);
        }
        LOG_CORE("SET_SYSTEM_AV_INFO: base %dx%d, FBO %dx%d @ %.2f fps",
                 m_frameWidth, m_frameHeight, m_fboWidth, m_fboHeight, m_fps);
        return true;
    }

    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    {
        auto *geom = (struct retro_game_geometry *)data;
        m_frameWidth = geom->base_width;
        m_frameHeight = geom->base_height;
        if (geom->aspect_ratio > 0)
        {
            m_aspectRatio = geom->aspect_ratio;
        }
        // Resize FBO if max dimensions changed
        int newMaxW = geom->max_width > 0 ? (int)geom->max_width : m_frameWidth;
        int newMaxH = geom->max_height > 0 ? (int)geom->max_height : m_frameHeight;
        if (newMaxW != m_fboWidth || newMaxH != m_fboHeight)
        {
            m_fboWidth = newMaxW;
            m_fboHeight = newMaxH;
            ResizeFBO(m_fboWidth, m_fboHeight);
        }
        return true;
    }
    
    case RETRO_ENVIRONMENT_SET_MESSAGE:
    {
        auto *msg = (const retro_message *)data;
        if (msg && msg->msg)
        {
            m_osdMessage = msg->msg;
            m_osdFrames = msg->frames;
            LOG_CORE("OSD Message (SET_MESSAGE): %s (%d frames)", m_osdMessage.c_str(), m_osdFrames);
        }
        return true;
    }
    
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
    {
        auto *msg = (const retro_message_ext *)data;
        if (msg && msg->msg)
        {
            m_osdMessage = msg->msg;
            m_osdFrames = msg->duration; // Usually given in ms but we store frames/duration depending on how it's sent. Let's just store as frames and let TicoMain convert.
            LOG_CORE("OSD Message (SET_MESSAGE_EXT): %s (%d max frames)", m_osdMessage.c_str(), m_osdFrames);
        }
        return true;
    }

    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;

    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = m_variablesUpdated;
        m_variablesUpdated = false;
        return true;

    default:
        break;
    }

    // Disc control interfaces
    unsigned maskedCmd = cmd & 0xFFFF;
    if (maskedCmd == RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE)
    {
        const auto *cb = (const retro_disk_control_callback *)data;
        if (cb)
        {
            memset(&m_diskControl, 0, sizeof(m_diskControl));
            m_diskControl.set_eject_state = cb->set_eject_state;
            m_diskControl.get_eject_state = cb->get_eject_state;
            m_diskControl.get_image_index = cb->get_image_index;
            m_diskControl.set_image_index = cb->set_image_index;
            m_diskControl.get_num_images = cb->get_num_images;
            m_diskControl.replace_image_index = cb->replace_image_index;
            m_diskControl.add_image_index = cb->add_image_index;
            m_hasDiskControl = true;
        }
        return true;
    }
    if (maskedCmd == RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE)
    {
        const auto *cb = (const retro_disk_control_ext_callback *)data;
        if (cb)
        {
            m_diskControl = *cb;
            m_hasDiskControl = true;
        }
        return true;
    }

    return false;
}

void TicoCore::HandleVideoRefresh(const void *data, unsigned width,
                                  unsigned height, size_t pitch)
{
    if (!data && !m_hwRender)
        return;

    // Resize FBO if dimensions changed (matching LibretroCoreStatic behavior)
    if ((int)width != m_frameWidth || (int)height != m_frameHeight)
    {
        m_frameWidth = width;
        m_frameHeight = height;
        m_fboWidth = width;
        m_fboHeight = height;

        if (m_hwRender && m_frameTexture != 0)
        {
            ResizeFBO(width, height);
        }
    }

    // For HW render, the core renders directly to our FBO
    // For SW render, we need to upload the pixel data
    if (!m_hwRender && data)
    {
        if (m_frameTexture == 0)
        {
            glGenTextures(1, &m_frameTexture);
        }

        glBindTexture(GL_TEXTURE_2D, m_frameTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
}

int16_t TicoCore::HandleInputState(unsigned port, unsigned device,
                                   unsigned index, unsigned id)
{
    if (port >= 4)
        return 0;

    if (device == RETRO_DEVICE_JOYPAD)
    {
        if (id < 16)
        {
            return m_inputState[port][id] ? 1 : 0;
        }
    }
    else if (device == RETRO_DEVICE_ANALOG)
    {
        if (index < 2 && id < 2)
        {
            return m_analogState[port][index][id];
        }
    }

    return 0;
}

//==============================================================================
// Configuration & RetroAchievements
//==============================================================================

void TicoCore::LoadConfig()
{
    if (m_configLoaded)
        return;

    const char *configPath;
#ifdef __SWITCH__
    configPath = "sdmc:/tico/config/cores/swanstation.jsonc";
#else
    configPath = "tico/config/cores/swanstation.jsonc";
#endif

    std::ifstream f(configPath);
    if (!f.good())
    {
        LOG_WARN("CORE", "No config found at %s. Using defaults.", configPath);
        return;
    }

    try
    {
        nlohmann::json j = nlohmann::json::parse(f, nullptr, false, true);
        if (j.is_discarded())
        {
            LOG_ERROR("CORE", "Failed to parse config at %s", configPath);
            return;
        }

        for (auto &el : j.items())
        {
            if (el.value().is_string())
            {
                m_configOptions[el.key()] = el.value().get<std::string>();
            }
            else if (el.value().is_boolean())
            {
                m_configOptions[el.key()] = el.value().get<bool>() ? "true" : "false";
            }
            else if (el.value().is_number())
            {
                m_configOptions[el.key()] = std::to_string(el.value().get<float>());
            }
        }

        m_configLoaded = true;
        LOG_CORE("Loaded %lu options from %s", m_configOptions.size(), configPath);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("CORE", "Error loading config: %s", e.what());
    }
}

std::string TicoCore::GetConfigValue(const std::string &key, const std::string &defaultVal)
{
    auto it = m_configOptions.find(key);
    if (it != m_configOptions.end())
    {
        return it->second;
    }
    return defaultVal;
}

//==============================================================================
// Disk Control
//==============================================================================

unsigned TicoCore::GetDiskCount() const
{
    if (!m_hasDiskControl || !m_diskControl.get_num_images)
        return 0;
    return m_diskControl.get_num_images();
}

unsigned TicoCore::GetCurrentDiskIndex() const
{
    if (!m_hasDiskControl || !m_diskControl.get_image_index)
        return 0;
    return m_diskControl.get_image_index();
}

bool TicoCore::SwapDisk(unsigned index)
{
    if (!m_hasDiskControl || !m_diskControl.set_eject_state ||
        !m_diskControl.set_image_index)
        return false;

    LOG_CORE("SwapDisk: index=%u", index);

    if (!m_diskControl.set_eject_state(true))
    {
        LOG_ERROR("CORE", "SwapDisk: Eject failed");
        return false;
    }

    if (!m_diskControl.set_image_index(index))
    {
        LOG_ERROR("CORE", "SwapDisk: set_image_index(%u) failed", index);
        m_diskControl.set_eject_state(false); 
        return false;
    }

    if (!m_diskControl.set_eject_state(false))
    {
        LOG_ERROR("CORE", "SwapDisk: Insert failed");
        return false;
    }

    LOG_CORE("SwapDisk: Successfully swapped to disc %u", index);
    return true;
}

bool TicoCore::GetDiskLabel(unsigned index, std::string &label) const
{
    if (!m_hasDiskControl || !m_diskControl.get_image_label)
        return false;

    char buf[256];
    if (m_diskControl.get_image_label(index, buf, sizeof(buf)))
    {
        label = buf;
        return true;
    }
    return false;
}

bool TicoCore::SwapDiskByPath(const std::string &discPath)
{
    if (!m_hasDiskControl || !m_diskControl.set_eject_state ||
        !m_diskControl.replace_image_index || !m_diskControl.set_image_index)
    {
        return false;
    }

    if (!m_diskControl.set_eject_state(true))
    {
        LOG_ERROR("CORE", "SwapDiskByPath: Eject failed");
        return false;
    }

    m_swapPending = true;
    m_swapDelayFrames = 120; // Wait 2 seconds of emulated time (at 60fps) before inserting
    m_pendingSwapPath = discPath;

    LOG_CORE("SwapDiskByPath: Ejected tray, queued replacement");
    return true;
}

//==============================================================================
// RetroAchievements
//==============================================================================

void TicoCore::LoadRAConfig()
{
    std::string accountsPath = "sdmc:/tico/config/accounts.jsonc";
    std::ifstream file(accountsPath);
    if (!file.is_open()) {
        LOG_CORE("WARN: accounts.jsonc not found at %s", accountsPath.c_str());
        return;
    }

    LOG_CORE("RA: Found accounts.jsonc at %s", accountsPath.c_str());
    nlohmann::json j = nlohmann::json::parse(file, nullptr, false, true);
    if (!j.is_discarded() && j.is_object()) {
        m_raEnabled = j.value("ra_enabled", false);
        m_raUsername = j.value("ra_username", "");
        m_raToken = j.value("ra_token", "");
        m_raPassword = j.value("ra_password", "");
        m_raHardcore = j.value("ra_hardcore_mode", false);
        
        std::string posStr = j.value("ra_alert_position", "top_right");
        if (posStr == "top_left") m_raAlertPosition = RAAlertPosition::TopLeft;
        else if (posStr == "top_right") m_raAlertPosition = RAAlertPosition::TopRight;
        else if (posStr == "bottom_left") m_raAlertPosition = RAAlertPosition::BottomLeft;
        else if (posStr == "bottom_right") m_raAlertPosition = RAAlertPosition::BottomRight;
        
        LOG_CORE("RA: Config loaded (Enabled: %d, User: %s)", m_raEnabled, m_raUsername.c_str());
    }
}

void TicoCore::SaveRAToken(const std::string& token)
{
    std::string accountsPath = "sdmc:/tico/config/accounts.jsonc";
    nlohmann::json j = nlohmann::json::object();
    std::ifstream inFile(accountsPath);
    if (inFile.is_open()) {
        auto parsed = nlohmann::json::parse(inFile, nullptr, false, true);
        inFile.close();
        if (!parsed.is_discarded()) j = parsed;
    }
    j["ra_token"] = token;
    m_raToken = token;
    std::ofstream outFile(accountsPath);
    if (outFile.is_open()) outFile << j.dump(4);
}

void TicoCore::RAIdentifyGame(rc_client_t* c, TicoCore* core)
{
    uint32_t console_id = 12; // Sony PlayStation
    LOG_CORE("RA: Identifying game... (Console ID: %u)", console_id);
    rc_client_begin_identify_and_load_game(c, console_id, core->m_gamePath.c_str(), nullptr, 0,
        [](int result, const char* error_message, rc_client_t* client, void* userdata) {
            TicoCore* core = (TicoCore*)userdata;
            if (result == RC_OK) {
                const rc_client_game_t* game = rc_client_get_game_info(client);
                if (game && game->title) {
                    core->PushRANotification("RetroAchievements", std::string("Playing: ") + game->title, "ra_icon");
                }
                core->PreloadRABadges();
            } else {
                core->PushRANotification("RetroAchievements", "Rom hash doesn't match or unable to recognize the game.", "ra_icon");
            }
        }, core);
}

void TicoCore::RALoginWithPassword(rc_client_t* c, TicoCore* core)
{
    if (core->m_raPassword.empty()) return;
    rc_client_begin_login_with_password(c, core->m_raUsername.c_str(), core->m_raPassword.c_str(),
        [](int res, const char* err, rc_client_t* c, void* ud) {
            TicoCore* core = (TicoCore*)ud;
            if (res == RC_OK) {
                const rc_client_user_t* user = rc_client_get_user_info(c);
                if (user && user->token) core->SaveRAToken(user->token);
                RAIdentifyGame(c, core);
            } else {
                core->PushRANotification("RetroAchievements", "Failed to authenticate, check your username/password.", "ra_icon");
            }
        }, core);
}

void TicoCore::PushRANotification(const std::string& title, const std::string& desc, const std::string& badge)
{
    RANotification n;
    n.title = title;
    n.description = desc;
    n.badge_name = badge;
    n.timer = 0.0f;
    n.textureId = 0;  // Let the overlay resolve textures lazily during render
    if (m_raNotifications.size() >= 5) m_raNotifications.erase(m_raNotifications.begin());
    m_raNotifications.push_back(std::move(n));
}

unsigned int TicoCore::GetRABadgeTexture(const std::string& badge_name)
{
    auto it = m_raBadgeCache.find(badge_name);
    if (it != m_raBadgeCache.end()) return it->second;
    std::string path = "sdmc:/tico/assets/ra/" + badge_name + ".png";
    // Check if file exists first to avoid stbi crash on non-existent files
    FILE* check = fopen(path.c_str(), "rb");
    if (!check) return 0;  // Badge not downloaded yet
    fclose(check);
    int w, h, ch;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (data) {
        unsigned int tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glBindTexture(GL_TEXTURE_2D, 0);
        stbi_image_free(data);
        m_raBadgeCache[badge_name] = tex;
        return tex;
    }
    return 0;
}

void TicoCore::DownloadAndCacheBadge(const std::string& badge_name)
{
    std::string cachePath = "sdmc:/tico/assets/ra/" + badge_name + ".png";
    FILE* check = fopen(cachePath.c_str(), "rb");
    if (check) { fclose(check); return; }
    std::string url = "https://media.retroachievements.org/Badge/" + badge_name + ".png";
    std::string response;
    CURL* curl = curl_easy_init();
    if (!curl) return;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK || httpCode != 200 || response.empty()) return;
    mkdir("sdmc:/tico/assets/ra", 0777);
    FILE* fp = fopen(cachePath.c_str(), "wb");
    if (fp) {
        fwrite(response.data(), 1, response.size(), fp);
        fclose(fp);
    }
}

void TicoCore::PreloadRABadges()
{
    if (!m_rcClient) return;
    rc_client_achievement_list_t* list = rc_client_create_achievement_list(m_rcClient,
        RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
    if (!list) return;
    std::vector<std::string> badges;
    for (uint32_t b = 0; b < list->num_buckets; b++) {
        for (uint32_t a = 0; a < list->buckets[b].num_achievements; a++) {
            const rc_client_achievement_t* ach = list->buckets[b].achievements[a];
            if (ach && ach->badge_name[0]) {
                std::string bn = ach->badge_name;
                std::string path = "sdmc:/tico/assets/ra/" + bn + ".png";
                FILE* check = fopen(path.c_str(), "rb");
                if (check) { fclose(check); continue; }
                badges.push_back(bn);
            }
        }
    }
    rc_client_destroy_achievement_list(list);
    for (const auto& badge : badges) {
        std::lock_guard<std::mutex> lock(m_raJobMutex);
        RAJob job; job.url = "__badge__"; job.post_data = badge; job.callback = nullptr;
        m_raJobQueue.push_back(std::move(job));
    }
    m_raJobCond.notify_one();
}

void TicoCore::LoadRAIcon() { }

void TicoCore::ProcessPendingBadgeUploads()
{
    std::vector<std::pair<std::string, std::vector<unsigned char>>> uploads;
    {
        std::lock_guard<std::mutex> lock(m_raBadgeUploadMutex);
        if (m_raPendingBadgeUploads.empty()) return;
        uploads = std::move(m_raPendingBadgeUploads);
    }
    for (auto& [name, data] : uploads) {
        if (data.empty()) continue;  // Skip empty data to avoid stbi crash
        int w, h, ch;
        unsigned char* pixels = stbi_load_from_memory(data.data(), (int)data.size(), &w, &h, &ch, 4);
        if (pixels) {
            unsigned int tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            glBindTexture(GL_TEXTURE_2D, 0);
            stbi_image_free(pixels);
            m_raBadgeCache[name] = tex;
        }
    }
}

