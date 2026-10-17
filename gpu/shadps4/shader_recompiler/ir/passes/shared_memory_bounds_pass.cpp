// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <vector>

#include "common/alignment.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {

// bbport: GCN bounds-checks LDS accesses against the workgroup's allocation (LDS_SIZE): reads
// beyond it return 0 and writes are dropped. Vulkan shared memory has no such check, so a shader
// that relies on it (U3's 1024-thread cloth solver sums lds[i] + lds[i + 512] with only 512 dwords
// allocated) reads the wave64 ballot scratch and whatever lies past the array instead of zeros.
// Out-of-range accesses are sent to a sink dword after the allocation, and loads and atomics
// from out of range yield 0. BB_LDS_BOUNDS=0 turns this off.
void SharedMemoryBoundsPass(IR::Program& program, const RuntimeInfo& runtime_info) {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_LDS_BOUNDS");
        return !env || std::atoi(env) != 0;
    }();
    const u32 size = runtime_info.hw.cs.shared_memory_size;
    if (!enabled || program.info.hw_stage != HwStage::Compute || size < sizeof(u64)) {
        return;
    }
    const u32 sink = Common::AlignUp(size, sizeof(u64));
    bool used_sink = false;
    for (IR::Block* const block : program.blocks) {
        for (auto it = block->Instructions().begin(); it != block->Instructions().end(); ++it) {
            IR::Inst& inst = *it;
            u32 bytes = 0;
            bool returns_value = true;
            switch (inst.GetOpcode()) {
            case IR::Opcode::LoadSharedU16:
                bytes = 2;
                break;
            case IR::Opcode::WriteSharedU16:
                bytes = 2;
                returns_value = false;
                break;
            case IR::Opcode::LoadSharedU32:
            case IR::Opcode::SharedAtomicIAdd32:
            case IR::Opcode::SharedAtomicISub32:
            case IR::Opcode::SharedAtomicSMin32:
            case IR::Opcode::SharedAtomicUMin32:
            case IR::Opcode::SharedAtomicSMax32:
            case IR::Opcode::SharedAtomicUMax32:
            case IR::Opcode::SharedAtomicInc32:
            case IR::Opcode::SharedAtomicDec32:
            case IR::Opcode::SharedAtomicAnd32:
            case IR::Opcode::SharedAtomicOr32:
            case IR::Opcode::SharedAtomicXor32:
            case IR::Opcode::SharedAtomicCmpSwap32:
                bytes = 4;
                break;
            case IR::Opcode::WriteSharedU32:
                bytes = 4;
                returns_value = false;
                break;
            case IR::Opcode::LoadSharedU64:
            case IR::Opcode::SharedAtomicIAdd64:
            case IR::Opcode::SharedAtomicISub64:
            case IR::Opcode::SharedAtomicSMin64:
            case IR::Opcode::SharedAtomicUMin64:
            case IR::Opcode::SharedAtomicSMax64:
            case IR::Opcode::SharedAtomicUMax64:
            case IR::Opcode::SharedAtomicInc64:
            case IR::Opcode::SharedAtomicDec64:
            case IR::Opcode::SharedAtomicAnd64:
            case IR::Opcode::SharedAtomicOr64:
            case IR::Opcode::SharedAtomicXor64:
            case IR::Opcode::SharedAtomicCmpSwap64:
                bytes = 8;
                break;
            case IR::Opcode::WriteSharedU64:
                bytes = 8;
                returns_value = false;
                break;
            default:
                continue;
            }
            if (inst.Flags<bool>()) { // GDS
                continue;
            }
            const IR::Value address = inst.Arg(0);
            if (address.IsImmediate() && address.U32() + bytes <= size) {
                continue;
            }
            IR::IREmitter ir{*block, it};
            const IR::U1 in_range =
                ir.ILessThanEqual(IR::U32{address}, ir.Imm32(size - bytes), false);
            inst.SetArg(0, ir.Select(in_range, address, ir.Imm32(sink)));
            used_sink = true;
            if (!returns_value) {
                continue;
            }
            // The zero for out-of-range reads, after the access. There is no 16-bit select, so a
            // 16-bit load is selected as 32 bits.
            IR::IREmitter after{*block, std::next(it)};
            IR::Value result;
            if (bytes == 2) {
                const IR::U32 wide{after.UConvert(32, IR::U16{IR::Value{&inst}})};
                result = after.UConvert(16, IR::U32{after.Select(in_range, wide, ir.Imm32(0))});
            } else {
                const IR::Value zero =
                    bytes == 8 ? IR::Value{ir.Imm64(u64{0})} : IR::Value{ir.Imm32(0)};
                result = after.Select(in_range, IR::Value{&inst}, zero);
            }
            // Everything but the instruction just added that reads the access reads the result.
            const IR::Inst* const reader = &*std::next(it);
            const std::vector<IR::Use> uses(inst.Uses().begin(), inst.Uses().end());
            for (const IR::Use& use : uses) {
                if (use.user != reader) {
                    use.user->SetArg(use.operand, result);
                }
            }
            it = IR::Block::InstructionList::s_iterator_to(*result.Inst());
        }
    }
    if (used_sink) {
        program.info.shared_memory_scratch_size = sink + sizeof(u64) - size;
    }
}

} // namespace Shader::Optimization
