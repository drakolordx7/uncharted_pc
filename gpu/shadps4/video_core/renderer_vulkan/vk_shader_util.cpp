// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <vector>
#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device) {
    // Test knob: BB_TEST_SPV_SALT=<n> changes the SPIR-V generator word of every module, so the
    // driver's own on-disk shader cache cannot hit (a cold-driver measurement on a used PC).
    static const char* salt_env = std::getenv("BB_TEST_SPV_SALT");
    std::vector<u32> salted;
    if (salt_env && code.size() > 5) {
        salted.assign(code.begin(), code.end());
        salted[2] ^= u32(std::strtoul(salt_env, nullptr, 0)) * 2654435761u;
        code = salted;
    }
    const vk::ShaderModuleCreateInfo shader_info = {
        .codeSize = code.size() * sizeof(u32),
        .pCode = code.data(),
    };

    auto [module_result, module] = device.createShaderModule(shader_info);
    ASSERT_MSG(module_result == vk::Result::eSuccess, "Failed to compile SPIR-V shader: {}",
               vk::to_string(module_result));
    return module;
}

} // namespace Vulkan
