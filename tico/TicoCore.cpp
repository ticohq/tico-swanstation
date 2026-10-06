/// @file TicoCore.cpp
/// @brief SwanStation driven directly (SwanHost), with the tico overlay.
/// Frames go to TicoShaderChain (software pixels, or the Vulkan renderer's
/// image on the shared device); memory card 1 is kept as <game>.mcd.

#include "TicoCore.h"
#include "TicoVulkan.h"
#include "swan/SwanHost.h"
#include "common/byte_stream.h"
#include "core/bus.h"
#include "core/cheats.h"
#include "core/cpu_core.h"
#include "core/host_interface.h"
#include "core/memory_card.h"
#include "core/pad.h"
#include "core/settings.h"
#include "core/system.h"
#include "libretro/libretro_audio_stream.h"
#include "TicoConfig.h"
#include "TicoSafeFile.h"
#include "TicoUtils.h"
#include <algorithm>
#include <json.hpp>
#include <SDL.h>
#include <SDL_mixer.h>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>
#include "TicoLogger.h"

// RetroAchievements
#include "rc_client.h"
#include <curl/curl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>
#include "TicoLogger.h"
#include "deps/stb/stb_image.h"



#ifdef __SWITCH__
#include <switch.h>

/// @brief Switch vibration handles and state
static HidVibrationDeviceHandle s_vibrationHandles[5][2] = {};
static HidVibrationValue s_currentVibration[5][2] = {};
static bool s_vibrationInitialized = false;
#endif



#define tico_debug_log(...) LOG_CORE(__VA_ARGS__)

// Earlier versions kept in backups/ beside each file: the last few sessions'
// saves, and the state each slot held before it was saved over.
static constexpr int kSaveBackups = 3;
static constexpr int kStateBackups = 1;

// Memory card 1 with the "Libretro" type is the frontend's to keep, as
// <game>.mcd beside the other saves (a raw 128 KB card image, the format
// RetroArch's .srm uses too).
static MemoryCardImage::DataArray *FrontendCard()
{
    if (!System::IsValid() || g_settings.memory_card_types[0] != MemoryCardType::Libretro)
        return nullptr;
    MemoryCard *card = g_pad.GetMemoryCard(0);
    return card ? &card->GetData() : nullptr;
}

static std::string SaveBase(const std::string &gamePath)
{
    std::string filename = gamePath;
    const size_t lastSlash = filename.find_last_of("/\\");
    if (lastSlash != std::string::npos)
        filename = filename.substr(lastSlash + 1);
    const size_t lastDot = filename.find_last_of('.');
    if (lastDot != std::string::npos)
        filename = filename.substr(0, lastDot);
    return TicoConfig::SavesPath() + filename;
}

void TicoCore::LoadSaveData()
{
    MemoryCardImage::DataArray *card = FrontendCard();
    if (!card)
        return;
    for (const char *ext : {".mcd", ".srm"})
    {
        const std::string path = SaveBase(m_gamePath) + ext;
        std::ifstream file(path, std::ios::binary);
        if (file && file.read(reinterpret_cast<char *>(card->data()), card->size()))
        {
            tico_debug_log("Loaded memory card from %s", path.c_str());
            m_savedCard.assign(card->begin(), card->end());
            return;
        }
    }
}

void TicoCore::SaveSaveData()
{
    MemoryCardImage::DataArray *card = FrontendCard();
    if (!card)
        return;
    // unchanged since it was loaded or last written
    if (m_savedCard.size() == card->size() && std::equal(card->begin(), card->end(), m_savedCard.begin()))
        return;
    TicoConfig::MakeDirs(TicoConfig::SavesPath());
    const std::string path = SaveBase(m_gamePath) + ".mcd";
    if (TicoSafeFile::Write(path, card->data(), card->size(), kSaveBackups))
    {
        m_savedCard.assign(card->begin(), card->end());
        tico_debug_log("Saved memory card to %s", path.c_str());
    }
    else
        tico_debug_log("ERROR: could not save memory card to %s", path.c_str());
}

// Static instance for callbacks
static TicoCore *s_instance = nullptr;
static const char *RAUserAgent();


//==============================================================================
// RetroAchievements Callbacks
//==============================================================================
static uint32_t RC_CCONV RAReadMemory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client)
{
    if (!s_instance) return 0;
    
    // rcheevos' PlayStation map: 2 MB of RAM at 0, the 1 KB scratchpad at 0x200000
    if (!System::IsValid())
        return 0;
    if (address < 0x200000) {
        if (address + num_bytes > Bus::g_ram_size)
            return 0;
        memcpy(buffer, Bus::g_ram + address, num_bytes);
        return num_bytes;
    }
    const uint32_t offset = address - 0x200000;
    if (offset + num_bytes > CPU::g_state.dcache.size())
        return 0;
    memcpy(buffer, CPU::g_state.dcache.data() + offset, num_bytes);
    return num_bytes;
}

