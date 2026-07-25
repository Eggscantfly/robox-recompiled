// sdk/os.c -- Revolution SDK "OS" core HLE: init, arenas, report, mutexes,
// time, threads. Single-threaded by default on the host; opt-in real host
// threads for designated worker entries (RECOMP_RUN_THREADS / RECOMP_WORKER_ENTRIES).
//
// This file is GAME-AGNOSTIC. All game-specific patches live in quirks/.
// Per-game VAs come in via:
//   * os_set_arena_bounds(lo1, hi1, lo2, hi2)   -- called by main after the
//                                                  ELF/DOL is loaded
//   * the quirks_t hooks registered by quirks/<gameid>.c
// If nothing is registered, OSInit uses ELF-derived defaults and skips all
// game-specific patches.

// clock_gettime / CLOCK_MONOTONIC visibility on glibc.
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "hle.h"
#include "../quirks/quirks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


// ---------------------------------------------------------------------------
// Arena state. We keep a copy in C globals AND mirror to the well-known
// low-memory slots so PPC code that reads them directly sees sane values.
//
// Arena slots in MEM1 low memory (libogc/SDK convention):
//   0x80000020  PhysicalMEM1Size
//   0x80000028  ConsoleBusSpeed
//   0x8000002C  CPUSpeed
//   0x800000F0  SimulatedMEM1Size
//   0x800000F8  Wii BusClockSpeed
//   0x800000FC  Wii CpuClockSpeed
//   0x80003100  MEM1 ArenaLo
//   0x80003104  MEM1 ArenaHi
//   0x80003118  MEM2 ArenaLo
//   0x8000311C  MEM2 ArenaHi
// ---------------------------------------------------------------------------

static uint32_t arena_lo_mem1 = 0;
static uint32_t arena_hi_mem1 = 0;
static uint32_t arena_lo_mem2 = 0;
static uint32_t arena_hi_mem2 = 0;
static int      arena_bounds_set = 0;

static void low_mem_poke_u32(uint32_t va, uint32_t value) { MEM_W32(va, value); }

// Called by main after ppc_load_elf() finishes. Caller passes the highest
// loaded VA in MEM1 (after .bss); we round up and use that as arena_lo.
// Stack reservation comes off the top.
void os_set_arena_bounds(uint32_t mem1_used_top,
                         uint32_t mem2_reserved_low,
                         uint32_t stack_reserve_bytes) {
    arena_lo_mem1 = (mem1_used_top + 31u) & ~31u;
    arena_hi_mem1 = (PPC_MEM1_BASE + PPC_MEM1_SIZE) - stack_reserve_bytes;
    arena_lo_mem2 = (PPC_MEM2_BASE + mem2_reserved_low + 31u) & ~31u;
    // MEM2 arena must stay inside the GUEST-VISIBLE MEM2 (64 MB on retail),
    // even when the host backing is larger (PPC_MEM2_SIZE). Games size
    // pools from OSGetMEM2ArenaHi-Lo; handing them the host backing size
    // makes them allocate hundreds of MB. Keep ~12 MB at the top for the
    // IOS/IPC reserve like the real BI2 arena does.
    {
        uint32_t guest_mem2 = (PPC_MEM2_SIZE > 0x04000000u) ? 0x04000000u
                                                            : PPC_MEM2_SIZE;
        arena_hi_mem2 = (PPC_MEM2_BASE + guest_mem2) - 0x00C00000u;
    }
    arena_bounds_set = 1;
}

// Default-fill arena bounds if main forgot to call os_set_arena_bounds.
// Picks values that work for "most" Wii games but are not optimal: arena
// starts at 8 MB into MEM1 (past the typical end of game .bss for small/
// medium titles) and reserves 64 KB stack. Big games WILL want to override.
static void apply_default_arena_bounds(void) {
    arena_lo_mem1 = 0x80800000;
    arena_hi_mem1 = (PPC_MEM1_BASE + PPC_MEM1_SIZE) - 0x10000;
    arena_lo_mem2 = PPC_MEM2_BASE + 0x100000;
    arena_hi_mem2 = (PPC_MEM2_BASE + PPC_MEM2_SIZE) - 0x10000;
    arena_bounds_set = 1;
}


// ---------------------------------------------------------------------------
// Safe-vtable mechanism. Quirks files can opt their game in by listing
// `safe_vtable_targets` in their quirks_t. Without that, this is completely
// inert -- no .bss pokes, no fake vtables.
//
// When enabled: we lay down a 1-instruction `blr` at SAFE_METHOD_VA, then
// build a 256-entry vtable at SAFE_VTABLE_VA whose every slot points at
// the blr. Quirks then list "object pointer" VAs that the recompiler can't
// reach (because their constructor never ran), and we patch *(VA) to point
// at the safe vtable. Vtable dispatches resolve to the blr -> return 0.
// ---------------------------------------------------------------------------

#define SAFE_METHOD_VA  0x80003400u
#define SAFE_VTABLE_VA  0x80003500u

uint32_t os_safe_vtable_va(void) { return SAFE_VTABLE_VA; }
uint32_t os_safe_method_va(void) { return SAFE_METHOD_VA; }

static int g_safe_vtable_installed = 0;
int os_safe_vtable_installed(void) { return g_safe_vtable_installed; }

static void install_safe_vtable_if_requested(const quirks_t *q) {
    if (!q || q->safe_vtable_target_count == 0) return;
    MEM_W32(SAFE_METHOD_VA, 0x4E800020u);   // PPC `blr`
    for (int i = 0; i < 256; ++i) {
        MEM_W32(SAFE_VTABLE_VA + i * 4, SAFE_METHOD_VA);
    }
    for (size_t i = 0; i < q->safe_vtable_target_count; ++i) {
        MEM_W32(q->safe_vtable_targets[i], SAFE_VTABLE_VA);
    }
    g_safe_vtable_installed = 1;
}


// ---------------------------------------------------------------------------
// Dummy "current thread" struct. Several OS globals at low memory point at
// thread/context blocks; code dereferences them as e.g. `sth r0, 0x2c8(r_thread)`.
// Without a real ctor we'd null-deref. Park a 4 KB zeroed region just past
// the safe-vtable area and point those slots at it.
// ---------------------------------------------------------------------------

#define DUMMY_THREAD_VA  0x80003200u

static void install_dummy_thread_pointers(void) {
    low_mem_poke_u32(0x800000E4, DUMMY_THREAD_VA);  // current thread (libogc)
    low_mem_poke_u32(0x800000D4, DUMMY_THREAD_VA);  // alt context slot
    low_mem_poke_u32(0x800000DC, DUMMY_THREAD_VA);  // alt context slot
}


// ---------------------------------------------------------------------------
// OSInit
// ---------------------------------------------------------------------------

void hle_OSInit(void) {
    HLE_TRACE("OSInit");

    // Seed low-memory boot info with plausible Wii values.
    low_mem_poke_u32(0x80000020, PPC_MEM1_SIZE);    // PhysMEM1
    low_mem_poke_u32(0x80000028, 243000000);        // Bus clock ~243 MHz
    low_mem_poke_u32(0x8000002C, 729000000);        // CPU clock ~729 MHz
    low_mem_poke_u32(0x800000F0, PPC_MEM1_SIZE);    // SimulatedMEM1
    low_mem_poke_u32(0x800000F8, 243000000);        // Wii BusClockSpeed
    low_mem_poke_u32(0x800000FC, 729000000);        // Wii CpuClockSpeed
    low_mem_poke_u32(0x80000060, 0);                // Exception handler base
    low_mem_poke_u32(0x80000034, 0);                // OSArenaHi sentinel

    if (!arena_bounds_set) apply_default_arena_bounds();
    low_mem_poke_u32(0x80003100, arena_lo_mem1);
    low_mem_poke_u32(0x80003104, arena_hi_mem1);
    low_mem_poke_u32(0x80003118, arena_lo_mem2);
    low_mem_poke_u32(0x8000311C, arena_hi_mem2);

    install_dummy_thread_pointers();

    const quirks_t *q = quirks_active();
    install_safe_vtable_if_requested(q);

    // Per-game low-memory pokes. quirks/<game>.c lists (va, value) pairs
    // for any spots the game reads at startup that our HLE didn't populate.
    if (q) {
        for (size_t i = 0; i < q->lowmem_poke_count; ++i) {
            MEM_W32(q->lowmem_pokes[i].va, q->lowmem_pokes[i].value);
        }
        if (q->osinit_post_hook) q->osinit_post_hook();
    }

    HLE_RET(0);
}

