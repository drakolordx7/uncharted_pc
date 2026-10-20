// SPDX-License-Identifier: GPL-2.0-or-later
// bbport diagnostics: host (system) memory gauges for BB_VRAM_LOG / BB_MEM_LOG. Each cache that keeps
// guest-sized data in system memory adds its bytes here; ReportHostMemory() prints them together
// with the process heaps and the process's private bytes.
#pragma once
#include <atomic>
#include <cstdint>

namespace bbport::hostmem {
inline std::atomic<uint64_t> shadow_bytes{0};  ///< BufferCache::ShadowGpuData copies
inline std::atomic<uint64_t> gauge_a{0}, gauge_b{0}, gauge_c{0}, gauge_d{0};
/// One "HOSTMEM:" line: private bytes, working set, process heaps (committed/in use), gauges.
void Report();
/// With -DBB_ALLOC_TRACE (BB_ALLOC_TRACE=lo-hi): the call stacks that hold the most live allocations of that size.
void ReportAllocTrace();
} // namespace bbport::hostmem
