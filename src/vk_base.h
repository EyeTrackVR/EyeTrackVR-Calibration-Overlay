#pragma once
#include "xr_vk.h"
#include <cstdint>

// ── Minimal Vulkan helpers ────────────────────────────────────────────────────
//
// The OpenXR build uses native quad composition layers, so Vulkan only needs to:
// upload a couple of RGBA textures and blit them into the runtime's swapchain
// images each frame. No render pass, pipeline, or shaders. The VkInstance/device
// themselves are created by the OpenXR XR_KHR_vulkan_enable2 path (see xr_app);
// this struct just holds the handles and a command pool.

struct VkContext {
    VkInstance       instance      = VK_NULL_HANDLE;
    VkPhysicalDevice phys          = VK_NULL_HANDLE;
    VkDevice         device        = VK_NULL_HANDLE;
    VkQueue          queue         = VK_NULL_HANDLE;
    uint32_t         queue_family  = 0;
    VkCommandPool    cmd_pool      = VK_NULL_HANDLE;

    bool init_pool();   // after device + queue are populated
    void destroy();
};

// An RGBA8 (SRGB) texture kept in TRANSFER_SRC layout, blit-able into swapchains.
struct Texture {
    VkImage        image       = VK_NULL_HANDLE;
    VkDeviceMemory mem         = VK_NULL_HANDLE;
    VkBuffer       staging     = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    void*          staging_ptr = nullptr;
    uint32_t       w = 0, h = 0;

    bool create(VkContext& c, uint32_t w, uint32_t h);
    // Copy rgba (w*h*4 bytes) into the image; leaves it in TRANSFER_SRC_OPTIMAL.
    void upload(VkContext& c, const uint8_t* rgba);
    void destroy(VkContext& c);
};

// Blit `src` (scaled) into a swapchain image, leaving it in
// COLOR_ATTACHMENT_OPTIMAL as the runtime expects at release.
void blit_texture(VkContext& c, const Texture& src,
                  VkImage dst, uint32_t dst_w, uint32_t dst_h);
