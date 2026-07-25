// src/runtime.c -- Host-side implementation of the PPC recomp runtime.
//
// Responsibilities:
//   1. Allocate and zero the guest memory arrays (MEM1, MEM2, deadzone).
//   2. Translate guest VAs to host pointers.
//   3. Fan MMIO writes into sdk/peripherals.c (CP-FIFO in particular).
//   4. Provide indirect-call dispatch via g_func_table_addrs/ptrs.
//   5. Provide the diagnostic helpers (ppc_unimplemented, ppc_trace, etc.)
//      the generated code and sdk/ call into.
//
// Scaffolded by recompile_wii.py from templates/runtime.c.tmpl. Safe to
// edit: the recompiler emits this only when missing.

// clock_gettime / CLOCK_MONOTONIC visibility on glibc.
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

PPCContext g_cpu;

uint8_t *g_mem1;
uint8_t *g_mem2;
uint8_t  g_deadzone[PPC_DEADZONE_SIZE];

static int g_runtime_ready;

// Highest end-VA written into MEM1 by ppc_load_elf. Used by main to call
// os_set_arena_bounds() so the OSInit arena_lo lands past the loaded .bss.
static uint32_t g_elf_mem1_top;


// ---------------------------------------------------------------------------
// Guest memory / MMIO
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Wild-pointer telemetry.
//
// ppc_host_ptr never faults: an unmapped VA is folded into g_deadzone so the
// guest keeps running. That is what makes the port survivable, but it also
// means a dereference through a garbage pointer leaves NO trace -- the guest
// reads zeros and wanders off, and the eventual crash happens somewhere
// unrelated. Recording the cold path costs nothing (we already fell through
// four range checks to get here) and turns "it died somewhere" into "it read
// 0xdeadbeef from func_800ddd34".
//
// Hardware MMIO is excluded: the guest legitimately pokes VI/PI/AI/DI
// registers every frame and those are meant to land in the deadzone.
// ---------------------------------------------------------------------------
uint64_t g_deadzone_hits;                 // wild (non-MMIO) accesses
uint32_t g_wild_ring_va[PPC_WILD_RING];
uint32_t g_wild_ring_lr[PPC_WILD_RING];
uint32_t g_wild_ring_head;

static inline int va_is_hw_mmio(uint32_t va) {
    // Broadway MMIO aperture (CC00_0000..CD80_0000) plus the GC-style
    // physical alias at 0C00_xxxx that some SDK code still uses.
    return (va >= 0xCC000000u && va < 0xCD800000u) ||
           (va >= 0x0C000000u && va < 0x0D800000u);
}

void *ppc_host_ptr(uint32_t va) {
    // Wii memory map per Broadway BAT setup:
    //   0x8000_0000  MEM1 cached
    //   0xC000_0000  MEM1 uncached
    //   0x9000_0000  MEM2 cached
    //   0xD000_0000  MEM2 uncached
    // Each cached/uncached pair folds onto the same host buffer.
    if (va >= PPC_MEM1_BASE && va < PPC_MEM1_BASE + PPC_MEM1_SIZE)
        return g_mem1 + (va - PPC_MEM1_BASE);
    if (va >= PPC_MEM1_UNCACHED_BASE && va < PPC_MEM1_UNCACHED_BASE + PPC_MEM1_SIZE)
        return g_mem1 + (va - PPC_MEM1_UNCACHED_BASE);
    if (va >= PPC_MEM2_BASE && va < PPC_MEM2_BASE + PPC_MEM2_SIZE)
        return g_mem2 + (va - PPC_MEM2_BASE);
    if (va >= PPC_MEM2_UNCACHED_BASE && va < PPC_MEM2_UNCACHED_BASE + PPC_MEM2_SIZE)
        return g_mem2 + (va - PPC_MEM2_UNCACHED_BASE);
    if (!va_is_hw_mmio(va)) {
        uint32_t h = g_wild_ring_head++;
        g_wild_ring_va[h & (PPC_WILD_RING - 1)] = va;
        g_wild_ring_lr[h & (PPC_WILD_RING - 1)] = g_cpu.lr;
        ++g_deadzone_hits;
    }
    // MMIO / unmapped: steer to deadzone so reads produce zero and writes
    // don't AV. Only the low 16 bits of `va` matter here.
    return g_deadzone + (va & (PPC_DEADZONE_SIZE - 1));
}

// CP-FIFO mapped register. Real Broadway has it at 0xCC00_8000; games can
// write bytes/halfwords/words through that VA and we push them into the
// FIFO interpreter in sdk/peripherals.c.
//
// We intentionally don't decode other MMIO ranges here. If the guest pokes
// 0xCD00_xxxx (IPC / audio / DVD control), the deadzone swallows it and the
// dedicated hle_* stubs handle the observable side-effects.
#define CP_FIFO_VA   0xCC008000u

bool ppc_mmio_va(uint32_t va) {
    // The Wii GP write-gather pipe occupies the 64-byte range
    // 0xCC008000..0xCC00803F. Every write within that span -- regardless
    // of byte offset -- is delivered to the same GP FIFO by hardware.
    //
    // FOUNDATIONAL FIX: previously matched only the exact base address.
    // That dropped the second half of every paired-single store (psq_st)
    // to the FIFO, because psq_st writes 4 bytes at va and 4 bytes at
    // va+4: only va matched the predicate, va+4 silently fell through to
    // the host-RAM path (no backing -> dropped). Effect: 12 of every 33
    // bytes (~36%) of __GXSetProjection / GXLoadPosMtxImm / vertex-array
    // emission were lost, corrupting every projection matrix and every
    // paired-single vertex/matrix submission. Surfaced as the constant
    // "purple splash, no real geometry" rendering and the per-frame
    // 0x7f/0xf0/0x3f/0xc5 unknown-opcode flood (those bytes were the
    // next command's payload sliding into the LoadXF's reserved slot
    // because the LoadXF's intended bytes never arrived).
    //
    // No other MMIO subsystem (PI=0xCC003xxx, VI=0xCC002xxx, AI=0xCC006xxx,
    // EXI=0xCC006800, etc.) lives in 0xCC008000..0xCC00803F, so widening
    // this predicate doesn't risk routing unrelated writes to the FIFO.
    return (va & ~0x3Fu) == CP_FIFO_VA;
}

extern void gx_fifo_push(const uint8_t *bytes, int n);

void ppc_mmio_write(uint32_t va, const void *data, int nbytes) {
    if ((va & ~0x3Fu) == CP_FIFO_VA) {
        static uint64_t total_bytes;
        total_bytes += nbytes;
        /* Every 64 KB of FIFO traffic — that is several times per frame, with
         * an fflush each time. Debug only (RECOMP_GX_TRACE=1). */
        extern int recomp_gx_trace(void);
        if (recomp_gx_trace() &&
            (total_bytes == nbytes || (total_bytes % 65536u) < (uint64_t)nbytes)) {
            fprintf(stderr, "[cp-fifo] write: %d bytes (total %llu)\n",
                    nbytes, (unsigned long long)total_bytes);
            fflush(stderr);
        }
        gx_fifo_push((const uint8_t *)data, nbytes);
    }
}


