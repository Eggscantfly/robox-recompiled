// sdk/hle.h -- HLE framework for Wii OS / SDK functions.
//
// Any function listed in hle_registry.json gets its generated body replaced
// with a thunk that calls the corresponding hle_<Name>() below. HLE functions
// use the guest calling convention directly: read args from g_cpu.gpr[3..]
// / g_cpu.fpr[1..], write return to g_cpu.gpr[3] or g_cpu.fpr[1], don't
// touch the stack pointer.
//
// Why this shape: the recompiler already emits every guest function as
// `void func_X(void)` that mutates g_cpu. HLE fits in the same mold, so we
// don't need a per-signature trampoline or marshalling code.

#pragma once
#include "../src/runtime.h"
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Host stack budget for any thread that executes guest code.
//
// Deep guest call chains become deep host call chains, so a guest-executing
// thread needs a very large host stack. On Windows the MAIN thread gets this
// from `-Wl,--stack` in CMakeLists.txt, but that linker flag is a Windows-only
// mechanism -- it is a no-op for the main thread on ELF/Mach-O. Every other
// platform (and every worker thread on all platforms, including Windows) has
// to request the size explicitly at thread-creation time. Android's default
// thread stack is ~1 MB, which is roughly 500x too small.
// ---------------------------------------------------------------------------
#define ROBOX_GUEST_STACK_BYTES (512u * 1024u * 1024u)

// Portable mkdir. Was a local macro in robox_io.c while every other caller used
// Win32 CreateDirectoryA directly, which is an undefined symbol when linking for
// Android. Returns 0 on success, non-zero on failure (already-exists included).
#if defined(_WIN32)
#  include <direct.h>
#  define robox_mkdir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  define robox_mkdir(p) mkdir((p), 0755)
#endif

// PPC big lock (sdk/os.c). Only one thread executes guest code at a time;
// blocking OS primitives release it so another guest thread can run. NOTE:
// g_cpu belongs to whoever holds the lock -- snapshot and restore it around
// every release/acquire pair.
void ppc_lock_acquire(void);
void ppc_lock_release(void);

// ---------------------------------------------------------------------------
// Argument helpers (PPC EABI).
// ---------------------------------------------------------------------------
//
// Wii uses the PowerPC EABI:
//   * first 8 integer/pointer args go in r3..r10
//   * first 8 float args go in f1..f8
//   * additional args spill onto the stack (rare for our needs)
//   * integer return value in r3; float return in f1
//
// Pointers from guest code are guest VAs; wrap them with HLE_PTR/HLE_STR to
// translate to a host pointer via ppc_host_ptr().

#define HLE_ARG_U32(n)    ((uint32_t) g_cpu.gpr[3 + (n)])
#define HLE_ARG_S32(n)    ((int32_t)  g_cpu.gpr[3 + (n)])
#define HLE_ARG_U16(n)    ((uint16_t) g_cpu.gpr[3 + (n)])
#define HLE_ARG_S16(n)    ((int16_t)  g_cpu.gpr[3 + (n)])
#define HLE_ARG_U8(n)     ((uint8_t)  g_cpu.gpr[3 + (n)])
#define HLE_ARG_F32(n)    ((float)    g_cpu.fpr[1 + (n)].f64)
#define HLE_ARG_F64(n)    (           g_cpu.fpr[1 + (n)].f64)

#define HLE_PTR(n, T)     ((T*)       ppc_host_ptr(g_cpu.gpr[3 + (n)]))
#define HLE_STR(n)        ((const char*) ppc_host_ptr(g_cpu.gpr[3 + (n)]))
#define HLE_VA(n)         ((uint32_t) g_cpu.gpr[3 + (n)])      // raw guest VA

#define HLE_RET(v)        (g_cpu.gpr[3]    = (uint32_t)(v))
#define HLE_RET_F64(v)    (g_cpu.fpr[1].f64 = (double)(v))

// ---------------------------------------------------------------------------
// Tracing -- every HLE call hits HLE_TRACE() by default. Silence the bulk
// by defining HLE_SILENT_DEFAULT to 1 and opt individual functions back in
// via HLE_LOUD().
// ---------------------------------------------------------------------------

void hle_trace_(const char *fn);

// Dirt-cheap thunks used by hle_registry.json to stub CPU-init chains.
void hle_nop_ret0   (void);
void hle_nop_ret1   (void);
void hle_nop_ret3   (void);
void hle_nop_retneg1(void);
void hle_nop_noret  (void);
#ifndef HLE_SILENT_DEFAULT
#  define HLE_SILENT_DEFAULT 0
#endif

#if HLE_SILENT_DEFAULT
#  define HLE_TRACE(fn)   ((void)0)
#else
#  define HLE_TRACE(fn)   hle_trace_(fn)
#endif

// Per-function opt-in / opt-out (last write wins if both are used).
#define HLE_LOUD(fn)     hle_trace_(fn)

// Not-implemented helper -- call at the top of a function whose body is
// missing. Prints once per distinct `fn`, then returns silently. This is
// different from ppc_unimplemented(), which aborts.
void hle_stub_(const char *fn);
#define HLE_STUB(fn)     hle_stub_(fn)

// ---------------------------------------------------------------------------
// Guest-memory string helpers used by OSReport / OSPanic / debug paths.
// ---------------------------------------------------------------------------
int hle_printf_guest(const char *fmt_guest_va_0_based, uint32_t first_arg_reg);
int hle_vprintf_guest(const char *fmt, int reg_index);

// ---------------------------------------------------------------------------
// Frame-drop profiler / crash dump (sdk/robox_profiler.c). Silent while the
// game keeps its 60 Hz deadline; on a slow frame it reports the host-side
// time split plus the hottest guest functions sampled during that frame.
// ---------------------------------------------------------------------------
/* Sub-scopes inside GXFIFO split the per-draw work, because "gxfifo is 91% of
 * the frame" is true but not actionable -- it covers texture setup, uniform
 * upload and the draw call itself. Nesting is fine: GXFIFO stays the total. */
enum { ROBOX_PROF_AUDIO = 0, ROBOX_PROF_GXFIFO, ROBOX_PROF_PRESENT,
       ROBOX_PROF_TEX, ROBOX_PROF_DRAW, ROBOX_PROF_UPLOAD,
       ROBOX_PROF_SCOPES };
void robox_prof_scope_begin(int id);
void robox_prof_scope_end(int id);
void robox_prof_frame(unsigned draws_this_frame);
void robox_prof_crash_dump(const char *reason);
extern unsigned long long g_frame_sleep_us;   /* set by frame_limiter */

#ifdef __cplusplus
}
#endif
