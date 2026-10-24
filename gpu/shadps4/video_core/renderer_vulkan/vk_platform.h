// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <fmt/format.h>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/string_literal.h"
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Frontend {
enum class WindowSystemType : u8;
class WindowSDL;
} // namespace Frontend

namespace Vulkan {

constexpr u32 TargetVulkanApiVersion = VK_API_VERSION_1_3;

vk::SurfaceKHR CreateSurface(vk::Instance instance, const Frontend::WindowSDL& emu_window);

vk::UniqueInstance CreateInstance(Frontend::WindowSystemType window_type, bool enable_validation,
                                  bool enable_crash_diagnostic);

vk::UniqueDebugUtilsMessengerEXT CreateDebugCallback(vk::Instance instance);

/// bbport: BB_GPU_ADDRESS_REPORT=1 (GPU address binding history for device-lost reports).
bool AddressReportEnabled();
vk::UniqueDebugUtilsMessengerEXT CreateAddressReportCallback(vk::Instance instance);
void DumpAddressBindings(u64 address);

/// bbport: the device a GPU crash report is read from. The port creates a single Vulkan device
/// and Instance registers it here once it exists (defined in vk_semaphore.cpp).
void SetDeviceLostDevice(vk::Device device);

/// bbport: prints the GPU crash report (driver fault info, checkpoints, arenas) and ends the
/// game (defined in vk_semaphore.cpp).
[[noreturn]] void ReportDeviceLost();

template <typename T>
concept VulkanHandleType = vk::isVulkanHandleType<T>::value;

template <VulkanHandleType HandleType>
void SetObjectName(vk::Device device, const HandleType& handle, std::string_view debug_name) {
    const vk::DebugUtilsObjectNameInfoEXT name_info = {
        .objectType = HandleType::objectType,
        .objectHandle = reinterpret_cast<u64>(static_cast<typename HandleType::NativeType>(handle)),
        .pObjectName = debug_name.data(),
    };
    auto result = device.setDebugUtilsObjectNameEXT(name_info);
    if (result != vk::Result::eSuccess) {
        LOG_DEBUG(Render_Vulkan, "Could not set object debug name: {}", vk::to_string(result));
    }
}

template <VulkanHandleType HandleType, typename... Args>
void SetObjectName(vk::Device device, const HandleType& handle, const char* format,
                   const Args&... args) {
    const std::string debug_name = fmt::vformat(format, fmt::make_format_args(args...));
    SetObjectName(device, handle, debug_name);
}

template <StringLiteral msg = "">
static void Check(vk::Result r) {
    // bbport: a lost device is a GPU crash, not a port bug. Report the driver's fault info, the
    // last checkpoints and the arenas instead of stopping on the generic assert below (the
    // report names the dispatch that hung the GPU; an assert here names nothing).
    if (r == vk::Result::eErrorDeviceLost) [[unlikely]] {
        ReportDeviceLost();
    }
    if constexpr (msg.len <= 1) {
        ASSERT_MSG(r == vk::Result::eSuccess, "vk::Result={}", vk::to_string(r));
    } else {
        ASSERT_MSG(r == vk::Result::eSuccess, "Failed to {}: vk::Result={}", msg.value,
                   vk::to_string(r));
    }
}

template <StringLiteral msg = "", typename T>
static T Check(vk::ResultValue<T> r) {
    if (r.result == vk::Result::eErrorDeviceLost) [[unlikely]] {
        ReportDeviceLost();
    }
    if constexpr (msg.len <= 1) {
        ASSERT_MSG(r.result == vk::Result::eSuccess, "vk::Result={}", vk::to_string(r.result));
    } else {
        ASSERT_MSG(r.result == vk::Result::eSuccess, "Failed to {}: vk::Result={}", msg.value,
                   vk::to_string(r.result));
    }
    return std::move(r.value);
}

/// bbport: like Check, but a lost device is only logged. For teardown (~Presenter): the process
/// is already going away and the crash report was printed by whichever call saw the loss first.
template <StringLiteral msg = "">
static void CheckTeardown(vk::Result r) {
    if (r != vk::Result::eErrorDeviceLost) {
        Check<msg>(r);
        return;
    }
    if constexpr (msg.len <= 1) {
        LOG_WARNING(Render_Vulkan, "Device lost during teardown");
    } else {
        LOG_WARNING(Render_Vulkan, "Device lost while trying to {}", msg.value);
    }
}

} // namespace Vulkan