static size_t CurlWriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Persistent RA worker thread entry point
void TicoCore::RAWorkerEntry(void* arg) {
    TicoCore* self = (TicoCore*)arg;
    
    while (true) {
        RAJob job;
        {
            std::unique_lock<std::mutex> lock(self->m_raJobMutex);
            self->m_raJobCond.wait(lock, [self]() {
                return !self->m_raJobQueue.empty() || !self->m_raWorkerRunning;
            });
            
            if (!self->m_raWorkerRunning && self->m_raJobQueue.empty())
                break;
            
            job = std::move(self->m_raJobQueue.front());
            self->m_raJobQueue.pop_front();
        }
        
        // Handle badge download jobs specially
        if (job.url == "__badge__") {
            self->DownloadAndCacheBadge(job.post_data);
            continue;
        }
        
        // Do the HTTP request on this worker thread
        CURL *curl = curl_easy_init();
        std::string readBuffer;
        long http_code = 0;
        std::string errorMsg;
        std::string requestUrl = job.url;
        
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, job.url.c_str());
            curl_easy_setopt(curl, CURLOPT_USERAGENT, RAUserAgent());
            if (!job.post_data.empty()) {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, job.post_data.c_str());
            }
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
            
            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK) {
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            } else {
                errorMsg = curl_easy_strerror(res);
                http_code = 500;
            }
            curl_easy_cleanup(curl);
        }
        
        // Queue result callback for main thread
        {
            std::lock_guard<std::mutex> lock(self->m_raCallbackMutex);
            self->m_raPendingCallbacks.push_back(
                [job, http_code, readBuffer, errorMsg, requestUrl]() {
                    tico_debug_log("RA: HTTP Request -> %s", requestUrl.c_str());
                    if (!errorMsg.empty()) {
                        tico_debug_log("RA HTTP Error: %s", errorMsg.c_str());
                    }
                    tico_debug_log("RA: HTTP Response %ld (size: %zu)", http_code, readBuffer.size());
                    
                    rc_api_server_response_t response;
                    memset(&response, 0, sizeof(response));
                    response.body = readBuffer.c_str();
                    response.body_length = readBuffer.size();
                    response.http_status_code = http_code;
                    
                    rc_client_server_callback_t cb = (rc_client_server_callback_t)job.callback;
                    if (cb) {
                        cb(&response, job.callback_data);
                    }
                }
            );
        }
    }
}

void TicoCore::StartRAWorker() {
#ifdef __SWITCH__
    m_raWorkerRunning = true;
    memset(&m_raThread, 0, sizeof(m_raThread));
    // Pin to core 0 (free for emulators), priority 0x2C (normal), stack 256KB
    Result rc = threadCreate(&m_raThread, RAWorkerEntry, this, NULL, 0x40000, 0x2C, 0);
    if (R_SUCCEEDED(rc)) {
        rc = threadStart(&m_raThread);
        if (R_SUCCEEDED(rc)) {
            m_raThreadCreated = true;
            tico_debug_log("RA: Worker thread started (core 0, 256KB stack)");
        } else {
            tico_debug_log("RA: threadStart failed: 0x%x", rc);
            threadClose(&m_raThread);
            m_raWorkerRunning = false;
        }
    } else {
        tico_debug_log("RA: threadCreate failed: 0x%x", rc);
        m_raWorkerRunning = false;
    }
#else
    // Stub
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
    tico_debug_log("RA: Worker thread stopped");
#endif
}

#ifndef TICO_APP_VERSION
#define TICO_APP_VERSION "dev"
#endif

// How RetroAchievements identifies this client: the frontend, the libretro
// core and the rcheevos integration, like other libretro frontends report it.
static const char *RAUserAgent()
{
    static std::string agent;
    if (agent.empty())
    {
        agent = std::string("tico-swanstation/") + TICO_APP_VERSION + " (Nintendo Switch) SwanStation";
        char clause[64] = "";
        if (rc_client_get_user_agent_clause(nullptr, clause, sizeof(clause)) > 0)
            agent += std::string(" ") + clause;
    }
    return agent.c_str();
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
        // Fallback: synchronous if worker not running
        tico_debug_log("RA: HTTP Request (sync) -> %s", request->url);
        CURL *curl = curl_easy_init();
        std::string readBuffer;
        long http_code = 0;
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, request->url);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, RAUserAgent());
            if (request->post_data) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->post_data);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            else { tico_debug_log("RA HTTP Error: %s", curl_easy_strerror(res)); http_code = 500; }
            curl_easy_cleanup(curl);
        }
        tico_debug_log("RA: HTTP Response %ld (size: %zu)", http_code, readBuffer.size());
        rc_api_server_response_t response;
        memset(&response, 0, sizeof(response));
        response.body = readBuffer.c_str();
        response.body_length = readBuffer.size();
        response.http_status_code = http_code;
        if (callback) callback(&response, callback_data);
    }
#else
    // On non-Switch: just do it synchronously
    tico_debug_log("RA: HTTP Request -> %s", request->url);
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
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        else { tico_debug_log("RA HTTP Error: %s", curl_easy_strerror(res)); http_code = 500; }
        curl_easy_cleanup(curl);
    }
    tico_debug_log("RA: HTTP Response %ld (size: %zu)", http_code, readBuffer.size());
    rc_api_server_response_t response;
    memset(&response, 0, sizeof(response));
    response.body = readBuffer.c_str();
    response.body_length = readBuffer.size();
    response.http_status_code = http_code;
    if (callback) callback(&response, callback_data);
#endif
}

//==============================================================================
// Content paths
//==============================================================================

namespace {
std::string ContentRoot(const char *key, const char *defaultRoot)
{
    static nlohmann::json config = [] {
#ifdef __SWITCH__
        std::ifstream f("sdmc:/tico/config/cores/swanstation.jsonc");
#else
        std::ifstream f("tico/config/cores/swanstation.jsonc");
#endif
        nlohmann::json j = f.good() ? nlohmann::json::parse(f, nullptr, false, true)
                                    : nlohmann::json::object();
        return j.is_object() ? j : nlohmann::json::object();
    }();

    std::string root = defaultRoot;
    auto it = config.find(key);
    if (it != config.end() && it->is_string() && !it->get<std::string>().empty())
        root = it->get<std::string>();
    if (root.back() != '/')
        root += '/';
    return root;
}
} // namespace

