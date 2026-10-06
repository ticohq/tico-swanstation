/// @file TicoGL.cpp
/// @brief EGL + desktop OpenGL frontend backend (see TicoGL.h).

#include "TicoGL.h"
#include "TicoLogger.h"

#include <glad.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "imgui_impl_opengl3.h"

#include <algorithm>
#include <cstdlib>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace TicoGL
{
namespace
{
constexpr const char *kTag = "GL";

EGLDisplay s_display = EGL_NO_DISPLAY;
EGLSurface s_surface = EGL_NO_SURFACE;
EGLContext s_context = EGL_NO_CONTEXT;
uint32_t s_width = 1280;
uint32_t s_height = 720;
bool s_imguiReady = false;
} // namespace

void *GetProcAddress(const char *name)
{
    return reinterpret_cast<void *>(eglGetProcAddress(name));
}

bool Init(uint32_t width, uint32_t height, bool zink)
{
    s_width = width;
    s_height = height;

    // Mesa picks the GL driver when the first EGL display is created, and an
    // EGLDisplay keeps it for its lifetime: nvc0 natively, or Zink on NVK.
    setenv("MESA_SWITCH_GL_DRIVER", zink ? "zink" : "nvc0", 1);
    setenv("MESA_LOADER_DRIVER_OVERRIDE", zink ? "zink" : "nouveau", 1);

    s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (s_display == EGL_NO_DISPLAY || !eglInitialize(s_display, nullptr, nullptr))
    {
        LOG_ERROR(kTag, "eglInitialize failed: 0x%x", eglGetError());
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_API))
    {
        LOG_ERROR(kTag, "eglBindAPI(OpenGL) failed: 0x%x", eglGetError());
        return false;
    }

    static const EGLint configAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE};
    EGLConfig config;
    EGLint numConfigs = 0;
    if (!eglChooseConfig(s_display, configAttribs, &config, 1, &numConfigs) || numConfigs == 0)
    {
        LOG_ERROR(kTag, "eglChooseConfig failed: 0x%x", eglGetError());
        return false;
    }

#ifdef __SWITCH__
    s_surface = eglCreateWindowSurface(s_display, config, (EGLNativeWindowType)nwindowGetDefault(), nullptr);
#endif
    if (s_surface == EGL_NO_SURFACE)
    {
        LOG_ERROR(kTag, "eglCreateWindowSurface failed: 0x%x", eglGetError());
        return false;
    }

    // SwanStation's OpenGL renderer wants a 3.3 core context
    static const EGLint contextAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, contextAttribs);
    if (s_context == EGL_NO_CONTEXT || !eglMakeCurrent(s_display, s_surface, s_surface, s_context))
    {
        LOG_ERROR(kTag, "OpenGL 3.3 core context failed: 0x%x", eglGetError());
        return false;
    }
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(GetProcAddress)))
    {
        LOG_ERROR(kTag, "Loading OpenGL entry points failed");
        return false;
    }
    LOG_INFO(kTag, "%s on %s (%s)", reinterpret_cast<const char *>(glGetString(GL_VERSION)),
             reinterpret_cast<const char *>(glGetString(GL_RENDERER)), zink ? "Zink" : "nvc0");

    eglSwapInterval(s_display, 1);
    if (!ImGui_ImplOpenGL3_Init("#version 330 core"))
    {
        LOG_ERROR(kTag, "ImGui_ImplOpenGL3_Init failed");
        return false;
    }
    s_imguiReady = true;
    return true;
}

void Shutdown()
{
    if (s_imguiReady)
    {
        ImGui_ImplOpenGL3_Shutdown();
        s_imguiReady = false;
    }
    if (s_display != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (s_context != EGL_NO_CONTEXT)
            eglDestroyContext(s_display, s_context);
        if (s_surface != EGL_NO_SURFACE)
            eglDestroySurface(s_display, s_surface);
        eglTerminate(s_display);
    }
    s_display = EGL_NO_DISPLAY;
    s_surface = EGL_NO_SURFACE;
    s_context = EGL_NO_CONTEXT;
}

bool BeginFrame()
{
    if (s_context == EGL_NO_CONTEXT)
        return false;
    ImGui_ImplOpenGL3_NewFrame();
    return true;
}

void EndFrame(ImDrawData *drawData)
{
    if (s_context == EGL_NO_CONTEXT)
        return;
    // the core left its own framebuffer bound: draw on the window's
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, static_cast<GLsizei>(s_width), static_cast<GLsizei>(s_height));
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (drawData)
        ImGui_ImplOpenGL3_RenderDrawData(drawData);
    eglSwapBuffers(s_display, s_surface);
}

void Resize(uint32_t width, uint32_t height)
{
    s_width = width;
    s_height = height;
}

void GetSwapExtent(uint32_t &width, uint32_t &height)
{
    width = s_width;
    height = s_height;
}

void SetVsync(bool enabled)
{
    if (s_display != EGL_NO_DISPLAY)
        eglSwapInterval(s_display, enabled ? 1 : 0);
}

void WaitIdle()
{
    if (s_context != EGL_NO_CONTEXT)
        glFinish();
}

ImTextureID CreateTextureRGBA(const unsigned char *rgba, int width, int height)
{
    if (s_context == EGL_NO_CONTEXT || !rgba || width <= 0 || height <= 0)
        return ImTextureID_Invalid;
    GLint previous = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
    return static_cast<ImTextureID>(tex);
}

void DestroyTexture(ImTextureID tex)
{
    if (s_context == EGL_NO_CONTEXT || tex == ImTextureID_Invalid)
        return;
    GLuint id = static_cast<GLuint>(tex);
    glDeleteTextures(1, &id);
}

bool ReadTexture(uint32_t texture, uint32_t width, uint32_t height, std::vector<uint8_t> &rgba)
{
    if (s_context == EGL_NO_CONTEXT || !texture || !width || !height)
        return false;
    GLint previous = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    const bool ok = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    std::vector<uint8_t> pixels;
    if (ok)
    {
        pixels.resize(static_cast<size_t>(width) * height * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE,
                     pixels.data());
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous));
    glDeleteFramebuffers(1, &fbo);
    if (!ok)
        return false;
    const size_t row = static_cast<size_t>(width) * 4;
    rgba.resize(pixels.size());
    for (uint32_t y = 0; y < height; ++y)
        std::copy_n(pixels.data() + (height - 1 - y) * row, row, rgba.data() + y * row);
    return true;
}

} // namespace TicoGL
