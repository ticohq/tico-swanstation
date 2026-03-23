/// @file TicoMain.cpp
/// @brief Entry point for tico-integrated swanstation NRO
/// Sets up SDL/EGL/ImGui and runs the main loop

#include "TicoCore.h"
#include "TicoOverlay.h"
#include "TicoConfig.h"
#include "TicoAudio.h"
#include "TicoTranslationManager.h"

#include <SDL.h>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "TicoUtils.h" // Exception-free title formatting
#include "TicoLogger.h"
#include <sys/stat.h>

#ifdef __SWITCH__
#include <switch.h>
#include <switch/runtime/env.h>
#include "glad.h"
#include <EGL/egl.h>
#endif

// ImGui includes
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"

//==============================================================================
// NX System Configuration (extern "C")
//==============================================================================

extern "C" {
u32 __NvOptimusEnablement = 1;
u32 __NvDeveloperOption = 1;
u32 __nx_applet_type = AppletType_Application;
size_t __nx_heap_size = 0;
}


// #ifdef __SWITCH__
// // Custom Exception Handler for swanstation JIT (forward declared from libnx_vmem.cpp)
// extern "C" {
//     // swanstation's handler
//     void swanstation_nx_exception_handler(ThreadExceptionDump *ctx);

//     // Override libnx's weak symbol
//     void __libnx_exception_handler(ThreadExceptionDump *ctx) {
//         swanstation_nx_exception_handler(ctx);
//     }
// }
// #endif

//==============================================================================
// Debug Logging (file-based for Switch since stdout doesn't work)
//==============================================================================

// Debug logging via TicoLogger.h

//==============================================================================
// Globals
//==============================================================================

static SDL_Window *g_window = nullptr;
#ifndef __SWITCH__
static SDL_GLContext g_glContext = nullptr;
#endif
static EGLDisplay g_eglDisplay = EGL_NO_DISPLAY;
static EGLContext g_eglContext = EGL_NO_CONTEXT;
static EGLSurface g_eglSurface = EGL_NO_SURFACE;

static std::unique_ptr<TicoCore> g_core;
static std::unique_ptr<TicoOverlay> g_overlay;

static bool g_running = true;
static TicoAudio g_audio;
static SDL_AudioDeviceID g_audioDevice = 0;

#ifdef __SWITCH__
static u8 g_lastOperationMode = 255; // Invalid initial value to force first update

/// Update nwindow crop and screen dimensions on dock/undock.
/// The nwindow is always 1920×1080; crop selects the visible sub-region.
/// Returns true if model actually changed.
static bool UpdateScreenMode()
{
    u8 operationMode = appletGetOperationMode();
    if (operationMode == g_lastOperationMode)
        return false;

    if (operationMode == AppletOperationMode_Handheld)
    {
        // Handheld: crop 1080p surface to 720p bottom-left.
        // OpenGL origin is bottom-left (Y=0), so in top-left crop coords:
        // top = 1080 - 720 = 360, bottom = 1080.
        nwindowSetCrop(nwindowGetDefault(), 0, 360, 1280, 1080);
        LOG_INFO("DISPLAY", "Mode → Handheld (1280×720 crop)");
        if (ImGui::GetCurrentContext()) {
            ImGui::GetIO().FontGlobalScale = 1.0f;
        }
    }
    else
    {
        // Docked: full 1080p
        nwindowSetCrop(nwindowGetDefault(), 0, 0, 1920, 1080);
        LOG_INFO("DISPLAY", "Mode → Docked (1920×1080)");
        if (ImGui::GetCurrentContext()) {
            ImGui::GetIO().FontGlobalScale = 1.5f;
        }
    }
    g_lastOperationMode = operationMode;
    return true;
}
#endif

//==============================================================================
// SDL/EGL Initialization
//==============================================================================

// Helper to get correct resolution based on Switch mode
static void GetDisplayResolution(int &w, int &h)
{
#ifdef __SWITCH__
    u8 opMode = appletGetOperationMode();
    if (opMode == AppletOperationMode_Handheld)
    {
        w = 1280;
        h = 720;
    }
    else
    {
        w = 1920;
        h = 1080;
    }
#else
    if (g_window)
        SDL_GetWindowSize(g_window, &w, &h);
    else
    {
        w = 1280;
        h = 720;
    }
#endif
}

