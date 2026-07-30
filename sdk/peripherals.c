// sdk/peripherals.c -- stub HLE for every non-core subsystem.
//
// Each stub logs its call (HLE_STUB once), returns a safe value, and unblocks
// the game's init chain. The goal isn't correctness -- it's to let the boot
// sequence complete so we can see where the REAL logic starts. Real
// implementations (real rendering via OpenGL, real audio via SDL, real
// Wiimote via SDL_gamecontroller) get dropped in later, one subsystem at a
// time, as we swap out stub with implementation.

// clock_gettime / CLOCK_MONOTONIC visibility on glibc.
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "hle.h"
#include "peripherals.h"
#include "gx_ogl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>   /* sinf/cosf: synthesized Wiimote/Nunchuk shake waveforms */


// ===========================================================================
// GX FIFO interpreter (minimal: BP register writes + EFB->XFB copy)
// ===========================================================================
//
// On real hardware, every byte written to 0xCC008000 feeds the GX command
// processor. We intercept those writes from MEM_W8/W16/W32 and push them
// into this ring. The decoder below understands the minimum needed to
// make solid-color splashes appear: BP register writes (opcode 0x61) and
// the PE_COPY_EXECUTE trigger (BP reg 0x52).
//
// We deliberately do NOT decode draw-primitive opcodes (0x80+) yet --
// textured polygons need a rasterizer, which is a separate project.

#define GX_FIFO_RING (1 << 20)   // 1 MB ring
static uint8_t  gx_fifo_ring[GX_FIFO_RING];
static uint32_t gx_fifo_head;    // read cursor
static uint32_t gx_fifo_tail;    // write cursor

// BP register state -- populated from 0x61 opcodes. Only the ones used by
// EFB copy & clear are tracked; everything else is ignored.
static uint32_t bp_clear_ar;    // reg 0x4F (AR), packed (a<<10)|r but Wii is (a<<10)|r with 10-bit each
static uint32_t bp_clear_gb;    // reg 0x50 (GB)
static uint32_t bp_efb_stride;  // reg 0x4D: destination XFB stride
uint32_t bp_dst_addr;           // reg 0x4B: EFB copy destination XFB VA (24-bit << 5)
static uint32_t bp_copy_src;    // reg 0x49: copy source top-left (x[9:0], y[19:10])
static uint32_t bp_copy_size;   // reg 0x4A: copy size-1 (w-1[9:0], h-1[19:10])
                                // (non-static so quirks/lyn_k3d.c K3D::Flip can poke it)

// Track the clear color via BP regs 0x4F (ALPHA<<10 | RED) and 0x50
// (GREEN<<10 | BLUE). Components are 10-bit; we use the top 8 bits as RGB.
uint8_t  gx_clear_r, gx_clear_g, gx_clear_b;   // exported for quirks/lyn_k3d.c

// ---------------------------------------------------------------------------
// Minimal software rasterizer + XF matrix tracking.
//
// Real GX does vertex transform in hardware: positions come in as 3D/2D in
// whatever space, then the currently-bound position matrix + projection
// matrix transforms them to clip space. We track:
//   * xf_pos_mtx[10][12]   -- 10 position matrices, each row-major 3x4
//   * xf_pos_mtx_idx       -- which of the 10 is currently bound
//   * xf_proj[16]          -- projection matrix (4x4 row-major)
// Both are filled by Load XF commands (opcode 0x10) when their XF memory
// base falls in the position-matrix (0x0000..0x01DF) or projection-matrix
// (0x1020..0x103F) ranges.
//
// Rasterization: software EFB (640x480 ARGB8). Each draw's vertices are
// assumed to be 12 bytes = 3 big-endian floats (x, y, z). We transform
// via the current position matrix, apply the projection, divide by w,
// map to screen, and scanline-fill the triangle with the current clear
// color. No texturing, no shading -- flat fills of the real geometry.
// ---------------------------------------------------------------------------

#define EFB_W 640
#define EFB_H 480
static uint32_t soft_efb[EFB_W * EFB_H];          // ARGB8 pixels
int soft_efb_has_content;                          // 1 = real draws this frame
static float    xf_pos_mtx[22][12] = {
    // Default every slot to identity so geometry lands in raw space even
    // when the game hasn't uploaded a matrix yet. 3x4 row-major identity.
    {1,0,0,0,  0,1,0,0,  0,0,1,0},
    {1,0,0,0,  0,1,0,0,  0,0,1,0},
    {1,0,0,0,  0,1,0,0,  0,0,1,0},
    {1,0,0,0,  0,1,0,0,  0,0,1,0},
    {1,0,0,0,  0,1,0,0,  0,0,1,0},
    {1,0,0,0,  0,1,0,0,  0,0,1,0},
    {1,0,0,0,  0,1,0,0,  0,0,1,0},
    {1,0,0,0,  0,1,0,0,  0,0,1,0},
    {1,0,0,0,  0,1,0,0,  0,0,1,0},
    {1,0,0,0,  0,1,0,0,  0,0,1,0},
};
static int      xf_pos_mtx_idx;
// Default projection: orthographic [-1,1] = identity. Starts as identity +
// ortho flag so first draws render at their raw vertex positions.
static float    xf_proj[16] = {
    1,0,0,0,
    0,1,0,0,
    0,0,1,0,
    0,0,0,1,
};
static int      xf_proj_is_ortho = 1;
int      rasterizer_enabled = -1;              // exported for quirks/lyn_k3d.c

/* XF channel material colors (ARGB8, set by GXSetChanMatColor).
 * GX XF memory 0x100c = color0 material, 0x100d = alpha0 material.
 * We store as 0xAARRGGBB for easy use as vertex color. */
static uint32_t xf_chan_mat[2] = { 0xFFFFFFFF, 0xFFFFFFFF };

void gx_execute_copy(uint32_t cmd);            // exported for quirks/lyn_k3d.c (cmd: bit14=to-XFB, bit11=clear)

// vi_xfb_va is defined further down near the VI block; forward-declare so
// gx_execute_copy (above that block) can prefer it for the EFB dst.
extern uint32_t vi_xfb_va;

// Broadway BP register IDs (confirmed from Dolphin's BPMemory.h):
//   0x49  BPMEM_EFB_TL          (EFB source top-left)
//   0x4A  BPMEM_EFB_WH          (EFB source width/height-1)
//   0x4B  BPMEM_EFB_ADDR        (dst address, high 24 bits << 5)
//   0x4D  BPMEM_EFB_STRIDE      (XFB dest stride)
//   0x4E  BPMEM_COPYYSCALE      (Y scale for XFB copy)
//   0x4F  BPMEM_CLEAR_AR        (alpha<<8 | red)   -- 8 bits each
//   0x50  BPMEM_CLEAR_GB        (green<<8 | blue)  -- 8 bits each
//   0x51  BPMEM_CLEAR_Z
//   0x52  BPMEM_TRIGGER_EFB_COPY
static void bp_texreg_decode(uint32_t reg, uint32_t val);  /* defined below g_tex[] */

static void bp_write(uint32_t reg, uint32_t val) {
    static uint32_t bp_reg_seen[8] = {0};   // bitmap of 0x00..0xFF
    if (reg < 0x100 && !(bp_reg_seen[reg >> 5] & (1u << (reg & 0x1F)))) {
        bp_reg_seen[reg >> 5] |= (1u << (reg & 0x1F));
        fprintf(stderr, "[BP] first write: reg=0x%02x val=0x%06x\n", reg, val);
        fflush(stderr);
    }
    bp_texreg_decode(reg, val);
    switch (reg) {
        case 0x4F: bp_clear_ar = val;
            // val = (alpha<<8) | red, each 8-bit
            gx_clear_r = (uint8_t)(val & 0xFF);
            break;
        case 0x50: bp_clear_gb = val;
            // val = (green<<8) | blue, each 8-bit
            gx_clear_g = (uint8_t)((val >> 8) & 0xFF);
            gx_clear_b = (uint8_t)(val & 0xFF);
            break;
        case 0x49: bp_copy_src  = val; break;   /* src top-left: x[9:0] y[19:10] */
        case 0x4A: bp_copy_size = val; break;   /* size-1:      w[9:0] h[19:10] */
        case 0x4B: bp_dst_addr = (val & 0x00FFFFFF) << 5; break;
        case 0x4D: bp_efb_stride = val & 0x3FF; break;
        case 0x52: {
            static uint32_t exec_n;
            if (exec_n++ < 8) {
                fprintf(stderr, "[BP] EFB_COPY_EXECUTE val=0x%06x dst=0x%08x rgb=%02x%02x%02x\n",
                        val, bp_dst_addr, gx_clear_r, gx_clear_g, gx_clear_b);
                fflush(stderr);
            }
            /* Copy command bits (Dolphin BPStructs): clear[11], to-XFB[14].
             * The old code treated EVERY copy as present+clear — but the
             * game does EFB->TEXTURE copies mid-frame (shadow/reflection
             * maps): each one presented a half-drawn frame and WIPED the
             * EFB mid-draw (the in-game full-screen flashing), and the
             * copy textures themselves never existed. */
            gx_execute_copy(val);
            break;
        }
        default: break;
    }
    /* [K0-WRITE] fade hunt: TEV konst bank 0 is the in-game post-pass fade
     * constant (post stage2 computes scene - K0.blue; K0 parked at ffffffff
     * = the permanent black world). Log every konst0 write with the guest LR
     * so the fade OWNER — and its silence after the fade-out — is
     * identifiable. reg 0xE0/0xE1 = bank 0 RA/BG word; val bit23 = konst. */
    if ((reg == 0xE0 || reg == 0xE1) && ((val >> 23) & 1)) {
        /* Many materials share konst0 — the value the POST pass sees is
         * whatever the LAST write before its draw left. Track that writer
         * exactly (g_last_k0_lr, printed by the [POST-TEV] probe). */
        extern uint32_t g_last_k0_lr, g_last_k0_val;
        g_last_k0_lr  = g_cpu.lr;
        if (reg == 0xE1) g_last_k0_val = val & 0x7FFFFFu;
        static uint32_t s_n;
        if (++s_n <= 200 || (s_n & 0x3F) == 0) {
            fprintf(stderr, "[K0-WRITE] reg=0x%02x val=0x%06x lr=0x%08x\n",
                    reg, val & 0x7FFFFFu, g_cpu.lr);
            fflush(stderr);
        }
    }
    gx_ogl_bp_write(reg, val);
}

// Execute the EFB-to-XFB copy. Real HW does a full resolve with filtering;
// we only handle the "clear mode" path (bit 11 of 0x52 is the clear flag,
// but most games set it via 0x45/0x46 too). For the splash we just fill
// the configured destination XFB with the clear color as YUYV 4:2:2.
// Convert one ARGB pixel to (Y, Cb, Cr) for the YUYV encoding below.
static inline void argb_to_ycbcr(uint32_t argb, int *Y, int *Cb, int *Cr) {
    int r = (int)((argb >> 16) & 0xFF);
    int g = (int)((argb >>  8) & 0xFF);
    int b = (int)( argb        & 0xFF);
    int y  = ((66*r + 129*g +  25*b + 128) >> 8) + 16;
    int cb = ((-38*r - 74*g + 112*b + 128) >> 8) + 128;
    int cr = ((112*r - 94*g -  18*b + 128) >> 8) + 128;
    if (y<0) y=0; if (y>255) y=255;
    if (cb<0) cb=0; if (cb>255) cb=255;
    if (cr<0) cr=0; if (cr>255) cr=255;
    *Y = y; *Cb = cb; *Cr = cr;
}

/* Per-draw GX logging (draw dumps, vertex/TEV state, FIFO writes). These run
 * tens of thousands of times per second, so they are OFF unless
 * RECOMP_GX_TRACE=1 — leaving them on cost a large chunk of the frame budget
 * and produced ~27 MB logs in a couple of minutes. */
int recomp_gx_trace(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GX_TRACE");
        on = (e && e[0] && e[0] != '0');
    }
    return on;
}

void gx_execute_copy(uint32_t cmd) {
    int is_xfb   = (cmd >> 14) & 1;
    int do_clear = (cmd >> 11) & 1;
    /* If OGL renderer is up, let it handle the frame present. */
    if (gx_ogl_ready()) {
        uint32_t clear_argb = 0xFF000000u
            | ((uint32_t)gx_clear_r << 16)
            | ((uint32_t)gx_clear_g <<  8)
            | (uint32_t)gx_clear_b;
        if (is_xfb) {
            /* TEMP(dark-text hunt): log the XFB copy rect/dst — if the game
             * copies in strips (multiple copies per frame), presenting on
             * every strip shows partial frames. */
            { static uint32_t s_n; if (s_n++ < 24) {
                uint32_t sx = bp_copy_src & 0x3FFu, sy = (bp_copy_src >> 10) & 0x3FFu;
                uint32_t cw = (bp_copy_size & 0x3FFu) + 1, ch2 = ((bp_copy_size >> 10) & 0x3FFu) + 1;
                fprintf(stderr, "[XFB-COPY] dst=0x%08x rect=(%u,%u %ux%u) clear=%d\n",
                        bp_dst_addr, sx, sy, cw, ch2, do_clear);
                fflush(stderr); } }
            gx_ogl_efb_copy(1, bp_dst_addr, 640, 480, 640, clear_argb);
            /* present path clears with the copy-clear color (frame boundary) */
        } else {
            /* EFB -> texture copy (shadow/reflection maps). Blit the source
             * rect into a GL texture registered under the destination VA;
             * texture binds to that VA then sample it directly. */
            uint32_t sx = bp_copy_src & 0x3FFu, sy = (bp_copy_src >> 10) & 0x3FFu;
            uint32_t cw = (bp_copy_size & 0x3FFu) + 1, ch2 = ((bp_copy_size >> 10) & 0x3FFu) + 1;
            uint32_t dst_va = bp_dst_addr | 0x80000000u;
            extern void gx_ogl_efb_copy_tex(uint32_t dst_va, uint32_t sx, uint32_t sy,
                                            uint32_t w, uint32_t h, int half_scale);
            int half = (cmd >> 9) & 1;   /* mipmap/half-scale copy bit */
            gx_ogl_efb_copy_tex(dst_va, sx, sy, cw, ch2, half);
            if (do_clear) {
                extern void gx_ogl_efb_clear(uint32_t clear_argb);
                gx_ogl_efb_clear(clear_argb);
            }
            static uint32_t s_n;
            if (s_n++ < 12 || (s_n & 0x3FFu) == 0) {
                fprintf(stderr, "[EFB-TEX] copy dst=0x%08x rect=(%u,%u %ux%u) clear=%d half=%d\n",
                        dst_va, sx, sy, cw, ch2, do_clear, half);
                fflush(stderr);
            }
        }
        return;
    }
    (void)is_xfb; (void)do_clear;

    // bp_dst_addr is the physical-address-style value set via BP[0x4B].
    // Prefer the game's current VI XFB pointer (which is what scan-out
    // actually reads from) when we have one -- avoids writing to a MEM1
    // mirror that never gets shown.
    uint32_t dst_va;
    if (vi_xfb_va) {
        dst_va = vi_xfb_va;
    } else if (bp_dst_addr) {
        dst_va = bp_dst_addr | 0x80000000u;
    } else {
        return;
    }
    uint8_t *xfb = (uint8_t*)ppc_host_ptr(dst_va);
    if (!xfb) return;

    // If the rasterizer has pixels for us, send them directly to the SDL
    // texture (bypassing YUYV round-trip which was losing bright hash shades
    // to chroma sub-sampling). Also write YUYV to the guest XFB so any guest
    // code that reads it back still sees valid data.
    //
    if (rasterizer_enabled == 1) {
        // Only blit if we actually have rasterized content; otherwise the
        // previous frame's content in g_efb_pixels stays on screen.
        // soft_efb_has_content is set by raster_primitive / raster_tri
        // whenever a pixel gets written, and cleared below after the
        // resolve-and-clear cycle.
        extern int soft_efb_has_content;
        extern void gx_debug_overlay(void);
        // Texture-thumbnail overlay — useful for confirming the texture
        // pipeline is wired up, but it ALWAYS forces soft_efb_has_content
        // and paints into the framebuffer, which hides whatever the game
        // is actually rendering. Default OFF so the window shows only
        // real GX output. Set RECOMP_DEBUG_OVERLAY=1 to re-enable.
        static int debug_overlay = -1;
        if (debug_overlay < 0) {
            const char *e = getenv("RECOMP_DEBUG_OVERLAY");
            debug_overlay = (e && e[0] && e[0] != '0');
        }
        if (debug_overlay) gx_debug_overlay();
        if (soft_efb_has_content) {
            video_blit_argb(soft_efb);
        }
        // Also fill XFB with YUYV for guest reads (optional but cheap).
        for (int y = 0; y < EFB_H; ++y) {
            uint8_t *row = xfb + y * EFB_W * 2;
            for (int x = 0; x < EFB_W; x += 2) {
                int y0, cb0, cr0, y1, cb1, cr1;
                argb_to_ycbcr(soft_efb[y*EFB_W + x    ], &y0, &cb0, &cr0);
                argb_to_ycbcr(soft_efb[y*EFB_W + x + 1], &y1, &cb1, &cr1);
                row[x*2 + 0] = (uint8_t)y0;
                row[x*2 + 1] = (uint8_t)((cb0 + cb1) / 2);
                row[x*2 + 2] = (uint8_t)y1;
                row[x*2 + 3] = (uint8_t)((cr0 + cr1) / 2);
            }
        }
        // Count unique pixel colors we wrote this frame -- not perfectly
        // accurate but tells us at a glance if the rasterizer changed
        // anything from the clear color.
        static uint32_t copy_n;
        uint32_t clear = 0xFF000000u
                       | ((uint32_t)gx_clear_r << 16)
                       | ((uint32_t)gx_clear_g <<  8)
                       | (uint32_t)gx_clear_b;
        if ((copy_n & 0x3F) == 0) {
            // Sample a few positions, count non-clear pixels.
            int non_clear = 0;
            for (int y = 0; y < EFB_H; y += 16)
                for (int x = 0; x < EFB_W; x += 16)
                    if (soft_efb[y*EFB_W+x] != clear) non_clear++;
            fprintf(stderr, "[GX-FIFO] EFB->XFB resolve copy #%u  non-clear samples=%d/%d  sample(100,100)=%08x\n",
                    copy_n, non_clear, (EFB_H/16)*(EFB_W/16), soft_efb[100*EFB_W+100]);
            fflush(stderr);
        }
        copy_n++;
        // Dump first frame that has any non-clear pixels as a PPM to disk so
        // we can verify output without depending on the SDL window.
        static int dumped;
        {   /* Opt-in, like the RAM dumps: this used to drop a PPM beside the
             * executable on the first frame that drew anything. */
            extern int robox_debug_dumps_wanted(void);
            if (!robox_debug_dumps_wanted()) dumped = 1;
        }
        if (!dumped) {
            for (int i = 0; i < EFB_W * EFB_H; ++i) {
                if (soft_efb[i] != clear) {
                    FILE *f = fopen("soft_efb.ppm", "wb");
                    if (f) {
                        fprintf(f, "P6\n%d %d\n255\n", EFB_W, EFB_H);
                        for (int j = 0; j < EFB_W * EFB_H; ++j) {
                            uint8_t pr = (soft_efb[j] >> 16) & 0xFF;
                            uint8_t pg = (soft_efb[j] >>  8) & 0xFF;
                            uint8_t pb = (soft_efb[j]      ) & 0xFF;
                            fputc(pr, f); fputc(pg, f); fputc(pb, f);
                        }
                        fclose(f);
                        fprintf(stderr, "[GX-FIFO] dumped soft_efb.ppm (copy #%u)\n", copy_n);
                        fflush(stderr);
                    }
                    dumped = 1;
                    break;
                }
            }
        }
        // Clear soft EFB for next frame to clear color, and mark the buffer
        // empty so a follow-up gx_execute_copy (e.g. from K3D_Flip) doesn't
        // blit a pure-clear buffer over the frame we just presented.
        if (soft_efb_has_content) {
            for (int i = 0; i < EFB_W * EFB_H; ++i) soft_efb[i] = clear;
            soft_efb_has_content = 0;
        }
        return;
    }
    int r = gx_clear_r, g = gx_clear_g, b = gx_clear_b;
    int Y  = ((66*r + 129*g +  25*b + 128) >> 8) + 16;
    int Cb = ((-38*r - 74*g + 112*b + 128) >> 8) + 128;
    int Cr = ((112*r - 94*g -  18*b + 128) >> 8) + 128;
    if (Y<0)Y=0; if (Y>255)Y=255;
    if (Cb<0)Cb=0; if (Cb>255)Cb=255;
    if (Cr<0)Cr=0; if (Cr>255)Cr=255;
    // 640x480 * 2 bytes. We don't yet honor non-standard copy source rects;
    // good enough for fullscreen clear-and-copy.
    for (int i = 0; i < 640 * 480 / 2; ++i) {
        xfb[i*4 + 0] = (uint8_t)Y;
        xfb[i*4 + 1] = (uint8_t)Cb;
        xfb[i*4 + 2] = (uint8_t)Y;
        xfb[i*4 + 3] = (uint8_t)Cr;
    }
    static uint8_t last_r, last_g, last_b;
    static int cnt;
    if (cnt < 5 ||
        gx_clear_r != last_r || gx_clear_g != last_g || gx_clear_b != last_b) {
        fprintf(stderr, "[GX-FIFO] EFB copy #%d dst=0x%08x rgb=%02x%02x%02x\n",
                cnt, dst_va, gx_clear_r, gx_clear_g, gx_clear_b);
        fflush(stderr);
        last_r = gx_clear_r; last_g = gx_clear_g; last_b = gx_clear_b;
    }
    cnt++;
}

// ---------------------------------------------------------------------------
// Rasterizer helpers.
// ---------------------------------------------------------------------------

// Decode a big-endian 32-bit float from raw bytes.
static inline float read_be_float(const uint8_t *p) {
    uint32_t w = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
               | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
    float f;
    __builtin_memcpy(&f, &w, 4);
    return f;
}

// Map transformed (clip-space x/y in [-1, 1], z ignored) to screen coords.
// If the projection yields x/y in a different range, the result will land
// outside [0, EFB_W/H) and the fill-triangle pixel loop will clip it. This
// is intentional: it means "wrong coordinate space just doesn't draw" rather
// than "wrong coordinate space writes garbage across screen."
static inline void project_vertex(float vx, float vy, float vz,
                                   int *sx, int *sy) {
    // Apply current position matrix (3x4): out = pos_mtx * (x,y,z,1)
    const float *m = xf_pos_mtx[xf_pos_mtx_idx];
    float ox = m[0]*vx + m[1]*vy + m[2]*vz  + m[3];
    float oy = m[4]*vx + m[5]*vy + m[6]*vz  + m[7];
    float oz = m[8]*vx + m[9]*vy + m[10]*vz + m[11];
    // Apply projection matrix (4x4)
    const float *p = xf_proj;
    float cx = p[0]*ox + p[1]*oy + p[2]*oz  + p[3];
    float cy = p[4]*ox + p[5]*oy + p[6]*oz  + p[7];
    float cw = xf_proj_is_ortho ? 1.0f
             : (p[12]*ox + p[13]*oy + p[14]*oz + p[15]);
    if (cw == 0.0f) cw = 1.0f;
    float nx = cx / cw;
    float ny = cy / cw;
    // Clip-space [-1..1] -> screen. Flip Y so -1 is bottom.
    *sx = (int)((nx * 0.5f + 0.5f) * (float)EFB_W);
    *sy = (int)((1.0f - (ny * 0.5f + 0.5f)) * (float)EFB_H);
}

// Fill a triangle in soft_efb with a solid ARGB color.
static void raster_tri(int x0, int y0, int x1, int y1, int x2, int y2,
                        uint32_t argb) {
    int minx = x0, maxx = x0, miny = y0, maxy = y0;
    if (x1 < minx) minx = x1;  if (x1 > maxx) maxx = x1;
    if (x2 < minx) minx = x2;  if (x2 > maxx) maxx = x2;
    if (y1 < miny) miny = y1;  if (y1 > maxy) maxy = y1;
    if (y2 < miny) miny = y2;  if (y2 > maxy) maxy = y2;
    if (minx < 0) minx = 0; if (maxx >= EFB_W) maxx = EFB_W - 1;
    if (miny < 0) miny = 0; if (maxy >= EFB_H) maxy = EFB_H - 1;
    // Edge-function test (standard half-plane, clockwise or ccw both fill).
    static uint32_t tri_degenerate, tri_filled;
    int e01 = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (e01 == 0) {
        tri_degenerate++;
        if ((tri_degenerate & 0xFFFu) == 0) {
            fprintf(stderr, "[raster] tri stats: %u filled, %u degenerate\n",
                    tri_filled, tri_degenerate);
            fflush(stderr);
        }
        return;
    }
    tri_filled++;
    int ccw = (e01 > 0) ? 1 : -1;
    int wrote_any = 0;
    for (int y = miny; y <= maxy; ++y) {
        for (int x = minx; x <= maxx; ++x) {
            int w0 = ((x1 - x0) * (y - y0) - (y1 - y0) * (x - x0)) * ccw;
            int w1 = ((x2 - x1) * (y - y1) - (y2 - y1) * (x - x1)) * ccw;
            int w2 = ((x0 - x2) * (y - y2) - (y0 - y2) * (x - x2)) * ccw;
            if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
                soft_efb[y * EFB_W + x] = argb;
                wrote_any = 1;
            }
        }
    }
    if (wrote_any) soft_efb_has_content = 1;
}

// Forward decls: definitions appear below (need cp_* tables / g_tex[]).
static int read_vertex_pos(uint8_t vat_idx, const uint8_t *v,
                            float *ox, float *oy, float *oz);
static int read_vertex_tex0(uint8_t vat_idx, const uint8_t *v,
                             float *os, float *ot);
static uint32_t gx_vertex_size(uint8_t vat_idx);
static uint32_t sample_texture(int mapid, float s, float t);

// CP state -- declared here (before rasterizer) so raster_primitive can
// test `cp_vtx_desc_hi` to decide whether to go down the textured path.
// Populated by hle_GXSetVtxDesc/AttrFmt and GXSet* HLEs below.
static uint32_t cp_vtx_desc_lo;
static uint32_t cp_vtx_desc_hi;
static uint32_t cp_vat_g0[8];
static uint32_t cp_vat_g1[8];
static uint32_t cp_vat_g2[8];
static uint32_t cp_array_base[16];
static uint32_t cp_array_stride[16];

// GxTexObj state (filled by hle_GXInitTexObj/LoadTexObj below). Declared
// here so sample_texture(), which appears earlier in the file than the HLE
// bodies, can refer to `g_tex[]`.
#define GX_MAX_TEXMAP 8
typedef struct {
    int       valid;
    uint32_t  data_va;
    uint32_t  width;
    uint32_t  height;
    uint32_t  fmt;
    uint32_t  wrap_s;
    uint32_t  wrap_t;
} GxTexObj;
static GxTexObj g_tex[GX_MAX_TEXMAP];

/* BP texture registers — the path the recompiled GXLoadTexObjPreLoaded
 * (and K3D's precompiled material blobs) use to bind textures. maps 0-3
 * at 0x80/0x88/0x94, maps 4-7 at 0xA0/0xA8/0xB4.
 *   SETMODE0 : wrap_s[1:0] wrap_t[3:2] filters
 *   SETIMAGE0: (w-1)[9:0] (h-1)[19:10] fmt[23:20]
 *   SETIMAGE3: physical addr >> 5 (image base; also the bind trigger —
 *              it is the last word the SDK pushes) */
static void bp_texreg_decode(uint32_t reg, uint32_t val) {
    int map = -1, kind = -1;
    if      (reg >= 0x80 && reg <= 0x83) { map = (int)(reg - 0x80);     kind = 0; }
    else if (reg >= 0xA0 && reg <= 0xA3) { map = (int)(reg - 0xA0) + 4; kind = 0; }
    else if (reg >= 0x88 && reg <= 0x8B) { map = (int)(reg - 0x88);     kind = 1; }
    else if (reg >= 0xA8 && reg <= 0xAB) { map = (int)(reg - 0xA8) + 4; kind = 1; }
    else if (reg >= 0x94 && reg <= 0x97) { map = (int)(reg - 0x94);     kind = 2; }
    else if (reg >= 0xB4 && reg <= 0xB7) { map = (int)(reg - 0xB4) + 4; kind = 2; }
    if (map < 0 || map >= GX_MAX_TEXMAP) return;
    if (kind == 0) {
        g_tex[map].wrap_s = val & 3;
        g_tex[map].wrap_t = (val >> 2) & 3;
    } else if (kind == 1) {
        g_tex[map].width  = (val & 0x3FF) + 1;
        g_tex[map].height = ((val >> 10) & 0x3FF) + 1;
        g_tex[map].fmt    = (val >> 20) & 0xF;
    } else {
        g_tex[map].data_va = ((val & 0x00FFFFFFu) << 5) | 0x80000000u;
        g_tex[map].valid   = 1;
        static unsigned s_n;
        if (s_n++ < 16 || (s_n & 0xFFFu) == 0) {
            fprintf(stderr, "[BP-TEX] map=%d data=0x%08x %ux%u fmt=%u\n",
                    map, g_tex[map].data_va, g_tex[map].width,
                    g_tex[map].height, g_tex[map].fmt);
            fflush(stderr);
        }
    }
}

// Fallback vertex size if the CP hasn't been programmed yet (it SHOULD be by
// the time the game issues its first draw, but we can still see partial
// writes if the FIFO wraps at boot).
#define GX_VTX_BYTES_GUESS  32u

// Textured triangle: barycentric interpolation of (s, t), per-pixel sample
// of texmap 0. Conservative half-plane test same as raster_tri.
static void raster_tri_textured(int x0, int y0, float s0, float t0,
                                 int x1, int y1, float s1, float t1,
                                 int x2, int y2, float s2, float t2) {
    int minx = x0, maxx = x0, miny = y0, maxy = y0;
    if (x1 < minx) minx = x1; if (x1 > maxx) maxx = x1;
    if (x2 < minx) minx = x2; if (x2 > maxx) maxx = x2;
    if (y1 < miny) miny = y1; if (y1 > maxy) maxy = y1;
    if (y2 < miny) miny = y2; if (y2 > maxy) maxy = y2;
    if (minx < 0) minx = 0; if (maxx >= EFB_W) maxx = EFB_W - 1;
    if (miny < 0) miny = 0; if (maxy >= EFB_H) maxy = EFB_H - 1;
    int denom = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (denom == 0) return;
    float inv_denom = 1.0f / (float)denom;
    int wrote_any = 0;
    for (int y = miny; y <= maxy; ++y) {
        for (int x = minx; x <= maxx; ++x) {
            int wA = (x1 - x) * (y2 - y) - (y1 - y) * (x2 - x);
            int wB = (x2 - x) * (y0 - y) - (y2 - y) * (x0 - x);
            int wC = denom - wA - wB;
            // Inside test (both orientations acceptable).
            if ((wA >= 0 && wB >= 0 && wC >= 0) ||
                (wA <= 0 && wB <= 0 && wC <= 0)) {
                float bA = (float)wA * inv_denom;
                float bB = (float)wB * inv_denom;
                float bC = 1.0f - bA - bB;
                float s = bA * s0 + bB * s1 + bC * s2;
                float t = bA * t0 + bB * t1 + bC * t2;
                uint32_t argbc = sample_texture(0, s, t);
                if ((argbc >> 24) != 0) {
                    soft_efb[y * EFB_W + x] = argbc;
                    wrote_any = 1;
                }
            }
        }
    }
    if (wrote_any) soft_efb_has_content = 1;
}

// Simple per-draw hash so different primitives get distinguishable shades.
static inline uint32_t hash_shade(uint32_t seed) {
    seed ^= seed >> 16; seed *= 0x7feb352du;
    seed ^= seed >> 15; seed *= 0x846ca68bu;
    seed ^= seed >> 16;
    return seed;
}

static int read_vertex_col0(uint8_t vat_idx, const uint8_t *v, uint32_t *abgr);

