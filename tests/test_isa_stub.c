/* The rewrite stubs and the int3 trap (isa_rewrite.c) against the real instructions: small code
 * snippets run natively and rewritten (jmp to a stub, jmp with the next instruction moved, int3),
 * from the same random registers, flags and memory; results, the flags the instruction defines, all
 * YMM registers and the memory must be identical. Windows only (the trap is a vectored handler).
 * Needs a CPU with BMI1, LZCNT, MOVBE, F16C and AVX (skips otherwise).
 * clang -std=c11 -O1 -Isrc tests/test_isa_stub.c src/isa_rewrite.c out/isa_emulate.o */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cpuid.h>
#include "isa_rewrite.h"
#ifndef _WIN32
int main(void) { puts("SKIP (Windows only)"); return 0; }
#else
#include <windows.h>

/* st: rax rcx rdx rflags (4 qwords), 4 pad, then ymm0-3 and ymm8-11 (32 bytes each, from byte 64). */
void __attribute__((sysv_abi)) run_snippet(const void *code, uint64_t *st);
__asm__(".text\n.globl run_snippet\nrun_snippet:\n"
        " push %rbx\n push %rbp\n push %r12\n push %r13\n push %r14\n push %r15\n"
        " mov %rsi,%rbx\n mov %rdi,%r12\n"
        " vmovups 64(%rbx),%ymm0\n vmovups 96(%rbx),%ymm1\n vmovups 128(%rbx),%ymm2\n vmovups 160(%rbx),%ymm3\n"
        " vmovups 192(%rbx),%ymm8\n vmovups 224(%rbx),%ymm9\n vmovups 256(%rbx),%ymm10\n vmovups 288(%rbx),%ymm11\n"
        " pushq 24(%rbx)\n popfq\n"
        " mov (%rbx),%rax\n mov 8(%rbx),%rcx\n mov 16(%rbx),%rdx\n"
        " call *%r12\n"
        " pushfq\n popq 24(%rbx)\n"
        " mov %rax,(%rbx)\n mov %rcx,8(%rbx)\n mov %rdx,16(%rbx)\n"
        " vmovups %ymm0,64(%rbx)\n vmovups %ymm1,96(%rbx)\n vmovups %ymm2,128(%rbx)\n vmovups %ymm3,160(%rbx)\n"
        " vmovups %ymm8,192(%rbx)\n vmovups %ymm9,224(%rbx)\n vmovups %ymm10,256(%rbx)\n vmovups %ymm11,288(%rbx)\n"
        " vzeroupper\n"
        " pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbp\n pop %rbx\n ret\n");

enum { ARITH = 0x8d5 };   /* CF PF AF ZF SF OF */
typedef struct {
    const char *name;
    uint8_t code[16]; int code_length;
    IsaSite site;
    uint64_t flag_mask;
    int memory;           /* rax points at a 16-byte buffer */
} Case;

static IsaTable *vh_table;
static LONG CALLBACK trap(EXCEPTION_POINTERS *info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT || !vh_table) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = info->ContextRecord;
    IsaRegs regs;
    regs.rflags = c->EFlags;
    memcpy(regs.gpr, &c->Rax, sizeof(regs.gpr));
    uintptr_t next;
    if (!isa_rewrite_trap(vh_table, (uintptr_t)info->ExceptionRecord->ExceptionAddress, &regs, &next)) return EXCEPTION_CONTINUE_SEARCH;
    memcpy(&c->Rax, regs.gpr, sizeof(regs.gpr));
    c->EFlags = (DWORD)regs.rflags;
    c->Rip = next;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static uint64_t edge(void) {
    static const uint64_t e[] = {0, 1, 2, 0x80, 0xff, 0x7fffffffull, 0x80000000ull, 0xffffffffull, 0x8000000000000000ull, ~0ull, 0x100};
    return rnd() % 3 ? rnd() : e[rnd() % (sizeof e / sizeof *e)];
}

static IsaSite site(uint8_t kind, uint8_t width, uint8_t dst, uint8_t src1, uint8_t src2, uint8_t mem_slot, uint8_t base, int length) {
    IsaSite s;
    memset(&s, 0, sizeof s);
    s.kind = kind; s.width = width; s.dst = dst; s.src1 = src1; s.src2 = src2; s.mem_slot = mem_slot;
    s.base = base; s.index = ISA_NO_REG; s.scale = 0; s.length = (uint8_t)length;
    return s;
}