namespace TicoConfig {
std::string SystemPath() { return ContentRoot("tico_system_path", "sdmc:/tico/system/") + "psx/"; }
std::string SavesPath() { return ContentRoot("tico_saves_path", "sdmc:/tico/saves/") + CURRENT_SLUG + "/"; }
std::string StatesPath() { return ContentRoot("tico_states_path", "sdmc:/tico/states/") + CURRENT_SLUG + "/"; }

void MakeDirs(const std::string &path)
{
    // A custom root may not exist yet, so create every missing level.
    for (size_t at = path.find('/', path.find(":/") != std::string::npos ? path.find(":/") + 2 : 1);
         at != std::string::npos; at = path.find('/', at + 1))
        mkdir(path.substr(0, at).c_str(), 0777);
}
} // namespace TicoConfig

//==============================================================================
// Construction
//==============================================================================

TicoCore::TicoCore()
{
    memset(m_inputState, 0, sizeof(m_inputState));
    memset(m_analogState, 0, sizeof(m_analogState));

    m_systemDir = TicoConfig::SystemPath();
    m_saveDir = TicoConfig::SavesPath();
    TicoConfig::MakeDirs(m_systemDir);
    TicoConfig::MakeDirs(m_saveDir);
}

TicoCore::~TicoCore()
{
    tico_debug_log("~TicoCore: destroying (gameLoaded=%d, initialized=%d)",
             m_gameLoaded, m_initialized);

    UnloadGame();

    if (m_initialized)
    {
        SwanHost::Shutdown();
        m_initialized = false;
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

    if (s_instance == this)
    {
        s_instance = nullptr;
    }

    tico_debug_log("~TicoCore: done");
}

//==============================================================================
// Initialization
//==============================================================================

bool TicoCore::Init()
{
    if (m_initialized)
        return true;

    s_instance = this;

    tico_debug_log("=== TicoCore::Init() ===");

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
        
        hidInitializeVibrationDevices(s_vibrationHandles[0], 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
        hidInitializeVibrationDevices(s_vibrationHandles[1], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
        hidInitializeVibrationDevices(s_vibrationHandles[2], 2, HidNpadIdType_No2, HidNpadStyleTag_NpadJoyDual);
        hidInitializeVibrationDevices(s_vibrationHandles[3], 2, HidNpadIdType_No3, HidNpadStyleTag_NpadJoyDual);
        hidInitializeVibrationDevices(s_vibrationHandles[4], 2, HidNpadIdType_No4, HidNpadStyleTag_NpadJoyDual);
        
        s_vibrationInitialized = true;
        tico_debug_log("Vibration devices initialized for P1-P4");
    }
#endif

    // Ensure the system directory exists (the BIOS images)
    struct stat st = {0};
    if (stat(m_systemDir.c_str(), &st) == -1) {
        mkdir(m_systemDir.c_str(), 0777);
    }
    tico_debug_log("System dir: %s", m_systemDir.c_str());

    // Load configuration to ensure variables are ready for init
    LoadConfig();
    tico_debug_log("Config loaded, %lu options", m_configOptions.size());

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
        if (m_trophySound) tico_debug_log("RA: Loaded trophy.mp3 successfully.");
        else tico_debug_log("RA: Failed to load trophy.mp3 -> %s", Mix_GetError());
    }

    // The core reads settings, prints messages and plays audio through these
    SwanHost::Frontend &host = SwanHost::Get();
    host.option = [this](const std::string &key) -> const std::string * {
        auto it = m_configOptions.find(key);
        return it != m_configOptions.end() ? &it->second : nullptr;
    };
    host.osd = [this](const std::string &message, float seconds) {
        ShowOSD(message, static_cast<int>(seconds * 60.0f));
    };
    host.audio = [this](const int16_t *samples, uint32_t frames) {
        if (m_audioSampleBatchCallback)
            m_audioSampleBatchCallback(samples, frames);
    };
    host.softwareFrame = [this](const void *data, uint32_t width, uint32_t height, uint32_t pitch,
                                HostDisplayPixelFormat format) {
        if (!data)
            return; // the previous frame stays up
        m_frameWidth = static_cast<int>(width);
        m_frameHeight = static_cast<int>(height);
        if (m_videoCallback)
            m_videoCallback(data, width, height, pitch,
                            format == HostDisplayPixelFormat::BGRA8 ? RETRO_PIXEL_FORMAT_XRGB8888
                                                                    : RETRO_PIXEL_FORMAT_RGB565);
    };
    host.systemDir = m_systemDir;
    host.savesDir = m_saveDir;
    host.cacheDir = TicoConfig::CachePath();
    host.renderer = SwanHost::ParseRenderer(GetConfigValue("swanstation_GPU_Renderer", "Vulkan"));
    host.pads[0] = ParsePadType(GetConfigValue("pad1_type", "DualShock"));
    host.pads[1] = ParsePadType(GetConfigValue("pad2_type", "DualShock"));
    if (!SwanHost::Initialize())
    {
        tico_debug_log("ERROR: SwanStation host failed to initialize");
        return false;
    }
    tico_debug_log("SwanStation ready (renderer %s)", GetConfigValue("swanstation_GPU_Renderer", "Vulkan").c_str());

    // ------------------------------------------------------------------
    // Setup RetroAchievements Client
    // ------------------------------------------------------------------
    LoadRAConfig();
    
    m_rcClient = rc_client_create(RAReadMemory, RAServerCall);
    if (m_rcClient) {
        rc_client_set_event_handler(m_rcClient, [](const rc_client_event_t* event, rc_client_t* client) {
            if (!s_instance) return;
            switch (event->type) {
                case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
                    if (event->achievement) {
                        std::string title = event->achievement->title;
                        std::string desc = event->achievement->description;
                        std::string badge = event->achievement->badge_name;
                        s_instance->PushRANotification(title, desc, badge);
                        if (s_instance->m_trophySound) {
                            Mix_PlayChannel(-1, s_instance->m_trophySound, 0);
                        }
                        tico_debug_log("RA: Achievement triggered: %s (badge: %s)", title.c_str(), badge.c_str());
                    }
                    break;
                case RC_CLIENT_EVENT_GAME_COMPLETED:
                    s_instance->PushRANotification("Game Mastered!", "All achievements unlocked!", "ra_icon");
                    if (s_instance->m_trophySound) {
                        Mix_PlayChannel(-1, s_instance->m_trophySound, 0);
                    }
                    break;
                case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
                    if (event->leaderboard) {
                        s_instance->PushRANotification("Leaderboard", event->leaderboard->title, "ra_icon");
                    }
                    break;
                case RC_CLIENT_EVENT_RESET:
                    // rc_client asks for a reset when hardcore turns on mid-game,
                    // so nothing from the softcore session carries over.
                    tico_debug_log("RA: reset requested by rc_client");
                    s_instance->Reset();
                    break;
                case RC_CLIENT_EVENT_SERVER_ERROR:
                    if (event->server_error) {
                        tico_debug_log("RA: Server error: %s", event->server_error->error_message);
                    }
                    break;
                default:
                    break;
            }
        });
        rc_client_set_hardcore_enabled(m_rcClient, m_raHardcore);
        
        StartRAWorker();
        
        if (!m_raUsername.empty() && !m_raToken.empty()) {
            tico_debug_log("RA: Existent token found. Auto login as %s...", m_raUsername.c_str());
            rc_client_begin_login_with_token(m_rcClient, m_raUsername.c_str(), m_raToken.c_str(),
                [](int res, const char* err, rc_client_t* c, void* ud) {
                    TicoCore* self = (TicoCore*)ud;
                    if (res == RC_OK) {
                        tico_debug_log("RA login success with token!");
                        // Token valid, let's identify the game
                        if (self->m_gameLoaded && !self->m_gamePath.empty()) {
                            RAIdentifyGame(c, self);
                        }
                    } else if (res == RC_INVALID_CREDENTIALS && !self->m_raPassword.empty()) {
                        tico_debug_log("RA token invalid or expired. Trying password...");
                        RALoginWithPassword(c, self);
                    } else {
                        tico_debug_log("RA login failed -> %s", err ? err : "Unknown");
                        self->PushRANotification("Login Failed", "Check your credentials.", "ra_icon");
                    }
                }, this);
        } else if (!m_raUsername.empty() && !m_raPassword.empty()) {
            tico_debug_log("RA: Auto login using password...");
            RALoginWithPassword(m_rcClient, this);
        }
    }

    m_initialized = true;
    return true;
}

//==============================================================================
// Game Loading
//==============================================================================

bool TicoCore::LoadGame(const std::string &path)
{
    tico_debug_log("=== TicoCore::LoadGame ===");
    tico_debug_log("  path: %s", path.c_str());

    m_gamePath = path;

    if (!m_initialized)
    {
        tico_debug_log("Not initialized, calling Init()");
        if (!Init())
        {
            tico_debug_log("ERROR: Init() failed");
            return false;
        }
    }

    // Disc images (.cue/.bin, .chd, .iso, .m3u playlists, .pbp), EXEs and
    // PSFs: the core opens them by path.
    if (!SwanHost::Boot(path))
    {
        tico_debug_log("ERROR: SwanStation could not boot %s", path.c_str());
        return false;
    }
    m_aspectRatio = SwanHost::AspectRatio();
    m_fps = SwanHost::FrameRate();
    m_sampleRate = LibretroAudioStream::SAMPLE_RATE;
    tico_debug_log("Booted: %.2f fps, aspect %.3f", m_fps, m_aspectRatio);

    m_gameLoaded = true;
    m_paused = false;
    tico_debug_log("LoadGame: Complete!");

    // Load native save data, falling back to legacy .srm saves when needed.
    LoadSaveData();
    LoadCheats();

    return true;
}

void TicoCore::UnloadGame()
{
    if (!m_gameLoaded)
        return;

    SaveSaveData();
    SwanHost::Unload();
    m_gameLoaded = false;

}

//==============================================================================
// Frame execution
//==============================================================================

void TicoCore::RunFrame()
{
    if (!m_gameLoaded || m_paused)
        return;

    if (m_variablesUpdated)
    {
        m_variablesUpdated = false;
        SwanHost::ReloadSettings();
    }

    // the overlay's pads, as PlayStation buttons (SwanHost::Button bits) and sticks
    for (unsigned port = 0; port < 2; ++port)
    {
        uint32_t buttons = 0;
        for (unsigned id = 0; id < SwanHost::ButtonCount; ++id)
            if (m_inputState[port][id])
                buttons |= 1u << id;
        std::array<float, 4> axes = {};
        for (unsigned stick = 0; stick < 2; ++stick)
            for (unsigned axis = 0; axis < 2; ++axis)
                axes[stick * 2 + axis] = m_analogState[port][stick][axis] / 32767.0f;
        SwanHost::SetPad(port, buttons, axes);
    }

    SwanHost::RunFrame();
    m_aspectRatio = SwanHost::AspectRatio();
    m_fps = SwanHost::FrameRate();

    for (unsigned port = 0; port < 2; ++port)
    {
        float large = 0.0f, small = 0.0f;
        SwanHost::GetRumble(port, large, small);
        SetRumble(port, large, small);
    }

    // memory card changes reach the SD card within a few seconds
    if (++m_saveCheckFrames >= 300)
    {
        m_saveCheckFrames = 0;
        SaveSaveData();
    }

    // RetroAchievements frame tick
    if (m_rcClient) {
        rc_client_do_frame(m_rcClient);
    }
    
    // Process async badge uploads
    ProcessPendingBadgeUploads();
    
    // Execute pending RA callbacks on main thread
    std::vector<std::function<void()>> cbs;
    {
        std::lock_guard<std::mutex> lock(m_raCallbackMutex);
        cbs = std::move(m_raPendingCallbacks);
    }
    for(auto& cb : cbs) cb();
}

void TicoCore::Reset()
{
    if (m_gameLoaded)
    {
        g_host_interface->ResetSystem();
        // achievement progress restarts with the game
        if (m_rcClient)
            rc_client_reset(m_rcClient);
    }
}

//==============================================================================
// Cheats
//==============================================================================
static std::string CheatsBase(const std::string &gamePath)
{
    std::string name = gamePath;
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos)
        name = name.substr(0, dot);
    const std::string dir = "sdmc:/tico/cheats/" + TicoConfig::CURRENT_SLUG + "/";
    TicoConfig::MakeDirs(dir);
    return dir + name;
}

