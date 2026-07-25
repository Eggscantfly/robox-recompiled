// src/runtime.h -- PPC recomp runtime surface consumed by every generated
// function and by sdk/. The generated code is basically a huge pile of
// assignments against g_cpu and MEM_* macros; this header defines both.
//
// Scaffolded by recompile_wii.py from templates/runtime.h.tmpl. Safe to
// edit: the recompiler emits this only when missing. Delete the file to
// regenerate from the template.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <math.h>                         // fabs/fabsf/sqrt used by generated code

// ---------------------------------------------------------------------------
// PPC_TAIL — guaranteed tail call for the generated branch-to-another-function
// edges.
//
// The recompiler turns a guest branch that leaves a function into a call
// followed by a return. For a BACKWARD branch that is a loop back-edge, so it
// only works if the compiler converts it into a jump; otherwise every guest
// loop iteration pushes a host frame and eventually overflows the stack. Until
// now that was an unwritten dependency on GCC's -O2 sibling-call optimisation:
// correct by luck, silent when it failed, and the reason the build could never
// be compiled -O0 and the main thread reserves 512 MB of stack.
//
// `musttail` makes it explicit. The compiler MUST emit a real tail call or fail
// to compile, so a lost optimisation becomes a build error instead of a
// runtime stack overflow. It also holds at -O0, which finally makes debug
// builds possible, and it is what lets the WebAssembly target work at all
// (measured: without it a 5M-iteration guest loop overflows the wasm stack;
// with it, it completes at both -O0 and -O2).
//
// Compilers without the attribute (MSYS2 GCC 14.2 among them) expand this to
// nothing and keep relying on -O2 sibcalls exactly as before — no change in
// behaviour, no regression.
// ---------------------------------------------------------------------------
#if defined(__has_attribute)
#  if __has_attribute(musttail)
#    define PPC_TAIL __attribute__((musttail))
#  endif
#endif
#ifndef PPC_TAIL
#  define PPC_TAIL
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Guest memory layout. Broadway VAs; the cached/uncached aliases of MEM1 are
// folded onto the same host buffer in ppc_host_ptr.
// ---------------------------------------------------------------------------

#define PPC_MEM1_BASE   0x80000000u
#define PPC_MEM1_SIZE   0x01800000u       // 24 MB (real Wii)
#define PPC_MEM1_UNCACHED_BASE 0xC0000000u
#define PPC_MEM2_BASE   0x90000000u
#define PPC_MEM2_UNCACHED_BASE 0xD0000000u
// Real Wii MEM2 is 64 MB. The default matches hardware; sdk/os.c's first-fit
// allocator is correct, so games that do bounded MEM2 allocation get the
// same address layout they would on a real Wii. If a specific game does
// truly enormous transient allocations (rare), bump this with
// -DPPC_MEM2_SIZE=0x10000000 to get 256 MB. Reserving more virtual memory
// is free until written, but it can hide real game-side leaks.
#ifndef PPC_MEM2_SIZE
#define PPC_MEM2_SIZE   0x04000000u       // 64 MB — matches hardware
#endif

// Deadzone for unmapped VAs. Reads return 0, writes go to /dev/null.
// Was 64 KB — but host-side bulk ops (fread, memcpy) handed a deadzone
// pointer with a multi-MB size overran it into the ADJACENT BSS globals
// (g_mem2's base pointer got a stray byte -> every later ppc_host_ptr
// wild -> AV). 16 MB of BSS is free until touched and swallows any
// realistic overrun. Offsets are still masked to this size in host_ptr.
#if defined(__3DS__)
// The 3DS cannot spare 16 MB of BSS for overrun headroom. 1 MB still folds every
// realistic wild access without faulting; it just aliases more of them together
// (wild accesses are already bugs, counted by g_deadzone_hits). Power-of-2 kept
// so the host_ptr fold-mask holds.
#define PPC_DEADZONE_SIZE   0x100000u
#else
#define PPC_DEADZONE_SIZE   0x1000000u
#endif

extern uint8_t *g_mem1;                   // PPC_MEM1_SIZE bytes
extern uint8_t *g_mem2;                   // PPC_MEM2_SIZE bytes
extern uint8_t  g_deadzone[PPC_DEADZONE_SIZE];