bool InitWindow()
{
    LOG_INFO("HOME", "Starting initialization...");

    // Match tico app SDL flags
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER |
                 SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0)
    {
        LOG_ERROR("HOME", "SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    LOG_INFO("HOME", "SDL initialized");

#ifdef __SWITCH__
    // On Switch, do NOT create an SDL window - it interferes with nwindowGetDefault()
    // EGL is managed using the native window directly (matching tico PlatformSwitch.cpp)
    g_window = nullptr;
    LOG_INFO("HOME", "Switch: skipping SDL window (using native window)");

    // nwindow is already 1920×1080 (set in main() pre-init).
    // Apply initial crop for current mode (handheld crops to 1280×720).
    UpdateScreenMode();
    int w, h;
    GetDisplayResolution(w, h);
    LOG_INFO("HOME", "Switch Resolution: %dx%d (logical)", w, h);

    // Initialize EGL
    g_eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_eglDisplay == EGL_NO_DISPLAY)
    {
        LOG_ERROR("EGL", "eglGetDisplay failed");
        return false;
    }
    LOG_INFO("EGL", "EGL display obtained");

    EGLint major, minor;
    if (!eglInitialize(g_eglDisplay, &major, &minor))
    {
        LOG_ERROR("EGL", "eglInitialize failed");
        return false;
    }
    LOG_INFO("EGL", "EGL %d.%d initialized", major, minor);

    // Choose config
    EGLConfig config;
    EGLint numConfigs;
    const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE};

    if (!eglChooseConfig(g_eglDisplay, configAttribs, &config, 1, &numConfigs))
    {
        LOG_ERROR("EGL", "eglChooseConfig failed");
        return false;
    }
    LOG_INFO("EGL", "EGL config chosen (numConfigs=%d)", numConfigs);

    // Create surface
    g_eglSurface = eglCreateWindowSurface(g_eglDisplay, config,
                                          nwindowGetDefault(), NULL);
    if (g_eglSurface == EGL_NO_SURFACE)
    {
        LOG_ERROR("EGL", "eglCreateWindowSurface failed");
        return false;
    }
    LOG_INFO("EGL", "EGL surface created");

    // Create context (OpenGL 4.3)
    eglBindAPI(EGL_OPENGL_API);
    const EGLint contextAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 4,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE};

    g_eglContext = eglCreateContext(g_eglDisplay, config, EGL_NO_CONTEXT, contextAttribs);
    if (g_eglContext == EGL_NO_CONTEXT)
    {
        LOG_ERROR("EGL", "eglCreateContext failed");
        return false;
    }
    LOG_INFO("EGL", "EGL context created");

    if (!eglMakeCurrent(g_eglDisplay, g_eglSurface, g_eglSurface, g_eglContext))
    {
        LOG_ERROR("EGL", "eglMakeCurrent failed");
        return false;
    }
    LOG_INFO("EGL", "EGL context made current");

    // Load OpenGL functions with GLAD
    if (!gladLoadGLLoader((GLADloadproc)eglGetProcAddress))
    {
        LOG_ERROR("HOME", "gladLoadGLLoader failed");
        return false;
    }

    // CRITICAL: Enable VSync to limit frame rate to display refresh
    // Without this, the game runs as fast as possible, causing audio desync
    eglSwapInterval(g_eglDisplay, 1);
    LOG_INFO("EGL", "VSync enabled (eglSwapInterval=1)");

    LOG_INFO("HOME", "OpenGL %s initialized", glGetString(GL_VERSION));

#else
    // Desktop: Use SDL OpenGL context
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    g_window = SDL_CreateWindow("swanstation",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                TicoConfig::WINDOW_WIDTH, TicoConfig::WINDOW_HEIGHT,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);

    if (!g_window)
    {
        LOG_ERROR("HOME", "SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    g_glContext = SDL_GL_CreateContext(g_window);
    if (!g_glContext)
    {
        LOG_ERROR("HOME", "SDL_GL_CreateContext failed: %s", SDL_GetError());
        return false;
    }

    SDL_GL_MakeCurrent(g_window, g_glContext);
    SDL_GL_SetSwapInterval(1);

    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress))
    {
        LOG_ERROR("HOME", "gladLoadGLLoader failed");
        return false;
    }

    LOG_INFO("HOME", "OpenGL %s initialized", glGetString(GL_VERSION));
