// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <vector>
#include "common/assert.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

// bbport: swaps the object motion address stand-ins in a vertex shader (runtime_info.h,
// MotionVectors) for the session's buffer addresses. The cache stores the stand-ins.
static void PatchMotionAddresses(std::vector<u32>& code) {
    using Shader::MotionVectors;
    constexpr u32 OpConstant = 43;
    for (size_t at = 5; at < code.size();) {
        const u32 op = code[at] & 0xFFFF;
        const u32 words = code[at] >> 16;
        if (words == 0 || at + words > code.size()) {
            return;
        }
        if (op == OpConstant && words == 5) {
            const u64 value = u64(code[at + 3]) | u64(code[at + 4]) << 32;
            u64 patched = value;
            if (value - MotionVectors::ParamsPlaceholder <= 16) {
                patched = MotionVectors::params_address + (value - MotionVectors::ParamsPlaceholder);
            } else if (value == MotionVectors::PositionsPlaceholder) {
                patched = MotionVectors::positions_address;
            }
            if (patched != value) {
                code[at + 3] = u32(patched);
                code[at + 4] = u32(patched >> 32);
            }
        }
        at += words;
    }
}

vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device) {
    // Test knob: BB_TEST_SPV_SALT=<n> changes the SPIR-V generator word of every module, so the
    // driver's own on-disk shader cache cannot hit (a cold-driver measurement on a used PC).
    static const char* salt_env = std::getenv("BB_TEST_SPV_SALT");
    std::vector<u32> patched;
    if (Shader::MotionVectors::positions_address != 0 && code.size() > 5) {
        // Cheap pre-check: only vertex shaders with the motion path contain the stand-ins.
        bool has_placeholder = false;
        for (size_t i = 5; i + 1 < code.size() && !has_placeholder; ++i) {
            has_placeholder = code[i + 1] == u32(Shader::MotionVectors::ParamsPlaceholder >> 32) &&
                              code[i] == u32(Shader::MotionVectors::ParamsPlaceholder);
        }
        if (has_placeholder) {
            patched.assign(code.begin(), code.end());
            PatchMotionAddresses(patched);
            code = patched;
        }
    }
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