// One cheat's codes: GameShark lines ("80012345 0063") joined with + or ;
static void SplitCodes(const std::string &text, std::vector<std::string> &out)
{
    std::string code;
    for (const char c : text + ";")
    {
        if (c == '+' || c == ';' || c == ',')
        {
            code = TicoUtils::Trim(code);
            if (!code.empty())
                out.push_back(code);
            code.clear();
        }
        else
            code += c;
    }
}

void TicoCore::LoadCheats()
{
    m_cheats.clear();
    const std::string base = CheatsBase(m_gamePath);

    // RetroArch .cht: cheatN_desc / cheatN_code (enable flags are ignored:
    // every cheat starts off)
    std::ifstream cht(base + ".cht");
    if (cht.is_open())
    {
        std::map<int, Cheat> byIndex;
        std::string line;
        while (std::getline(cht, line))
        {
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string key = TicoUtils::Trim(line.substr(0, eq));
            std::string value = TicoUtils::Trim(line.substr(eq + 1));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                value = value.substr(1, value.size() - 2);
            int index = -1;
            char field[16] = {0};
            if (sscanf(key.c_str(), "cheat%d_%15s", &index, field) != 2 || index < 0)
                continue;
            if (!strcmp(field, "desc"))
                byIndex[index].name = value;
            else if (!strcmp(field, "code"))
                SplitCodes(value, byIndex[index].codes);
        }
        for (auto &entry : byIndex)
        {
            if (entry.second.codes.empty())
                continue;
            if (entry.second.name.empty())
                entry.second.name = "Cheat " + std::to_string(entry.first + 1);
            m_cheats.push_back(entry.second);
        }
    }

    // .cheats: "# Name", then one code (or several joined with +) per line
    std::ifstream simple(base + ".cheats");
    if (simple.is_open())
    {
        std::string line;
        while (std::getline(simple, line))
        {
            const std::string text = TicoUtils::Trim(line);
            if (text.empty() || text[0] == '!')
                continue;
            if (text[0] == '#')
            {
                m_cheats.push_back(Cheat());
                m_cheats.back().name = TicoUtils::Trim(text.substr(1));
                continue;
            }
            if (m_cheats.empty())
            {
                m_cheats.push_back(Cheat());
                m_cheats.back().name = "Cheat";
            }
            SplitCodes(text, m_cheats.back().codes);
        }
    }
    tico_debug_log("CHEATS: %zu for %s", m_cheats.size(), base.c_str());
}

