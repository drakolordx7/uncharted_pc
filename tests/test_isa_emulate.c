/* Checks src/isa_emulate.c against the real CPU instructions: MOVBE load and store (16/32/64), LZCNT and TZCNT
 * (16/32/64), ANDN, BEXTR, BLSI, BLSMSK and BLSR (32/64), and VCVTPH2PS over all 65536 half values.
 * Results are compared bit for bit. Flags are compared only where isa_defined_flags() says the instruction
 * defines them. Build: clang -std=c11 -O1 -mbmi -mlzcnt -mmovbe -mf16c -Isrc tests/test_isa_emulate.c out/isa_emulate.o
 * (out/isa_emulate.o is built separately with -mgeneral-regs-only). */
#include <cpuid.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "isa_emulate.h"

#define X86_PF (1u << 2)
#define X86_AF (1u << 4)
#define FLAG_BITS ((uint64_t)(ISA_CF | X86_PF | X86_AF | ISA_ZF | ISA_SF | ISA_OF))
#define RANDOM_CASES 20000
#define MXCSR_DEFAULT 0x1F80u

/* xorshift64 with a fixed seed, so every run checks the same inputs. */
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* Random value skewed toward small numbers (many leading or trailing zeros). */
static uint64_t rnd_skew(void) {
    uint64_t v = rnd();
    return v >> (rnd() & 63);
}

static const uint64_t edges[] = {
    0, 1, 2, 3, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x10000,
    0x7fffffffu, 0x80000000u, 0xffffffffu, 0x100000000ull,
    0x7fffffffffffffffull, 0x8000000000000000ull, ~0ull,
    0x00ff00ff00ff00ffull, 0xff00ff00ff00ff00ull, 0x0123456789abcdefull,
};
#define NEDGES (sizeof edges / sizeof edges[0])

static unsigned long long g_cases;

/* ---- real-CPU oracles ----
 * Each register-form oracle runs one instruction with rax = dst, rcx = src1, rdx = src2 (AT&T operand order is
 * reversed from Intel: "op src2, src1, dst"). The flags go in through popfq and come out through pushfq. The
 * lea steps move rsp past any red zone before the push, so the stack use cannot clobber the caller's locals. */
typedef uint64_t (*RealRegOp)(uint64_t dst, uint64_t s1, uint64_t s2, uint64_t *flags);
typedef uint64_t (*RealLoadOp)(uint64_t dst, const unsigned char *mem);
typedef void (*RealStoreOp)(uint64_t value, unsigned char *mem);

#define REAL_RR(FN, INSN, DST, S1, S2)                                          \
static uint64_t FN(uint64_t dst, uint64_t s1, uint64_t s2, uint64_t *flags) {   \
    uint64_t res = dst, fin = *flags, fout;                                     \
    __asm__ volatile(                                                           \
        "lea -128(%%rsp), %%rsp\n\t"                                            \
        "push %[fin]\n\t"                                                       \
        "popfq\n\t"                                                             \
        INSN " " S2 ", " S1 ", " DST "\n\t"                                     \
        "pushfq\n\t"                                                            \
        "pop %[fout]\n\t"                                                       \
        "lea 128(%%rsp), %%rsp\n\t"                                             \
        : "+a"(res), [fout] "=&r"(fout)                                         \
        : "c"(s1), "d"(s2), [fin] "r"(fin)                                      \
        : "cc", "memory");                                                      \
    *flags = fout;                                                              \
    return res;                                                                 \
}

#define REAL_R(FN, INSN, DST, S1)                                               \
static uint64_t FN(uint64_t dst, uint64_t s1, uint64_t s2, uint64_t *flags) {   \
    uint64_t res = dst, fin = *flags, fout;                                     \
    (void)s2;                                                                   \
    __asm__ volatile(                                                           \
        "lea -128(%%rsp), %%rsp\n\t"                                            \
        "push %[fin]\n\t"                                                       \
        "popfq\n\t"                                                             \
        INSN " " S1 ", " DST "\n\t"                                             \
        "pushfq\n\t"                                                            \
        "pop %[fout]\n\t"                                                       \
        "lea 128(%%rsp), %%rsp\n\t"                                             \
        : "+a"(res), [fout] "=&r"(fout)                                         \
        : "c"(s1), [fin] "r"(fin)                                               \
        : "cc", "memory");                                                      \
    *flags = fout;                                                              \
    return res;                                                                 \
}

