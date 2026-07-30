/* sdk/gx_ogl.h -- OpenGL GX renderer public interface.
 *
 * This replaces the software rasterizer (raster_primitive / soft_efb) with
 * a real OpenGL 3.3 core-profile pipeline.  The GX FIFO decoder in
 * peripherals.c calls into this API directly after parsing each draw call.
 */
#pragma once
#ifndef GX_OGL_H
#define GX_OGL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ---- initialisation ---------------------------------------------------- */

/* Call once after the SDL GL window/context is created. */
void gx_ogl_init(void);

/* 1 if the GL renderer is up and ready, 0 if init failed (fall back to SW). */
int gx_ogl_ready(void);

/* Letterbox rect the EFB was last blitted into, in window pixels. The input
 * layer needs it to convert a mouse/touch coordinate into a game coordinate:
 * the game occupies only this sub-rectangle of the window, with black bars
 * around it. Zero width/height means nothing has been presented yet. */
void gx_ogl_get_present_rect(int *x, int *y, int *w, int *h);

/* ---- GX state pushed from the FIFO decoder ----------------------------- */

/* XF matrices: modelview (pos_mtx[slot][0..11]) and projection (proj[0..6])
 * Both are in the same layout as the XF memory on hardware.
 * pos_mtx is 3x4 row-major.  proj_compact is 6 floats + format flag. */
void gx_ogl_set_pos_matrix(int slot, const float row0[4], const float row1[4],
                             const float row2[4]);
void gx_ogl_set_proj(const float proj_compact[7]);  /* [0..5] coefs, [6] ortho flag */

/* CP vertex format (VAT / VCD) as raw register values (same as gp_vat_g0 etc.) */
void gx_ogl_set_cp_vtxdesc(uint32_t lo, uint32_t hi);
void gx_ogl_set_cp_vat(int slot, uint32_t g0, uint32_t g1, uint32_t g2);
void gx_ogl_set_cp_array(int attr, uint32_t base, uint32_t stride);

/* BP registers forwarded from the FIFO decoder */
void gx_ogl_bp_write(uint32_t reg, uint32_t val);

/* Texture object (from HLE GXInitTexObj / GXLoadTexObj).
 * guest_va = start of texture data in guest memory.
 * fmt      = GX TextureFormat (0=I4 .. 14=CMPR).
 * wrap_s/t = 0=clamp, 1=repeat, 2=mirror. */
void gx_ogl_set_tex(int slot, uint32_t guest_va, uint32_t w, uint32_t h,
                    uint32_t fmt, uint32_t wrap_s, uint32_t wrap_t);

/* ---- draw calls -------------------------------------------------------- */

/* Submit one decoded primitive.  `verts` is an array of n_verts packed
 * entries; each entry is:
 *   float x, y, z   (post-XF position in guest clip-space)
 *   uint8_t r,g,b,a (vertex colour, host byte order = ABGR as uint32)
 *   float s, t       (tex-coord for texmap 0, in 0..1 range)
 *   uint8_t mtx_idx  (position matrix index / 3, same as PPC register)
 *
 * prim_type mirrors GX_QUADS(0x80) / GX_TRIANGLES(0x90) / etc.
 */

typedef struct {
    float    x, y, z;
    uint32_t color;   /* ARGB8 big-endian from hardware, stored ABGR host */
    float    s, t;
    uint8_t  mtx_idx;
} GxOglVertex;

void gx_ogl_draw(uint32_t prim_type, const GxOglVertex *verts, int n_verts);

/* ---- player sprite capture (robox_mario.c) ----------------------------- */
/* Between begin/end, every submitted vertex is also projected on the CPU and
 * accumulated into a screen bounding box; with `suppress`, those draws are
 * swallowed entirely (the character becomes invisible while its screen
 * placement is still learned). The quad is latched for a few frames and
 * returned in the overlay's virtual 1280x720 space, ready for
 * gx_ogl_overlay_rect. */
void gx_ogl_player_capture_begin(int suppress);
void gx_ogl_player_capture_end(void);
int  gx_ogl_player_quad(float *x, float *y, float *w, float *h);

/* SMB-style camera assist: feed the on-screen error (overlay-space px scaled
 * to NDC by the caller) between the player and where the camera should hold
 * him; an eased, clamped view shift is applied to world draws only. Decays
 * automatically on frames with no feed. */