// Rasterize `count` vertices as a draw-primitive of type `opcode`.
// opcode low 3 bits = VAT index, which tells us the vertex format.
// Vertex data is stored inline in the FIFO at the given stride.
static void raster_primitive(uint8_t opcode, uint32_t count, const uint8_t *vbuf,
                              uint32_t stride) {
    if (count < 3) return;

    /* The game's fullscreen video quad samples a movie texture whose guest
     * YUV upload path we never implemented — it would draw opaque black.
     * While a bink movie is open, identify the quad by its unmistakable
     * shape — 4 direct-pos verts, no UVs (texgen-from-position), unit XY,
     * far-negative Z — and substitute the DLL-decoded frame as its texture
     * (passthrough TEV, see gx_ogl_draw). The movie then renders inside the
     * EFB with the game's own layering: the intro covers the still-loading
     * world; the title background movie sits UNDER the menu. */
    {
        extern int bink_hle_is_open(void);
        int has_uvs_pre = ((cp_vtx_desc_hi >> 0) & 3) != 0;
        if (count == 4 && !has_uvs_pre && gx_ogl_ready() && bink_hle_is_open()) {
            float qx = 0, qy = 0, qz = 0;
            if (read_vertex_pos(opcode & 7, vbuf, &qx, &qy, &qz) &&
                qx >= -0.01f && qx <= 1.01f && qy >= -0.01f && qy <= 1.01f) {
                if (qz < -100.0f) {
                    extern const void *bink_hle_peek_frame(int *w, int *h);
                    extern void gx_ogl_set_tex_host_bgra(const void *pix, int w, int h);
                    int bw = 0, bh = 0;
                    const void *pix = bink_hle_peek_frame(&bw, &bh);
                    if (pix) gx_ogl_set_tex_host_bgra(pix, bw, bh);
                    else     return;   /* no frame yet: drop the black quad */
                }
            }
        }
    }

    /* VIDEO SURFACES (census finding): the menu/title draws the movie
     * through the game's OWN surface widgets — 854x480 composites and the
     * 640x448 I8 Y-plane 4-stage YUV draw (per-vertex matrix indices,
     * tref0=0x3c33c4) — sampling guest planes our build never fills (the
     * DLL decodes movies host-side). Those draws painted the stale-garbage
     * WHITE WASH over the title. Whenever a movie is open and a draw binds
     * a movie-surface-sized texture, substitute the DLL frame (the
     * override also forces a passthrough TEV — the game's YUV math would
     * mangle RGB). In Dolphin the title's dome/city backdrop IS this
     * surface playing the attract movie. */
    {
        extern int bink_hle_is_open(void);
        extern int bink_hle_has_frame(void);
        /* Gate on has_frame (not is_open): the surface must KEEP showing the
         * last decoded frame after BinkClose — the post-movie shake-tutorial
         * screen draws this surface as its background, and reverting to the
         * never-filled guest planes painted an opaque BLACK quad over it.
         * The dims are video-specific, so post-close hijack risk is nil. */
        if (gx_ogl_ready() && g_tex[0].valid &&
            (bink_hle_is_open() || bink_hle_has_frame()) &&
            ((g_tex[0].width == 854 && g_tex[0].height == 480) ||
             (g_tex[0].width == 640 && g_tex[0].height == 448 && g_tex[0].fmt == 1))) {
            extern const void *bink_hle_peek_frame(int *w, int *h);
            extern void gx_ogl_set_tex_host_bgra(const void *pix, int w, int h);
            int bw = 0, bh = 0;
            const void *pix = bink_hle_peek_frame(&bw, &bh);
            if (pix) {
                gx_ogl_set_tex_host_bgra(pix, bw, bh);
                static unsigned s_n;
                if (s_n++ < 6) {
                    fprintf(stderr, "[BINK-SURF] video-surface draw fed with DLL frame"
                            " (tex %ux%u fmt=%u)\n",
                            g_tex[0].width, g_tex[0].height, g_tex[0].fmt);
                    fflush(stderr);
                }
            }
        }
    }

    /* ---- OpenGL fast path ---- */
    if (gx_ogl_ready() && count <= 32768) {
        uint8_t vat = opcode & 7;
        int has_uvs = ((cp_vtx_desc_hi >> 0) & 3) != 0;
        int has_tex = g_tex[0].valid;   /* texture counts even without
                                           vertex UVs: UI quads rely on
                                           texgen-from-position */
        extern void gx_ogl_set_uv_from_pos(int on);
        gx_ogl_set_uv_from_pos(has_tex && !has_uvs);
        /* Forward ALL bound texmaps — the menu widgets' TEV samples texmap1
         * (I8 alpha-mask atlas) in stage 0 and texmap0 (CMPR color atlas)
         * in stage 1; forwarding only slot 0 left unit 1 empty and the
         * color*mask combine collapsed (invisible menu icons). The GL side
         * caches uploads per (va,fmt), so this is cheap. */
        for (int m = 0; m < GX_MAX_TEXMAP; ++m) {
            if (g_tex[m].valid) {
                gx_ogl_set_tex(m, g_tex[m].data_va, g_tex[m].width,
                               g_tex[m].height, g_tex[m].fmt,
                               g_tex[m].wrap_s, g_tex[m].wrap_t);
            } else {
                gx_ogl_set_tex(m, 0, 0, 0, 0, 0, 0);
            }
        }
        /* Raster (RASC) color: per-vertex Color0 when the vertex format
         * supplies it (the GX channel default is matsrc=GX_SRC_VTX and this
         * game never writes XF 0x100e to change it); otherwise fall back to
         * the channel-0 material register. xf_chan_mat is stored 0xAARRGGBB;
         * the GL attrib reads bytes in memory order, so convert to ABGR
         * (bytes R,G,B,A) — same layout read_vertex_col0 produces. */
        uint32_t cm = xf_chan_mat[0];
        uint32_t shade = (cm & 0xFF00FF00u)
                       | ((cm >> 16) & 0xFFu)
                       | ((cm & 0xFFu) << 16);

        GxOglVertex *overts = (GxOglVertex*)alloca(count * sizeof(GxOglVertex));
        int ok = 1;
        for (uint32_t i = 0; i < count; i++) {
            float vx = 0, vy = 0, vz = 0;
            float vs = 0, vt = 0;
            uint32_t vcol;
            if (!read_vertex_pos(vat, vbuf + i*stride, &vx, &vy, &vz)) { ok = 0; break; }
            if (has_uvs) read_vertex_tex0(vat, vbuf + i*stride, &vs, &vt);
            overts[i].x = vx; overts[i].y = vy; overts[i].z = vz;
            overts[i].color = read_vertex_col0(vat, vbuf + i*stride, &vcol) ? vcol : shade;
            overts[i].s = vs; overts[i].t = vt;
            overts[i].mtx_idx = (uint8_t)(xf_pos_mtx_idx * 3);
        }
        if (ok) { gx_ogl_draw(opcode, overts, (int)count); return; }
    }

    // Draw-visibility: each primitive gets a bright hash-derived color so
    // the real game geometry clearly pops against the clear-color background.
    // This is honest output -- every pixel we fill represents a real vertex
    // the game submitted; we just aren't (yet) doing per-vertex lighting or
    // texture sampling.
    static uint32_t prim_counter;
    uint32_t h = hash_shade(++prim_counter ^ ((uint32_t)opcode << 16) ^ count);
    int rr = (int)((h      ) & 0xFF);
    int gg = (int)((h >>  8) & 0xFF);
    int bb = (int)((h >> 16) & 0xFF);
    // Ensure at least one channel is bright so draws are always visible.
    int max = rr > gg ? (rr > bb ? rr : bb) : (gg > bb ? gg : bb);
    if (max < 180) {
        int boost = 180 - max;
        rr += boost; gg += boost; bb += boost;
        if (rr > 255) rr = 255;
        if (gg > 255) gg = 255;
        if (bb > 255) bb = 255;
    }
    uint32_t argb = 0xFF000000u | ((uint32_t)rr << 16)
                                | ((uint32_t)gg <<  8)
                                | (uint32_t)bb;

    uint8_t vat = opcode & 7;
    int has_tex = ((cp_vtx_desc_hi >> 0) & 3) != 0 && g_tex[0].valid;

    // Per-vertex scratch: screen xy + tex st.
    struct V { int sx, sy; float s, t; };
    struct V a, b, c, d;

    #define VLOAD(i, V) do {                                            \
        float vx, vy, vz;                                               \
        if (!read_vertex_pos(vat, vbuf + (i)*stride, &vx, &vy, &vz)) {  \
            return;                                                     \
        }                                                               \
        project_vertex(vx, vy, vz, &(V).sx, &(V).sy);                   \
        (V).s = 0; (V).t = 0;                                           \
        if (has_tex) read_vertex_tex0(vat, vbuf + (i)*stride, &(V).s, &(V).t); \
    } while (0)

    #define RASTER_TRI(A, B, C) do {                                    \
        if (has_tex) {                                                  \
            raster_tri_textured((A).sx,(A).sy,(A).s,(A).t,              \
                                (B).sx,(B).sy,(B).s,(B).t,              \
                                (C).sx,(C).sy,(C).s,(C).t);             \
        } else {                                                        \
            raster_tri((A).sx,(A).sy,(B).sx,(B).sy,(C).sx,(C).sy,argb); \
        }                                                               \
    } while (0)

    // Per Dolphin: cmdbyte = 0x80..0xBF. Primitive type extracted via
    //   primitive = (cmdbyte & GX_PRIMITIVE_MASK) >> GX_PRIMITIVE_SHIFT
    //             = (cmdbyte & 0x78) >> 3
    // VAT index via (cmdbyte & 0x07). For our switch, masking with 0xF8
    // is fine because VAT bits 0-2 are zero by construction here, but the
    // safer modern equivalent is `opcode & 0x78`.
    switch (opcode & 0xF8) {
        case 0x80:     // QUADS: 4 verts = 2 tris
        case 0x88: {   // QUADS_2: behaves identically to QUADS per Dolphin
            for (uint32_t q = 0; q + 3 < count; q += 4) {
                VLOAD(q+0, a); VLOAD(q+1, b); VLOAD(q+2, c); VLOAD(q+3, d);
                RASTER_TRI(a, b, c);
                RASTER_TRI(a, c, d);
            }
            break;
        }
        case 0x90: {   // TRIANGLES
            for (uint32_t t = 0; t + 2 < count; t += 3) {
                VLOAD(t+0, a); VLOAD(t+1, b); VLOAD(t+2, c);
                RASTER_TRI(a, b, c);
            }
            break;
        }
        case 0x98: {   // TRIANGLE_STRIP
            for (uint32_t t = 2; t < count; ++t) {
                VLOAD(t-2, a); VLOAD(t-1, b); VLOAD(t, c);
                RASTER_TRI(a, b, c);
            }
            break;
        }
        case 0xA0: {   // TRIANGLE_FAN
            VLOAD(0, a);
            for (uint32_t t = 1; t + 1 < count; ++t) {
                VLOAD(t, b); VLOAD(t+1, c);
                RASTER_TRI(a, b, c);
            }
            break;
        }
        case 0xA8: {   // LINES — pairs of vertices form line segments.
            // Soft rasterizer doesn't have a line primitive; degrade to
            // a thin triangle so the geometry is at least visible.
            for (uint32_t l = 0; l + 1 < count; l += 2) {
                VLOAD(l+0, a); VLOAD(l+1, b);
                RASTER_TRI(a, b, b);   // degenerate, but visible per pixel
            }
            break;
        }
        case 0xB0: {   // LINE_STRIP — connected line sequence.
            for (uint32_t l = 1; l < count; ++l) {
                VLOAD(l-1, a); VLOAD(l, b);
                RASTER_TRI(a, b, b);
            }
            break;
        }
        case 0xB8: {   // POINTS — single-vertex draws. Render as 1px tris.
            for (uint32_t p = 0; p < count; ++p) {
                VLOAD(p, a);
                RASTER_TRI(a, a, a);
            }
            break;
        }
        default: break;
    }
    #undef VLOAD
    #undef RASTER_TRI
}

// Called when Load XF fires. base = xf memory offset (in u32 words from XF
// start). n = number of u32s to write.
//
// XF memory map (libogc GX_SetArray / standard Wii GX):
//   0x0000..0x00FF : position matrix memory (64 matrices × 4 u32 = 256 words)
//                    Each 3x4 matrix is 12 floats laid out row-major.
//   0x0400..0x04FF : normal matrix memory
//   0x0500..0x05FF : texture matrix memory (8 × 4 u32)
//   0x1000..0x1003 : scale + rotate A
//   0x1020..0x102C : projection matrix (7 floats for persp, or 6 + flag)
//
// For our pipeline we track pos matrices and projection. Projection on the
// Wii uses a 6 or 7 float layout:
//   [0..3]   row 0 (A, B, C, D) -- but Wii packs this differently:
//   Actually GX uses a 6-float "compact" projection + a format flag at [6]:
//     persp: [A, B, C, D, E, F, 0] where matrix is:
//        | A  0  B  0 |
//        | 0  C  D  0 |
//        | 0  0  E  F |
//        | 0  0 -1  0 |
//     ortho: [A, B, C, D, E, F, 1] where matrix is:
//        | A  0  0  B |
//        | 0  C  0  D |
//        | 0  0  E  F |
//        | 0  0  0  1 |
// We store the compact 6 floats + flag, then expand to 4x4 when projecting.
static float xf_proj_compact[7];   // 6 coefs + format flag
static int   xf_proj_loaded = 0;

static void xf_write(uint32_t base, const uint8_t *data, uint32_t n_u32) {
    /* Black-menu hunt: count pos-matrix loads whose payload is entirely
     * zero vs real, as they arrive in the FIFO (any source: imm, DL). */
    if (base < 0x108 && n_u32 >= 12) {
        int allz = 1;
        for (uint32_t i = 0; i < 12 * 4; ++i) if (data[i]) { allz = 0; break; }
        static uint32_t s_n, s_z, s_logz;
        s_n++;
        if (allz) s_z++;
        if ((allz && s_logz < 8 && ++s_logz) || (s_n & 0x7FFFu) == 0) {
            fprintf(stderr, "[XF-POSMTX] load base=0x%04x n=%u %s zero=%u/%u\n",
                    base, n_u32, allz ? "ALL-ZERO" : "ok", s_z, s_n);
            fflush(stderr);
        }
    }
    static int log_n;
    if (log_n < 12) {
        fprintf(stderr, "[XF] load base=0x%04x n=%u first=%08x %08x %08x\n",
                base, n_u32,
                n_u32 >= 1 ? ((data[0]<<24)|(data[1]<<16)|(data[2]<<8)|data[3]) : 0,
                n_u32 >= 2 ? ((data[4]<<24)|(data[5]<<16)|(data[6]<<8)|data[7]) : 0,
                n_u32 >= 3 ? ((data[8]<<24)|(data[9]<<16)|(data[10]<<8)|data[11]) : 0);
        fflush(stderr);
        log_n++;
    }
    for (uint32_t i = 0; i < n_u32; ++i) {
        float f = read_be_float(data + i * 4);
        uint32_t off = base + i;
        /* DIAG (menu channel-color hunt): log the channel config the game sets
         * — NumColors, ambient/material colors, and the CtrlColor regs whose
         * bit0=matsrc (0=material REG, 1=vertex) / bit6=ambsrc / bit1=lighting.
         * This tells us where the raster (RASC) color the menu uses comes from. */
        if (off >= 0x1006 && off <= 0x1011) {
            uint32_t raw = ((uint32_t)data[i*4]<<24)|((uint32_t)data[i*4+1]<<16)
                         |((uint32_t)data[i*4+2]<<8)|(uint32_t)data[i*4+3];
            static uint32_t seen[24][2]; static int ns; int dup=0;
            for (int j=0;j<ns;j++) if (seen[j][0]==off&&seen[j][1]==raw){dup=1;break;}
            if (!dup && ns<24) { seen[ns][0]=off; seen[ns][1]=raw; ns++;
                const char* nm = off==0x100e?"COLOR0CNTRL":off==0x100f?"COLOR1CNTRL":
                                 off==0x100a?"AMBIENT0":off==0x100b?"AMBIENT1":
                                 off==0x100c?"MATERIAL0":off==0x100d?"MATERIAL1":
                                 off==0x1006?"NUMCOLORS":off==0x1010?"ALPHA0CNTRL":
                                 off==0x1011?"ALPHA1CNTRL":"chan";
                fprintf(stderr, "[CHAN] XF[0x%04x] %-11s = 0x%08x\n", off, nm, raw);
                fflush(stderr);
            }
        }
        if (off < 0x0108) {                 // pos + texture matrix memory
            uint32_t mtx = off / 12;
            uint32_t idx = off % 12;
            if (mtx < 22) {
                /* QUIRK EDIT (TEMPORARY — remove once the recompiler float
                 * bug is fixed): the game builds this matrix's 1.0 elements as
                 * doubles, and the recompiled float store writes the HIGH 32
                 * bits of double 1.0 (0x3ff00000) instead of rounding to the
                 * float 1.0 (0x3f800000). Read back as f32 that bit pattern is
                 * 1.875, giving every fullscreen UI quad a 1.875x scale (only
                 * the top-left ~53% lands in the EFB). Correct the exact
                 * corrupt pattern back to the 1.0 it was meant to be. */
                { uint32_t fb; memcpy(&fb, &f, 4);
                  if (fb == 0x3ff00000u) {
                      f = 1.0f;
                      static int s_warned;
                      if (!s_warned) { s_warned = 1;
                          fprintf(stderr, "[QUIRK] pos-mtx 0x3ff00000 (double-1.0 hi word) -> 1.0\n");
                          fflush(stderr); }
                  } }
                xf_pos_mtx[mtx][idx] = f;
                /* Forward complete matrix to OGL renderer */
                if (gx_ogl_ready()) {
                    const float *m = xf_pos_mtx[mtx];
                    float r0[4] = {m[0],m[1],m[2],m[3]};
                    float r1[4] = {m[4],m[5],m[6],m[7]};
                    float r2[4] = {m[8],m[9],m[10],m[11]};
                    gx_ogl_set_pos_matrix((int)mtx, r0, r1, r2);
                }
            }
        } else if (off >= 0x100c && off <= 0x100d) {
            /* XF channel material color (ARGB8 packed as u32 integer, NOT float) */
            const uint8_t *raw = data + i * 4;
            uint32_t packed = ((uint32_t)raw[0]<<24)|((uint32_t)raw[1]<<16)
                             |((uint32_t)raw[2]<<8)|(uint32_t)raw[3];
            /* packed = 0xRRGGBBAA (GX format) → store as 0xAARRGGBB */
            uint8_t r8=(packed>>24)&0xff, g8=(packed>>16)&0xff,
                    b8=(packed>>8)&0xff,  a8=(packed)&0xff;
            xf_chan_mat[off - 0x100c] = ((uint32_t)a8<<24)|((uint32_t)r8<<16)
                                       |((uint32_t)g8<<8)|(uint32_t)b8;
            /* DIAG (fade hunt): log every distinct channel color the game sets.
             * A fade-to-black via GXSetChanMatColor shows up here as an ARGB
             * ramp toward 0x00000000/0xff000000. Remove once understood. */
            { static uint32_t s_last[2] = {0xdeadbeef,0xdeadbeef};
              static uint32_t s_n;
              uint32_t v = xf_chan_mat[off - 0x100c];
              if (v != s_last[off - 0x100c]) {
                  s_last[off - 0x100c] = v;
                  /* Churns per draw in-game — debug only (fade hunt is done). */
                  if (recomp_gx_trace() && (++s_n <= 400 || (s_n & 0xFFFu) == 0)) {
                      fprintf(stderr, "[FADE?] ChanColor[%u] = 0x%08x (ARGB)\n",
                              off - 0x100c, v); fflush(stderr);
                  }
              } }
        } else if (off >= 0x1020 && off < 0x1027) {
            /* The compact 7-float projection layout: 0x1020..0x1026 (6 coefs +
             * the format flag at k==6). The bound was 0x1028, which let k reach
             * 7 and write one float PAST xf_proj_compact[7] into whatever
             * neighbouring host global follows it in .bss -- a silent
             * corruption with no source-level write site at the victim. */
            uint32_t k = off - 0x1020;
            /* DIAG (menu NaN hunt): the menu submits some draws with a NaN
             * projection coefficient -> NaN geometry. Log the raw bits so we
             * can tell a double->float-hi-word corruption (exp bits set, e.g.
             * 0x7ffxxxxx) from genuine uninitialized garbage. */
            if (k < 6 && f != f) {
                uint32_t bits; memcpy(&bits, &f, 4);
                static int nn; if (nn++ < 24) {
                    fprintf(stderr, "[PROJ-NAN] coef[%u] raw=0x%08x\n", k, bits);
                    fflush(stderr);
                }
            }
            xf_proj_compact[k] = f;
            if (k == 6) {
                xf_proj_loaded = 1;
                // Slot 6 is the projection type flag (GX_ORTHOGRAPHIC=1 /
                // GX_PERSPECTIVE=0). It's an INTEGER, not a float -- the
                // game's recompiled GXSetProjection stores it via `stw`
                // (not `stfs`). Reading as float and casting back to int
                // loses the value because the bit pattern of integer 1
                // (0x00000001) reads as the float 1.4e-45, and
                // `(int)1.4e-45 == 0`. So every projection was being
                // treated as perspective regardless of the game's choice;
                // ortho-shape matrices ran through perspective's z-divide
                // and collapsed all vertices to a single point.
                const uint8_t *slot6 = data + i * 4;
                uint32_t type_word = ((uint32_t)slot6[0] << 24)
                                   | ((uint32_t)slot6[1] << 16)
                                   | ((uint32_t)slot6[2] <<  8)
                                   |  (uint32_t)slot6[3];
                int is_ortho = (type_word & 1u) ? 1 : 0;
                xf_proj_is_ortho = is_ortho;
                // Expand to full 4x4 row-major
                float A = xf_proj_compact[0];
                float B = xf_proj_compact[1];
                float C = xf_proj_compact[2];
                float D = xf_proj_compact[3];
                float E = xf_proj_compact[4];
                float F = xf_proj_compact[5];
                if (is_ortho) {
                    xf_proj[ 0]=A;  xf_proj[ 1]=0;  xf_proj[ 2]=0;  xf_proj[ 3]=B;
                    xf_proj[ 4]=0;  xf_proj[ 5]=C;  xf_proj[ 6]=0;  xf_proj[ 7]=D;
                    xf_proj[ 8]=0;  xf_proj[ 9]=0;  xf_proj[10]=E;  xf_proj[11]=F;
                    xf_proj[12]=0;  xf_proj[13]=0;  xf_proj[14]=0;  xf_proj[15]=1;
                } else {
                    xf_proj[ 0]=A;  xf_proj[ 1]=0;  xf_proj[ 2]=B;  xf_proj[ 3]=0;
                    xf_proj[ 4]=0;  xf_proj[ 5]=C;  xf_proj[ 6]=D;  xf_proj[ 7]=0;
                    xf_proj[ 8]=0;  xf_proj[ 9]=0;  xf_proj[10]=E;  xf_proj[11]=F;
                    xf_proj[12]=0;  xf_proj[13]=0;  xf_proj[14]=-1; xf_proj[15]=0;
                }
                static int log_n;
                if (log_n < 8) {
                    fprintf(stderr,
                        "[XF] proj %s (type_word=0x%08x) A=%.3f B=%.3f C=%.3f D=%.3f E=%.3f F=%.3f\n",
                        is_ortho ? "ortho" : "persp", type_word, A,B,C,D,E,F);
                    fflush(stderr);
                    log_n++;
                }
                if (gx_ogl_ready()) {
                    float compact[7] = {A,B,C,D,E,F,(float)is_ortho};
                    gx_ogl_set_proj(compact);
                }
            }
        }
    }
}

static inline uint8_t fifo_read(void) {
    uint8_t b = gx_fifo_ring[gx_fifo_head & (GX_FIFO_RING - 1)];
    gx_fifo_head++;
    return b;
}
static inline uint32_t fifo_avail(void) { return gx_fifo_tail - gx_fifo_head; }

// ---------------------------------------------------------------------------
// CP state tracking for real vertex-size calculation.
//
// The Command Processor has registers for the Vertex Descriptor (which
// attributes are present and in what form -- Direct, Index8, Index16, or
// NotPresent) and 8 Vertex Attribute Tables (which describe the format of
// each attribute -- float/short/byte, XYZ/XY, etc.).
//
// Register layout (matches libogc / Dolphin):
//   0x30, 0x40    Unknown matrix index stuff
//   0x50          VtxDesc low (PosMatIdx..Color1 formats)
//   0x60          VtxDesc high (Tex0Coord..Tex7Coord formats)
//   0x70..0x77    VAT[0..7] group 0 (Pos/Nrm/Color/Tex0)
//   0x80..0x87    VAT[0..7] group 1 (Tex1..Tex4)
//   0x90..0x97    VAT[0..7] group 2 (Tex4..Tex7)
//   0xA0..0xAF    Array base addresses (indexed vertex)
//   0xB0..0xBF    Array strides
// ---------------------------------------------------------------------------

// (cp_vtx_desc_* / cp_vat_* / cp_array_* declared earlier in the file.)

static void cp_write(uint8_t reg, uint32_t val) {
    if (reg == 0x30) {
        /* MatrixIndex A: posnrm row[5:0], tex0 row[11:6], tex1 row[17:12]...
         * rows are XF matrix-memory rows (4 words each); slot = row / 3. */
        int pos_row  = (int)(val & 0x3F);
        int tex0_row = (int)((val >> 6) & 0x3F);
        xf_pos_mtx_idx = pos_row / 3;
        if (xf_pos_mtx_idx >= 22) xf_pos_mtx_idx = 0;
        extern void gx_ogl_set_tex0_mtx_slot(int slot);
        gx_ogl_set_tex0_mtx_slot(tex0_row == 60 ? -1 : tex0_row / 3);
        static int s_n;
        if (s_n++ < 8) {
            fprintf(stderr, "[CP] matIdxA=0x%08x pos_row=%d tex0_row=%d\n",
                    val, pos_row, tex0_row);
            fflush(stderr);
        }
    }
    if (reg == 0x50)                 cp_vtx_desc_lo = val;
    else if (reg == 0x60)            cp_vtx_desc_hi = val;
    else if (reg >= 0x70 && reg < 0x78) cp_vat_g0[reg - 0x70] = val;
    else if (reg >= 0x80 && reg < 0x88) cp_vat_g1[reg - 0x80] = val;
    else if (reg >= 0x90 && reg < 0x98) cp_vat_g2[reg - 0x90] = val;
    else if (reg >= 0xA0 && reg < 0xB0) {
        /* GX array-base registers hold PHYSICAL addresses (MEM1 phys 0x0...,
         * MEM2 phys 0x10000000..). Both map to virtual by +0x80000000.
         * Stored untranslated, every indexed vertex fetch fell into the
         * deadzone and read ZEROS — the invisible (all-zero geometry) menu
         * widgets. Translate once here so all consumers (pos/color/tex
         * fetch, indexed XF loads) see a proper VA. */
        cp_array_base[reg - 0xA0] = (val < 0x80000000u) ? val + 0x80000000u : val;
    }
    else if (reg >= 0xB0 && reg < 0xC0) cp_array_stride[reg - 0xB0] = val;
}

// VertexComponentFormat values in VtxDesc:
//   0 NotPresent, 1 Direct, 2 Index8, 3 Index16
// ComponentFormat values in VAT:
//   0 UByte, 1 Byte, 2 UShort, 3 Short, 4 Float, 5..7 InvalidFloat
// CoordComponentCount: 0 XY, 1 XYZ
// NormalComponentCount: 0 NBT (single N), 1 NBT (three vectors)
// ColorComponentCount: 0 RGB, 1 RGBA
// ColorFormat: 0 RGB565, 1 RGB888, 2 RGB888x, 3 RGBA4444, 4 RGBA6666, 5 RGBA8888
// TexComponentCount: 0 S, 1 ST

static int pos_size_bytes(int vcf, int fmt, int elems) {
    if (vcf == 0) return 0;                  // not present
    if (vcf == 2) return 1;                  // Index8
    if (vcf == 3) return 2;                  // Index16
    // Direct: bytes = component_bytes * (elems ? 3 : 2)
    int n = elems ? 3 : 2;
    if (fmt == 0 || fmt == 1) return n;      // UByte/Byte  1 byte each
    if (fmt == 2 || fmt == 3) return 2 * n;  // UShort/Short
    return 4 * n;                            // Float (or invalid -> treat as float)
}

static int norm_size_bytes(int vcf, int fmt, int elems, int index3) {
    if (vcf == 0) return 0;
    if (vcf == 2) return index3 ? 3 : 1;
    if (vcf == 3) return index3 ? 6 : 2;
    // Direct: 3 components, or 9 if NTB
    int n = elems ? 9 : 3;
    if (fmt == 0 || fmt == 1) return n;
    if (fmt == 2 || fmt == 3) return 2 * n;
    return 4 * n;
}

static int color_size_bytes(int vcf, int comp) {
    if (vcf == 0) return 0;
    if (vcf == 2) return 1;
    if (vcf == 3) return 2;
    // Direct, by ColorFormat (comp)
    switch (comp) {
        case 0: return 2;   // RGB565
        case 1: return 3;   // RGB888
        case 2: return 4;   // RGB888x
        case 3: return 2;   // RGBA4444
        case 4: return 3;   // RGBA6666 (24-bit)
        case 5: return 4;   // RGBA8888
        default: return 4;
    }
}

static int texc_size_bytes(int vcf, int fmt, int elems) {
    if (vcf == 0) return 0;
    if (vcf == 2) return 1;
    if (vcf == 3) return 2;
    int n = elems ? 2 : 1;
    if (fmt == 0 || fmt == 1) return n;
    if (fmt == 2 || fmt == 3) return 2 * n;
    return 4 * n;
}

// Compute the real per-vertex byte count for the given VAT index using the
// current CP state.
static uint32_t gx_vertex_size(uint8_t vat_idx) {
    uint8_t vidx = vat_idx & 7;
    uint32_t desc_lo = cp_vtx_desc_lo;
    uint32_t desc_hi = cp_vtx_desc_hi;
    uint32_t g0 = cp_vat_g0[vidx];
    uint32_t g1 = cp_vat_g1[vidx];
    uint32_t g2 = cp_vat_g2[vidx];

    // Each matrix-index adds 1 byte each (9 bits: PosMatIdx + 8 tex mat idxs)
    uint32_t mat_bits = desc_lo & 0x1FF;
    uint32_t size = 0;
    while (mat_bits) { size += (mat_bits & 1); mat_bits >>= 1; }

    int pos_vcf   = (desc_lo >>  9) & 3;
    int norm_vcf  = (desc_lo >> 11) & 3;
    int col0_vcf  = (desc_lo >> 13) & 3;
    int col1_vcf  = (desc_lo >> 15) & 3;

    // VAT g0: PosElements[0], PosFormat[1..3], PosFrac[4..8],
    //         NormalElements[9], NormalFormat[10..12],
    //         Color0Elements[13], Color0Comp[14..16],
    //         Color1Elements[17], Color1Comp[18..20],
    //         Tex0Elements[21], Tex0Format[22..24], Tex0Frac[25..29],
    //         ByteDequant[30], NormalIndex3[31]
    int pos_elems   = (g0 >>  0) & 1;
    int pos_fmt     = (g0 >>  1) & 7;
    int norm_elems  = (g0 >>  9) & 1;
    int norm_fmt    = (g0 >> 10) & 7;
    int col0_comp   = (g0 >> 14) & 7;
    int col1_comp   = (g0 >> 18) & 7;
    int tex0_elems  = (g0 >> 21) & 1;
    int tex0_fmt    = (g0 >> 22) & 7;
    int norm_index3 = (g0 >> 31) & 1;

    size += pos_size_bytes (pos_vcf,  pos_fmt,  pos_elems);
    size += norm_size_bytes(norm_vcf, norm_fmt, norm_elems, norm_index3);
    size += color_size_bytes(col0_vcf, col0_comp);
    size += color_size_bytes(col1_vcf, col1_comp);

    // Tex 0 from g0 (elements bit 21, fmt bits 22..24)
    // Tex 1..4 from g1:
    //   Tex1 elems/fmt bits[0]/[1..3] frac[4..8]
    //   Tex2 elems/fmt bits[9]/[10..12] frac[13..17]
    //   Tex3 elems/fmt bits[18]/[19..21] frac[22..26]
    //   Tex4 elems/fmt bits[27]/[28..30]  (frac continues in g2 bits 0..4)
    // Tex 5..7 from g2:
    //   Tex5 elems/fmt bits[5]/[6..8]
    //   Tex6 elems/fmt bits[14]/[15..17]
    //   Tex7 elems/fmt bits[23]/[24..26]

    int tex_vcf[8];
    for (int i = 0; i < 8; ++i) tex_vcf[i] = (desc_hi >> (i*2)) & 3;

    size += texc_size_bytes(tex_vcf[0], tex0_fmt, tex0_elems);
    size += texc_size_bytes(tex_vcf[1], (g1 >>  1) & 7, (g1 >>  0) & 1);
    size += texc_size_bytes(tex_vcf[2], (g1 >> 10) & 7, (g1 >>  9) & 1);
    size += texc_size_bytes(tex_vcf[3], (g1 >> 19) & 7, (g1 >> 18) & 1);
    size += texc_size_bytes(tex_vcf[4], (g1 >> 28) & 7, (g1 >> 27) & 1);
    size += texc_size_bytes(tex_vcf[5], (g2 >>  6) & 7, (g2 >>  5) & 1);
    size += texc_size_bytes(tex_vcf[6], (g2 >> 15) & 7, (g2 >> 14) & 1);
    size += texc_size_bytes(tex_vcf[7], (g2 >> 24) & 7, (g2 >> 23) & 1);

    return size;
}

