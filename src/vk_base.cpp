#include "vk_base.h"

#include <cstring>
#include <functional>
#include <iostream>

namespace {

uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t type_bits,
                          VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return 0;
}

// Record a one-shot command buffer and block until the GPU finishes.
void one_shot(VkContext& c, const std::function<void(VkCommandBuffer)>& record) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool        = c.cmd_pool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(c.device, &ai, &cmd);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    record(cmd);
    vkEndCommandBuffer(cmd);

    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(c.device, &fi, nullptr, &fence);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;
    vkQueueSubmit(c.queue, 1, &si, fence);
    vkWaitForFences(c.device, 1, &fence, VK_TRUE, UINT64_MAX);

    vkDestroyFence(c.device, fence, nullptr);
    vkFreeCommandBuffers(c.device, c.cmd_pool, 1, &cmd);
}

void image_barrier(VkCommandBuffer cmd, VkImage img,
                   VkImageLayout from, VkImageLayout to,
                   VkAccessFlags src_access, VkAccessFlags dst_access,
                   VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout           = from;
    b.newLayout           = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = img;
    b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask       = src_access;
    b.dstAccessMask       = dst_access;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

constexpr VkFormat kTexFormat = VK_FORMAT_R8G8B8A8_SRGB;

} // namespace

bool VkContext::init_pool() {
    VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = queue_family;
    if (vkCreateCommandPool(device, &ci, nullptr, &cmd_pool) != VK_SUCCESS) {
        std::cout << "[ERROR] vkCreateCommandPool failed\n";
        return false;
    }
    return true;
}

void VkContext::destroy() {
    if (cmd_pool) vkDestroyCommandPool(device, cmd_pool, nullptr);
    cmd_pool = VK_NULL_HANDLE;
    // instance/device are owned by the OpenXR runtime path; destroyed in xr_app.
}

bool Texture::create(VkContext& c, uint32_t width, uint32_t height) {
    w = width;
    h = height;

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType     = VK_IMAGE_TYPE_2D;
    ii.format        = kTexFormat;
    ii.extent        = {w, h, 1};
    ii.mipLevels     = 1;
    ii.arrayLayers   = 1;
    ii.samples       = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ii.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(c.device, &ii, nullptr, &image) != VK_SUCCESS) {
        std::cout << "[ERROR] vkCreateImage failed\n";
        return false;
    }

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(c.device, image, &mr);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize  = mr.size;
    ma.memoryTypeIndex = find_memory_type(c.phys, mr.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(c.device, &ma, nullptr, &mem) != VK_SUCCESS) {
        std::cout << "[ERROR] image vkAllocateMemory failed\n";
        return false;
    }
    vkBindImageMemory(c.device, image, mem, 0);

    // Persistent host-visible staging buffer (reused for text re-uploads).
    const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size        = bytes;
    bi.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(c.device, &bi, nullptr, &staging) != VK_SUCCESS) {
        std::cout << "[ERROR] staging vkCreateBuffer failed\n";
        return false;
    }
    VkMemoryRequirements smr{};
    vkGetBufferMemoryRequirements(c.device, staging, &smr);
    VkMemoryAllocateInfo sma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    sma.allocationSize  = smr.size;
    sma.memoryTypeIndex = find_memory_type(
        c.phys, smr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(c.device, &sma, nullptr, &staging_mem) != VK_SUCCESS) {
        std::cout << "[ERROR] staging vkAllocateMemory failed\n";
        return false;
    }
    vkBindBufferMemory(c.device, staging, staging_mem, 0);
    vkMapMemory(c.device, staging_mem, 0, bytes, 0, &staging_ptr);
    return true;
}

void Texture::upload(VkContext& c, const uint8_t* rgba) {
    std::memcpy(staging_ptr, rgba, (size_t)w * h * 4);

    one_shot(c, [&](VkCommandBuffer cmd) {
        image_barrier(cmd, image,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent      = {w, h, 1};
        vkCmdCopyBufferToImage(cmd, staging, image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        image_barrier(cmd, image,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    });
}

void Texture::destroy(VkContext& c) {
    if (staging_ptr) { vkUnmapMemory(c.device, staging_mem); staging_ptr = nullptr; }
    if (staging)     vkDestroyBuffer(c.device, staging, nullptr);
    if (staging_mem) vkFreeMemory(c.device, staging_mem, nullptr);
    if (image)       vkDestroyImage(c.device, image, nullptr);
    if (mem)         vkFreeMemory(c.device, mem, nullptr);
    image = VK_NULL_HANDLE; mem = VK_NULL_HANDLE;
    staging = VK_NULL_HANDLE; staging_mem = VK_NULL_HANDLE;
}

void blit_texture(VkContext& c, const Texture& src,
                  VkImage dst, uint32_t dst_w, uint32_t dst_h) {
    one_shot(c, [&](VkCommandBuffer cmd) {
        image_barrier(cmd, dst,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0]  = {0, 0, 0};
        blit.srcOffsets[1]  = {(int32_t)src.w, (int32_t)src.h, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0]  = {0, 0, 0};
        blit.dstOffsets[1]  = {(int32_t)dst_w, (int32_t)dst_h, 1};
        vkCmdBlitImage(cmd,
                       src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dst,       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);

        // Runtimes expect color swapchain images in COLOR_ATTACHMENT_OPTIMAL
        // at xrReleaseSwapchainImage.
        image_barrier(cmd, dst,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    });
}