void gx_ogl_cam_assist(float err_x_ndc, float err_y_ndc);

/* Player quad in game NDC (with the cosmetic assist shift removed), for
 * reasoning about the game's own camera. Returns 0 when stale. */
int gx_ogl_player_quad_ndc(float *cx, float *cy, float *w, float *h);

/* ---- EFB copy / frame present ----------------------------------------- */

/* Called when BP reg 0x52 triggers an EFB copy.  For screen copies
 * (is_xfb != 0) this blits the current GL framebuffer to the screen. */
void gx_ogl_efb_copy(int is_xfb, uint32_t dst_va, uint32_t w, uint32_t h,
                      uint32_t dst_stride, uint32_t clear_color_argb);

/* Pump SDL events, swap buffers, present the frame. */
void gx_ogl_present(void);

/* Publish the rendered frame. Wraps SDL_GL_SwapWindow, except on Emscripten
 * where the context uses explicitSwapControl and must be committed by hand. */
struct SDL_Window;
void robox_gl_swap(struct SDL_Window *w);

/* ---------------------------------------------------------------------------
 * On-screen touch controls.
 *
 * video.c owns the layout and the live pressed/deflection state; gx_ogl.c draws
 * it. Both sides work from the SAME numbers, so what is drawn is exactly what
 * the hit-test responds to -- the overlay can never drift out of alignment with
 * the controls it depicts.
 *
 * Coordinates are fractions of the window: cx of width, cy of height, r of
 * min(width, height). That keeps the layout correct on any screen shape.
 * ------------------------------------------------------------------------- */
typedef struct {
    float       cx, cy, r;
    int         pressed;
    const char *label;      /* NULL for the thumbstick base/knob */
} RoboxTouchCircle;

/* Fills `out` with the buttons, plus the thumbstick base and knob when a finger
 * is on it. Returns the number written. */
int video_touch_overlay(RoboxTouchCircle *out, int max);

/* 1 once any touch has been seen -- the overlay stays hidden on a device that
 * is being driven by a keyboard or gamepad, so it never clutters the screen. */
int video_touch_active(void);

/* Draw the touch controls over the presented frame. No-op until a touch has
 * been seen. */
void gx_ogl_render_touch_overlay(void);

/* ---------------------------------------------------------------------------
 * Overlay drawing: panels and text over the presented frame.
 *
 * All coordinates are in a virtual 1280x720 space, NOT window pixels -- the
 * shader rescales, so a layout written once is correct at any window size and
 * on web, where SDL reports a stale canvas size.
 *
 * Usage: begin(), any number of rect/text calls, end(). Everything between is
 * batched into a single upload and a single draw call.
 * ------------------------------------------------------------------------- */
void  gx_ogl_overlay_begin(void);
void  gx_ogl_overlay_rect(float x, float y, float w, float h,
                          float r, float g, float b, float a);
void  gx_ogl_overlay_outline(float x, float y, float w, float h, float thickness,
                             float r, float g, float b, float a);
/* `track` is extra letterspacing per glyph, in virtual px. */
void  gx_ogl_overlay_text(float x, float y, const char *str,
                          float r, float g, float b, float a,
                          float scale, float track);
float gx_ogl_overlay_text_width(const char *str, float scale, float track);
/* Arbitrary triangle with a colour per corner. Gradients, glows, beams and
 * anything else that is not axis-aligned are built from this. */
void  gx_ogl_overlay_tri(float x0, float y0, float r0, float g0, float b0, float a0,
                         float x1, float y1, float r1, float g1, float b1, float a1,
                         float x2, float y2, float r2, float g2, float b2, float a2);
void  gx_ogl_overlay_end(void);

/* Force the EFB clear colour, overriding what the guest programmed. -1 hands
 * control back to the game. See the definition in gx_ogl.c for why an overlay
 * rectangle is not a substitute. */
void  gx_ogl_set_clear_override(int64_t argb_or_minus1);

/* Overlay images. Loads one of the game's own .tpl textures (path relative to
 * the game folder or to Assets/) and returns a handle, or -1. Images draw
 * after the batched quads, so they sit on top of rects and text. */
int   gx_ogl_overlay_load_tpl(const char *path);
void  gx_ogl_overlay_image(int handle, float x, float y, float w, float h,
                           float r, float g, float b, float a);

#ifdef __cplusplus
}
#endif
#endif /* GX_OGL_H */