// Read a position from vertex data using the current CP state. Returns
// the position in model space as three floats, and tells the caller how
// many bytes the full vertex occupied.
//
// For indexed positions, we read the index and dereference cp_array_base
// + cp_array_stride using ppc_host_ptr. That gets us real geometry when
// the game ships vertex arrays out-of-band (very common).
static int read_vertex_pos(uint8_t vat_idx, const uint8_t *v,
                            float *ox, float *oy, float *oz) {
    uint8_t vidx = vat_idx & 7;
    uint32_t desc_lo = cp_vtx_desc_lo;
    uint32_t g0 = cp_vat_g0[vidx];

    // Skip matrix-idx bytes that precede position.
    int off = 0;
    uint32_t mat_bits = desc_lo & 0x1FF;
    while (mat_bits) { off += (mat_bits & 1); mat_bits >>= 1; }

    int pos_vcf   = (desc_lo >>  9) & 3;
    int pos_elems = (g0 >>  0) & 1;
    int pos_fmt   = (g0 >>  1) & 7;
    int pos_frac  = (g0 >>  4) & 0x1F;
    if (pos_vcf == 0) return 0;

    // For Direct: decode inline.
    if (pos_vcf == 1) {
        int n = pos_elems ? 3 : 2;
        float s = 1.0f / (float)(1u << pos_frac);
        float vals[3] = {0, 0, 0};
        const uint8_t *p = v + off;
        for (int i = 0; i < n; ++i) {
            switch (pos_fmt) {
                case 0: vals[i] = (float)(p[0])      * s; p += 1; break;  // UByte
                case 1: vals[i] = (float)((int8_t)p[0]) * s; p += 1; break; // Byte
                case 2: { uint16_t h = (p[0] << 8) | p[1]; vals[i] = (float)h * s; p += 2; break; }
                case 3: { int16_t  h = (int16_t)((p[0] << 8) | p[1]); vals[i] = (float)h * s; p += 2; break; }
                default: {   // Float
                    uint32_t w = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                               | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
                    float f; __builtin_memcpy(&f, &w, 4); vals[i] = f; p += 4; break;
                }
            }
        }
        *ox = vals[0]; *oy = vals[1]; *oz = vals[2];
        return 1;
    }

    // Indexed: read index, dereference CP array.
    uint32_t idx;
    if (pos_vcf == 2) { idx = v[off]; }
    else              { idx = ((uint32_t)v[off] << 8) | v[off + 1]; }
    uint32_t base   = cp_array_base[0];         // 0 = CP_ARRAY_POSITION
    uint32_t stride = cp_array_stride[0];
    if (stride == 0) return 0;
    uint32_t va = base + idx * stride;
    const uint8_t *p = (const uint8_t*)ppc_host_ptr(va);
    if (!p) return 0;

    int n = pos_elems ? 3 : 2;
    float s = 1.0f / (float)(1u << pos_frac);
    float vals[3] = {0, 0, 0};
    for (int i = 0; i < n; ++i) {
        switch (pos_fmt) {
            case 0: vals[i] = (float)(p[0])      * s; p += 1; break;
            case 1: vals[i] = (float)((int8_t)p[0]) * s; p += 1; break;
            case 2: { uint16_t h = (p[0] << 8) | p[1]; vals[i] = (float)h * s; p += 2; break; }
            case 3: { int16_t  h = (int16_t)((p[0] << 8) | p[1]); vals[i] = (float)h * s; p += 2; break; }
            default: {
                uint32_t w = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                           | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
                float f; __builtin_memcpy(&f, &w, 4); vals[i] = f; p += 4; break;
            }
        }
    }
    *ox = vals[0]; *oy = vals[1]; *oz = vals[2];
    return 1;
}

// Read vertex Color0 as host-order ABGR (bytes R,G,B,A in memory — what
// gx_ogl's GL_UNSIGNED_BYTE x4 attrib expects). Returns 1 on success.
// The GX channel default (GXInit, never overridden by this game: XF 0x100e
// is never written) is lighting OFF + matsrc = GX_SRC_VTX, so when the
// vertex format supplies Color0 the rasterized channel color IS the vertex
// color. Substituting the material register (0x00000000 at the title menu)
// rendered every widget transparent black — the invisible menu.
static int read_vertex_col0(uint8_t vat_idx, const uint8_t *v, uint32_t *abgr) {
    uint32_t desc_lo = cp_vtx_desc_lo;
    int col0_vcf = (desc_lo >> 13) & 3;
    if (col0_vcf == 0) return 0;

    uint8_t vidx = vat_idx & 7;
    uint32_t g0 = cp_vat_g0[vidx];
    int comp = (g0 >> 14) & 7;          /* Color0Comp: GX_RGB565..GX_RGBA8 */

    /* Byte offset of Color0 = matrix-index bytes + position + normal. */
    int off = 0;
    uint32_t mat_bits = desc_lo & 0x1FF;
    while (mat_bits) { off += (mat_bits & 1); mat_bits >>= 1; }
    {
        int pos_vcf   = (desc_lo >>  9) & 3;
        int norm_vcf  = (desc_lo >> 11) & 3;
        int pos_elems = (g0 >>  0) & 1;
        int pos_fmt   = (g0 >>  1) & 7;
        int norm_elems = (g0 >>  9) & 1;
        int norm_fmt   = (g0 >> 10) & 7;
        int norm_index3 = (g0 >> 31) & 1;
        off += pos_size_bytes (pos_vcf,  pos_fmt,  pos_elems);
        off += norm_size_bytes(norm_vcf, norm_fmt, norm_elems, norm_index3);
    }

    const uint8_t *p;
    if (col0_vcf == 1) {
        p = v + off;                     /* direct: inline in the vertex */
    } else {
        uint32_t idx = (col0_vcf == 2) ? v[off]
                     : (((uint32_t)v[off] << 8) | v[off + 1]);
        uint32_t base = cp_array_base[2], stride = cp_array_stride[2];
        if (!stride) return 0;
        p = (const uint8_t *)ppc_host_ptr(base + idx * stride);
        if (!p) return 0;
    }

    uint8_t r, g, b, a = 0xFF;
    switch (comp) {
        case 0: {  /* GX_RGB565 */
            uint16_t c = (uint16_t)((p[0] << 8) | p[1]);
            r = (uint8_t)(((c >> 11) & 0x1F) << 3);
            g = (uint8_t)(((c >>  5) & 0x3F) << 2);
            b = (uint8_t)(( c        & 0x1F) << 3);
            break;
        }
        case 1:    /* GX_RGB8  */
        case 2:    /* GX_RGBX8 */
            r = p[0]; g = p[1]; b = p[2];
            break;
        case 3: {  /* GX_RGBA4 */
            uint16_t c = (uint16_t)((p[0] << 8) | p[1]);
            r = (uint8_t)(((c >> 12) & 0xF) * 17);
            g = (uint8_t)(((c >>  8) & 0xF) * 17);
            b = (uint8_t)(((c >>  4) & 0xF) * 17);
            a = (uint8_t)(( c        & 0xF) * 17);
            break;
        }
        case 4: {  /* GX_RGBA6 */
            uint32_t c = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
            r = (uint8_t)(((c >> 18) & 0x3F) << 2);
            g = (uint8_t)(((c >> 12) & 0x3F) << 2);
            b = (uint8_t)(((c >>  6) & 0x3F) << 2);
            a = (uint8_t)(( c        & 0x3F) << 2);
            break;
        }
        default:   /* GX_RGBA8 */
            r = p[0]; g = p[1]; b = p[2]; a = p[3];
            break;
    }
    *abgr = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
    return 1;
}

// Compute the byte offset of the Tex0 attribute within a vertex, using the
// current CP state. Returns -1 if Tex0 is not present.
static int tex0_byte_offset(uint8_t vat_idx) {
    uint8_t vidx = vat_idx & 7;
    uint32_t desc_lo = cp_vtx_desc_lo;
    uint32_t g0 = cp_vat_g0[vidx];
    int tex0_vcf = (cp_vtx_desc_hi >> 0) & 3;
    if (tex0_vcf == 0) return -1;

    int off = 0;
    uint32_t mat_bits = desc_lo & 0x1FF;
    while (mat_bits) { off += (mat_bits & 1); mat_bits >>= 1; }

    int pos_vcf   = (desc_lo >>  9) & 3;
    int norm_vcf  = (desc_lo >> 11) & 3;
    int col0_vcf  = (desc_lo >> 13) & 3;
    int col1_vcf  = (desc_lo >> 15) & 3;

    int pos_elems = (g0 >>  0) & 1;
    int pos_fmt   = (g0 >>  1) & 7;
    int norm_elems = (g0 >>  9) & 1;
    int norm_fmt   = (g0 >> 10) & 7;
    int col0_comp  = (g0 >> 14) & 7;
    int col1_comp  = (g0 >> 18) & 7;
    int norm_index3 = (g0 >> 31) & 1;

    off += pos_size_bytes (pos_vcf,  pos_fmt,  pos_elems);
    off += norm_size_bytes(norm_vcf, norm_fmt, norm_elems, norm_index3);
    off += color_size_bytes(col0_vcf, col0_comp);
    off += color_size_bytes(col1_vcf, col1_comp);
    return off;
}

// Read vertex Tex0 into (s, t) floats. Returns 1 on success, 0 if Tex0 not
// present.
static int read_vertex_tex0(uint8_t vat_idx, const uint8_t *v,
                             float *os, float *ot) {
    int tex0_vcf = (cp_vtx_desc_hi >> 0) & 3;
    if (tex0_vcf == 0) { *os = 0; *ot = 0; return 0; }
    int off = tex0_byte_offset(vat_idx);
    if (off < 0) { *os = 0; *ot = 0; return 0; }

    uint8_t vidx = vat_idx & 7;
    uint32_t g0 = cp_vat_g0[vidx];
    int tex0_elems = (g0 >> 21) & 1;
    int tex0_fmt   = (g0 >> 22) & 7;
    int tex0_frac  = (g0 >> 25) & 0x1F;
    const uint8_t *p = v + off;
    float scale = 1.0f / (float)(1u << tex0_frac);
    float vals[2] = {0, 0};
    int n = tex0_elems ? 2 : 1;

    if (tex0_vcf == 1) {
        // Direct
        for (int i = 0; i < n; ++i) {
            switch (tex0_fmt) {
                case 0: vals[i] = (float)(p[0]) * scale; p += 1; break;
                case 1: vals[i] = (float)((int8_t)p[0]) * scale; p += 1; break;
                case 2: { uint16_t h = (p[0] << 8) | p[1];
                          vals[i] = (float)h * scale; p += 2; break; }
                case 3: { int16_t  h = (int16_t)((p[0] << 8) | p[1]);
                          vals[i] = (float)h * scale; p += 2; break; }
                default: {
                    uint32_t w = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                               | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
                    float f; __builtin_memcpy(&f, &w, 4); vals[i] = f; p += 4; break;
                }
            }
        }
    } else {
        // Indexed Tex0 via cp_array_base[4] / stride[4]
        uint32_t idx = (tex0_vcf == 2) ? p[0] : ((uint32_t)p[0] << 8) | p[1];
        uint32_t base = cp_array_base[4], stride = cp_array_stride[4];
        if (!stride) { *os = 0; *ot = 0; return 0; }
        const uint8_t *q = (const uint8_t*)ppc_host_ptr(base + idx * stride);
        if (!q) { *os = 0; *ot = 0; return 0; }
        for (int i = 0; i < n; ++i) {
            switch (tex0_fmt) {
                case 0: vals[i] = (float)(q[0]) * scale; q += 1; break;
                case 1: vals[i] = (float)((int8_t)q[0]) * scale; q += 1; break;
                case 2: { uint16_t h = (q[0] << 8) | q[1];
                          vals[i] = (float)h * scale; q += 2; break; }
                case 3: { int16_t  h = (int16_t)((q[0] << 8) | q[1]);
                          vals[i] = (float)h * scale; q += 2; break; }
                default: {
                    uint32_t w = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16)
                               | ((uint32_t)q[2] <<  8) |  (uint32_t)q[3];
                    float f; __builtin_memcpy(&f, &w, 4); vals[i] = f; q += 4; break;
                }
            }
        }
    }
    *os = vals[0];
    *ot = vals[1];
    return 1;
}

// ---------------------------------------------------------------------------
// Texture sampling.
//
// Wii/GC textures are stored in a TILED layout. For every format, the texture
// is broken into blocks (typical: 4x4 for RGB565/RGB5A3/RGBA8/IA8, 8x4 for
// I4/IA4, 8x8 for I8, 4x4 for CI4/CI8, 8x8 for CMPR). Within each block
// pixels go in row-major order, then the blocks themselves are row-major
// across the image.
//
// We implement a minimal set: I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8.
// CMPR (DXT1) needs 4x4 DXT1 block decode -- added next.
// ---------------------------------------------------------------------------

static inline uint32_t argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// Read a single pixel from a Wii texture.  s,t in [0,1) pre-wrapped.
// Returns ARGB8.
static uint32_t sample_tex_i4(const uint8_t *data, int w, int h, int u, int v) {
    // block 8x8, 4bpp -> 32 bytes/block
    int bx = u / 8, by = v / 4;
    int ix = u % 8, iy = v % 4;
    int blocks_per_row = (w + 7) / 8;
    const uint8_t *blk = data + (by * blocks_per_row + bx) * 32;
    int nib = blk[iy * 4 + ix / 2];
    int val = (ix & 1) ? (nib & 0xF) : (nib >> 4);
    uint8_t g = (uint8_t)((val * 255) / 15);
    return argb(255, g, g, g);
}
static uint32_t sample_tex_i8(const uint8_t *data, int w, int h, int u, int v) {
    // block 8x4, 8bpp -> 32 bytes
    int bx = u / 8, by = v / 4;
    int ix = u % 8, iy = v % 4;
    int blocks_per_row = (w + 7) / 8;
    const uint8_t *blk = data + (by * blocks_per_row + bx) * 32;
    uint8_t g = blk[iy * 8 + ix];
    return argb(255, g, g, g);
}
static uint32_t sample_tex_ia4(const uint8_t *data, int w, int h, int u, int v) {
    int bx = u / 8, by = v / 4;
    int ix = u % 8, iy = v % 4;
    int blocks_per_row = (w + 7) / 8;
    const uint8_t *blk = data + (by * blocks_per_row + bx) * 32;
    uint8_t b = blk[iy * 8 + ix];
    uint8_t a = (b & 0xF0);
    uint8_t i = (b & 0x0F) << 4;
    return argb(a, i, i, i);
}
static uint32_t sample_tex_ia8(const uint8_t *data, int w, int h, int u, int v) {
    int bx = u / 4, by = v / 4;
    int ix = u % 4, iy = v % 4;
    int blocks_per_row = (w + 3) / 4;
    const uint8_t *blk = data + (by * blocks_per_row + bx) * 32;
    uint16_t word = (blk[(iy * 4 + ix) * 2] << 8) | blk[(iy * 4 + ix) * 2 + 1];
    uint8_t i = word & 0xFF;
    uint8_t a = word >> 8;
    return argb(a, i, i, i);
}
static uint32_t sample_tex_rgb565(const uint8_t *data, int w, int h, int u, int v) {
    int bx = u / 4, by = v / 4;
    int ix = u % 4, iy = v % 4;
    int blocks_per_row = (w + 3) / 4;
    const uint8_t *blk = data + (by * blocks_per_row + bx) * 32;
    uint16_t p = (blk[(iy * 4 + ix) * 2] << 8) | blk[(iy * 4 + ix) * 2 + 1];
    uint8_t r = (uint8_t)((((p >> 11) & 0x1F) * 255 + 15) / 31);
    uint8_t g = (uint8_t)((((p >>  5) & 0x3F) * 255 + 31) / 63);
    uint8_t b = (uint8_t)(((p         & 0x1F) * 255 + 15) / 31);
    return argb(255, r, g, b);
}
static uint32_t sample_tex_rgb5a3(const uint8_t *data, int w, int h, int u, int v) {
    int bx = u / 4, by = v / 4;
    int ix = u % 4, iy = v % 4;
    int blocks_per_row = (w + 3) / 4;
    const uint8_t *blk = data + (by * blocks_per_row + bx) * 32;
    uint16_t p = (blk[(iy * 4 + ix) * 2] << 8) | blk[(iy * 4 + ix) * 2 + 1];
    uint8_t a, r, g, b;
    if (p & 0x8000) {            // opaque: 1bit + RGB555
        a = 255;
        r = (uint8_t)((((p >> 10) & 0x1F) * 255 + 15) / 31);
        g = (uint8_t)((((p >>  5) & 0x1F) * 255 + 15) / 31);
        b = (uint8_t)(((p         & 0x1F) * 255 + 15) / 31);
    } else {                     // RGBA4443-ish: 3bit A + 4bit each RGB
        a = (uint8_t)((((p >> 12) & 0x07) * 255 + 3) / 7);
        r = (uint8_t)((((p >>  8) & 0x0F) * 255 + 7) / 15);
        g = (uint8_t)((((p >>  4) & 0x0F) * 255 + 7) / 15);
        b = (uint8_t)(((p         & 0x0F) * 255 + 7) / 15);
    }
    return argb(a, r, g, b);
}
static uint32_t sample_tex_rgba8(const uint8_t *data, int w, int h, int u, int v) {
    // block 4x4, 64 bytes: 32 bytes AR then 32 bytes GB interleaved
    int bx = u / 4, by = v / 4;
    int ix = u % 4, iy = v % 4;
    int blocks_per_row = (w + 3) / 4;
    const uint8_t *blk = data + (by * blocks_per_row + bx) * 64;
    uint8_t a = blk[(iy * 4 + ix) * 2 + 0];
    uint8_t r = blk[(iy * 4 + ix) * 2 + 1];
    uint8_t g = blk[(iy * 4 + ix) * 2 + 32];
    uint8_t b = blk[(iy * 4 + ix) * 2 + 33];
    return argb(a, r, g, b);
}

static uint32_t sample_texture(int mapid, float s, float t) {
    if (mapid < 0 || mapid >= GX_MAX_TEXMAP || !g_tex[mapid].valid) {
        return 0xFF000000u;       // default black so missing textures are obvious
    }
    GxTexObj *tx = &g_tex[mapid];
    if (!tx->width || !tx->height) return 0xFF000000u;
    // Wrap
    float fs = s - (float)(int)s;
    float ft = t - (float)(int)t;
    if (fs < 0) fs += 1.0f;
    if (ft < 0) ft += 1.0f;
    int u = (int)(fs * (float)tx->width);  if (u < 0) u = 0; if (u >= (int)tx->width)  u = tx->width  - 1;
    int v = (int)(ft * (float)tx->height); if (v < 0) v = 0; if (v >= (int)tx->height) v = tx->height - 1;

    const uint8_t *data = (const uint8_t*)ppc_host_ptr(tx->data_va);
    if (!data) return 0xFF000000u;

    switch (tx->fmt) {
        case 0x00: return sample_tex_i4    (data, tx->width, tx->height, u, v);
        case 0x01: return sample_tex_i8    (data, tx->width, tx->height, u, v);
        case 0x02: return sample_tex_ia4   (data, tx->width, tx->height, u, v);
        case 0x03: return sample_tex_ia8   (data, tx->width, tx->height, u, v);
        case 0x04: return sample_tex_rgb565(data, tx->width, tx->height, u, v);
        case 0x05: return sample_tex_rgb5a3(data, tx->width, tx->height, u, v);
        case 0x06: return sample_tex_rgba8 (data, tx->width, tx->height, u, v);
        default: {
            // Unknown / CMPR: return a checker pattern so the caller at least
            // sees "this texture exists but I can't decode it" rather than
            // a silent black fill.
            return ((u ^ v) & 4) ? 0xFFFF00FFu : 0xFF000000u;
        }
    }
}

// Draw every valid GxTexObj as a thumbnail along the top of soft_efb so
// the user can see real game texture data. Not a HUD or fake overlay --
// every pixel is sampled from actual guest-memory texture bytes the game
// uploaded via GXInitTexObj.
void gx_debug_overlay(void) {
    int x = 4, y = 4;
    int panel_h = 0;
    for (int i = 0; i < GX_MAX_TEXMAP; ++i) {
        GxTexObj *tx = &g_tex[i];
        if (!tx->valid || !tx->width || !tx->height) continue;
        // Thumbnail: each tex is displayed in a fixed-size cell (128x128)
        // with aspect-preserving up/down scale so even a 1x1 or 4x256 is
        // clearly readable.
        int cell = 128;
        int tw, th;
        if ((int)tx->width >= (int)tx->height) {
            tw = cell;
            th = (int)((int64_t)cell * tx->height / tx->width);
            if (th < 8) th = 8;
        } else {
            th = cell;
            tw = (int)((int64_t)cell * tx->width / tx->height);
            if (tw < 8) tw = 8;
        }
        if (x + tw + 4 > EFB_W) { x = 4; y += panel_h + 4; panel_h = 0; }
        if (y + th + 10 > EFB_H) break;
        // Frame border (black) so each thumbnail is visible against
        // similarly-colored backgrounds.
        for (int dy = -1; dy <= th; ++dy) {
            for (int dx = -1; dx <= tw; ++dx) {
                int px = x + dx, py = y + dy;
                if (px < 0 || py < 0 || px >= EFB_W || py >= EFB_H) continue;
                if (dx == -1 || dy == -1 || dx == tw || dy == th) {
                    soft_efb[py * EFB_W + px] = 0xFF000000u;
                }
            }
        }
        for (int dy = 0; dy < th; ++dy) {
            for (int dx = 0; dx < tw; ++dx) {
                float s = (float)dx / (float)tw;
                float t = (float)dy / (float)th;
                soft_efb[(y + dy) * EFB_W + (x + dx)] = sample_texture(i, s, t);
            }
        }
        x += tw + 4;
        if (th > panel_h) panel_h = th;
        soft_efb_has_content = 1;
    }
    // If NO textures are valid, write a small "no textures" marker so
    // the viewer knows the overlay ran but had nothing to show.
    static int announced;
    if (!g_tex[0].valid && !g_tex[1].valid && !announced) {
        for (int dy = 0; dy < 8; ++dy)
            for (int dx = 0; dx < 8; ++dx)
                soft_efb[(4 + dy) * EFB_W + (4 + dx)] = 0xFFFF0000u;
        soft_efb_has_content = 1;
        announced = 1;
    }
}

// True when `va..va+n` lies entirely inside real guest RAM (MEM1 cached or
// uncached, or MEM2). Used to reject FIFO commands that reference pointers
// which would silently resolve to the deadzone (desync artifacts).
static int gx_va_is_ram(uint32_t va, uint32_t n) {
    if (va >= 0x80000000u && va + n <= 0x81800000u) return 1;   // MEM1
    if (va >= 0xC0000000u && va + n <= 0xC1800000u) return 1;   // MEM1 uncached
    if (va >= 0x90000000u && va + n <= 0x94000000u) return 1;   // MEM2
    return 0;
}

// Trace of the most recent opcodes the parser dispatched, dumped on desync
// so the byte-slip that caused it is visible in the log.
typedef struct { uint8_t op; uint32_t start; } gx_cmd_trace_t;
static gx_cmd_trace_t gx_cmd_trace[32];
static uint32_t gx_cmd_trace_n;