// VA -> host pointer. Returns a pointer into g_mem1/g_mem2 for mapped
// addresses and into g_deadzone for anything else. Never returns NULL;
// sdk code and generated code both rely on that.
void *ppc_host_ptr(uint32_t va);


// ---------------------------------------------------------------------------
// Endian-swapping guest memory access. Generated code funnels every load/
// store through these so we don't need to byte-swap anything at load time.
// ---------------------------------------------------------------------------

static inline uint32_t _ppc_bswap32(uint32_t x) {
    return ((x & 0xFFu) << 24) | ((x & 0xFF00u) << 8)
         | ((x & 0xFF0000u) >> 8) | ((x & 0xFF000000u) >> 24);
}
static inline uint16_t _ppc_bswap16(uint16_t x) {
    return (uint16_t)(((x & 0xFFu) << 8) | ((x & 0xFF00u) >> 8));
}
static inline uint64_t _ppc_bswap64(uint64_t x) {
    return ((uint64_t)_ppc_bswap32((uint32_t)x) << 32)
         | (uint64_t)_ppc_bswap32((uint32_t)(x >> 32));
}

/* INVESTIGATE-ONLY (revert before regen): SCR2CPP enable-flag watch decls
 * forward-declared above MEM_R32/MEM_W32 so the inline checks compile. */
void ppc_watch_scr2cpp_r32(uint32_t v);
void ppc_watch_scr2cpp_w32(uint32_t v);

/* INVESTIGATE-ONLY: SCR wait-timer write watch.
 * After SCR_b_CheckMetaWait first sees a wait_va, it arms this and every
 * subsequent write at that VA logs new value + LR. */
extern uint32_t g_scr_wait_watch_va;
void ppc_watch_scr_wait_w(uint32_t va, float value);

/* INVESTIGATE-ONLY: log a vtable/indirect dispatch target with categorization.
 * Used inside ViD::OneFrame to see whether each bctrl resolves into RSO,
 * main ELF, a stub, or something else. */
void ppc_watch_vtbl(uint32_t site_pc, uint32_t target);
/* INVESTIGATE-ONLY: log first time an RSO-range function is entered
 * (called from inside ppc_call_indirect when target >= 0x81000000). */
void ppc_watch_rso_enter(uint32_t target);

/* INVESTIGATE-ONLY: simple tagged log helper for boot-trace events. */
void ppc_dbg_tag(const char *tag);
void ppc_dbg_tag_str(const char *tag, uint32_t guest_strptr);
void ppc_dbg_tag_u32(const char *tag, uint32_t value);
void ppc_dbg_tag_str_u32(const char *tag, uint32_t guest_strptr, uint32_t value);

static inline uint8_t  MEM_R8 (uint32_t va) { return *(uint8_t *)ppc_host_ptr(va); }
static inline uint16_t MEM_R16(uint32_t va) {
    uint16_t v; __builtin_memcpy(&v, ppc_host_ptr(va), 2); return _ppc_bswap16(v);
}
static inline uint32_t MEM_R32(uint32_t va) {
    uint32_t v; __builtin_memcpy(&v, ppc_host_ptr(va), 4);
    uint32_t hv = _ppc_bswap32(v);
    return hv;
}
static inline uint64_t MEM_R64(uint32_t va) {
    uint64_t v; __builtin_memcpy(&v, ppc_host_ptr(va), 8); return _ppc_bswap64(v);
}

// MMIO hook. Writes into CP-FIFO / GX register VAs get forwarded into the
// FIFO interpreter in sdk/peripherals.c instead of touching guest memory.
void ppc_mmio_write(uint32_t va, const void *data, int nbytes);
bool ppc_mmio_va(uint32_t va);            // true if the VA is MMIO, not RAM