// A cheat's lines stay one code: conditional GameShark lines (D0/E0...)
// guard the line after them.
void TicoCore::ApplyCheats()
{
    if (!m_gameLoaded)
        return;
    std::unique_ptr<CheatList> list;
    uint32_t index = 0;
    for (const Cheat &cheat : m_cheats)
    {
        if (!cheat.enabled)
            continue;
        std::string joined;
        for (const std::string &code : cheat.codes)
            joined += (joined.empty() ? "" : "+") + code;
        CheatCode cc;
        cc.description = cheat.name;
        cc.enabled = true;
        if (!CheatList::ParseLibretroCheat(&cc, joined.c_str()))
        {
            tico_debug_log("CHEATS: cannot read %s", cheat.name.c_str());
            continue;
        }
        if (!list)
            list = std::make_unique<CheatList>();
        list->SetCode(index++, std::move(cc));
    }
    System::SetCheatList(std::move(list));
}

void TicoCore::ToggleCheat(size_t index)
{
    if (index >= m_cheats.size() || IsHardcoreActive())
        return;
    m_cheats[index].enabled = !m_cheats[index].enabled;
    ApplyCheats();
}

bool TicoCore::IsHardcoreActive() const
{
    return m_rcClient && rc_client_get_hardcore_enabled(m_rcClient);
}

bool TicoCore::CanPause(int &secondsRemaining)
{
    secondsRemaining = 0;
    if (!m_gameLoaded || !IsHardcoreActive())
        return true;
    uint32_t framesRemaining = 0;
    if (rc_client_can_pause(m_rcClient, &framesRemaining))
        return true;
    const double fps = m_fps > 0.0 ? m_fps : 60.0;
    secondsRemaining = (int)((framesRemaining + fps - 1.0) / fps);
    if (secondsRemaining < 1)
        secondsRemaining = 1;
    return false;
}

