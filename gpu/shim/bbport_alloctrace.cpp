// SPDX-License-Identifier: GPL-2.0-or-later
// bbport diagnostics, compiled only with -DBB_ALLOC_TRACE (CXXFLAGS at configure time): replaces global operator new/delete to attribute the live
// allocations of one size range (BB_ALLOC_TRACE=lo-hi bytes) to their call stacks. bbport::hostmem::Report prints
// the stacks with the most live allocations (frames as RVAs into the exe; resolve with llvm-symbolizer).
#ifdef BB_ALLOC_TRACE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <malloc.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace {
constexpr size_t Slots = 1u << 21;
std::atomic<void*> g_ptr[Slots];
std::atomic<uint32_t> g_stack[Slots];
size_t g_lo = 1, g_hi = 0;
bool g_init = false;
struct Stack { void* frames[14]; unsigned n; long long live; long long total; };
std::mutex g_mutex;
std::unordered_map<uint32_t, Stack>* g_stacks;

void Init() {
    const char* env = std::getenv("BB_ALLOC_TRACE");
    if (env) {
        unsigned long lo = 0, hi = 0;
        if (std::sscanf(env, "%lu-%lu", &lo, &hi) == 2) {
            g_lo = lo;
            g_hi = hi;
        }
    }
    g_stacks = new std::unordered_map<uint32_t, Stack>();
    g_init = true;
}
size_t Hash(void* p) { return (reinterpret_cast<uintptr_t>(p) >> 4) * 0x9E3779B97F4A7C15ull >> 43 & (Slots - 1); }

void Record(void* p, size_t n) {
    if (!g_init) {
        return;
    }
    if (n < g_lo || n > g_hi) {
        return;
    }
    void* frames[14];
    DWORD hash = 0;
    const USHORT count = RtlCaptureStackBackTrace(2, 14, frames, &hash);
    {
        std::scoped_lock lk{g_mutex};
        auto& s = (*g_stacks)[hash];
        if (s.n == 0) {
            s.n = count;
            std::copy(frames, frames + count, s.frames);
        }
        ++s.live;
        ++s.total;
    }
    for (size_t i = Hash(p), tries = 0; tries < 64; i = (i + 1) & (Slots - 1), ++tries) {
        void* expected = nullptr;
        if (g_ptr[i].compare_exchange_strong(expected, p)) {
            g_stack[i].store(hash);
            return;
        }
    }
}
void Forget(void* p) {
    if (!g_init || !p || g_lo > g_hi) {
        return;
    }
    for (size_t i = Hash(p), tries = 0; tries < 64; i = (i + 1) & (Slots - 1), ++tries) {
        if (g_ptr[i].load(std::memory_order_relaxed) == p) {
            const uint32_t hash = g_stack[i].load();
            g_ptr[i].store(nullptr);
            std::scoped_lock lk{g_mutex};
            --(*g_stacks)[hash].live;
            return;
        }
    }
}
struct Starter {
    Starter() { Init(); }
} g_starter;
} // namespace

namespace bbport::hostmem {
void ReportAllocTrace() {
    if (!g_init || g_lo > g_hi) {
        return;
    }
    std::vector<Stack> v;
    {
        std::scoped_lock lk{g_mutex};
        for (auto& [h, s] : *g_stacks) {
            v.push_back(s);
        }
    }
    std::sort(v.begin(), v.end(), [](const Stack& a, const Stack& b) { return a.live > b.live; });
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    for (size_t i = 0; i < v.size() && i < 6; ++i) {
        std::printf("ALLOCTRACE live %lld of %lld:", v[i].live, v[i].total);
        for (unsigned f = 0; f < v[i].n; ++f) {
            std::printf(" %llx", (unsigned long long)(reinterpret_cast<uintptr_t>(v[i].frames[f]) - base));
        }
        std::printf("\n");
    }
}
} // namespace bbport::hostmem

void* operator new(size_t n) {
    void* p = std::malloc(n ? n : 1);
    if (!p) {
        throw std::bad_alloc();
    }
    Record(p, n);
    return p;
}
void* operator new[](size_t n) { return operator new(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept {
    void* p = std::malloc(n ? n : 1);
    if (p) {
        Record(p, n);
    }
    return p;
}
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return operator new(n, std::nothrow); }
void* operator new(size_t n, std::align_val_t a) {
    void* p = _aligned_malloc(n ? n : 1, static_cast<size_t>(a));
    if (!p) {
        throw std::bad_alloc();
    }
    Record(p, n);
    return p;
}
void* operator new[](size_t n, std::align_val_t a) { return operator new(n, a); }
void operator delete(void* p) noexcept { Forget(p); std::free(p); }
void operator delete[](void* p) noexcept { Forget(p); std::free(p); }
void operator delete(void* p, size_t) noexcept { Forget(p); std::free(p); }
void operator delete[](void* p, size_t) noexcept { Forget(p); std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { Forget(p); std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { Forget(p); std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { Forget(p); _aligned_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { Forget(p); _aligned_free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { Forget(p); _aligned_free(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { Forget(p); _aligned_free(p); }
#else
namespace bbport::hostmem {
void ReportAllocTrace() {}
} // namespace bbport::hostmem
#endif