// ---------------------------------------------------------------------------
// Indirect dispatch
// ---------------------------------------------------------------------------
//
// The table is sorted by construction (see generated func_table.c), so a
// binary search is correct -- but with ~11k entries that is ~14 dependent
// loads scattered over 44 KB, and it runs on EVERY indirect dispatch.
//
// The old single-entry cache only helped a callsite that repeats the same
// target back to back. The hottest dispatcher in the game is the bytecode
// interpreter's jump table (see ppc_jump_indirect), which cycles through a
// handful of DIFFERENT case targets -- so it missed the cache every time and
// paid the full search once per VM instruction.
//
// A direct-mapped cache indexed by the guest address fixes that. It stores an
// INDEX, not a function pointer, and every hit is re-validated against the
// authoritative address table. Two consequences, both deliberate:
//   * Race-free without atomics. Guest worker threads share this cache; a
//     torn or stale index simply fails validation and falls through to the
//     binary search, so the worst case is a slow lookup, never a wrong one.
//   * ppc_patch_func keeps working. Mods rewrite g_func_table_ptrs, and
//     because we re-read that array on every hit rather than caching the
//     pointer, a patch takes effect immediately.
#define PPC_LOOKUP_CACHE_SIZE 4096u          /* power of two */
static uint32_t g_lookup_cache[PPC_LOOKUP_CACHE_SIZE];

PPC_Func ppc_lookup_func(uint32_t addr) {
    if (g_func_table_count == 0) return NULL;

    // Guest code is 4-byte aligned, so bits 2+ carry all the entropy.
    uint32_t slot = (addr >> 2) & (PPC_LOOKUP_CACHE_SIZE - 1u);
    uint32_t idx  = g_lookup_cache[slot];
    if (idx < g_func_table_count && g_func_table_addrs[idx] == addr)
        return g_func_table_ptrs[idx];

    size_t lo = 0, hi = g_func_table_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint32_t m = g_func_table_addrs[mid];
        if (m == addr) {
            g_lookup_cache[slot] = (uint32_t)mid;
            return g_func_table_ptrs[mid];
        }
        if (m < addr) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

void ppc_patch_func(uint32_t addr, PPC_Func fn) {
    size_t lo = 0, hi = g_func_table_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint32_t m = g_func_table_addrs[mid];
        if (m == addr) { g_func_table_ptrs[mid] = fn; return; }
        if (m < addr) lo = mid + 1; else hi = mid;
    }
}

// Printing every unknown-address call makes the log useless fast. Keep a
// tiny seen-set so each bogus VA prints once.
#define UNKNOWN_CACHE 64
static uint32_t s_unknown_seen[UNKNOWN_CACHE];
static size_t   s_unknown_count;

static bool note_unknown_once(uint32_t addr) {
    for (size_t i = 0; i < s_unknown_count; ++i)
        if (s_unknown_seen[i] == addr) return false;
    if (s_unknown_count < UNKNOWN_CACHE)
        s_unknown_seen[s_unknown_count++] = addr;
    return true;
}

// Liveness heartbeat: every N indirect calls print a line so a stuck tight
// loop in pure PPC code still shows up in logs (otherwise stdout is silent
// for minutes at a time). 200k calls ≈ a handful of frames when running.
// Set PPC_HEARTBEAT_STRIDE=0 to silence entirely.
#ifndef PPC_HEARTBEAT_STRIDE
#define PPC_HEARTBEAT_STRIDE 200000u
#endif

// Recent indirect dispatches. The generated code assigns g_cpu.lr before every
// direct `bl`, so g_cpu.lr alone already pins the crash site -- but a vtable
// call chain is exactly what a C++ game crashes inside, and this ring makes the
// last 128 of them visible for free. Deliberately unlocked: guest worker
// threads racing on it produce a slightly interleaved history, which is a much
// better outcome than paying for atomics on every dispatch.
uint32_t g_ind_ring_target[PPC_IND_RING];
uint32_t g_ind_ring_lr    [PPC_IND_RING];
uint32_t g_ind_ring_head;

// ---------------------------------------------------------------------------
// `bctr` -- branch to CTR. A JUMP, not a call.
//
// Split out from ppc_call_indirect purely so the dispatch can be a guaranteed
// tail call: clang's musttail demands the caller and callee agree on parameter
// count, and every recompiled function is void(void). Taking the target from
// g_cpu.ctr instead of an argument makes this void(void) too, so both edges of
// the dispatch -- generated code -> here, and here -> the target -- can be
// real wasm `return_call`s.
//
// Why that matters: a jump-table dispatch LOOP (Robox's bytecode interpreter
// at 0x80051000 runs one `bctr` per VM instruction, each case tail-jumping
// back to the loop head) otherwise leaves a host frame behind per instruction
// executed. A 512 MB desktop stack hid that for years; the browser caps wasm
// frames by COUNT, so level loading died with "RangeError: Maximum call stack
// size exceeded" no matter how large -sSTACK_SIZE was. With both edges tail
// calls the loop runs at constant depth -- exactly as the hardware does it.
//
// No recursion guard here on purpose: this path cannot grow the stack, so
// there is nothing to guard against.
void ppc_jump_indirect(void) {
    uint32_t addr = g_cpu.ctr;
    {
        uint32_t h = g_ind_ring_head++;
        g_ind_ring_target[h & (PPC_IND_RING - 1)] = addr;
        g_ind_ring_lr    [h & (PPC_IND_RING - 1)] = g_cpu.lr;
    }
    PPC_Func fn = ppc_lookup_func(addr);
    if (fn) {
        PPC_TAIL return fn();
    }
    // Unknown target: rare, and handled identically to the call case. Not a
    // tail call, but a dispatch that is about to abort or fall back is by
    // definition not the hot loop.
    ppc_call_indirect(addr);
}