#endif

    // Initialize SDL_mixer for callback-based audio
    // Initialize Audio Backend
    if (TicoConfig::USE_SDLQUEUEAUDIO)
    {
        // Queue-based audio (Push model)
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = TicoAudio::SAMPLE_RATE;
        want.format = AUDIO_S16SYS;
        want.channels = TicoAudio::CHANNELS;
        want.samples = 2048;
        want.callback = NULL; // Push mode requires NULL callback

        g_audioDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (g_audioDevice == 0)
        {
            LOG_ERROR("AUDIO", "SDL_OpenAudioDevice failed: %s", SDL_GetError());
        }
        else
        {
            LOG_INFO("AUDIO", "SDL_QueueAudio initialized. DeviceID: %d, Freq: %d", g_audioDevice, have.freq);
        }
    }
    else
    {
        // Callback-based audio (Pull model via SDL_mixer)
        if (Mix_OpenAudio(44100, AUDIO_S16SYS, 2, 1024) < 0)
        {
            LOG_ERROR("AUDIO", "Mix_OpenAudio failed: %s", Mix_GetError());
        }
        else
        {
            LOG_INFO("AUDIO", "SDL_mixer initialized");
        }
    }

    return true;
}

// Audio Callbacks using TicoAudio system
static void AudioSampleCallback(int16_t left, int16_t right)
{
    g_audio.PushSample(left, right);
}

static size_t AudioSampleBatchCallback(const int16_t *data, size_t frames)
{
    return g_audio.PushSamples(data, frames);
}

static void AudioFlushCallback()
{
    g_audio.Flush();
    LOG_INFO("AUDIO", "Audio flushed");
}

bool InitImGui()
{
    LOG_INFO("HOME", "InitImGui starting...");

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr; // Prevent generation of imgui.ini
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    LOG_INFO("HOME", "ImGui context created");

    // Setup Platform/Renderer backends
#ifdef __SWITCH__
    // On Switch with EGL, pass nullptr for context
    ImGui_ImplSDL2_InitForOpenGL(g_window, nullptr);
    ImGui_ImplOpenGL3_Init("#version 430 core");
#else
    ImGui_ImplSDL2_InitForOpenGL(g_window, g_glContext);
    ImGui_ImplOpenGL3_Init("#version 330 core");
#endif
    LOG_INFO("HOME", "ImGui backends initialized");

    // Load font
#ifdef __SWITCH__
    io.Fonts->AddFontFromFileTTF(TicoConfig::FONT_PATH, TicoConfig::FONT_SIZE);
    // Load secondary font for RA alert descriptions
    io.Fonts->AddFontFromFileTTF("romfs:/fonts/description.ttf", TicoConfig::FONT_SIZE * 0.75f);
#else
    io.Fonts->AddFontFromFileTTF("assets/fonts/font.ttf", TicoConfig::FONT_SIZE);
    // Load secondary font for RA alert descriptions
    io.Fonts->AddFontFromFileTTF("assets/fonts/description.ttf", TicoConfig::FONT_SIZE * 0.75f);
#endif

    LOG_INFO("HOME", "ImGui initialized");
    return true;
}

void CleanupWindow()
{
    // CRITICAL: Ensure all GPU commands complete before teardown
    // to prevent driver hangs during cleanup
    glFinish();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

#ifdef __SWITCH__
    if (g_eglContext != EGL_NO_CONTEXT)
    {
        eglMakeCurrent(g_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(g_eglDisplay, g_eglContext);
    }
    if (g_eglSurface != EGL_NO_SURFACE)
    {
        eglDestroySurface(g_eglDisplay, g_eglSurface);
    }
    if (g_eglDisplay != EGL_NO_DISPLAY)
    {
        eglTerminate(g_eglDisplay);
    }

    // Chainloaded NROs inherit the main thread's TLS — explicitly drop
    // any remaining per-thread EGL state after the display is gone,
    // so the next NRO's eglInitialize() starts clean.
    eglReleaseThread();
#else
    if (g_glContext)
    {
        SDL_GL_DeleteContext(g_glContext);
    }
#endif

    if (g_window)
    {
        SDL_DestroyWindow(g_window);
    }

    SDL_Quit();
}

//==============================================================================
// Main Loop
//==============================================================================

void ProcessEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        ImGui_ImplSDL2_ProcessEvent(&event);

        if (event.type == SDL_QUIT)
        {
            g_running = false;
        }

#ifdef __SWITCH__
        // Handle applet exit
        if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == SDL_SCANCODE_ESCAPE)
        {
            g_running = false;
        }
#endif
    }
}