static inline void MEM_W8(uint32_t va, uint8_t v) {
    if (ppc_mmio_va(va)) { ppc_mmio_write(va, &v, 1); return; }
    *(uint8_t *)ppc_host_ptr(va) = v;
}
static inline void MEM_W16(uint32_t va, uint16_t v) {
    uint16_t be = _ppc_bswap16(v);
    if (ppc_mmio_va(va)) { ppc_mmio_write(va, &be, 2); return; }
    __builtin_memcpy(ppc_host_ptr(va), &be, 2);
}
/* INVESTIGATE-ONLY: current-world slot watch. mgr(SDA[-0x652c])=0x901023e0
 * every run (deterministic heap); [mgr+0xc]=0x901023ec is the current-WOR
 * pointer. The menu load stores a non-WOR object (raw record 0x9023a670)
 * there — log every store + LR to find the activation site. */
void ppc_watch_worcur_w(uint32_t v);

/* INVESTIGATE-ONLY: RVL_NandHelper NANDLastError cell (static obj 0x80589080,
 * [r13-0x579c]). Error 3 lands there early in boot with ZERO IOS traffic and
 * later parks RVL_BootUpFlow in state 36 (error dialog) — which deadlocks the
 * menu track (label-2 SAV_Step_C poll) and the menu-world loader job 2. */
void ppc_watch_nanderr_w(uint32_t v);

static inline void MEM_W32(uint32_t va, uint32_t v) {
    uint32_t be = _ppc_bswap32(v);
    if (ppc_mmio_va(va)) { ppc_mmio_write(va, &be, 4); return; }
    __builtin_memcpy(ppc_host_ptr(va), &be, 4);
}
static inline void MEM_W64(uint32_t va, uint64_t v) {
    uint64_t be = _ppc_bswap64(v);
    __builtin_memcpy(ppc_host_ptr(va), &be, 8);
}

// Float variants: guest stores IEEE-754 single/double in big-endian layout.
// Translate through the matching integer swap.
static inline float MEM_RF(uint32_t va) {
    uint32_t u = MEM_R32(va); float f; __builtin_memcpy(&f, &u, 4); return f;
}
static inline void MEM_WF(uint32_t va, float f) {
    uint32_t u; __builtin_memcpy(&u, &f, 4); MEM_W32(va, u);
}
static inline double MEM_RD(uint32_t va) {
    uint64_t u = MEM_R64(va); double d; __builtin_memcpy(&d, &u, 8); return d;
}
static inline void MEM_WD(uint32_t va, double d) {
    uint64_t u; __builtin_memcpy(&u, &d, 8); MEM_W64(va, u);
}


// ---------------------------------------------------------------------------
// PPC register state. Everything the generated code touches lives here.
// ---------------------------------------------------------------------------

/* FOUNDATIONAL FIX (black-menu root cause): ps[] was `float ps[2]` sharing
 * bytes with f64. Scalar ops write .f64 (double) but paired-single ops read
 * .ps[] — so after `lfs f3, =1.0` a ps_merge/psq_st saw the RAW LOW/HIGH
 * WORDS of double 1.0: ps[0]=0x00000000=0.0f, ps[1]=0x3ff00000=1.875f.
 * That is the systemic "0x3ff00000 / 1.875 double->float" corruption seen
 * all over (zeroed UI matrices -> invisible menu, 1.875x UI scale, NaN
 * projections). Making ps[] doubles turns f64 into an alias of ps[0] (same
 * bytes, same type): every existing .f64 and .ps[i] site becomes value-
 * coherent. Gekko ops that replicate a single result into BOTH halves
 * (lfs, fadds/fmuls/.../frsp) additionally dup into ps[1] — those dups are
 * inserted in the generated code by fix_fpr.py (rerun after any regen!). */
typedef union {
    double   f64;                         // scalar view == ps[0] (same bytes)
    uint64_t u64;                         // bit view of ps[0]
    double   ps[2];                       // paired singles (Broadway), ps[0] aliases f64
} PPCFpr;

typedef struct {
    uint32_t gpr[32];
    PPCFpr   fpr[32];
    uint32_t lr;
    uint32_t ctr;
    uint32_t xer;                         // bit 29 = CA, bit 30 = OV, bit 31 = SO
    uint32_t cr;                          // packed 8x4 fields
    uint32_t msr;
    uint32_t fpscr;
} PPCContext;

// Single-threaded: the game's own "threads" (DvdStreamThreadProc etc.)
// are skipped from OSResumeThread since their wait loops desync without
// the host-level timing they assume. Matches the AC PC-port approach:
// keep the guest deliberately single-threaded, and let main do all work.
extern PPCContext g_cpu;