void hle_OSReport(void) {
    hle_printf_guest(NULL, 0);
    fflush(stdout);
}

void hle_OSVReport(void) {
    hle_printf_guest(NULL, 0);
    fflush(stdout);
}

#include <SDL2/SDL.h>
#include "../src/wii_death_wav.h"

extern SDL_Window *g_window;
extern SDL_Renderer *g_renderer;

extern void gx_ogl_render_crash_screen(const char *title, const char *details, const char *regs);

void play_wii_death_and_hang(const char *title, const char *details) {
    // Bring-up default: DISABLED. The on-screen Wii death screen keeps the
    // process alive forever which stalls automated run/fix cycles. Set
    // RECOMP_DEATH_SCREEN=1 to re-enable the full screen + sound + hang.
    {
        const char *e = getenv("RECOMP_DEATH_SCREEN");
        if (!(e && e[0] && e[0] != '0')) {
            fprintf(stderr, "\n[death] %s\n", title ? title : "Fatal error");
            if (details) fprintf(stderr, "[death] %s\n", details);
            ppc_dump_registers();
            fflush(stderr);
            exit(1);
        }
    }
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    
    SDL_AudioSpec wav_spec;
    Uint32 wav_length;
    Uint8 *wav_buffer;
    SDL_AudioDeviceID deviceId = 0;

    SDL_RWops *rw = SDL_RWFromConstMem(wii_death_wav, wii_death_wav_len);
    if (rw && SDL_LoadWAV_RW(rw, 1, &wav_spec, &wav_buffer, &wav_length)) {
        deviceId = SDL_OpenAudioDevice(NULL, 0, &wav_spec, NULL, 0);
        if (deviceId) {
            SDL_PauseAudioDevice(deviceId, 0);
        }
    }
    
    char regs[2048];
    snprintf(regs, sizeof(regs),
        "r0 : %08x  r1 : %08x  r2 : %08x  r3 : %08x\n"
        "r4 : %08x  r5 : %08x  r6 : %08x  r7 : %08x\n"
        "r8 : %08x  r9 : %08x  r10: %08x  r11: %08x\n"
        "r12: %08x  r13: %08x  r14: %08x  r15: %08x\n"
        "r16: %08x  r17: %08x  r18: %08x  r19: %08x\n"
        "r20: %08x  r21: %08x  r22: %08x  r23: %08x\n"
        "r24: %08x  r25: %08x  r26: %08x  r27: %08x\n"
        "r28: %08x  r29: %08x  r30: %08x  r31: %08x\n"
        "LR : %08x  CR : %08x  CTR: %08x  XER: %08x",
        g_cpu.gpr[0], g_cpu.gpr[1], g_cpu.gpr[2], g_cpu.gpr[3],
        g_cpu.gpr[4], g_cpu.gpr[5], g_cpu.gpr[6], g_cpu.gpr[7],
        g_cpu.gpr[8], g_cpu.gpr[9], g_cpu.gpr[10], g_cpu.gpr[11],
        g_cpu.gpr[12], g_cpu.gpr[13], g_cpu.gpr[14], g_cpu.gpr[15],
        g_cpu.gpr[16], g_cpu.gpr[17], g_cpu.gpr[18], g_cpu.gpr[19],
        g_cpu.gpr[20], g_cpu.gpr[21], g_cpu.gpr[22], g_cpu.gpr[23],
        g_cpu.gpr[24], g_cpu.gpr[25], g_cpu.gpr[26], g_cpu.gpr[27],
        g_cpu.gpr[28], g_cpu.gpr[29], g_cpu.gpr[30], g_cpu.gpr[31],
        g_cpu.lr, g_cpu.cr, g_cpu.ctr, g_cpu.xer
    );
    
    gx_ogl_render_crash_screen(title, details, regs);

    
    while (1) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) exit(1);
            if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_ESCAPE) exit(1);
                if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) {
                    FILE *f = fopen("crash_dump.txt", "w");
                    if (f) {
                        fprintf(f, "--- CRASH DUMP ---\n\n");
                        fprintf(f, "Title: %s\n\n", title ? title : "Fatal error");
                        if (details) fprintf(f, "Details:\n%s\n\n", details);
                        fprintf(f, "Registers:\n%s\n", regs);
                        fclose(f);
                    }
                    exit(1);
                }
            }
        }
        
        if (deviceId && wav_buffer && wav_length > 0) {
            if (SDL_GetQueuedAudioSize(deviceId) < wav_length) {
                SDL_QueueAudio(deviceId, wav_buffer, wav_length);
            }
        }
        SDL_Delay(16);
    }
}

void hle_OSPanic(void) {
    const char *file = HLE_STR(0);
    int line          = HLE_ARG_S32(1);
    uint32_t fmt_va = HLE_ARG_U32(2);
    const char *fmt = fmt_va ? (const char *)ppc_host_ptr(fmt_va) : NULL;

    static struct { const char *key; uint32_t hits; } seen[32];
    static int seen_count;
    for (int i = 0; i < seen_count; ++i) {
        if (seen[i].key == file && (int)(seen[i].hits & 0xFFFFFF) == line) {
            seen[i].hits += 0x01000000u;
            if ((seen[i].hits >> 24) == 0x10u) {
                fprintf(stderr, "[OSPanic] %s:%d: (suppressed further hits)\n",
                        file ? file : "(?)", line);
            }
            return;
        }
    }
    if (seen_count < 32) {
        seen[seen_count].key = file;
        seen[seen_count].hits = (uint32_t)line | 0x01000000u;
        seen_count++;
    }
    fprintf(stderr, "[OSPanic] %s:%d: %s\n",
            file ? file : "(?)", line, fmt ? fmt : "(no fmt)");
    fflush(stderr);
    // Bring-up default: OSPanic is a LOG, not a stop. Games panic on
    // subsystem hiccups (font stream accounting, etc.) that are survivable
    // while the port stabilizes. RECOMP_PANIC_FATAL=1 restores the halt.
    static int panic_fatal = -1;
    if (panic_fatal < 0) {
        const char *e = getenv("RECOMP_PANIC_FATAL");
        panic_fatal = (e && e[0] && e[0] != '0');
    }
    if (!panic_fatal) return;
    char title_buf[64];
    char details_buf[256];
    snprintf(title_buf, sizeof(title_buf), "Fatal error : OSPanic");
    snprintf(details_buf, sizeof(details_buf), "Location: %s:%d\n%s", file ? file : "(?)", line, fmt ? fmt : "(no fmt)");
    play_wii_death_and_hang(title_buf, details_buf);
    abort();
}

void hle_OSFatal(void) {
    const char *msg = HLE_STR(2);
    fprintf(stderr, "[OSFatal] LR=0x%08x  sp=0x%08x  msg=%s\n",
            g_cpu.lr, g_cpu.gpr[1], msg ? msg : "(no msg)");
    fprintf(stderr, "         r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x\n",
            g_cpu.gpr[3], g_cpu.gpr[4], g_cpu.gpr[5], g_cpu.gpr[6], g_cpu.gpr[7]);
    static int abort_on_fatal = -1;
    if (abort_on_fatal < 0) {
        const char *e = getenv("RECOMP_OSFATAL_ABORT");
        abort_on_fatal = (e && e[0] && e[0] != '0');
    }
    char title_buf[64];
    char details_buf[256];
    snprintf(title_buf, sizeof(title_buf), "Fatal error : OSFatal");
    snprintf(details_buf, sizeof(details_buf), "Message: %s\nLR: 0x%08x SP: 0x%08x\nr3: %08x r4: %08x", msg ? msg : "(no msg)", g_cpu.lr, g_cpu.gpr[1], g_cpu.gpr[3], g_cpu.gpr[4]);
    play_wii_death_and_hang(title_buf, details_buf);
    if (abort_on_fatal) abort();
    fprintf(stderr, "         (continuing past OSFatal; set RECOMP_OSFATAL_ABORT=1 to abort)\n");
    fflush(stderr);
}


