// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

namespace AmdGpu {
struct ComputeProgram;
union Regs;
} // namespace AmdGpu

namespace Shader {
struct Info;
}

namespace Vulkan {

class Rasterizer;

/// bbport: shaders whose HLE writes guest memory. They run where the dispatch is processed,
/// not on the draw recording thread: the SRT walkers of the draws after them read that memory.
bool IsGuestMemoryShaderHLE(unsigned long long pgm_hash);

/// bbport: the range a 64-dword scalar copy (the mesh raycasters' results, U2) moves, as the
/// dispatch's registers give it; nullopt for other shaders and for copies the HLE does not take.
struct ScalarCopyRange {
    unsigned long long src;
    unsigned long long dst;
    unsigned long long size;
};
std::optional<ScalarCopyRange> RaycasterCopyRange(const Shader::Info& info,
                                                  const AmdGpu::ComputeProgram& cs_program);
/// bbport: whether such a copy of `size` bytes, its source holding GPU-written bytes, leaves
/// guest memory alone when it runs (copied on the GPU only, or its guest part deferred to the
/// landing thread), as the BB_SCALAR_COPY* switches set it.
bool RaycasterCopyLeavesGuestMemory(unsigned long long size);

/// Attempts to execute a shader using HLE if possible.
bool ExecuteShaderHLE(const Shader::Info& info, const AmdGpu::Regs& regs,
                      const AmdGpu::ComputeProgram& cs_program, Rasterizer& rasterizer);

} // namespace Vulkan
