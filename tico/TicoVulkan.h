/// @file TicoVulkan.h
/// @brief Vulkan device, swapchain and ImGui renderer for the tico frontend.
///
/// On Switch this runs on Mesa's NVK, statically linked: its loaderless
/// libvulkan.a exports the vk* entry points, so they are called directly. On
/// the desktop volk loads them from the system loader. The frame loop is:
///
///   VkCommandBuffer cmd = TicoVulkan::BeginFrame();
///   ... record offscreen work (upload, shader passes) into cmd ...
///   TicoVulkan::EndFrame(ImGui::GetDrawData());   // swapchain pass + present
///
/// Everything the game draws reaches the screen through ImGui: the shader
/// chain renders into an image, which the overlay draws with AddImage.
#pragma once

#ifdef __SWITCH__
#include <vulkan/vulkan.h>
#else
#include <volk.h>
#endif

#include "imgui.h"

#include <cstdint>
#include <functional>

struct SDL_Window;

namespace TicoVulkan
{

/// Frames the CPU may record ahead of the GPU. Per-frame resources (staging
/// buffers, uniform buffers, descriptor sets) are allocated this many times.
constexpr uint32_t kFramesInFlight = 2;

struct Context
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkPhysicalDeviceProperties props = {};
    VkPhysicalDeviceMemoryProperties memProps = {};
};

/// A core that renders with its own Vulkan code shares this device: called
/// with the instance, the chosen GPU and the surface, `create` makes the
/// device (with VK_KHR_swapchain among its extensions) and returns it with
/// its graphics queue; `destroy` takes it back at Shutdown. Set before Init.
struct DeviceFactory
{
    std::function<bool(VkInstance, VkPhysicalDevice, VkSurfaceKHR, VkDevice &, VkQueue &, uint32_t &)> create;
    std::function<void()> destroy;
};
void SetDeviceFactory(DeviceFactory factory);

/// Run the frontend on OpenGL instead (TicoGL), for a core that renders with
/// OpenGL: Init and the frame and texture calls below forward there. Call
/// before Init. BeginFrame then returns a placeholder (never recorded into).
void UseOpenGL(bool zink);
bool IsOpenGL();

/// Bring up instance, surface, device, swapchain and ImGui's Vulkan backend.
/// `window` is the SDL window on the desktop and ignored on Switch. An ImGui
/// context must exist.
bool Init(SDL_Window *window, uint32_t width, uint32_t height);
void Shutdown();

const Context &Ctx();

/// Slot of the frame being recorded, 0..kFramesInFlight-1.
uint32_t FrameIndex();

/// Wait for this slot's previous submission, acquire a swapchain image and
/// begin the frame's command buffer. Returns VK_NULL_HANDLE when the frame
/// has to be skipped (the swapchain is recreated on the next call).
VkCommandBuffer BeginFrame();

/// Draw ImGui into the swapchain image, submit and present.
void EndFrame(ImDrawData *drawData);

/// Desktop debug: save the next presented frame (with ImGui) as a PNG.
void RequestScreenshot(const char *path);

/// Resize the swapchain, e.g. on a handheld/docked switch.
void Resize(uint32_t width, uint32_t height);
void GetSwapExtent(uint32_t &width, uint32_t &height);

/// FIFO (vsync) or IMMEDIATE (uncapped, for fast-forward) presentation.
void SetVsync(bool enabled);

/// Run `fn` once the GPU can no longer be using what it frees.
void DeferDestroy(std::function<void()> fn);

void WaitIdle();

bool FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props, uint32_t &index);

/// Submit one-shot work synchronously (texture uploads outside the frame).
VkCommandBuffer BeginOneShot();
void EndOneShot(VkCommandBuffer cmd);

/// Layout transition over the whole colour image.
void TransitionImage(VkCommandBuffer cmd, VkImage image, uint32_t mipLevels,
                     VkImageLayout oldLayout, VkImageLayout newLayout,
                     VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                     VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);

/// A sampled 2D colour image with memory and view.
struct Image
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

/// `swizzleAlphaOne` forces alpha to 1 in the view, for XRGB/RGB formats.
bool CreateImage(Image &out, uint32_t width, uint32_t height, VkFormat format,
                 VkImageUsageFlags usage, uint32_t mipLevels = 1,
                 bool swizzleAlphaOne = false);
/// Destroy immediately (caller guarantees the GPU is done) ...
void DestroyImage(Image &img);
/// ... or once in-flight frames have finished.
void DeferDestroyImage(Image &img);

/// Host-visible, coherent buffer.
struct Buffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    VkDeviceSize size = 0;
};
bool CreateBuffer(Buffer &out, VkDeviceSize size, VkBufferUsageFlags usage);
void DestroyBuffer(Buffer &buf);
void DeferDestroyBuffer(Buffer &buf);

/// RGBA8 texture for the overlay (icons, badges, avatar). 0 on failure.
ImTextureID CreateTextureRGBA(const unsigned char *rgba, int width, int height);
void DestroyTexture(ImTextureID tex);

/// Expose an image the caller owns to ImGui. It must be in
/// SHADER_READ_ONLY_OPTIMAL whenever ImGui draws it.
ImTextureID RegisterImage(VkImageView view);
void UnregisterImage(ImTextureID tex);

} // namespace TicoVulkan