// ---------------------------------------------------------------------------
// Arena setters / getters (SDK-compatible).
// ---------------------------------------------------------------------------

void hle_OSSetMEM1ArenaLo(void) { arena_lo_mem1 = HLE_ARG_U32(0); low_mem_poke_u32(0x80003100, arena_lo_mem1); HLE_RET(0); }
void hle_OSSetMEM1ArenaHi(void) { arena_hi_mem1 = HLE_ARG_U32(0); low_mem_poke_u32(0x80003104, arena_hi_mem1); HLE_RET(0); }
void hle_OSSetMEM2ArenaLo(void) { arena_lo_mem2 = HLE_ARG_U32(0); low_mem_poke_u32(0x80003118, arena_lo_mem2); HLE_RET(0); }
void hle_OSSetMEM2ArenaHi(void) { arena_hi_mem2 = HLE_ARG_U32(0); low_mem_poke_u32(0x8000311C, arena_hi_mem2); HLE_RET(0); }
void hle_OSGetMEM1ArenaLo(void) { HLE_RET(arena_lo_mem1); }
void hle_OSGetMEM1ArenaHi(void) { HLE_RET(arena_hi_mem1); }
void hle_OSGetMEM2ArenaLo(void) { HLE_RET(arena_lo_mem2); }
void hle_OSGetMEM2ArenaHi(void) { HLE_RET(arena_hi_mem2); }


// ---------------------------------------------------------------------------
// Heap allocator. A first-fit free-list allocator over a contiguous slab of
// MEM2. Free, alloc, and realloc all work. Block metadata lives HOST-side
// so guest scribbles past their alloc don't trash headers.
// ---------------------------------------------------------------------------

// The HLE heap must stay OUT of guest-visible MEM2 (0x90000000..0x94000000):
// Robox's own recompiled OSCreateHeap builds the Lua/MSL heap at the MEM2
// arena lo (0x90100000..0x90500000), and a base of 0x90100000 double-booked
// that region — the first HLE p_Alloc handed out 0x90100000, the caller's
// (HLE'd, host-side) memset wiped the OSHeap descriptor, and every later
// Lua allocation failed -> "not enough memory" panic at title init.
// 0x94000000..0x9d000000 sits above the guest arena in host-only backing
// (g_mem2 is 256 MB) and was already part of the old heap's address range.
#define HEAP_BASE_VA   0x94000000u
#if defined(__3DS__)
// The HLE heap lives just above the guest-visible 64 MB of MEM2, and every byte
// of its address range must be backed by the g_mem2 buffer (PPC_MEM2_SIZE). On
// the 3DS that buffer is only 72 MB (see CMakeLists ROBOX_MEM2_SIZE), so the HLE
// heap gets the 8 MB window 0x94000000..0x94800000 -- enough to reach the menu
// for the speed probe. Growing it means growing ROBOX_MEM2_SIZE in lockstep,
// which is the memory squeeze the handheld port has to win. MAX_BLOCKS is the
// free-list metadata array (12 B each); 128 K blocks (1.5 MB) is 8x the >16 K a
// LyN world needs and a fraction of the 12.5 MB the desktop's 1 M blocks cost.
#define HEAP_END_VA    0x94800000u
#define MAX_BLOCKS     131072u
#else
#define HEAP_END_VA    0x9d000000u   // 144 MB of host-only MEM2 backing
#define MAX_BLOCKS     1048576u  /* was 16384: LyN worlds hold >16K live allocs; cap is host-side only */
#endif

typedef struct {
    uint32_t base;
    uint32_t size;
    uint8_t  used;
} HeapBlock;

static HeapBlock g_heap[MAX_BLOCKS];
static size_t    g_heap_count;
static int       g_heap_inited;

static void heap_init(void) {
    if (g_heap_inited) return;
    g_heap[0].base = HEAP_BASE_VA;
    g_heap[0].size = HEAP_END_VA - HEAP_BASE_VA;
    g_heap[0].used = 0;
    g_heap_count = 1;
    g_heap_inited = 1;
}

static uint32_t heap_alloc(uint32_t size, uint32_t align) {
    if (!g_heap_inited) heap_init();
    if (align == 0 || (align & (align - 1)) != 0) align = 16;
    if (size == 0) size = 1;
    size = (size + 15u) & ~15u;

    for (size_t i = 0; i < g_heap_count; ++i) {
        if (g_heap[i].used) continue;
        uint32_t base    = g_heap[i].base;
        uint32_t aligned = (base + align - 1) & ~(align - 1);
        uint32_t pad     = aligned - base;
        if (g_heap[i].size < pad + size) continue;

        uint32_t leftover = g_heap[i].size - pad - size;

        if (pad > 0) {
            if (g_heap_count + 1 >= MAX_BLOCKS) return 0;
            memmove(&g_heap[i + 1], &g_heap[i], (g_heap_count - i) * sizeof(HeapBlock));
            g_heap_count++;
            g_heap[i].base = base;
            g_heap[i].size = pad;
            g_heap[i].used = 0;
            ++i;
        }
        g_heap[i].base = aligned;
        g_heap[i].size = size;
        g_heap[i].used = 1;
        if (leftover > 0) {
            if (g_heap_count + 1 >= MAX_BLOCKS) return aligned;
            memmove(&g_heap[i + 2], &g_heap[i + 1], (g_heap_count - i - 1) * sizeof(HeapBlock));
            g_heap_count++;
            g_heap[i + 1].base = aligned + size;
            g_heap[i + 1].size = leftover;
            g_heap[i + 1].used = 0;
        }
        /* NO memset: real hardware allocators do NOT clear memory, and the
         * game RELIES on that — confirmed live: the sound engine loads its
         * sample bank, frees it, re-allocates the same region (deterministic
         * first-fit) and arms AX voices expecting the samples to still be
         * there. Our zeroing erased every re-allocated bank → silent audio.
         * Never-yet-used memory is still zero from the calloc'd backing, so
         * dropping the memset is hardware-accurate in both directions. */
        {   /* high-water tracing: how full the tracker gets */
            static size_t hw;
            if (g_heap_count > hw + 4096) {
                hw = g_heap_count;
                fprintf(stderr, "[heap] high-water: %zu blocks lr=0x%08x\n",
                        g_heap_count, g_cpu.lr);
                fflush(stderr);
            }
        }
        return aligned;
    }
    static int warned;
    if (!warned) {
        warned = 1;
        fprintf(stderr, "[heap] out of memory: requested %u bytes, %zu blocks tracked"
                " lr=0x%08x sp=0x%08x\n",
                size, g_heap_count, g_cpu.lr, g_cpu.gpr[1]);
        fflush(stderr);
    }
    return 0;
}

static void heap_coalesce(void) {
    for (size_t i = 0; i + 1 < g_heap_count; ) {
        if (!g_heap[i].used && !g_heap[i + 1].used &&
            g_heap[i].base + g_heap[i].size == g_heap[i + 1].base) {
            g_heap[i].size += g_heap[i + 1].size;
            memmove(&g_heap[i + 1], &g_heap[i + 2], (g_heap_count - i - 2) * sizeof(HeapBlock));
            g_heap_count--;
        } else {
            ++i;
        }
    }
}

static void heap_free(uint32_t addr) {
    if (addr == 0) return;
    if (!g_heap_inited) return;
    for (size_t i = 0; i < g_heap_count; ++i) {
        if (g_heap[i].base == addr && g_heap[i].used) {
            g_heap[i].used = 0;
            heap_coalesce();
            return;
        }
    }
    // Free of unknown pointer: probably a guest-managed sub-allocator.
    // Silently ignore.
}