int main(void) {
    unsigned a, b, c, d;
    __get_cpuid(1, &a, &b, &c, &d);
    unsigned leaf1 = c;
    __get_cpuid_count(7, 0, &a, &b, &c, &d);
    unsigned leaf7 = b;
    __get_cpuid(0x80000001, &a, &b, &c, &d);
    if (!((leaf1 >> 28 & 1) && (leaf1 >> 22 & 1) && (leaf1 >> 29 & 1) && (leaf7 >> 3 & 1) && (c >> 5 & 1))) { puts("SKIP"); return 0; }

    static const Case cases_template[] = {
        {"blsr ecx,ecx", {0xc4, 0xe2, 0x70, 0xf3, 0xc9}, 5, {0}, 0, 0},
        {"andn eax,ecx,edx", {0xc4, 0xe2, 0x70, 0xf2, 0xc2}, 5, {0}, 0, 0},
        {"bextr rax,rcx,rdx", {0xc4, 0xe2, 0xe8, 0xf7, 0xc1}, 5, {0}, 0, 0},
        {"tzcnt rax,rcx", {0xf3, 0x48, 0x0f, 0xbc, 0xc1}, 5, {0}, 0, 0},
        {"lzcnt ax,cx", {0x66, 0xf3, 0x0f, 0xbd, 0xc1}, 5, {0}, 0, 0},
        {"lzcnt eax,ecx; add eax,1 (moved)", {0xf3, 0x0f, 0xbd, 0xc1, 0x83, 0xc0, 0x01}, 7, {0}, ARITH, 0},
        {"lzcnt eax,ecx; jmp $+2 (int3)", {0xf3, 0x0f, 0xbd, 0xc1, 0xeb, 0x00}, 6, {0}, 0, 0},
        {"movbe [rax],ecx; nop (moved)", {0x0f, 0x38, 0xf1, 0x08, 0x90}, 5, {0}, 0, 1},
        {"movbe ecx,[rax]; nop (moved)", {0x0f, 0x38, 0xf0, 0x08, 0x90}, 5, {0}, 0, 1},
        {"movbe [rax],ecx; jmp $+2 (int3)", {0x0f, 0x38, 0xf1, 0x08, 0xeb, 0x00}, 6, {0}, 0, 1},
        {"vcvtph2ps xmm1,xmm2", {0xc4, 0xe2, 0x79, 0x13, 0xca}, 5, {0}, 0, 0},
        {"vcvtph2ps xmm9,xmm10", {0xc4, 0x42, 0x79, 0x13, 0xca}, 5, {0}, 0, 0},
    };
    enum { N = sizeof cases_template / sizeof *cases_template, CASE_STRIDE = 64 };
    Case cases[N];
    memcpy(cases, cases_template, sizeof cases);
    cases[0].site = site(ISA_BLSR, 32, 1, 1, ISA_NO_REG, 0, ISA_NO_REG, 5);
    cases[1].site = site(ISA_ANDN, 32, 0, 1, 2, 0, ISA_NO_REG, 5);
    cases[2].site = site(ISA_BEXTR, 64, 0, 1, 2, 0, ISA_NO_REG, 5);
    cases[3].site = site(ISA_TZCNT, 64, 0, 1, ISA_NO_REG, 0, ISA_NO_REG, 5);
    cases[4].site = site(ISA_LZCNT, 16, 0, 1, ISA_NO_REG, 0, ISA_NO_REG, 5);
    cases[5].site = site(ISA_LZCNT, 32, 0, 1, ISA_NO_REG, 0, ISA_NO_REG, 4);
    cases[6].site = site(ISA_LZCNT, 32, 0, 1, ISA_NO_REG, 0, ISA_NO_REG, 4);
    cases[7].site = site(ISA_MOVBE_STORE, 32, ISA_NO_REG, 1, ISA_NO_REG, ISA_MEM_DST, 0, 4);
    cases[8].site = site(ISA_MOVBE_LOAD, 32, 1, ISA_NO_REG, ISA_NO_REG, ISA_MEM_SRC, 0, 4);
    cases[9].site = site(ISA_MOVBE_STORE, 32, ISA_NO_REG, 1, ISA_NO_REG, ISA_MEM_DST, 0, 4);
    cases[10].site = site(ISA_VCVTPH2PS, 0, 1, 2, ISA_NO_REG, 0, ISA_NO_REG, 5);
    cases[11].site = site(ISA_VCVTPH2PS, 0, 9, 10, ISA_NO_REG, 0, ISA_NO_REG, 5);
    for (int i = 0; i < N; i++) {
        Case *k = &cases[i];
        k->site.offset = 0x1000 + (uint64_t)i * CASE_STRIDE;
        memcpy(k->site.original, k->code, k->site.length);
        k->site.mode = ISA_MODE_JMP;
        if (k->site.length == 4) {   /* 4-byte instruction: the next instruction moves into the stub, or an int3 */
            if (strstr(k->name, "int3")) k->site.mode = ISA_MODE_INT3;
            else {
                k->site.mode = ISA_MODE_JMP_RELOCATE;
                k->site.relocated_length = (uint8_t)(k->code_length - 4);
                memcpy(k->site.relocated, k->code + 4, k->site.relocated_length);
            }
        }
        k->flag_mask = k->flag_mask ? k->flag_mask : isa_defined_flags(k->site.kind);
    }

    enum { IMAGE = 0x10000, STUBS = 0x10000 };
    unsigned char *image = VirtualAlloc(NULL, IMAGE + STUBS, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    unsigned char *native = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!image || !native) { puts("FAIL: no memory"); return 1; }
    memset(image, 0xcc, IMAGE);
    memset(native, 0xcc, 0x10000);
    IsaTable t;
    memset(&t, 0, sizeof t);
    IsaSite sites[N];
    uint64_t hits[N] = {0};
    for (int i = 0; i < N; i++) {
        sites[i] = cases[i].site;
        memcpy(image + sites[i].offset, cases[i].code, cases[i].code_length);
        image[sites[i].offset + cases[i].code_length] = 0xc3;
        memcpy(native + sites[i].offset, cases[i].code, cases[i].code_length);
        native[sites[i].offset + cases[i].code_length] = 0xc3;
    }
    t.sites = sites; t.hits = hits; t.count = N; t.image_size = IMAGE;
    vh_table = &t;
    AddVectoredExceptionHandler(1, trap);
    IsaReport r = isa_rewrite_apply(&t, image, image + IMAGE, ~0u);
    printf("applied: %u rewritten (%u jmp, %u relocated, %u int3), %u skipped\n", r.rewritten, r.jmp, r.relocated, r.int3, r.skipped);
    if (r.rewritten != N || r.skipped || r.int3 != 2 || r.relocated != 3) { puts("FAIL: unexpected rewrite report"); return 1; }
    DWORD old;
    VirtualProtect(image + IMAGE, STUBS, PAGE_EXECUTE_READ, &old);

    long total = 0;
    for (int i = 0; i < N; i++) {
        const Case *k = &cases[i];
        for (int round = 0; round < 3000; round++) {
            uint64_t st_a[80] __attribute__((aligned(32))), st_b[80] __attribute__((aligned(32)));
            unsigned char mem_a[16] = {0}, mem_b[16] = {0};
            st_a[0] = edge(); st_a[1] = edge(); st_a[2] = edge();
            st_a[3] = rnd() & ARITH;
            for (int j = 4; j < 80; j++) st_a[j] = rnd();
            for (int j = 0; j < 16; j++) mem_a[j] = mem_b[j] = (unsigned char)rnd();
            memcpy(st_b, st_a, sizeof st_a);
            st_a[0] = (uint64_t)(uintptr_t)mem_a;
            st_b[0] = (uint64_t)(uintptr_t)mem_b;
            if (!k->memory) st_b[0] = st_a[0] = edge();
            run_snippet(native + k->site.offset, st_a);
            run_snippet(image + k->site.offset, st_b);
            int bad = 0;
            if (!k->memory && st_a[0] != st_b[0]) bad = 1;
            if (st_a[1] != st_b[1] || st_a[2] != st_b[2]) bad = 1;
            if ((st_a[3] ^ st_b[3]) & k->flag_mask) bad = 1;
            if (memcmp(st_a + 8, st_b + 8, 256) || memcmp(mem_a, mem_b, 16)) bad = 1;
            if (bad) {
                printf("FAIL %s round %d: rax %llx/%llx rcx %llx/%llx rdx %llx/%llx flags %llx/%llx (mask %llx)\n", k->name, round,
                       (unsigned long long)st_a[0], (unsigned long long)st_b[0], (unsigned long long)st_a[1], (unsigned long long)st_b[1],
                       (unsigned long long)st_a[2], (unsigned long long)st_b[2], (unsigned long long)st_a[3], (unsigned long long)st_b[3],
                       (unsigned long long)k->flag_mask);
                return 1;
            }
            total++;
        }
        printf("%-36s ok (stub runs %llu)\n", k->name, (unsigned long long)hits[i]);
    }
    printf("OK %ld runs\n", total);
    return 0;
}
#endif
