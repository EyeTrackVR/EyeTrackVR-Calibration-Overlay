#pragma once
// Single include point that pulls in Vulkan and OpenXR with the Vulkan graphics
// bindings enabled, in the required order (vulkan.h before openxr_platform.h).
#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