// Desync forensics: hexdump the ring around the failure point plus the
// recent-command trace. `head_at_fail` points just past the bad opcode.
static void gx_fifo_forensics(uint8_t op, uint32_t head_at_fail) {
    static int s_n;
    if (s_n >= 6) return;
    s_n++;
    fprintf(stderr,
            "[GX-FIFO] DESYNC #%d op=0x%02x head=%u avail=%u desc=%08x:%08x\n",
            s_n, op, head_at_fail, gx_fifo_tail - head_at_fail,
            cp_vtx_desc_hi, cp_vtx_desc_lo);
    fprintf(stderr, "  prev48:");
    for (int i = 48; i >= 1; --i)
        fprintf(stderr, " %02x",
                gx_fifo_ring[(head_at_fail - 1u - (uint32_t)i) & (GX_FIFO_RING - 1)]);
    fprintf(stderr, "\n  next32:");
    for (int i = 0; i < 32; ++i)
        fprintf(stderr, " %02x",
                gx_fifo_ring[(head_at_fail + (uint32_t)i) & (GX_FIFO_RING - 1)]);
    fprintf(stderr, "\n  last cmds (op@ringpos, oldest first):");
    for (uint32_t i = 0; i < 32; ++i) {
        gx_cmd_trace_t *t = &gx_cmd_trace[(gx_cmd_trace_n + i) & 31];
        if (t->op || t->start) fprintf(stderr, " %02x@%u", t->op, t->start);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

// Ring lock: guest threads are real host threads here, and any of them can
// hit the write-gather pipe. Serialize push+parse so two emitters can't
// interleave bytes mid-command. Uncontended cost is one atomic exchange.
static volatile int gx_fifo_lock_word;
static void gx_fifo_lock(void)   { while (__sync_lock_test_and_set(&gx_fifo_lock_word, 1)) { } }
static void gx_fifo_unlock(void) { __sync_lock_release(&gx_fifo_lock_word); }

static void gx_fifo_push_ring(const uint8_t *bytes, int n);

/* Fade hunt: LR + BG-word of the most recent TEV konst0 write (see bp_write);
 * read by the [POST-TEV] probe in gx_ogl.c to name the post-pass fade owner. */
uint32_t g_last_k0_lr, g_last_k0_val;

void gx_fifo_push(const uint8_t *bytes, int n) {
    /* NOTE: do NOT put profiler scope timers here — the guest's write-gather
     * pipe calls this tens of thousands of times per frame (a few bytes at a
     * time), so two QueryPerformanceCounter calls per invocation cost
     * milliseconds per frame. The GX scope is taken per PRIMITIVE instead,
     * around raster_primitive(), which is ~600 calls/frame. */
    /* Display-list recording: while GXBeginDisplayList is active, the
     * write-gather pipe streams into the caller's buffer instead of the
     * live FIFO (mirrors real hardware redirecting the CP gather pipe).
     * Recording state is THREAD-LOCAL: only the thread that called
     * GXBeginDisplayList diverts; other threads' pushes keep flowing to
     * the live ring (previously a loader thread recording while the render
     * thread emitted would steal the render thread's bytes into the DL). */
    extern _Thread_local uint32_t g_gx_dl_va, g_gx_dl_cap, g_gx_dl_len, g_gx_dl_lost;
    if (g_gx_dl_va) {
        uint8_t *dst = (uint8_t *)ppc_host_ptr(g_gx_dl_va + g_gx_dl_len);
        if (dst && g_gx_dl_len + (uint32_t)n <= g_gx_dl_cap) {
            memcpy(dst, bytes, (size_t)n);
            g_gx_dl_len += (uint32_t)n;
        } else {
            // A dropped push punches a hole in the recorded list; replaying
            // a hole-y DL is guaranteed parser desync. Track it so End/Call
            // can warn (and so we know recording overflow is the culprit).
            g_gx_dl_lost += (uint32_t)n;
        }
        return;
    }
    gx_fifo_lock();
    gx_fifo_push_ring(bytes, n);
    gx_fifo_unlock();
}

static void gx_fifo_push_ring(const uint8_t *bytes, int n) {
    // Overflow guard: if this push would lap unread bytes, the pending
    // stream is unrecoverable (old and new bytes would interleave once the
    // write cursor wraps over the read cursor). Drop the pending stream and
    // restart clean at this push's boundary -- one bounded glitch instead
    // of megabytes of interleaved garbage.
    {
        uint32_t pending = gx_fifo_tail - gx_fifo_head;
        if ((uint32_t)n > GX_FIFO_RING - pending) {
            static uint32_t s_ovf;
            if (++s_ovf <= 16 || (s_ovf & 0xFF) == 0) {
                fprintf(stderr,
                        "[GX-FIFO] OVERFLOW #%u push=%d pending=%u -- dropping pending stream\n",
                        s_ovf, n, pending);
                fflush(stderr);
            }
            gx_fifo_head = gx_fifo_tail;
            if ((uint32_t)n > GX_FIFO_RING) {
                bytes += (uint32_t)n - GX_FIFO_RING;
                n = GX_FIFO_RING;
            }
        }
    }
    for (int i = 0; i < n; ++i)
        gx_fifo_ring[(gx_fifo_tail + i) & (GX_FIFO_RING - 1)] = bytes[i];
    gx_fifo_tail += (uint32_t)n;

    while (fifo_avail() > 0) {
        uint32_t start = gx_fifo_head;
        uint8_t op = fifo_read();
        // Record every considered opcode; dumped by gx_fifo_forensics on
        // desync so the lead-up (last good commands + the slip) is visible.
        gx_cmd_trace[gx_cmd_trace_n & 31].op = op;
        gx_cmd_trace[gx_cmd_trace_n & 31].start = start;
        gx_cmd_trace_n++;
        // Count each opcode byte once so we can see the mix after a run.
        static uint32_t op_histo[256];
        op_histo[op]++;
        static uint32_t op_total;
        if ((++op_total) == 100000) {
            fprintf(stderr, "[GX-FIFO] opcode histogram after 100k opcodes:\n");
            for (int i = 0; i < 256; ++i) {
                if (op_histo[i]) {
                    fprintf(stderr, "  op 0x%02x : %u\n", i, op_histo[i]);
                }
            }
            fflush(stderr);
        }
        if (op == 0x00) continue;                                   // NOP
        if (op == 0x44) continue;                                   // GX_CMD_UNKNOWN_METRICS — 1-byte, no payload
        if (op == 0x47) continue;                                   // PE Finish marker — 1-byte per Dolphin's
                                                                    // OpcodeDecoding.cpp. YAGCD shows games
                                                                    // emit it as `*(u32*)FIFO = 0x4700XXXX`,
                                                                    // i.e. byte stream [0x47, 0x00, 0xXX, 0xXX].
                                                                    // The trailing 3 bytes are processed as
                                                                    // separate opcodes (0x00 = NOP, then the
                                                                    // 16-bit token value as two opcodes — usually
                                                                    // benign because token values are small or
                                                                    // followed by aligned commands).
        if (op == 0x48) continue;                                   // GX_CMD_INVL_VC (vertex-cache invalidate) —
                                                                    // 1-byte per Dolphin. Same handling as 0x47.
        if (op == 0x61) {                                           // Load BP
            if (fifo_avail() < 4) { gx_fifo_head = start; return; }
            uint32_t w = ((uint32_t)fifo_read() << 24) |
                         ((uint32_t)fifo_read() << 16) |
                         ((uint32_t)fifo_read() <<  8) |
                         ((uint32_t)fifo_read()      );
            bp_write(w >> 24, w & 0x00FFFFFF);
            continue;
        }
        if (op == 0x08) {                                           // Load CP (1 reg, 4-byte val)
            if (fifo_avail() < 5) { gx_fifo_head = start; return; }
            uint8_t reg = fifo_read();
            uint32_t val = ((uint32_t)fifo_read() << 24) |
                           ((uint32_t)fifo_read() << 16) |
                           ((uint32_t)fifo_read() <<  8) |
                           ((uint32_t)fifo_read()      );
            cp_write(reg, val);
            static int cp_first[256];
            if (reg < 256 && !cp_first[reg]) {
                cp_first[reg] = 1;
                fprintf(stderr, "[CP] first write: reg=0x%02x val=0x%08x\n", reg, val);
                fflush(stderr);
            }
            continue;
        }
        if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {  // Indexed XF load
            // WAS A STUB (payload discarded) — which left XF matrix memory
            // ZERO for everything loaded this way. The menu/UI widgets load
            // their position matrices exclusively via indexed XF loads, so
            // all UI geometry transformed by a zero matrix: the invisible
            // (black) menu. Payload: index[31:16], (count-1)[15:12],
            // xf_addr[11:0]; source = CP array 12..15 (op 0x20/28/30/38)
            // at cp_array_base + index*stride, count 32-bit BE words.
            if (fifo_avail() < 4) { gx_fifo_head = start; return; }
            uint32_t w = ((uint32_t)fifo_read() << 24) |
                         ((uint32_t)fifo_read() << 16) |
                         ((uint32_t)fifo_read() <<  8) |
                         ((uint32_t)fifo_read()      );
            uint32_t idx     = w >> 16;
            uint32_t count   = ((w >> 12) & 0xFu) + 1;
            uint32_t xf_addr = w & 0xFFFu;
            uint32_t arr     = 12 + ((op - 0x20) >> 3);
            uint32_t base    = cp_array_base[arr];
            uint32_t stride  = cp_array_stride[arr];
            if (base && stride) {
                uint32_t src = base + idx * stride;
                uint8_t buf[16 * 4];
                const uint8_t *p = (const uint8_t *)ppc_host_ptr(src);
                for (uint32_t i = 0; i < count * 4 && i < sizeof buf; ++i) buf[i] = p[i];
                xf_write(xf_addr, buf, count);
                static uint32_t s_n;
                if (s_n < 8 || (s_n & 0xFFFu) == 0) {
                    fprintf(stderr, "[XF-INDX#%u] op=0x%02x arr=%u idx=%u count=%u xf=0x%03x src=0x%08x\n",
                            s_n, op, arr, idx, count, xf_addr, src);
                    fflush(stderr);
                }
                s_n++;
            }
            continue;
        }
        if (op == 0x40) {                                           // Call display list
            // YAGCD: 4 bytes of DL VA + 4 bytes of DL size. We follow the
            // list by pushing its bytes into the FIFO ring at the head of
            // the queue. Real hardware reads them via DMA from main memory;
            // we just mirror them and let the same loop process them.
            if (fifo_avail() < 8) { gx_fifo_head = start; return; }
            uint32_t dl_va   = ((uint32_t)fifo_read() << 24) |
                               ((uint32_t)fifo_read() << 16) |
                               ((uint32_t)fifo_read() <<  8) |
                               ((uint32_t)fifo_read()      );
            uint32_t dl_size = ((uint32_t)fifo_read() << 24) |
                               ((uint32_t)fifo_read() << 16) |
                               ((uint32_t)fifo_read() <<  8) |
                               ((uint32_t)fifo_read()      );
            // Validate hard. Every in-stream CallDL observed so far has been
            // a desync artifact (garbage VA like 0x00520052 -> deadzone);
            // accepting one injects megabytes of junk into a 1 MB ring,
            // lapping the read cursor and destroying every pending command
            // (the full-screen flashing during world load). Real hardware
            // requires DL address and size to be 32-byte aligned, and the
            // address must be actual RAM (games pass the physical address;
            // translate like the CP array bases).
            uint32_t dl_vva = (dl_va < 0x80000000u) ? dl_va + 0x80000000u : dl_va;
            if ((dl_va & 31u) || (dl_size & 31u) ||
                dl_size == 0 || dl_size > 0x00100000u ||
                !gx_va_is_ram(dl_vva, dl_size) ||
                dl_size > GX_FIFO_RING - fifo_avail()) {
                static uint32_t s_rej;
                if (++s_rej <= 16 || (s_rej & 0x3FF) == 0) {
                    fprintf(stderr, "[GX-FIFO] CallDL @0x%08x size=%u rejected (invalid)\n",
                            dl_va, dl_size);
                    fflush(stderr);
                }
                gx_fifo_forensics(op, start + 1);
                // Bogus CallDL = we are desynced; resync one byte past the
                // 0x40 rather than trusting the 8 "argument" bytes.
                gx_fifo_head = start + 1;
                continue;
            }
            const uint8_t *dl = (const uint8_t *)ppc_host_ptr(dl_vva);
            if (!dl) continue;
            // Execute the DL BEFORE the rest of the current stream (real
            // hardware semantics): insert its bytes at the READ cursor so
            // the next loop iterations consume them first, then resume the
            // caller's remaining bytes. Inserting at [head-size, head) only
            // touches already-consumed ring space -- safe as long as the
            // unread span plus the DL fits the ring. Nested CallDLs inside
            // the inserted bytes re-enter here and stack correctly (DFS).
            if (fifo_avail() + dl_size <= GX_FIFO_RING - 64) {
                gx_fifo_head -= dl_size;
                for (uint32_t i = 0; i < dl_size; ++i)
                    gx_fifo_ring[(gx_fifo_head + i) & (GX_FIFO_RING - 1)] = dl[i];
            } else {
                // Fallback: tail-splice (wrong order vs. the pending bytes,
                // but better than dropping). Should be unreachable for the
                // small material DLs this game nests.
                fprintf(stderr, "[GX-FIFO] CallDL tail-splice fallback @0x%08x size=%u avail=%u\n",
                        dl_va, dl_size, fifo_avail());
                fflush(stderr);
                for (uint32_t i = 0; i < dl_size; ++i) {
                    gx_fifo_ring[gx_fifo_tail & (GX_FIFO_RING - 1)] = dl[i];
                    gx_fifo_tail++;
                }
            }
            static uint32_t dl_count;
            if (++dl_count <= 5 || (dl_count & 0x3FF) == 0) {
                fprintf(stderr, "[GX-FIFO] CallDL #%u @0x%08x size=%u\n",
                        dl_count, dl_va, dl_size);
                fflush(stderr);
            }
            continue;
        }
        // 0x44: vertex-cache invalidate (no payload). Already handled above
        // as a NOP-ish opcode at the top of the dispatch.
        if (op == 0x10) {                                           // Load XF
            if (fifo_avail() < 4) { gx_fifo_head = start; return; }
            uint32_t nh_hi = (uint32_t)fifo_read();
            uint32_t nh_lo = (uint32_t)fifo_read();
            uint32_t nh = (nh_hi << 8) | nh_lo;
            uint32_t base_hi = (uint32_t)fifo_read();
            uint32_t base_lo = (uint32_t)fifo_read();
            uint32_t base = (base_hi << 8) | base_lo;
            // Per Dolphin OpcodeDecoding.cpp: stream size is masked to 4 bits.
            // Hardware allows at most 16 transfers per LoadXF command. Without
            // this mask, a malformed FIFO with high bits set in nh would have
            // us over-consume the payload and desync.
            nh &= 0xFu;
            uint32_t payload = (nh + 1) * 4;
            if (fifo_avail() < payload) { gx_fifo_head = start; return; }
            // Snapshot the payload into a temp buffer so xf_write can parse
            // without touching the ring.
            uint8_t xf_buf[256];
            if (payload <= sizeof xf_buf) {
                for (uint32_t i = 0; i < payload; ++i) xf_buf[i] = fifo_read();
                xf_write(base, xf_buf, (nh + 1));
            } else {
                for (uint32_t i = 0; i < payload; ++i) fifo_read();
            }
            continue;
        }
        if ((op & 0xC0) == 0x80 && op < 0xC0) {                     // Draw primitive
            // Per Dolphin's OpcodeDecoding.cpp:
            //   GX_PRIMITIVE_START = 0x80, GX_PRIMITIVE_END = 0xbf
            //   GX_PRIMITIVE_MASK  = 0x78  (bits 5:3 = primitive type)
            //   GX_VAT_MASK        = 0x07  (bits 2:0 = VAT index)
            // Old mask `(op & 0xE0) == 0x80` was WRONG — it only matched
            // 0x80-0x9F, missing TRIANGLE_FAN (0xA0), LINES (0xA8),
            // LINE_STRIP (0xB0), POINTS (0xB8). Half the primitive opcodes
            // were silently dropped.
            if (fifo_avail() < 2) { gx_fifo_head = start; return; }
            uint32_t nvtx = (uint32_t)fifo_read() << 8;
            nvtx         |= fifo_read();
            // Use real vertex size from current CP/VAT state. If still 0
            // (CP never programmed), treat as a no-payload draw so the
            // decoder doesn't consume bytes we shouldn't.
            uint32_t vsize = gx_vertex_size(op & 7);
            uint32_t payload = nvtx * vsize;
            // Sanity cap: a desynced parse that lands on a 0x80-0xBF data
            // byte fabricates a draw whose 16-bit count is garbage (observed:
            // vtx=26175 -> 130 KB "payload"). Left alone it stalls the parser
            // until that much accumulates, then swallows it all -- entire
            // frames of real commands vanish (the model-switching flicker).
            // Largest legitimate draws observed are a few thousand verts, so
            // treat anything wildly past that as a desync artifact: put back
            // the count bytes and resync one byte past the bogus opcode.
            // (largest legitimate draw observed in logs: 2118 verts; the
            // desync-born fakes claim 15421-55600)
            if (nvtx > 8192 || payload > (256u << 10)) {
                static uint32_t s_bad;
                if (++s_bad <= 8 || (s_bad & 0x3FF) == 0) {
                    fprintf(stderr,
                            "[GX-FIFO] implausible draw #%u op=0x%02x vtx=%u vsize=%u -- desync skip\n",
                            s_bad, op, nvtx, vsize);
                    fflush(stderr);
                }
                gx_fifo_forensics(op, start + 1);
                gx_fifo_head = start + 1;
                continue;
            }
            if (fifo_avail() < payload) { gx_fifo_head = start; return; }
            if (rasterizer_enabled < 0) {
                const char *e = getenv("RECOMP_RASTERIZE");
                rasterizer_enabled = !(e && e[0] == '0');   // default ON
            }
            static uint8_t vbuf[256 * 1024];
            if (rasterizer_enabled && payload <= sizeof(vbuf)) {
                for (uint32_t i = 0; i < payload; ++i) vbuf[i] = fifo_read();
                robox_prof_scope_begin(ROBOX_PROF_GXFIFO);
                raster_primitive(op, nvtx, vbuf, vsize);
                robox_prof_scope_end(ROBOX_PROF_GXFIFO);
            } else {
                for (uint32_t i = 0; i < payload; ++i) fifo_read();
            }
            static uint32_t draws;
            draws++;
            // Per-draw dumps: OFF unless RECOMP_GX_TRACE=1. At ~20k draws/sec
            // these were writing ~600 flushed stderr lines per second (26 MB
            // logs) and were a real source of stutter.
            static uint32_t real_draw_log;
            int log_this = 0;
            if (recomp_gx_trace()) {
                // Log the first 3 draws, then sample every 256th real
                // (non-empty) draw so we can see if the game is drawing new
                // content or just repeating the splash quad.
                log_this = (draws <= 3);
                if (!log_this && nvtx > 0 &&
                    (++real_draw_log <= 10 || (real_draw_log & 0xFF) == 1)) {
                    log_this = 1;
                }
            }
            // STATE CENSUS (menu-fidelity hunt): dump every DISTINCT draw
            // configuration once — (tref0, tev-stage0, bound tex, prim,
            // genmode). Periodic sampling kept missing the low-frequency
            // wash draw; a census cannot.
            if (!log_this && nvtx > 0 && recomp_gx_trace()) {
                extern uint32_t gx_ogl_get_bp(uint32_t reg);
                uint32_t sig = gx_ogl_get_bp(0x28) ^ (gx_ogl_get_bp(0xC0) * 31u)
                             ^ (g_tex[0].data_va * 7u) ^ ((uint32_t)op << 24)
                             ^ (gx_ogl_get_bp(0x00) * 131u);
                static uint32_t s_seen[96]; static int s_ns;
                int dup = 0;
                for (int i = 0; i < s_ns; ++i) if (s_seen[i] == sig) { dup = 1; break; }
                if (!dup && s_ns < 96) {
                    s_seen[s_ns++] = sig;
                    log_this = 2;   /* census hit — full dump below */
                }
            }
            if (log_this == 2) {
                fprintf(stderr, "[GX-CENSUS] new draw config:\n");
            }
            if (log_this) {
                fprintf(stderr, "[GX-FIFO] draw #%u op=0x%02x vtx=%u vsize=%u desc=%08x:%08x g0=%08x\n",
                        draws, op, nvtx, vsize, cp_vtx_desc_hi, cp_vtx_desc_lo,
                        cp_vat_g0[op & 7]);
                // Dump raw payload bytes
                fprintf(stderr, "    payload (%u bytes):", payload);
                for (uint32_t i = 0; i < payload && i < 64; ++i) {
                    fprintf(stderr, " %02x", vbuf[i]);
                }
                fprintf(stderr, "\n");
                // Dump each vert's decoded pos and its projected screen pos
                for (uint32_t i = 0; i < nvtx && i < 8; ++i) {
                    float vx=0, vy=0, vz=0;
                    if (read_vertex_pos(op & 7, vbuf + i * vsize, &vx, &vy, &vz)) {
                        int sx, sy;
                        uint32_t vc = 0;
                        int has_c = read_vertex_col0(op & 7, vbuf + i * vsize, &vc);
                        project_vertex(vx, vy, vz, &sx, &sy);
                        fprintf(stderr, "    v%u pos=(%.3f,%.3f,%.3f) screen=(%d,%d) col0=%s%08x\n",
                                i, vx, vy, vz, sx, sy, has_c ? "" : "none/", vc);
                    }
                }
                // Active position matrix at this draw (black-menu hunt: the
                // widget draws transform through an all-zero slot 0).
                {
                    const float *pm = xf_pos_mtx[xf_pos_mtx_idx];
                    fprintf(stderr,
                        "    posmtx[%d] = [%g %g %g %g | %g %g %g %g | %g %g %g %g]\n",
                        xf_pos_mtx_idx,
                        pm[0],pm[1],pm[2],pm[3], pm[4],pm[5],pm[6],pm[7],
                        pm[8],pm[9],pm[10],pm[11]);
                }
                // Texture state at this draw (menu-texture hunt): the bound
                // map0 + which map/coord each TEV stage references.
                {
                    extern uint32_t gx_ogl_get_bp(uint32_t reg);
                    uint32_t tref0 = gx_ogl_get_bp(0x28);
                    fprintf(stderr,
                        "    tex0: valid=%d va=0x%08x %ux%u fmt=%u | tref0=0x%06x tev1c=0x%06x tev1a=0x%06x\n",
                        g_tex[0].valid, g_tex[0].data_va, g_tex[0].width,
                        g_tex[0].height, g_tex[0].fmt, tref0,
                        gx_ogl_get_bp(0xC2), gx_ogl_get_bp(0xC3));
                    fprintf(stderr,
                        "    tev0c=0x%06x tev0a=0x%06x tev2c=0x%06x tev3c=0x%06x tref1=0x%06x blend=0x%06x\n",
                        gx_ogl_get_bp(0xC0), gx_ogl_get_bp(0xC1),
                        gx_ogl_get_bp(0xC4), gx_ogl_get_bp(0xC6),
                        gx_ogl_get_bp(0x29), gx_ogl_get_bp(0x41));
                }
                // Menu-fidelity hunt: texgen matrix + TEV constant/register
                // banks (the blue tint and the atlas subrect both live here).
                {
                    extern int gx_ogl_get_tex0_mtx(float rows[8]);
                    extern void gx_ogl_get_tev_colors(uint8_t k[16], uint8_t t[16]);
                    float tm[8]; uint8_t kc[16], tr[16];
                    int slot = gx_ogl_get_tex0_mtx(tm);
                    gx_ogl_get_tev_colors(kc, tr);
                    fprintf(stderr,
                        "    texmtx[%d]=[%g %g %g %g | %g %g %g %g]"
                        " konst0=%02x%02x%02x%02x konst1=%02x%02x%02x%02x"
                        " treg0=%02x%02x%02x%02x treg1=%02x%02x%02x%02x\n",
                        slot, tm[0],tm[1],tm[2],tm[3], tm[4],tm[5],tm[6],tm[7],
                        kc[0],kc[1],kc[2],kc[3], kc[4],kc[5],kc[6],kc[7],
                        tr[0],tr[1],tr[2],tr[3], tr[4],tr[5],tr[6],tr[7]);
                }
                // Indexed positions: show the raw array bytes the first
                // vertex dereferences, so array-content vs parse bugs are
                // distinguishable.
                {
                    uint32_t desc_lo = cp_vtx_desc_lo;
                    int pos_vcf = (desc_lo >> 9) & 3;
                    if (pos_vcf >= 2 && nvtx > 0) {
                        int moff = 0;
                        uint32_t mat_bits = desc_lo & 0x1FF;
                        while (mat_bits) { moff += (mat_bits & 1); mat_bits >>= 1; }
                        uint32_t idx0 = (pos_vcf == 2) ? vbuf[moff]
                                      : (((uint32_t)vbuf[moff] << 8) | vbuf[moff + 1]);
                        uint32_t base = cp_array_base[0], stride = cp_array_stride[0];
                        uint32_t va = base + idx0 * stride;
                        const uint8_t *ap = (const uint8_t *)ppc_host_ptr(va);
                        fprintf(stderr,
                            "    posarray base=0x%08x stride=%u idx0=%u -> va=0x%08x:",
                            base, stride, idx0, va);
                        if (ap) for (int i = 0; i < 48; ++i) fprintf(stderr, " %02x", ap[i]);
                        fprintf(stderr, "\n");
                    }
                }
                // Peek at next few bytes after payload
                fprintf(stderr, "    next bytes:");
                for (uint32_t i = 0; i < 16 && fifo_avail() > i; ++i) {
                    fprintf(stderr, " %02x",
                            gx_fifo_ring[(gx_fifo_head + i) & (GX_FIFO_RING - 1)]);
                }
                fprintf(stderr, "\n");
                fflush(stderr);
            }
            continue;
        }
        // Truly unknown opcode. Log once per value so we can diagnose.
        static uint32_t unk_seen[8] = {0};
        int known_i = -1;
        for (int i = 0; i < 8; ++i) {
            if ((unk_seen[i] >> 8) == op) { known_i = i; break; }
            if (unk_seen[i] == 0) {
                unk_seen[i] = ((uint32_t)op << 8) | 1;
                fprintf(stderr, "[GX-FIFO] unknown opcode 0x%02x -- resyncing\n", op);
                fflush(stderr);
                gx_fifo_forensics(op, gx_fifo_head);
                break;
            }
        }
        // Resync: just skip this single byte and try the next. Going to
        // next NOP is too aggressive -- NOPs appear inside vertex payloads.
        // Single-byte advance means we quickly find real opcode alignment
        // once things are back in sync.
    }
}


// ===========================================================================
// VI -- Video Interface (the Wii's framebuffer scanout hardware).
// ===========================================================================

static uint32_t vi_retrace_count = 0;
uint32_t vi_xfb_va = 0;   // non-static so gx_execute_copy's forward-decl resolves

extern void video_init(void);
extern void video_present(void);
extern void video_copy_efb_to_window(uint32_t);
extern void video_blit_argb(const uint32_t *pixels);

void hle_VIInit              (void) {
    video_init();   // pop the window the moment the game asks for VI
    HLE_RET(0);
}
void hle_VIConfigure         (void) { HLE_RET(0); }  // no-op: we set our own GL resolution
void hle_VISetNextFrameBuffer(void) {
    // Game's chosen scanout target. Store as-is; we do NOT second-guess it
    // or substitute a value -- if the game says scan 0, we scan 0 (black).
    vi_xfb_va = HLE_ARG_U32(0);
    HLE_RET(0);
}
void hle_VISetBlack          (void) {   // no-op blanker, but log for fade hunt
    static int s_last = -1; int b = (int)HLE_ARG_U32(0);
    if (b != s_last) { s_last = b;
        fprintf(stderr, "[FADE?] VISetBlack(%d)\n", b); fflush(stderr); }
    HLE_RET(0);
}
void hle_VIFlush             (void) { HLE_RET(0); }

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>   /* timeBeginPeriod: 1ms scheduler granularity for the frame limiter */
#endif

// Counter incremented by every VIWaitForRetrace call. Watchdog thread
// polls this; if it stops advancing, we dump. uint32 volatile is
// naturally atomic on x86 with the memory ordering we need.
volatile uint32_t g_vi_tick = 0;
/* Per-frame liveness signal for the freeze watchdog. hle_K3D_Flip bumps this
 * every flip. Unlike g_vi_tick (VIWaitForRetrace — this game calls it only
 * twice at boot), this advances 60x/sec while the game loop is alive, so if it
 * STOPS the game thread is genuinely hung (vs the window just not repainting). */
volatile uint32_t g_flip_count = 0;
volatile uint64_t g_vi_last_retrace_ms = 0;

// VI retrace callbacks the game registers via VISetPost/PreRetraceCallback.
// hle_VIWaitForRetrace fires these on every retrace tick.
uint32_t vi_post_retrace_cb = 0;
uint32_t vi_pre_retrace_cb  = 0;

/* Real-hardware VI retraces tick at 60 Hz from an interrupt regardless of
 * whether anyone waits. The game calls VIWaitForRetrace only twice at boot
 * and afterwards just READS VIGetRetraceCount for its timelines -- with the
 * old only-advance-on-wait model the counter froze at 2 and every
 * retrace-timed animation (logo fades!) stood still forever. This pump is
 * called once per K3D::Flip: it advances the counter on a host 60 Hz clock
 * and fires the registered pre/post retrace callbacks (capped catch-up). */
void vi_pump_retraces(void) {
    static uint64_t s_last_ms;
    uint64_t now = ms_now();
    if (!s_last_ms) { s_last_ms = now; return; }
    uint32_t ticks = (uint32_t)((now - s_last_ms) * 60ull / 1000ull);
    if (!ticks) return;
    if (ticks > 6) ticks = 6;              /* cap catch-up after stalls */
    s_last_ms = now;
    for (uint32_t i = 0; i < ticks; ++i) {
        vi_retrace_count++;
        if (vi_pre_retrace_cb) {
            g_cpu.gpr[3] = vi_retrace_count;
            ppc_call_indirect(vi_pre_retrace_cb);
        }
        if (vi_post_retrace_cb) {
            g_cpu.gpr[3] = vi_retrace_count;
            ppc_call_indirect(vi_post_retrace_cb);
        }
    }
    /* fire queued async IOS completions at the same interrupt point */
    { extern void ios_async_pump(void); ios_async_pump(); }
    /* fire due host-side OS alarms (decrementer isn't emulated); paces the
     * DvdStreamThreadProc music feeder among others -- quirks/lyn_stream.c */
    { extern void lyn_alarm_pump(void); lyn_alarm_pump(); }
    /* AX 5 ms audio frame callback: ~3 fires per 60 Hz retrace */
    {
        extern uint32_t g_ax_frame_cb;
        if (g_ax_frame_cb) {
            for (int k = 0; k < 3; ++k) ppc_call_indirect(g_ax_frame_cb);
        }
    }
    /* mix the AX voices the game just updated (one video frame of audio) */
    { extern void ax_mixer_frame(void); ax_mixer_frame(); }
    static int s_n;
    if (s_n < 5) {
        fprintf(stderr, "[VI] pump: retrace=%u pre_cb=0x%08x post_cb=0x%08x\n",
                vi_retrace_count, vi_pre_retrace_cb, vi_post_retrace_cb);
        fflush(stderr);
        s_n++;
    }
}

uint64_t ms_now(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq = {0};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (uint64_t)((t.QuadPart * 1000) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

/* Software frame limiter — paces the caller to the NTSC field rate (59.94 Hz)
 * regardless of the host monitor's refresh. The game advances ONE simulation
 * step per K3D::Flip and calls Flip as fast as the recompiled CPU can go
 * (600+/sec), so without a limiter the whole game runs ~10x too fast. Present
 * was already throttled to 60 Hz, but that only slowed the *display*, not the
 * simulation — this throttles the simulation itself.
 *
 * Absolute-deadline pacing (accumulate the next target, don't measure deltas)
 * so small per-frame jitter doesn't drift. Sleep for the bulk of the wait,
 * busy-spin the final <2 ms for accuracy. Cap via RECOMP_FPS_CAP (0 = uncapped). */
void frame_limiter(void) {
    static int env_cap = -2;
    if (env_cap == -2) {
        const char *e = getenv("RECOMP_FPS_CAP");
        env_cap = e ? atoi(e) : 0;   /* 0 = use the adaptive default below */
#if defined(_WIN32)
        timeBeginPeriod(1);          /* 1 ms Sleep() granularity */
#endif
    }
    /* During bink playback, let the frame run at a high, steady rate so the
     * movie presents smoothly (the DLL still decodes at its OWN 30fps via QPC;
     * this just gives many even presents per decoded frame instead of the
     * jittery 2-per-frame a hard 60 cap produces). Otherwise cap at 60 for
     * correct game speed. RECOMP_FPS_CAP overrides. */
    extern int bink_hle_is_active(void);
    int cap = env_cap ? env_cap : (bink_hle_is_active() ? 240 : 60);
    if (cap <= 0) return;

#if defined(_WIN32)
    static LARGE_INTEGER freq = {0};
    static LARGE_INTEGER next = {0};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    /* period in QPC ticks; *1001/1000 gives the true 59.94 Hz for cap=60 */
    long long period = (freq.QuadPart * 1001LL) / ((long long)cap * 1000LL);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (next.QuadPart == 0) next.QuadPart = now.QuadPart;
    next.QuadPart += period;
    if (next.QuadPart <= now.QuadPart) {
        /* Fell >1 frame behind (load spike / stall) — resync without a
         * catch-up burst that would make the game lurch forward.
         *
         * Resync to `now`, NOT `now + period`. The next call begins with
         * `next += period`, so seeding a full period here lands the following
         * deadline TWO periods out: one over-budget frame would make the very
         * next frame sleep an extra refresh even when its own work fit
         * comfortably in budget. That is a 60 -> 30 oscillation, and it
         * averaged out to the ~50-55 fps (with a ~31 fps floor) seen in busy
         * scenes -- while the profiler correctly reported only 13-18 ms of
         * work. It also halved the input poll rate for those frames, because
         * the game polls once per VIWaitForRetrace. */
        next.QuadPart = now.QuadPart;
        return;
    }
    {   /* the wait is idle time — tell the profiler so it does not count it
         * as frame work (otherwise every capped frame looks like a drop). */
        extern volatile int g_prof_in_wait;
        LARGE_INTEGER wait_start = now;
        g_prof_in_wait = 1;             /* sampler: this is idle, not work */
        for (;;) {
            QueryPerformanceCounter(&now);
            long long remain = next.QuadPart - now.QuadPart;
            if (remain <= 0) break;
            long long remain_ms = (remain * 1000LL) / freq.QuadPart;
            if (remain_ms > 2) Sleep((DWORD)(remain_ms - 1));
            /* else: busy-spin the final <2 ms */
        }
        g_prof_in_wait = 0;
        g_frame_sleep_us += (unsigned long long)
            (((now.QuadPart - wait_start.QuadPart) * 1000000LL) / freq.QuadPart);
    }
#else
    static uint64_t next_us = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_us = (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
    uint64_t period_us = 1001000ull / (uint64_t)cap;   /* ~16683us at cap=60 */
    if (next_us == 0) next_us = now_us;
    next_us += period_us;
    /* Resync to `now`, not `now + period` -- see the Windows branch above for
     * why the extra period turns one late frame into two. */
    if (next_us <= now_us) { next_us = now_us; return; }
    {   /* Same bookkeeping the Windows branch does, and for the same reason:
         * this wait is IDLE, and a profiler that counts it as frame work
         * reports every capped frame as a drop and puts the frame-end
         * function at the top of every profile. Missing here, it made the
         * web sampler read "0% headroom" while the game sat at a steady
         * 60 fps, and it inflates the "guest" figure on Android too. */
        extern volatile int g_prof_in_wait;
        extern unsigned long long g_frame_sleep_us;
        uint64_t wait_start_us = now_us;
        /* How much of the tail to busy-spin instead of sleeping.
         *
         * A browser worker's scheduler is coarse: nanosleep routinely
         * overshoots by several ms, and an overshoot that carries past the
         * compositor's next vsync costs a WHOLE extra refresh -- the frame
         * becomes 33 ms and the fps readout halves. That is the entire cause
         * of the web build's periodic dips: measured dropped frames did only
         * 4.6-12.5 ms of work (even at 657 draws, comfortably inside the
         * 16.7 ms budget) yet took ~30 ms, with ~20 ms spent asleep. Spinning
         * the last 5 ms costs a few percent of one worker and makes scheduler
         * jitter unable to eat a refresh. Native targets keep the tight 1 ms
         * tail, where nanosleep is accurate and battery matters more. */
#if defined(__EMSCRIPTEN__)
        const uint64_t spin_us = 5000;
#else
        const uint64_t spin_us = 1000;
#endif
        g_prof_in_wait = 1;
        for (;;) {
            clock_gettime(CLOCK_MONOTONIC, &ts);
            now_us = (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
            if (now_us >= next_us) break;
            uint64_t rem = next_us - now_us;
            if (rem > spin_us + 1000) {
                struct timespec s = {0, (long)((rem - spin_us) * 1000)};
                nanosleep(&s, NULL);
            }
        }
        g_prof_in_wait = 0;
        g_frame_sleep_us += now_us - wait_start_us;
    }
#endif
}

#if defined(_WIN32)
DWORD WINAPI retrace_stall_watchdog(LPVOID arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    fprintf(stderr, "[stall] watchdog armed\n"); fflush(stderr);
    // Heartbeat via WriteFile directly to stderr handle -- avoid stdio
    // locks that the main thread might be holding via fprintf.
    HANDLE herr = GetStdHandle(STD_ERROR_HANDLE);
    #define HB(s) do { DWORD _w; WriteFile(herr, (s), (DWORD)strlen(s), &_w, NULL); } while (0)
    uint32_t last_tick = 0;
    uint64_t first_stall_t = 0;
    uint32_t dumps = 0;
    uint32_t heartbeats = 0;
    extern volatile uint32_t g_flip_count;
    uint32_t last_flip = 0; uint64_t last_flip_ms = 0;
    for (;;) {
        Sleep(250);
        /* --- Liveness tracker: watch the per-frame flip counter --- */
        uint32_t cur_flip = g_flip_count;
        uint64_t now = ms_now();
        if (last_flip_ms == 0) { last_flip = cur_flip; last_flip_ms = now; }
        if (cur_flip != last_flip) {
            /* alive — print an FPS heartbeat once a second */
            if ((heartbeats++ % 4) == 0) {
                uint64_t dt = now - last_flip_ms; if (!dt) dt = 1;
                double fps = (double)(cur_flip - last_flip) * 1000.0 / (double)dt;
                char tmsg[96];
                int n = snprintf(tmsg, sizeof tmsg,
                    "[LIVE] flips=%u (%.1f fps) — game loop alive\n", cur_flip, fps);
                DWORD w; WriteFile(herr, tmsg, (DWORD)n, &w, NULL);
            }
            last_flip = cur_flip; last_flip_ms = now;
        }
        uint32_t cur_tick = g_flip_count;
        if (cur_tick == 0) continue;
        if (cur_tick != last_tick) { last_tick = cur_tick; first_stall_t = ms_now(); continue; }
        uint64_t gap = ms_now() - first_stall_t;
        if (gap < 1000) continue;
        if (dumps >= 3) continue;
        dumps++;
        char msg[256];
        int n = snprintf(msg, sizeof msg,
            "\n[FREEZE] game loop hung %llums (dump #%u) LR=0x%08x CTR=0x%08x SP=0x%08x — where it died:\n",
            (unsigned long long)gap, dumps, g_cpu.lr, g_cpu.ctr, g_cpu.gpr[1]);
        DWORD w; WriteFile(herr, msg, (DWORD)n, &w, NULL);
        ppc_dump_recent_calls();
        ppc_dump_hotspots();
        fflush(stderr);

        // On first stall, dump guest RAM so we can diff against Dolphin's dump.
        //
        // Opt-in: this is 88 MB and only means anything if you are actually
        // diffing against a Dolphin capture. Unconditionally it dropped
        // mem1.bin, mem2.bin and a logs/ directory beside the executable on
        // any stall, which on a shipped build is a large surprise.
        extern int robox_debug_dumps_wanted(void);
        if (dumps == 1 && robox_debug_dumps_wanted()) {
            extern uint8_t *g_mem1;
            extern uint8_t *g_mem2;
            /* Relative to the working directory, like everything else that
             * writes under logs/. This used to be an absolute path into a
             * different project on one machine, so the dump silently went
             * nowhere for anyone else. */
            const char *dir = "logs";
            CreateDirectoryA(dir, NULL);
            char path[MAX_PATH];
            snprintf(path, sizeof path, "%s\\mem1.bin", dir);
            HANDLE hf = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf != INVALID_HANDLE_VALUE) {
                DWORD wb; WriteFile(hf, g_mem1, 0x01800000, &wb, NULL);
                CloseHandle(hf);
                HB("[stall] mem1.bin written\n");
            }
            snprintf(path, sizeof path, "%s\\mem2.bin", dir);
            hf = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf != INVALID_HANDLE_VALUE) {
                DWORD wb; WriteFile(hf, g_mem2, 0x04000000, &wb, NULL);
                CloseHandle(hf);
                HB("[stall] mem2.bin written\n");
            }
        }
    }
}
#else
// Non-Windows: the watchdog is currently Win32-only (uses WriteFile directly
// to skirt stdio locks held by main during a stall). On POSIX, main()
// would need to spawn a pthread that calls a pthread_create-friendly
// equivalent. Leaving it un-implemented for now -- the recompiler still
// runs without the watchdog; you just won't get the stall-dump auto-print.
void *retrace_stall_watchdog(void *arg) {
    (void)arg;
    return NULL;
}
#endif

// Generic input injection goes through hle_KPADRead (further down) -- it
// works regardless of where the game keeps its buffers. Per-game overrides
// can subclass that in quirks/<game>.c.
static void poke_fake_buttons(uint32_t frame) {
    (void)frame;
    // Intentionally empty in the generic build.
}

// Per-frame audio pump. Robox never routes through vi_pump_retraces (that
// was the RGH K3D::Flip path), so without this the AX 5 ms frame callback
// (the game's sound-engine heartbeat) and the AI DMA completion callback
// (the music streamer) never fire: voices got acquired at boot and then the
// whole engine sat silent forever.
static void audio_pump_frame(void) {
    /* AX frame callback: the Wii AX ucode interrupts every 3 ms (96 samples
     * @ 32 kHz). The old fixed 3 fires/frame was ~54% of real time — the
     * MIDI sequencer (which self-clocks on this callback) played everything
     * slow and delivered note-offs late (hanging notes). Accumulate real
     * elapsed time and fire one callback per 3 ms, capped for stalls. */
    {
        extern uint32_t g_ax_frame_cb;
        static uint64_t s_last_ms;
        uint64_t now = ms_now();
        if (!s_last_ms) s_last_ms = now;
        uint32_t fires = (uint32_t)((now - s_last_ms) / 3u);
        if (fires) {
            s_last_ms += (uint64_t)fires * 3u;
            if (fires > 12) fires = 12;    /* stall catch-up cap (~36 ms) */
            for (uint32_t k = 0; k < fires; ++k) {
                if (g_ax_frame_cb) ppc_call_indirect(g_ax_frame_cb);
                /* host MIDI sequencer: same 3 ms heartbeat */
                { extern void robox_midi_pump(double dt_seconds);
                  robox_midi_pump(0.003); }
            }
        }
    }
    /* Mix whatever voices the game's setters put in RUN state. */
    { extern void ax_mixer_frame(void); ax_mixer_frame(); }
    /* AI DMA: consume at 32 kHz stereo s16 (128 kB/s); when the current
     * buffer is drained, fire the completion callback so the game feeds
     * the next one. */
    {
        extern uint32_t g_ai_dma_cb;
        extern int32_t  g_ai_bytes_left;
        if (g_ai_dma_cb && g_ai_bytes_left > 0) {
            g_ai_bytes_left -= 32000 * 4 / 60;
            if (g_ai_bytes_left <= 0)
                ppc_call_indirect(g_ai_dma_cb);
        }
    }
}

void hle_VIWaitForRetrace    (void) {
    /* Robox paces its simulation on VIWaitForRetrace (there is no K3D::Flip
     * here). Without the limiter the game runs as fast as the recompiled CPU
     * goes (~500 fps): physics, animation timers and the input debounce all
     * break. frame_limiter() blocks until the next 59.94 Hz deadline —
     * exactly what the real VI retrace wait does. RECOMP_FPS_CAP overrides. */
    frame_limiter();
    { extern uint32_t g_frame_draw_calls_last;
      robox_prof_frame(g_frame_draw_calls_last); }
    g_vi_tick++;
    g_vi_last_retrace_ms = ms_now();
    vi_retrace_count++;
    robox_prof_scope_begin(ROBOX_PROF_AUDIO);
    audio_pump_frame();
    robox_prof_scope_end(ROBOX_PROF_AUDIO);
    // No-op: fake buttons are now injected through hle_KPADRead which the
    // game's input loop uses. poke_fake_buttons() is dead code -- keeping
    // the function definition for reference only.
    (void)poke_fake_buttons;
    if (vi_retrace_count < 10 || (vi_retrace_count % 500) == 0) {
        fprintf(stderr, "[VI] retrace %u xfb=0x%08x post_cb=0x%08x\n",
                vi_retrace_count, vi_xfb_va, vi_post_retrace_cb);
        fflush(stderr);
    }
    // Dump the ring + hotspots at key frame milestones so we can see
    // exactly which functions the game is exercising each frame.
    if (vi_retrace_count == 100 || vi_retrace_count == 1000 ||
        vi_retrace_count == 10000) {
        fprintf(stderr, "\n[VI] === frame %u snapshot ===\n", vi_retrace_count);
        ppc_dump_recent_calls();
        ppc_dump_hotspots();
        fflush(stderr);
    }
    // At frame 50, dump ALL unique function addresses from the last ring of
    // trace events so we can see the full "one frame" body -- the hot-15 view
    // only shows the tightest inner loop. This lets us identify whether any
    // game-logic (update/draw) code runs or just fps-sync + GX disp-copy.
    if (vi_retrace_count == 50) {
        extern void ppc_dump_unique_recent(uint32_t span);
        fprintf(stderr, "\n[VI] === frame 50 unique funcs in last 2000 calls ===\n");
        ppc_dump_unique_recent(2000);
    }
    // Fire registered pre/post retrace callbacks. Real HW calls these
    // from a VI interrupt; we fake it at the sync point. Callbacks take
    // (u32 retraceCount) -- pass it in r3.
    if (vi_pre_retrace_cb) {
        g_cpu.gpr[3] = vi_retrace_count;
        ppc_call_indirect(vi_pre_retrace_cb);
    }
    if (vi_post_retrace_cb) {
        g_cpu.gpr[3] = vi_retrace_count;
        ppc_call_indirect(vi_post_retrace_cb);
    }
    // Fire any queued DVD/async completion callbacks before the game
    // proceeds to its next frame. On real hardware these run from an
    // interrupt; we fake it at the next stable sync point.
    extern void dvd_pump_callbacks_hook(void);
    dvd_pump_callbacks_hook();
    // Decode and present exactly what the game put in the XFB. No overlays,
    // no fake fallbacks. If the game hasn't set an XFB yet we keep whatever
    // was last in the framebuffer (black after video_init).
    if (vi_xfb_va) video_copy_efb_to_window(vi_xfb_va);
    video_present();
    HLE_RET(0);
}
void hle_VIGetRetraceCount   (void) {
    // Real Wii VI retrace counter ticks on hardware vblank, NOT on read.
    // Returning current value without incrementing matches that contract;
    // the counter only advances inside hle_VIWaitForRetrace.
    HLE_RET(vi_retrace_count);
}
void hle_VIGetCurrentLine    (void) { HLE_RET(0); }
void hle_VIGetTvFormat       (void) { HLE_RET(0); }   // 0 = NTSC
void hle_VISetCopyFilter     (void) { HLE_RET(0); }
// The real VISetPost/PreRetraceCallback register a guest function to call
// on each vblank. We remember the current one so VIWaitForRetrace can
// actually invoke it -- otherwise games that run frame logic from the
// callback (not the main loop) just sit there.
// Forward-declared earlier in the file so hle_VIWaitForRetrace can fire them.
extern uint32_t vi_post_retrace_cb;
extern uint32_t vi_pre_retrace_cb;

void hle_VISetPostRetraceCallback(void) {
    uint32_t prev = vi_post_retrace_cb;
    vi_post_retrace_cb = HLE_ARG_U32(0);
    fprintf(stderr, "[VI] SetPostRetraceCallback -> 0x%08x (was 0x%08x)\n",
            vi_post_retrace_cb, prev);
    fflush(stderr);
    HLE_RET(prev);
}
void hle_VISetPreRetraceCallback(void) {
    uint32_t prev = vi_pre_retrace_cb;
    vi_pre_retrace_cb = HLE_ARG_U32(0);
    fprintf(stderr, "[VI] SetPreRetraceCallback -> 0x%08x (was 0x%08x)\n",
            vi_pre_retrace_cb, prev);
    fflush(stderr);
    HLE_RET(prev);
}


// ===========================================================================
// GX -- Graphics (Hollywood TEV pipeline). All stubs for now -- replacing
//       with OpenGL/Vulkan/D3D12 is a MAJOR project. The stubs let the
//       init chain complete; nothing renders.
// ===========================================================================

void hle_GXInit(void) {
    /* GXInit is HLE'd, so the SDK's __GXData deferred-state block never gets
     * its shadow words initialized. Those words carry the BP REGISTER NUMBER
     * in the top byte; the recompiled GXSet* bodies read-modify-write the
     * VALUE bits and emit the whole word to the write-gather pipe. Unseeded
     * (top byte 0), every deferred emission -- blend, zmode, TREF, TEV --
     * landed on BP reg 0x00: Robox's menu drew its 15%-alpha overlay layers
     * with blending stuck OFF, overwriting the scene ("zoomed-in blurry
     * starfield blobs over black").
     *
     * ROBOX: __GXData is a STATIC SDK block (0x8025c700) whose pointer lives
     * in SDA2 at r2-0x64b0 (initialized data, valid before GXInit runs).
     * Seed THAT block. (The previous RGH-specific version built a fake block
     * at 0x817d0000 and poked its pointer into 0x806c5540 -- an address
     * inside Robox's ExpHeap. Both writes removed.)
     * Only the register byte is seeded (value bits preserved) except where a
     * hardware default matters (zmode, KSEL swap tables). */
    uint32_t gxd = MEM_R32(g_cpu.gpr[2] - 0x64b0u);
    if (gxd < 0x80003000u || gxd >= 0x81800000u) {
        fprintf(stderr, "[GX] GXInit: bad __GXData ptr 0x%08x (SDA2 r2-0x64b0) -- not seeding\n", gxd);
        fflush(stderr);
        HLE_RET(0);
        return;
    }
    #define GXD_SEED(off, regbyte_val) \
        MEM_W32(gxd + (off), ((uint32_t)(regbyte_val) << 24) | (MEM_R32(gxd + (off)) & 0x00FFFFFFu))
    for (uint32_t i = 0; i < 8; i++)                /* TREF 0x28..0x2F */
        GXD_SEED(0x150u + i*4u, 0x28 + i);
    for (uint32_t s = 0; s < 16; s++) {             /* TEV color/alpha 0xC0.. */
        GXD_SEED(0x180u + s*4u, 0xC0 + 2*s);
        GXD_SEED(0x1c0u + s*4u, 0xC1 + 2*s);
    }
    GXD_SEED(0x07Cu, 0x22);                         /* lpsize          */
    GXD_SEED(0x148u, 0x20);                         /* scissor TL      */
    GXD_SEED(0x14Cu, 0x21);                         /* scissor BR      */
    GXD_SEED(0x170u, 0x27);                         /* iref            */
    GXD_SEED(0x174u, 0x0F);                         /* bpMask          */
    GXD_SEED(0x178u, 0x25);                         /* indTexScale0    */
    GXD_SEED(0x17Cu, 0x26);                         /* indTexScale1    */
    /* KSEL 0xF6..0xFD: konst selectors + TEV swap tables (SDK defaults). */
    {
        static const uint8_t ksel_swap[8] = { 0x4, 0xE, 0x0, 0xC, 0x5, 0xD, 0xA, 0xE };
        for (uint32_t i = 0; i < 8; i++)
            MEM_W32(gxd + 0x200u + i*4u, ((uint32_t)(0xF6 + i) << 24) | ksel_swap[i]);
    }
    GXD_SEED(0x220u, 0x41);                         /* cmode0 (blend)  */
    GXD_SEED(0x224u, 0x42);                         /* cmode1 (dstA)   */
    MEM_W32(gxd + 0x228u, 0x40000017u);             /* zmode LEQUAL+wr */
    GXD_SEED(0x22Cu, 0x43);                         /* peCtrl          */
    #undef GXD_SEED
    fprintf(stderr, "[GX] seeded __GXData shadow reg bytes at 0x%08x\n", gxd);
    fflush(stderr);
    HLE_RET(gxd);
}
void hle_GXInitFifoBase         (void) { HLE_RET(0); }  // FIFO intercepted via g_gx_fifo, not real HW

// GXGetTexBufferSize(w, h, fmt, mipmap, maxLod) -> byte size of a texture in
// the given format. The recompiled version uses a bctrl-driven jump table
// (format switch) whose targets are mid-function VAs; the recompiler treats
// those as indirect calls to "unknown" and the function returns nonsense,
// which breaks every subsequent texture allocation.
//
// HLE: compute a safe over-approximation. RGBA8 (fmt 6 / 0x16) is 4 bytes
// per texel; most other tiled formats are <= 4 bytes. We round width/height
// to 4-pixel tiles (the GX texture alignment). If mipmap is set we scale
// by 4/3 (geometric series of 1 + 1/4 + 1/16 + ...). Overshoots by at most
// ~2x for small textures, which is fine with the bump allocator.
void hle_GXGetTexBufferSize(void) {
    uint32_t w      = HLE_ARG_U32(0);
    uint32_t h      = HLE_ARG_U32(1);
    uint32_t fmt    = HLE_ARG_U32(2);
    uint32_t mipmap = HLE_ARG_U32(3);
    // Round up to 4-pixel tiles; zero in either dim returns zero.
    if (w == 0 || h == 0) { HLE_RET(0); return; }
    w = (w + 3u) & ~3u;
    h = (h + 3u) & ~3u;
    // Most formats are <= 4 bytes/pixel. RGBA8 is 4. C8 is 1 but game's
    // palette math assumes 2 anyway. Over-allocating is fine; under isn't.
    uint32_t bpp = (fmt == 6 || fmt == 0x16) ? 4 : 4;   // be generous
    uint32_t base = w * h * bpp;
    if (mipmap) base = (base * 4u) / 3u;
    HLE_RET(base);
}
void hle_GXSetCPUFifo           (void) { HLE_RET(0); }  // FIFO wiring handled by gx_ogl
void hle_GXSetGPFifo            (void) { HLE_RET(0); }
void hle_GXFlush                (void) { HLE_RET(0); }   // hot, stay silent
void hle_GXDrawDone             (void) { HLE_RET(0); }
void hle_GXAbortFrame           (void) { HLE_RET(0); }
void hle_GXSetViewport          (void) { HLE_RET(0); }
void hle_GXSetScissor           (void) { HLE_RET(0); }
// GX_SetProjection(f32 *mtx, u32 type)
//   mtx:  pointer to 6 floats (compact projection form, A..F) OR 4x4 matrix.
//   type: 0=perspective, 1=orthographic
// Nintendo/libogc converts a 4x4 matrix to the 6-float compact form before
// uploading to XF. But many games pass the compact form directly and it's
// stored at mtx[0..5]. We grab it and expand to our 4x4.
void hle_GXSetProjection(void) {
    uint32_t mtx_va = HLE_ARG_U32(0);
    uint32_t type   = HLE_ARG_U32(1);
    uint8_t *p = (uint8_t*)ppc_host_ptr(mtx_va);
    if (p) {
        // Nintendo SDK passes a full 4x4 matrix (16 floats, row-major).
        // Extract compact form per libogc GX_SetProjection:
        //   ortho : [m00, m03, m11, m13, m22, m23]
        //   persp : [m00, m02, m11, m12, m22, m23]
        float m[16];
        for (int i = 0; i < 16; ++i) m[i] = read_be_float(p + i * 4);
        float A, B, C, D, E, F;
        if (type & 1) {   // ortho
            A = m[0];  B = m[3];   // row 0: scale, translate (x)
            C = m[5];  D = m[7];   // row 1: scale, translate (y)
            E = m[10]; F = m[11];  // row 2: scale, translate (z)
        } else {          // perspective
            A = m[0];  B = m[2];
            C = m[5];  D = m[6];
            E = m[10]; F = m[11];
        }
        xf_proj_compact[0]=A; xf_proj_compact[1]=B; xf_proj_compact[2]=C;
        xf_proj_compact[3]=D; xf_proj_compact[4]=E; xf_proj_compact[5]=F;
        xf_proj_compact[6]= (float)(type & 1);
        xf_proj_is_ortho = (int)(type & 1);
        xf_proj_loaded = 1;
        if (xf_proj_is_ortho) {
            xf_proj[ 0]=A;  xf_proj[ 1]=0;  xf_proj[ 2]=0;  xf_proj[ 3]=B;
            xf_proj[ 4]=0;  xf_proj[ 5]=C;  xf_proj[ 6]=0;  xf_proj[ 7]=D;
            xf_proj[ 8]=0;  xf_proj[ 9]=0;  xf_proj[10]=E;  xf_proj[11]=F;
            xf_proj[12]=0;  xf_proj[13]=0;  xf_proj[14]=0;  xf_proj[15]=1;
        } else {
            xf_proj[ 0]=A;  xf_proj[ 1]=0;  xf_proj[ 2]=B;  xf_proj[ 3]=0;
            xf_proj[ 4]=0;  xf_proj[ 5]=C;  xf_proj[ 6]=D;  xf_proj[ 7]=0;
            xf_proj[ 8]=0;  xf_proj[ 9]=0;  xf_proj[10]=E;  xf_proj[11]=F;
            xf_proj[12]=0;  xf_proj[13]=0;  xf_proj[14]=-1; xf_proj[15]=0;
        }
        static int log_n;
        if (log_n < 8) {
            fprintf(stderr, "[GX] SetProjection %s A=%.3f B=%.3f C=%.3f D=%.3f E=%.3f F=%.3f\n",
                    xf_proj_is_ortho ? "ortho" : "persp", A,B,C,D,E,F);
            fflush(stderr);
            log_n++;
        }
        /* NaN HUNT: if the game handed us a NaN projection matrix, log WHO
         * (caller LR), WHERE the matrix is (mtx_va), and the raw source words
         * so we can trace back which routine computed the NaN. */
        if (A != A || C != C || E != E) {
            uint32_t b0, b5, b10;
            memcpy(&b0,&m[0],4); memcpy(&b5,&m[5],4); memcpy(&b10,&m[10],4);
            static int nn; if (nn++ < 24) {
                fprintf(stderr, "[PROJ-NAN] SetProjection NaN! caller_lr=0x%08x mtx_va=0x%08x m0=0x%08x m5=0x%08x m10=0x%08x\n",
                        g_cpu.lr, mtx_va, b0, b5, b10);
                fflush(stderr);
            }
        }
    }
    HLE_RET(0);
}

// GX_LoadPosMtxImm(f32 mt[3][4], u32 id)
// Writes a 3x4 matrix (12 floats) into position-matrix memory at slot id*12.
void hle_GXLoadPosMtxImm(void) {
    uint32_t mtx_va = HLE_ARG_U32(0);
    uint32_t id     = HLE_ARG_U32(1);
    uint8_t *p = (uint8_t*)ppc_host_ptr(mtx_va);
    if (p && id < 10) {
        for (int i = 0; i < 12; ++i) {
            xf_pos_mtx[id][i] = read_be_float(p + i * 4);
        }
        static int log_n;
        if (log_n < 4) {
            fprintf(stderr, "[GX] LoadPosMtxImm id=%u m=(%.3f,%.3f,%.3f | %.3f,%.3f,%.3f | %.3f,%.3f,%.3f)\n",
                    id,
                    xf_pos_mtx[id][0], xf_pos_mtx[id][1], xf_pos_mtx[id][2],
                    xf_pos_mtx[id][4], xf_pos_mtx[id][5], xf_pos_mtx[id][6],
                    xf_pos_mtx[id][8], xf_pos_mtx[id][9], xf_pos_mtx[id][10]);
            fflush(stderr);
            log_n++;
        }
    }
    HLE_RET(0);
}

void hle_GXLoadNrmMtxImm        (void) { HLE_RET(0); }
void hle_GXSetCurrentMtx(void) {
    uint32_t id = HLE_ARG_U32(0);
    xf_pos_mtx_idx = (int)(id & 0xF);
    if (xf_pos_mtx_idx >= 10) xf_pos_mtx_idx = 0;
    static int log_n;
    if (log_n < 8) {
        fprintf(stderr, "[GX] SetCurrentMtx id=%u -> slot=%d\n", id, xf_pos_mtx_idx);
        fflush(stderr);
        log_n++;
    }
    HLE_RET(0);
}
// ---------------------------------------------------------------------------
// GXSetVtxDesc / GXSetVtxAttrFmt / GXClearVtxDesc -- write the same CP state
// the hardware FIFO path would normally populate. Without this, the FIFO
// decoder can't compute real vertex sizes and desyncs on the first draw.
//
// Attribute IDs (from Nintendo SDK / libogc):
//   0  PNMTXIDX       -> desc_lo bit 0
//   1..8 TEX[0..7]MTXIDX -> desc_lo bits 1..8
//   9  POS            -> desc_lo bits 9..10
//   10 NRM            -> desc_lo bits 11..12
//   11 CLR0           -> desc_lo bits 13..14
//   12 CLR1           -> desc_lo bits 15..16
//   13..20 TEX[0..7]  -> desc_hi bits (a-13)*2 .. (a-13)*2+1
// ---------------------------------------------------------------------------

void hle_GXSetVtxDesc(void) {
    uint32_t attr = HLE_ARG_U32(0);
    uint32_t type = HLE_ARG_U32(1) & 3;
    if (attr <= 8) {
        cp_vtx_desc_lo = (cp_vtx_desc_lo & ~(1u << attr)) | ((type & 1u) << attr);
    } else if (attr == 9) {
        cp_vtx_desc_lo = (cp_vtx_desc_lo & ~(3u <<  9)) | (type <<  9);
    } else if (attr == 10) {
        cp_vtx_desc_lo = (cp_vtx_desc_lo & ~(3u << 11)) | (type << 11);
    } else if (attr == 11) {
        cp_vtx_desc_lo = (cp_vtx_desc_lo & ~(3u << 13)) | (type << 13);
    } else if (attr == 12) {
        cp_vtx_desc_lo = (cp_vtx_desc_lo & ~(3u << 15)) | (type << 15);
    } else if (attr >= 13 && attr <= 20) {
        int bit = (int)(attr - 13) * 2;
        cp_vtx_desc_hi = (cp_vtx_desc_hi & ~(3u << bit)) | (type << bit);
    }
    static int log_n;
    if (log_n < 32) {
        fprintf(stderr, "[GX] SetVtxDesc attr=%u type=%u -> desc=%08x:%08x\n",
                attr, type, cp_vtx_desc_hi, cp_vtx_desc_lo);
        fflush(stderr);
        log_n++;
    }
    HLE_RET(0);
}

void hle_GXClearVtxDesc         (void) {
    cp_vtx_desc_lo = 0;
    cp_vtx_desc_hi = 0;
    HLE_RET(0);
}

// GXSetVtxDescv(GXVtxDescList *list)
// list is terminated by attr == 0xFF or some sentinel; each entry is
// {u32 attr, u32 type}. We walk the list and pretend each entry came
// through GXSetVtxDesc. The game's internal struct is updated by the
// real game code (since this function isn't HLE'd in the name map,
// the real game version also runs and populates its bookkeeping).
void hle_GXSetVtxDescv(void) {
    uint32_t list_va = HLE_ARG_U32(0);
    for (int i = 0; i < 64; ++i) {
        uint32_t attr_va = list_va + i * 8;
        uint32_t attr, type;
        uint8_t *ap = (uint8_t*)ppc_host_ptr(attr_va);
        uint8_t *tp = (uint8_t*)ppc_host_ptr(attr_va + 4);
        if (!ap || !tp) break;
        attr = ((uint32_t)ap[0] << 24) | ((uint32_t)ap[1] << 16)
             | ((uint32_t)ap[2] <<  8) |  (uint32_t)ap[3];
        type = ((uint32_t)tp[0] << 24) | ((uint32_t)tp[1] << 16)
             | ((uint32_t)tp[2] <<  8) |  (uint32_t)tp[3];
        if (attr == 0xFFu || attr > 30) break;
        type &= 3u;
        if (attr <= 8) {
            cp_vtx_desc_lo = (cp_vtx_desc_lo & ~(1u << attr)) | ((type & 1u) << attr);
        } else if (attr == 9) {
            cp_vtx_desc_lo = (cp_vtx_desc_lo & ~(3u <<  9)) | (type <<  9);
        } else if (attr == 10) {
            cp_vtx_desc_lo = (cp_vtx_desc_lo & ~(3u << 11)) | (type << 11);
        } else if (attr == 11) {
            cp_vtx_desc_lo = (cp_vtx_desc_lo & ~(3u << 13)) | (type << 13);
        } else if (attr == 12) {
            cp_vtx_desc_lo = (cp_vtx_desc_lo & ~(3u << 15)) | (type << 15);
        } else if (attr >= 13 && attr <= 20) {
            int bit = (int)(attr - 13) * 2;
            cp_vtx_desc_hi = (cp_vtx_desc_hi & ~(3u << bit)) | (type << bit);
        }
    }
    static int log_n;
    if (log_n < 24) {
        fprintf(stderr, "[GX] SetVtxDescv list=0x%08x -> desc=%08x:%08x vsize[0]=%u\n",
                list_va, cp_vtx_desc_hi, cp_vtx_desc_lo, gx_vertex_size(0));
        fflush(stderr);
        log_n++;
    }
    HLE_RET(0);
}

// GXSetVtxAttrFmtv(GXVtxFmt vtxfmt, GXVtxAttrFmtList *list)
// Each list entry: {u32 attr, u32 cnt, u32 type, u32 frac} terminated by attr==0xFF
void hle_GXSetVtxAttrFmtv(void) {
    uint32_t vtxfmt = HLE_ARG_U32(0) & 7;
    uint32_t list_va = HLE_ARG_U32(1);
    for (int i = 0; i < 32; ++i) {
        uint32_t entry_va = list_va + i * 16;
        uint32_t vals[4];
        int ok = 1;
        for (int j = 0; j < 4; ++j) {
            uint8_t *p = (uint8_t*)ppc_host_ptr(entry_va + j * 4);
            if (!p) { ok = 0; break; }
            vals[j] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                    | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
        }
        if (!ok) break;
        uint32_t attr  = vals[0];
        uint32_t count = vals[1] & 1;
        uint32_t type  = vals[2] & 7;
        uint32_t frac  = vals[3] & 0x1F;
        if (attr == 0xFFu || attr > 30) break;
        uint32_t *g0 = &cp_vat_g0[vtxfmt];
        uint32_t *g1 = &cp_vat_g1[vtxfmt];
        uint32_t *g2 = &cp_vat_g2[vtxfmt];
        if (attr == 9) {
            *g0 = (*g0 & ~0x000001FFu) | (count << 0) | (type << 1) | (frac << 4);
        } else if (attr == 10) {
            *g0 = (*g0 & ~0x00001E00u) | (count <<  9) | (type << 10);
        } else if (attr == 11) {
            *g0 = (*g0 & ~0x0001E000u) | (count << 13) | (type << 14);
        } else if (attr == 12) {
            *g0 = (*g0 & ~0x001E0000u) | (count << 17) | (type << 18);
        } else if (attr == 13) {
            *g0 = (*g0 & ~0x3FE00000u) | (count << 21) | (type << 22) | (frac << 25);
        } else if (attr == 14) {
            *g1 = (*g1 & ~0x000001FFu) | (count << 0) | (type << 1) | (frac << 4);
        } else if (attr == 15) {
            *g1 = (*g1 & ~0x0003FE00u) | (count <<  9) | (type << 10) | (frac << 13);
        } else if (attr == 16) {
            *g1 = (*g1 & ~0x07FC0000u) | (count << 18) | (type << 19) | (frac << 22);
        } else if (attr == 17) {
            *g1 = (*g1 & ~0x78000000u) | (count << 27) | (type << 28);
            *g2 = (*g2 & ~0x0000001Fu) | frac;
        } else if (attr == 18) {
            *g2 = (*g2 & ~0x00003FE0u) | (count <<  5) | (type <<  6) | (frac <<  9);
        } else if (attr == 19) {
            *g2 = (*g2 & ~0x007FC000u) | (count << 14) | (type << 15) | (frac << 18);
        } else if (attr == 20) {
            *g2 = (*g2 & ~0xFF800000u) | (count << 23) | (type << 24) | (frac << 27);
        }
    }
    static int log_n;
    if (log_n < 16) {
        fprintf(stderr, "[GX] SetVtxAttrFmtv vf=%u list=0x%08x -> vsize[%u]=%u\n",
                vtxfmt, list_va, vtxfmt, gx_vertex_size(vtxfmt));
        fflush(stderr);
        log_n++;
    }
    HLE_RET(0);
}

// GXSetVtxAttrFmt(vtxfmt, attr, count, type, frac)
void hle_GXSetVtxAttrFmt(void) {
    uint32_t vtxfmt = HLE_ARG_U32(0) & 7;
    uint32_t attr   = HLE_ARG_U32(1);
    uint32_t count  = HLE_ARG_U32(2) & 1;
    uint32_t type   = HLE_ARG_U32(3) & 7;
    uint32_t frac   = HLE_ARG_U32(4) & 0x1F;
    uint32_t *g0 = &cp_vat_g0[vtxfmt];
    uint32_t *g1 = &cp_vat_g1[vtxfmt];
    uint32_t *g2 = &cp_vat_g2[vtxfmt];
    if (attr == 9) {         // POS: bits 0(elems), 1..3(fmt), 4..8(frac)
        *g0 = (*g0 & ~0x000001FFu) | (count << 0) | (type << 1) | (frac << 4);
    } else if (attr == 10) { // NRM: bits 9(elems), 10..12(fmt)
        *g0 = (*g0 & ~0x00001E00u) | (count <<  9) | (type << 10);
    } else if (attr == 11) { // CLR0: bits 13(elems), 14..16(fmt)
        *g0 = (*g0 & ~0x0001E000u) | (count << 13) | (type << 14);
    } else if (attr == 12) { // CLR1: bits 17(elems), 18..20(fmt)
        *g0 = (*g0 & ~0x001E0000u) | (count << 17) | (type << 18);
    } else if (attr == 13) { // TEX0 in g0: bits 21, 22..24, 25..29
        *g0 = (*g0 & ~0x3FE00000u) | (count << 21) | (type << 22) | (frac << 25);
    } else if (attr == 14) { // TEX1 in g1: bits 0, 1..3, 4..8
        *g1 = (*g1 & ~0x000001FFu) | (count << 0) | (type << 1) | (frac << 4);
    } else if (attr == 15) { // TEX2 in g1: bits 9, 10..12, 13..17
        *g1 = (*g1 & ~0x0003FE00u) | (count <<  9) | (type << 10) | (frac << 13);
    } else if (attr == 16) { // TEX3 in g1: bits 18, 19..21, 22..26
        *g1 = (*g1 & ~0x07FC0000u) | (count << 18) | (type << 19) | (frac << 22);
    } else if (attr == 17) { // TEX4 in g1: bits 27, 28..30  (frac in g2 bits 0..4)
        *g1 = (*g1 & ~0x78000000u) | (count << 27) | (type << 28);
        *g2 = (*g2 & ~0x0000001Fu) | frac;
    } else if (attr == 18) { // TEX5 in g2: bits 5, 6..8, 9..13
        *g2 = (*g2 & ~0x00003FE0u) | (count <<  5) | (type <<  6) | (frac <<  9);
    } else if (attr == 19) { // TEX6 in g2: bits 14, 15..17, 18..22
        *g2 = (*g2 & ~0x007FC000u) | (count << 14) | (type << 15) | (frac << 18);
    } else if (attr == 20) { // TEX7 in g2: bits 23, 24..26, 27..31
        *g2 = (*g2 & ~0xFF800000u) | (count << 23) | (type << 24) | (frac << 27);
    }
    static int log_n;
    if (log_n < 24) {
        fprintf(stderr, "[GX] SetVtxAttrFmt vf=%u attr=%u cnt=%u type=%u frac=%u vsize[%u]=%u\n",
                vtxfmt, attr, count, type, frac, vtxfmt, gx_vertex_size(vtxfmt));
        fflush(stderr);
        log_n++;
    }
    HLE_RET(0);
}

// GXSetArray(attr, base_ptr, stride)
void hle_GXSetArray(void) {
    uint32_t attr   = HLE_ARG_U32(0);
    uint32_t base   = HLE_ARG_U32(1);
    uint32_t stride = HLE_ARG_U32(2);
    // Attribute ID maps onto CP array index:
    //   9  POS       -> 0
    //   10 NRM       -> 1
    //   11 CLR0      -> 2
    //   12 CLR1      -> 3
    //   13..20 TEX0..7 -> 4..11
    int idx = -1;
    if (attr == 9) idx = 0;
    else if (attr == 10) idx = 1;
    else if (attr == 11) idx = 2;
    else if (attr == 12) idx = 3;
    else if (attr >= 13 && attr <= 20) idx = 4 + (int)(attr - 13);
    if (idx >= 0 && idx < 16) {
        /* normalize physical -> virtual, same as the CP-register path */
        cp_array_base[idx]   = (base < 0x80000000u) ? base + 0x80000000u : base;
        cp_array_stride[idx] = stride;
    }
    static int log_n;
    if (log_n < 12) {
        fprintf(stderr, "[GX] SetArray attr=%u base=%08x stride=%u\n", attr, base, stride);
        fflush(stderr);
        log_n++;
    }
    HLE_RET(0);
}
void hle_GXSetNumChans          (void) { HLE_RET(0); }
void hle_GXSetNumTexGens        (void) { HLE_RET(0); }
void hle_GXSetNumTevStages      (void) { HLE_RET(0); }
void hle_GXSetTevOp             (void) { HLE_RET(0); }
void hle_GXSetTevOrder          (void) { HLE_RET(0); }
void hle_GXSetChanCtrl          (void) { HLE_RET(0); }
// GXSetBlendMode(type, src_factor, dst_factor, logic_op) -> CMODE0 (BP 0x41).
// HARDWARE bit layout: enable[0] logic_en[1] dither[2] col_upd[3]
// alpha_upd[4] dst[7:5] src[10:8] subtract[11] logic_op[15:12]. Bits 2-4
// belong to GXSetDither/ColorUpdate/AlphaUpdate (still nops) -- preserved
// from the current shadow. (Unused by Robox -- its GXSetBlendMode runs
// recompiled -- but kept hardware-accurate for any port that binds it.)
void hle_GXSetBlendMode(void) {
    uint32_t type = HLE_ARG_U32(0);
    uint32_t sf   = HLE_ARG_U32(1) & 7u;
    uint32_t df   = HLE_ARG_U32(2) & 7u;
    uint32_t op   = HLE_ARG_U32(3) & 0xFu;
    extern uint32_t gx_ogl_get_bp(uint32_t reg);
    uint32_t v = gx_ogl_get_bp(0x41) & 0x001Cu;
    if (type == 1u || type == 3u) v |= 1u;        /* GX_BM_BLEND / SUBTRACT */
    if (type == 2u)               v |= 2u;        /* GX_BM_LOGIC */
    if (type == 3u)               v |= 1u << 11;  /* GX_BM_SUBTRACT */
    v |= (df << 5) | (sf << 8) | (op << 12);
    bp_write(0x41, v);
    HLE_RET(0);
}
// GXSetAlphaCompare(comp0, ref0, op, comp1, ref1) -> BP 0xF3.
void hle_GXSetAlphaCompare(void) {
    uint32_t c0 = HLE_ARG_U32(0) & 7u,  r0 = HLE_ARG_U32(1) & 0xFFu;
    uint32_t op = HLE_ARG_U32(2) & 3u;
    uint32_t c1 = HLE_ARG_U32(3) & 7u,  r1 = HLE_ARG_U32(4) & 0xFFu;
    bp_write(0xF3, r0 | (r1 << 8) | (c0 << 16) | (c1 << 19) | (op << 22));
    HLE_RET(0);
}
// GXSetZMode(compare_enable, func, update_enable) -> BP 0x40.
void hle_GXSetZMode(void) {
    uint32_t en = HLE_ARG_U32(0) & 1u, fn = HLE_ARG_U32(1) & 7u,
             up = HLE_ARG_U32(2) & 1u;
    bp_write(0x40, en | (fn << 1) | (up << 4));
    HLE_RET(0);
}
void hle_GXSetZCompLoc          (void) { HLE_RET(0); }
void hle_GXSetCullMode          (void) { HLE_RET(0); }
void hle_GXSetColorUpdate       (void) { HLE_RET(0); }
void hle_GXSetAlphaUpdate       (void) { HLE_RET(0); }
// GXSetCopyClear(GXColor color, u32 zval). On PPC EABI, GXColor is passed
// as a 32-bit value in r3 packed as 0xRRGGBBAA. Extract the RGB and stash
// it so the next EFB copy fills the XFB with this color.
void hle_GXSetCopyClear         (void) {
    uint32_t color = HLE_ARG_U32(0);
    gx_clear_r = (uint8_t)((color >> 24) & 0xFF);
    gx_clear_g = (uint8_t)((color >> 16) & 0xFF);
    gx_clear_b = (uint8_t)((color >>  8) & 0xFF);
    /* DIAG (fade hunt): a copy-clear fade shows up as this RGBA ramping to
     * black. Log distinct values only. Remove once understood. */
    { static uint32_t s_last = 0xdeadbeef;
      if (color != s_last) { s_last = color;
          fprintf(stderr, "[FADE?] GXSetCopyClear = 0x%08x (RGBA)\n", color);
          fflush(stderr); } }
    HLE_RET(0);
}

// GXCopyDisp(dst_xfb_va, clear_flag). On real hw this writes BP[0x4B] +
// a few others then BP[0x52]. Our approximation: record the dst and run
// the same clear-fill path the BP[0x52] handler uses.
void hle_GXCopyDisp             (void) {
    uint32_t dst = HLE_ARG_U32(0);
    if (dst) bp_dst_addr = dst & 0x00FFFFFFu;
    gx_execute_copy((1u << 14) | ((HLE_ARG_U32(1) & 1u) << 11));  /* display copy (+clear flag) */
    HLE_RET(0);
}
// GXCopyTex(void *dest, GXBool clear): EFB -> texture copy. The source rect
// is already latched from the recompiled GXSetTexCopySrc's BP writes
// (bp_copy_src / bp_copy_size). This was a stub — which black-screened the
// entire in-game world: LyN's POST-PROCESS pass renders the scene, copies
// the EFB to a 640x456 RGBA8 texture, then redraws fullscreen through its
// grading/fade TEV. With the copy dropped, that quad sampled a never-filled
// guest buffer and painted the frame solid black (layer-dump proof:
// layer_3083 = full world, draw 3084 = post quad, layer_3084 = black).
void hle_GXCopyTex(void) {
    uint32_t dst   = HLE_ARG_U32(0);       /* guest VA of the copy target */
    uint32_t clear = HLE_ARG_U32(1) & 1u;
    /* The copy parameters are NOT in our BP shadow: the recompiled
     * GXSetTexCopySrc/Dst store their composed BP words into the __GXData
     * struct ([r2-0x66e0]) at +0x240 (BP 0x49: src x|y<<10), +0x244
     * (BP 0x4A: (w-1)|(h-1)<<10) and +0x24c (the 0x52 copy-execute word:
     * dst format, half-scale bit 9, ...). The real GXCopyTex flushes those
     * to the FIFO; since we replace it, read them from the shadow directly.
     * (First version ignored them -> rect=(0,0 1x1): a one-pixel copy.) */
    uint32_t gxd = MEM_R32(g_cpu.gpr[2] - 0x66e0);
    uint32_t cmd = 0;
    if (gxd >= 0x80000000u) {
        bp_copy_src  = MEM_R32(gxd + 0x240) & 0x00FFFFFFu;
        bp_copy_size = MEM_R32(gxd + 0x244) & 0x00FFFFFFu;
        cmd          = MEM_R32(gxd + 0x24c) & 0x00FFFFFFu;
    }
    if (dst) bp_dst_addr = dst & 0x7FFFFFFFu;   /* store phys-style, like BP 0x4B */
    cmd = (cmd & ~(1u << 11)) | (clear << 11);  /* clear flag from the call arg */
    cmd &= ~(1u << 14);                         /* texture copy, never XFB */
    gx_execute_copy(cmd);
    HLE_RET(0);
}
void hle_GXSetCopyFilter        (void) { HLE_RET(0); }
void hle_GXSetDispCopyGamma     (void) { HLE_RET(0); }
void hle_GXSetDispCopySrc       (void) { HLE_RET(0); }
void hle_GXSetDispCopyDst       (void) { HLE_RET(0); }
void hle_GXSetDispCopyYScale    (void) { HLE_RET(1.0f); HLE_RET_F64(1.0); }  // returns float

// ---------------------------------------------------------------------------
// GX texture state.
//
// GXTexObj is a 32-byte opaque struct in guest memory. Standard libogc
// layout (applied here from the Nintendo SDK + Dolphin texture loader):
//   offset 0x00: BPMEM_TX_SETMODE0 (filter / LOD / wrap)
//   offset 0x04: BPMEM_TX_SETMODE1 (LOD range)
//   offset 0x08: BPMEM_TX_SETIMAGE0 (width-1 | height-1<<10 | fmt<<20)
//   offset 0x0C: BPMEM_TX_SETIMAGE3 (texture data VA / 32)
//   offset 0x10: TLUT offset (for CI4/CI8/CI14X2)
//   offset 0x14..0x1F: additional state (unused in our sampler)
//
// We don't push any of these into the FIFO; instead, `GXLoadTexObj`
// records the bound texture's metadata into the CPU-side `g_tex[N]`
// slot so the rasterizer can sample from it when a draw lands.
// ---------------------------------------------------------------------------

// (GxTexObj / g_tex are declared near the top of this file so the software
// rasterizer can sample from them.)

// Texture formats (GX_TF_*):
//   0x00 I4    0x01 I8     0x02 IA4    0x03 IA8
//   0x04 RGB565  0x05 RGB5A3  0x06 RGBA8  0x08 C4    0x09 C8
//   0x0A C14X2   0x0E CMPR (S3TC / DXT1)

// GXInitTexObj(GXTexObj *obj, void *image_ptr, u16 width, u16 height,
//              u8 format, u8 wrap_s, u8 wrap_t, u8 mipmap)
void hle_GXInitTexObj(void) {
    uint32_t obj_va  = HLE_ARG_U32(0);
    uint32_t img_va  = HLE_ARG_U32(1);
    uint32_t width   = HLE_ARG_U32(2) & 0xFFFF;
    uint32_t height  = HLE_ARG_U32(3) & 0xFFFF;
    uint32_t format  = HLE_ARG_U32(4) & 0xFF;
    uint32_t wrap_s  = HLE_ARG_U32(5) & 3;
    uint32_t wrap_t  = HLE_ARG_U32(6) & 3;
    /* Dump every texture the game builds an object for -- catches art loaded
     * into memory even if it's never drawn. (See gx_ogl.c) */
    { extern void gx_ogl_dump_tex(uint32_t, uint32_t, uint32_t, uint32_t);
      gx_ogl_dump_tex(img_va, width, height, format); }
    uint32_t mipmap = HLE_ARG_U32(7) & 1;
    if (obj_va) {
        /* Build the REAL SDK GXTexObj: the struct holds the precomputed BP
         * register payloads and consumers read it NATIVELY — the recompiled
         * GXLoadTexObjPreLoaded pushes [+0x00/04/08/0C] into the FIFO, K3D's
         * make_swizzler bakes them into material blobs, and the +0x1F flags
         * byte (bit0 = initialized) gates whether K3D binds at all. The old
         * homebrew stash left flags=0 -> K3D skipped every texture bind ->
         * the textureless menu (white keys on white glow, gradient dome).
         *   +0x00 mode0  : wrap_s[1:0] wrap_t[3:2] mag[4] min[7:5]
         *   +0x04 mode1  : 0 (lod)
         *   +0x08 image0 : (w-1)[9:0] (h-1)[19:10] fmt[23:20]
         *   +0x0C image3 : physical_addr >> 5
         *   +0x14 fmt    : u32 copy of the GX format
         *   +0x1F flags  : bit0 = valid, bit1 = mipmap */
        uint8_t *p = (uint8_t *)ppc_host_ptr(obj_va);
        if (p) {
            uint32_t min_f  = mipmap ? 6u : 4u;   /* lin_mip_lin : linear */
            uint32_t mode0  = (wrap_s & 3) | ((wrap_t & 3) << 2)
                            | (1u << 4) | (min_f << 5);
            uint32_t image0 = ((width - 1) & 0x3FF)
                            | (((height - 1) & 0x3FF) << 10)
                            | ((format & 0xF) << 20);
            uint32_t phys   = img_va & 0x7FFFFFFFu;   /* VA -> phys (MEM1+MEM2) */
            uint32_t image3 = (phys >> 5) & 0x00FFFFFFu;
            uint32_t be[4] = { mode0, 0, image0, image3 };
            for (int i = 0; i < 4; ++i) {
                p[i*4+0] = (uint8_t)(be[i] >> 24); p[i*4+1] = (uint8_t)(be[i] >> 16);
                p[i*4+2] = (uint8_t)(be[i] >>  8); p[i*4+3] = (uint8_t)(be[i]);
            }
            p[0x14] = 0; p[0x15] = 0; p[0x16] = 0; p[0x17] = (uint8_t)format;
            p[0x1F] = (uint8_t)(1u | (mipmap << 1));
        }
    }
    // Also register the texture in a rotating preview slot of g_tex[] so
    // the debug overlay can show it even before the game calls LoadTexObj.
    // Real LoadTexObj call (below) will overwrite slot[mapid] with the
    // game-specified binding. Gated by the same RECOMP_DEBUG_OVERLAY env
    // var as the overlay itself -- without the overlay this auto-preview
    // just pollutes g_tex[] with bindings the game never asked for.
    static int debug_overlay = -1;
    if (debug_overlay < 0) {
        const char *e = getenv("RECOMP_DEBUG_OVERLAY");
        debug_overlay = (e && e[0] && e[0] != '0');
    }
    if (debug_overlay && img_va && width && height) {
        static int preview_next;
        int slot = preview_next % GX_MAX_TEXMAP;
        preview_next++;
        // Don't trample slot 0 if the game already LoadTexObj'd into it.
        if (slot == 0 && g_tex[0].valid) slot = (preview_next % (GX_MAX_TEXMAP - 1)) + 1;
        g_tex[slot].valid  = 1;
        g_tex[slot].data_va = img_va;
        g_tex[slot].width  = width;
        g_tex[slot].height = height;
        g_tex[slot].fmt    = format;
        g_tex[slot].wrap_s = wrap_s;
        g_tex[slot].wrap_t = wrap_t;
    }
    static int log_n;
    if (log_n < 12) {
        fprintf(stderr, "[TEX] InitTexObj obj=0x%08x img=0x%08x %ux%u fmt=%u wrap=%u/%u\n",
                obj_va, img_va, width, height, format, wrap_s, wrap_t);
        fflush(stderr);
        log_n++;
    }
    HLE_RET(0);
}

// GXLoadTexObj(GXTexObj *obj, GXTexMapID mapid) — decodes the REAL SDK
// GXTexObj layout written by hle_GXInitTexObj above (and by the recompiled
// GXInitTexObj* variants, which always used this layout).
void hle_GXLoadTexObj(void) {
    uint32_t obj_va = HLE_ARG_U32(0);
    uint32_t mapid  = HLE_ARG_U32(1);
    if (mapid < GX_MAX_TEXMAP && obj_va) {
        uint8_t *p = (uint8_t *)ppc_host_ptr(obj_va);
        if (p) {
            uint32_t mode0  = ((uint32_t)p[0x00] << 24) | ((uint32_t)p[0x01] << 16)
                            | ((uint32_t)p[0x02] <<  8) |  (uint32_t)p[0x03];
            uint32_t image0 = ((uint32_t)p[0x08] << 24) | ((uint32_t)p[0x09] << 16)
                            | ((uint32_t)p[0x0A] <<  8) |  (uint32_t)p[0x0B];
            uint32_t image3 = ((uint32_t)p[0x0C] << 24) | ((uint32_t)p[0x0D] << 16)
                            | ((uint32_t)p[0x0E] <<  8) |  (uint32_t)p[0x0F];
            g_tex[mapid].valid  = 1;
            g_tex[mapid].width  = (image0 & 0x3FF) + 1;
            g_tex[mapid].height = ((image0 >> 10) & 0x3FF) + 1;
            g_tex[mapid].fmt    = (image0 >> 20) & 0xF;
            g_tex[mapid].wrap_s = mode0 & 3;
            g_tex[mapid].wrap_t = (mode0 >> 2) & 3;
            g_tex[mapid].data_va = ((image3 & 0x00FFFFFFu) << 5) | 0x80000000u;
            /* Log each DISTINCT texture (va,fmt) once — a flat call cap gets
             * exhausted by the intro movie rebinding every frame and then
             * hides every menu texture bind. */
            {
                static uint32_t s_seen[64][2]; static int s_ns;
                uint32_t va = g_tex[mapid].data_va, key = g_tex[mapid].fmt;
                int dup = 0;
                for (int i = 0; i < s_ns; ++i)
                    if (s_seen[i][0] == va && s_seen[i][1] == key) { dup = 1; break; }
                if (!dup && s_ns < 64) {
                    s_seen[s_ns][0] = va; s_seen[s_ns][1] = key; s_ns++;
                    fprintf(stderr, "[TEX] LoadTexObj map=%u data=0x%08x %ux%u fmt=%u\n",
                            mapid, va, g_tex[mapid].width, g_tex[mapid].height,
                            g_tex[mapid].fmt);
                    fflush(stderr);
                }
            }
        }
    }
    HLE_RET(0);
}
// Display-list HLEs. LyN records ALL its material state (TEV combiners,
// TREF, blend, zmode...) into GX display lists and replays them per draw.
// The old no-op stubs silently dropped every one of those state writes --
// the FIFO never saw a single TREF/TEV register and everything rendered
// black. Recording redirects the write-gather pipe into the guest buffer
// (see gx_dl_capture in ppc_mmio_write's feed, via gx_fifo_push);
// GXCallDisplayList feeds the recorded bytes straight into the FIFO parser.
// THREAD-LOCAL: recording only diverts the pushes of the thread that called
// GXBeginDisplayList; other threads keep hitting the live ring.
_Thread_local uint32_t g_gx_dl_va;   // record target (0 = not recording)
_Thread_local uint32_t g_gx_dl_cap;  // buffer capacity
_Thread_local uint32_t g_gx_dl_len;  // bytes recorded so far
_Thread_local uint32_t g_gx_dl_lost; // bytes DROPPED during recording (buffer
                            // full / unmapped target) -- a nonzero value means
                            // the recorded list has holes and will desync the
                            // parser when replayed

void hle_GXBeginDisplayList(void) {
    g_gx_dl_va  = HLE_ARG_U32(0);
    g_gx_dl_cap = HLE_ARG_U32(1);
    g_gx_dl_len = 0;
    g_gx_dl_lost = 0;
    static int s_n;
    if (s_n++ < 8) {
        fprintf(stderr, "[GX-DL] begin record va=0x%08x cap=%u\n",
                g_gx_dl_va, g_gx_dl_cap);
        fflush(stderr);
    }
    HLE_RET(0);
}

void hle_GXEndDisplayList(void) {
    uint32_t len = g_gx_dl_len;
    // GX pads display lists to a 32-byte boundary with NOP (0x00) bytes.
    if (g_gx_dl_va && (len & 31u)) {
        uint32_t pad = 32u - (len & 31u);
        uint8_t *dst = (uint8_t *)ppc_host_ptr(g_gx_dl_va + len);
        if (dst && len + pad <= g_gx_dl_cap) { memset(dst, 0, pad); len += pad; }
    }
    static int s_n;
    if (s_n++ < 8) {
        fprintf(stderr, "[GX-DL] end record va=0x%08x len=%u\n",
                g_gx_dl_va, len);
        fflush(stderr);
    }
    if (g_gx_dl_lost) {
        // Always log: a hole-y DL replayed later is a guaranteed desync,
        // and this is the only place the hole is observable.
        fprintf(stderr, "[GX-DL] RECORD OVERFLOW va=0x%08x cap=%u lost=%u bytes (list has holes!)\n",
                g_gx_dl_va, g_gx_dl_cap, g_gx_dl_lost);
        fflush(stderr);
        // Neutralize it: fill the recorded region with NOPs so replaying
        // the truncated list is a harmless no-op (one mesh/material skipped)
        // instead of a parser desync that trashes whole frames.
        uint8_t *base = (uint8_t *)ppc_host_ptr(g_gx_dl_va);
        if (base && len <= g_gx_dl_cap) memset(base, 0, len);
    }
    g_gx_dl_va = 0;
    g_gx_dl_len = 0;
    HLE_RET(len);
}

void hle_GXCallDisplayList(void) {
    uint32_t va = HLE_ARG_U32(0);
    uint32_t n  = HLE_ARG_U32(1);
    const uint8_t *src = (const uint8_t *)ppc_host_ptr(va);
    static int s_n;
    if (s_n++ < 8) {
        fprintf(stderr, "[GX-DL] call va=0x%08x len=%u\n", va, n);
        fflush(stderr);
    }
    // Nested call while RECORDING: real hardware records a 9-byte CallDL
    // command, NOT the callee's contents. Inlining the contents was the
    // world-load killer: the game sizes its record buffers for the 9-byte
    // form, so inlining overflowed them (RECORD OVERFLOW lost=NN in logs;
    // every observed overflow == own bytes + nested list size), the list
    // was truncated with holes, and replaying it desynced the parser every
    // frame -- the constant in-game flashing. Emit the real command; the
    // op-0x40 handler expands it (validated, correct order) at replay.
    if (g_gx_dl_va) {
        if (va && n) {
            uint8_t cmd[9];
            cmd[0] = 0x40;
            cmd[1] = (uint8_t)(va >> 24); cmd[2] = (uint8_t)(va >> 16);
            cmd[3] = (uint8_t)(va >>  8); cmd[4] = (uint8_t)(va      );
            cmd[5] = (uint8_t)(n  >> 24); cmd[6] = (uint8_t)(n  >> 16);
            cmd[7] = (uint8_t)(n  >>  8); cmd[8] = (uint8_t)(n       );
            gx_fifo_push(cmd, 9);   // recording is active -> lands in the DL
        }
        HLE_RET(0);
        return;
    }
    // The ring should be at a command boundary here (the game calls this
    // between commands). Pending bytes mean a partial command is buffered --
    // the DL bytes would splice INTO its payload and garble both streams.
    // Log it: this is a prime suspect for the world-load desync.
    {
        uint32_t pending = gx_fifo_tail - gx_fifo_head;
        if (pending) {
            static uint32_t s_warn;
            if (++s_warn <= 16 || (s_warn & 0x3FF) == 0) {
                fprintf(stderr,
                        "[GX-DL] WARNING #%u: call va=0x%08x len=%u with %u bytes pending in ring\n",
                        s_warn, va, n, pending);
                fflush(stderr);
            }
        }
    }
    if (src && n && n < 0x01000000u && gx_va_is_ram(va, n))
        gx_fifo_push(src, (int)n);
    HLE_RET(0);
}


// ===========================================================================
// AX / AI -- Audio.
//
// AX is the high-level mixer ("Audio System Library"). Real games:
//   1. AXInit
//   2. AXAcquireVoice(prio, callback, userdata) -> AXVOICE*
//   3. Configure the voice (AXSetVoiceState, AXSetVoiceSrc, AXSetVoiceMix...)
//   4. Periodically refresh state in the AXRegisterCallback frame callback.
//   5. AXFreeVoice(v) when done.
//
// We don't render audio yet, but we DO need to hand out non-NULL voice
// pointers so games' "if (!v) return false;" early-outs don't trigger.
// Each voice is a guest-side scratch buffer so games can write to it
// (priority field, source state, etc.) without crashing.
//
// AXVOICE struct is tiny (Nintendo SDK lays it out ~0x40 bytes). We
// reserve a fixed pool in low MEM2 at boot (AXInit) and hand out the
// VAs as voice handles. Free puts them back in the pool.
// ===========================================================================

#define AX_MAX_VOICES   96      // matches retail Wii AX voice count
/* Voice slot size: the RECOMPILED AXSetVoice* setters (they are COMPLETE,
 * not HLE'd) write parameter-block fields up to at least +0xe1 into each
 * voice struct (state@0x38, addr@0x96, adpcm@0xa6, src@0xce, adpcmLoop@0xdc).
 * The old 0x80 slot size let every voice scribble its neighbor. */
#define AX_VOICE_SIZE   0x200

typedef struct {
    uint32_t voice_va;          // 0 = slot empty
    int      in_use;
    int32_t  prio;
} AxSlot;

static AxSlot   g_ax_slots[AX_MAX_VOICES];
static uint32_t g_ax_pool_base;
static int      g_ax_pool_ready;

extern uint32_t game_heap_alloc(uint32_t size, uint32_t align);

static void ax_pool_init(void) {
    if (g_ax_pool_ready) return;
    // Reserve the voice pool up front. game_heap_alloc lives in the
    // SDK heap (MEM2). 96 * 0x80 = 12 KB.
    g_ax_pool_base = game_heap_alloc(AX_MAX_VOICES * AX_VOICE_SIZE, 32);
    if (!g_ax_pool_base) {
        fprintf(stderr, "[AX] failed to reserve voice pool\n");
        return;
    }
    // Zero the pool so guest reads of unused fields return 0.
    uint8_t *p = (uint8_t *)ppc_host_ptr(g_ax_pool_base);
    if (p) memset(p, 0, AX_MAX_VOICES * AX_VOICE_SIZE);
    for (int i = 0; i < AX_MAX_VOICES; ++i) {
        g_ax_slots[i].voice_va = g_ax_pool_base + i * AX_VOICE_SIZE;
        g_ax_slots[i].in_use   = 0;
    }
    g_ax_pool_ready = 1;
}

void hle_AXInit                (void) {
    extern void audio_init(void);
    audio_init();
    ax_pool_init();
    /* The recompiled AX lib keeps its own "initialized" flag in SDA at
     * r13-0x3f68 (read by func_80106e50 = AXIsInit). Robox's MIDI-player
     * init (func_8010bf70, music) checks it and BAILS when zero — with
     * AXInit HLE'd nobody ever set it, so the music player never
     * registered and the game stayed music-less. */
    MEM_W32(g_cpu.gpr[13] - 0x3f68u, 1);
    HLE_RET(1);     // claim success so init continues
}
void hle_AXIsInit              (void) { HLE_RET(1); }
void hle_AXQuit                (void) { HLE_RET(0); }
// The AX frame callback is the game's 5 ms audio heartbeat: it mixes
// voices, feeds Bink audio, and advances every audio-paced state machine.
// Swallowing it froze all of that (Bink intro waited on audio forever).
// Capture it here; vi_pump_retraces fires it ~3x per 60 Hz retrace.
uint32_t g_ax_frame_cb = 0;
void hle_AXRegisterCallback(void) {
    uint32_t prev = g_ax_frame_cb;
    g_ax_frame_cb = HLE_ARG_U32(0);
    fprintf(stderr, "[AX] frame callback registered: 0x%08x\n", g_ax_frame_cb);
    fflush(stderr);
    HLE_RET(prev);
}
void hle_AXSetMode             (void) { HLE_RET(0); }
void hle_AXGetMode             (void) { HLE_RET(0); }

// AXVOICE *AXAcquireVoice(s32 priority, AXAcquireVoiceCallback cb, u32 user)
// Returns NULL if no voice available, else pointer to AXVOICE.
void hle_AXAcquireVoice(void) {
    int32_t prio = HLE_ARG_S32(0);
    /* TEMP(audio hunt): is the sound engine even asking for voices? */
    { static int n; if (n < 8) { ++n;
        fprintf(stderr, "[AXACQ] prio=%d lr=0x%08x\n", prio, g_cpu.lr);
        fflush(stderr); } }
    ax_pool_init();
    if (!g_ax_pool_ready) { HLE_RET(0); return; }

    // Try to find a free slot first.
    for (int i = 0; i < AX_MAX_VOICES; ++i) {
        if (!g_ax_slots[i].in_use) {
            g_ax_slots[i].in_use = 1;
            g_ax_slots[i].prio   = prio;
            HLE_RET(g_ax_slots[i].voice_va);
            return;
        }
    }

    // All slots taken. SDK behavior: steal a lower-priority voice if
    // one exists. We don't do priority dropping (no audio is playing
    // anyway) -- just return NULL so the caller knows acquisition failed.
    HLE_RET(0);
}

// void AXFreeVoice(AXVOICE *v)
void hle_AXFreeVoice(void) {
    uint32_t voice_va = HLE_ARG_U32(0);
    if (!voice_va) { HLE_RET(0); return; }
    for (int i = 0; i < AX_MAX_VOICES; ++i) {
        if (g_ax_slots[i].voice_va == voice_va) {
            g_ax_slots[i].in_use = 0;
            // Clear the voice memory so a subsequent acquire of the
            // same slot starts clean.
            uint8_t *p = (uint8_t *)ppc_host_ptr(voice_va);
            if (p) memset(p, 0, AX_VOICE_SIZE);
            break;
        }
    }
    HLE_RET(0);
}

// ===========================================================================
// AX VOICE MIXER — the same math Dolphin's AXWii ucode HLE runs, applied to
// the parameter blocks the game's own (recompiled) AXSetVoice* setters
// maintain inside our voice pool. Field offsets confirmed from those
// setters' code:
//   +0x38 state (u16: 0 stop, 1 run)
//   +0x96 loopFlag u16   +0x98 format u16 (0 ADPCM, 10 PCM16, 25 PCM8)
//   +0x9a loopAddr u32   +0x9e endAddr u32   +0xa2 currentAddr u32
//         (sample-units: ADPCM = nibbles incl. frame headers; PHYSICAL mem)
//   +0xa6 s16 coefs[16]  +0xc6 gain  +0xc8 pred_scale  +0xca yn1  +0xcc yn2
//   +0xce ratioHi/+0xd0 ratioLo (16.16)  +0xd2 currentAddressFrac
//   +0xdc loop_pred_scale  +0xde loop_yn1  +0xe0 loop_yn2
// Mixing: 32 kHz voice clock resampled to the host device rate, linear
// interpolation, fixed bring-up volume; output = interleaved s16 BE stereo
// pushed into the SDL ring (audio_submit_host).
// ===========================================================================

static int16_t ax_clamp16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32767) return -32767;
    return (int16_t)v;
}

/* one decoded source sample per voice, host-side interp cache */
static struct { int16_t prev, cur; } g_ax_interp[AX_MAX_VOICES];

/* advance one source sample; returns 0 when the voice ended */
static int ax_advance_sample(uint32_t v, int slot) {
    uint32_t fmt  = MEM_R16(v + 0x98u);
    uint32_t cur  = MEM_R32(v + 0xa2u);
    uint32_t end  = MEM_R32(v + 0x9eu);

    if (cur >= end) {
        if (MEM_R16(v + 0x96u)) {          /* looped voice */
            cur = MEM_R32(v + 0x9au);
            /* Restore the ADPCM loop context ONLY when the game precomputed
             * one (static looping samples). STREAM RINGS carry the all-zero
             * signature: the feeder rewrites the ring continuously, so the
             * baked context is stale by wrap time — restoring it snapped
             * yn1/yn2 and produced a click every ring pass (the crunchy
             * music: one discontinuity every 2.226 s = 0x13e00-byte ring).
             * Keeping the decoder state continuous is seamless; pred/scale
             * refreshes at the next 16-nibble frame header regardless. */
            if (fmt == 0) {
                uint16_t lps = MEM_R16(v + 0xdcu);
                uint16_t ly1 = MEM_R16(v + 0xdeu);
                uint16_t ly2 = MEM_R16(v + 0xe0u);
                if (lps | ly1 | ly2) {
                    MEM_W16(v + 0xc8u, lps);
                    MEM_W16(v + 0xcau, ly1);
                    MEM_W16(v + 0xccu, ly2);
                }
            }
        } else {
            MEM_W16(v + 0x38u, 0);         /* state = STOP */
            return 0;
        }
    }

    int32_t sample = 0;
    if (fmt == 0) {                        /* DSP-ADPCM, addr in nibbles */
        if ((cur & 15u) == 0) {            /* frame header: pred/scale byte */
            uint32_t hdr_va = (cur >> 1) + 0x80000000u;
            MEM_W16(v + 0xc8u, MEM_R8(hdr_va));
            cur += 2;
        }
        uint32_t ps    = MEM_R16(v + 0xc8u);
        int32_t  scale = 1 << (ps & 0xFu);
        uint32_t ci    = (ps >> 4) & 7u;
        int32_t  c1    = (int16_t)MEM_R16(v + 0xa6u + ci * 4u);
        int32_t  c2    = (int16_t)MEM_R16(v + 0xa6u + ci * 4u + 2u);
        uint32_t byte_va = (cur >> 1) + 0x80000000u;
        uint8_t  b     = MEM_R8(byte_va);
        int32_t  nib   = (cur & 1u) ? (b & 0xF) : (b >> 4);
        if (nib >= 8) nib -= 16;
        int32_t yn1 = (int16_t)MEM_R16(v + 0xcau);
        int32_t yn2 = (int16_t)MEM_R16(v + 0xccu);
        /* Dolphin's DSP-ADPCM step: ((nibble*scale)<<11 + 0x400 + c1*yn1 + c2*yn2) >> 11 */
        sample = ax_clamp16((((nib * scale) << 11) + 1024 + c1 * yn1 + c2 * yn2) >> 11);
        MEM_W16(v + 0xccu, (uint16_t)(int16_t)yn1);
        MEM_W16(v + 0xcau, (uint16_t)(int16_t)sample);
        cur += 1;
    } else if (fmt == 10) {                /* PCM16, addr in samples */
        sample = (int16_t)MEM_R16(cur * 2u + 0x80000000u);
        cur += 1;
    } else if (fmt == 25) {                /* PCM8, addr in bytes */
        sample = (int32_t)((int8_t)MEM_R8(cur + 0x80000000u)) << 8;
        cur += 1;
    } else {
        MEM_W16(v + 0x38u, 0);
        return 0;
    }

    MEM_W32(v + 0xa2u, cur);
    g_ax_interp[slot].prev = g_ax_interp[slot].cur;
    g_ax_interp[slot].cur  = (int16_t)sample;
    return 1;
}

/* Debug WAV dump of the mixer output (s16le stereo @ device rate).
 * File: axmix.wav next to the exe; capped at ~120 s. Header sizes are
 * patched on exit.
 *
 * Off unless ROBOX_AXDUMP is set. This was on unconditionally while the audio
 * path was being brought up, which meant every launch quietly wrote a WAV
 * beside the executable and kept writing for two minutes -- ~23 MB of disk per
 * run, for a diagnostic nobody outside this project wants. */
static FILE    *g_axwav;
static uint32_t g_axwav_bytes;
static int      g_axwav_enabled = -1;   /* -1 = not yet checked */

static int axwav_wanted(void) {
    if (g_axwav_enabled < 0) {
        const char *e = getenv("ROBOX_AXDUMP");
        g_axwav_enabled = (e && e[0] && e[0] != '0');
    }
    return g_axwav_enabled;
}
static void axwav_close(void) {
    if (!g_axwav) return;
    uint32_t riff = 36 + g_axwav_bytes, data = g_axwav_bytes;
    fseek(g_axwav, 4, SEEK_SET);  fwrite(&riff, 4, 1, g_axwav);
    fseek(g_axwav, 40, SEEK_SET); fwrite(&data, 4, 1, g_axwav);
    fclose(g_axwav); g_axwav = NULL;
}
static void axwav_write(const int16_t *lr, int nframes, int freq) {
    if (!axwav_wanted()) return;
    if (g_axwav_bytes > 120u * 48000u * 4u) { axwav_close(); return; }
    if (!g_axwav) {
        g_axwav = fopen("axmix.wav", "wb");
        if (!g_axwav) return;
        uint32_t u32; uint16_t u16;
        fwrite("RIFF", 1, 4, g_axwav); u32 = 0; fwrite(&u32, 4, 1, g_axwav);
        fwrite("WAVEfmt ", 1, 8, g_axwav);
        u32 = 16;              fwrite(&u32, 4, 1, g_axwav);
        u16 = 1;               fwrite(&u16, 2, 1, g_axwav);   /* PCM */
        u16 = 2;               fwrite(&u16, 2, 1, g_axwav);   /* stereo */
        u32 = (uint32_t)freq;  fwrite(&u32, 4, 1, g_axwav);
        u32 = (uint32_t)freq * 4; fwrite(&u32, 4, 1, g_axwav);
        u16 = 4;               fwrite(&u16, 2, 1, g_axwav);
        u16 = 16;              fwrite(&u16, 2, 1, g_axwav);
        fwrite("data", 1, 4, g_axwav); u32 = 0; fwrite(&u32, 4, 1, g_axwav);
        atexit(axwav_close);
        fprintf(stderr, "[AXMIX] dumping mixer output to axmix.wav (%d Hz)\n", freq);
        fflush(stderr);
    }
    fwrite(lr, 4, (size_t)nframes, g_axwav);
    g_axwav_bytes += (uint32_t)nframes * 4u;
}

// Host-side note release: the game's recompiled SYN envelope never ramps a
// released voice's volume down (notes hung forever). The release entry point
// (func_80111760) is hooked to call this; the mixer then fades the voice out
// over ~200 ms and stops it, which also lets the SYN voice reaper reclaim it
// (it waits for AX state 0).
static uint8_t  g_ax_releasing[AX_MAX_VOICES];
static int32_t  g_ax_relgain[AX_MAX_VOICES];   /* 16.16, 0x10000 = 1.0 */
void robox_ax_note_release(uint32_t axvoice_va) {
    for (int i = 0; i < AX_MAX_VOICES; ++i) {
        if (g_ax_slots[i].voice_va == axvoice_va) {
            if (!g_ax_releasing[i]) {
                g_ax_releasing[i] = 1;
                g_ax_relgain[i]   = 0x10000;
            }
            return;
        }
    }
}

/* --- audio envelope, for mods ------------------------------------------- */
//
// The final mix already computes a frame peak for its own logging; turning
// that into something a script can read costs two multiply-adds and gives
// audio-reactive mods a real signal instead of a hardcoded tempo that drifts
// against whatever is actually playing.
//
// Two values, because they answer different questions. `level` is a fast-
// attack / slow-release follower: how loud is it right now. `beat` is that
// minus a much slower average, rectified -- a transient detector, which is
// what "flash on the beat" actually means. Written from the audio callback
// and read from the frame thread without a lock: they are single floats whose
// worst case is one stale read, and a mutex on the audio path would be a far
// worse trade.
static volatile float g_aud_fast, g_aud_slow;

/* Raw mono tap, so a mod can do its own analysis (robox.audio.spectrum runs an
 * FFT over this). A ring rather than a callback: the audio side just keeps
 * writing and whoever reads takes the newest window, so a reader that misses a
 * block or runs at a different rate simply sees slightly older audio instead
 * of blocking the mixer. */
#define AUD_RING 2048
static float             g_aud_ring[AUD_RING];
static volatile unsigned g_aud_ring_w;
static volatile int      g_aud_ring_rate = 32000;

/* Newest `n` samples, oldest first. Returns how many were written. */
int robox_audio_capture(float *out, int n, int *rate_out) {
    if (n > AUD_RING) n = AUD_RING;
    if (n < 0) n = 0;
    const unsigned w = g_aud_ring_w;
    for (int i = 0; i < n; ++i)
        out[i] = g_aud_ring[(w - (unsigned)n + (unsigned)i) & (AUD_RING - 1)];
    if (rate_out) *rate_out = g_aud_ring_rate;
    return n;
}

float robox_audio_level(void) { return g_aud_fast; }
float robox_audio_beat(void) {
    float d = (g_aud_fast - g_aud_slow) * 3.0f;
    return d <= 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
}

void ax_mixer_frame(void) {
    extern int  audio_device_freq(void);
    extern void audio_submit_host(const void *data, uint32_t len);
    if (!g_ax_pool_ready) return;

    /* DIAG: the boot voices point at bank memory that stays zero. Log the
     * moment anything writes there (fill happened) — or never log (fill
     * never attempted). Bank VA deterministic across runs. */
    {
        static int s_filled;
        if (!s_filled && MEM_R32(0x90833141u)) {
            s_filled = 1;
            fprintf(stderr, "[AXBANK] sound bank 0x90833141 became nonzero: %08x %08x\n",
                    MEM_R32(0x90833141u), MEM_R32(0x90833145u));
            fflush(stderr);
        }
    }

    /* Self-regulating pacing: top the SDL ring up to ~85 ms. This absorbs
     * the 59.94-vs-60 rate mismatch, frame jitter, and start/stop gaps that
     * used to leave the ring nearly empty (every device callback underran
     * against the submitter = the constant crackle). */
    extern uint32_t audio_ring_fill_bytes(void);
    int freq = audio_device_freq();
    uint32_t fill_frames = audio_ring_fill_bytes() / 4;   /* s16 stereo */
    uint32_t target = (uint32_t)freq * 85u / 1000u;
    int nout = (fill_frames < target) ? (int)(target - fill_frames) : 0;
    if (nout > 2048) nout = 2048;
    if (nout <= 0) return;

    static int32_t mixbuf[2048 * 2];
    memset(mixbuf, 0, (size_t)nout * 2 * sizeof(int32_t));

    int active = 0;
    static uint8_t s_was_running[AX_MAX_VOICES];
    for (int s = 0; s < AX_MAX_VOICES; ++s) {
        if (!g_ax_slots[s].in_use) { s_was_running[s] = 0; continue; }
        uint32_t v = g_ax_slots[s].voice_va;
        /* TEMP(audio hunt): what does Robox's AX lib write into an acquired
         * voice? Dump the header region whenever it changes (capped). */
        { static uint32_t s_hash[AX_MAX_VOICES]; static int s_dumps;
          uint32_t h = 2166136261u;
          for (uint32_t o = 0x30; o < 0x60; o += 2)
              h = (h ^ MEM_R16(v + o)) * 16777619u;
          if (h != s_hash[s] && s_dumps < 24) { s_hash[s] = h; ++s_dumps;
            fprintf(stderr, "[AXRAW] slot=%d va=0x%08x +30:", s, v);
            for (uint32_t o = 0x30; o < 0x60; o += 2)
                fprintf(stderr, " %04x", MEM_R16(v + o));
            fprintf(stderr, "\n");
            fflush(stderr);
          } }
        if (MEM_R16(v + 0x38u) != 1) { s_was_running[s] = 0; continue; }   /* not RUN */
        if (!s_was_running[s]) {                 /* voice just started: dump params */
            s_was_running[s] = 1;
            g_ax_releasing[s] = 0;               /* fresh note: cancel any old fade */
            g_ax_relgain[s]   = 0x10000;
            static int s_vlog;
            if (s_vlog < 48) {
                s_vlog++;
                uint32_t cur = MEM_R32(v + 0xa2u);
                uint32_t dva = (cur >> 1) + 0x80000000u;
                fprintf(stderr, "[AXVOICE] slot=%d fmt=%u loop=%u loopA=0x%x endA=0x%x curA=0x%x ratio=0x%04x%04x pred=0x%x data@0x%08x: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                        s, MEM_R16(v + 0x98u), MEM_R16(v + 0x96u),
                        MEM_R32(v + 0x9au), MEM_R32(v + 0x9eu), cur,
                        MEM_R16(v + 0xceu), MEM_R16(v + 0xd0u), MEM_R16(v + 0xc8u),
                        dva, MEM_R8(dva), MEM_R8(dva+1), MEM_R8(dva+2), MEM_R8(dva+3),
                        MEM_R8(dva+4), MEM_R8(dva+5), MEM_R8(dva+6), MEM_R8(dva+7));
                fflush(stderr);
            }
        }
        active++;

        uint32_t ratio = (MEM_R16(v + 0xceu) << 16) | MEM_R16(v + 0xd0u);
        if (ratio == 0) ratio = 0x10000;
        /* voice clock is 32 kHz; convert to device rate (16.16 step) */
        uint32_t step = (uint32_t)(((uint64_t)ratio * 32000u) / (uint32_t)freq);
        uint32_t frac = MEM_R16(v + 0xd2u);
        /* AXPBVE: current volume (+0x40, 0x8000 = 1.0) and PER-SAMPLE delta
         * (+0x42, signed). The hardware ucode ramps vol += delta each 32 kHz
         * sample; the SYN music engine relies on that for note releases (it
         * writes a negative delta ONCE at note-off and frees the voice when
         * the ramp lands at zero). Ignoring the delta held every note at its
         * key-on volume forever. Track the ramp in 16.16 against the
         * resample step and write the result back so the engine sees it. */
        int32_t vol16   = (int32_t)MEM_R16(v + 0x40u);
        if (vol16 > 0x8000) vol16 = 0x8000;
        /* host-side release fade: ~200 ms from full gain to silence */
        int32_t relgain = g_ax_relgain[s];
        int32_t reldec  = g_ax_releasing[s] ? (5 * 0x10000) / freq : 0;

        for (int i = 0; i < nout; ++i) {
            frac += step;
            while (frac >= 0x10000u) {
                frac -= 0x10000u;
                if (!ax_advance_sample(v, s)) { frac = 0; goto voice_done; }
            }
            {
                int32_t a = g_ax_interp[s].prev, b = g_ax_interp[s].cur;
                int32_t smp = a + (int32_t)(((int64_t)(b - a) * frac) >> 16);
                smp = (int32_t)(((int64_t)smp * vol16) >> 15);
                if (reldec) {
                    relgain -= reldec;
                    if (relgain < 0) relgain = 0;
                    smp = (int32_t)(((int64_t)smp * relgain) >> 16);
                }
                smp = (smp * 5) / 8;                    /* fixed bring-up master */
                mixbuf[i * 2]     += smp;
                mixbuf[i * 2 + 1] += smp;
            }
        }
        if (reldec) {
            g_ax_relgain[s] = relgain;
            if (relgain == 0) {
                MEM_W16(v + 0x38u, 0);                  /* faded out: stop voice */
                g_ax_releasing[s] = 0;
                s_was_running[s]  = 0;
            }
        }
      voice_done:
        MEM_W16(v + 0xd2u, (uint16_t)frac);
    }

    {
        static int s_n;
        static int s_last_active = -1;
        if (active != s_last_active && s_n < 40) {
            s_n++;
            fprintf(stderr, "[AXMIX] active voices: %d\n", active);
            fflush(stderr);
        }
        s_last_active = active;
    }
    /* Settings menu (sdk/robox_menu.c). Music is skipped at the source rather
     * than scaled to zero so a muted track costs no synthesis at all. */
    extern int g_music_on;      /* 1 = mix the DLS sampler in */
    extern int g_audio_volume;  /* 0..100, applied to the final mix below */

    /* MOD #1: mix the host DLS sampler's music voices in alongside the
     * game's AX voices (SFX). No-op when mods/robox.dls isn't loaded. */
    if (g_music_on) {
        extern void robox_synth_render(int32_t *mix, int nout, int freq);
        /* A Lua mod playing its own track (robox.audio.music_play -- a
         * playlist, say) owns the music while it runs: skip the sampler, or
         * the game's song plays underneath it. Muted, not stopped, so fmidi
         * keeps its place and is simply heard again when the track ends. */
        extern int robox_wav_is_direct(void);
        if (!robox_wav_is_direct())
            robox_synth_render(mixbuf, nout, freq);
        /* MOD: WAV music packs -- sounds only while a mapped song plays
         * (sdk/robox_wav.c); the DLS sampler is idle for that song. */
        extern void robox_wav_render(int32_t *mix, int nout, int freq);
        robox_wav_render(mixbuf, nout, freq);
    }

    /* Always submit — including silence. Gating on active voices drained
     * the ring at every sound start/stop boundary (discontinuity clicks). */

    static uint8_t out[2048 * 4];
    static int16_t wav[2048 * 2];
    int32_t peak = 0;
    for (int i = 0; i < nout * 2; ++i) {
        /* Scale before clamping: clamping first would flatten peaks and then
         * quiet the already-distorted result. */
        int32_t scaled = g_audio_volume >= 100 ? mixbuf[i]
                                               : (mixbuf[i] * g_audio_volume) / 100;
        int16_t smp = ax_clamp16(scaled);
        int32_t a = smp < 0 ? -smp : smp; if (a > peak) peak = a;
        out[i * 2]     = (uint8_t)((uint16_t)smp >> 8);   /* big-endian */
        out[i * 2 + 1] = (uint8_t)((uint16_t)smp & 0xFF);
        wav[i] = smp;                                     /* little-endian host */
    }
    /* Envelope follower off the peak computed above. Attack is instant so a
     * kick lands on the frame it happens; release and the slow average are
     * per-block, and this block is a handful of milliseconds. */
    {
        float p = (float)peak / 32768.0f;
        if (p > 1.0f) p = 1.0f;
        float f = g_aud_fast;
        f = (p > f) ? p : f + (p - f) * 0.18f;
        g_aud_fast = f;
        g_aud_slow = g_aud_slow + (f - g_aud_slow) * 0.015f;

        /* Mono downmix into the ring for spectrum analysis. */
        unsigned w = g_aud_ring_w;
        for (int i = 0; i < nout; ++i) {
            g_aud_ring[(w + (unsigned)i) & (AUD_RING - 1)] =
                ((float)wav[i * 2] + (float)wav[i * 2 + 1]) * (1.0f / 65536.0f);
        }
        g_aud_ring_w   = w + (unsigned)nout;
        g_aud_ring_rate = freq;
    }

    audio_submit_host(out, (uint32_t)(nout * 4));
    axwav_write(wav, nout, freq);
    {
        static uint32_t s_total, s_ticks;
        s_total += (uint32_t)(nout * 4);
        if ((++s_ticks % 300u) == 0) {                    /* ~every 5 s */
            fprintf(stderr, "[AXMIX] submitted=%u bytes peak=%d active=%d\n",
                    s_total, peak, active);
            fflush(stderr);
        }
    }
}

void hle_AISetStreamVolLeft    (void) { HLE_RET(0); }
void hle_AISetStreamVolRight   (void) { HLE_RET(0); }
void hle_AISetStreamSampleRate (void) { HLE_RET(0); }
void hle_AIInit                (void) {
    // Open SDL audio as a side effect. Idempotent, safe to call many times.
    extern void audio_init(void);
    audio_init();
    HLE_RET(1);     // claim success so the game attempts DMA setup
}

// AI DMA surface. The game fills a buffer, calls AISetDMAStartAddr +
// AISetDMALength, then AIStartDMA. We capture the (va, len) pair and
// pipe it into SDL when AIStartDMA fires.
static uint32_t g_ai_dma_va;
static uint32_t g_ai_dma_len;

void hle_AISetDMAStartAddr(void) {
    g_ai_dma_va = HLE_ARG_U32(0);
    HLE_RET(0);
}
void hle_AISetDMALength(void) {
    g_ai_dma_len = HLE_ARG_U32(0);
    HLE_RET(0);
}
void hle_AIStartDMA(void) {
    extern void audio_submit(uint32_t va, uint32_t len);
    extern int32_t g_ai_bytes_left;
    if (g_ai_dma_va && g_ai_dma_len) {
        audio_submit(g_ai_dma_va, g_ai_dma_len);
        g_ai_bytes_left = (int32_t)g_ai_dma_len;
        static int n; if (n < 6) { ++n;
            fprintf(stderr, "[AI] StartDMA va=0x%08x len=%u\n", g_ai_dma_va, g_ai_dma_len);
            fflush(stderr); }
    }
    HLE_RET(0);
}
void hle_AIStopDMA(void) { HLE_RET(0); }
void hle_AIGetDMAStartAddr(void) { HLE_RET(g_ai_dma_va); }
void hle_AIGetDMALength   (void) { HLE_RET(g_ai_dma_len); }
void hle_AIGetDMABytesLeft(void) { HLE_RET(0); }    // "idle, ready for next"
// The AI DMA completion callback is the music streamer's heartbeat: DMA
// finishes -> callback refills the double buffer + starts the next DMA.
// Swallowing it (old stub) killed music after the first buffer. Store it;
// audio_pump_frame() fires it paced by consumed bytes.
uint32_t g_ai_dma_cb = 0;
int32_t  g_ai_bytes_left = 0;
void hle_AIRegisterDMACallback(void) {
    uint32_t prev = g_ai_dma_cb;
    g_ai_dma_cb = HLE_ARG_U32(0);
    fprintf(stderr, "[AI] DMA callback registered: 0x%08x\n", g_ai_dma_cb);
    fflush(stderr);
    HLE_RET(prev);
}
void hle_AIInitDMA        (void) { HLE_RET(0); }
void hle_AICheckInit      (void) { HLE_RET(1); }


// Engine-specific Flip / swap-buffer HLE lives under quirks/.


// ===========================================================================
// WPAD / KPAD / PAD -- Controller input. All stubs return "no buttons held".
// ===========================================================================

void hle_WPADInit              (void) { HLE_RET(0); }  // input handled via hle_WPADRead
void hle_WPADShutdown          (void) { HLE_RET(0); }
// WPADRead(chan, WPADStatus *status) -> s32 (0 = ok).
//
// WPADStatus layout (big-endian on Wii):
//   0x00 u16 buttons    <- bitfield, see below
//   0x02 s16 accelX ... etc.
//
// Real WPAD button bits (Nintendo SDK):
//   WPAD_BUTTON_LEFT  = 0x0001   WPAD_BUTTON_RIGHT = 0x0002
//   WPAD_BUTTON_DOWN  = 0x0004   WPAD_BUTTON_UP    = 0x0008
//   WPAD_BUTTON_PLUS  = 0x0010
//   WPAD_BUTTON_2     = 0x0100   WPAD_BUTTON_1     = 0x0200
//   WPAD_BUTTON_B     = 0x0400   WPAD_BUTTON_A     = 0x0800
//   WPAD_BUTTON_MINUS = 0x1000
//   WPAD_BUTTON_HOME  = 0x8000
//
// Every Wii game opens with 1-2 splashes + a "hold the Wiimote"
// health-and-safety style screen that wants A or 2. We cycle short
// bursts of A, 2, PLUS, HOME so every confirm-wait eventually advances.
void hle_WPADRead(void) {
    static uint32_t wpad_read_count = 0;
    if ((++wpad_read_count % 10000) == 1) {
        fprintf(stderr, "[WPAD] read #%u\n", wpad_read_count); fflush(stderr);
    }
    uint32_t status_va = HLE_ARG_U32(1);
    if (!status_va) { HLE_RET(0); return; }
    uint8_t *p = (uint8_t *)ppc_host_ptr(status_va);
    if (!p) { HLE_RET(0); return; }
    memset(p, 0, 0x30);
    // REAL input only — the buttons actually held on the keyboard this frame
    // (see sdk/video.c key_to_wpad). No synthetic presses: the game advances
    // a confirm screen only when the user really presses A (Enter/Space/Z).
    extern uint32_t video_input_hold(void);
    uint16_t buttons = (uint16_t)video_input_hold();
    /* Mario mode (robox_mario.c): hide the robot's fire button from the
     * guest during live gameplay -- Mario runs with it instead. */
    { extern uint32_t robox_mario_button_mask(void);
      buttons &= (uint16_t)~robox_mario_button_mask(); }
    p[0] = (uint8_t)(buttons >> 8);
    p[1] = (uint8_t) buttons;
    HLE_RET(0);
}
// WPADProbe(chan, type_out) -> status (0 = WPAD_ERR_NONE = connected).
// Previously returned -1 which made the game's "wait for controller"
// loops spin forever.
void hle_WPADProbe(void) {
    uint32_t type_out = HLE_ARG_U32(1);
    if (type_out) MEM_W32(type_out, 1);   // WPAD_DEV_FS = Wiimote + Nunchuk
    /* TEMP(menu-input hunt): who polls probe, for which channel? */
    { static int n; if (n < 12) { ++n;
        fprintf(stderr, "[WPADProbe] chan=%d type_out=0x%08x lr=0x%08x\n",
                (int32_t)HLE_ARG_U32(0), type_out, g_cpu.lr); fflush(stderr); } }
    HLE_RET(0);
}
void hle_WPADSetDataFormat     (void) { HLE_RET(0); }
void hle_WPADSetAutoSamplingBuf(void) { HLE_RET(0); }
void hle_WPADControlDpd        (void) { HLE_RET(0); }
void hle_WPADControlMotor      (void) { HLE_RET(0); }

void hle_KPADInit              (void) { HLE_RET(0); }
// KPADRead(chan, KPADStatus *status, u32 count) -> num_reads.
// KPADStatus layout (first 8 bytes are what title-screen logic checks):
//   0x00 u32 hold   (buttons currently held)
//   0x04 u32 trig   (edge: just pressed)
// Button bits use the same WPAD_BUTTON_* values as WPADRead above.
void hle_KPADRead(void) {
    static uint32_t kpad_read_count = 0;
    if ((++kpad_read_count % 10000) == 1 || kpad_read_count <= 8) {
        fprintf(stderr, "[KPAD] read #%u chan=%d count=%d lr=0x%08x "
                        "f28=%g f29=%g f30=%g f31=%g\n",
                kpad_read_count, (int32_t)HLE_ARG_U32(0),
                (int32_t)HLE_ARG_U32(2), g_cpu.lr,
                g_cpu.fpr[28].ps[0], g_cpu.fpr[29].ps[0],
                g_cpu.fpr[30].ps[0], g_cpu.fpr[31].ps[0]);
        fflush(stderr);
    }
    uint32_t status_va = HLE_ARG_U32(1);
    uint32_t count     = HLE_ARG_U32(2);
    if (!status_va || !count) { HLE_RET(1); return; }
    uint8_t *p = (uint8_t *)ppc_host_ptr(status_va);
    if (!p) { HLE_RET(1); return; }
    memset(p, 0, 0xF0);
    // REAL input only. hold = buttons the user is actually holding; trig = the
    // rising edge (just-pressed this read) computed against the previous hold.
    // No synthetic presses — confirm screens wait for a real A (Enter/Space/Z).
    extern uint32_t video_input_hold(void);
    static uint32_t prev_hold = 0;
    uint32_t hold = video_input_hold();
    /* Mario mode: hide the robot's fire button (trig follows the masked
     * hold, so no synthetic edges either). */
    { extern uint32_t robox_mario_button_mask(void);
      hold &= ~robox_mario_button_mask(); }
    uint32_t trig = hold & ~prev_hold;
    /* Robox: the main loop's per-channel controller state (obj 0x801fe040
     * +0x4078, one s32 per channel) is initialized to -1 ("no controller")
     * and on real hardware updated by WPAD connect events that our stubbed
     * BT/IOS machinery never delivers. -1 makes the input poller discard
     * every sample (dimmed menu, no cursor, no button response). Force
     * channel 0 to -2 (sideways wiimote, matching the menu's 1/2-button
     * prompts; the poller accepts {-2,-3,-4,-7}). */
    if (g_cpu.lr == 0x80022c58u &&
        (int32_t)MEM_R32(g_cpu.gpr[28] + 0x4078u) == -1) {
        MEM_W32(g_cpu.gpr[28] + 0x4078u, (uint32_t)(int32_t)-2);
        fprintf(stderr, "[KPAD] forced main-loop chan0 state -1 -> -2 (wiimote connected)\n");
        fflush(stderr);
    }
    /* TEMP(menu-input hunt): log every button edge the game reads, plus the
     * game's per-channel connection-state words at 0x80240870 (the input pump
     * discards a channel whose state word is 0). */
    if (hold != prev_hold) {
        fprintf(stderr, "[KPAD] hold 0x%04x -> 0x%04x (trig 0x%04x) chanstate=%d,%d,%d,%d assign=%08x %08x %08x %08x\n",
                prev_hold, hold, trig,
                (int32_t)MEM_R32(0x80240870u), (int32_t)MEM_R32(0x80240874u),
                (int32_t)MEM_R32(0x80240878u), (int32_t)MEM_R32(0x8024087Cu),
                MEM_R32(0x8023c790u), MEM_R32(0x8023c794u),
                MEM_R32(0x8023c798u), MEM_R32(0x8023c79Cu));
        fflush(stderr);
    }
    prev_hold = hold;

    p[0]=(hold>>24); p[1]=(hold>>16); p[2]=(hold>>8); p[3]=hold;
    p[4]=(trig>>24); p[5]=(trig>>16); p[6]=(trig>>8); p[7]=trig;
    /* IR pointer from the host mouse (sdk/video.c). KPADStatus:
     *   +0x20 Vec2 pos  — pointer in [-1,+1], +y down
     *   +0x28 Vec2 vec  — pointer velocity (per read)
     *   +0x5E s8 dpd_valid_fg — nonzero = pointer on screen */
    {
        extern void video_input_pointer(float *x, float *y, int *valid);
        static float s_px, s_py;
        float mx = 0, my = 0; int mvalid = 0;
        video_input_pointer(&mx, &my, &mvalid);
        if (mvalid) {
            MEM_WF(status_va + 0x20, mx);
            MEM_WF(status_va + 0x24, my);
            MEM_WF(status_va + 0x28, mx - s_px);
            MEM_WF(status_va + 0x2C, my - s_py);
            s_px = mx; s_py = my;
            p[0x5E] = 2;
        } else {
            p[0x5E] = 0;
        }
        /* Level (no-roll) orientation: the game rotates the hand cursor by
         * the wiimote roll derived from KPADStatus.horizon — (0,0) reads as
         * a sideways twist. A level wiimote is horizon=(1,0), acc=(0,0,1)G. */
        MEM_WF(status_va + 0x34, 1.0f);   /* horizon.x */
        MEM_WF(status_va + 0x38, 0.0f);   /* horizon.y */
        /* Wiimote accelerometer. Idle = level 1G. While the shake key (E) is
         * held, oscillate violently so every style of shake detector fires:
         * raw-axis swings, |acc| spikes (acc_value up to ~4G), and a large
         * acc_speed (the KPAD-computed derivative). Phase advances per read
         * so consecutive reads see the vector whip back and forth. */
        extern int video_input_shake(void);
        int shake = video_input_shake();
        float ph = (float)kpad_read_count * 1.7f;
        if (shake & 1) {
            float ax = sinf(ph) * 3.0f;
            float ay = cosf(ph * 0.73f) * 2.5f;
            float az = 1.0f + sinf(ph * 1.31f) * 2.0f;
            MEM_WF(status_va + 0x0C, ax);
            MEM_WF(status_va + 0x10, ay);
            MEM_WF(status_va + 0x14, az);
            MEM_WF(status_va + 0x18, sqrtf(ax*ax + ay*ay + az*az));
            MEM_WF(status_va + 0x1C, 6.0f);   /* acc_speed: huge derivative */
        } else {
            /* Tilt. On hardware you physically roll the Wiimote, which moves
             * gravity out of Z and into X -- the game has no "tilt button", it
             * just reads the accelerometer. So the tilt keys have to produce a
             * plausible ACCELERATION, not a flag: writing a level vector here
             * unconditionally is why binding tilt did nothing.
             *
             * ~30 degrees of roll. Far enough past any sane threshold to
             * register, still a reading a real hand could produce, and |acc|
             * stays 1 g so anything checking magnitude is unbothered. */
            extern int video_input_p1_tilt(void);
            const int   tilt = video_input_p1_tilt();   /* -1 left, +1 right */
            const float ax   = tilt * 0.50f;
            const float az   = tilt ? 0.87f : 1.0f;
            MEM_WF(status_va + 0x0C, ax);    /* acc.x */
            MEM_WF(status_va + 0x10, 0.0f);  /* acc.y */
            MEM_WF(status_va + 0x14, az);    /* acc.z (gravity reaction) */
            MEM_WF(status_va + 0x18, 1.0f);  /* acc_value = |acc| */
            MEM_WF(status_va + 0x1C, 0.0f);  /* acc_speed */
        }
        /* Nunchuk extension block (ex_status.fs): stick from WASD, its own
         * accelerometer with the same shake synthesis on the Q key.
         *   +0x60 Vec2 stick  (+x right, +y up, unit range)
         *   +0x68 Vec  acc    +0x74 f32 acc_value   +0x78 f32 acc_speed */
        {
            extern void video_input_stick(float *x, float *y);
            float sx = 0, sy = 0;
            video_input_stick(&sx, &sy);
            MEM_WF(status_va + 0x60, sx);
            MEM_WF(status_va + 0x64, sy);
            if (shake & 2) {
                float nx = sinf(ph * 0.91f) * 3.0f;
                float ny = cosf(ph * 1.13f) * 2.5f;
                float nz = 1.0f + sinf(ph * 0.67f) * 2.0f;
                MEM_WF(status_va + 0x68, nx);
                MEM_WF(status_va + 0x6C, ny);
                MEM_WF(status_va + 0x70, nz);
                MEM_WF(status_va + 0x74, sqrtf(nx*nx + ny*ny + nz*nz));
                MEM_WF(status_va + 0x78, 6.0f);
            } else {
                MEM_WF(status_va + 0x68, 0.0f);
                MEM_WF(status_va + 0x6C, 0.0f);
                MEM_WF(status_va + 0x70, 1.0f);
                MEM_WF(status_va + 0x74, 1.0f);
                MEM_WF(status_va + 0x78, 0.0f);
            }
        }
    }
    /* KPADStatus.dev_type @0x5C: 1 = KPAD_DEV_TYPE_FS (Nunchuk attached).
     * The boot flow REQUIRES the extension (IO_JoystickExtensionRequired) --
     * dev_type 0 parks the game on the "You need a Nunchuk" screen forever. */
    p[0x5C] = 1;
    p[0x5D] = 0;   /* wpad_err = KPAD_READ_OK */
    /* Pointer plausibility fields (menu-input hunt): a real wiimote aimed at
     * the screen reports a sensor-bar distance ~1m and a data format that
     * INCLUDES DPD data (WPAD_FMT_FS_ACC_DPD = 5). Left zero, a game that
     * validates either field treats the pointer as absent. */
    MEM_WF(status_va + 0x48, 1.0f);   /* dist */
    p[0x5F] = 5;                      /* data_format = FS_ACC_DPD */
    HLE_RET(1);
}

void hle_PADInit               (void) { HLE_RET(0); }
void hle_PADRead               (void) { HLE_RET(0); }
void hle_PADControlMotor       (void) { HLE_RET(0); }


// ===========================================================================
// DVD -- backed by the host "Assets/" directory.
//
// Nintendo's DVDFileInfo layout (ABI we must honor since the game knows it):
//   0x00 .. 0x1C  DVDCommandBlock (prev, next, command, state, offset, ...)
//   0x20          u32 startAddr  (on real HW = file start on disc)
//   0x24          u32 length     (file length)
//   0x28          DVDCBCallback transfer-done callback
//   0x2C          void *userData
// Total size 0x30. We reuse `startAddr` as our own handle table index so the
// guest struct stays opaque-compatible.
// ===========================================================================

#include <stdio.h>

#include "dvd_io.h"

struct dvd_handle_slot dvd_handles[DVD_MAX_HANDLES];

// Root directory under which guest DVD paths resolve. Resolved once from
// $RECOMP_ASSETS_DIR, else "./Assets", else "../Assets" (lets you run from
// build/ without copying). Cached so repeat opens don't re-probe.
static const char *dvd_assets_root(void) {
    static char cached[512];
    static int  resolved;
    if (resolved) return cached[0] ? cached : NULL;
    resolved = 1;

    const char *env = getenv("RECOMP_ASSETS_DIR");
    const char *candidates[3];
    int nc = 0;
    if (env && env[0]) candidates[nc++] = env;
    candidates[nc++] = "Assets";
    candidates[nc++] = "../Assets";

    for (int i = 0; i < nc; ++i) {
        // Probe for existence via a trial fopen of the directory isn't
        // portable; instead try opening a sentinel listing. We just pick
        // the first candidate whose directory appears usable by opening
        // *any* file under it. Cheap trick: if the dir literally exists
        // we get an error opening "candidate/" as a file (EISDIR/EACCES).
        // Good enough; worst case the first real fopen below fails and
        // the DVDOpen caller sees a miss.
        strncpy(cached, candidates[i], sizeof cached - 1);
        cached[sizeof cached - 1] = 0;
        // Don't verify -- first real use will tell us if the root is bad.
        fprintf(stderr, "[DVD] asset root = '%s' (env=%s)\n",
                cached, env ? env : "<unset>");
        fflush(stderr);
        return cached;
    }
    cached[0] = 0;
    return NULL;
}

// Resolve a guest filename to a host path under the asset root. The game
// passes paths like "media/default.tpl", "/Assets/media/default.tpl",
// "Assets/media/default.tpl", or Windows-style with backslashes; normalize
// all of them to a single "<root>/<normalized>" host path.
FILE *dvd_host_open(const char *guest_name, uint32_t *out_len) {
    if (!guest_name) return NULL;
    const char *name = guest_name;
    while (*name == '/' || *name == '\\') ++name;

    // Strip optional "Assets/" prefix (case insensitive, ASCII) -- the
    // guest sometimes includes it, sometimes doesn't.
    {
        const char *p = name;
        static const char assets_pfx[] = "assets/";
        int i = 0;
        while (i < 7 && p[i] &&
               ((p[i] == assets_pfx[i]) ||
                (p[i] >= 'A' && p[i] <= 'Z' && (p[i] + 32) == assets_pfx[i]) ||
                (p[i] == '\\' && assets_pfx[i] == '/'))) ++i;
        if (i == 7) name = p + 7;
    }

    // Try every plausible assets root -- when the binary runs from build/,
    // "../Assets" is the real path; when run from the project root it's
    // "Assets/"; when RECOMP_ASSETS_DIR is set it wins.
    const char *env = getenv("RECOMP_ASSETS_DIR");
    const char *roots[4] = {0};
    int nr = 0;
    if (env && env[0]) roots[nr++] = env;
    roots[nr++] = "Assets";
    roots[nr++] = "../Assets";
    roots[nr++] = ".";   // last-ditch: maybe file is in cwd

    char hostpath[1024];
    FILE *fp = NULL;
    for (int i = 0; i < nr && !fp; ++i) {
        snprintf(hostpath, sizeof hostpath, "%s/%s", roots[i], name);
        for (char *q = hostpath; *q; ++q) if (*q == '\\') *q = '/';
        fp = fopen(hostpath, "rb");
    }
    if (!fp) {
        fp = fopen(guest_name, "rb");
        if (!fp) return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    fseek(fp, 0, SEEK_SET);
    if (out_len) *out_len = (uint32_t)sz;
    return fp;
}

int dvd_alloc_handle(FILE *fp, uint32_t len) {
    for (int i = 1; i < DVD_MAX_HANDLES; ++i) {
        if (!dvd_handles[i].in_use) {
            dvd_handles[i].fp = fp;
            dvd_handles[i].length = len;
            dvd_handles[i].in_use = 1;
            return i;
        }
    }
    return 0;
}

void hle_DVDInit               (void) { HLE_RET(0); }  // DVD handled via host file I/O
void hle_DVDInquiryAsync       (void) { HLE_RET(1); }  // returns 1 = inquiry queued

// DVDGetFSTLocation returns the VA where the Wii FST lives. Games sanity-
// check this against MEM1's tail: "FST must fit in the last ~800 KB of
// MEM1" (otherwise the FST was loaded somewhere bogus). We return an
// address in that tail window and pre-zero the region so any FST walk
// sees an empty directory.
//
// MEM1 ends at 0x81800000 (mirrored) / 0x01800000 (physical). We park the
// FST at 0x817FF000, giving the game exactly 0x1000 bytes of headroom.
// Pre-zero from runtime_init; see ppc_runtime_init() -> this address is
// already zeroed because all of g_mem1 starts zeroed.
#define FAKE_FST_VA     0x817FF000u
#define FAKE_FST_SIZE   0x400u          // 1 KB of "FST data" (all zeros = no entries)

void hle_DVDGetFSTLocation(void) { HLE_RET(FAKE_FST_VA); }
void hle_DVDGetFSTSize    (void) { HLE_RET(FAKE_FST_SIZE); }
void hle_DVDGetFSTEntryNum(void) { HLE_RET(0); }         // 0 files in root

// DVD entry table (AC PC port shape): register any path the game asks
// about, return its index. DVDFastOpen below uses the index to look up
// the path and open via the host filesystem.
//
// Rationale: the GC/Wii FST walker is messy to emulate when we don't have
// the disc image. Skip FST entirely -- the path string is the canonical
// identity and we just keep a lookup table.
#define DVD_MAX_ENTRIES 2048
static struct {
    char path[256];
    int  used;
} g_dvd_entries[DVD_MAX_ENTRIES];
static int g_dvd_entry_count;

static int dvd_register_path(const char *path) {
    if (!path) return -1;
    for (int i = 0; i < g_dvd_entry_count; ++i) {
        if (g_dvd_entries[i].used && strcmp(g_dvd_entries[i].path, path) == 0) {
            return i;
        }
    }
    if (g_dvd_entry_count >= DVD_MAX_ENTRIES) return -1;
    int idx = g_dvd_entry_count++;
    strncpy(g_dvd_entries[idx].path, path, sizeof g_dvd_entries[idx].path - 1);
    g_dvd_entries[idx].path[sizeof g_dvd_entries[idx].path - 1] = '\0';
    g_dvd_entries[idx].used = 1;
    return idx;
}

void hle_DVDConvertPathToEntrynum(void) {
    uint32_t path_va = HLE_ARG_U32(0);
    const char *path = path_va ? (const char *)ppc_host_ptr(path_va) : NULL;
    int idx = dvd_register_path(path);
    static int log_n;
    if (log_n < 20) {
        fprintf(stderr, "[DVD] ConvertPath '%s' -> %d\n",
                path ? path : "(null)", idx);
        fflush(stderr); log_n++;
    }
    HLE_RET((uint32_t)idx);
}

// DVDFastOpen(entrynum, DVDFileInfo*) -> BOOL
// DVDFileInfo layout (libogc / SDK):
//   0x00..0x13  DVDCommandBlock (we tuck a host slot index at +0x00)
//   0x14        cb: DVDCallback (0)
//   0x18        fileInfo.fp marker  (we stash magic DVD_HOST_TAG)
//   0x1C..0x2F  misc
//   0x30        startAddr  (we use 0; reads are offset-based into handle)
//   0x34        length
//
// We don't own the 60-byte struct format strictly; we store:
//   +0x00 = host big_handles[] index
//   +0x34 = length
void hle_DVDFastOpen(void) {
    int32_t  entry  = (int32_t)HLE_ARG_U32(0);
    uint32_t fi_va  = HLE_ARG_U32(1);
    if (entry < 0 || entry >= g_dvd_entry_count || !g_dvd_entries[entry].used) {
        HLE_RET(0); return;
    }
    if (!fi_va) { HLE_RET(0); return; }
    const char *path = g_dvd_entries[entry].path;
    uint32_t length = 0;
    FILE *fp = dvd_host_open(path, &length);
    if (!fp) {
        static int miss_n;
        if (miss_n < 20) {
            fprintf(stderr, "[DVD] FastOpen miss entry=%d '%s'\n", entry, path);
            fflush(stderr); miss_n++;
        }
        HLE_RET(0); return;
    }
    int h = dvd_alloc_handle(fp, length);
    if (!h) { fclose(fp); HLE_RET(0); return; }
    // libogc DVDFileInfo layout: cb (32 bytes) at 0x00, startAddr at 0x20,
    // length at 0x24. Match hle_DVDOpen -- stash handle in startAddr so
    // the existing DVDRead path picks it up unchanged.
    uint8_t *p = (uint8_t *)ppc_host_ptr(fi_va);
    if (p) for (int i = 0; i < 0x2C; ++i) p[i] = 0;
    MEM_W32(fi_va + 0x20, (uint32_t)h);
    MEM_W32(fi_va + 0x24, length);
    int slot = h;
    static int log_n;
    if (log_n < 20) {
        fprintf(stderr, "[DVD] FastOpen entry=%d '%s' slot=%d len=%u\n",
                entry, path, slot, length);
        fflush(stderr); log_n++;
    }
    HLE_RET(1);
}


// Engine-specific HLE (ARC stubs, HomeButton, asset containers,
// std::IO, module loader, handler-list shortcuts) lives under quirks/.




// ===========================================================================
// EXI / HIO2 stubs.
//
// The EXternal Interface is Wii's debug / memcard bus. HIO2 is the dev-host
// I/O link used only on debug consoles. On a retail console these return
// "nothing connected" and the game skips its debug logging. We do the same.
// ===========================================================================

void hle_EXIInit              (void) { HLE_RET(0); }
void hle_EXIWait              (void) { HLE_RET(0); }   // never spin
void hle_EXIGetConsoleType    (void) { HLE_RET(0x00000003); }   // OS_CONSOLE_RETAIL3
void hle_EXIProbeEx           (void) { HLE_RET(0); }
void hle_EXIAttach            (void) { HLE_RET(0); }
void hle_EXIDetach            (void) { HLE_RET(0); }
void hle_EXISelect            (void) { HLE_RET(0); }
void hle_EXIDeselect          (void) { HLE_RET(0); }
void hle_EXILock              (void) { HLE_RET(1); }
void hle_EXIUnlock            (void) { HLE_RET(1); }
void hle_EXIImm               (void) { HLE_RET(0); }
void hle_EXIImmEx             (void) { HLE_RET(0); }
void hle_EXIDma               (void) { HLE_RET(0); }
void hle_EXISync              (void) { HLE_RET(0); }
void hle_EXIGetID             (void) { HLE_RET(0); }
void hle_EXISetExiCallback    (void) { HLE_RET(0); }

void hle_HIO2Init             (void) { HLE_RET(0); }   // 0 = not connected
void hle_HIO2EnumDevices      (void) { HLE_RET(0); }
void hle_HIO2Open             (void) { HLE_RET(0); }
void hle_HIO2Close            (void) { HLE_RET(0); }
void hle_HIO2ReadMailbox      (void) { HLE_RET(0); }
void hle_HIO2WriteMailbox     (void) { HLE_RET(0); }
void hle_HIO2Read             (void) { HLE_RET(0); }
void hle_HIO2Write            (void) { HLE_RET(0); }
void hle_HIO2ReadStatus       (void) { HLE_RET(0); }

// DVDOpen(const char *filename, DVDFileInfo *fi) -> BOOL (1 = ok).
void hle_DVDOpen(void) {
    uint32_t name_va = HLE_ARG_U32(0);
    uint32_t fi_va   = HLE_ARG_U32(1);
    if (!name_va || !fi_va) { HLE_RET(0); return; }
    const char *name = (const char *)ppc_host_ptr(name_va);
    uint32_t len = 0;
    FILE *fp = dvd_host_open(name, &len);
    if (!fp) {
        static int miss;
        if (miss < 20) {
            fprintf(stderr, "[DVD] miss: '%s'\n", name);
            fflush(stderr); miss++;
        }
        HLE_RET(0); return;
    }
    int h = dvd_alloc_handle(fp, len);
    if (!h) { fclose(fp); HLE_RET(0); return; }
    // Fill DVDFileInfo fields the game cares about. Store our handle in
    // the startAddr slot (real HW's on-disc start) and the length at 0x24.
    MEM_W32(fi_va + 0x20, (uint32_t)h);
    MEM_W32(fi_va + 0x24, len);
    static int hit;
    if (hit < 30) {
        fprintf(stderr, "[DVD] open '%s' -> h=%d len=%u\n", name, h, len);
        fflush(stderr); hit++;
    }
    HLE_RET(1);
}

void hle_DVDClose(void) {
    uint32_t fi_va = HLE_ARG_U32(0);
    if (!fi_va) { HLE_RET(1); return; }
    uint32_t h = MEM_R32(fi_va + 0x20);
    if (h > 0 && h < DVD_MAX_HANDLES && dvd_handles[h].in_use) {
        fclose(dvd_handles[h].fp);
        dvd_handles[h].in_use = 0;
        dvd_handles[h].fp = NULL;
    }
    HLE_RET(1);
}

// Shared synchronous implementation for DVDRead / DVDReadPrio / async.
// Signature: (DVDFileInfo *fi, void *dst, s32 len, s32 offset[, prio])
//            -> s32 bytes read (or -1 on error).
static int32_t dvd_read_sync(uint32_t fi_va, uint32_t dst_va, int32_t len, int32_t off) {
    if (!fi_va || len <= 0) return 0;
    uint32_t h = MEM_R32(fi_va + 0x20);
    if (h == 0 || h >= DVD_MAX_HANDLES || !dvd_handles[h].in_use) return -1;
    FILE *fp = dvd_handles[h].fp;
    fseek(fp, off, SEEK_SET);
    uint8_t *dst = (uint8_t *)ppc_host_ptr(dst_va);
    if (!dst) return -1;
    size_t got = fread(dst, 1, (size_t)len, fp);
    return (int32_t)got;
}

void hle_DVDRead(void) {
    uint32_t fi  = HLE_ARG_U32(0);
    uint32_t dst = HLE_ARG_U32(1);
    int32_t  len = HLE_ARG_S32(2);
    int32_t  off = HLE_ARG_S32(3);
    HLE_RET((uint32_t)dvd_read_sync(fi, dst, len, off));
}

// ReadAsync / ReadAsyncPrio: (fi, dst, len, off, callback[, prio]).
// Real HW queues and fires the callback on completion. We do it
// synchronously, then invoke the callback as if it were synchronous --
// except that requires calling guest code, which we don't do from HLE
// (no re-entry). So we fire the callback on the NEXT VIWaitForRetrace
// instead, via a small pending-callback queue.
#define DVD_CB_QUEUE 16
static struct { uint32_t cb_va; int32_t ret; uint32_t fi_va; } dvd_cb_queue[DVD_CB_QUEUE];
static int dvd_cb_count = 0;

static void dvd_queue_callback(uint32_t fi_va, uint32_t cb_va, int32_t ret) {
    if (!cb_va) return;
    if (dvd_cb_count < DVD_CB_QUEUE) {
        dvd_cb_queue[dvd_cb_count].cb_va = cb_va;
        dvd_cb_queue[dvd_cb_count].ret = ret;
        dvd_cb_queue[dvd_cb_count].fi_va = fi_va;
        dvd_cb_count++;
    }
}

// Drain pending DVD callbacks. Declared here; called from VIWaitForRetrace
// as a convenient synchronous pump point.
void dvd_pump_callbacks_hook(void) {
    for (int i = 0; i < dvd_cb_count; ++i) {
        uint32_t cb = dvd_cb_queue[i].cb_va;
        g_cpu.gpr[3] = (uint32_t)dvd_cb_queue[i].ret;
        g_cpu.gpr[4] = dvd_cb_queue[i].fi_va;
        ppc_call_indirect(cb);
    }
    dvd_cb_count = 0;
}

void hle_DVDReadAsync(void) {
    uint32_t fi  = HLE_ARG_U32(0);
    uint32_t dst = HLE_ARG_U32(1);
    int32_t  len = HLE_ARG_S32(2);
    int32_t  off = HLE_ARG_S32(3);
    uint32_t cb  = HLE_ARG_U32(4);
    int32_t rv = dvd_read_sync(fi, dst, len, off);
    dvd_queue_callback(fi, cb, rv);
    HLE_RET(1);
}

void hle_DVDReadAsyncPrio(void) {
    uint32_t fi  = HLE_ARG_U32(0);
    uint32_t dst = HLE_ARG_U32(1);
    int32_t  len = HLE_ARG_S32(2);
    int32_t  off = HLE_ARG_S32(3);
    uint32_t cb  = HLE_ARG_U32(4);
    // prio at arg 5 is ignored
    int32_t rv = dvd_read_sync(fi, dst, len, off);
    dvd_queue_callback(fi, cb, rv);
    HLE_RET(1);
}

void hle_DVDGetCommandBlockStatus(void) { HLE_RET(0); }
void hle_DVDGetFileInfoStatus    (void) { HLE_RET(0); }


// ===========================================================================
// Per-game asset loaders live in quirks/ (this game: quirks/lyn_big.c). They
// consume the DVD handle table and dvd_host_open() / dvd_alloc_handle()
// helpers exposed via sdk/dvd_io.h.
// ===========================================================================

void hle_ISFS_Open             (void) { HLE_RET(-1); }
void hle_ISFS_Read             (void) { HLE_RET(-1); }
void hle_ISFS_Close            (void) { HLE_RET(0); }


// ===========================================================================
// IPC / IOS -- Inter-Processor Comms with the "starlet" ARM.
//
// Most game init paths open at least one IOS device. We track which
// devices we hand out fake FDs for so subsequent Read/Ioctl calls
// can recognize them. Returning -1 from IOS_Open works for some games
// but breaks others that gate-check the FD and bail if it's negative.
//
// FD allocation: small positive integers, with a 1:1 map back to the
// device path so handlers like hle_IOS_Ioctl can dispatch per-device.
// ===========================================================================

#define IOS_MAX_FDS 32

typedef enum {
    IOS_DEV_NONE = 0,
    IOS_DEV_STM_IMMEDIATE,    // /dev/stm/immediate     -- power, dimming, etc.
    IOS_DEV_STM_EVENTHOOK,    // /dev/stm/eventhook     -- power button events
    IOS_DEV_FS,               // /dev/fs                -- NAND filesystem
    IOS_DEV_ES,               // /dev/es                -- title management
    IOS_DEV_SHA,              // /dev/sha               -- SHA-1 / HMAC
    IOS_DEV_DI,               // /dev/di                -- disc interface
    IOS_DEV_OTHER,            // anything else we recognize the path of
} IosDevKind;

static IosDevKind g_ios_fds[IOS_MAX_FDS];

void hle_IPCCltInit            (void) { HLE_RET(0); }  // IPC not needed (no IOS)
void hle_IPCiProfQueueReq      (void) { HLE_RET(0); }

void hle_IOS_Open(void) {
    uint32_t path_va = HLE_ARG_U32(0);
    if (!path_va) { HLE_RET(-1); return; }
    const char *path = (const char *)ppc_host_ptr(path_va);
    if (!path) { HLE_RET(-1); return; }

    IosDevKind kind = IOS_DEV_NONE;
    if      (!strcmp(path, "/dev/stm/immediate")) kind = IOS_DEV_STM_IMMEDIATE;
    else if (!strcmp(path, "/dev/stm/eventhook")) kind = IOS_DEV_STM_EVENTHOOK;
    else if (!strcmp(path, "/dev/fs"))            kind = IOS_DEV_FS;
    else if (!strcmp(path, "/dev/es"))            kind = IOS_DEV_ES;
    else if (!strcmp(path, "/dev/sha"))           kind = IOS_DEV_SHA;
    else if (!strcmp(path, "/dev/di"))            kind = IOS_DEV_DI;
    else if (!strncmp(path, "/dev/", 5))          kind = IOS_DEV_OTHER;

    if (kind == IOS_DEV_NONE) {
        static int unknown_count;
        if (unknown_count++ < 20) {
            fprintf(stderr, "[IOS] Open '%s' -> -1 (unrecognized)\n", path);
            fflush(stderr);
        }
        HLE_RET((uint32_t)-1);
        return;
    }

    // Allocate first free FD (skip 0, since 0 is a common "no fd" sentinel).
    for (int fd = 1; fd < IOS_MAX_FDS; ++fd) {
        if (g_ios_fds[fd] == IOS_DEV_NONE) {
            g_ios_fds[fd] = kind;
            static int open_count;
            if (open_count++ < 30) {
                fprintf(stderr, "[IOS] Open '%s' -> fd=%d kind=%d\n", path, fd, kind);
                fflush(stderr);
            }
            HLE_RET((uint32_t)fd);
            return;
        }
    }

    fprintf(stderr, "[IOS] Open '%s' -> -1 (FD table full)\n", path);
    HLE_RET((uint32_t)-1);
}

void hle_IOS_Close(void) {
    int32_t fd = HLE_ARG_S32(0);
    if (fd > 0 && fd < IOS_MAX_FDS) g_ios_fds[fd] = IOS_DEV_NONE;
    HLE_RET(0);
}

void hle_IOS_Read(void) {
    int32_t fd = HLE_ARG_S32(0);
    if (fd <= 0 || fd >= IOS_MAX_FDS || g_ios_fds[fd] == IOS_DEV_NONE) {
        HLE_RET((uint32_t)-1);
        return;
    }
    // Most read targets we care about (STM, ES, SHA) deliver via Ioctl,
    // not Read. Returning 0 ("0 bytes read") tells the caller "no data
    // available" without indicating an error -- safer than -1 for games
    // that retry on error.
    HLE_RET(0);
}

void hle_IOS_Write(void) {
    int32_t fd = HLE_ARG_S32(0);
    int32_t len = HLE_ARG_S32(2);
    if (fd <= 0 || fd >= IOS_MAX_FDS || g_ios_fds[fd] == IOS_DEV_NONE) {
        HLE_RET((uint32_t)-1);
        return;
    }
    // Pretend the write succeeded.
    HLE_RET((uint32_t)len);
}

// STM ioctls we care about (per WiiBrew /dev/stm pages):
//   0x1003 SetIdleLedMode
//   0x1004 SetDimmingState
//   0x2001 RegisterEvent (only on /dev/stm/eventhook)
//   0x3001 ReleaseEvent
//   0x3002 ShutdownToIdle
//   0x3003 PowerOff
// ES ioctls: many. We only special-case GetTitleID for now.
// SHA ioctls: 0=Init, 1=Contribute, 2=Final.
void hle_IOS_Ioctl(void) {
    int32_t  fd     = HLE_ARG_S32(0);
    uint32_t cmd    = HLE_ARG_U32(1);
    if (fd <= 0 || fd >= IOS_MAX_FDS) { HLE_RET((uint32_t)-1); return; }
    IosDevKind kind = g_ios_fds[fd];

    switch (kind) {
    case IOS_DEV_STM_IMMEDIATE:
    case IOS_DEV_STM_EVENTHOOK:
        // STM commands are mostly fire-and-forget. PowerOff/Reset would
        // shut us down; we handle those explicitly.
        if (cmd == 0x3003u) {        // STM_PowerOff
            fprintf(stderr, "[STM] PowerOff requested -- exiting\n");
            exit(0);
        }
        if (cmd == 0x3002u) {        // STM_ShutdownToIdle
            fprintf(stderr, "[STM] ShutdownToIdle requested -- exiting\n");
            exit(0);
        }
        // Everything else: pretend success and consume.
        HLE_RET(0);
        return;
    case IOS_DEV_SHA:
        // SHA Init/Contribute/Final all stubbed to success. Without a
        // real SHA implementation, callers that *check* the hash will
        // mismatch -- but most callers just need it to return 0.
        HLE_RET(0);
        return;
    case IOS_DEV_ES:
        // Title management. Almost everything: success-but-empty.
        // ES_GetTitleID would normally fill the output buffer with the
        // running title's ID; we leave it zero, which tells callers
        // "not running from a launched title" -- matches our reality.
        HLE_RET(0);
        return;
    case IOS_DEV_FS:
    case IOS_DEV_DI:
    case IOS_DEV_OTHER:
        HLE_RET(0);
        return;
    default:
        HLE_RET((uint32_t)-1);
        return;
    }
}

void hle_IOS_Ioctlv(void) {
    int32_t  fd  = HLE_ARG_S32(0);
    if (fd <= 0 || fd >= IOS_MAX_FDS || g_ios_fds[fd] == IOS_DEV_NONE) {
        HLE_RET((uint32_t)-1);
        return;
    }
    // Same logic as Ioctl: pretend success. Real ioctlv handling would
    // need to interpret the iovec array, which we don't.
    HLE_RET(0);
}


// ===========================================================================
// SC -- System Config (NAND-stored user prefs: language, aspect ratio, ...).
// ===========================================================================

void hle_SCInit                (void) { HLE_RET(0); }  // SC defaults provided by SCGetLanguage etc.
void hle_SCCheckStatus         (void) { HLE_RET(0); }    // 0 = ready
void hle_SCGetLanguage         (void) { HLE_RET(1); }    // 1 = English
/* 0 = 4:3, 1 = 16:9. Defaults to 16:9 — it's a Wii recomp, we want widescreen.
 * The game then composes screens (e.g. the health/safety strap screen) for a
 * widescreen frame instead of cropping the 16:9 art to 4:3, and gx_ogl_present
 * un-squishes the anamorphic EFB to 16:9. Set RECOMP_WIDESCREEN=0 to force 4:3. */
int recomp_widescreen(void) {
    static int ws = -1;
    if (ws < 0) {
        const char *e = getenv("RECOMP_WIDESCREEN");
        ws = (e && e[0]) ? (e[0] != '0') : 1;   /* default ON */
    }
    return ws;
}
void hle_SCGetAspectRatio      (void) {
    int ar = recomp_widescreen() ? 1 : 0;
    static int n;
    if (n < 4) { ++n;
        fprintf(stderr, "[SC] GetAspectRatio -> %d (%s) lr=0x%08x\n",
                ar, ar ? "16:9" : "4:3", g_cpu.lr);
        fflush(stderr);
    }
    HLE_RET(ar);
}
void hle_SCGetEuRgb60Mode      (void) { HLE_RET(0); }


// (EXI stubs live earlier in this file — dedup removed.)
void hle_EXIProbe              (void) { HLE_RET(0); }
void hle_SIInit                (void) { HLE_RET(0); }


// ===========================================================================
// Miscellaneous one-offs we've seen in Wii boot paths.
// ===========================================================================

void hle___OSBootDolphin       (void) { HLE_RET(0); }
void hle___OSInitSystemCall    (void) { HLE_RET(0); }
void hle___OSInitAlarm         (void) { HLE_RET(0); }
void hle___OSContextInit       (void) { HLE_RET(0); }
void hle___OSInterruptInit     (void) { HLE_RET(0); }
void hle___OSExceptionInit     (void) { HLE_RET(0); }
void hle___OSModuleInit        (void) { HLE_RET(0); }
void hle___OSInitIPCBuffer     (void) { HLE_RET(0); }
void hle___OSInitMemoryProtection(void) { HLE_RET(0); }
void hle___OSInitPlayTime      (void) { HLE_RET(0); }
void hle___OSInitNet           (void) { HLE_RET(0); }
void hle___OSInitSram          (void) { HLE_RET(0); }
void hle___OSInitSTM           (void) { HLE_RET(0); }
void hle___OSThreadInit        (void) { HLE_RET(0); }
void hle___OSInitAudioSystem   (void) { HLE_RET(0); }

// ===========================================================================
// Async IOS variants. The sync IOS_* are HLE'd above, but the SDK's NAND
// layer uses the *Async forms whose recompiled bodies enqueue IPC requests
// to hardware that doesn't exist -- completions never arrive, so NAND async
// callbacks never fire and GST_SaveManager_State_WAIT gates the whole boot
// sequence (autosave notice -> logos -> menu) forever.
//
// Each wrap runs the sync HLE for the real result, then queues the IPC
// callback; the queue drains from vi_pump_retraces (next "interrupt"), which
// keeps SDK reentrancy assumptions intact.
// ===========================================================================
typedef struct { uint32_t cb, result, userdata; } IosAsyncDone;
static IosAsyncDone s_ios_done_q[32];
static int s_ios_done_n;

static void ios_async_finish2(const char *tag, uint32_t cb, uint32_t result, uint32_t userdata) {
    static int s_n;
    if (s_n++ < 24) {
        fprintf(stderr, "[IOS-ASYNC] %s cb=0x%08x result=%d ud=0x%08x\n",
                tag, cb, (int)result, userdata);
        fflush(stderr);
    }
    if (!cb) return;
    if (s_ios_done_n < (int)(sizeof s_ios_done_q / sizeof s_ios_done_q[0])) {
        s_ios_done_q[s_ios_done_n].cb = cb;
        s_ios_done_q[s_ios_done_n].result = result;
        s_ios_done_q[s_ios_done_n].userdata = userdata;
        s_ios_done_n++;
    }
}

void ios_async_pump(void) {
    /* fire queued IPC completions (called from vi_pump_retraces) */
    int n = s_ios_done_n;
    s_ios_done_n = 0;
    for (int i = 0; i < n; ++i) {
        g_cpu.gpr[3] = s_ios_done_q[i].result;
        g_cpu.gpr[4] = s_ios_done_q[i].userdata;
        ppc_call_indirect(s_ios_done_q[i].cb);
    }
}

extern void __real_func_80405130(void);
void __wrap_func_80405130(void) {   /* IOS_OpenAsync(path, mode, cb, ud) */
    uint32_t cb = HLE_ARG_U32(2), ud = HLE_ARG_U32(3);
    hle_IOS_Open();
    ios_async_finish2("open", cb, g_cpu.gpr[3], ud);
    HLE_RET(0);
}
extern void __real_func_80405380(void);
void __wrap_func_80405380(void) {   /* IOS_CloseAsync(fd, cb, ud) */
    uint32_t cb = HLE_ARG_U32(1), ud = HLE_ARG_U32(2);
    hle_IOS_Close();
    ios_async_finish2("close", cb, g_cpu.gpr[3], ud);
    HLE_RET(0);
}
extern void __real_func_804054f0(void);
void __wrap_func_804054f0(void) {   /* IOS_ReadAsync(fd, buf, len, cb, ud) */
    uint32_t cb = HLE_ARG_U32(3), ud = HLE_ARG_U32(4);
    hle_IOS_Read();
    ios_async_finish2("read", cb, g_cpu.gpr[3], ud);
    HLE_RET(0);
}
extern void __real_func_80405700(void);
void __wrap_func_80405700(void) {   /* IOS_WriteAsync(fd, buf, len, cb, ud) */
    uint32_t cb = HLE_ARG_U32(3), ud = HLE_ARG_U32(4);
    hle_IOS_Write();
    ios_async_finish2("write", cb, g_cpu.gpr[3], ud);
    HLE_RET(0);
}
extern void __real_func_80405910(void);
void __wrap_func_80405910(void) {   /* IOS_SeekAsync(fd, where, whence, cb, ud) */
    uint32_t cb = HLE_ARG_U32(3), ud = HLE_ARG_U32(4);
    ios_async_finish2("seek", cb, 0, ud);
    HLE_RET(0);
}
extern void __real_func_80405ae0(void);
void __wrap_func_80405ae0(void) {   /* IOS_IoctlAsync(fd,cmd,in,il,out,ol,cb,ud) */
    int32_t  fd = HLE_ARG_S32(0);
    uint32_t cb = HLE_ARG_U32(6), ud = HLE_ARG_U32(7);
    /* STM eventhook semantics: the ioctl PARKS until a power/reset event.
     * Completing it (even with an error) makes the OS re-register in a
     * spin and OSPanic("Error on STM state event handler"). Park it like
     * real hardware. Match by fd kind AND by the eventhook command 0x1000
     * (the game holds fd 0, outside our tracking). */
    uint32_t cmd_ = HLE_ARG_U32(1);
    if (cmd_ == 0x1000u ||
        (fd > 0 && fd < IOS_MAX_FDS && g_ios_fds[fd] == IOS_DEV_STM_EVENTHOOK)) {
        HLE_RET(0);
        return;
    }
    hle_IOS_Ioctl();
    if ((int32_t)g_cpu.gpr[3] < 0) {
        static int s_n;
        if (s_n++ < 8) {
            fprintf(stderr, "[IOS-ASYNC] ioctl FAIL fd=%d kind=%d cmd=0x%x\n",
                    fd, (fd > 0 && fd < IOS_MAX_FDS) ? (int)g_ios_fds[fd] : -99,
                    HLE_ARG_U32(1));
            fflush(stderr);
        }
    }
    ios_async_finish2("ioctl", cb, g_cpu.gpr[3], ud);
    HLE_RET(0);
}
extern void __real_func_80405e90(void);
void __wrap_func_80405e90(void) {   /* IOS_IoctlvAsync(fd,cmd,ic,oc,vec,cb,ud) */
    uint32_t cb = HLE_ARG_U32(5), ud = HLE_ARG_U32(6);
    hle_IOS_Ioctlv();
    ios_async_finish2("ioctlv", cb, g_cpu.gpr[3], ud);
    HLE_RET(0);
}
