#pragma once
#include "xr_vk.h"
#include <cstdint>

// ── Minimal Vulkan helpers ────────────────────────────────────────────────────
//
// The OpenXR build uses native quad composition layers, so Vulkan only needs to:
// upload a few RGBA textures and blit them into the runtime's swapchain images.
// No render pass, pipeline, or shaders. The VkInstance/device themselves are
// created by the OpenXR XR_KHR_vulkan_enable2 path (see xr_app); this struct
// holds the handles plus one reusable command buffer that batches all of a
// frame's uploads and blits into a single submit.

struct VkContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    bool init_pool();  // after device + queue are populated
    // Begin recording the frame's command buffer (waits for the previous one).
    // Returns VK_NULL_HANDLE if the GPU is hung or lost.
    VkCommandBuffer begin();
    // Submit and wait for completion. Returns false on device loss etc.
    bool submit_wait();
    void destroy();
};

// An RGBA8 (SRGB) texture kept in TRANSFER_SRC layout, blit-able into swapchains.
// The persistent staging buffer is reused for every re-upload.
struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    void* staging_ptr = nullptr;
    uint32_t w = 0, h = 0;

    bool create(VkContext& c, uint32_t w, uint32_t h);
    // Copy a sw x sh straight-alpha RGBA image into staging, multiplying its
    // alpha by `alpha` (layer opacity without needing a runtime extension).
    void stage(const uint8_t* rgba, uint32_t sw, uint32_t sh, float alpha = 1.0f);
    // Record staging → image for the top-left sw x sh region.
    void record_upload(VkCommandBuffer cmd, uint32_t sw, uint32_t sh);
    void destroy(VkContext& c);
};

// Record a blit of src's top-left (sw, sh) region into dst's (dw, dh) region,
// leaving dst in COLOR_ATTACHMENT_OPTIMAL as the runtime expects at release.
// When clear_dst is set the rest of dst is cleared to transparent first, so a
// sub-rectangle quad never samples stale texels at its border.
void record_blit(VkCommandBuffer cmd, const Texture& src, uint32_t sw, uint32_t sh, VkImage dst, uint32_t dw,
                 uint32_t dh, bool clear_dst = false);
