// SPDX-License-Identifier: AGPL-3.0-only
// Modified by simplyyjessie, 2026-10-03 (Linux port). See NOTICE.md

#pragma once

#include <vector>
#include <sstream>

#include <vulkan/vulkan.h>
#include <openvr.h>
#ifndef _WIN32
#include <algorithm>
#include <SDL3/SDL_vulkan.h>
#endif

#define VK_VALIDATE_RESULT(e)                                  \
    if (e != VK_SUCCESS)                                       \
        fprintf(stderr, "[Vulkan] Error: VkResult = %d\n", e); \
    if (e > 0)                                                 \
        assert(e);                                             \

static auto IsVulkanInstanceExtensionAvailable(std::string extension) -> bool 
{
    auto IsExtensionAvailable = [](const std::vector<VkExtensionProperties>& properties, std::string extension) {
        for (const VkExtensionProperties& p : properties)
            if (strcmp(p.extensionName, extension.c_str()) == 0)
                return true;
        return false;
        };

    uint32_t extension_properties_count = {};
    std::vector<VkExtensionProperties> extension_properties = {};

    vkEnumerateInstanceExtensionProperties(nullptr, &extension_properties_count, nullptr);

    if (extension_properties_count > 0) {
        extension_properties.resize(extension_properties_count);
        vkEnumerateInstanceExtensionProperties(nullptr, &extension_properties_count, extension_properties.data());
    }
    else {
        throw std::runtime_error("Failed to get list of extensions required by OpenVR");
    }

    return IsExtensionAvailable(extension_properties, extension);
}

static auto IsVulkanDeviceExtensionAvailable(const VkPhysicalDevice& physical_device, std::string extension) -> bool 
{
    auto IsExtensionAvailable = [](const std::vector<VkExtensionProperties>& properties, std::string extension) {
        for (const VkExtensionProperties& p : properties)
            if (strcmp(p.extensionName, extension.c_str()) == 0)
                return true;
        return false;
    };

    uint32_t extension_properties_count = {};
    std::vector<VkExtensionProperties> extension_properties = {};

    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_properties_count, nullptr);

    if (extension_properties_count > 0) {
        extension_properties.resize(extension_properties_count);
        vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_properties_count, extension_properties.data());
    }
    else {
        throw std::runtime_error("Failed to get list of extensions required by OpenVR");
    }

    return IsExtensionAvailable(extension_properties, extension);
}

inline bool g_skipOpenVRVulkanExtensions = false;

#ifndef _WIN32
// The desktop window needs the surface extensions of whichever windowing
// system SDL picked (X11 or Wayland). OpenVR's list does not include them,
// so add SDL's on top. SDL video must be initialised first.
static auto AppendWindowSurfaceExtensions(std::vector<std::string>& result) -> void
{
    Uint32 count = 0;
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
    for (Uint32 i = 0; names && i < count; i++)
        if (std::find(result.begin(), result.end(), names[i]) == result.end())
            result.push_back(names[i]);
}
#endif

static auto GetVulkanInstanceExtensionsRequiredByOpenVR() -> std::vector<std::string>
{
    std::vector<std::string> result{};

    if (g_skipOpenVRVulkanExtensions || !vr::VRCompositor())
    {
        result.push_back("VK_KHR_surface");
#ifdef _WIN32
        result.push_back("VK_KHR_win32_surface");
#else
        AppendWindowSurfaceExtensions(result);
#endif
        return result;
    }

    uint32_t buffer_len = vr::VRCompositor()->GetVulkanInstanceExtensionsRequired(nullptr, 0);
    if (buffer_len > 0) 
    {
        std::vector<char> buffer(buffer_len + 1);
        vr::VRCompositor()->GetVulkanInstanceExtensionsRequired(buffer.data(), buffer_len);
        buffer[buffer_len] = '\0';

        std::string token{};
        std::istringstream token_stream(buffer.data());
        while (std::getline(token_stream, token, ' ')) {
            if (IsVulkanInstanceExtensionAvailable(token)) {
                result.push_back(token);
            } else {
                throw std::runtime_error(std::format("ERROR! {} instance extension asked by OpenVR was NOT available\n", token.c_str()));
            }
        }
    } else {
        throw std::runtime_error("Failed to get list of extensions required by OpenVR");
    }

#ifndef _WIN32
    AppendWindowSurfaceExtensions(result);
#endif
    return result;
}

static auto GetVulkanDeviceExtensionsRequiredByOpenVR(const VkPhysicalDevice& device) -> std::vector<std::string> 
{
    std::vector<std::string> result{};

    if (g_skipOpenVRVulkanExtensions || !vr::VRCompositor()) {
        return result;
    }

    uint32_t buffer_len = vr::VRCompositor()->GetVulkanDeviceExtensionsRequired(device, nullptr, 0);
    if (buffer_len > 0) {
        std::vector<char> buffer(buffer_len + 1);
        vr::VRCompositor()->GetVulkanDeviceExtensionsRequired(device, buffer.data(), buffer_len);
        buffer[buffer_len] = '\0';

        std::string token{};
        std::istringstream token_stream(buffer.data());
        while (std::getline(token_stream, token, ' ')) {
            if (IsVulkanDeviceExtensionAvailable(device, token.data())) {
                result.push_back(token);
            } else {
                throw std::runtime_error(std::format("ERROR! {} device extension asked by OpenVR was NOT available\n", token.c_str()));
            }
        }
    } else {
        throw std::runtime_error("Failed to get list of extensions required by OpenVR");
    }

    return result;
}