static uint32_t heap_realloc(uint32_t addr, uint32_t new_size) {
    if (addr == 0) return heap_alloc(new_size, 16);
    if (new_size == 0) { heap_free(addr); return 0; }
    if (!g_heap_inited) return 0;
    for (size_t i = 0; i < g_heap_count; ++i) {
        if (g_heap[i].base == addr && g_heap[i].used) {
            uint32_t old_size = g_heap[i].size;
            if (new_size <= old_size) return addr;
            uint32_t fresh = heap_alloc(new_size, 16);
            if (!fresh) return 0;
            memcpy(ppc_host_ptr(fresh), ppc_host_ptr(addr), old_size);
            heap_free(addr);
            return fresh;
        }
    }
    return 0;
}

uint32_t game_heap_alloc(uint32_t size, uint32_t align) { return heap_alloc(size, align); }
void     game_heap_free (uint32_t addr) { heap_free(addr); }


// ---------------------------------------------------------------------------
// SDK heap shims. All map to the host heap. Game-specific routing belongs
// in quirks/<game>.c, not here.
// ---------------------------------------------------------------------------

void hle_MEMAllocFromExpHeap(void) {
    uint32_t size  = HLE_ARG_U32(1);
    int32_t  align = HLE_ARG_S32(2);
    if (align < 0) align = -align;
    HLE_RET(heap_alloc(size, (uint32_t)align));
}
void hle_MEMAllocFromExpHeapEx(void) { hle_MEMAllocFromExpHeap(); }
void hle_MEMFreeToExpHeap(void)      { heap_free(HLE_ARG_U32(1)); HLE_RET(0); }

void hle_game_p_Alloc(void)         { HLE_RET(heap_alloc(HLE_ARG_U32(1), 16)); }
void hle_game_p_AllocAlign(void) {
    uint32_t size  = HLE_ARG_U32(1);
    uint32_t align = HLE_ARG_U32(2);
    HLE_RET(heap_alloc(size, align ? align : 16));
}
void hle_game_p_AllocExt(void)      { HLE_RET(heap_alloc(HLE_ARG_U32(1), 16)); }
void hle_game_p_AllocAlignExt(void) {
    uint32_t size  = HLE_ARG_U32(1);
    uint32_t align = HLE_ARG_U32(2);
    HLE_RET(heap_alloc(size, align ? align : 16));
}
void hle_game_Free(void)         { heap_free(HLE_ARG_U32(1)); HLE_RET(0); }
void hle_game_FreeTmp(void)      { heap_free(HLE_ARG_U32(1)); HLE_RET(0); }
void hle_game_Realloc(void)      { HLE_RET(heap_realloc(HLE_ARG_U32(1), HLE_ARG_U32(2))); }
void hle_game_ReallocAlign(void) { HLE_RET(heap_realloc(HLE_ARG_U32(1), HLE_ARG_U32(2))); }


void hle_OSAllocFromMEM1ArenaLo(void) {
    uint32_t size  = HLE_ARG_U32(0);
    uint32_t align = HLE_ARG_U32(1);
    if (align == 0) align = 32;
    uint32_t p = (arena_lo_mem1 + (align - 1)) & ~(align - 1);
    arena_lo_mem1 = p + size;
    low_mem_poke_u32(0x80003100, arena_lo_mem1);
    HLE_RET(p);
}
void hle_OSAllocFromMEM2ArenaLo(void) {
    uint32_t size  = HLE_ARG_U32(0);
    uint32_t align = HLE_ARG_U32(1);
    if (align == 0) align = 32;
    uint32_t p = (arena_lo_mem2 + (align - 1)) & ~(align - 1);
    arena_lo_mem2 = p + size;
    low_mem_poke_u32(0x80003118, arena_lo_mem2);
    HLE_RET(p);
}


// ---------------------------------------------------------------------------
// Interrupts / mutexes: single-threaded host -> NOPs.
// ---------------------------------------------------------------------------

static uint32_t fake_int_state = 1;

void hle_OSDisableInterrupts(void) { uint32_t prev = fake_int_state; fake_int_state = 0; HLE_RET(prev); }
void hle_OSEnableInterrupts (void) { uint32_t prev = fake_int_state; fake_int_state = 1; HLE_RET(prev); }
void hle_OSRestoreInterrupts(void) { fake_int_state = HLE_ARG_U32(0); HLE_RET(0); }

void hle_OSInitMutex(void)         { HLE_RET(0); }
void hle_OSLockMutex(void)         { HLE_RET(0); }
void hle_OSUnlockMutex(void)       { HLE_RET(0); }
void hle_OSTryLockMutex(void)      { HLE_RET(1); }
void hle_OSInitCond(void)          { HLE_RET(0); }
void hle_OSSignalCond(void)        { HLE_RET(0); }
void hle_OSWaitCond(void)          { HLE_RET(0); }
void hle_OSBroadcastCond(void)     { HLE_RET(0); }


// ---------------------------------------------------------------------------
// Time. Broadway TB ticks at bus/4 ~= 60.75 MHz on Wii.
// ---------------------------------------------------------------------------

#if defined(_WIN32)
#include <windows.h>
#elif defined(__3DS__)
#include <3ds.h>
#endif

static uint64_t host_ns_now(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)c.QuadPart * 1000000000ull / (uint64_t)freq.QuadPart;
#elif defined(__3DS__)
    // newlib's clock_gettime(CLOCK_MONOTONIC) is not dependable on the 3DS.
    // svcGetSystemTick counts at SYSCLOCK_ARM11 (268.111856 MHz) on both the Old
    // and New 3DS, independent of the 268/804 MHz core-speed switch.
    return (uint64_t)(svcGetSystemTick() * (1000000000.0 / (double)SYSCLOCK_ARM11));
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static uint64_t base_ns;

uint64_t ppc_time_base_full(void) {
    if (!base_ns) base_ns = host_ns_now();
    uint64_t delta_ns = host_ns_now() - base_ns;
    // Broadway TB at bus/4 with bus=243 MHz -> 60.75 MHz.
    // ticks = delta_ns * 60750 / 1_000_000.
    return (delta_ns * 60750ull) / 1000000ull;
}

uint32_t ppc_time_base_tb_lower(void) { return (uint32_t)ppc_time_base_full(); }
uint32_t ppc_time_base_tb_upper(void) { return (uint32_t)(ppc_time_base_full() >> 32); }

void hle_OSGetTime(void) {
    uint64_t tb = ppc_time_base_full();
    g_cpu.gpr[3] = (uint32_t)(tb >> 32);
    g_cpu.gpr[4] = (uint32_t)tb;
}
void hle_OSGetTick(void) { HLE_RET((uint32_t)ppc_time_base_full()); }


// ---------------------------------------------------------------------------
// Threads. Single-threaded by default. Per-game opt-in real host threads
// for designated worker entries via quirks->worker_entries or env override.
// ---------------------------------------------------------------------------

#define MAX_THREADS 32

typedef struct {
    int      in_use;
    uint32_t handle;
    uint32_t entry;
    uint32_t param;
    uint32_t stack_top;
    uint32_t prio;
    int      detached;
} ThreadSlot;

static ThreadSlot g_threads[MAX_THREADS];
static uint32_t   g_next_thread_handle = 0x80000001u;

// ---------------------------------------------------------------------------
// PPC big lock: only one thread executes PPC code at a time.
// Wii is single-core; threads alternate. The lock is acquired before any
// ppc_call_indirect and released inside blocking OS primitives so the other
// thread can run.
//
// PORTABILITY: this whole section used to live inside a single
// `#if defined(_WIN32)` with no `#else`, so every symbol below simply did not
// exist off Windows -- and generated code calls hle_OSInitMessageQueue()
// unconditionally, so the link failed. It is now built on SDL2 (already a hard
// dependency) and compiles everywhere. The mappings preserve Windows
// behaviour exactly:
//     CRITICAL_SECTION      -> SDL_mutex  (both recursive)
//     auto-reset HANDLE     -> SDL_sem    (a Win32 auto-reset event IS a
//                                          binary semaphore; see evt_signal)
//     CreateSemaphore       -> SDL_sem
//     CreateThread          -> SDL_CreateThread
//     InterlockedIncrement  -> SDL_AtomicAdd
// ---------------------------------------------------------------------------
static SDL_mutex *g_ppc_cs;
static int        g_ppc_cs_ready = 0;

