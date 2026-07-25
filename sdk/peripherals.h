// sdk/peripherals.h -- Internal hooks exposed to quirks/ files that need to
// poke renderer / video / module-loader state owned by sdk/peripherals.c
// (or sdk/video.c, sdk/os.c, sdk/rt.c). Keep this surface small; everything
// here is a private contract between sdk/ and quirks/, not a public API.

#pragma once

#include "hle.h"

#include <stdint.h>

// ---- Renderer / GX state (defined in sdk/peripherals.c) ----
extern uint32_t bp_dst_addr;          // EFB copy destination XFB VA (24-bit << 5)
extern uint32_t vi_xfb_va;            // XFB VA the game most recently set via VI
extern uint8_t  gx_clear_r;           // EFB clear colour, last write to BP reg 0x4F/0x50
extern uint8_t  gx_clear_g;
extern uint8_t  gx_clear_b;
extern int      rasterizer_enabled;   // -1 = unprobed, 0 = off, 1 = on (RECOMP_RASTERIZE)

void gx_execute_copy(uint32_t cmd);   // EFB copy (cmd bit14=to-XFB, bit11=clear)
void video_present(void);             // SDL window pump
void video_copy_efb_to_window(uint32_t guest_xfb_va);
// Declared here rather than at the point of use: peripherals.c calls this from
// the soft-EFB path long before the extern block further down the file, so
// without a prototype in scope C implicitly declares it as returning int. GCC
// demotes that to a warning (silenced by -Wno-error=implicit-function-declaration),
// but Clang rejects the resulting conflict outright, which broke the Android build.
void video_blit_argb(const uint32_t *pixels);

// ---- Misc runtime helpers ----
uint64_t ms_now(void);                // Monotonic ms since process start
uint32_t game_heap_alloc(uint32_t size, uint32_t align);

// ---- DLL / safe-vtable fallback (defined in sdk/os.c) ----
uint32_t os_safe_vtable_va(void);
int      os_safe_vtable_installed(void);
