/// @file SwanShaders.h
/// @brief RetroArch slang presets over SwanStation's frames (Vulkan only).
#pragma once

class TicoShaderChain;

namespace SwanShaders
{
/// Creates the shader chain and adds the Shaders category to Settings. Call
/// once the Vulkan device is up and the overlay is initialized.
bool Init();
/// Loads the preset the settings name when it changes (outside a frame).
void Update();
void Shutdown();
/// The chain the game is drawn through; null on OpenGL or before Init.
TicoShaderChain *Chain();
} // namespace SwanShaders