#define REAL_LOAD(FN, DST)                                                      \
static uint64_t FN(uint64_t dst, const unsigned char *mem) {                    \
    uint64_t res = dst;                                                         \
    __asm__ volatile("movbe (%[m]), " DST "\n\t"                                \
                     : "+a"(res) : [m] "r"(mem) : "memory");                    \
    return res;                                                                 \
}

#define REAL_STORE(FN, SRC)                                                     \
static void FN(uint64_t value, unsigned char *mem) {                            \
    __asm__ volatile("movbe " SRC ", (%[m])\n\t"                                \
                     : : "a"(value), [m] "r"(mem) : "memory");                  \
}

REAL_RR(real_andn32, "andn", "%%eax", "%%ecx", "%%edx")
REAL_RR(real_andn64, "andn", "%%rax", "%%rcx", "%%rdx")
REAL_RR(real_bextr32, "bextr", "%%eax", "%%ecx", "%%edx")
REAL_RR(real_bextr64, "bextr", "%%rax", "%%rcx", "%%rdx")
REAL_R(real_blsi32, "blsi", "%%eax", "%%ecx")
REAL_R(real_blsi64, "blsi", "%%rax", "%%rcx")
REAL_R(real_blsmsk32, "blsmsk", "%%eax", "%%ecx")
REAL_R(real_blsmsk64, "blsmsk", "%%rax", "%%rcx")
REAL_R(real_blsr32, "blsr", "%%eax", "%%ecx")
REAL_R(real_blsr64, "blsr", "%%rax", "%%rcx")
REAL_R(real_lzcnt16, "lzcnt", "%%ax", "%%cx")
REAL_R(real_lzcnt32, "lzcnt", "%%eax", "%%ecx")
REAL_R(real_lzcnt64, "lzcnt", "%%rax", "%%rcx")
REAL_R(real_tzcnt16, "tzcnt", "%%ax", "%%cx")
REAL_R(real_tzcnt32, "tzcnt", "%%eax", "%%ecx")
REAL_R(real_tzcnt64, "tzcnt", "%%rax", "%%rcx")
REAL_LOAD(real_movbe_load16, "%%ax")
REAL_LOAD(real_movbe_load32, "%%eax")
REAL_LOAD(real_movbe_load64, "%%rax")
REAL_STORE(real_movbe_store16, "%%ax")
REAL_STORE(real_movbe_store32, "%%eax")
REAL_STORE(real_movbe_store64, "%%rax")

/* VEX vcvtph2ps xmm0, xmm1: the four halves in the low 64 bits of xmm1 go to four singles in xmm0. */
static void real_vcvtph2ps(uint64_t in, uint32_t out[4]) {
    __asm__ volatile(
        "vmovq %[in], %%xmm1\n\t"
        "vcvtph2ps %%xmm1, %%xmm0\n\t"
        "vmovups %%xmm0, (%[out])\n\t"
        : : [in] "r"(in), [out] "r"(out)
        : "xmm0", "xmm1", "memory");
}

/* ---- reporting ---- */
static void die_case(const char *why, const char *name, unsigned width, uint64_t s1, uint64_t s2, uint64_t dst,
                     uint64_t flags_in, uint64_t exp_res, uint64_t act_res, uint64_t exp_fl, uint64_t act_fl,
                     uint64_t defined) {
    printf("MISMATCH (%s): %s width %u\n", why, name, width);
    printf("  inputs: src1=0x%016llx src2=0x%016llx dst_in=0x%016llx flags_in=0x%03llx\n",
           (unsigned long long)s1, (unsigned long long)s2, (unsigned long long)dst,
           (unsigned long long)(flags_in & FLAG_BITS));
    printf("  expected: result=0x%016llx flags=0x%03llx\n", (unsigned long long)exp_res,
           (unsigned long long)(exp_fl & defined));
    printf("  actual:   result=0x%016llx flags=0x%03llx\n", (unsigned long long)act_res,
           (unsigned long long)(act_fl & defined));
    printf("  compared flag mask=0x%03llx\n", (unsigned long long)defined);
    exit(1);
}

static void report(const char *name, unsigned width, unsigned long long start) {
    printf("%-8s width %2u: %llu cases ok\n", name, width, g_cases - start);
}

