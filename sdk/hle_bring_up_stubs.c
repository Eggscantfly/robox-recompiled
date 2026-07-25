// sdk/hle_bring_up_stubs.c -- No-op HLE implementations referenced by the
// generated code but not yet covered by os.c / video.c / peripherals.c /
// rt.c. Keeping these as harmless stubs lets the link step succeed during
// bring-up; replace the body of any individual hle_* with real behavior
// when its absence becomes the actual blocker.
//
// All of these are SDK leaves the recompiler maps to via hle_name_map.py.
// Cache / LC ops, font loaders, message-queue / semaphore primitives,
// abort / exit handlers -- none are load-bearing for a from-cold boot.

#include "hle.h"
#include <SDL2/SDL.h>

#include <stdio.h>
#include <stdlib.h>

// ---- Cache management. We don't model PPC L1/L2 caches on the host, so
// every range op is a no-op. Real games call these constantly around DMA.
void hle_DCFlushRange      (void) { HLE_RET(0); }
void hle_DCInvalidateRange (void) { HLE_RET(0); }
void hle_DCStoreRange      (void) { HLE_RET(0); }
void hle_ICInvalidateRange (void) { HLE_RET(0); }
void hle_ICFlashInvalidate (void) { HLE_RET(0); }

// ---- Locked-cache config. No host equivalent; report not enabled.
void hle_LCEnable          (void) { HLE_RET(0); }
void hle_LCDisable         (void) { HLE_RET(0); }

// ---- Font loader. Returning 0 ("no font loaded") is fine for bring-up;
// the only consumer is OSPanic / OSReport text rendering, which we cover
// via host stdio in os.c.
void hle_OSInitFont        (void) { HLE_RET(0); }
void hle_OSLoadFont        (void) { HLE_RET(0); }
void hle_OSGetFontTexel    (void) { HLE_RET(0); }
void hle_OSGetFontEncode   (void) { HLE_RET(0); }
void hle_OSSetFontEncode   (void) { HLE_RET(0); }

// Message queues and semaphores are now implemented in os.c.

// ---- Sleep on guest ticks. The guest TBR is fake; just spin-yield.
void hle_OSSleepTicks(void) {
    /* ticks -> ms (Wii timebase ~60.75 MHz => ticks/60750 = ms). Sleep for
     * real and RELEASE the ppc lock so other guest threads (main loop!)
     * keep running -- the splash-screen thread sleeps between logos. */
    uint64_t ticks = ((uint64_t)HLE_ARG_U32(0) << 32) | HLE_ARG_U32(1);
    uint32_t ms = (uint32_t)(ticks / 60750ull);
    if (ms > 5000) ms = 5000;
    if (ms) {
        // ppc_lock_acquire/release are declared in hle.h.
        // Another guest thread runs (and clobbers g_cpu) while we sleep with
        // the lock released — snapshot/restore our context around the wait.
        PPCContext saved_ctx = g_cpu;
        ppc_lock_release();
        SDL_Delay(ms);
        ppc_lock_acquire();
        g_cpu = saved_ctx;
    }
    HLE_RET(0);
}

// ---- Boot ROM read. Caller wants the IPL header bytes; return zero so
// the consistency-check paths take the "unknown ROM" branch rather than
// trusting fabricated content.
void hle___OSReadROM       (void) { HLE_RET(0); }

// ---- Halt. Game called PPCHalt -- on a console this stops the CPU.
// On the host, fall through to exit so we don't spin forever.
void hle_PPCHalt           (void) {
    fprintf(stderr, "[hle] PPCHalt -- guest halted\n");
    exit(0);
}

// ---- C runtime termination paths. Generated code reaches these from the
// CW EH unwinder + libc stubs. exit/abort hand off to the host runtime;
// the throw / terminate handlers should never actually fire (they imply
// uncaught C++ exception in PPC code we recompiled), but the symbols
// must exist so dependent translation units link.
void hle_exit              (void) {
    fprintf(stderr, "[hle] guest exit(%d) called  lr=0x%08x\n",
            (int) HLE_ARG_U32(0), g_cpu.lr);
    // Robox: exit(1) from luaD_throw's no-handler branch (0x80036e3c) is a
    // Lua panic. r31 = lua_State; dump the error object from the stack top
    // so the failing script/line is visible. Try the float-build TValue
    // (8 bytes) then the double-build (16 bytes) layout.
    if (g_cpu.lr >= 0x80036ce4u && g_cpu.lr <= 0x80036e40u) {
        uint32_t L = g_cpu.gpr[31];
        if (L >= 0x80003000u && L < 0x94000000u) {
            uint32_t top = MEM_R32(L + 0x8);
            for (int layout = 0; layout < 2 && top > 16; ++layout) {
                uint32_t sz    = layout ? 16 : 8;
                uint32_t value = MEM_R32(top - sz);
                uint32_t tt    = MEM_R32(top - sz + (layout ? 8 : 4));
                if (tt == 4 && value >= 0x80003000u && value < 0x94000000u) {
                    const char *s = (const char *)ppc_host_ptr(value + 0x10);
                    if (s) {
                        fprintf(stderr, "[lua-panic] %.*s\n", 512, s);
                        extern void robox_malloc_dump(void);
                        robox_malloc_dump();
                        { extern void robox_expheap_dump(void);
                          robox_expheap_dump(); }
                        // guest backtrace via the r1 frame chain
                        {
                            uint32_t sp = g_cpu.gpr[1];
                            fprintf(stderr, "[lua-panic] guest bt:");
                            for (int fi = 0; fi < 12 && sp >= 0x80003000u && sp < 0x817f0000u; ++fi) {
                                uint32_t up = MEM_R32(sp);
                                uint32_t lr = (up >= 0x80003000u) ? MEM_R32(up + 4) : 0;
                                fprintf(stderr, " %08x", lr);
                                if (up <= sp) break;
                                sp = up;
                            }
                            fprintf(stderr, "\n");
                        }
                        break;
                    }
                }
            }
        }
    }
    fflush(stderr);
    exit((int) HLE_ARG_U32(0));
}
/* The guest's own abort(). This used to be a bare abort() with no output at
 * all, which made a guest-initiated abort completely undiagnosable -- the
 * process just vanished with a SIGABRT and no reason. That is exactly what
 * happened on the first Android run. Log who called it and dump the guest
 * backtrace before going down. */