// ---------------------------------------------------------------------------
// CR / XER helpers. The generated code calls these by name; keep the shapes
// exactly as the recompiler emits.
// ---------------------------------------------------------------------------

// CR0 bit positions (PPC big-endian bit numbering, top-bit first):
//   bit 31 = LT, 30 = GT, 29 = EQ, 28 = SO.
// CR field N occupies bits (31 - 4N) .. (28 - 4N).
#define CR0_FROM_RESULT(signed_result) do {                    \
    int32_t  _r = (int32_t)(signed_result);                    \
    uint32_t _c = (_r < 0) ? 8u : (_r > 0) ? 4u : 2u;          \
    /* SO copies from XER.SO (bit 31 of XER) */                \
    _c |= (g_cpu.xer >> 31) & 1u;                              \
    g_cpu.cr = (g_cpu.cr & 0x0FFFFFFFu) | (_c << 28);          \
} while (0)

#define CR_SET_SIGNED(field, a, b, xer_unused) do {            \
    int32_t  _a = (int32_t)(a), _b = (int32_t)(b);             \
    uint32_t _c = (_a < _b) ? 8u : (_a > _b) ? 4u : 2u;        \
    _c |= (g_cpu.xer >> 31) & 1u;                              \
    uint32_t _shift = 28u - 4u * (uint32_t)(field);            \
    g_cpu.cr = (g_cpu.cr & ~(0xFu << _shift)) | (_c << _shift);\
    (void)(xer_unused);                                        \
} while (0)

#define CR_SET_UNSIGNED(field, a, b, xer_unused) do {          \
    uint32_t _a = (uint32_t)(a), _b = (uint32_t)(b);           \
    uint32_t _c = (_a < _b) ? 8u : (_a > _b) ? 4u : 2u;        \
    _c |= (g_cpu.xer >> 31) & 1u;                              \
    uint32_t _shift = 28u - 4u * (uint32_t)(field);            \
    g_cpu.cr = (g_cpu.cr & ~(0xFu << _shift)) | (_c << _shift);\
    (void)(xer_unused);                                        \
} while (0)

// XER carry (bit 29 in big-endian numbering = bit 2 from MSB) lives at
// bit 29 of the 32-bit xer. Use positional-from-LSB: XER.CA is bit 29 counting
// from MSB = bit 2 from LSB. We pack it as bit 29 in our uint32 to match the
// architectural layout expected by mfxer/mtxer paths.
#define XER_CA_BIT   29u
#define XER_GET_CA() ((g_cpu.xer >> XER_CA_BIT) & 1u)
#define XER_SET_CA(cond) do {                                                 \
    if (cond) g_cpu.xer |=  (1u << XER_CA_BIT);                               \
    else      g_cpu.xer &= ~(1u << XER_CA_BIT);                               \
} while (0)


// ---------------------------------------------------------------------------
// Arithmetic helpers the recompiler inlines by name.
// ---------------------------------------------------------------------------

// ADDC: a + b, set XER.CA from 33rd bit.
static inline uint32_t ADDC(uint32_t a, uint32_t b) {
    uint64_t r = (uint64_t)a + (uint64_t)b;
    XER_SET_CA(r > 0xFFFFFFFFull);
    return (uint32_t)r;
}
// ADDE: a + b + CA, set CA from 33rd bit.
static inline uint32_t ADDE(uint32_t a, uint32_t b) {
    uint64_t r = (uint64_t)a + (uint64_t)b + (XER_GET_CA() ? 1u : 0u);
    XER_SET_CA(r > 0xFFFFFFFFull);
    return (uint32_t)r;
}
// SUBFC: b - a (i.e. ~a + b + 1), set CA from 33rd bit.
static inline uint32_t SUBFC(uint32_t a, uint32_t b) {
    uint64_t r = (uint64_t)(~a) + (uint64_t)b + 1ull;
    XER_SET_CA(r > 0xFFFFFFFFull);
    return (uint32_t)r;
}
// SUBFE: ~a + b + CA, set CA from 33rd bit.
static inline uint32_t SUBFE(uint32_t a, uint32_t b) {
    uint64_t r = (uint64_t)(~a) + (uint64_t)b + (XER_GET_CA() ? 1u : 0u);
    XER_SET_CA(r > 0xFFFFFFFFull);
    return (uint32_t)r;
}