/* ---- register forms ---- */
static void check_reg(const char *name, uint8_t kind, uint8_t width, RealRegOp real, uint64_t dst, uint64_t s1,
                      uint64_t s2, uint64_t flags_in) {
    IsaSite site;
    IsaRegs regs;
    uint64_t before[16], fl = flags_in & FLAG_BITS, exp, defined = isa_defined_flags(kind);
    int i;

    memset(&site, 0, sizeof site);
    site.kind = kind;
    site.width = width;
    site.dst = 0;
    site.src1 = 1;
    site.src2 = 2;
    site.mem_slot = ISA_MEM_NONE;
    site.base = ISA_NO_REG;
    site.index = ISA_NO_REG;

    memset(&regs, 0, sizeof regs);
    for (i = 0; i < 16; i++) regs.gpr[i] = rnd();
    regs.gpr[0] = dst;
    regs.gpr[1] = s1;
    regs.gpr[2] = s2;
    regs.rflags = fl;
    memcpy(before, regs.gpr, sizeof before);

    exp = real(dst, s1, s2, &fl);
    isa_emulate(&site, &regs);
    g_cases++;

    if (regs.gpr[0] != exp)
        die_case("result", name, width, s1, s2, dst, flags_in, exp, regs.gpr[0], fl, regs.rflags, defined);
    if ((regs.rflags ^ fl) & defined)
        die_case("flags", name, width, s1, s2, dst, flags_in, exp, regs.gpr[0], fl, regs.rflags, defined);
    for (i = 1; i < 16; i++)
        if (regs.gpr[i] != before[i])
            die_case("other register changed", name, width, s1, s2, dst, flags_in, before[i], regs.gpr[i], 0, 0,
                     0);
}

/* Edge pairs (two-source) or edge singles, then random inputs. No printing. */
static void reg_cases(const char *name, uint8_t kind, uint8_t width, RealRegOp real, int two_src) {
    size_t i, j;
    unsigned k;
    for (i = 0; i < NEDGES; i++) {
        for (j = 0; j < (two_src ? NEDGES : 1); j++) {
            uint64_t dst = rnd(), fl = rnd(), s2 = two_src ? edges[j] : 0;
            check_reg(name, kind, width, real, dst, edges[i], s2, fl);
        }
    }
    for (k = 0; k < RANDOM_CASES; k++) {
        uint64_t dst = rnd(), fl = rnd();
        uint64_t s1 = (k & 1) ? rnd() : rnd_skew();
        uint64_t s2 = two_src ? ((k & 2) ? rnd() : rnd_skew()) : 0;
        check_reg(name, kind, width, real, dst, s1, s2, fl);
    }
}

static void group_reg(const char *name, uint8_t kind, uint8_t width, RealRegOp real, int two_src) {
    unsigned long long start = g_cases;
    reg_cases(name, kind, width, real, two_src);
    report(name, width, start);
}

/* BEXTR: every 16-bit control word (bits 16-63 random) against a random source, then random in-range fields. */
static void group_bextr(uint8_t width, RealRegOp real) {
    unsigned long long start = g_cases;
    uint32_t ctl;
    unsigned k;
    reg_cases("BEXTR", ISA_BEXTR, width, real, 1);
    for (ctl = 0; ctl < 65536; ctl++) {
        uint64_t s1 = rnd(), c = ctl | (rnd() & ~0xffffull), dst = rnd(), fl = rnd();
        check_reg("BEXTR", ISA_BEXTR, width, real, dst, s1, c, fl);
    }
    for (k = 0; k < RANDOM_CASES; k++) {
        unsigned st = (unsigned)(rnd() % width);
        unsigned len = 1u + (unsigned)(rnd() % (width - st));
        uint64_t c = (uint64_t)st | ((uint64_t)len << 8) | (rnd() & ~0xffffull);
        uint64_t s1 = rnd(), dst = rnd(), fl = rnd();
        check_reg("BEXTR", ISA_BEXTR, width, real, dst, s1, c, fl);
    }
    report("BEXTR", width, start);
}

/* ---- MOVBE ---- */
static uint64_t bytes_as_u64(const unsigned char *p, unsigned width) {
    uint64_t v = 0;
    memcpy(&v, p, width / 8);
    return v;
}

static void fill16(unsigned char *b) {
    int j;
    for (j = 0; j < 16; j += 8) {
        uint64_t r = rnd();
        memcpy(b + j, &r, 8);
    }
}

