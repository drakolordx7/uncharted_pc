/* SPDX-License-Identifier: GPL-2.0-or-later
 * TEMPORARY diagnostic: hardware data breakpoints on two guest addresses (4 bytes each), logging which
 * instructions touch them (guest return addresses on the stack too when the instruction is in the host).
 * Windows only; BB_STICK_WATCH turns it on. */
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>

#define GUEST_SIZE 0xa00000ull
static volatile int enabled;
static uint64_t image_base, watched[2];
static struct { uint64_t rip; unsigned long count; } seen[2][96];

static LONG CALLBACK veh(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode!=EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c=ep->ContextRecord;
    if (!(c->Dr6 & 0xf)) return EXCEPTION_CONTINUE_SEARCH;
    const unsigned bits=(unsigned)(c->Dr6 & 0xf);
    c->Dr6=0;
    if (enabled) {
        const int w=(bits&1) ? 0 : 1;
        const uint64_t rip=(uint64_t)c->Rip;
        int i=0;
        for (;i<96 && seen[w][i].rip && seen[w][i].rip!=rip;++i) {}
        if (i<96) {
            if (!seen[w][i].rip) {
                seen[w][i].rip=rip;
                const int guest=rip>=image_base && rip<image_base+GUEST_SIZE;
                printf("Watch%d: first access by %s, next instruction at image+%#llx\n",w,guest ? "guest" : "HOST",
                       (unsigned long long)(rip-image_base));
                {   /* the guest return addresses on the stack, to find the callers */
                    const uint64_t *sp=(const uint64_t *)(uintptr_t)c->Rsp;
                    printf("Watch%d: callers:",w);
                    int shown=0;
                    for (int k=0;k<400 && shown<14;++k) {
                        uint64_t v; if (IsBadReadPtr(sp+k,8)) break; v=sp[k];
                        if (v>=image_base+0x1000 && v<image_base+0x800000) { printf(" image+%#llx",(unsigned long long)(v-image_base)); ++shown; }
                    }
                    printf("\n");
                }
            }
            ++seen[w][i].count;
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI apply_thread(LPVOID unused) {
    (void)unused;
    const DWORD self=GetCurrentThreadId(), pid=GetCurrentProcessId();
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if (snap==INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te; te.dwSize=sizeof(te);
    int count=0;
    for (BOOL ok=Thread32First(snap,&te);ok;ok=Thread32Next(snap,&te)) {
        if (te.th32OwnerProcessID!=pid || te.th32ThreadID==self) continue;
        HANDLE th=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th)!=(DWORD)-1) {
            CONTEXT ctx; ctx.ContextFlags=CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th,&ctx)) {
                ctx.Dr0=(DWORD64)watched[0];
                ctx.Dr1=(DWORD64)watched[1];
                ctx.Dr7=getenv("BB_WATCH_WRITE") ? 0xDD0005 : 0xFF0005; /* L0 L1, 4 bytes; read/write, or writes only with BB_WATCH_WRITE */
                if (SetThreadContext(th,&ctx)) ++count;
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
    printf("Watch: breakpoints set on %d threads (%#llx, %#llx)\n",count,(unsigned long long)watched[0],(unsigned long long)watched[1]);
    fflush(stdout);
    return 0;
}

void stickwatch_arm(uint64_t a0, uint64_t a1, uint64_t image) {
    static int installed, done;
    if (done || !getenv("BB_STICK_WATCH")) return;
    image_base=image;
    if (!installed) { AddVectoredExceptionHandler(1,veh); installed=1; }
    watched[0]=a0; watched[1]=a1;
    done=1;
    HANDLE t=CreateThread(NULL,0,apply_thread,NULL,0,NULL);
    if (t) CloseHandle(t);
}
void stickwatch_enable(int on) { enabled=on; }
void stickwatch_summary(void) {
    for (int w=0;w<2;++w) for (int i=0;i<96 && seen[w][i].rip;++i)
        printf("WatchSummary%d: image+%#llx accessed %lu times\n",w,(unsigned long long)(seen[w][i].rip-image_base),seen[w][i].count);
    fflush(stdout);
}
#else
void stickwatch_arm(uint64_t a0, uint64_t a1, uint64_t image) { (void)a0; (void)a1; (void)image; }
void stickwatch_enable(int on) { (void)on; }
void stickwatch_summary(void) {}
#endif