void HandleInput()
{
    // Support up to 2 controllers
    SDL_GameController *controllers[2] = {nullptr, nullptr};
    int numControllers = 0;
    
    for (int i = 0; i < SDL_NumJoysticks() && numControllers < 2; i++)
    {
        if (SDL_IsGameController(i))
        {
            controllers[numControllers] = SDL_GameControllerOpen(i);
            numControllers++;
        }
    }

    // Tico Overlay Mode
    // Let overlay consume input if visible (using player 1)
    if (g_overlay && numControllers > 0 && g_overlay->HandleInput(controllers[0]))
    {
        // Check for exit request
        if (g_overlay->ShouldExit())
        {
            LOG_INFO("HOME", "ShouldExit detected! g_running will be false.");
#ifdef __SWITCH__
            const char *primaryNro = "sdmc:/switch/tico.nro";
            const char *fallbackNro = "sdmc:/switch/tico/tico.nro";
            const char *targetNro = nullptr;

            // Check if primaryNro exists, else check fallbackNro
            struct stat buffer;
            if (stat(primaryNro, &buffer) == 0)
            {
                targetNro = primaryNro;
            }
            else if (stat(fallbackNro, &buffer) == 0)
            {
                targetNro = fallbackNro;
            }

            if (targetNro != nullptr)
            {
                // Build args as space-separated string (per libnx envSetNextLoad docs)
                // Format: "nro_path --resume"
                char args[512];
                snprintf(args, sizeof(args), "%s --resume", targetNro);

                envSetNextLoad(targetNro, args);
                LOG_INFO("HOME", "Chainloading back to %s with args: %s", targetNro, args);
            }
            else
            {
                LOG_WARN("HOME", "Chainload target not found! Exiting normally.");
            }

            // Clean up imgui.ini to avoid clutter/persistence issues
            remove("imgui.ini");
            LOG_INFO("HOME", "Deleted imgui.ini");
#endif
            g_running = false;
        }
        // Check for reset request
        if (g_overlay->ShouldReset())
        {
            g_overlay->ClearReset();
            if (g_core)
            {
                g_core->Reset();
            }
        }
        return;
    }

    // Pass input to core when overlay not visible
    if (g_core)
    {
        g_core->ClearInputs();

        for (int p = 0; p < numControllers; p++)
        {
            SDL_GameController *controller = controllers[p];
            if (!controller) continue;

            // Map PlayStation from Switch layout
            // Physical Right -> PSX Circle -> SDL_B (Right in Xbox layout)
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_A,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_B));
            // Physical Down -> PSX Cross -> SDL_A (Down in Xbox layout)
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_B,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_A));
            // Physical Top -> PSX Triangle -> SDL_Y (Top in Xbox layout)
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_X,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_Y));
            // Physical Left -> PSX Square -> SDL_X (Left in Xbox layout)
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_Y,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_X));
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_START,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_START));
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_SELECT,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_BACK));
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_UP,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_UP));
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_DOWN,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN));
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_LEFT,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT));
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_RIGHT,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT));
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_L,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER));
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_R,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER));
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_L2,
                                  SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000);
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_R2,
                                  SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000);
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_L3,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_LEFTSTICK));
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_R3,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_RIGHTSTICK));

            // Analog sticks
            g_core->SetAnalogState(p, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X,
                                   SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX));
            g_core->SetAnalogState(p, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y,
                                   SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY));
            g_core->SetAnalogState(p, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X,
                                   SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX));
            g_core->SetAnalogState(p, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y,
                                   SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY));
        }
    }
}