void ppc_call_indirect(uint32_t addr) {
    {
        uint32_t h = g_ind_ring_head++;
        g_ind_ring_target[h & (PPC_IND_RING - 1)] = addr;
        g_ind_ring_lr    [h & (PPC_IND_RING - 1)] = g_cpu.lr;
    }
    // Optional focused trace: PPC_INDIRECT_LR_FILTER=0xVA prints every
    // indirect call whose caller LR matches. Useful for "what did THAT
    // bctrl dispatch to?" without flooding the log.
    {
        static uint32_t filter_lr = 0; static int inited;
        if (!inited) {
            inited = 1;
            const char *e = getenv("PPC_INDIRECT_LR_FILTER");
            if (e && *e) filter_lr = (uint32_t)strtoul(e, NULL, 0);
        }
        if (filter_lr && (g_cpu.lr == filter_lr || g_cpu.lr == filter_lr - 4 || g_cpu.lr == filter_lr + 4)) {
            uint32_t saved_r3 = g_cpu.gpr[3];
            PPC_Func fn2 = ppc_lookup_func(addr);
            fprintf(stderr, "[ppc-filter] bctrl lr=0x%08x ctr=0x%08x -> %s (r3 pre=0x%08x)\n",
                    g_cpu.lr, addr, fn2 ? "real" : "UNKNOWN", saved_r3);
            fflush(stderr);
            if (fn2) fn2();
            else { fprintf(stderr, "[ppc-filter]   filter cannot resolve -- aborting\n"); abort(); }
            fprintf(stderr, "[ppc-filter]   -> r3 post=0x%08x\n", g_cpu.gpr[3]);
            fflush(stderr);
            return;
        }
    }
    // Optional firehose: RECOMP_TRACE_INDIRECT=1 logs every indirect call.
    // Invaluable for "which ctor/callback wrecked the state" hunts.
    {
        static int trace_ind = -1;
        if (trace_ind < 0) {
            const char *e = getenv("RECOMP_TRACE_INDIRECT");
            trace_ind = (e && e[0] && e[0] != '0');
        }
        if (trace_ind) {
            fprintf(stderr, "[ind] -> 0x%08x lr=0x%08x sp=0x%08x r3=0x%08x\n",
                    addr, g_cpu.lr, g_cpu.gpr[1], g_cpu.gpr[3]);
            fflush(stderr);
        }
    }
    /* INVESTIGATE (RGH-only, disabled for Robox): guest main-thread stack
     * overflow detector keyed to RGH's stack VAs — that range is ordinary
     * heap for other games. Gate on env so it can come back if needed. */
    if (getenv("RECOMP_RGH_STACKCHK")) {
        uint32_t sp = g_cpu.gpr[1];
        if (sp < 0x806c5ed8u && sp >= 0x806c0000u) {
            static unsigned s_ovf;
            if (s_ovf < 16 || (s_ovf & 0x3ff) == 0) {
                fprintf(stderr, "[STACK-OVF#%u] sp=0x%08x (%u bytes past end) call=0x%08x lr=0x%08x\n",
                        s_ovf, sp, 0x806c5ed8u - sp, addr, g_cpu.lr);
                fflush(stderr);
            }
            s_ovf++;
        }
    }
    PPC_Func fn = ppc_lookup_func(addr);
    if (fn) {
        // Generic runaway-recursion guard. Replaces the misdirected per-function
        // SLib_routing depth guards (u_GetGroupUsage doesn't even recurse). Any
        // indirect-call recursion that eats an unreasonable slice of the 512 MB
        // stack is cut off cleanly here, and the culprit address is logged so we
        // can see what actually recurses -- instead of overflowing into a crash.
        {
            // Thread-local: guest worker threads run on their own host stacks;
            // a shared base would measure the inter-thread stack distance
            // (GBs) and falsely cut off every worker call.
            static __thread char *s_base; char probe;
            if (!s_base) s_base = &probe;
            ptrdiff_t used = s_base - &probe; if (used < 0) used = -used;
            if (used > (ptrdiff_t)300 * 1024 * 1024) {
                static int fired;
                if (fired++ < 40) {
                    fprintf(stderr, "[recursion-guard] ~%ldMB stack: addr=0x%08x lr=0x%08x -- cutting off runaway recursion\n",
                            (long)(used >> 20), addr, g_cpu.lr);
                    fflush(stderr);
                }
                g_cpu.gpr[3] = 0;
                return;
            }
        }
#if PPC_HEARTBEAT_STRIDE
        static uint64_t ct;
        if (((++ct) % PPC_HEARTBEAT_STRIDE) == 0) {
            fprintf(stderr, "[ppc] heartbeat: %llu indirect calls, last dest=0x%08x lr=0x%08x sp=0x%08x\n",
                    (unsigned long long)ct, addr, g_cpu.lr, g_cpu.gpr[1]);
            fflush(stderr);
        }
#endif
        /* INVESTIGATE-ONLY (revert before regen): log indirect call into
         * RSO range so we can see if AI-module code is being dispatched. */
        if (addr >= 0x81000000u && addr < 0x82000000u) ppc_watch_rso_enter(addr);
        fn(); return;
    }

    // Quirks integration: the safe-vtable method is a real `blr` written
    // into guest memory by sdk/os.c when a quirks profile asks for it. If
    // the call lands there, treat it as a no-op return -- this is the
    // intended path for any vtable slot the quirks profile redirected.
    extern uint32_t os_safe_method_va(void);
    if (addr == os_safe_method_va()) return;

    // Quirks integration: vtable targets the function-discovery pass missed
    // (tiny arg-setting thunks between known functions). The quirk emulates
    // the thunk and dispatches to the real target; returns 1 if handled.
    extern int quirks_unknown_indirect(uint32_t addr);
    if (quirks_unknown_indirect(addr)) return;

    extern void play_wii_death_and_hang(const char *title, const char *details);

    // addr=0 is always invalid: null vtable slot or uninitialized function
    // pointer. Never a valid call target, so skip silently rather than abort.
    if (addr == 0u) {
        static unsigned s_null_ctr;
        if (s_null_ctr < 24) {
            fprintf(stderr, "[ppc] null indirect call (CTR=0) lr=0x%08x sp=0x%08x r3=0x%08x r4=0x%08x r5=0x%08x -- skipping\n",
                    g_cpu.lr, g_cpu.gpr[1], g_cpu.gpr[3], g_cpu.gpr[4], g_cpu.gpr[5]);
            fflush(stderr);
        }
        ++s_null_ctr;
        g_cpu.gpr[3] = 0;
        return;
    }

    // Lenient mode: opt-in via env, OFF BY DEFAULT.
    //
    // RECOMP_INDIRECT_LENIENT=1  -- log unknowns and continue (returning r3
    //                               = RECOMP_INDIRECT_DEFAULT_R3, default 0).
    //                               Use this only for bring-up; it masks
    //                               correctness bugs cascading from missing
    //                               recompiled functions.
    // RECOMP_INDIRECT_LENIENT=0 (default) -- abort on unknown indirect dest.
    static int lenient = -1;
    if (lenient < 0) {
        const char *e = getenv("RECOMP_INDIRECT_LENIENT");
        lenient = (e && e[0] && e[0] != '0');
    }
    if (!lenient) {
        fprintf(stderr, "[ppc] FATAL: indirect call to unknown addr 0x%08x lr=0x%08x sp=0x%08x\n",
                addr, g_cpu.lr, g_cpu.gpr[1]);
        fprintf(stderr, "       set RECOMP_INDIRECT_LENIENT=1 to continue past unknowns during bring-up.\n");
        ppc_dump_registers();
        fflush(stderr);
        {   /* post-mortem with guest backtrace + recent guest addresses,
             * so the missing function can be identified without a full log */
            extern void robox_prof_crash_dump(const char *reason);
            char why[80];
            snprintf(why, sizeof why,
                     "indirect call to unknown addr 0x%08x (lr 0x%08x)",
                     addr, g_cpu.lr);
            robox_prof_crash_dump(why);
        }
        char details_buf[256];
        snprintf(details_buf, sizeof(details_buf), "Unknown Address: 0x%08x\nLR: 0x%08x SP: 0x%08x", addr, g_cpu.lr, g_cpu.gpr[1]);
        play_wii_death_and_hang("Fatal error : Indirect Call", details_buf);
        abort();
    }
    int loud = note_unknown_once(addr);
    if (loud) {
        fprintf(stderr, "[ppc] (lenient) indirect call to unknown addr 0x%08x lr=0x%08x\n",
                addr, g_cpu.lr);
        fflush(stderr);
    }
    static int dr3 = -1;
    if (dr3 < 0) {
        const char *e = getenv("RECOMP_INDIRECT_DEFAULT_R3");
        dr3 = (e && e[0]) ? (int)strtoul(e, NULL, 0) : 0;
    }
    g_cpu.gpr[3] = (uint32_t)dr3;
}


// ---------------------------------------------------------------------------
// Unimplemented / trace
// ---------------------------------------------------------------------------
//
// Design: a PARTIAL function may contain a handful of unimplemented mnemonics
// and still work if the unimplemented path is cold. We only abort if the
// same site fires a lot, which indicates we're actually executing it.

#ifndef PPC_ABORT_ON_UNIMPL
#define PPC_ABORT_ON_UNIMPL 0
#endif

// Named struct so GCC will assign compound literals of the same type.
// Anonymous-struct-of-array would work too but every write site needs the
// full member list spelled out, which is annoying.
typedef struct {
    uint32_t    pc;
    uint32_t    hits;
    const char *mnem;
} PpcUnimplRec;

#define UNIMPL_CACHE 128
static PpcUnimplRec s_unimpl[UNIMPL_CACHE];
static size_t       s_unimpl_count;