/* Load: the memory operand is at [rsi] (base = gpr 6); dst = gpr 0. */
static void check_load(uint8_t width, RealLoadOp real, uint64_t dst, const unsigned char *mem) {
    IsaSite site;
    IsaRegs regs;
    uint64_t before[16], exp;
    int i;

    memset(&site, 0, sizeof site);
    site.kind = ISA_MOVBE_LOAD;
    site.width = width;
    site.dst = 0;
    site.src1 = ISA_NO_REG;
    site.src2 = ISA_NO_REG;
    site.mem_slot = ISA_MEM_SRC;
    site.base = 6;
    site.index = ISA_NO_REG;
    site.scale = 0;
    site.disp = 0;

    memset(&regs, 0, sizeof regs);
    for (i = 0; i < 16; i++) regs.gpr[i] = rnd();
    regs.gpr[0] = dst;
    regs.gpr[6] = (uint64_t)(uintptr_t)mem;
    regs.rflags = rnd() & FLAG_BITS;
    memcpy(before, regs.gpr, sizeof before);

    exp = real(dst, mem);
    isa_emulate(&site, &regs);
    g_cases++;

    if (regs.gpr[0] != exp)
        die_case("result", "MOVBE load", width, bytes_as_u64(mem, width), 0, dst, 0, exp, regs.gpr[0], 0, 0, 0);
    for (i = 1; i < 16; i++)
        if (regs.gpr[i] != before[i])
            die_case("other register changed", "MOVBE load", width, bytes_as_u64(mem, width), 0, dst, 0, before[i],
                     regs.gpr[i], 0, 0, 0);
}

/* Store: value in gpr 1; the memory operand is at [rsi] (base = gpr 6). Both buffers start from the same pattern. */
static void check_store(uint8_t width, RealStoreOp real, uint64_t value, const unsigned char *pattern) {
    unsigned char rbuf[16], ebuf[16];
    IsaSite site;
    IsaRegs regs;
    uint64_t before[16];
    int i;

    memcpy(rbuf, pattern, 16);
    memcpy(ebuf, pattern, 16);

    memset(&site, 0, sizeof site);
    site.kind = ISA_MOVBE_STORE;
    site.width = width;
    site.dst = ISA_NO_REG;
    site.src1 = 1;
    site.src2 = ISA_NO_REG;
    site.mem_slot = ISA_MEM_DST;
    site.base = 6;
    site.index = ISA_NO_REG;
    site.scale = 0;
    site.disp = 0;

    memset(&regs, 0, sizeof regs);
    for (i = 0; i < 16; i++) regs.gpr[i] = rnd();
    regs.gpr[1] = value;
    regs.gpr[6] = (uint64_t)(uintptr_t)ebuf;
    regs.rflags = rnd() & FLAG_BITS;
    memcpy(before, regs.gpr, sizeof before);

    real(value, rbuf);
    isa_emulate(&site, &regs);
    g_cases++;

    if (memcmp(rbuf, ebuf, 16) != 0)
        die_case("memory", "MOVBE store", width, value, 0, 0, 0, bytes_as_u64(rbuf, width),
                 bytes_as_u64(ebuf, width), 0, 0, 0);
    for (i = 0; i < 16; i++)
        if (regs.gpr[i] != before[i])
            die_case("other register changed", "MOVBE store", width, value, 0, 0, 0, before[i], regs.gpr[i], 0, 0,
                     0);
}

static void group_load(uint8_t width, RealLoadOp real) {
    unsigned long long start = g_cases;
    unsigned char buf[16];
    size_t i;
    unsigned k;
    for (i = 0; i < NEDGES; i++) {
        uint64_t dst = rnd();
        fill16(buf);
        memcpy(buf, &edges[i], 8);
        check_load(width, real, dst, buf);
    }
    for (k = 0; k < RANDOM_CASES; k++) {
        uint64_t dst = rnd();
        uint64_t v = (k & 1) ? rnd() : rnd_skew();
        fill16(buf);
        memcpy(buf, &v, 8);
        check_load(width, real, dst, buf);
    }
    report("MOVBE ld", width, start);
}

static void group_store(uint8_t width, RealStoreOp real) {
    unsigned long long start = g_cases;
    unsigned char pat[16];
    size_t i;
    unsigned k;
    for (i = 0; i < NEDGES; i++) {
        fill16(pat);
        check_store(width, real, edges[i], pat);
    }
    for (k = 0; k < RANDOM_CASES; k++) {
        uint64_t v = (k & 1) ? rnd() : rnd_skew();
        fill16(pat);
        check_store(width, real, v, pat);
    }
    report("MOVBE st", width, start);
}

/* ---- VCVTPH2PS ---- */
static void vcvt_fail(uint32_t h, uint64_t in, int lane, uint32_t expected, uint32_t actual) {
    printf("MISMATCH (result): VCVTPH2PS lane %d half 0x%04x xmm_in=0x%016llx\n", lane, h,
           (unsigned long long)in);
    printf("  expected single bits=0x%08x\n", expected);
    printf("  actual   single bits=0x%08x\n", actual);
    exit(1);
}