void Render()
{
    static int frameCount = 0;
    frameCount++;

    // Log first few frames for debugging
    if (frameCount <= 3)
    {
        LOG_DEBUG("RENDER", "Frame %d: Render starting", frameCount);
    }

    // Start ImGui frame
    ImGui_ImplOpenGL3_NewFrame();

#ifdef __SWITCH__
    // Detect dock/undock and update crop dynamically
    UpdateScreenMode();

    // Set ImGui display size based on logical resolution
    ImGuiIO &io = ImGui::GetIO();
    int logW, logH;
    GetDisplayResolution(logW, logH);
    io.DisplaySize = ImVec2((float)logW, (float)logH);
    io.DeltaTime = 1.0f / 60.0f;
#else
    ImGui_ImplSDL2_NewFrame();
#endif
    ImGui::NewFrame();

    int w, h;
    GetDisplayResolution(w, h);
    ImVec2 displaySize((float)w, (float)h);

    // Run core frame - but check for crashes
    if (g_core)
    {
        bool overlayVisible = g_overlay && g_overlay->IsVisible();

        // Only skip runframe if overlay IS visible AND we are using Tico
        // If not using Tico, overlayVisible is false, so we always RunFrame.
        if (!overlayVisible)
        {
            if (frameCount <= 3)
            {
                LOG_DEBUG("RENDER", "Frame %d: Calling RunFrame", frameCount);
            }
            g_core->RunFrame();
            if (frameCount <= 3)
            {
                LOG_DEBUG("RENDER", "Frame %d: RunFrame returned", frameCount);
            }
        }
    }

    // Render
    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Render overlay via ImGui
    if (g_overlay)
    {
        unsigned int tex = g_core ? g_core->GetFrameTextureID() : 0;
        float ar = g_core ? g_core->GetAspectRatio() : 4.0f / 3.0f;
        int fw = g_core ? g_core->GetFrameWidth() : 640;
        int fh = g_core ? g_core->GetFrameHeight() : 480;
        int fboW = g_core ? g_core->GetFBOWidth() : 0;
        int fboH = g_core ? g_core->GetFBOHeight() : 0;

        g_overlay->Render(displaySize, tex, ar, fw, fh, fboW, fboH);
    }
    
    // Draw OSD Message Pill
    if (g_core && g_core->GetOSDFrames() > 0)
    {
        ImDrawList *fg = ImGui::GetForegroundDrawList();
        const float marginX = 24.0f;
        const float marginY = 16.0f;
        const float padX = 16.0f;
        const float padY = 8.0f;
        const float rounding = 14.0f;
        
        int frames = g_core->GetOSDFrames();
        float alpha = 1.0f;
        if (frames < 30) alpha = frames / 30.0f;
        
        std::string msg = g_core->GetOSDMessage();
        ImVec2 textSize = ImGui::CalcTextSize(msg.c_str());
        
        float screenW = ImGui::GetIO().DisplaySize.x;
        
        float pillW = textSize.x + padX * 2;
        float pillH = textSize.y + padY * 2;
        float pillX = marginX; // Default to P1 / general message (Left)
        
        if (msg.find("Controller 2") != std::string::npos || (msg.find("#2") != std::string::npos))
        {
            pillX = screenW - pillW - marginX; // P2 (Right)
        }
        
        float pillY = marginY;
        
        ImU32 bgCol = IM_COL32(0, 0, 0, (int)(alpha * 153));
        fg->AddRectFilled(ImVec2(pillX, pillY), ImVec2(pillX + pillW, pillY + pillH), bgCol, rounding);
        
        ImU32 textCol = IM_COL32(255, 255, 255, (int)(alpha * 240));
        fg->AddText(ImVec2(pillX + padX, pillY + padY), textCol, msg.c_str());
        
        g_core->DecrementOSD();
    }

    // Render ImGui
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

#ifdef __SWITCH__
    eglSwapBuffers(g_eglDisplay, g_eglSurface);
#else
    SDL_GL_SwapWindow(g_window);
#endif
}

//==============================================================================
// Main
//==============================================================================