void ppc_unimplemented(const char *mnem, uint32_t pc) {
    for (size_t i = 0; i < s_unimpl_count; ++i) {
        if (s_unimpl[i].pc == pc) {
            s_unimpl[i].hits++;
            // Loud after 1M hits -- catches hot paths that loop over us.
            if (s_unimpl[i].hits == 1000000u) {
                fprintf(stderr, "[ppc] HOT unimplemented: %s @ 0x%08x (1M+ hits)\n",
                        mnem, pc);
                fflush(stderr);
            }
            if (PPC_ABORT_ON_UNIMPL) abort();
            return;
        }
    }
    if (s_unimpl_count < UNIMPL_CACHE) {
        s_unimpl[s_unimpl_count].pc   = pc;
        s_unimpl[s_unimpl_count].hits = 1;
        s_unimpl[s_unimpl_count].mnem = mnem;
        s_unimpl_count++;
        fprintf(stderr, "[ppc] unimplemented: %s @ 0x%08x\n", mnem, pc);
        fflush(stderr);
    }
    if (PPC_ABORT_ON_UNIMPL) abort();
}


// ---------------------------------------------------------------------------
// Trace ring. Only enabled when -DPPC_TRACE_ALL=1; gate is checked in the
// generated code's prologue so release builds pay nothing.
// ---------------------------------------------------------------------------

#define TRACE_RING 4096
static uint32_t s_trace_ring[TRACE_RING];
static size_t   s_trace_head;

void ppc_trace(uint32_t pc) {
    s_trace_ring[s_trace_head % TRACE_RING] = pc;
    s_trace_head++;
    // Guest-r1 canary: the first function entered with an insane stack
    // pointer names the exact spot where a frame push/pop went unbalanced.
    {
        uint32_t sp = g_cpu.gpr[1];
        if (sp < 0x80003000u || sp > 0x817ff000u) {
            static int fired;
            if (fired < 4) {
                ++fired;
                fprintf(stderr, "[R1-CANARY] entering 0x%08x with sp=0x%08x lr=0x%08x r31=0x%08x\n",
                        pc, sp, g_cpu.lr, g_cpu.gpr[31]);
                // Dump the last 32 trace entries for context.
                fprintf(stderr, "[R1-CANARY] recent entries:");
                for (int i = 32; i >= 1; --i) {
                    uint32_t idx = (s_trace_head - i) % TRACE_RING;
                    fprintf(stderr, " %08x", s_trace_ring[idx]);
                }
                fprintf(stderr, "\n");
                fflush(stderr);
            }
        }
    }
    // Echo to stderr if PPC_TRACE_STREAM is set (much noisier than the ring,
    // but precisely shows function-entry order).
    static int stream = -1;
    if (stream < 0) { const char *e = getenv("PPC_TRACE_STREAM"); stream = (e && e[0] && e[0] != '0'); }
    if (stream) {
        fprintf(stderr, "[trace] %08x sp=%08x lr=%08x\n", pc, g_cpu.gpr[1], g_cpu.lr);
        fflush(stderr);
    }
}

// sdk/peripherals.c fires these on VI retrace ticks 100/1000/10000. They
// come from an older tracing layer; keep them as no-ops rather than adding
// plumbing that nobody is reading yet.
void ppc_dump_recent_calls(void) { /* stub: see runtime.h */ }
void ppc_dump_hotspots   (void) { /* stub: see runtime.h */ }

void ppc_dbg_mark(const char *tag) {
    fprintf(stderr, "[dbg-mark] %s\n", tag);
    fflush(stderr);
}

void ppc_dump_unique_recent(uint32_t span) {
    if (s_trace_head == 0) return;
    size_t take = span; if (take > TRACE_RING) take = TRACE_RING;
    if (take > s_trace_head) take = s_trace_head;
    // Tiny de-dup so a tight loop doesn't drown the output.
    uint32_t last = 0;
    fprintf(stderr, "[trace] last %zu unique PCs:\n", take);
    for (size_t i = 0; i < take; ++i) {
        size_t idx = (s_trace_head - take + i) % TRACE_RING;
        if (s_trace_ring[idx] == last) continue;
        last = s_trace_ring[idx];
        fprintf(stderr, "  0x%08x\n", last);
    }
    fflush(stderr);
}


// ---------------------------------------------------------------------------
// SPR catch-all -- unknown SPRs read zero, writes accepted. Broadway boot
// code pokes HID0/HID2/MSR/GQR* during __init_registers but we don't model
// those registers; returning 0 keeps the boot chain alive.
// ---------------------------------------------------------------------------

// Guest GQR registers -- consumed by the quantized paired-single load/store
// helpers in runtime.h. Everything else stays unmodeled (reads as 0).
uint32_t g_gqr[8];

static int spr_gqr_index(const char *name) {
    if (name && name[0] == 'G' && name[1] == 'Q' && name[2] == 'R' &&
        name[3] >= '0' && name[3] <= '7' && name[4] == '\0')
        return name[3] - '0';
    return -1;
}

uint32_t ppc_spr_read(const char *name) {
    int gi = spr_gqr_index(name);
    if (gi >= 0) return g_gqr[gi];
    return 0u;
}
void ppc_spr_write(const char *name, uint32_t value) {
    int gi = spr_gqr_index(name);
    if (gi >= 0) { g_gqr[gi] = value; return; }
    (void)value;
}


// ---------------------------------------------------------------------------
// Time base -- derived from CLOCK_MONOTONIC so it always advances. Broadway
// TB ticks at bus_clock/4 ≈ 60.75 MHz; we approximate.
// ---------------------------------------------------------------------------

// Portable monotonic tick source. We prefer QueryPerformanceCounter on
// Windows because mingw-w64 gates timespec_get on C11-in-strict-mode and
// it's just easier to dodge. On other platforms clock_gettime is universal.
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static uint64_t ppc_tb_ticks(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    // 60.75 MHz tb rate; scale from host QPC domain.
    return (uint64_t)(((__int64)now.QuadPart * 60750000LL) / freq.QuadPart);
}
#else
static uint64_t ppc_tb_ticks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    // 60.75 MHz ≈ 60_750_000 ticks per second.
    return (uint64_t)ts.tv_sec * 60750000ull
         + (uint64_t)ts.tv_nsec * 60750000ull / 1000000000ull;
}
#endif

// ppc_time_base_tb_lower / _upper are owned by sdk/os.c (it has the
// fuller time-base management). Keep ppc_tb_ticks above as the host
// clock primitive in case future runtime code needs a TB-rate tick
// source; mark it used to silence the unused-static warning.
__attribute__((unused)) static uint64_t (*const _keep_ppc_tb_ticks)(void) = &ppc_tb_ticks;


// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/* INVESTIGATE-ONLY (revert before regen): SCR wait-timer dynamic VA watch.
 * SCR_b_CheckMetaWait computes the wait_obj VA at runtime; we capture it
 * the first time and arm a watch on writes to that exact VA. */
uint32_t g_scr_wait_watch_va;
static unsigned g_scr_wait_w_seen;
void ppc_watch_arm_scr_wait(uint32_t va) {
    if (g_scr_wait_watch_va != va) {
        fprintf(stderr, "[SCR-WATCH-ARM] wait_va=0x%08x (was 0x%08x)\n",
                va, g_scr_wait_watch_va);
        fflush(stderr);
        g_scr_wait_watch_va = va;
        g_scr_wait_w_seen = 0;
    }
}
void ppc_watch_scr_wait_w(uint32_t va, float value) {
    if (g_scr_wait_w_seen < 64) {
        fprintf(stderr, "[SCR-WRITE] va=0x%08x new_value=%g lr=0x%08x (write #%u)\n",
                va, (double)value, g_cpu.lr, g_scr_wait_w_seen + 1);
        fflush(stderr);
    }
    g_scr_wait_w_seen++;
}
/* INVESTIGATE-ONLY (audio): who writes the first 16 bytes of the silent AX
 * sample ring at 0x90833140? (see runtime.h MEM_W32) */