void TicoCore::Idle()
{
    ProcessPendingBadgeUploads();
    std::vector<std::function<void()>> cbs;
    {
        std::lock_guard<std::mutex> lock(m_raCallbackMutex);
        cbs = std::move(m_raPendingCallbacks);
    }
    for (auto &cb : cbs)
        cb();
    if (m_rcClient)
        rc_client_idle(m_rcClient);
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

// rc_client's achievement progress (hit counts, measured values) for a state
// file, so loading it restores where every achievement stood.
static std::string ProgressPath(const std::string &statePath)
{
    return statePath + ".ra";
}

bool TicoCore::SaveState(const std::string &path)
{
    if (!m_gameLoaded)
        return false;

    std::vector<uint8_t> data(System::MAX_SAVE_STATE_SIZE);
    std::unique_ptr<ByteStream> stream =
        ByteStream_CreateMemoryStream(data.data(), static_cast<uint32_t>(data.size()));
    if (!System::SaveState(stream.get()))
    {
        tico_debug_log("ERROR: SwanStation could not save the state");
        return false;
    }
    const size_t size = static_cast<size_t>(stream->GetPosition());

    // the slot's previous state stays in backups/ (one level)
    const bool written = TicoSafeFile::Write(path, data.data(), size, kStateBackups);
    if (!written)
    {
        tico_debug_log("ERROR: Failed to write save state: %s", path.c_str());
        return false;
    }
    tico_debug_log("Saved state to %s", path.c_str());

    const std::string progressPath = ProgressPath(path);
    const size_t progressSize = m_rcClient ? rc_client_progress_size(m_rcClient) : 0;
    std::vector<uint8_t> progress(progressSize);
    if (progressSize > 0 &&
        rc_client_serialize_progress_sized(m_rcClient, progress.data(), progressSize) == RC_OK)
    {
        if (FILE *pf = fopen(progressPath.c_str(), "wb"))
        {
            fwrite(progress.data(), 1, progressSize, pf);
            fclose(pf);
        }
    }
    else
    {
        // a stale file would restore progress from an older state
        remove(progressPath.c_str());
    }
    return written;
}

bool TicoCore::LoadState(const std::string &path)
{
    if (!m_gameLoaded)
        return false;

    // RetroAchievements hardcore forbids loading states.
    if (IsHardcoreActive())
    {
        tico_debug_log("LoadState: refused, hardcore mode is active");
        return false;
    }

    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp)
    {
        tico_debug_log("LoadState: File not found: %s", path.c_str());
        return false;
    }

    fseek(fp, 0, SEEK_END);
    size_t fileSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fileSize == 0)
    {
        fclose(fp);
        return false;
    }

    std::vector<uint8_t> data(fileSize);
    if (fread(data.data(), 1, fileSize, fp) != fileSize)
    {
        fclose(fp);
        return false;
    }
    fclose(fp);

    // Flush audio
    if (m_audioFlushCallback)
    {
        tico_debug_log("Resetting SDL audio device...");
        m_audioFlushCallback();
    }

    std::unique_ptr<ByteStream> stream =
        ByteStream_CreateReadOnlyMemoryStream(data.data(), static_cast<uint32_t>(fileSize));
    bool success = System::LoadState(stream.get());

    if (success)
    {
        tico_debug_log("Loaded state from %s", path.c_str());
        // Restore achievement progress with the state; a state saved without
        // it resets progress, so nothing from the abandoned timeline counts.
        if (m_rcClient)
        {
            std::vector<uint8_t> progress;
            if (FILE *pf = fopen(ProgressPath(path).c_str(), "rb"))
            {
                fseek(pf, 0, SEEK_END);
                const long progressSize = ftell(pf);
                fseek(pf, 0, SEEK_SET);
                if (progressSize > 0)
                {
                    progress.resize((size_t)progressSize);
                    if (fread(progress.data(), 1, progress.size(), pf) != progress.size())
                        progress.clear();
                }
                fclose(pf);
            }
            if (progress.empty() ||
                rc_client_deserialize_progress_sized(m_rcClient, progress.data(), progress.size()) != RC_OK)
                rc_client_deserialize_progress_sized(m_rcClient, nullptr, 0);
        }
        tico_debug_log("Running one frame to force display update...");
        SwanHost::RunFrame();
    }
    else
    {
        tico_debug_log("ERROR: SwanStation could not load the state");
    }
    return success;
}

//==============================================================================
// Rumble
//==============================================================================

void TicoCore::SetRumble(unsigned port, float large, float small)
{
#ifdef __SWITCH__
    if (!s_vibrationInitialized || port >= 4 ||
        GetConfigValue("swanstation_Controller_EnableRumble", "true") != "true")
        return;
    // player 1 is the handheld pair or pad 1, as the system shows them
    int device = port + 1;
    if (port == 0 && appletGetOperationMode() == AppletOperationMode_Handheld)
        device = 0;
    HidVibrationValue *v = s_currentVibration[device];
    if (v[0].amp_low == large && v[0].amp_high == small)
        return;
    for (int i = 0; i < 2; ++i)
    {
        v[i].amp_low = large;
        v[i].amp_high = small;
    }
    hidSendVibrationValues(s_vibrationHandles[device], v, 2);
#endif
}

SwanHost::PadType TicoCore::ParsePadType(const std::string &value)
{
    if (value == "Digital")
        return SwanHost::PadType::Digital;
    if (value == "None")
        return SwanHost::PadType::None;
    return SwanHost::PadType::DualShock;
}

//==============================================================================
// Configuration
//==============================================================================

