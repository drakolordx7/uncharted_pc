/* Based on SpartanPort (FelipePrado0/SpartanPort, GPL-2.0): src/cpu_check.c, src/isa_emulate.c, src/isa_rewrite.c, scripts/isa_rewrite.py. */
#include "cpu_check.h"
#include <cpuid.h>
#include <stdio.h>

CpuFeatureWords cpu_feature_words(void) {
    CpuFeatureWords words = {0, 0, 0};
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (__get_cpuid(1, &a, &b, &c, &d)) words.leaf1_ecx = c;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) words.leaf7_ebx = b;
    if (__get_cpuid(0x80000001, &a, &b, &c, &d)) words.ext1_ecx = c;
    (void)a; (void)d;
    return words;
}

/* Jaguar's sets that Intel CPUs got with Haswell (Ivy Bridge has F16C, Sandy Bridge neither): the
 * game's instructions of these sets are rewritten at load time (isa_rewrite.c). SSE4.2, POPCNT and
 * AVX cannot be: probe.c stops without them. */
enum { LEAF1, LEAF7, EXT1 };
typedef struct { const char *name; int word; uint32_t bit; unsigned set; } CpuSet;
static const CpuSet rewritable_sets[] = {
    {"MOVBE", LEAF1, 1u << 22, CPU_SET_MOVBE}, {"F16C", LEAF1, 1u << 29, CPU_SET_F16C},
    {"BMI1", LEAF7, 1u << 3, CPU_SET_BMI1},    {"LZCNT", EXT1, 1u << 5, CPU_SET_LZCNT},
};

/* Names the missing sets, comma separated in out; returns how many, and their CPU_SET_* bits in *mask. */
static int missing_sets(CpuFeatureWords words, const CpuSet *sets, size_t count, char *out, size_t size,
                        unsigned *mask) {
    const uint32_t values[] = {words.leaf1_ecx, words.leaf7_ebx, words.ext1_ecx};
    int missing = 0;
    size_t used = 0;
    *mask = 0;
    if (size) out[0] = 0;
    for (size_t i = 0; i < count; ++i) {
        if (values[sets[i].word] & sets[i].bit) continue;
        if (used < size) {
            const int n = snprintf(out + used, size - used, "%s%s", missing ? ", " : "", sets[i].name);
            used = n < 0 ? size : used + (size_t)n;
            if (used >= size) used = size - 1;
        }
        *mask |= sets[i].set;
        ++missing;
    }
    return missing;
}

unsigned cpu_missing_rewritable(CpuFeatureWords words, char *out, size_t size) {
    unsigned mask;
    missing_sets(words, rewritable_sets, sizeof rewritable_sets / sizeof *rewritable_sets, out, size, &mask);
    return mask;
}