static void ppc_lock_init(void) {
    if (!g_ppc_cs_ready) {
        g_ppc_cs = SDL_CreateMutex();
        g_ppc_cs_ready = 1;
        SDL_LockMutex(g_ppc_cs);          // main thread holds it from the start
    }
}
void ppc_lock_acquire(void) { if (g_ppc_cs_ready) SDL_LockMutex(g_ppc_cs); }
void ppc_lock_release(void) { if (g_ppc_cs_ready) SDL_UnlockMutex(g_ppc_cs); }

// Win32 auto-reset event semantics on top of SDL_sem: SetEvent on an already
// signalled event is a no-op, so cap the count at 1 rather than letting it
// grow. Every waiter here re-checks its predicate in a loop, so the small race
// on SDL_SemValue only ever costs one extra spurious wakeup.
static SDL_sem *evt_create(int signalled) { return SDL_CreateSemaphore(signalled ? 1u : 0u); }
static void     evt_signal(SDL_sem *e)    { if (e && SDL_SemValue(e) == 0) SDL_SemPost(e); }
static void     evt_wait(SDL_sem *e)      { if (e) SDL_SemWait(e); }

// g_cpu is owned by whichever thread holds the PPC big lock. Generated code
// keeps EVERY register in the global struct, so a thread that releases the
// lock mid-execution (to let another guest thread run) MUST snapshot its
// context first and restore it after reacquiring — the other thread's
// generated code overwrites every g_cpu field while it runs.
// Pattern at every handoff site:
//     PPCContext saved_ctx = g_cpu;
//     ppc_lock_release(); <wait>; ppc_lock_acquire();
//     g_cpu = saved_ctx;

// ---------------------------------------------------------------------------
// OS message queues -- real ring-buffer implementation.
// The Wii OSMessageQueue struct sits in guest memory; we shadow it with a
// host-side table keyed by guest VA.
// OSMessage on Wii is a void* (4-byte guest pointer).
// ---------------------------------------------------------------------------
#define MQ_TABLE_SIZE  64
#define MQ_MAX_MSGS   256

typedef struct {
    uint32_t va;           // guest queue VA (key)
    uint32_t buf[MQ_MAX_MSGS]; // ring buffer of message values
    int      head, tail, count, cap;
    SDL_sem *not_empty;    // signaled when a message is available
    SDL_sem *not_full;     // signaled when space is available
} HostMQ;

static HostMQ    g_mq[MQ_TABLE_SIZE];
static int       g_mq_count = 0;
static SDL_mutex *g_mq_cs;
static int       g_mq_cs_ready = 0;

static void mq_global_init(void) {
    if (!g_mq_cs_ready) {
        g_mq_cs = SDL_CreateMutex();
        g_mq_cs_ready = 1;
    }
}

static HostMQ *mq_find(uint32_t va) {
    for (int i = 0; i < g_mq_count; ++i)
        if (g_mq[i].va == va) return &g_mq[i];
    return NULL;
}

static HostMQ *mq_find_or_create(uint32_t va, int cap) {
    HostMQ *mq = mq_find(va);
    if (mq) return mq;
    if (g_mq_count >= MQ_TABLE_SIZE) return NULL;
    mq = &g_mq[g_mq_count++];
    mq->va   = va;
    mq->head = mq->tail = mq->count = 0;
    mq->cap  = (cap > 0 && cap <= MQ_MAX_MSGS) ? cap : MQ_MAX_MSGS;
    mq->not_empty = evt_create(0);
    mq->not_full  = evt_create(1);
    return mq;
}

void hle_OSInitMessageQueue(void) {
    mq_global_init();
    ppc_lock_init();
    uint32_t queue_va = HLE_ARG_U32(0);
    /* r4 = msg buffer va (we manage internally), r5 = capacity */
    int cap = (int)HLE_ARG_U32(2);
    SDL_LockMutex(g_mq_cs);
    mq_find_or_create(queue_va, cap);
    SDL_UnlockMutex(g_mq_cs);
    fprintf(stderr, "[MQ] InitMessageQueue va=0x%08x cap=%d\n", queue_va, cap);
    fflush(stderr);
    HLE_RET(0);
}

void hle_OSSendMessage(void) {
    uint32_t queue_va = HLE_ARG_U32(0);
    uint32_t msg      = HLE_ARG_U32(1);  // guest pointer / value
    uint32_t flags    = HLE_ARG_U32(2);  // 0=non-block, 1=block

    mq_global_init();
    SDL_LockMutex(g_mq_cs);
    HostMQ *mq = mq_find_or_create(queue_va, MQ_MAX_MSGS);
    SDL_UnlockMutex(g_mq_cs);
    if (!mq) { HLE_RET(0); return; }

    if (flags & 1) {
        // Blocking: wait until space available. Another guest thread may run
        // (and clobber g_cpu) while we hold the lock released — save/restore.
        PPCContext saved_ctx = g_cpu;
        while (1) {
            SDL_LockMutex(g_mq_cs);
            int full = (mq->count >= mq->cap);
            SDL_UnlockMutex(g_mq_cs);
            if (!full) break;
            ppc_lock_release();
            evt_wait(mq->not_full);
            ppc_lock_acquire();
        }
        g_cpu = saved_ctx;
    }

    SDL_LockMutex(g_mq_cs);
    if (mq->count < mq->cap) {
        mq->buf[mq->tail] = msg;
        mq->tail = (mq->tail + 1) % mq->cap;
        mq->count++;
        evt_signal(mq->not_empty);
        SDL_UnlockMutex(g_mq_cs);
        HLE_RET(1);
    } else {
        SDL_UnlockMutex(g_mq_cs);
        HLE_RET(0);
    }
}

void hle_OSJamMessage(void) {
    // Insert at front (jam = priority message)
    uint32_t queue_va = HLE_ARG_U32(0);
    uint32_t msg      = HLE_ARG_U32(1);
    mq_global_init();
    SDL_LockMutex(g_mq_cs);
    HostMQ *mq = mq_find_or_create(queue_va, MQ_MAX_MSGS);
    if (mq && mq->count < mq->cap) {
        mq->head = (mq->head - 1 + mq->cap) % mq->cap;
        mq->buf[mq->head] = msg;
        mq->count++;
        evt_signal(mq->not_empty);
    }
    SDL_UnlockMutex(g_mq_cs);
    HLE_RET(1);
}

void hle_OSReceiveMessage(void) {
    uint32_t queue_va = HLE_ARG_U32(0);
    uint32_t out_va   = HLE_ARG_U32(1);  // where to store the message
    uint32_t flags    = HLE_ARG_U32(2);  // 0=non-block, 1=block

    mq_global_init();
    SDL_LockMutex(g_mq_cs);
    HostMQ *mq = mq_find_or_create(queue_va, MQ_MAX_MSGS);
    SDL_UnlockMutex(g_mq_cs);
    if (!mq) { HLE_RET(0); return; }

    if (flags & 1) {
        // Blocking: release PPC lock and wait for a message. Save/restore
        // g_cpu — another guest thread runs while the lock is released.
        PPCContext saved_ctx = g_cpu;
        while (1) {
            SDL_LockMutex(g_mq_cs);
            int has = (mq->count > 0);
            SDL_UnlockMutex(g_mq_cs);
            if (has) break;
            ppc_lock_release();
            evt_wait(mq->not_empty);
            ppc_lock_acquire();
        }
        g_cpu = saved_ctx;
    }

    SDL_LockMutex(g_mq_cs);
    if (mq->count > 0) {
        uint32_t val = mq->buf[mq->head];
        mq->head = (mq->head + 1) % mq->cap;
        mq->count--;
        evt_signal(mq->not_full);
        SDL_UnlockMutex(g_mq_cs);
        if (out_va) MEM_W32(out_va, val);
        HLE_RET(1);
    } else {
        SDL_UnlockMutex(g_mq_cs);
        HLE_RET(0);
    }
}