void ppc_watch_sndring_w(uint32_t va, uint32_t v) {
    static unsigned s_n;
    if (s_n < 16) {
        s_n++;
        fprintf(stderr, "[SNDRING-W#%u] [0x%08x] <- 0x%08x lr=0x%08x\n",
                s_n, va, v, g_cpu.lr);
        fflush(stderr);
    }
}
/* INVESTIGATE-ONLY (menu vertex hunt): UI vertex-array write watch
 * (see runtime.h MEM_W32). The pool at 0x904afec0 has MULTIPLE writers
 * (K3D_2DRenderer::SendList+0x4c writes real glyph quads; something else
 * writes the degenerate [X,0,1] entries the draws read). Log per UNIQUE
 * LR: first 3 writes each, so the healthy writer can't saturate the cap. */
void ppc_watch_uivtx_w(uint32_t va, uint32_t v) {
    static uint32_t s_lrs[24]; static unsigned s_cnt[24]; static int s_nlr;
    uint32_t lr = g_cpu.lr;
    int idx = -1;
    for (int i = 0; i < s_nlr; ++i) if (s_lrs[i] == lr) { idx = i; break; }
    if (idx < 0 && s_nlr < 24) { idx = s_nlr++; s_lrs[idx] = lr; }
    if (idx < 0) return;
    if (++s_cnt[idx] <= 3) {
        float f; __builtin_memcpy(&f, &v, 4);
        fprintf(stderr, "[UIVTX-W] lr=0x%08x w#%u [0x%08x] <- 0x%08x (%g)\n",
                lr, s_cnt[idx], va, v, f);
        fflush(stderr);
    }
}
/* INVESTIGATE-ONLY (revert before regen): NandHelper error-cell write watch
 * (see runtime.h MEM_W32). Error 3 parks BootUpFlow in state 36. */
void ppc_watch_nanderr_w(uint32_t v) {
    static unsigned s_n;
    if (s_n < 32) {
        fprintf(stderr, "[NANDERR-W#%u] NANDLastError <- %u lr=0x%08x\n",
                s_n + 1, v, g_cpu.lr);
        fflush(stderr);
    }
    s_n++;
}
/* INVESTIGATE-ONLY (revert before regen): current-world slot [mgr+0xc] =
 * 0x901023ec write watch (see runtime.h MEM_W32). Finds who activates each
 * world — the menu load stores a raw record ptr instead of a WOR. */
void ppc_watch_worcur_w(uint32_t v) {
    static unsigned s_n;
    if (s_n < 64) {
        fprintf(stderr, "[WORCUR-W#%u] [mgr+0xc] <- 0x%08x  ([new+0]=0x%08x) lr=0x%08x\n",
                s_n + 1, v, (v >= 0x80000000u && v < 0x94000000u) ? MEM_R32(v) : 0,
                g_cpu.lr);
        fflush(stderr);
    }
    s_n++;
}
/* INVESTIGATE-ONLY (revert before regen): tagged trace helpers. */
void ppc_dbg_tag(const char *tag) {
    fprintf(stderr, "%s\n", tag);
    fflush(stderr);
}
void ppc_dbg_tag_str(const char *tag, uint32_t guest_strptr) {
    const char *s = "(null)";
    if (guest_strptr) s = (const char *)ppc_host_ptr(guest_strptr);
    fprintf(stderr, "%s '%s'\n", tag, s ? s : "(null)");
    fflush(stderr);
}
void ppc_dbg_tag_u32(const char *tag, uint32_t value) {
    fprintf(stderr, "%s 0x%08x\n", tag, value);
    fflush(stderr);
}
void ppc_dbg_tag_str_u32(const char *tag, uint32_t guest_strptr, uint32_t value) {
    const char *s = "(null)";
    if (guest_strptr) s = (const char *)ppc_host_ptr(guest_strptr);
    fprintf(stderr, "%s '%s' result=0x%08x\n", tag, s ? s : "(null)", value);
    fflush(stderr);
}

/* INVESTIGATE-ONLY (revert before regen): SCR_ComUpdate entry log. */
void ppc_dbg_scr_com(uint32_t head_struct, uint32_t scripts_head) {
    fprintf(stderr, "[SCR-COM] entry head=0x%08x scripts_list=0x%08x %s\n",
            head_struct, scripts_head,
            scripts_head ? "(walk)" : "(EARLY EXIT)");
    fflush(stderr);
}

/* Categorize a dispatch target and log per-site first-N samples. */
void ppc_watch_vtbl(uint32_t site_pc, uint32_t target) {
    static struct { uint32_t site; unsigned seen; } slots[16];
    int idx = -1;
    for (int i = 0; i < 16; ++i) {
        if (slots[i].site == site_pc) { idx = i; break; }
        if (slots[i].site == 0) { slots[i].site = site_pc; idx = i; break; }
    }
    if (idx < 0) return;
    if (slots[idx].seen >= 4) { slots[idx].seen++; return; }
    const char *range;
    if (target == 0)                                   range = "stub-zero";
    else if (target < 0x80000000u)                     range = "low";
    else if (target >= 0x80000000u && target < 0x80700000u) range = "main-elf";
    else if (target >= 0x81000000u && target < 0x82000000u) range = "rso";
    else                                                range = "other";
    fprintf(stderr, "[VTBL] site=0x%08x ctr=0x%08x range=%s\n",
            site_pc, target, range);
    fflush(stderr);
    slots[idx].seen++;
}

void ppc_watch_rso_enter(uint32_t target) {
    static unsigned n;
    static uint32_t seen[64];
    if (n >= 50) { n++; return; }
    for (unsigned i = 0; i < n && i < 64; ++i) if (seen[i] == target) return;
    if (n < 64) seen[n] = target;
    fprintf(stderr, "[RSO-ENTER] func=0x%08x lr=0x%08x\n", target, g_cpu.lr);
    fflush(stderr);
    n++;
}

void ppc_watch_log_scr_check(int32_t state_idx, uint32_t wait_va,
                             float wait_value, int result) {
    static unsigned seen;
    if (seen < 60) {
        fprintf(stderr, "[SCR-CHECK] state_index=%d wait_va=0x%08x wait_value=%g\n",
                state_idx, wait_va, (double)wait_value);
        fflush(stderr);
    }
    seen++;
    (void)result;
}

/* INVESTIGATE-ONLY (revert before regen): SCR2CPP enable-flag watch.
 * Logs every read and every write of 0x806c2a18 with the current LR so we
 * can see if the flag ever transitions and who would set it. Read logging
 * is rate-limited so we don't drown stderr. */
static unsigned g_scr2cpp_r_seen, g_scr2cpp_w_seen;
void ppc_watch_scr2cpp_r32(uint32_t v) {
    if (g_scr2cpp_r_seen < 8 || v != 0) {
        fprintf(stderr, "[WATCH-R] 0x806c2a18 = 0x%08x  lr=0x%08x\n",
                v, g_cpu.lr);
        fflush(stderr);
    }
    g_scr2cpp_r_seen++;
}
void ppc_watch_scr2cpp_w32(uint32_t v) {
    fprintf(stderr, "[WATCH-W] SLIB-SLOT 0x806c6b14 := 0x%08x  lr=0x%08x  (write #%u)\n",
            v, g_cpu.lr, ++g_scr2cpp_w_seen);
    fflush(stderr);
}