static inline int32_t  MULHW (int32_t  a, int32_t  b) {
    return (int32_t)(((int64_t)a * (int64_t)b) >> 32);
}
static inline uint32_t MULHWU(uint32_t a, uint32_t b) {
    return (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32);
}
static inline uint32_t ROTL32(uint32_t x, uint32_t n) {
    n &= 31; return (x << n) | (x >> ((32u - n) & 31u));
}
static inline uint32_t CLZ32(uint32_t x) {
    if (!x) return 32u;
#ifdef __GNUC__
    return (uint32_t)__builtin_clz(x);
#else
    uint32_t n = 0; while (!(x & 0x80000000u)) { x <<= 1; ++n; } return n;
#endif
}

// FRSQRTE: Broadway's "reciprocal square root estimate" opcode. The HW is
// a low-precision estimate (~5 bits); we hand back a full IEEE result,
// which is a tighter answer and almost always what the game actually wanted.
static inline double FRSQRTE(double x) {
    if (x <= 0.0) return 0.0;             // HW returns +Inf; 0 is safer for poked memory
    return 1.0 / sqrt(x);
}


// ---------------------------------------------------------------------------
// Paired-singles quantized load/store (psq_l / psq_st). W = single-element
// bit (1 = one element, ps1 forced to 1.0 on load), I = GQR index. The GQR
// selects the in-memory element type and a 2^n scale:
//   ld_type = GQR bits 16-18, ld_scale = bits 24-29 (6-bit signed)
//   st_type = GQR bits  0-2,  st_scale = bits  8-13 (6-bit signed)
//   types: 0 = f32, 4 = u8, 5 = u16, 6 = s8, 7 = s16
// GQR0 is 0 by SDK convention (raw float pairs), so ordinary FP spills are
// unaffected; the audio codec programs GQR2-5 with the integer types.
// ---------------------------------------------------------------------------
extern uint32_t g_gqr[8];   // written via ppc_spr_write("GQRn", v)

static inline float ppc_psq_scale_factor(int scale6) {
    int s = (scale6 << 26) >> 26;            /* sign-extend 6-bit field */
    return ldexpf(1.0f, -s);                 /* dequant factor 2^-s */
}
static inline float ppc_psq_load_one(uint32_t va, int type, float factor) {
    switch (type) {
        case 4: return (float)MEM_R8(va)            * factor;   /* u8  */
        case 5: return (float)MEM_R16(va)           * factor;   /* u16 */
        case 6: return (float)(int8_t)MEM_R8(va)    * factor;   /* s8  */
        case 7: return (float)(int16_t)MEM_R16(va)  * factor;   /* s16 */
        default: return MEM_RF(va);
    }
}
static inline void ppc_psq_store_one(uint32_t va, int type, float factor, float f) {
    float v = f * factor;                    /* factor = 2^st_scale */
    switch (type) {
        case 4: MEM_W8 (va, (uint8_t) (v <= 0.0f ? 0 : v >= 255.0f ? 255 : (int32_t)v)); break;
        case 5: MEM_W16(va, (uint16_t)(v <= 0.0f ? 0 : v >= 65535.0f ? 65535 : (int32_t)v)); break;
        case 6: MEM_W8 (va, (uint8_t)(int8_t) (v <= -128.0f ? -128 : v >= 127.0f ? 127 : (int32_t)v)); break;
        case 7: MEM_W16(va, (uint16_t)(int16_t)(v <= -32768.0f ? -32768 : v >= 32767.0f ? 32767 : (int32_t)v)); break;
        default: MEM_WF(va, f); break;
    }
}
static inline void ppc_psq_load(int fpr_idx, uint32_t va, int W, int I) {
    uint32_t gqr  = g_gqr[I & 7];
    int type      = (int)((gqr >> 16) & 7u);
    int esz       = (type == 4 || type == 6) ? 1 : (type == 5 || type == 7) ? 2 : 4;
    float factor  = (type >= 4) ? ppc_psq_scale_factor((int)((gqr >> 24) & 0x3Fu)) : 1.0f;
    g_cpu.fpr[fpr_idx].ps[0] = ppc_psq_load_one(va, type, factor);
    g_cpu.fpr[fpr_idx].ps[1] = W ? 1.0f : ppc_psq_load_one(va + (uint32_t)esz, type, factor);
}
static inline void ppc_psq_store(int fpr_idx, uint32_t va, int W, int I) {
    uint32_t gqr  = g_gqr[I & 7];
    int type      = (int)(gqr & 7u);
    int esz       = (type == 4 || type == 6) ? 1 : (type == 5 || type == 7) ? 2 : 4;
    int s         = (int)((gqr >> 8) & 0x3Fu);
    float factor  = (type >= 4) ? ldexpf(1.0f, (s << 26) >> 26) : 1.0f;
    ppc_psq_store_one(va, type, factor, g_cpu.fpr[fpr_idx].ps[0]);
    if (!W) ppc_psq_store_one(va + (uint32_t)esz, type, factor, g_cpu.fpr[fpr_idx].ps[1]);
}