void TicoCore::SetOption(const std::string &key, const std::string &value)
{
    std::string &stored = m_configOptions[key];
    if (stored == value)
        return;
    stored = value;
    m_variablesUpdated = true;
}

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
        tico_debug_log("No config found at %s. Using defaults.", configPath);
        m_configLoaded = true; // Mark as loaded so we don't retry
        return;
    }

    nlohmann::json j = nlohmann::json::parse(f, nullptr, false, true);
    if (j.is_discarded())
    {
        tico_debug_log("ERROR: Failed to parse config at %s", configPath);
        m_configLoaded = true;
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
    tico_debug_log("Loaded %lu options from %s", m_configOptions.size(), configPath);
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

bool TicoCore::GetVariable(const char *key, const char **value)
{
    auto it = m_configOptions.find(key);
    if (it != m_configOptions.end())
    {
        *value = it->second.c_str();
        return true;
    }
    return false;
}

void TicoCore::LoadRAConfig()
{
    std::string accountsPath = "sdmc:/tico/config/accounts.jsonc";
    std::ifstream file(accountsPath);
    if (!file.is_open()) {
        tico_debug_log("WARN: accounts.jsonc not found at %s", accountsPath.c_str());
        return;
    }

    tico_debug_log("RA: Found accounts.jsonc at %s", accountsPath.c_str());
    nlohmann::json j = nlohmann::json::parse(file, nullptr, false, true);
    if (!j.is_discarded() && j.is_object()) {
        m_raEnabled = j.value("ra_enabled", false);
        m_raUsername = j.value("ra_username", "");
        m_raToken = j.value("ra_token", "");
        m_raPassword = j.value("ra_password", "");
        m_raHardcore = j.value("ra_hardcore_mode", false);
        
        // Read alert position
        std::string posStr = j.value("ra_alert_position", "top_right");
        if (posStr == "top_left") m_raAlertPosition = RAAlertPosition::TopLeft;
        else if (posStr == "top_right") m_raAlertPosition = RAAlertPosition::TopRight;
        else if (posStr == "bottom_left") m_raAlertPosition = RAAlertPosition::BottomLeft;
        else if (posStr == "bottom_right") m_raAlertPosition = RAAlertPosition::BottomRight;
        
        tico_debug_log("RA: Config loaded (Enabled: %d, User: %s, HasToken: %d, HasPassword: %d)",
            m_raEnabled, m_raUsername.c_str(), !m_raToken.empty(), !m_raPassword.empty());
    } else {
        tico_debug_log("WARN: Failed to parse accounts.jsonc for RA settings.");
    }
}

void TicoCore::SaveRAToken(const std::string& token)
{
    std::string accountsPath = "sdmc:/tico/config/accounts.jsonc";
    nlohmann::json j = nlohmann::json::object();
    
    // Read existing config
    std::ifstream inFile(accountsPath);
    if (inFile.is_open()) {
        auto parsed = nlohmann::json::parse(inFile, nullptr, false, true);
        inFile.close();
        if (!parsed.is_discarded()) j = parsed;
    }
    
    // Update token
    j["ra_token"] = token;
    m_raToken = token;
    
    // Write back
    std::ofstream outFile(accountsPath);
    if (outFile.is_open()) {
        outFile << j.dump(4);
        outFile.close();
        tico_debug_log("RA: Token saved to accounts.jsonc");
    } else {
        tico_debug_log("RA: WARNING - Failed to save token to %s", accountsPath.c_str());
    }
}

void TicoCore::RAIdentifyGame(rc_client_t* c, TicoCore* core)
{
    const uint32_t console_id = TicoConfig::GetRcConsoleId();

    tico_debug_log("RA: Identifying game... (Console ID: %u)", console_id);
    // hashed from the disc image (its executable), read through rcheevos' cd reader
    rc_client_begin_identify_and_load_game(c, console_id, core->m_gamePath.c_str(), nullptr, 0,
        [](int result, const char* error_message, rc_client_t* client, void* userdata) {
            TicoCore* core = (TicoCore*)userdata;
            if (result == RC_OK) {
                tico_debug_log("RA: Game loaded and identified!");
                const rc_client_game_t* game = rc_client_get_game_info(client);
                if (game && game->title) {
                    core->PushRANotification("RetroAchievements",
                        std::string("Playing: ") + game->title, "ra_icon");
                }
                // Preload all achievement badges in the background
                core->PreloadRABadges();
            } else {
                tico_debug_log("RA: Failed to identify game: %s", error_message ? error_message : "Unknown");
                core->PushRANotification("RetroAchievements", 
                    "Rom hash doesn't match or unable to recognize the game, achievements disabled.", "ra_icon");
            }
        }, core);
}

void TicoCore::RALoginWithPassword(rc_client_t* c, TicoCore* core)
{
    if (core->m_raPassword.empty()) {
        tico_debug_log("RA: No password configured. Continuing without RA.");
        return;
    }
    
    tico_debug_log("RA: Logging in with password...");
    rc_client_begin_login_with_password(c, core->m_raUsername.c_str(), core->m_raPassword.c_str(),
        [](int res, const char* err, rc_client_t* c, void* ud) {
            TicoCore* core = (TicoCore*)ud;
            if (res == RC_OK) {
                const rc_client_user_t* user = rc_client_get_user_info(c);
                if (user && user->token) {
                    tico_debug_log("RA: Password login successful! Saving token...");
                    core->SaveRAToken(user->token);
                } else {
                    tico_debug_log("RA: Password login OK but no token returned");
                }
                RAIdentifyGame(c, core);
            } else {
                tico_debug_log("RA: Password login failed: %s. Continuing without RA.", err ? err : "Unknown");
                core->PushRANotification("RetroAchievements", 
                    "Failed to authenticate, check your username/password and try again.", "ra_icon");
            }
        }, core);
}

void TicoCore::PushRANotification(const std::string& title, const std::string& desc,
                                   const std::string& badge)
{
    RANotification n;
    n.title = title;
    n.description = desc;
    n.badge_name = badge;
    n.timer = 0.0f;
    
    // Look up badge texture
    if (badge == "ra_icon") {
        n.textureId = m_raIconTexture;
    } else if (!badge.empty()) {
        n.textureId = GetRABadgeTexture(badge);
    }
    
    // Cap at 5 visible notifications
    if (m_raNotifications.size() >= 5) {
        m_raNotifications.erase(m_raNotifications.begin());
    }
    m_raNotifications.push_back(std::move(n));
    tico_debug_log("RA: Notification pushed: %s - %s (badge: %s)",
        title.c_str(), desc.c_str(), badge.c_str());
}

ImTextureID TicoCore::GetRABadgeTexture(const std::string& badge_name)
{
    // Check cache first
    auto it = m_raBadgeCache.find(badge_name);
    if (it != m_raBadgeCache.end()) return it->second;
    
    // Try loading from SD card cache
    std::string path = "sdmc:/tico/assets/ra/" + badge_name + ".png";
    int w, h, ch;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (data) {
        ImTextureID tex = TicoVulkan::CreateTextureRGBA(data, w, h);
        stbi_image_free(data);
        m_raBadgeCache[badge_name] = tex;
        return tex;
    }
    return ImTextureID_Invalid;
}

void TicoCore::DownloadAndCacheBadge(const std::string& badge_name)
{
    // Check if already cached on disk
    std::string cachePath = "sdmc:/tico/assets/ra/" + badge_name + ".png";
    FILE* check = fopen(cachePath.c_str(), "rb");
    if (check) { fclose(check); return; } // already on disk
    
    // Download from RA
    std::string url = "https://media.retroachievements.org/Badge/" + badge_name + ".png";
    std::string response;
    
    CURL* curl = curl_easy_init();
    if (!curl) return;
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, RAUserAgent());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    
    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK || httpCode != 200 || response.empty()) {
        tico_debug_log("RA: Failed to download badge %s (http %ld)", badge_name.c_str(), httpCode);
        return;
    }
    
    // Ensure directory exists
    mkdir("sdmc:/tico/assets/ra", 0777);
    
    // Save to disk
    FILE* fp = fopen(cachePath.c_str(), "wb");
    if (fp) {
        fwrite(response.data(), 1, response.size(), fp);
        fclose(fp);
        tico_debug_log("RA: Cached badge %s (%zu bytes)", badge_name.c_str(), response.size());
    }
}