/* Pristine copies of the guest-RAM base pointers, snapshotted at init.
 * Something host-side intermittently scribbles the BSS globals g_mem1 /
 * g_mem2 (observed: single-byte and larger corruptions -> every later
 * ppc_host_ptr goes wild). Until the writer is caught, the watchdog
 * compares against these and self-heals, logging loudly. */
uint8_t *g_mem1_orig, *g_mem2_orig;

void ppc_runtime_init(void) {
    if (g_runtime_ready) return;
    g_mem1 = (uint8_t *)calloc(PPC_MEM1_SIZE, 1);
    g_mem2 = (uint8_t *)calloc(PPC_MEM2_SIZE, 1);
    if (!g_mem1 || !g_mem2) {
        fprintf(stderr, "[runtime] failed to allocate guest memory\n");
        abort();
    }
    g_mem1_orig = g_mem1;
    g_mem2_orig = g_mem2;
#if defined(_WIN32)
    {   /* verify the CRT really gave us the full regions and they are
         * readable end-to-end (an AV was recorded 32 bytes below the MEM2
         * region end, which should be impossible for a committed calloc). */
        extern size_t _msize(void *);
        volatile uint8_t probe1 = g_mem1[PPC_MEM1_SIZE - 1];
        volatile uint8_t probe2 = g_mem2[PPC_MEM2_SIZE - 1];
        fprintf(stderr, "[runtime] g_mem1=%p _msize=0x%zx  g_mem2=%p _msize=0x%zx  endprobes=%u,%u\n",
                (void*)g_mem1, _msize(g_mem1), (void*)g_mem2, _msize(g_mem2),
                probe1, probe2);
        fflush(stderr);
    }
#endif
    memset(g_deadzone, 0, sizeof g_deadzone);
    memset(&g_cpu, 0, sizeof g_cpu);
    g_runtime_ready = 1;
}

void ppc_reset_memory(void) {
    if (!g_runtime_ready) { ppc_runtime_init(); return; }
    memset(g_mem1, 0, PPC_MEM1_SIZE);
    memset(g_mem2, 0, PPC_MEM2_SIZE);
    memset(g_deadzone, 0, sizeof g_deadzone);
    memset(&g_cpu, 0, sizeof g_cpu);
}

void ppc_dump_registers(void) {
    fprintf(stderr, "[cpu] pc=<see trace>\n");
    for (int i = 0; i < 32; i += 4) {
        fprintf(stderr, "  r%02d=%08x  r%02d=%08x  r%02d=%08x  r%02d=%08x\n",
                i, g_cpu.gpr[i], i+1, g_cpu.gpr[i+1],
                i+2, g_cpu.gpr[i+2], i+3, g_cpu.gpr[i+3]);
    }
    fprintf(stderr, "  lr=%08x ctr=%08x xer=%08x cr=%08x msr=%08x\n",
            g_cpu.lr, g_cpu.ctr, g_cpu.xer, g_cpu.cr, g_cpu.msr);
    fflush(stderr);
}


// ---------------------------------------------------------------------------
// Minimal PPC ELF32 loader. We mirror elf_loader.py's loading rules: copy
// every SHF_ALLOC section with SHT_PROGBITS (code, rodata, data, sdata...)
// into guest memory at its .sh_addr. SHT_NOBITS sections (BSS) are zeroed.
//
// Anything else (SHT_SYMTAB, SHT_STRTAB, .comment, debug info) is skipped.
// ---------------------------------------------------------------------------

#define EI_NIDENT 16
#define ET_EXEC   2
#define EM_PPC    20
#define ELFDATA2MSB 2
#define SHT_PROGBITS 1
#define SHT_NOBITS   8
#define SHF_ALLOC    0x2

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0]<<8) | p[1]); }
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16) |
           ((uint32_t)p[2]<<8)  |  (uint32_t)p[3];
}

int ppc_load_elf(const char *path, uint32_t *entry_out) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "[elf] cannot open %s\n", path); return 1; }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    if (fsize <= 0) { fclose(fp); return 2; }
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)fsize);
    if (!buf) { fclose(fp); return 3; }
    if ((long)fread(buf, 1, (size_t)fsize, fp) != fsize) {
        free(buf); fclose(fp); return 4;
    }
    fclose(fp);

    if (fsize < 52 || memcmp(buf, "\x7F""ELF", 4) != 0) {
        fprintf(stderr, "[elf] not an ELF file\n"); free(buf); return 5;
    }
    if (buf[5] != ELFDATA2MSB) {
        fprintf(stderr, "[elf] not big-endian\n"); free(buf); return 6;
    }
    uint16_t machine = be16(buf + 18);
    if (machine != EM_PPC) {
        fprintf(stderr, "[elf] machine=%u not PPC\n", machine); free(buf); return 7;
    }

    uint32_t entry    = be32(buf + 24);
    uint32_t e_shoff  = be32(buf + 32);
    uint16_t e_shentsize = be16(buf + 46);
    uint16_t e_shnum     = be16(buf + 48);

    if (e_shoff == 0 || e_shnum == 0 || e_shentsize < 40) {
        fprintf(stderr, "[elf] missing section table\n"); free(buf); return 8;
    }
    if (e_shoff + (uint64_t)e_shnum * e_shentsize > (uint64_t)fsize) {
        fprintf(stderr, "[elf] section table out of range\n"); free(buf); return 9;
    }

    size_t loaded_segments = 0;
    uint64_t loaded_bytes  = 0;
    uint32_t mem1_top = 0;   // highest end-VA we wrote inside MEM1
    for (uint16_t i = 0; i < e_shnum; ++i) {
        const uint8_t *sh = buf + e_shoff + (uint32_t)i * e_shentsize;
        uint32_t sh_type  = be32(sh + 4);
        uint32_t sh_flags = be32(sh + 8);
        uint32_t sh_addr  = be32(sh + 12);
        uint32_t sh_off   = be32(sh + 16);
        uint32_t sh_size  = be32(sh + 20);
        if (!(sh_flags & SHF_ALLOC) || sh_size == 0) continue;
        if (sh_type != SHT_PROGBITS && sh_type != SHT_NOBITS) continue;
        uint8_t *dst = (uint8_t *)ppc_host_ptr(sh_addr);
        if (!dst) continue;
        uint32_t end_excl;
        int in_mem1 = 0;
        if (sh_addr >= PPC_MEM1_BASE && sh_addr < PPC_MEM1_BASE + PPC_MEM1_SIZE) {
            end_excl = PPC_MEM1_BASE + PPC_MEM1_SIZE;
            in_mem1 = 1;
        } else if (sh_addr >= PPC_MEM2_BASE && sh_addr < PPC_MEM2_BASE + PPC_MEM2_SIZE) {
            end_excl = PPC_MEM2_BASE + PPC_MEM2_SIZE;
        } else if (sh_addr >= PPC_MEM1_UNCACHED_BASE &&
                   sh_addr < PPC_MEM1_UNCACHED_BASE + PPC_MEM1_SIZE) {
            end_excl = PPC_MEM1_UNCACHED_BASE + PPC_MEM1_SIZE;
        } else {
            continue;
        }
        uint32_t max_size = end_excl - sh_addr;
        if (sh_size > max_size) sh_size = max_size;

        if (sh_type == SHT_NOBITS) {
            memset(dst, 0, sh_size);
        } else {
            if ((uint64_t)sh_off + sh_size > (uint64_t)fsize) continue;
            memcpy(dst, buf + sh_off, sh_size);
            loaded_bytes += sh_size;
        }
        if (in_mem1 && sh_addr + sh_size > mem1_top) {
            mem1_top = sh_addr + sh_size;
        }
        loaded_segments++;
    }

    free(buf);

    g_elf_mem1_top = mem1_top;

    fprintf(stderr, "[elf] %s: entry=0x%08x, %zu segments, %llu bytes loaded, mem1_top=0x%08x\n",
            path, entry, loaded_segments, (unsigned long long)loaded_bytes, mem1_top);
    fflush(stderr);

    if (entry_out) *entry_out = entry;
    return 0;
}

