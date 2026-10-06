/// @file TicoGL.h
/// @brief The frontend on OpenGL, for a core that renders with OpenGL.
///
/// An EGL window surface on the native window with a desktop OpenGL 3.3 core
/// context (Mesa's nvc0, or Zink over NVK, picked before EGL starts), ImGui's
/// OpenGL 3 backend, and overlay textures as GL textures. TicoVulkan forwards
/// its frame and texture calls here once UseOpenGL() was called, so the rest
/// of the frontend does not change. The core renders in the same context, on
/// the same thread.
#pragma once

#include "imgui.h"

#include <cstdint>
#include <vector>

namespace TicoGL
{

/// Bring up EGL and ImGui's OpenGL backend. `zink` runs OpenGL on NVK.
bool Init(uint32_t width, uint32_t height, bool zink);
void Shutdown();

/// The default framebuffer, cleared, for the next frame.
bool BeginFrame();
/// Draw ImGui and present.
void EndFrame(ImDrawData *drawData);

void Resize(uint32_t width, uint32_t height);
void GetSwapExtent(uint32_t &width, uint32_t &height);
void SetVsync(bool enabled);
void WaitIdle();

/// RGBA8 texture for the overlay. 0 on failure.
ImTextureID CreateTextureRGBA(const unsigned char *rgba, int width, int height);
void DestroyTexture(ImTextureID tex);

/// A GL texture's pixels as RGBA8, top row first (GL keeps them bottom up).
bool ReadTexture(uint32_t texture, uint32_t width, uint32_t height, std::vector<uint8_t> &rgba);

/// How OpenGL resolves entry points (the core's glad loads through it).
void *GetProcAddress(const char *name);

} // namespace TicoGL