// ---------------------------------------------------------------------------
// Counting semaphores
// ---------------------------------------------------------------------------
#define SEM_TABLE_SIZE 64
typedef struct { uint32_t va; SDL_sem *h; } HostSem;
static HostSem g_sems[SEM_TABLE_SIZE];
static int     g_sem_count = 0;

static HostSem *sem_find_or_create(uint32_t va, int init_val) {
    for (int i = 0; i < g_sem_count; ++i)
        if (g_sems[i].va == va) return &g_sems[i];
    if (g_sem_count >= SEM_TABLE_SIZE) return NULL;
    HostSem *s = &g_sems[g_sem_count++];
    s->va = va;
    s->h  = SDL_CreateSemaphore(init_val > 0 ? (Uint32)init_val : 0u);
    return s;
}

void hle_OSInitSemaphore(void) {
    uint32_t sem_va   = HLE_ARG_U32(0);
    int      init_val = (int)HLE_ARG_S32(1);
    sem_find_or_create(sem_va, init_val);
    HLE_RET(0);
}

void hle_OSWaitSemaphore(void) {
    uint32_t sem_va = HLE_ARG_U32(0);
    HostSem *s = sem_find_or_create(sem_va, 0);
    if (s) {
        PPCContext saved_ctx = g_cpu;   // another guest thread may run meanwhile
        ppc_lock_release();
        SDL_SemWait(s->h);
        ppc_lock_acquire();
        g_cpu = saved_ctx;
    }
    HLE_RET(0);
}

void hle_OSSignalSemaphore(void) {
    uint32_t sem_va = HLE_ARG_U32(0);
    HostSem *s = sem_find_or_create(sem_va, 0);
    if (s) SDL_SemPost(s->h);
    HLE_RET(0);
}

typedef struct {
    uint32_t entry;
    uint32_t param;
    uint32_t stack_top;
} HostThreadArgs;

// Start handshake: bumped by each worker the moment it holds the PPC lock.
// hle_OSResumeThread waits on this so a new worker runs its prologue (which
// reads argument blocks from the CREATOR's still-live stack frame) before the
// creator proceeds — matching real HW, where the prio-16 worker preempts at
// the creator's OSYieldThread. Without this the LoadList worker read a stale
// PTMF and the menu load never ran (see MENU_LOAD_BLOCKER.md).
static SDL_atomic_t g_host_workers_started;
/* Number of host worker threads currently EXECUTING guest code. NOTE: the
 * DVD stream worker (0x80264f20) lives for the whole session, so this never
 * reaches zero — liveness checks for the LOADLIST handshake must use
 * g_host_loadlist_alive (counts only the 0x801712f0 LoadList worker). */
SDL_atomic_t g_host_workers_alive;
/* LoadList worker (entry 0x801712f0) instances currently alive. Read by the
 * finish-handshake rescue (funcs_0006 [APOST-RESCUE]): main-thread AsyncPost
 * being told to wait for the worker (state2048==2) while this is zero means
 * the ping-pong desynced as the worker exited — a permanent park. */
SDL_atomic_t g_host_loadlist_alive;

static int SDLCALL host_thread_wrapper(void *p) {
    HostThreadArgs *a = (HostThreadArgs *)p;
    ppc_lock_acquire();  // wait until main thread yields
    SDL_AtomicIncRef(&g_host_workers_started);
    SDL_AtomicIncRef(&g_host_workers_alive);
    if (a->entry == 0x801712f0u) SDL_AtomicIncRef(&g_host_loadlist_alive);
    g_cpu.gpr[1] = (a->stack_top - 0x40) & ~0xFu;
    g_cpu.gpr[3] = a->param;
    g_cpu.lr     = 0xDEADBEEFu;
    fprintf(stderr, "[OS-host] worker thread running entry=0x%08x param=0x%08x sp=0x%08x\n",
            a->entry, a->param, g_cpu.gpr[1]);
    fflush(stderr);
    ppc_call_indirect(a->entry);
    fprintf(stderr, "[OS-host] worker thread returned (entry=0x%08x)\n", a->entry);
    fflush(stderr);
    SDL_AtomicDecRef(&g_host_workers_alive);
    if (a->entry == 0x801712f0u) SDL_AtomicDecRef(&g_host_loadlist_alive);
    ppc_lock_release();
    free(a);
    return 0;
}

// Set while the LoadList worker (0x801712f0) runs inline. Tells
// hle_OSYieldThread to pump the STD_IO read-completion idle callback so the
// worker's async load reads actually finalize (cooperative scheduling). See
// [[boot-sequence]] and the OSResumeThread inline-run block below.
volatile int g_inline_loader_active = 0;
volatile uint32_t g_yield_pump_count = 0;

// Known worker thread entry points for Rabbids Go Home.
// VID_MailHandler: video command thread — blocks on OSReceiveMessage,
// must run as a real host thread so the main thread can send it messages.
static const uint32_t g_rgh_workers[] = {
    /* No hardcoded workers: per-game worker entries come from the active
       quirks profile (quirks/<game>.c) or RECOMP_WORKER_ENTRIES. The old
       RGH VAs collide with other games' address space. */
    0u,
};

static int entry_is_worker(uint32_t entry) {
    for (size_t i = 0; i < sizeof(g_rgh_workers)/sizeof(g_rgh_workers[0]); ++i)
        if (g_rgh_workers[i] == entry) return 1;
    const quirks_t *q = quirks_active();
    if (q) {
        for (size_t i = 0; i < q->worker_entry_count; ++i) {
            if (q->worker_entries[i] == entry) return 1;
        }
    }
    const char *override = getenv("RECOMP_WORKER_ENTRIES");
    if (override && *override) {
        const char *p = override;
        while (*p) {
            uint32_t va = (uint32_t)strtoul(p, (char**)&p, 0);
            if (va == entry) return 1;
            while (*p == ',' || *p == ' ') ++p;
        }
    }
    return 0;
}

// Revolution SDK OSThread struct field offsets (from WiiBrew/Revolution_OS).
// We don't store the full context here -- we only initialize the fields
// games are likely to read so a sanity-check doesn't trip on uninit zeros.
//   0x2C8  u16 state         (0=stopped, 1=ready, 2=active, 4=sleeping, 8=joinable)
//   0x2CA  u16 detached      (1 = detached, 0 = joinable)
//   0x2CC  u32 suspend_count
//   0x2D0  s32 priority
//   0x2D4  s32 effective_prio
//   0x304  void *stack_base
//   0x308  void *stack_end
#define OST_STATE_OFF       0x2C8
#define OST_DETACHED_OFF    0x2CA
#define OST_SUSPEND_OFF     0x2CC
#define OST_PRIO_OFF        0x2D0
#define OST_EFFPRIO_OFF     0x2D4
#define OST_STACK_BASE_OFF  0x304
#define OST_STACK_END_OFF   0x308

#define OST_STATE_STOPPED   0
#define OST_STATE_READY     1
#define OST_STATE_ACTIVE    2