void hle_abort(void) {
    fprintf(stderr, "[hle] guest called abort() -- lr=0x%08x sp=0x%08x\n",
            g_cpu.lr, g_cpu.gpr[1]);
    fprintf(stderr, "      r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x\n",
            g_cpu.gpr[3], g_cpu.gpr[4], g_cpu.gpr[5],
            g_cpu.gpr[6], g_cpu.gpr[7]);
    /* Walk the guest r1 frame chain: on PPC each frame's first word is the
     * caller's stack pointer and the LR is saved at +4. */
    uint32_t sp = g_cpu.gpr[1];
    for (int depth = 0; depth < 16 && sp >= 0x80000000u && sp < 0x81800000u; ++depth) {
        uint32_t next = MEM_R32(sp);
        uint32_t lr   = MEM_R32(sp + 4);
        if (!lr) break;
        fprintf(stderr, "      #%-2d lr=0x%08x sp=0x%08x\n", depth, lr, sp);
        if (next <= sp) break;      /* not ascending -> chain is bogus, stop */
        sp = next;
    }
    /* Scan registers + the top of the stack for pointers to ASCII strings --
     * the LyN engine's assert/panic leaves the message text (e.g. a Lua error
     * like "attempt to call a nil value") behind in a register or a stack slot.
     * Finding it turns "guest called abort()" into a named failure. */
    fprintf(stderr, "[hle] abort message scan:\n");
    for (int i = 3; i <= 12; ++i) {
        uint32_t v = g_cpu.gpr[i];
        if (v < 0x80000000u || v >= 0x94000000u) continue;
        char buf[121]; int n = 0, ok = 0;
        for (; n < 120; ++n) { uint8_t c = MEM_R8(v + n); if (!c) break;
            buf[n] = (c >= 0x20 && c < 0x7f) ? (char)c : '.'; if (c >= 0x20 && c < 0x7f) ok++; }
        buf[n] = 0;
        if (n >= 5 && ok * 4 >= n * 3) fprintf(stderr, "      r%-2d 0x%08x -> \"%s\"\n", i, v, buf);
    }
    for (int off = 0; off < 0x800; off += 4) {
        uint32_t v = MEM_R32(g_cpu.gpr[1] + off);
        if (v < 0x80000000u || v >= 0x94000000u) continue;
        char buf[161]; int n = 0, ok = 0;
        for (; n < 160; ++n) { uint8_t c = MEM_R8(v + n); if (!c) break;
            buf[n] = (c >= 0x20 && c < 0x7f) ? (char)c : '.'; if (c >= 0x20 && c < 0x7f) ok++; }
        buf[n] = 0;
        /* Keep anything that looks like a message OR a ".lua" source path. */
        int is_lua = 0; for (int k = 0; k + 4 <= n; ++k)
            if (buf[k]=='.'&&buf[k+1]=='l'&&buf[k+2]=='u'&&buf[k+3]=='a') { is_lua = 1; break; }
        if ((n >= 8 && ok * 8 >= n * 7) || is_lua)
            fprintf(stderr, "      [sp+0x%03x] 0x%08x -> \"%s\"\n", off, v, buf);
    }
    fflush(stderr);
    extern void robox_prof_crash_dump(const char *reason);
    robox_prof_crash_dump("guest called abort()");
    abort();
}
void hle___throw(void) {
    fprintf(stderr, "[hle] __throw reached -- uncaught PPC exception\n");
    abort();
}
void hle_std_terminate     (void) {
    fprintf(stderr, "[hle] std::terminate reached\n");
    abort();
}
void hle_ExPPC_ThrowHandler(void) {
    fprintf(stderr, "[hle] ExPPC_ThrowHandler reached\n");
    abort();
}