uint32_t ppc_loaded_mem1_top(void) {
    if (g_elf_mem1_top) return g_elf_mem1_top;
    // Fallback: 8 MB into MEM1 (clears typical small-game .bss).
    return 0x80800000u;
}

// ---------------------------------------------------------------------------
// DOL loader. 7 text + 11 data sections at fixed header slots, plus one
// bss range. No magic number; ppc_load_image() dispatches on the ELF
// magic and falls back to DOL.
// ---------------------------------------------------------------------------

int ppc_load_dol(const char *path, uint32_t *entry_out) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "[dol] cannot open %s\n", path); return 1; }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize < 0x100) { fclose(fp); return 2; }
    uint8_t *buf = (uint8_t *)malloc((size_t)fsize);
    if (!buf) { fclose(fp); return 3; }
    if ((long)fread(buf, 1, (size_t)fsize, fp) != fsize) {
        free(buf); fclose(fp); return 4;
    }
    fclose(fp);

    uint32_t mem1_top = 0;
    size_t loaded = 0;
    uint64_t bytes = 0;
    // Zero bss BEFORE copying sections: DOL headers routinely declare a bss
    // range that overlaps trailing data sections (.sdata/.sdata2 live inside
    // it). Zeroing after the copy wiped every small-data static — allocator
    // vtables, float constant pools, font params — the source of the
    // all-zero-matrix screen. Sections copied afterwards win.
    {
        uint32_t bss_addr = be32(buf + 0xd8);
        uint32_t bss_size = be32(buf + 0xdc);
        if (bss_addr && bss_size) {
            uint8_t *dst = (uint8_t *)ppc_host_ptr(bss_addr);
            if (dst) memset(dst, 0, bss_size);
            if (bss_addr >= PPC_MEM1_BASE &&
                bss_addr < PPC_MEM1_BASE + PPC_MEM1_SIZE &&
                bss_addr + bss_size > mem1_top)
                mem1_top = bss_addr + bss_size;
        }
    }
    for (int i = 0; i < 18; ++i) {
        uint32_t off  = be32(buf + 0x00 + i * 4);
        uint32_t addr = be32(buf + 0x48 + i * 4);
        uint32_t size = be32(buf + 0x90 + i * 4);
        if (!off || !addr || !size) continue;
        if ((uint64_t)off + size > (uint64_t)fsize) continue;
        uint8_t *dst = (uint8_t *)ppc_host_ptr(addr);
        if (!dst) continue;
        memcpy(dst, buf + off, size);
        if (addr >= PPC_MEM1_BASE && addr < PPC_MEM1_BASE + PPC_MEM1_SIZE &&
            addr + size > mem1_top)
            mem1_top = addr + size;
        ++loaded;
        bytes += size;
    }
    uint32_t bss_addr = be32(buf + 0xd8);
    uint32_t bss_size = be32(buf + 0xdc);
    uint32_t entry    = be32(buf + 0xe0);
    free(buf);

    g_elf_mem1_top = mem1_top;
    fprintf(stderr, "[dol] %s: entry=0x%08x, %zu sections, %llu bytes, "
            "bss=0x%08x+0x%x, mem1_top=0x%08x\n",
            path, entry, loaded, (unsigned long long)bytes,
            bss_addr, bss_size, mem1_top);
    fflush(stderr);
    if (entry_out) *entry_out = entry;
    return 0;
}

int ppc_load_image(const char *path, uint32_t *entry_out) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "[load] cannot open %s\n", path); return 1; }
    uint8_t magic[4] = {0};
    fread(magic, 1, 4, fp);
    fclose(fp);
    if (memcmp(magic, "\x7F""ELF", 4) == 0)
        return ppc_load_elf(path, entry_out);
    return ppc_load_dol(path, entry_out);
}


// ---------------------------------------------------------------------------
// RSO internal relocations
// ---------------------------------------------------------------------------
//
// RSO modules ship with a trailing RELA-format relocation block that fixes
// up every lis/addi pair, every ADDR32 pointer, and every cross-section
// REL24 branch in the module's own .text/.data. Without applying this block
// every lis/addi computes 0 -> jump tables compute table_addr=0 -> the
// runtime falls into ppc_call_indirect(0). The block ends with an entry
// where r_offset, r_info, and r_addend are all zero (R_PPC_NONE terminator).
//
// sym_idx in r_info >> 8 indexes the module's own SECTION table -- this is
// purely an internal-relocation block. Cross-module references (RSO -> ELF)
// go through the import table separately.

static inline uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}
static inline uint16_t rd_be16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}
static inline void wr_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8); p[3] = (uint8_t)(v      );
}
static inline void wr_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

// PPC reloc types we patch. Anything else is silently ignored (NONE / unknown).
#define R_PPC_NONE      0
#define R_PPC_ADDR32    1
#define R_PPC_ADDR16_LO 4
#define R_PPC_ADDR16_HA 6
#define R_PPC_REL24    10

// Walk backward from EOF in 12-byte strides until an entry stops looking
// like a valid RELA. Block must end with NONE (offset/info/addend all 0).
// Returns 0 on no/invalid block; otherwise sets *out_off and *out_count.
static int rso_find_reloc_block(const uint8_t *file, size_t file_size,
                                 uint32_t max_section_off, uint32_t n_sections,
                                 uint32_t *out_off, uint32_t *out_count) {
    if (file_size < 12) return 0;
    // Last 12 bytes must be terminator.
    if (rd_be32(file + file_size - 12) != 0 ||
        rd_be32(file + file_size -  8) != 0 ||
        rd_be32(file + file_size -  4) != 0)
        return 0;
    size_t pos = file_size - 12;
    while (pos >= 12) {
        size_t prev = pos - 12;
        uint32_t r_off  = rd_be32(file + prev + 0);
        uint32_t r_info = rd_be32(file + prev + 4);
        uint32_t r_type = r_info & 0xFF;
        uint32_t sym    = r_info >> 8;
        if (r_type > 110) break;
        if (r_off >= max_section_off + 0x10000) break;
        if (sym > n_sections) break;
        pos = prev;
    }
    *out_off   = (uint32_t)pos;
    *out_count = (uint32_t)((file_size - pos) / 12);
    return 1;
}