void hle_OSCreateThread(void) {
    ppc_lock_init();  // ensure PPC lock exists before any thread is created
    // Revolution SDK signature:
    //   BOOL OSCreateThread(OSThread *thread, void *(*entry)(void*),
    //                       void *param, void *stack, u32 stackSize,
    //                       OSPriority priority, u16 attr);
    uint32_t thread_va = HLE_ARG_U32(0);   // r3 — OSThread* (the handle)
    uint32_t entry     = HLE_ARG_U32(1);
    uint32_t param     = HLE_ARG_U32(2);
    uint32_t stack     = HLE_ARG_U32(3);
    uint32_t stack_sz  = HLE_ARG_U32(4);
    int32_t  prio      = HLE_ARG_S32(5);
    fprintf(stderr, "[OS] CreateThread entry=0x%08x stack=0x%08x+0x%x prio=%d\n",
            entry, stack, stack_sz, prio);
    fflush(stderr);

    // Initialize the OSThread struct so games that introspect it don't
    // read uninitialized memory. Specifically, state=READY tells the
    // scheduler this thread can be resumed; without it some games refuse
    // to call OSResumeThread.
    if (thread_va) {
        MEM_W16(thread_va + OST_STATE_OFF,      OST_STATE_READY);
        MEM_W16(thread_va + OST_DETACHED_OFF,   0);
        MEM_W32(thread_va + OST_SUSPEND_OFF,    1);    // starts suspended
        MEM_W32(thread_va + OST_PRIO_OFF,       (uint32_t)prio);
        MEM_W32(thread_va + OST_EFFPRIO_OFF,    (uint32_t)prio);
        MEM_W32(thread_va + OST_STACK_BASE_OFF, stack + stack_sz);
        MEM_W32(thread_va + OST_STACK_END_OFF,  stack);
    }

    for (int i = 0; i < MAX_THREADS; ++i) {
        if (g_threads[i].in_use) continue;
        g_threads[i].in_use   = 1;
        // Use the OSThread* VA as the handle. This is what games pass
        // to OSResumeThread / OSSuspendThread / OSJoinThread / etc.,
        // so the lookup will actually find the thread. Old behavior
        // used an auto-incremented integer that nobody ever passed in.
        g_threads[i].handle   = thread_va ? thread_va : (g_next_thread_handle++);
        g_threads[i].entry    = entry;
        g_threads[i].param    = param;
        g_threads[i].stack_top= stack + stack_sz;
        g_threads[i].prio     = (uint32_t)prio;
        g_threads[i].detached = 0;
        // Revolution SDK OSCreateThread returns BOOL: 1=success, 0=failure.
        // (Old behavior returned the synthetic handle; games don't use the
        // return value as a handle, only as a truthy/falsy check.)
        HLE_RET(1);
        return;
    }
    HLE_RET(0);   // table full
}

void hle_OSResumeThread(void) {
    uint32_t handle = HLE_ARG_U32(0);
    for (int i = 0; i < MAX_THREADS; ++i) {
        if (!g_threads[i].in_use || g_threads[i].handle != handle) continue;
        const char *run_env = getenv("RECOMP_RUN_THREADS");
        int run_inline = (run_env && run_env[0] && run_env[0] != '0');
        int is_worker  = entry_is_worker(g_threads[i].entry);
        // Threads that are self-terminating (they do a bounded unit of work
        // and return via blr) must run INLINE to completion, not on a host
        // thread: g_cpu is global with no per-thread context, so a host thread
        // races/corrupts the main thread's state. Run them here with a full
        // CPU-context save/restore.
        //   0x80429d74 = RVL_SplashScreenThread: plays the whole logo sequence.
        //   0x801712f0 = LoadList worker (LoadListCreateThread's entry): locks,
        //                does one WOR load pass (manager vtable +0x24/+0x70/+0x74),
        //                unlocks, returns. The main thread parks in the LoadList
        //                state-2 action (LoadList_RunYield -> OSYieldThread)
        //                waiting for this worker; without it the Map Startup
        //                (menu) load hangs at state 2 forever. See [[boot-sequence]].
        // 0x801712f0 (LoadList worker): running it fully inline used to DEADLOCK
        // -- it spins in func_8009c9d0 `while (mgr.state2048 != 2) { vtable+0x1c:
        // OSYieldThread + load-step; }` and state->2 never fired because the
        // load-step's async reads only get finalized by the STD_IO read-
        // completion idle callback, which normally runs on the (blocked) main
        // thread. FIX: while g_inline_loader_active, hle_OSYieldThread pumps that
        // idle callback -- and the worker calls OSYieldThread every spin
        // iteration -- so the reads complete cooperatively and the load advances.
        // Default ON; disable with RECOMP_INLINE_LOADER=0. See [[boot-sequence]].
        int inline_this = (g_threads[i].entry == 0x80429d74u);
        if (g_threads[i].entry == 0x801712f0u) {
            const char *le = getenv("RECOMP_INLINE_LOADER");
            inline_this = (le && le[0] && le[0] != '0');   // default OFF (hangs; still investigating)
        }
        if (inline_this) {
            fprintf(stderr, "[OS] running thread 0x%08x inline to completion\n",
                    g_threads[i].entry);
            fflush(stderr);
            int is_loader = (g_threads[i].entry == 0x801712f0u);
            PPCContext saved = g_cpu;
            g_cpu.gpr[1] = (g_threads[i].stack_top - 0x40) & ~0xFu;
            g_cpu.gpr[3] = g_threads[i].param;
            g_cpu.lr     = 0xDEADBEEFu;
            if (is_loader) { g_inline_loader_active = 1; g_yield_pump_count = 0; }
            ppc_call_indirect(g_threads[i].entry);
            if (is_loader) g_inline_loader_active = 0;
            g_cpu = saved;
            if (handle) {
                MEM_W16(handle + OST_STATE_OFF, OST_STATE_ACTIVE);
                MEM_W32(handle + OST_SUSPEND_OFF, 0);
            }
            HLE_RET(0);
            return;
        }
        if (is_worker) {
            HostThreadArgs *a = (HostThreadArgs *)malloc(sizeof(HostThreadArgs));
            a->entry = g_threads[i].entry;
            a->param = g_threads[i].param;
            a->stack_top = g_threads[i].stack_top;
            // LoadList worker: the game gives it a heap-allocated 16KB stack
            // (0x907569e0+0x4000) that the game's own allocator ALSO hands out
            // to the LOA pending-ref table (0x57e40 @ 0x90756b30) — the table
            // fill then scribbles the worker's frames (r30->0, dead spin).
            // Give it a private stack in the reserved guest region instead
            // (below the relocated main stack @0x816e0000, above RSO bss).
            if (a->entry == 0x801712f0u) a->stack_top = 0x815c0000u;
            int started_before = SDL_AtomicGet(&g_host_workers_started);
            // Guest worker stacks live in guest memory, but the HOST stack this
            // thread gets must be large: deep guest call chains become deep host
            // call chains (same reason the Windows main thread reserves 512 MB
            // via -Wl,--stack). SDL's default would be ~1 MB on Android.
            SDL_Thread *h = SDL_CreateThreadWithStackSize(
                host_thread_wrapper, "robox-worker", ROBOX_GUEST_STACK_BYTES, a);
            if (h) {
                SDL_DetachThread(h);
                // Hand the CPU to the new worker NOW (real HW: the prio-16
                // worker preempts the creator). Its argument block may live in
                // the creator's stack frame, so it must run its prologue
                // before we return. Wait for it to take the PPC lock; our
                // re-acquire then blocks until its first yield/exit.
                PPCContext saved_ctx = g_cpu;
                ppc_lock_release();
                for (int spin = 0; SDL_AtomicGet(&g_host_workers_started) == started_before; ++spin) {
                    SDL_Delay(1);
                    if (spin == 5000) {
                        fprintf(stderr, "[OS] worker 0x%08x never started (5s) — continuing\n",
                                g_threads[i].entry);
                        fflush(stderr);
                        break;
                    }
                }
                ppc_lock_acquire();
                g_cpu = saved_ctx;
                fprintf(stderr, "[OS] worker handshake done (entry=0x%08x)\n",
                        g_threads[i].entry);
                fflush(stderr);
            }
        } else if (run_inline) {
            uint32_t saved_lr = g_cpu.lr; uint32_t saved_sp = g_cpu.gpr[1];
            g_cpu.gpr[1] = (g_threads[i].stack_top - 0x40) & ~0xFu;
            g_cpu.gpr[3] = g_threads[i].param;
            ppc_call_indirect(g_threads[i].entry);
            g_cpu.lr = saved_lr; g_cpu.gpr[1] = saved_sp;
        }
        // Mark the OSThread struct as ACTIVE so any later poll
        // sees the correct state.
        if (handle) {
            MEM_W16(handle + OST_STATE_OFF, OST_STATE_ACTIVE);
            MEM_W32(handle + OST_SUSPEND_OFF, 0);
        }
        // else: skip the thread body. HLE Wait/Receive will pretend the
        // worker already completed.
        HLE_RET(0);
        return;
    }
    HLE_RET(0);
}