static void group_vcvt(void) {
    unsigned long long start = g_cases;
    uint32_t h;
    for (h = 0; h < 65536; h++) {
        uint64_t in = 0;
        uint32_t real[4], emu[4];
        IsaSite site;
        IsaRegs regs;
        int i;

        /* Lane i holds h ^ (i * 0x1111). Each XOR is a bijection on 16 bits, so every lane sees every half value. */
        for (i = 0; i < 4; i++) {
            uint64_t lane = (uint64_t)(h ^ (uint32_t)(i * 0x1111u));
            in |= lane << (16 * i);
        }
        real_vcvtph2ps(in, real);

        memset(&site, 0, sizeof site);
        site.kind = ISA_VCVTPH2PS;
        site.width = 32;
        site.dst = 0;
        site.src1 = 1;
        site.src2 = 2;
        site.base = ISA_NO_REG;
        site.index = ISA_NO_REG;

        memset(&regs, 0, sizeof regs);
        regs.xmm_in[0] = in;
        isa_emulate(&site, &regs);

        emu[0] = (uint32_t)regs.xmm_out[0];
        emu[1] = (uint32_t)(regs.xmm_out[0] >> 32);
        emu[2] = (uint32_t)regs.xmm_out[1];
        emu[3] = (uint32_t)(regs.xmm_out[1] >> 32);
        for (i = 0; i < 4; i++) {
            g_cases++;
            if (emu[i] != real[i]) vcvt_fail(h, in, i, real[i], emu[i]);
        }
    }
    report("VCVTPH2PS", 128, start);
}

/* ---- main ---- */
static int cpu_has_isa(void) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    int bmi1, movbe, f16c, lzcnt;
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return 0;
    bmi1 = (b >> 3) & 1;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return 0;
    movbe = (c >> 22) & 1;
    f16c = (c >> 29) & 1;
    if (!__get_cpuid(0x80000001u, &a, &b, &c, &d)) return 0;
    lzcnt = (c >> 5) & 1;
    return bmi1 && movbe && f16c && lzcnt;
}

int main(void) {
    unsigned int saved_mxcsr = 0, default_mxcsr = MXCSR_DEFAULT;

    if (!cpu_has_isa()) {
        printf("SKIP: CPU lacks BMI1, LZCNT, MOVBE or F16C (CPUID)\n");
        return 0;
    }

    __asm__ volatile("stmxcsr %0" : "=m"(saved_mxcsr));
    __asm__ volatile("ldmxcsr %0" : : "m"(default_mxcsr));

    group_reg("ANDN", ISA_ANDN, 32, real_andn32, 1);
    group_reg("ANDN", ISA_ANDN, 64, real_andn64, 1);
    group_bextr(32, real_bextr32);
    group_bextr(64, real_bextr64);
    group_reg("BLSI", ISA_BLSI, 32, real_blsi32, 0);
    group_reg("BLSI", ISA_BLSI, 64, real_blsi64, 0);
    group_reg("BLSMSK", ISA_BLSMSK, 32, real_blsmsk32, 0);
    group_reg("BLSMSK", ISA_BLSMSK, 64, real_blsmsk64, 0);
    group_reg("BLSR", ISA_BLSR, 32, real_blsr32, 0);
    group_reg("BLSR", ISA_BLSR, 64, real_blsr64, 0);
    group_reg("LZCNT", ISA_LZCNT, 16, real_lzcnt16, 0);
    group_reg("LZCNT", ISA_LZCNT, 32, real_lzcnt32, 0);
    group_reg("LZCNT", ISA_LZCNT, 64, real_lzcnt64, 0);
    group_reg("TZCNT", ISA_TZCNT, 16, real_tzcnt16, 0);
    group_reg("TZCNT", ISA_TZCNT, 32, real_tzcnt32, 0);
    group_reg("TZCNT", ISA_TZCNT, 64, real_tzcnt64, 0);
    group_load(16, real_movbe_load16);
    group_load(32, real_movbe_load32);
    group_load(64, real_movbe_load64);
    group_store(16, real_movbe_store16);
    group_store(32, real_movbe_store32);
    group_store(64, real_movbe_store64);
    group_vcvt();

    __asm__ volatile("ldmxcsr %0" : : "m"(saved_mxcsr));
    printf("OK %llu cases\n", g_cases);
    return 0;
}
