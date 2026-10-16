/* The games' display configuration: one init function per executable stores the screen size
 * (1920x1080), the aspect ratio (16:9 as a float) and the sizes derived from it into a global
 * settings block that the whole renderer reads. Rewriting the immediates of that function
 * makes the game allocate and render everything at another size.
 *
 * BB_GAME_RES=WxH   the game's render and display size (default: untouched, 1920x1080).
 *
 * The init function is found by its code, not by an offset, so every version of the three
 * executables is covered:  mov [r+8],1920  mov [r+0xc],1080 ... stores of 1920/1080 at
 * +0x18/+0x1c, +0x24/+0x28, +0x38/+0x3c, the aspect 0x3fe38e39 at +0x20/+0x2c, and the two
 * values derived from the size (1920/8 and 1080/4) at +0x40/+0x44. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put32(unsigned char *p, uint32_t v) { memcpy(p,&v,4); }
static uint32_t get32(const unsigned char *p) { uint32_t v; memcpy(&v,p,4); return v; }
static uint32_t float_bits(float f) { uint32_t v; memcpy(&v,&f,4); return v; }

/* One size block: `start` is the first store (mov [r+8],1920). Patches the stores in the next
 * `window` bytes that match the known (disp, immediate) pairs; returns how many it changed. */
static int patch_block(unsigned char *code, size_t start, size_t window, size_t size, uint32_t w, uint32_t h) {
    const unsigned char reg=code[start+1];
    int changed=0;
    uint32_t aspect=float_bits((float)w/(float)h);
    for (size_t i=start;i+7<=start+window && i+7<=size;++i) {
        if (code[i]==0xc7 && code[i+1]==reg) {            /* mov dword [r+disp8], imm32 */
            const unsigned disp=code[i+2];
            const uint32_t imm=get32(code+i+3);
            uint32_t value=imm;
            if (imm==1920 && (disp==0x08 || disp==0x18 || disp==0x24 || disp==0x38)) value=w;
            else if (imm==1080 && (disp==0x0c || disp==0x1c || disp==0x28 || disp==0x3c)) value=h;
            else if (imm==0x3fe38e39 && (disp==0x20 || disp==0x2c)) value=aspect;
            else if (imm==0xf0 && disp==0x40) value=(w+7)/8;
            else if (imm==0x10e && disp==0x44) value=(h+3)/4;
            if (value!=imm) { put32(code+i+3,value); ++changed; i+=6; }
        } else if (i+10<=size && code[i]==0x48 && code[i+1]==0xb8 && get32(code+i+2)==1920 && get32(code+i+6)==1080) {
            put32(code+i+2,w); put32(code+i+6,h); ++changed;   /* movabs rax, (1080<<32)|1920 -> [r+0x38] */
            i+=9;
        }
    }
    return changed;
}

/* The memory budget table of each executable (module data): entries {u32 id, u32 type, u64 size,
 * u64 alignment, char *name} of 32 bytes that end at id 0. The game checks that every type's entries add
 * up to a constant (type 2: the system direct memory, 4608 MiB; type 6: its GPU pool, which is
 * entry 3 of type 2). Entry 133 of type 6 (497 MiB; 561 in Uncharted 3) is the render target
 * heap ("Render Target Heap" in the game's allocation failures): it, the pool entry and the
 * total grow by `extra_mb` together. */
static int grow_entry(unsigned char *image, uint64_t image_size, uint32_t id, uint32_t type, uint64_t min_mb, uint64_t max_mb,
                      uint64_t extra_mb, const char *what) {
    int grown=0;
    for (uint64_t i=0;i+32<=image_size;i+=8) {
        if (get32(image+i)!=id || get32(image+i+4)!=type) continue;
        uint64_t size, alignment;
        memcpy(&size,image+i+8,8); memcpy(&alignment,image+i+16,8);
        if (alignment==0 || (alignment&(alignment-1)) || size<(min_mb<<20) || size>(max_mb<<20)) continue;
        size+=extra_mb<<20;
        memcpy(image+i+8,&size,8);
        printf("Display: %s at image offset %#llx grown by %llu MiB to %llu MiB\n",what,(unsigned long long)i,
               (unsigned long long)extra_mb,(unsigned long long)(size>>20));
        ++grown;
    }
    return grown;
}

static int grow_gpu_pool(unsigned char *image, uint64_t image_size, uint64_t extra_mb) {
    int grown=grow_entry(image,image_size,3,2,2000,4000,extra_mb,"GPU memory pool");
    grown+=grow_entry(image,image_size,133,6,300,1000,extra_mb,"render target heap");
    /* The game checks that the table adds up to its constant (movabs rdx, 0x120000000 = 4608 MiB
       in the memory validation function): the constant grows with it. */
    static const unsigned char total[10]={0x48,0xba,0,0,0,0x20,1,0,0,0};
    for (uint64_t i=0;i+sizeof(total)<=image_size;++i) {
        if (memcmp(image+i,total,sizeof(total))) continue;
        uint64_t value=0x120000000ull+(extra_mb<<20);
        memcpy(image+i+2,&value,8);
        printf("Display: memory total check at image offset %#llx now %llu MiB\n",(unsigned long long)i,(unsigned long long)(value>>20));
    }
    return grown;
}

void runtime_display_patch(unsigned char *image, uint64_t image_size) {
    const char *res=getenv("BB_GAME_RES");
    unsigned w=0, h=0;
    if (!res || sscanf(res,"%ux%u",&w,&h)!=2 || w<640 || h<360 || w>7680 || h>4320 || (w==1920 && h==1080)) return;
    w&=~1u; h&=~1u;
    const char *mem=getenv("BB_GAME_MEM_MB");
    const double ratio=(double)w*h/(1920.0*1080.0);
    uint64_t extra=mem && *mem ? strtoull(mem,NULL,10) : ratio>1.0 ? (uint64_t)((ratio-1.0)*1100.0)+256 : 0;
    if (extra) grow_gpu_pool(image,image_size,extra);   /* run.py raises BB_DMEM_MB by the same amount */
    size_t blocks=0, stores=0;
    /* mov dword [r+8],1920 ; mov dword [r+0xc],1080  (same base register) */
    for (uint64_t i=0;i+14<=image_size;++i) {
        if (image[i]!=0xc7 || (image[i+1]&0xf8)!=0x40 || image[i+2]!=0x08 || get32(image+i+3)!=1920) continue;
        if (image[i+7]!=0xc7 || image[i+8]!=image[i+1] || image[i+9]!=0x0c || get32(image+i+10)!=1080) continue;
        stores+=(size_t)patch_block(image,(size_t)i,0x1e0,(size_t)image_size,w,h);
        ++blocks;
    }
    printf("Display: BB_GAME_RES %ux%u: %zu configuration block(s), %zu stores changed\n",w,h,blocks,stores);
}