void hle_OSCancelThread(void) {
    uint32_t handle = HLE_ARG_U32(0);
    for (int i = 0; i < MAX_THREADS; ++i) {
        if (g_threads[i].in_use && g_threads[i].handle == handle) {
            g_threads[i].in_use = 0;
            if (handle) MEM_W16(handle + OST_STATE_OFF, OST_STATE_STOPPED);
            break;
        }
    }
    HLE_RET(0);
}

void hle_OSDetachThread(void) {
    uint32_t handle = HLE_ARG_U32(0);
    for (int i = 0; i < MAX_THREADS; ++i) {
        if (g_threads[i].in_use && g_threads[i].handle == handle) {
            g_threads[i].detached = 1;
            if (handle) MEM_W16(handle + OST_DETACHED_OFF, 1);
            break;
        }
    }
    HLE_RET(0);
}

void hle_OSExitThread(void) {
    // Caller is the thread that's exiting. Mark our current thread
    // as stopped (we don't track which one is current; just set the
    // exit code 0 in the OSThread struct via r3 if the SDK uses that
    // calling convention -- it doesn't always).
    HLE_RET(0);
}

void hle_OSJoinThread(void) {
    // Single-threaded host model: by the time anyone joins, the worker
    // either ran inline (already done) or never ran (RECOMP_RUN_THREADS
    // off). Either way, return immediately with success.
    HLE_RET(0);
}

void hle_OSSleepThread(void) {
    // OSSleepThread parks the caller until OSWakeupThread. We can't park
    // precisely; yield the PPC big lock once so other guest threads make
    // progress, then return and let the caller's retry loop spin-yield.
    if (g_ppc_cs_ready) {
        PPCContext saved_ctx = g_cpu;
        ppc_lock_release();
        SDL_Delay(1);
        ppc_lock_acquire();
        g_cpu = saved_ctx;
    }
    HLE_RET(0);
}
void hle_OSWakeupThread(void)        { HLE_RET(0); }
void hle_OSYieldThread(void) {
    // Cooperative pump for the inline LoadList worker: while it spins waiting
    // for its load to finish (calling OSYieldThread each iteration), drive the
    // STD_IO read-completion idle callback (SDA-0x65c8 = *0x806c27d8) so the
    // pending async reads finalize and the load actually progresses. Guarded
    // against reentrancy; g_cpu is saved/restored so only memory side effects
    // (completed reads) survive, not register clobber.
    if (g_inline_loader_active) {
        static int in_pump = 0;
        g_yield_pump_count++;
        if (!in_pump) {
            uint32_t idle_cb = MEM_R32(0x806c27d8u);
            if (idle_cb >= 0x80003000u && idle_cb < 0x81800000u) {
                in_pump = 1;
                PPCContext saved = g_cpu;
                g_cpu.lr = 0xDEAD0002u;
                ppc_call_indirect(idle_cb);
                g_cpu = saved;
                in_pump = 0;
            }
            // Safety net: if pumping still hasn't completed the load after a
            // large number of yields, force the worker's spin to exit by
            // setting mgr.state2048 = 2 so we don't hard-hang. (Better a
            // possibly-incomplete load than a frozen game.)
            if (g_yield_pump_count == 400000u) {
                extern uint32_t g_wor_mgr;
                if (g_wor_mgr >= 0x80003000u && g_wor_mgr < 0x94000000u) {
                    fprintf(stderr, "[OS] inline loader still spinning after 400k "
                            "yields -> forcing mgr.state2048=2 to avoid hang\n");
                    fflush(stderr);
                    MEM_W32(g_wor_mgr + 0x2048, 2);
                }
            }
        }
    }
    // REAL yield: hand the PPC big lock to any other runnable guest thread.
    // The MapStartup (menu) load is a producer/consumer handshake — the main
    // thread parks here at LoadList state 2 (LoadList_RunYield) while the
    // LoadList worker (0x801712f0, real host thread) consumes the state and
    // runs LoadList_Do; the worker's own spin also yields here each
    // iteration, handing the lock back to main. g_cpu belongs to the lock
    // holder, so snapshot/restore around the handoff.
    if (g_ppc_cs_ready) {
        /* [YIELD] probe: confirm both threads actually pass through here */
        {
            static volatile unsigned s_n;
            unsigned n = ++s_n;
            if (n < 8 || (n % 100000u) == 0) {
                fprintf(stderr, "[YIELD] #%u tid=%lu\n", n, (unsigned long)SDL_ThreadID());
                fflush(stderr);
            }
        }
        PPCContext saved_ctx = g_cpu;
        ppc_lock_release();
        // SDL_Delay(1), not 0: the mutex is unfair — with a 0 delay the
        // yielding thread re-grabs it before the blocked peer ever wakes,
        // starving it forever (seen as the LoadList worker spinning at
        // 0x8009cb78 while main never ran its continuation). A real 1ms
        // window lets the waiter in.
        SDL_Delay(1);
        ppc_lock_acquire();
        g_cpu = saved_ctx;
    }
    HLE_RET(0);
}
void hle_OSSetThreadPriority(void)   { HLE_RET(1); }
void hle_OSGetThreadPriority(void)   { HLE_RET(16); }
void hle_OSGetCurrentThread(void)    { HLE_RET(DUMMY_THREAD_VA); }
void hle_OSInitThreadQueue(void)     { HLE_RET(0); }
void hle_OSContinueThread(void)      { HLE_RET(0); }

void hle_OSSuspendThread(void) {
    uint32_t handle = HLE_ARG_U32(0);
    if (handle) {
        // Increment the OSThread.suspend counter so subsequent resume
        // calls track properly. Don't actually pause anything; we're
        // single-threaded.
        uint32_t cur = MEM_R32(handle + OST_SUSPEND_OFF);
        MEM_W32(handle + OST_SUSPEND_OFF, cur + 1);
    }
    HLE_RET(0);
}

void hle_OSCheckActiveThreads(void)  { HLE_RET(1); }


// ---------------------------------------------------------------------------
// Reset / shutdown
// ---------------------------------------------------------------------------

void hle_OSResetSystem(void)    { fprintf(stderr, "[OS] OSResetSystem -- exiting\n"); exit(0); }
void hle_OSReturnToMenu(void)   { fprintf(stderr, "[OS] OSReturnToMenu -- exiting\n"); exit(0); }
void hle_OSGetResetCode(void)   { HLE_RET(0); }
void hle_OSGetLaunchCode(void)  { HLE_RET(0); }
void hle_OSGetReturnCode(void)  { HLE_RET(0); }


// ---------------------------------------------------------------------------
// Misc small helpers.
// ---------------------------------------------------------------------------

void hle_OSCalcCRC32(void)           { HLE_RET(0); }
// Console type constants from Revolution SDK (OS.h):
//   OS_CONSOLE_RETAIL_MASK   = 0x10000000  (zero for retail systems)
//   OS_CONSOLE_RETAIL3       = 0x00000003  (retail Wii)
//   OS_CONSOLE_RETAIL4       = 0x00000004  (retail Wii, later boards)
//   OS_CONSOLE_NDEV_2_1      = 0x10000021  (dev kit)
//   OS_CONSOLE_EMULATOR_PC   = 0x10000001  (Dolphin et al)
// We pretend to be retail. Some games gate-check this for behavior
// differences: NDEV builds get extra debug overlays, emulator builds
// might skip security checks. Returning 0 (the old GameCube ARTHUR
// board ID) is wrong and asserts in OSInit on some SDK versions.
void hle_OSGetConsoleType(void)      { HLE_RET(0x00000003); }   // OS_CONSOLE_RETAIL3
void hle_OSGetSystemInfo(void)       { HLE_RET(0); }
void hle_OSGetPhysicalMem1Size(void) { HLE_RET(PPC_MEM1_SIZE); }
void hle_OSGetPhysicalMem2Size(void) { HLE_RET(PPC_MEM2_SIZE); }
void hle_OSGetSimulatedMem1Size(void){ HLE_RET(PPC_MEM1_SIZE); }
void hle_OSGetSimulatedMem2Size(void){ HLE_RET(PPC_MEM2_SIZE); }
void hle_OSGetConsoleSimMemSize(void){ HLE_RET(PPC_MEM1_SIZE + PPC_MEM2_SIZE); }
