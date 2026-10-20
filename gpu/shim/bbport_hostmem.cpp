// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_hostmem.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <map>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

namespace bbport::hostmem {
#ifdef _WIN32
// BB_HOSTMEM_WALK=1: every 30 s a walk of the process heaps, the allocations bucketed by size (exact up to 4 KiB, then
// by power of two); the buckets that hold the most bytes, so two reports show what grows.
static void Walk() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_HOSTMEM_WALK");
        return env && env[0] == '1';
    }();
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (!enabled || now - last < std::chrono::seconds(30)) {
        return;
    }
    last = now;
    std::map<unsigned long long, std::pair<unsigned long long, unsigned long long>> buckets; // size -> count, bytes
    HANDLE heaps[256];
    const DWORD count = GetProcessHeaps(256, heaps);
    for (DWORD i = 0; i < count && i < 256; ++i) {
        if (!HeapLock(heaps[i])) {
            continue;
        }
        PROCESS_HEAP_ENTRY entry{};
        while (HeapWalk(heaps[i], &entry)) {
            if (!(entry.wFlags & PROCESS_HEAP_ENTRY_BUSY)) {
                continue;
            }
            unsigned long long size = entry.cbData;
            if (size > 4096) {
                unsigned long long p = 8192;
                while (p < size) {
                    p <<= 1;
                }
                size = p;
            }
            auto& b = buckets[size];
            ++b.first;
            b.second += entry.cbData;
        }
        HeapUnlock(heaps[i]);
    }
    std::vector<std::pair<unsigned long long, std::pair<unsigned long long, unsigned long long>>> v(buckets.begin(),
                                                                                                     buckets.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
    std::printf("HOSTWALK:");
    for (size_t i = 0; i < v.size() && i < 14; ++i) {
        std::printf(" %llu:%llux=%lluK", v[i].first, v[i].second.first, v[i].second.second >> 10);
    }
    std::printf("\n");
}
#endif
void Report() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                         sizeof(pmc));
    HANDLE heaps[256];
    const DWORD count = GetProcessHeaps(256, heaps);
    unsigned long long committed = 0, allocated = 0, big_committed = 0, big_allocated = 0;
    unsigned big_index = 0;
    for (DWORD i = 0; i < count && i < 256; ++i) {
        HEAP_SUMMARY hs{};
        hs.cb = sizeof(hs);
        if (!HeapSummary(heaps[i], 0, &hs)) {
            continue;
        }
        committed += hs.cbCommitted;
        allocated += hs.cbAllocated;
        if (hs.cbCommitted > big_committed) {
            big_committed = hs.cbCommitted;
            big_allocated = hs.cbAllocated;
            big_index = i;
        }
    }
    std::printf("HOSTMEM: private %llu MiB, working set %llu MiB; %lu heaps committed %llu MiB (in use %llu MiB), "
                "largest #%u committed %llu MiB (in use %llu MiB); shadows %llu MiB, gauges %llu %llu %llu %llu\n",
                (unsigned long long)(pmc.PrivateUsage >> 20), (unsigned long long)(pmc.WorkingSetSize >> 20),
                (unsigned long)count, committed >> 20, allocated >> 20, big_index, big_committed >> 20,
                big_allocated >> 20, (unsigned long long)(shadow_bytes.load() >> 20),
                (unsigned long long)gauge_a.load(), (unsigned long long)gauge_b.load(),
                (unsigned long long)gauge_c.load(), (unsigned long long)gauge_d.load());
    Walk();
    ReportAllocTrace();
#endif
}
} // namespace bbport::hostmem