void TicoCore::PreloadRABadges()
{
    if (!m_rcClient) return;
    
    tico_debug_log("RA: Preloading achievement badges...");
    
    // Get all achievement lists
    rc_client_achievement_list_t* list = rc_client_create_achievement_list(m_rcClient,
        RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
        RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
    if (!list) {
        tico_debug_log("RA: No achievement list to preload");
        return;
    }
    
    // Collect all unique badge names
    std::vector<std::string> badges;
    for (uint32_t b = 0; b < list->num_buckets; b++) {
        for (uint32_t a = 0; a < list->buckets[b].num_achievements; a++) {
            const rc_client_achievement_t* ach = list->buckets[b].achievements[a];
            if (ach && ach->badge_name[0]) {
                std::string bn = ach->badge_name;
                // Check if not already cached on disk
                std::string path = "sdmc:/tico/assets/ra/" + bn + ".png";
                FILE* check = fopen(path.c_str(), "rb");
                if (check) { fclose(check); continue; }
                badges.push_back(bn);
            }
        }
    }
    rc_client_destroy_achievement_list(list);
    
    if (badges.empty()) {
        tico_debug_log("RA: All badges already cached");
        return;
    }
    
    tico_debug_log("RA: Need to download %zu badges", badges.size());
    
    // Queue downloads on the RA worker thread (via a regular job mechanism)
    // We'll use a simple approach: spawn them as background tasks
    for (const auto& badge : badges) {
        // Push to job queue so worker thread does the download
        std::string badgeCopy = badge;
        {
            std::lock_guard<std::mutex> lock(m_raJobMutex);
            // We abuse the job queue: url = badge download URL, post_data = badge_name,
            // callback = nullptr (special marker for badge download)
            RAJob job;
            job.url = "__badge__";
            job.post_data = badgeCopy;
            job.callback = nullptr;
            job.callback_data = nullptr;
            m_raJobQueue.push_back(std::move(job));
        }
        m_raJobCond.notify_one();
    }
}

void TicoCore::LoadRAIcon()
{
    // Try loading ra.svg - but nanosvg is only in the overlay.
    // Instead, try loading a cached PNG version, or just skip if not available.
    // The SVG will be loaded by the overlay, which has nanosvg.
    tico_debug_log("RA: LoadRAIcon called (will be loaded by overlay)");
}

void TicoCore::ProcessPendingBadgeUploads()
{
    std::vector<std::pair<std::string, std::vector<unsigned char>>> uploads;
    {
        std::lock_guard<std::mutex> lock(m_raBadgeUploadMutex);
        if (m_raPendingBadgeUploads.empty()) return;
        uploads = std::move(m_raPendingBadgeUploads);
    }
    
    for (auto& [name, data] : uploads) {
        int w, h, ch;
        unsigned char* pixels = stbi_load_from_memory(data.data(), (int)data.size(), &w, &h, &ch, 4);
        if (pixels) {
            m_raBadgeCache[name] = TicoVulkan::CreateTextureRGBA(pixels, w, h);
            stbi_image_free(pixels);
        }
    }
}