// ---------------------------------------------------------------------------
// Control flow: indirect calls, unimplemented stubs, trace.
// ---------------------------------------------------------------------------

typedef void (*PPC_Func)(void);

// Populated by the generated func_table.c constructor.
extern PPC_Func  g_func_table_ptrs [];
extern uint32_t  g_func_table_addrs[];
extern size_t    g_func_table_count;

// Dispatch `addr` through the table. Prints + aborts for unknown addresses
// by default; see PPC_INDIRECT_SOFT_FAIL to downgrade to a warn-and-return.
PPC_Func ppc_lookup_func(uint32_t addr);
void     ppc_call_indirect(uint32_t addr);

// `bctr` (branch to CTR): a JUMP, so generated code reaches it as
// `PPC_TAIL return ppc_jump_indirect();`. Deliberately void(void) -- matching
// every recompiled function -- because musttail requires the caller and callee
// to agree on parameter count. The target is read from g_cpu.ctr.
void     ppc_jump_indirect(void);

// Patch a single entry in the function table so ppc_call_indirect(addr) calls
// `fn` instead of the generated recompiled function. Safe to call after
// ppc_load_elf / ppc_runtime_init; has no effect if `addr` is not in the table.
void ppc_patch_func(uint32_t addr, PPC_Func fn);

// STATUS:PARTIAL functions emit these for unhandled mnemonics. First hit
// prints; we then continue so the guest can still make progress. Flip
// PPC_ABORT_ON_UNIMPL=1 to turn these back into hard aborts while hunting.
void ppc_unimplemented(const char *mnem, uint32_t pc);

// Trace hook, enabled with -DPPC_TRACE_ALL=1. Release builds leave the
// define at 0 so every generated function's prologue compiles away.
void ppc_trace(uint32_t pc);

// Named checkpoint marker. Cheap printf when called; meant to be sprinkled
// into generated C by hand during debugging. Strips at compile time if
// -DPPC_NO_DBG_MARK is set.
void ppc_dbg_mark(const char *tag);

// SPR catch-all (HID*, GQR*, PMC*, BATs). Unknown reads return 0. Writes
// are logged on first sight then silently accepted.
uint32_t ppc_spr_read (const char *name);
void     ppc_spr_write(const char *name, uint32_t value);

// Time base. Broadway TB ticks at bus/4 (~60.75 MHz). We derive it from
// a monotonic host clock so OSGetTime / __OSGetSystemTime return something
// moving, which keeps polling loops from spinning forever.
uint32_t ppc_time_base_tb_lower(void);
uint32_t ppc_time_base_tb_upper(void);


// ---------------------------------------------------------------------------
// Lifecycle -- wiring called from main().
// ---------------------------------------------------------------------------

// Allocate g_mem1/g_mem2 and zero them. Safe to call multiple times (no-op
// after the first) so tests can reset by calling ppc_reset_memory().
void ppc_runtime_init(void);
void ppc_reset_memory(void);

// Parse a Wii/GC PPC ELF32 file and copy its loadable segments into guest
// memory at their requested VAs. Returns 0 on success, non-zero on error.
// Writes the entry-point VA to *entry_out if non-NULL.
int  ppc_load_elf(const char *path, uint32_t *entry_out);