int main(int argc, char *argv[])
{
#ifdef __SWITCH__
    // CRITICAL: Exact initialization order from tico app
    // This order is REQUIRED for EGL to work on Switch!

    // 1. Lock exit first
    appletLockExit();

    // 2. Socket initialization removed to prevent up to 10s timeout hang without wifi
    socketInitializeDefault();

    // 3. RomFS initialization
    romfsInit();

    // 4. Set window dimensions BEFORE any EGL/SDL
    // Always 1920x1080 — Switch compositor downscales for handheld automatically.
    nwindowSetDimensions(nwindowGetDefault(), 1920, 1080);

    LOG_INFO("HOME", "Switch pre-init complete (socket, romfs, nwindow)");
#endif

    LOG_INFO("HOME", "swanstation starting...");

    // Initialize Translations
    TicoTranslationManager::Instance().Init();

    // Initialize window and graphics
    LOG_INFO("HOME", "Calling InitWindow...");
    if (!InitWindow())
    {
        LOG_ERROR("HOME", "Failed to initialize window");
        return 1;
    }
    LOG_INFO("HOME", "InitWindow succeeded");

    // Paint the hardware screen black immediately to hide any OS/loader random VRAM artifacts
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
#ifdef __SWITCH__
    eglSwapBuffers(g_eglDisplay, g_eglSurface);
#else
    SDL_GL_SwapWindow(g_window);
#endif

    // Initialize ImGui
    LOG_INFO("HOME", "Calling InitImGui...");
    if (!InitImGui())
    {
        LOG_ERROR("HOME", "Failed to initialize ImGui");
        CleanupWindow();
        return 1;
    }
    LOG_INFO("HOME", "InitImGui succeeded");
#ifdef __SWITCH__
    g_lastOperationMode = 255; // Force screen mode update on first loop frame to apply FontGlobalScale
#endif

    // Create core and overlay
    LOG_INFO("HOME", "Creating core...");
    g_core = std::make_unique<TicoCore>();

    LOG_INFO("HOME", "Creating overlay...");
    g_overlay = std::make_unique<TicoOverlay>();
    g_overlay->SetCore(g_core.get());

    // Set audio callbacks
    g_core->SetAudioCallbacks(AudioSampleCallback, AudioSampleBatchCallback, AudioFlushCallback);

    // Initialize callback-based audio system
    if (!g_audio.Init(g_audioDevice))
    {
        LOG_WARN("HOME", "TicoAudio init failed");
    }

    LOG_INFO("HOME", "Core and overlay created");

    // Determine ROM path from arguments
    std::string romPath = TicoConfig::TEST_ROM;
    bool romArgFound = false;

    if (argc > 1)
    {
        // Assume the first argument is the ROM path
        romPath = argv[1];
        romArgFound = true;
        LOG_INFO("HOME", "ROM path provided via argv: %s", romPath.c_str());
    }

    if (!romArgFound)
    {
        LOG_INFO("HOME", "No ROM argument provided. Using default: %s", romPath.c_str());
    }

    // Determine Game Title
    std::string gameTitle;
    // Extract clean title from filename
    size_t lastSlash = romPath.find_last_of("/\\");
    std::string filename = (lastSlash != std::string::npos) ? romPath.substr(lastSlash + 1) : romPath;
    size_t lastDot = filename.find_last_of(".");
    if (lastDot != std::string::npos)
    {
        gameTitle = filename.substr(0, lastDot);
    }
    else
    {
        gameTitle = filename;
    }

    // Use TicoUtils for title cleaning (exception-free)
    std::string cleanTitle = TicoUtils::GetCleanTitle(filename);
    if (cleanTitle.empty())
        cleanTitle = filename; // Fallback

    g_overlay->SetGameTitle(cleanTitle);

    // Load ROM
    LOG_INFO("HOME", "Loading ROM: %s", romPath.c_str());
    if (!g_core->LoadGame(romPath))
    {
        LOG_ERROR("HOME", "Failed to load ROM");
        // Don't exit - show error in overlay or let user see what happened
    }

    // Main loop
    Uint32 lastTime = SDL_GetTicks();

    while (g_running)
    {
#ifdef __SWITCH__
        // Check applet exit
        if (!appletMainLoop())
        {
            g_running = false;
            break;
        }
#endif

        // Calculate delta time
        Uint32 currentTime = SDL_GetTicks();
        float deltaTime = (currentTime - lastTime) / 1000.0f;
        lastTime = currentTime;

        // Update overlay animation
        if (g_overlay)
        {
            g_overlay->Update(deltaTime);
        }

        ProcessEvents();
        HandleInput();
        Render();
    }

    // Cleanup
    LOG_INFO("HOME", "Starting cleanup...");
    g_overlay.reset(); // Safe even if null
    g_core.reset();

    // Shutdown audio before CleanupWindow
    g_audio.Shutdown();
    // Shutdown SDL_mixer if used
    if (!TicoConfig::USE_SDLQUEUEAUDIO)
    {
        Mix_CloseAudio();
    }
    LOG_INFO("HOME", "Audio shutdown complete");

    CleanupWindow();
    LOG_INFO("HOME", "CleanupWindow done");

#ifdef __SWITCH__
    LOG_INFO("HOME", "Calling romfsExit...");
    romfsExit();
    LOG_INFO("HOME", "Calling socketExit...");
    socketExit();
    LOG_INFO("HOME", "Calling appletUnlockExit...");
    appletUnlockExit();
    LOG_INFO("HOME", "All Switch cleanup done");
#endif

    LOG_INFO("HOME", "Clean exit");
    return 0;
}
