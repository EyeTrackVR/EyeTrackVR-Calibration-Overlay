#include "vk_base.h"

#include <algorithm>
#include <cstring>
#include <iostream>

namespace {

bool find_memory_type(VkPhysicalDevice phys, uint32_t type_bits, VkMemoryPropertyFlags props, uint32_t& out) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) {
            out = i;
            return true;
        }
    }
    return false;
}

void image_barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                   VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

constexpr VkFormat kTexFormat = VK_FORMAT_R8G8B8A8_SRGB;

}  // namespace

bool VkContext::init_pool() {
    VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = queue_family;
    if (vkCreateCommandPool(device, &ci, nullptr, &cmd_pool) != VK_SUCCESS) {
        std::cout << "[ERROR] vkCreateCommandPool failed\n";
        return false;
    }
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = cmd_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &ai, &cmd) != VK_SUCCESS) {
        std::cout << "[ERROR] vkAllocateCommandBuffers failed\n";
        return false;
    }
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(device, &fi, nullptr, &fence) != VK_SUCCESS) {
        std::cout << "[ERROR] vkCreateFence failed\n";
        return false;
    }
    return true;
}

VkCommandBuffer VkContext::begin() {
    if (vkWaitForFences(device, 1, &fence, VK_TRUE, 2'000'000'000ull) != VK_SUCCESS) {
        std::cout << "[ERROR] GPU did not finish the previous frame (hung or lost device)\n";
        return VK_NULL_HANDLE;
    }
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

bool VkContext::submit_wait() {
    vkEndCommandBuffer(cmd);
    vkResetFences(device, 1, &fence);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    const VkResult r = vkQueueSubmit(queue, 1, &si, fence);
    if (r != VK_SUCCESS) {
        static int logged = 0;
        if (logged++ < 3) std::cout << "[ERROR] vkQueueSubmit failed (VkResult " << (int)r << ")\n";
        // Leave the fence signalled so the next begin() does not hang.
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkDestroyFence(device, fence, nullptr);
        vkCreateFence(device, &fi, nullptr, &fence);
        return false;
    }
    // The runtime reads the swapchain images after xrReleaseSwapchainImage;
    // finishing here keeps that ordering trivially correct.
    const VkResult w = vkWaitForFences(device, 1, &fence, VK_TRUE, 2'000'000'000ull);
    if (w != VK_SUCCESS) {
        std::cout << "[ERROR] GPU work did not complete (VkResult " << (int)w << ")\n";
        return false;
    }
    return true;
}

void VkContext::destroy() {
    if (fence) vkDestroyFence(device, fence, nullptr);
    if (cmd_pool) vkDestroyCommandPool(device, cmd_pool, nullptr);
    fence = VK_NULL_HANDLE;
    cmd = VK_NULL_HANDLE;
    cmd_pool = VK_NULL_HANDLE;
    // instance/device are owned by the OpenXR runtime path; destroyed in xr_app.
}

bool Texture::create(VkContext& c, uint32_t width, uint32_t height) {
    w = width;
    h = height;

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = kTexFormat;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(c.device, &ii, nullptr, &image) != VK_SUCCESS) {
        std::cout << "[ERROR] vkCreateImage failed\n";
        return false;
    }

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(c.device, image, &mr);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = mr.size;
    if (!find_memory_type(c.phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, ma.memoryTypeIndex) &&
        !find_memory_type(c.phys, mr.memoryTypeBits, 0, ma.memoryTypeIndex)) {
        std::cout << "[ERROR] no memory type for texture\n";
        return false;
    }
    if (vkAllocateMemory(c.device, &ma, nullptr, &mem) != VK_SUCCESS) {
        std::cout << "[ERROR] image vkAllocateMemory failed\n";
        return false;
    }
    vkBindImageMemory(c.device, image, mem, 0);

    // Persistent host-visible staging buffer (reused for every re-upload).
    const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(c.device, &bi, nullptr, &staging) != VK_SUCCESS) {
        std::cout << "[ERROR] staging vkCreateBuffer failed\n";
        return false;
    }
    VkMemoryRequirements smr{};
    vkGetBufferMemoryRequirements(c.device, staging, &smr);
    VkMemoryAllocateInfo sma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    sma.allocationSize = smr.size;
    if (!find_memory_type(c.phys, smr.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          sma.memoryTypeIndex)) {
        std::cout << "[ERROR] no host-visible memory for staging\n";
        return false;
    }
    if (vkAllocateMemory(c.device, &sma, nullptr, &staging_mem) != VK_SUCCESS) {
        std::cout << "[ERROR] staging vkAllocateMemory failed\n";
        return false;
    }
    vkBindBufferMemory(c.device, staging, staging_mem, 0);
    if (vkMapMemory(c.device, staging_mem, 0, bytes, 0, &staging_ptr) != VK_SUCCESS) {
        std::cout << "[ERROR] vkMapMemory failed\n";
        staging_ptr = nullptr;
        return false;
    }
    return true;
}

void Texture::stage(const uint8_t* rgba, uint32_t sw, uint32_t sh, float alpha) {
    sw = std::min(sw, w);
    sh = std::min(sh, h);
    auto* dst = static_cast<uint8_t*>(staging_ptr);
    if (!dst) return;
    const size_t n = (size_t)sw * sh * 4;
    if (alpha >= 0.999f) {
        std::memcpy(dst, rgba, n);
        return;
    }
    const uint32_t a = (uint32_t)std::clamp(alpha * 256.0f, 0.0f, 256.0f);
    for (size_t i = 0; i < n; i += 4) {
        dst[i + 0] = rgba[i + 0];
        dst[i + 1] = rgba[i + 1];
        dst[i + 2] = rgba[i + 2];
        dst[i + 3] = (uint8_t)((rgba[i + 3] * a) >> 8);
    }
}

void Texture::record_upload(VkCommandBuffer cmd, uint32_t sw, uint32_t sh) {
    sw = std::min(sw, w);
    sh = std::min(sh, h);
    image_barrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.bufferRowLength = sw;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {sw, sh, 1};
    vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    image_barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
}

void Texture::destroy(VkContext& c) {
    if (staging_ptr) {
        vkUnmapMemory(c.device, staging_mem);
        staging_ptr = nullptr;
    }
    if (staging) vkDestroyBuffer(c.device, staging, nullptr);
    if (staging_mem) vkFreeMemory(c.device, staging_mem, nullptr);
    if (image) vkDestroyImage(c.device, image, nullptr);
    if (mem) vkFreeMemory(c.device, mem, nullptr);
    image = VK_NULL_HANDLE;
    mem = VK_NULL_HANDLE;
    staging = VK_NULL_HANDLE;
    staging_mem = VK_NULL_HANDLE;
}

void record_blit(VkCommandBuffer cmd, const Texture& src, uint32_t sw, uint32_t sh, VkImage dst, uint32_t dw,
                 uint32_t dh, bool clear_dst) {
    image_barrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (clear_dst) {
        const VkClearColorValue zero{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr,
                             0, nullptr);
    }

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {(int32_t)std::min(sw, src.w), (int32_t)std::min(sh, src.h), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = {(int32_t)dw, (int32_t)dh, 1};
    vkCmdBlitImage(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_LINEAR);

    // Runtimes expect color swapchain images in COLOR_ATTACHMENT_OPTIMAL
    // at xrReleaseSwapchainImage.
    image_barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
}