// DOL loader + magic-sniffing dispatcher (ELF magic -> ELF, else DOL).
int  ppc_load_dol(const char *path, uint32_t *entry_out);
int  ppc_load_image(const char *path, uint32_t *entry_out);

// Highest end-VA written into MEM1 by the most recent ppc_load_elf().
// main passes this to os_set_arena_bounds() so the SDK arena_lo lands past
// the loaded .bss instead of stomping it. Returns 0x80800000 fallback if
// no ELF has been loaded.
uint32_t ppc_loaded_mem1_top(void);

// Raw load: read `path` and memcpy its bytes into guest memory starting
// at VA `load_base`. Used for .rso / .rel modules where the recompiler
// already chose a load base and the module's code references addresses
// of the form (load_base + file_offset). Returns 0 on success.
int  ppc_load_module_image(const char *path, uint32_t load_base);

// External relocation patch: a single cross-module reference (RSO -> ELF)
// already resolved at build time. The runtime applies the patch against
// guest memory after rso_apply_internal_relocs.
//
//   file_off  -- byte offset within the module file image (matches the
//                relocation entry's r_offset, which is also where the
//                bytes live in guest memory at load_base + file_off).
//   target_va -- the imported symbol's resolved guest VA (in main ELF).
//   type      -- R_PPC_* (1=ADDR32, 4=ADDR16_LO, 6=ADDR16_HA, 10=REL24).
//   addend    -- RELA addend (added to target_va before patching).
typedef struct RecompExtPatch {
    uint32_t file_off;
    uint32_t target_va;
    uint8_t  type;
    uint8_t  _pad[3];
    uint32_t addend;
} RecompExtPatch;

// Apply N external-reloc patches against the module loaded at `load_base`.
// Mirrors the apply switch in rso_apply_internal_relocs but the symbol VA
// is supplied directly (no section-table lookup needed).
void ppc_apply_external_relocs(uint32_t load_base,
                               uint32_t n,
                               const RecompExtPatch *patches);

// Convenience: log guest-side register snapshot for crash diagnostics.
void ppc_dump_registers(void);

// ---------------------------------------------------------------------------
// Crash telemetry rings (src/runtime.c). Both are plain arrays updated without
// synchronisation on the hot path; read them only from a crash reporter, where
// a torn entry costs nothing and a mutex would cost every frame.
// ---------------------------------------------------------------------------

// Last N indirect (bctrl/vtable) dispatches: target VA + the caller's LR.
#define PPC_IND_RING 128
extern uint32_t g_ind_ring_target[PPC_IND_RING];
extern uint32_t g_ind_ring_lr    [PPC_IND_RING];
extern uint32_t g_ind_ring_head;          // total dispatches; index with & (N-1)

// Last N accesses through a VA that is neither guest RAM nor hardware MMIO,
// i.e. dereferences of a garbage pointer that ppc_host_ptr silently absorbed.
#define PPC_WILD_RING 32
extern uint64_t g_deadzone_hits;
extern uint32_t g_wild_ring_va[PPC_WILD_RING];
extern uint32_t g_wild_ring_lr[PPC_WILD_RING];
extern uint32_t g_wild_ring_head;

// For sdk/peripherals.c diagnostics -- prints recent unique PCs from the
// trace ring. Safe no-op if tracing isn't enabled.
void ppc_dump_unique_recent(uint32_t span);

// Legacy diagnostic hooks that sdk/peripherals.c fires on specific VI
// retrace ticks. If you didn't instrument call history / hotspots, these
// are safe no-ops. Kept as weak stubs so the retrace handler compiles.
void ppc_dump_recent_calls(void);
void ppc_dump_hotspots   (void);


// ---------------------------------------------------------------------------
// Include the generated forward-decl header so every caller can reference
// any func_XXXXXXXX symbol without writing extern declarations. Guarded so
// fresh trees (before the first recompile) still build runtime-only pieces.
// ---------------------------------------------------------------------------
#if defined(__has_include)
#  if __has_include("func_decls.h")
#    include "func_decls.h"
#  endif
#endif


#ifdef __cplusplus
}
#endif