static void rso_apply_internal_relocs(uint8_t *guest_base, uint32_t load_base,
                                       const uint8_t *file, size_t file_size) {
    // Parse RSO header: numSec at 0x08, secInfoOff at 0x0C, version at 0x18.
    if (file_size < 0x58) return;
    uint32_t numSec      = rd_be32(file + 0x08);
    uint32_t secInfoOff  = rd_be32(file + 0x0C);
    if (numSec == 0 || numSec > 64) return;
    if (secInfoOff + numSec * 8u > file_size) return;

    // Build per-section base VA. Loaded sections with non-zero file_off
    // map at load_base + file_off. BSS sections (file_off == 0, size > 0)
    // get a synthetic VA past the last loaded section, aligned to 32.
    uint32_t sec_va[64]   = {0};
    uint32_t sec_size[64] = {0};
    for (uint32_t i = 0; i < numSec; i++) {
        uint32_t raw_off = rd_be32(file + secInfoOff + i * 8 + 0);
        uint32_t sz      = rd_be32(file + secInfoOff + i * 8 + 4);
        uint32_t f_off   = raw_off & ~1u;
        if (sz == 0 && f_off == 0) continue;        // null section
        if (f_off) {
            sec_va[i]   = load_base + f_off;
            sec_size[i] = sz;
        } else {
            // BSS -- place after the last loaded section, aligned to 32.
            uint32_t last_end = load_base;
            for (uint32_t j = 0; j < i; j++) {
                if (sec_va[j]) {
                    uint32_t end = sec_va[j] + sec_size[j];
                    if (end > last_end) last_end = end;
                }
            }
            sec_va[i]   = (last_end + 31u) & ~31u;
            sec_size[i] = sz;
        }
    }

    // Use header fields +0x30/+0x34 to locate the internal reloc table exactly.
    // (The old heuristic scan from EOF was finding wrong blocks.)
    uint32_t rel_off = rd_be32(file + 0x30);
    uint32_t rel_sz  = rd_be32(file + 0x34);
    uint32_t n_rel   = 0;
    if (rel_off && rel_sz && rel_off + rel_sz <= (uint32_t)file_size)
        n_rel = rel_sz / 12;
    if (!n_rel) {
        fprintf(stderr, "[reloc] module @ 0x%08x: no internal relocs (off=0x%x sz=0x%x)\n",
                load_base, rel_off, rel_sz);
        fflush(stderr);
        return;
    }

    uint32_t s_addr32 = 0, s_ha = 0, s_lo = 0, s_rel24 = 0;
    uint32_t s_none = 0, s_unres = 0, s_other = 0;
    for (uint32_t i = 0; i < n_rel; i++) {
        uint32_t entry = rel_off + i * 12;
        uint32_t r_off    = rd_be32(file + entry + 0);
        uint32_t r_info   = rd_be32(file + entry + 4);
        uint32_t r_addend = rd_be32(file + entry + 8);
        uint32_t r_type = r_info & 0xFF;
        uint32_t sym    = r_info >> 8;
        if (r_type == R_PPC_NONE) { s_none++; continue; }
        if (sym >= numSec || !sec_va[sym]) { s_unres++; continue; }
        uint32_t sym_value = sec_va[sym] + r_addend;
        uint8_t *tgt = guest_base + r_off;     // guest_base = host_ptr(load_base)
        switch (r_type) {
            case R_PPC_ADDR32:
                wr_be32(tgt, sym_value); s_addr32++; break;
            case R_PPC_ADDR16_HA:
                wr_be16(tgt, (uint16_t)(((sym_value + 0x8000) >> 16) & 0xFFFF));
                s_ha++; break;
            case R_PPC_ADDR16_LO:
                wr_be16(tgt, (uint16_t)(sym_value & 0xFFFF));
                s_lo++; break;
            case R_PPC_REL24: {
                // Patch the 24-bit displacement of the existing bl/b at r_off,
                // preserving the original AA/LK bits.
                uint32_t orig = rd_be32(tgt);
                uint32_t pc   = load_base + r_off;
                uint32_t disp = (sym_value - pc) & 0x03FFFFFC;
                wr_be32(tgt, (orig & 0xFC000003u) | disp);
                s_rel24++;
                break;
            }
            default:
                s_other++; break;
        }
    }
    fprintf(stderr,
        "[reloc] applied %u entries to module @ 0x%08x\n"
        "  ADDR32:    %u\n  ADDR16_HA: %u\n  ADDR16_LO: %u\n  REL24:     %u\n"
        "  skipped:   %u (NONE), %u (unresolved), %u (other)\n",
        s_addr32 + s_ha + s_lo + s_rel24,
        load_base, s_addr32, s_ha, s_lo, s_rel24,
        s_none, s_unres, s_other);
    fflush(stderr);
}

// Apply build-time-resolved external relocations against guest memory.
// Symbol VAs come pre-baked in the patch list (resolved against the main
// ELF's symbol table + SEL exports during recompile_wii.py), so we don't
// re-lookup names here -- just patch the bytes. Same encoding as the
// internal-reloc apply switch.
void ppc_apply_external_relocs(uint32_t load_base,
                               uint32_t n,
                               const RecompExtPatch *patches) {
    if (!n || !patches) return;
    uint8_t *guest_base = (uint8_t *)ppc_host_ptr(load_base);
    if (!guest_base) {
        fprintf(stderr,
            "[ext-reloc] no host mapping for load_base 0x%08x; skipping %u patches\n",
            load_base, n);
        return;
    }
    uint32_t s_addr32 = 0, s_ha = 0, s_lo = 0, s_rel24 = 0, s_other = 0;
    for (uint32_t i = 0; i < n; i++) {
        const RecompExtPatch *p = &patches[i];
        uint32_t sym_value = p->target_va + p->addend;
        uint8_t *tgt       = guest_base + p->file_off;
        switch (p->type) {
            case R_PPC_ADDR32:
                wr_be32(tgt, sym_value); s_addr32++; break;
            case R_PPC_ADDR16_HA:
                wr_be16(tgt, (uint16_t)(((sym_value + 0x8000) >> 16) & 0xFFFF));
                s_ha++; break;
            case R_PPC_ADDR16_LO:
                wr_be16(tgt, (uint16_t)(sym_value & 0xFFFF));
                s_lo++; break;
            case R_PPC_REL24: {
                uint32_t orig = rd_be32(tgt);
                uint32_t pc   = load_base + p->file_off;
                uint32_t disp = (sym_value - pc) & 0x03FFFFFC;
                wr_be32(tgt, (orig & 0xFC000003u) | disp);
                s_rel24++;
                break;
            }
            default:
                s_other++; break;
        }
    }
    fprintf(stderr,
        "[ext-reloc] applied %u external relocs to module @ 0x%08x\n"
        "  ADDR32: %u  ADDR16_HA: %u  ADDR16_LO: %u  REL24: %u  other: %u\n",
        s_addr32 + s_ha + s_lo + s_rel24, load_base,
        s_addr32, s_ha, s_lo, s_rel24, s_other);
    fflush(stderr);
}

// Raw module loader: memcpy the entire file at `path` into guest memory
// starting at VA `load_base`, then apply the module's internal relocations
// in-place. Returns 0 on success; non-zero on error.
int ppc_load_module_image(const char *path, uint32_t load_base) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        // Retry under the assets dir (common for RSOs that ship there).
        char alt[1024];
        snprintf(alt, sizeof alt, "Assets/%s", path);
        fp = fopen(alt, "rb");
        if (!fp) {
            fprintf(stderr, "[mod] cannot open %s\n", path);
            return 1;
        }
    }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    if (fsize <= 0) { fclose(fp); return 2; }
    fseek(fp, 0, SEEK_SET);
    uint8_t *dst = (uint8_t *)ppc_host_ptr(load_base);
    if (!dst) { fclose(fp); return 3; }
    size_t got = fread(dst, 1, (size_t)fsize, fp);
    fclose(fp);
    fprintf(stderr, "[mod] loaded %s at 0x%08x (%zu bytes)\n",
            path, load_base, got);
    fflush(stderr);

    // Apply the RSO's own internal relocations. We work directly off the
    // already-memcpy'd bytes (they're a faithful copy of the file image).
    rso_apply_internal_relocs(dst, load_base, dst, got);
    return 0;
}
