#if !defined(__3DS__)  /* PICA200 has no OpenGL; sdk/gx_c3d.c + sdk/platform_3ds.c stand in */
/* sdk/gx_ogl.c -- OpenGL 3.3 GX renderer.
 *
 * ─── Licensing ───────────────────────────────────────────────────────────
 * Parts of this file are ported from Dolphin (https://dolphin-emu.org),
 * which is licensed GPL-2.0-or-later. Specifically:
 *
 *   • the GX texgen path              from VideoCommon/VertexShaderGen.cpp
 *   • the integer TEV colour combiner from VideoCommon/PixelShaderGen.cpp
 *   • the BP register decoding        from VideoCommon/BPStructs.cpp
 *
 * They are translations of Dolphin's logic into this renderer's structures
 * rather than verbatim copies, but a translation is still a derivative work.
 * That is why this project as a whole is GPL-2.0-or-later; see LICENSE for
 * the full text and THIRD-PARTY.md for every component and its terms.
 *
 * Do not remove this notice, and add to it if more is ported.
 *
 * (sdk/peripherals.c cites Dolphin too, but for hardware facts -- register
 * numbers, FIFO opcode encodings -- rather than ported code.)
 * ─────────────────────────────────────────────────────────────────────────
 *
 * Replaces the software rasterizer with a real GPU pipeline.
 * Texture decoding (GX native formats → RGBA8) is done inline.
 *
 * Design
 * ──────
 * • SDL2 owns the GL context (SDL_GL_CreateContext on our window).
 * • GL functions loaded via SDL_GL_GetProcAddress at init time.
 * • One vertex shader + fragment shader pair with uniforms for
 *   the current projection/modelview matrix.
 * • Dynamic streaming VBO: vertices decoded from GX stream,
 *   uploaded per draw call.
 * • Texture cache keyed on (guest_va, width, height, fmt) with
 *   max 256 entries (LRU eviction would be nice but not yet).
 * • GX texture formats decoded to RGBA8 in CPU before GL upload.
 */


#include <SDL2/SDL.h>
extern int g_show_fps;
extern double g_current_fps;
void gx_ogl_render_fps(void);
#include "gx_ogl.h"
#include "hle.h"          /* frame-drop profiler scopes */

/* ---------------------------------------------------------------------------
 * GL headers.
 *
 * This used to be an unguarded `#include <windows.h>` + <GL/gl.h> + <GL/glext.h>,
 * which stopped the compile dead on every non-Windows platform. windows.h was
 * not vestigial: the GLFN macro below needs APIENTRYP, which the Windows GL
 * headers get from windows.h's WINGDIAPI/APIENTRY. SDL's GL headers supply the
 * same types and enums on every platform and pull in windows.h themselves where
 * it is actually needed.
 *
 * Every GL entry point is resolved through SDL_GL_GetProcAddress with the
 * typedefs declared by GLFN, so these headers are only needed for GL types,
 * enums and APIENTRYP -- not for function declarations.
 * ------------------------------------------------------------------------- */
#if defined(ROBOX_GLES)
#  include <SDL2/SDL_opengles2.h>
#  include <GLES3/gl3.h>
#else
#  include <SDL2/SDL_opengl.h>
#  include <SDL2/SDL_opengl_glext.h>
#endif
#include <SDL2/SDL.h>

#ifndef APIENTRYP
#  ifdef APIENTRY
#    define APIENTRYP APIENTRY *
#  else
#    define APIENTRYP *
#  endif
#endif

/* ---------------------------------------------------------------------------
 * Shader prologue.
 *
 * Desktop GL 3.3 core and GLES 3.0 speak the same GLSL dialect for everything
 * this renderer uses, with one decisive exception: a GLSL ES fragment shader
 * has NO default precision for float (declaring one is mandatory) and defaults
 * `int` to mediump, which is only guaranteed 16 bits. The TEV uber-shader packs
 * combiner state into 32-bit ints and shifts by up to 24 (u_alpha_test is
 * ref0 | comp0<<8 | logic<<11 | ref1<<16 | comp1<<24), so a mediump int would
 * silently truncate the alpha test rather than fail to compile -- exactly the
 * kind of bug that only shows up on some draws.
 *
 * `precision` statements are legal (and ignored) in desktop GLSL 1.30+, so one
 * prologue serves both targets and the shader bodies stay identical.
 * ------------------------------------------------------------------------- */
#if defined(ROBOX_GLES)
#  define GLSL_VERSION "#version 300 es\n"
#else
#  define GLSL_VERSION "#version 330 core\n"
#endif
#define GLSL_PROLOGUE GLSL_VERSION "precision highp float;\nprecision highp int;\n"

/* ---------------------------------------------------------------------------
 * BGRA uploads.
 *
 * GL_BGRA is a valid client pixel format on desktop GL but does not exist in
 * GLES 3.0, so the two glTexImage2D calls that push BGRA source data (the host
 * blit surface and the Bink SURFACE32 path) will not even compile there.
 *
 * Texture swizzle IS core in both GL 3.3 and GLES 3.0, so on ES we upload the
 * identical bytes as GL_RGBA and tell the sampler to swap R and B. That costs
 * nothing at upload time -- no CPU-side channel shuffling per frame.
 * ------------------------------------------------------------------------- */
#if defined(ROBOX_GLES)
#  define GX_BGRA_FORMAT GL_RGBA
#  define GX_BGRA_SWIZZLE()                                                   \
      do {                                                                    \
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);      \
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);       \
      } while (0)
#else
#  define GX_BGRA_FORMAT GL_BGRA
#  define GX_BGRA_SWIZZLE() do { } while (0)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* =========================================================================
 * GL function pointer loading via SDL_GL_GetProcAddress
 * ========================================================================= */

#define GLFN(ret, name, ...) \
    typedef ret (APIENTRYP PFN_##name##_T)(__VA_ARGS__); \
    static PFN_##name##_T p##name;

GLFN(void,   glGenVertexArrays, GLsizei n, GLuint *arrays)
GLFN(void,   glBindVertexArray, GLuint array)
GLFN(void,   glGenBuffers, GLsizei n, GLuint *buffers)
GLFN(void,   glBindBuffer, GLenum target, GLuint buffer)
GLFN(void,   glBufferData, GLenum target, GLsizeiptr size, const void *data, GLenum usage)
GLFN(void,   glBufferSubData, GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
GLFN(void,   glDrawElementsBaseVertex, GLenum mode, GLsizei count, GLenum type, const void *indices, GLint basevertex)
GLFN(void *, glMapBufferRange, GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access)
GLFN(GLboolean, glUnmapBuffer, GLenum target)
GLFN(void,   glVertexAttribPointer, GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer)
GLFN(void,   glVertexAttribIPointer, GLuint index, GLint size, GLenum type, GLsizei stride, const void *pointer)
GLFN(void,   glEnableVertexAttribArray, GLuint index)
GLFN(GLuint, glCreateShader, GLenum shaderType)
GLFN(void,   glShaderSource, GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length)
GLFN(void,   glCompileShader, GLuint shader)
GLFN(void,   glGetShaderiv, GLuint shader, GLenum pname, GLint *params)
GLFN(void,   glGetShaderInfoLog, GLuint shader, GLsizei maxLength, GLsizei *length, GLchar *infoLog)
GLFN(GLuint, glCreateProgram,)
GLFN(void,   glAttachShader, GLuint program, GLuint shader)
GLFN(void,   glLinkProgram, GLuint program)
GLFN(void,   glDeleteProgram, GLuint program)
GLFN(void,   glGetProgramiv, GLuint program, GLenum pname, GLint *params)
GLFN(void,   glGetProgramInfoLog, GLuint program, GLsizei maxLength, GLsizei *length, GLchar *infoLog)
GLFN(void,   glUseProgram, GLuint program)
GLFN(GLint,  glGetUniformLocation, GLuint program, const GLchar *name)
GLFN(void,   glUniform1i, GLint location, GLint v0)
GLFN(void,   glUniformMatrix4fv, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
GLFN(void,   glDeleteShader, GLuint shader)
GLFN(void,   glGenFramebuffers, GLsizei n, GLuint *ids)
GLFN(void,   glBindFramebuffer, GLenum target, GLuint framebuffer)
GLFN(void,   glFramebufferTexture2D, GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)
GLFN(GLenum, glCheckFramebufferStatus, GLenum target)
GLFN(void,   glBlitFramebuffer, GLint srcX0,GLint srcY0,GLint srcX1,GLint srcY1,GLint dstX0,GLint dstY0,GLint dstX1,GLint dstY1,GLbitfield mask,GLenum filter)
GLFN(void,   glGenRenderbuffers, GLsizei n, GLuint *renderbuffers)
GLFN(void,   glBindRenderbuffer, GLenum target, GLuint renderbuffer)
GLFN(void,   glRenderbufferStorage, GLenum target, GLenum internalformat, GLsizei width, GLsizei height)
GLFN(void,   glFramebufferRenderbuffer, GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer)
GLFN(void,   glActiveTexture, GLenum texture)
GLFN(void,   glUniform1iv, GLint location, GLsizei count, const GLint *value)
GLFN(void,   glUniform4fv, GLint location, GLsizei count, const GLfloat *value)
GLFN(void,   glUniform2fv, GLint location, GLsizei count, const GLfloat *value)
GLFN(void,   glBlendEquation, GLenum mode)

#undef GLFN

/* Shorten calls */
#define glGenVertexArrays     pglGenVertexArrays
#define glBindVertexArray     pglBindVertexArray
#define glGenBuffers          pglGenBuffers
#define glBindBuffer          pglBindBuffer
#define glBufferData          pglBufferData
#define glBufferSubData       pglBufferSubData
#define glDrawElementsBaseVertex pglDrawElementsBaseVertex
#define glMapBufferRange      pglMapBufferRange
#define glUnmapBuffer         pglUnmapBuffer
#define glVertexAttribPointer pglVertexAttribPointer
#define glVertexAttribIPointer pglVertexAttribIPointer
#define glEnableVertexAttribArray pglEnableVertexAttribArray
#define glCreateShader        pglCreateShader
#define glShaderSource        pglShaderSource
#define glCompileShader       pglCompileShader
#define glGetShaderiv         pglGetShaderiv
#define glGetShaderInfoLog    pglGetShaderInfoLog
#define glCreateProgram       pglCreateProgram
#define glAttachShader        pglAttachShader
#define glLinkProgram         pglLinkProgram
#define glDeleteProgram       pglDeleteProgram
#define glGetProgramiv        pglGetProgramiv
#define glGetProgramInfoLog   pglGetProgramInfoLog
#define glUseProgram          pglUseProgram
#define glGetUniformLocation  pglGetUniformLocation
#define glUniform1i           pglUniform1i
#define glUniformMatrix4fv    pglUniformMatrix4fv
#define glDeleteShader        pglDeleteShader
#define glGenFramebuffers     pglGenFramebuffers
#define glBindFramebuffer     pglBindFramebuffer
#define glFramebufferTexture2D pglFramebufferTexture2D
#define glCheckFramebufferStatus pglCheckFramebufferStatus
#define glBlitFramebuffer     pglBlitFramebuffer
#define glGenRenderbuffers    pglGenRenderbuffers
#define glBindRenderbuffer    pglBindRenderbuffer
#define glRenderbufferStorage pglRenderbufferStorage
#define glFramebufferRenderbuffer pglFramebufferRenderbuffer
#define glActiveTexture       pglActiveTexture
#define glUniform1iv          pglUniform1iv
#define glUniform4fv          pglUniform4fv
#define glUniform2fv          pglUniform2fv
#define glBlendEquation       pglBlendEquation

static void load_gl_procs(void) {
#define LOAD(name) p##name = (PFN_##name##_T)SDL_GL_GetProcAddress(#name); \
    if (!p##name) fprintf(stderr, "[gx_ogl] WARNING: failed to load " #name "\n");
    LOAD(glGenVertexArrays)
    LOAD(glBindVertexArray)
    LOAD(glGenBuffers)
    LOAD(glBindBuffer)
    LOAD(glBufferData)
    LOAD(glBufferSubData)
    LOAD(glDrawElementsBaseVertex)
    LOAD(glMapBufferRange)
    LOAD(glUnmapBuffer)
    LOAD(glVertexAttribPointer)
    LOAD(glVertexAttribIPointer)
    LOAD(glEnableVertexAttribArray)
    LOAD(glCreateShader)
    LOAD(glShaderSource)
    LOAD(glCompileShader)
    LOAD(glGetShaderiv)
    LOAD(glGetShaderInfoLog)
    LOAD(glCreateProgram)
    LOAD(glAttachShader)
    LOAD(glLinkProgram)
    LOAD(glDeleteProgram)
    LOAD(glGetProgramiv)
    LOAD(glGetProgramInfoLog)
    LOAD(glUseProgram)
    LOAD(glGetUniformLocation)
    LOAD(glUniform1i)
    LOAD(glUniformMatrix4fv)
    LOAD(glDeleteShader)
    LOAD(glGenFramebuffers)
    LOAD(glBindFramebuffer)
    LOAD(glFramebufferTexture2D)
    LOAD(glCheckFramebufferStatus)
    LOAD(glBlitFramebuffer)
    LOAD(glGenRenderbuffers)
    LOAD(glBindRenderbuffer)
    LOAD(glRenderbufferStorage)
    LOAD(glFramebufferRenderbuffer)
    LOAD(glActiveTexture)
    LOAD(glUniform1iv)
    LOAD(glUniform4fv)
    LOAD(glUniform2fv)
    LOAD(glBlendEquation)
#undef LOAD
}

/* =========================================================================
 * Present rectangle (letterbox), shared with the input layer.
 *
 * gx_ogl_present blits the EFB into a centred sub-rectangle of the window to
 * preserve the game's aspect, clearing the surrounding bars to black. Input
 * needs that same rectangle to convert a window/touch coordinate into a game
 * coordinate -- see video.c's pointer mapping.
 *
 * Seeded to the full window so a pointer event arriving before the first
 * present still maps sanely rather than dividing by zero.
 * ========================================================================= */
static int g_present_x = 0, g_present_y = 0, g_present_w = 0, g_present_h = 0;

static void gx_ogl_set_present_rect(int x, int y, int w, int h) {
    g_present_x = x; g_present_y = y; g_present_w = w; g_present_h = h;
}

void gx_ogl_get_present_rect(int *x, int *y, int *w, int *h) {
    *x = g_present_x; *y = g_present_y; *w = g_present_w; *h = g_present_h;
}

/* Window size at the last present, for mapping game NDC into the overlay's
 * virtual 1280x720 space (which spans the whole window, not the letterbox). */
static int g_present_win_w, g_present_win_h;

/* ---- player sprite capture (see gx_ogl.h) ------------------------------ */
static int   g_pcap_on, g_pcap_suppress, g_pcap_seen;
static float g_pcap_min_x, g_pcap_min_y, g_pcap_max_x, g_pcap_max_y; /* NDC */
static float g_pcap_quad[4];       /* latched overlay-space x,y,w,h        */
static int   g_pcap_fresh;         /* frames the latched quad stays valid  */

/* ---- camera assist (robox_mario.c) -------------------------------------
 * A small, eased post-projection view shift applied ONLY to draws that use
 * the same projection as the captured player -- HUD and menus use their own
 * projections and stay put. The mod feeds the on-screen error between where
 * the player is and where an SMB camera would want him; because the capture
 * itself sees shifted coordinates, this is a closed loop and converges.   */
static float g_cam_shift_x, g_cam_shift_y;      /* NDC units               */
static float g_cam_world_proj[7];
static int   g_cam_world_proj_ok;
static int   g_cam_ticked;                       /* fed this frame?        */
static float g_pcap_proj[7];                     /* proj seen during capture */
static int   g_pcap_proj_ok;
static float g_pcap_mv[12];                      /* view matrix, same draw   */
static float g_view_mv[12], g_view_proj[7];      /* latched for queries      */
static int   g_view_ok;
static float g_pcap_ndc[4];                      /* cx, cy, w, h in NDC      */
static int   g_pcap_ndc_ok;

/* Player quad in game NDC (pre-assist), for camera reasoning. */
int gx_ogl_player_quad_ndc(float *cx, float *cy, float *w, float *h) {
    if (!g_pcap_ndc_ok || g_pcap_fresh <= 0) return 0;
    *cx = g_pcap_ndc[0]; *cy = g_pcap_ndc[1];
    *w  = g_pcap_ndc[2]; *h  = g_pcap_ndc[3];
    return 1;
}
/* GX_STATE_DIRTY() is defined further down; bump its counter directly.     */
extern uint32_t g_gx_state_gen;

void gx_ogl_cam_assist(float err_x_ndc, float err_y_ndc) {
    /* Integral controller: nudge by a fraction of the remaining error.
     * X clamp = just under a quarter screen each way: enough to hold the
     * classic look-ahead at full run against the game camera's lag, while
     * staying inside the margin the game keeps rendered around the view. */
    float nx = g_cam_shift_x + err_x_ndc * 0.14f;
    float ny = g_cam_shift_y + err_y_ndc * 0.06f;
    if (nx >  0.48f) nx =  0.48f;
    if (nx < -0.48f) nx = -0.48f;
    if (ny >  0.20f) ny =  0.20f;
    if (ny < -0.20f) ny = -0.20f;
    if (nx != g_cam_shift_x || ny != g_cam_shift_y) {
        g_cam_shift_x = nx;
        g_cam_shift_y = ny;
        ++g_gx_state_gen;          /* view changed: no stale draw merging  */
    }
    g_cam_ticked = 1;
}

static void cam_assist_frame_decay(void) {
    /* No feed this frame (menu, transition, mod off): ease back to the
     * game's own framing instead of freezing a stale offset. */
    if (!g_cam_ticked && (g_cam_shift_x != 0.0f || g_cam_shift_y != 0.0f)) {
        g_cam_shift_x *= 0.90f;
        g_cam_shift_y *= 0.90f;
        if (g_cam_shift_x > -1e-3f && g_cam_shift_x < 1e-3f) g_cam_shift_x = 0;
        if (g_cam_shift_y > -1e-3f && g_cam_shift_y < 1e-3f) g_cam_shift_y = 0;
        ++g_gx_state_gen;
    }
    g_cam_ticked = 0;
}

void gx_ogl_player_capture_begin(int suppress) {
    g_pcap_on = 1;
    g_pcap_suppress = suppress;
    g_pcap_seen = 0;
    g_pcap_proj_ok = 0;
}

void gx_ogl_player_capture_end(void) {
    g_pcap_on = 0;
    if (!g_pcap_seen) return;

    if (g_pcap_proj_ok) {          /* remember which projection is "world" */
        for (int i = 0; i < 7; i++) g_cam_world_proj[i] = g_pcap_proj[i];
        for (int i = 0; i < 12; i++) g_view_mv[i] = g_pcap_mv[i];
        for (int i = 0; i < 7; i++) g_view_proj[i] = g_pcap_proj[i];
        g_view_ok = 1;
        g_cam_world_proj_ok = 1;
    }

    /* NDC quad, MINUS the cosmetic camera-assist shift: callers reasoning
     * about the game's own camera need the unshifted picture. */
    g_pcap_ndc[0] = (g_pcap_min_x + g_pcap_max_x) * 0.5f - g_cam_shift_x;
    g_pcap_ndc[1] = (g_pcap_min_y + g_pcap_max_y) * 0.5f - g_cam_shift_y;
    g_pcap_ndc[2] = g_pcap_max_x - g_pcap_min_x;
    g_pcap_ndc[3] = g_pcap_max_y - g_pcap_min_y;
    g_pcap_ndc_ok = 1;

    /* NDC -> window pixels (through the letterbox), GL bottom-origin.      */
    float px = (float)g_present_x, py = (float)g_present_y;
    float pw = (float)g_present_w, ph = (float)g_present_h;
    int   ww = g_present_win_w, wh = g_present_win_h;
    if (pw <= 0 || ph <= 0 || ww <= 0 || wh <= 0) return;

    float wx0 = px + (g_pcap_min_x * 0.5f + 0.5f) * pw;
    float wx1 = px + (g_pcap_max_x * 0.5f + 0.5f) * pw;
    float gy0 = py + (g_pcap_min_y * 0.5f + 0.5f) * ph;   /* bottom-origin  */
    float gy1 = py + (g_pcap_max_y * 0.5f + 0.5f) * ph;
    float ty0 = (float)wh - gy1;                           /* top-origin     */
    float ty1 = (float)wh - gy0;

    g_pcap_quad[0] = wx0 * 1280.0f / (float)ww;
    g_pcap_quad[1] = ty0 *  720.0f / (float)wh;
    g_pcap_quad[2] = (wx1 - wx0) * 1280.0f / (float)ww;
    g_pcap_quad[3] = (ty1 - ty0) *  720.0f / (float)wh;
    g_pcap_fresh   = 3;
}

int gx_ogl_player_quad(float *x, float *y, float *w, float *h) {
    if (g_pcap_fresh <= 0) return 0;
    *x = g_pcap_quad[0]; *y = g_pcap_quad[1];
    *w = g_pcap_quad[2]; *h = g_pcap_quad[3];
    return 1;
}

/* =========================================================================
 * GLSL shaders
 * ========================================================================= */

static const char *k_vert_src =
GLSL_PROLOGUE
"layout(location=0) in vec3 a_pos;\n"
"layout(location=1) in vec4 a_color;\n"
"layout(location=2) in vec2 a_tex0;\n"
"uniform mat4 u_mvp;\n"
"uniform int  u_uv_from_pos;\n"  /* 1 = UVs from position (texgen substitute) */
"uniform vec4 u_texmtx0;\n"
"uniform vec4 u_texmtx1;\n"
"out vec4 v_color;\n"
"out vec2 v_tex;\n"          /* single texcoord shared by all stages */
"void main() {\n"
"    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
"    v_color = a_color;\n"
/* GX texgen (ported from Dolphin VertexShaderGen.cpp): position-source REGULAR
 * texgen input coord is (pos.x, pos.y, pos.z, 1); GX's AB11 input form forces
 * z=1 so the texture matrix's 3rd column (the atlas sub-rect offset) actually
 * contributes. Using z=0 (our old bug) dropped that offset, stretching the
 * whole atlas over every quad. Dolphin also converts NaN coords to 1.0 here
 * (bugs.dolphin-emu.org/issues/11458). */
"    vec3 tc = a_pos.xyz;\n"
"    if (isnan(tc.x)) tc.x = 1.0;\n"
"    if (isnan(tc.y)) tc.y = 1.0;\n"
"    if (isnan(tc.z)) tc.z = 1.0;\n"
"    vec4 p4 = vec4(tc.x, tc.y, 1.0, 1.0);\n"
"    v_tex   = (u_uv_from_pos != 0)\n"
"            ? vec2(dot(u_texmtx0, p4), dot(u_texmtx1, p4))\n"
"            : a_tex0;\n"
"}\n";

/* ---- Full GX TEV uber-shader -------------------------------------------
 * Implements the GX texture environment pipeline:
 *   out = clamp( (mix(a,b,c) + d + bias) * scale )  [add mode]
 *   out = clamp( (d - mix(a,b,c) + bias) * scale )  [sub mode]
 *
 * Uniforms (set per draw-call from g_bp[]):
 *   u_num_stages       - active stage count (1-8)
 *   u_tev_color[8]     - packed color combiner word per stage
 *   u_tev_alpha[8]     - packed alpha combiner word per stage
 *   u_tev_order[8]     - packed tex/ras order per stage
 *   u_tev_ksel[8]      - packed konst color/alpha selector per stage
 *   u_konst_color[4]   - GXSetTevKColor values (normalized vec4)
 *   u_tex[8]           - sampler2D texture slots 0-7
 *   u_tex_en[8]        - texture valid flags per slot
 *   u_alpha_test       - packed alpha compare (ref0 | comp0<<8 | logic<<11 | ref1<<16 | comp1<<24)
 * ----------------------------------------------------------------------- */
/* Body only -- no #version/precision. The prologue is prepended at compile
 * time by compile_tev_program(), which also injects
 *   #define ROBOX_NUM_STAGES <n>
 * to specialise the shader per stage count. That define turns the dynamic TEV
 * loop below into a compile-time-bounded one, which is the whole point: the
 * uber-shader was register-allocated for the 8-stage worst case, so even a
 * 1-stage menu draw ran at Midgard's lowest occupancy (measured: 190 ms of a
 * 241 ms frame). A per-stage-count program lets the driver unroll the loop and
 * free the dead TEV registers, restoring occupancy for the common cheap case. */
static const char *k_frag_src =
"#ifndef ROBOX_NUM_STAGES\n#define ROBOX_NUM_STAGES 8\n#endif\n"
"in  vec4 v_color;\n"
"in  vec2 v_tex;\n"
"out vec4 frag;\n"
"\n"
"uniform sampler2D u_tex[8];\n"
"uniform int       u_tex_en[8];\n"
"uniform int       u_num_stages;\n"
/* The per-stage TEV structural words drive the big selector if-chains
 * (get_konst 32-way, cc_in 15-way, etc.). When a variant is specialised
 * (ROBOX_SPECIALIZED) these are emitted as `const int` arrays with the same
 * names in the generated preamble, so with the loop unrolled the selectors
 * fold to a single path -- which is where the ~50x fragment-cost win comes
 * from. Unspecialised, they stay uniforms and the shader is the fallback. */
"#ifndef ROBOX_SPECIALIZED\n"
"uniform int       u_tev_color[8];\n"
"uniform int       u_tev_alpha[8];\n"
"uniform int       u_tev_order[8];\n"
"uniform int       u_tev_ksel[8];\n"
"uniform int       u_swap[4];\n"   /* swap tables: r|g<<2|b<<4|a<<6 */
"#endif\n"
"uniform vec4      u_konst_color[4];\n"
"uniform vec4      u_tev_reg[4];\n"
"uniform int       u_alpha_test;\n"
"\n"
/* Sample a specific texture slot */
"vec4 sample_tex(int slot) {\n"
"    if (u_tex_en[slot] == 0) return vec4(0.0);\n"
"    if (slot == 0) return texture(u_tex[0], v_tex);\n"
"    if (slot == 1) return texture(u_tex[1], v_tex);\n"
"    if (slot == 2) return texture(u_tex[2], v_tex);\n"
"    if (slot == 3) return texture(u_tex[3], v_tex);\n"
"    if (slot == 4) return texture(u_tex[4], v_tex);\n"
"    if (slot == 5) return texture(u_tex[5], v_tex);\n"
"    if (slot == 6) return texture(u_tex[6], v_tex);\n"
"    return texture(u_tex[7], v_tex);\n"
"}\n"
"\n"
/* Konst color (GX_TEV_KCSEL_*) */
"vec3 get_konst_rgb(int sel) {\n"
"    if (sel == 0)  return vec3(1.0);\n"
"    if (sel == 1)  return vec3(0.875);\n"
"    if (sel == 2)  return vec3(0.75);\n"
"    if (sel == 3)  return vec3(0.625);\n"
"    if (sel == 4)  return vec3(0.5);\n"
"    if (sel == 5)  return vec3(0.375);\n"
"    if (sel == 6)  return vec3(0.25);\n"
"    if (sel == 7)  return vec3(0.125);\n"
"    if (sel == 12) return u_konst_color[0].rgb;\n"
"    if (sel == 13) return u_konst_color[1].rgb;\n"
"    if (sel == 14) return u_konst_color[2].rgb;\n"
"    if (sel == 15) return u_konst_color[3].rgb;\n"
"    if (sel == 16) return vec3(u_konst_color[0].r);\n"
"    if (sel == 17) return vec3(u_konst_color[1].r);\n"
"    if (sel == 18) return vec3(u_konst_color[2].r);\n"
"    if (sel == 19) return vec3(u_konst_color[3].r);\n"
"    if (sel == 20) return vec3(u_konst_color[0].g);\n"
"    if (sel == 21) return vec3(u_konst_color[1].g);\n"
"    if (sel == 22) return vec3(u_konst_color[2].g);\n"
"    if (sel == 23) return vec3(u_konst_color[3].g);\n"
"    if (sel == 24) return vec3(u_konst_color[0].b);\n"
"    if (sel == 25) return vec3(u_konst_color[1].b);\n"
"    if (sel == 26) return vec3(u_konst_color[2].b);\n"
"    if (sel == 27) return vec3(u_konst_color[3].b);\n"
"    if (sel == 28) return vec3(u_konst_color[0].a);\n"
"    if (sel == 29) return vec3(u_konst_color[1].a);\n"
"    if (sel == 30) return vec3(u_konst_color[2].a);\n"
"    if (sel == 31) return vec3(u_konst_color[3].a);\n"
"    return vec3(0.0);\n"
"}\n"
"float get_konst_a(int sel) {\n"
"    if (sel == 0)  return 1.0;\n"
"    if (sel == 1)  return 0.875;\n"
"    if (sel == 2)  return 0.75;\n"
"    if (sel == 3)  return 0.625;\n"
"    if (sel == 4)  return 0.5;\n"
"    if (sel == 5)  return 0.375;\n"
"    if (sel == 6)  return 0.25;\n"
"    if (sel == 7)  return 0.125;\n"
"    if (sel == 16) return u_konst_color[0].r;\n"
"    if (sel == 17) return u_konst_color[1].r;\n"
"    if (sel == 18) return u_konst_color[2].r;\n"
"    if (sel == 19) return u_konst_color[3].r;\n"
"    if (sel == 20) return u_konst_color[0].g;\n"
"    if (sel == 21) return u_konst_color[1].g;\n"
"    if (sel == 22) return u_konst_color[2].g;\n"
"    if (sel == 23) return u_konst_color[3].g;\n"
"    if (sel == 24) return u_konst_color[0].b;\n"
"    if (sel == 25) return u_konst_color[1].b;\n"
"    if (sel == 26) return u_konst_color[2].b;\n"
"    if (sel == 27) return u_konst_color[3].b;\n"
"    if (sel == 28) return u_konst_color[0].a;\n"
"    if (sel == 29) return u_konst_color[1].a;\n"
"    if (sel == 30) return u_konst_color[2].a;\n"
"    if (sel == 31) return u_konst_color[3].a;\n"
"    return 0.0;\n"
"}\n"
"\n"
/* Color input selector (GX_CC_*) */
"vec3 cc_in(int sel, vec4 texc, vec4 ras, vec4 prev, vec4 r0, vec4 r1, vec4 r2, vec3 konst) {\n"
"    if (sel == 0)  return prev.rgb;\n"      /* CC_CPREV */
"    if (sel == 1)  return vec3(prev.a);\n"  /* CC_APREV */
"    if (sel == 2)  return r0.rgb;\n"        /* CC_C0 */
"    if (sel == 3)  return vec3(r0.a);\n"   /* CC_A0 */
"    if (sel == 4)  return r1.rgb;\n"        /* CC_C1 */
"    if (sel == 5)  return vec3(r1.a);\n"   /* CC_A1 */
"    if (sel == 6)  return r2.rgb;\n"        /* CC_C2 */
"    if (sel == 7)  return vec3(r2.a);\n"   /* CC_A2 */
"    if (sel == 8)  return texc.rgb;\n"      /* CC_TEXC */
"    if (sel == 9)  return vec3(texc.a);\n" /* CC_TEXA */
"    if (sel == 10) return ras.rgb;\n"       /* CC_RASC */
"    if (sel == 11) return vec3(ras.a);\n"  /* CC_RASA */
"    if (sel == 12) return vec3(1.0);\n"    /* CC_ONE */
"    if (sel == 13) return vec3(0.5);\n"    /* CC_HALF */
"    if (sel == 14) return konst;\n"         /* CC_KONST */
"    return vec3(0.0);\n"                    /* CC_ZERO */
"}\n"
"\n"
/* Alpha input selector (GX_CA_*) */
"float ca_in(int sel, vec4 texc, vec4 ras, vec4 prev, vec4 r0, vec4 r1, vec4 r2, float konst_a) {\n"
"    if (sel == 0) return prev.a;\n"         /* CA_APREV */
"    if (sel == 1) return r0.a;\n"           /* CA_A0 */
"    if (sel == 2) return r1.a;\n"           /* CA_A1 */
"    if (sel == 3) return r2.a;\n"           /* CA_A2 */
"    if (sel == 4) return texc.a;\n"         /* CA_TEXA */
"    if (sel == 5) return ras.a;\n"          /* CA_RASA */
"    if (sel == 6) return konst_a;\n"        /* CA_KONST */
"    return 0.0;\n"                          /* CA_ZERO */
"}\n"
"\n"
/* Run one TEV stage color combiner
 * packed word: a[3:0] b[7:4] c[11:8] d[15:12] bias[17:16] op[18] clamp[19] scale[21:20] dest[23:22]
 * NOTE: this is the SHADER-LOCAL layout — setup_tev_uniforms REPACKS the raw
 * BP word (which is d-low: d[3:0] c[7:4] b[11:8] a[15:12]) into this order
 * before upload. Change either side only in lockstep with the other. */
/* Integer TEV color combiner, ported from Dolphin PixelShaderGen.cpp
 * (WriteTevRegular): (d + bias + lerp(a,b,c)) * scale, all in GX's 0-255 int
 * space. a,b,c are masked &255; d is not. lerp uses c scaled 0..255->0..256
 * (c + (c>>7)) then >>8, with a rounding bias (+128 add / +127 sub). */
"vec3 tev_color(int pk, vec4 texc, vec4 ras, vec4 prev, vec4 r0, vec4 r1, vec4 r2, vec3 konst) {\n"
"    int a_s=(pk)&0xF; int b_s=(pk>>4)&0xF; int c_s=(pk>>8)&0xF; int d_s=(pk>>12)&0xF;\n"
"    int bias=(pk>>16)&0x3; int op=(pk>>18)&0x1; int doClamp=(pk>>19)&0x1; int sc=(pk>>20)&0x3;\n"
"    vec3 af = cc_in(a_s,texc,ras,prev,r0,r1,r2,konst);\n"
"    vec3 bf = cc_in(b_s,texc,ras,prev,r0,r1,r2,konst);\n"
"    vec3 cf = cc_in(c_s,texc,ras,prev,r0,r1,r2,konst);\n"
"    vec3 df = cc_in(d_s,texc,ras,prev,r0,r1,r2,konst);\n"
     /* bias==3 = GX COMPARE mode (Dolphin WriteTevCompare): the combiner
      * becomes d + (compare(a,b) ? c : 0); mode = shift<<1 | op selects the
      * comparison width. Kept EXACT integer regardless of ROBOX_FLOAT_TEV --
      * threshold/mask materials need bit-accuracy. */
"    if (bias == 3) {\n"
"        ivec3 a = ivec3(round(af*255.0)) & 255;\n"
"        ivec3 b = ivec3(round(bf*255.0)) & 255;\n"
"        ivec3 c = ivec3(round(cf*255.0)) & 255;\n"
"        ivec3 d = ivec3(round(df*255.0));\n"
"        int mode = (sc << 1) | op;\n"
"        ivec3 res;\n"
"        if (mode <= 5) {\n"
"            int av, bv;\n"
"            if (mode < 2)      { av = a.r; bv = b.r; }\n"                       /* R8 */
"            else if (mode < 4) { av = (a.g<<8)|a.r; bv = (b.g<<8)|b.r; }\n"    /* GR16 */
"            else               { av = (a.b<<16)|(a.g<<8)|a.r; bv = (b.b<<16)|(b.g<<8)|b.r; }\n" /* BGR24 */
"            bool hit = ((mode & 1) == 0) ? (av > bv) : (av == bv);\n"
"            res = d + (hit ? c : ivec3(0));\n"
"        } else {\n"                                                             /* RGB8: per-channel */
"            bvec3 hit = ((mode & 1) == 0) ? greaterThan(a, b) : equal(a, b);\n"
"            res = d + ivec3(hit) * c;\n"
"        }\n"
"        res = (doClamp!=0) ? clamp(res, ivec3(0), ivec3(255)) : clamp(res, ivec3(-1024), ivec3(1023));\n"
"        return vec3(res) / 255.0;\n"
"    }\n"
/* Regular combiner: (d + bias + lerp(a,b,c)) * scale. ROBOX_FLOAT_TEV does the
 * whole thing in float [0,1] instead of GX 0-255 integer space -- it drops four
 * round()+convert+mask ops and the integer mul/shift per fragment, which is the
 * bulk of the fragment ALU. Not bit-exact (bias 0.5 vs 128/255, no integer
 * truncation), but visually indistinguishable for this game's materials. The
 * exact integer path is the fallback (RECOMP_TEV_INT=1). */
"#ifdef ROBOX_FLOAT_TEV\n"
"    float bo = (bias==1) ? 0.5 : ((bias==2) ? -0.5 : 0.0);\n"
"    vec3 lp = mix(af, bf, cf);\n"
"    vec3 r = (op==0) ? (df + bo + lp) : (df + bo - lp);\n"
"    r *= (sc==1) ? 2.0 : ((sc==2) ? 4.0 : ((sc==3) ? 0.5 : 1.0));\n"
"    return (doClamp!=0) ? clamp(r, vec3(0.0), vec3(1.0)) : clamp(r, vec3(-4.0), vec3(4.0));\n"
"#else\n"
"    ivec3 a = ivec3(round(af*255.0)) & 255;\n"
"    ivec3 b = ivec3(round(bf*255.0)) & 255;\n"
"    ivec3 c = ivec3(round(cf*255.0)) & 255;\n"
"    ivec3 d = ivec3(round(df*255.0));\n"
"    ivec3 db = d + ((bias==1)?ivec3(128):(bias==2)?ivec3(-128):ivec3(0));\n"
"    ivec3 lerp = (a << 8) + (b - a) * (c + (c >> 7));\n"
"    if (sc==1) { db = db << 1; lerp = lerp << 1; }\n"
"    else if (sc==2) { db = db << 2; lerp = lerp << 2; }\n"
"    int lb = (sc==3) ? 0 : ((op==0) ? 128 : 127);\n"
"    ivec3 lf = (lerp + lb) >> 8;\n"
"    ivec3 res = (op==0) ? (db + lf) : (db - lf);\n"
"    if (sc==3) res = res >> 1;\n"
"    res = (doClamp!=0) ? clamp(res, ivec3(0), ivec3(255)) : clamp(res, ivec3(-1024), ivec3(1023));\n"
"    return vec3(res) / 255.0;\n"
"#endif\n"
"}\n"
"\n"
/* Run one TEV stage alpha combiner
 * packed word: rswap[1:0] tswap[3:2] a[6:4] b[9:7] c[12:10] d[15:13] bias[17:16] op[18] clamp[19] scale[21:20] dest[23:22]
 * (shader-local layout — repacked from the d-low BP word by setup_tev_uniforms,
 * same as tev_color above) */
"float tev_alpha(int pk, vec4 texc, vec4 ras, vec4 prev, vec4 r0, vec4 r1, vec4 r2, float konst_a) {\n"
"    int a_s  = (pk >> 4) & 0x7;\n"
"    int b_s  = (pk >> 7) & 0x7;\n"
"    int c_s  = (pk >>10) & 0x7;\n"
"    int d_s  = (pk >>13) & 0x7;\n"
"    int bias = (pk >>16) & 0x3;\n"
"    int op   = (pk >>18) & 0x1;\n"
"    int doClamp = (pk >>19) & 0x1;\n"
"    int sc   = (pk >>20) & 0x3;\n"
"    float af = ca_in(a_s,texc,ras,prev,r0,r1,r2,konst_a);\n"
"    float bf = ca_in(b_s,texc,ras,prev,r0,r1,r2,konst_a);\n"
"    float cf = ca_in(c_s,texc,ras,prev,r0,r1,r2,konst_a);\n"
"    float df = ca_in(d_s,texc,ras,prev,r0,r1,r2,konst_a);\n"
     /* bias==3 = compare mode. Kept EXACT integer (see tev_color). */
"    if (bias == 3) {\n"
"        int a = int(round(af*255.0)) & 255;\n"
"        int b = int(round(bf*255.0)) & 255;\n"
"        int c = int(round(cf*255.0)) & 255;\n"
"        int d = int(round(df*255.0));\n"
"        int mode = (sc << 1) | op;\n"
"        bool hit = ((mode & 1) == 0) ? (a > b) : (a == b);\n"
"        int res = d + (hit ? c : 0);\n"
"        res = (doClamp!=0) ? clamp(res, 0, 255) : clamp(res, -1024, 1023);\n"
"        return float(res) / 255.0;\n"
"    }\n"
"#ifdef ROBOX_FLOAT_TEV\n"
"    float bo = (bias==1) ? 0.5 : ((bias==2) ? -0.5 : 0.0);\n"
"    float lp = mix(af, bf, cf);\n"
"    float r = (op==0) ? (df + bo + lp) : (df + bo - lp);\n"
"    r *= (sc==1) ? 2.0 : ((sc==2) ? 4.0 : ((sc==3) ? 0.5 : 1.0));\n"
"    return (doClamp!=0) ? clamp(r, 0.0, 1.0) : clamp(r, -4.0, 4.0);\n"
"#else\n"
"    int a = int(round(af*255.0)) & 255;\n"
"    int b = int(round(bf*255.0)) & 255;\n"
"    int c = int(round(cf*255.0)) & 255;\n"
"    int d = int(round(df*255.0));\n"
"    int db = d + ((bias==1)?128:(bias==2)?-128:0);\n"
"    int lerp = (a << 8) + (b - a) * (c + (c >> 7));\n"
"    if (sc==1) { db = db << 1; lerp = lerp << 1; }\n"
"    else if (sc==2) { db = db << 2; lerp = lerp << 2; }\n"
"    int lb = (sc==3) ? 0 : ((op==0) ? 128 : 127);\n"
"    int lf = (lerp + lb) >> 8;\n"
"    int res = (op==0) ? (db + lf) : (db - lf);\n"
"    if (sc==3) res = res >> 1;\n"
"    res = (doClamp!=0) ? clamp(res, 0, 255) : clamp(res, -1024, 1023);\n"
"    return float(res) / 255.0;\n"
"#endif\n"
"}\n"
"\n"
/* TEV swap tables: rswap/tswap in the alpha combiner word (bits[1:0]/[3:2])
 * select one of 4 tables that re-route the ras/tex channels before the
 * combiners run (fonts/tints commonly broadcast one channel via RRRA etc). */
"float pick_ch(vec4 c, int s) {\n"
"    if (s == 0) return c.r;\n"
"    if (s == 1) return c.g;\n"
"    if (s == 2) return c.b;\n"
"    return c.a;\n"
"}\n"
"vec4 apply_swap(vec4 c, int tbl) {\n"
"    return vec4(pick_ch(c, tbl & 3), pick_ch(c, (tbl >> 2) & 3),\n"
"                pick_ch(c, (tbl >> 4) & 3), pick_ch(c, (tbl >> 6) & 3));\n"
"}\n"
"\n"
"bool alpha_cmp(int comp, float alpha, float ref) {\n"
"    if (comp == 0) return false;\n"         /* NEVER */
"    if (comp == 1) return alpha <  ref;\n"  /* LESS */
"    if (comp == 2) return alpha == ref;\n"  /* EQUAL */
"    if (comp == 3) return alpha <= ref;\n"  /* LEQUAL */
"    if (comp == 4) return alpha >  ref;\n"  /* GREATER */
"    if (comp == 5) return alpha != ref;\n"  /* NEQUAL */
"    if (comp == 6) return alpha >= ref;\n"  /* GEQUAL */
"    return true;\n"                         /* ALWAYS */
"}\n"
"\n"
"void main() {\n"
"    vec4 ras = v_color;\n"                  /* rasterized color = vertex color */
/* TEV output registers init from GXSetTevColor (Dolphin: prev=reg0, c0=reg1,
 * c1=reg2, c2=reg3). Previously hardwired to 0, which zeroed the C0/C1/C2
 * offsets many UI combiners add (e.g. TEXC*KONST + C0). */
"    vec4 tev_prev = u_tev_reg[0];\n"
"    vec4 tev_r0   = u_tev_reg[1];\n"
"    vec4 tev_r1   = u_tev_reg[2];\n"
"    vec4 tev_r2   = u_tev_reg[3];\n"
"\n"
"    vec3 last_rgb = tev_prev.rgb; float last_a = tev_prev.a;\n"
"    for (int s = 0; s < ROBOX_NUM_STAGES; s++) {\n"
"        int ord  = u_tev_order[s];\n"
"        int tmap = ord        & 0x7;\n"     /* tex_map 0-7 */
"        int ten  = (ord >> 6) & 0x1;\n"     /* tex enable */
"        vec4 texc = (ten != 0) ? sample_tex(tmap) : vec4(0.0);\n"
        /* Rasterized channel per stage (TREF color[9:7]): 0=COLOR0A0,
         * 1=COLOR1A1 (approximated by the same vertex color), 7=NULL=zero.
         * Feeding v_color to NULL-channel stages brightened every layer
         * whose combiner references RASC with no channel bound. */
"        int rchan = (ord >> 7) & 0x7;\n"
"        vec4 ras  = (rchan <= 1) ? v_color : vec4(0.0);\n"
"\n"
"        int apk = u_tev_alpha[s];\n"
        /* Per-stage channel swaps (GX swap tables): tswap re-routes the
         * sampled texture's channels, rswap the rasterized color's. */
"        texc = apply_swap(texc, u_swap[(apk >> 2) & 3]);\n"
"        ras  = apply_swap(ras,  u_swap[apk & 3]);\n"
"\n"
"        int ksel    = u_tev_ksel[s];\n"
"        vec3 konst  = get_konst_rgb(ksel & 0x1F);\n"
"        float konst_a = get_konst_a((ksel >> 5) & 0x1F);\n"
"\n"
"        int cpk = u_tev_color[s];\n"
"        vec3  new_rgb = tev_color(cpk, texc, ras, tev_prev, tev_r0, tev_r1, tev_r2, konst);\n"
"        float new_a   = tev_alpha(apk, texc, ras, tev_prev, tev_r0, tev_r1, tev_r2, konst_a);\n"
"\n"
        /* Color and alpha results route to their OWN destination registers
         * (hardware semantics). The old combined write clobbered the other
         * register's channels whenever cdst != adst. */
"        int cdst = (cpk >> 22) & 0x3;\n"
"        int adst = (apk >> 22) & 0x3;\n"
"        if (cdst == 0) tev_prev.rgb = new_rgb;\n"
"        else if (cdst == 1) tev_r0.rgb = new_rgb;\n"
"        else if (cdst == 2) tev_r1.rgb = new_rgb;\n"
"        else tev_r2.rgb = new_rgb;\n"
"        if (adst == 0) tev_prev.a = new_a;\n"
"        else if (adst == 1) tev_r0.a = new_a;\n"
"        else if (adst == 2) tev_r1.a = new_a;\n"
"        else tev_r2.a = new_a;\n"
"        last_rgb = new_rgb; last_a = new_a;\n"
"    }\n"
"\n"
    /* Pixel output = the LAST stage's own result (not unconditionally
     * prev — the final stage may target any register). */
"    frag = vec4(last_rgb, last_a);\n"
"\n"
    /* Alpha test */
"    if (u_alpha_test != 0) {\n"
"        int at = u_alpha_test;\n"
"        float ref0  = float((at)       & 0xFF) / 255.0;\n"
"        int   comp0 = (at >> 8)  & 0x7;\n"
"        int   logic = (at >> 11) & 0x3;\n"
"        float ref1  = float((at >> 16) & 0xFF) / 255.0;\n"
"        int   comp1 = (at >> 24) & 0x7;\n"
"        bool  r0    = alpha_cmp(comp0, frag.a, ref0);\n"
"        bool  r1    = alpha_cmp(comp1, frag.a, ref1);\n"
"        bool  pass;\n"
"        if      (logic == 0) pass = r0 && r1;\n"  /* AND */
"        else if (logic == 1) pass = r0 || r1;\n"  /* OR */
"        else if (logic == 2) pass = r0 ^^ r1;\n"  /* XOR */
"        else                 pass = !(r0 ^^ r1);\n" /* XNOR */
"        if (!pass) discard;\n"
"    }\n"
"}\n";

/* Clear vertex/fragment shaders (full-screen quad, used for EFB clear) */
static const char *k_clear_vert_src =
GLSL_PROLOGUE
"layout(location=0) in vec2 a_pos;\n"
"void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

static const char *k_clear_frag_src =
GLSL_PROLOGUE
"uniform vec4 u_color;\n"
"out vec4 frag;\n"
"void main() { frag = u_color; }\n";

/* =========================================================================
 * GL state
 * ========================================================================= */

/* Defined further down, but called from the EFB-copy and present paths above
 * it. GCC tolerated the implicit declaration; Clang rejects it outright. */
void gx_batch_flush(void);

static int g_ogl_ready = 0;
uint32_t g_draw_calls = 0;
uint32_t g_frame_draw_calls = 0;
uint32_t g_frame_draw_calls_last = 0;   /* last completed frame (profiler) */
static SDL_GLContext g_gl_ctx = NULL;

/* Main draw shader */
static GLuint g_prog = 0;
static GLint  g_u_mvp          = -1;

/* Experiment: bypass the TEV uber-shader with a trivial fragment shader.
 * Set RECOMP_GPU_FLAT=1 (via robox_env.cfg on device). Keeps the SAME vertex
 * shader, geometry, blend and overdraw -- only the fragment ALU changes -- so
 * the profiler's `present` scope isolates uber-shader cost from fill/bandwidth.
 * Renders wrong on purpose; it is a measurement, not a mode. */
static GLuint g_prog_flat = 0;
static GLint  g_u_mvp_flat = -1;
static int    g_gpu_flat = -1;
static int gpu_flat(void) {
    if (g_gpu_flat < 0) {
        const char *e = getenv("RECOMP_GPU_FLAT");
        g_gpu_flat = (e && e[0] && e[0] != '0');
    }
    return g_gpu_flat;
}

/* Second probe: a MINIMAL REALISTIC shader -- one texture sample modulated by
 * vertex color, which is what the great majority of this game's draws actually
 * compute. RECOMP_GPU_SIMPLE=1.
 *
 * This is the decisive test for whether full C-side TEV codegen is worth
 * building. The flat probe (constant color, ~4 ms) proves the raster/blend/
 * overdraw floor is cheap, but it samples no textures. If THIS probe is also
 * fast, then a generated straight-line shader would be fast too and codegen is
 * the right investment. If it is slow, the remaining cost is texture bandwidth
 * or overdraw rather than shader ALU, and codegen would not help. */
static GLuint g_prog_simple = 0;
static GLint  g_u_mvp_simple = -1, g_u_tex0_simple = -1;
static int    g_gpu_simple = -1;
static int gpu_simple(void) {
    if (g_gpu_simple < 0) {
        const char *e = getenv("RECOMP_GPU_SIMPLE");
        g_gpu_simple = (e && e[0] && e[0] != '0');
    }
    return g_gpu_simple;
}
/* TEV uniforms */
static GLint  g_u_tex[8]       = {-1,-1,-1,-1,-1,-1,-1,-1};
static GLint  g_u_tex_en[8]    = {-1,-1,-1,-1,-1,-1,-1,-1};
static GLint  g_u_num_stages   = -1;
static GLint  g_u_tev_color    = -1;   /* int[8] uniform array base */
static GLint  g_u_tev_alpha    = -1;
static GLint  g_u_tev_order    = -1;
static GLint  g_u_tev_ksel     = -1;
static GLint  g_u_swap         = -1;   /* int[4]: swap tables r|g<<2|b<<4|a<<6 */
static GLint  g_u_konst_color  = -1;   /* vec4[4] uniform array base */
static GLint  g_u_tev_reg      = -1;   /* vec4[4]: TEV output-register init (prev/c0/c1/c2) */
static GLint  g_u_alpha_test   = -1;
static GLint  g_u_uv_from_pos  = -1;
static int    g_uv_from_pos;

/* ---------------------------------------------------------------------------
 * Draw-state key invalidation. Declared up here because the setters that bump
 * it live far above gx_draw_state_key() itself -- see the long comment there
 * for why the key is memoized at all.
 *
 * Bump ONLY when a hashed value actually changes: several of these setters run
 * per-primitive (gx_ogl_set_tex for all 8 texmaps on every draw) re-writing
 * values that are usually identical, and invalidating unconditionally would
 * cost strictly more than the hash it replaces.
 * ------------------------------------------------------------------------- */
extern uint32_t g_gx_state_gen;
#define GX_STATE_DIRTY() (++g_gx_state_gen)

/* -1 = consult RECOMP_GX_KEY_VERIFY; 0/1 = forced. Set by the web build before
 * the guest starts (sdk/robox_web_debug.c), which cannot use getenv. */
int g_gx_key_verify = -1;

/* Which g_bp[] registers the key hashes. MUST stay in sync with the MIX(g_bp[..])
 * lines in gx_draw_state_key_full(); RECOMP_GX_KEY_VERIFY=1 catches drift. */
#define GX_BP_REG_IS_HASHED(r) \
    ((r) == 0x00 || (r) == 0x28 || (r) == 0x40 || (r) == 0x41 || (r) == 0xF3)
static GLint  g_u_texmtx0     = -1;
static GLint  g_u_texmtx1     = -1;

/* Clear shader */
static GLuint g_clear_prog = 0;
static GLint  g_u_clear_color = -1;

/* VAO / VBO for streaming geometry */
static GLuint g_vao = 0;
static GLuint g_vbo = 0;

/* Write cursor into g_vbo. Draws sub-allocate forward through the buffer so the
 * CPU never overwrites vertices the GPU is still reading; the buffer is orphaned
 * only when the cursor wraps. See the upload site in the primitive path. */
static GLsizeiptr g_vbo_head = 0;
/* Cleared each present so the next frame starts with a fresh orphan. */
static int g_vbo_frame_open = 0;

/* Static index buffer that expands quads to triangles: for quad q the indices
 * are (4q, 4q+1, 4q+2) and (4q, 4q+2, 4q+3). Lets a batch of N quads draw as
 * ONE indexed call instead of N glDrawArrays. Unsigned short, so it covers
 * 65536/4 = 16384 quads. */
#define QUAD_IBO_MAX_QUADS 16384
static GLuint g_quad_ibo = 0;

/* ---------------------------------------------------------------------------
 * Diagnostic pixel-readback gate.
 *
 * Three probes ([BRIGHT] at present, [BRIGHT] scene-at-copy, [POST-DRAW]) each
 * do a glReadPixels and were throttled by WALL CLOCK to "once per second".
 * That is free at 60 fps and catastrophic when slow, for two compounding
 * reasons: on a tile-based GPU a mid-frame readback forces the tiler to flush
 * every queued job and the CPU to block (15-40 ms on a Mali-T760, versus
 * ~nothing on a desktop immediate-mode GPU), and a wall-clock throttle inside
 * the per-draw path can fire MULTIPLE times within a single slow frame.
 * So the probes got more expensive the slower the game ran -- they defended
 * themselves against exactly the measurement that would find them.
 *
 * Now off unless RECOMP_GX_PROBE=1, cached like recomp_gx_trace().
 * ------------------------------------------------------------------------- */
static int gx_probe_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GX_PROBE");
        on = (e && e[0] && e[0] != '0');
    }
    return on;
}
#define VBO_CAPACITY (1 << 16)  /* 64k vertices max per draw (ample) */

/* EFB framebuffer (offscreen render target).
 *
 * Native GameCube/Wii EFB is 640x480. We render the scene into a texture that
 * is (scale x) larger -- Dolphin's "internal resolution" -- and then let the
 * final present blit downscale it to the window. Because the game submits all
 * scene geometry in normalized clip space (there is no host-side viewport or
 * scissor; hle_GXSetScissor is a no-op and the projection matrix does the
 * framing), simply enlarging this render target rasterizes every polygon and
 * edge at the higher resolution -- crisp instead of 640x480-blocky -- with no
 * per-draw coordinate changes. Downscaling a 3x buffer to a smaller window is
 * supersampling, so it also antialiases for free.
 *
 * g_efb_w/h are the LIVE (scaled) dimensions used everywhere in this file;
 * EFB_NATIVE_W/H are the fixed GX pixel space that guest copy rectangles are
 * expressed in, so those must be multiplied by g_efb_scale before they index
 * the enlarged buffer (see gx_ogl_efb_copy_tex). */
#define EFB_NATIVE_W 640
#define EFB_NATIVE_H 480
#define EFB_SCALE_MAX 4
static GLuint g_efb_fbo  = 0;
static GLuint g_efb_col  = 0;  /* colour texture */
static GLuint g_efb_dep  = 0;  /* depth renderbuffer */
static int    g_efb_scale = 1; /* internal-resolution multiplier, 1..EFB_SCALE_MAX */
static int    g_efb_w    = EFB_NATIVE_W;
static int    g_efb_h    = EFB_NATIVE_H;

extern SDL_Window *g_window;
void  gx_ogl_layer_free(void);   /* fwd: drop the layer-dump scratch on resize */

/* Pick the starting internal resolution, before the EFB is allocated so it is
 * created at the right size. RECOMP_IR_SCALE (1..4) forces it; otherwise it is
 * chosen to roughly match the display height (480 * scale ~= panel height), the
 * same idea as Dolphin's "auto (window size)". Mobile/web default to native --
 * those are fill-rate bound where the desktop build is guest-CPU bound. */
static int gx_ogl_internal_scale_default(void) {
    const char *e = getenv("RECOMP_IR_SCALE");
    if (e && e[0]) {
        int s = atoi(e);
        return s < 1 ? 1 : (s > EFB_SCALE_MAX ? EFB_SCALE_MAX : s);
    }
    {   /* A choice made in the settings menu on a previous run. Beats the
         * panel-height heuristic, loses to the environment variable. */
        extern int video_cfg_get_ir_scale(void);
        int saved = video_cfg_get_ir_scale();
        if (saved >= 1 && saved <= EFB_SCALE_MAX) return saved;
    }
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
    return 1;
#else
    int hz_h = 0;
    SDL_DisplayMode dm;
    int disp = g_window ? SDL_GetWindowDisplayIndex(g_window) : 0;
    if (disp >= 0 && SDL_GetCurrentDisplayMode(disp, &dm) == 0) hz_h = dm.h;
    int s = hz_h > 0 ? (hz_h + EFB_NATIVE_H / 2) / EFB_NATIVE_H : 2;  /* round */
    if (s < 2) s = 2;                        /* a modern desktop wants at least 2x */
    if (s > EFB_SCALE_MAX) s = EFB_SCALE_MAX;
    return s;
#endif
}

/* Point g_efb_w/h at the scaled size for a given multiplier. Does not touch GL;
 * callers either allocate fresh (init) or resize the attachments (live). */
static void gx_ogl_apply_scale_dims(int scale) {
    if (scale < 1) scale = 1;
    if (scale > EFB_SCALE_MAX) scale = EFB_SCALE_MAX;
    g_efb_scale = scale;
    g_efb_w = EFB_NATIVE_W * scale;
    g_efb_h = EFB_NATIVE_H * scale;
}

int gx_ogl_get_internal_scale(void) { return g_efb_scale; }

/* Live change from the settings menu. Reallocates the EFB colour texture and
 * depth buffer at the new size; the next frame clears and redraws in full, so
 * there is no stale content. Must run on the GL thread (the menu event loop and
 * present share it). No-op until the renderer is up. */
void gx_ogl_set_internal_scale(int scale) {
    if (scale < 1) scale = 1;
    if (scale > EFB_SCALE_MAX) scale = EFB_SCALE_MAX;
    {   /* Remember it. Raising this for a capture and finding it back at the
         * default next launch is the kind of thing nobody reports as a bug and
         * everybody notices. */
        extern void video_cfg_set_ir_scale(int s);
        video_cfg_set_ir_scale(scale);
    }
    if (!g_ogl_ready || scale == g_efb_scale) { gx_ogl_apply_scale_dims(scale); return; }

    gx_batch_flush();                       /* nothing half-drawn into the old EFB */
    gx_ogl_apply_scale_dims(scale);

    glBindTexture(GL_TEXTURE_2D, g_efb_col);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_efb_w, g_efb_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindRenderbuffer(GL_RENDERBUFFER, g_efb_dep);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, g_efb_w, g_efb_h);

    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    glViewport(0, 0, g_efb_w, g_efb_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    gx_ogl_layer_free();                    /* its scratch is sized to the old EFB */
    fprintf(stderr, "[gx_ogl] internal resolution %dx (%dx%d)\n",
            g_efb_scale, g_efb_w, g_efb_h);
    fflush(stderr);
}

/* Full-screen quad VAO for clear / present */
static GLuint g_fsq_vao = 0;
static GLuint g_fsq_vbo = 0;

/* =========================================================================
 * GX state mirrored from the FIFO decoder
 * ========================================================================= */

#define N_POS_MTX   22   /* XF matrix memory rows 0..63 / 3: pos 0-9, tex mtx land at 10+ */
static float g_pos_mtx[N_POS_MTX][12];  /* [slot][row*4+col], 3×4 row-major */

/* Projection compact: [0..5] = 6 coefficients, [6] = 0 perspective / 1 ortho */
static float g_proj[7] = { 1,0,0,0,0,0, 0 };

/* BP registers (the ones we care about) */
static uint32_t g_bp[0x100];

/* =========================================================================
 * Texture cache
 * ========================================================================= */

#define TEX_CACHE_SIZE  256

typedef struct {
    uint32_t guest_va;
    uint32_t w, h, fmt;
    GLuint   gl_id;
    uint32_t lru_tick;
    uint32_t content_hash;   /* FNV of the guest bytes at upload time */
    uint32_t hash_frame;     /* frame stamp of the last hash check */
} TexCacheEntry;

/* Bumped once per present so each cached texture re-hashes at most once
 * per frame (full-coverage hashing is too hot to run per bind). */
static uint32_t g_tex_frame_stamp = 1;

static TexCacheEntry g_tex_cache[TEX_CACHE_SIZE];
static uint32_t      g_tex_lru = 0;

/* GxTexObj state from gx_ogl_set_tex */
typedef struct { uint32_t va, w, h, fmt, ws, wt; int valid; } OglTexObj;
static OglTexObj g_tex_obj[8];

/* =========================================================================
 * GX texture format decoding → RGBA8
 * All GX formats are stored as 4×4 (or 8×8 for CMPR) tiles in memory.
 * We decode them to a linear RGBA8 buffer for upload to OpenGL.
 * ========================================================================= */

static uint16_t g_swap16(uint16_t x) {
    return (x >> 8) | (x << 8);
}

static void decode_i4(uint8_t *dst, const uint8_t *src, int w, int h) {
    for (int ty = 0; ty < h; ty += 8) {
        for (int tx = 0; tx < w; tx += 8) {
            for (int y = ty; y < ty+8 && y < h; y++) {
                for (int x = tx; x < tx+8 && x < w; x++) {
                    int idx = (y-ty)*8 + (x-tx);
                    uint8_t nib = (idx & 1) ? (src[idx>>1] & 0x0f) : (src[idx>>1] >> 4);
                    uint8_t v = nib | (nib << 4);
                    uint8_t *p = dst + (y*w+x)*4;
                    p[0]=p[1]=p[2]=p[3]=v;
                }
            }
            src += 32;
        }
    }
}

static void decode_i8(uint8_t *dst, const uint8_t *src, int w, int h) {
    for (int ty = 0; ty < h; ty += 4) {
        for (int tx = 0; tx < w; tx += 8) {
            for (int y = ty; y < ty+4 && y < h; y++) {
                for (int x = tx; x < tx+8 && x < w; x++) {
                    uint8_t v = src[(y-ty)*8 + (x-tx)];
                    uint8_t *p = dst + (y*w+x)*4;
                    p[0]=p[1]=p[2]=p[3]=v;
                }
            }
            src += 32;
        }
    }
}

static void decode_ia4(uint8_t *dst, const uint8_t *src, int w, int h) {
    for (int ty = 0; ty < h; ty += 4) {
        for (int tx = 0; tx < w; tx += 8) {
            for (int y = ty; y < ty+4 && y < h; y++) {
                for (int x = tx; x < tx+8 && x < w; x++) {
                    uint8_t b = src[(y-ty)*8 + (x-tx)];
                    /* GX IA4: ALPHA in the high nibble, intensity low
                     * (matches Dolphin's decoder; was swapped here). */
                    uint8_t a = b >> 4; uint8_t i = b & 0xf;
                    uint8_t *p = dst + (y*w+x)*4;
                    p[0]=p[1]=p[2] = i|(i<<4);
                    p[3] = a|(a<<4);
                }
            }
            src += 32;
        }
    }
}

static void decode_ia8(uint8_t *dst, const uint8_t *src, int w, int h) {
    for (int ty = 0; ty < h; ty += 4) {
        for (int tx = 0; tx < w; tx += 4) {
            for (int y = ty; y < ty+4 && y < h; y++) {
                for (int x = tx; x < tx+4 && x < w; x++) {
                    const uint8_t *s = src + ((y-ty)*4+(x-tx))*2;
                    uint8_t a = s[0], i = s[1];
                    uint8_t *p = dst + (y*w+x)*4;
                    p[0]=p[1]=p[2]=i; p[3]=a;
                }
            }
            src += 32;
        }
    }
}

static void decode_rgb565(uint8_t *dst, const uint8_t *src, int w, int h) {
    for (int ty = 0; ty < h; ty += 4) {
        for (int tx = 0; tx < w; tx += 4) {
            for (int y = ty; y < ty+4 && y < h; y++) {
                for (int x = tx; x < tx+4 && x < w; x++) {
                    const uint8_t *s = src + ((y-ty)*4+(x-tx))*2;
                    uint16_t v = g_swap16((uint16_t)(s[0]|(s[1]<<8)));
                    uint8_t r = ((v>>11)&0x1f); r = (r<<3)|(r>>2);
                    uint8_t g = ((v>>5)&0x3f);  g = (g<<2)|(g>>4);
                    uint8_t b = (v&0x1f);        b = (b<<3)|(b>>2);
                    uint8_t *p = dst + (y*w+x)*4;
                    p[0]=r; p[1]=g; p[2]=b; p[3]=0xff;
                }
            }
            src += 32;
        }
    }
}

static void decode_rgb5a3(uint8_t *dst, const uint8_t *src, int w, int h) {
    for (int ty = 0; ty < h; ty += 4) {
        for (int tx = 0; tx < w; tx += 4) {
            for (int y = ty; y < ty+4 && y < h; y++) {
                for (int x = tx; x < tx+4 && x < w; x++) {
                    const uint8_t *s = src + ((y-ty)*4+(x-tx))*2;
                    uint16_t v = g_swap16((uint16_t)(s[0]|(s[1]<<8)));
                    uint8_t *p = dst + (y*w+x)*4;
                    if (v & 0x8000) { /* RGB555 opaque */
                        uint8_t r = ((v>>10)&0x1f); r=(r<<3)|(r>>2);
                        uint8_t g = ((v>>5)&0x1f);  g=(g<<3)|(g>>2);
                        uint8_t b = (v&0x1f);        b=(b<<3)|(b>>2);
                        p[0]=r; p[1]=g; p[2]=b; p[3]=0xff;
                    } else {         /* A3 RGB4 */
                        uint8_t a = ((v>>12)&7); a=(a<<5)|(a<<2)|(a>>1);
                        uint8_t r = ((v>>8)&0xf);  r=(r<<4)|r;
                        uint8_t g = ((v>>4)&0xf);  g=(g<<4)|g;
                        uint8_t b = (v&0xf);        b=(b<<4)|b;
                        p[0]=r; p[1]=g; p[2]=b; p[3]=a;
                    }
                }
            }
            src += 32;
        }
    }
}

static void decode_rgba8(uint8_t *dst, const uint8_t *src, int w, int h) {
    /* GX RGBA8 stores 4×4 tiles in two 32-byte AR/GB sub-blocks */
    for (int ty = 0; ty < h; ty += 4) {
        for (int tx = 0; tx < w; tx += 4) {
            const uint8_t *ar = src;
            const uint8_t *gb = src + 32;
            for (int y = ty; y < ty+4 && y < h; y++) {
                for (int x = tx; x < tx+4 && x < w; x++) {
                    int i = (y-ty)*4 + (x-tx);
                    uint8_t a = ar[i*2];
                    uint8_t r = ar[i*2+1];
                    uint8_t g = gb[i*2];
                    uint8_t b = gb[i*2+1];
                    uint8_t *p = dst + (y*w+x)*4;
                    p[0]=r; p[1]=g; p[2]=b; p[3]=a;
                }
            }
            src += 64;
        }
    }
}

/* CMPR (S3TC DXT1) — GX stores 2×2 blocks of DXT1 blocks in 8×8 tiles */
static uint32_t dxt1_color(uint16_t v) {
    uint8_t r = ((v>>11)&0x1f); r=(r<<3)|(r>>2);
    uint8_t g = ((v>>5)&0x3f);  g=(g<<2)|(g>>4);
    uint8_t b = (v&0x1f);        b=(b<<3)|(b>>2);
    return (0xff<<24)|(r<<16)|(g<<8)|b;
}

static void decode_cmpr(uint8_t *dst, const uint8_t *src, int w, int h) {
    /* Each 32-byte GX tile covers an 8×8 region as 4 DXT1 sub-blocks */
    for (int ty = 0; ty < h; ty += 8) {
        for (int tx = 0; tx < w; tx += 8) {
            for (int sub = 0; sub < 4; sub++) {
                int bx = tx + (sub & 1) * 4;
                int by = ty + (sub >> 1) * 4;
                const uint8_t *b = src + sub * 8;
                uint16_t c0 = g_swap16((uint16_t)(b[0]|(b[1]<<8)));
                uint16_t c1 = g_swap16((uint16_t)(b[2]|(b[3]<<8)));
                uint32_t lu = ((uint32_t)b[4]<<24)|((uint32_t)b[5]<<16)|
                               ((uint32_t)b[6]<<8)|b[7];
                /* Build 4-colour palette */
                uint32_t pal[4];
                pal[0] = dxt1_color(c0);
                pal[1] = dxt1_color(c1);
                if (c0 > c1) {
                    uint8_t *p0=(uint8_t*)(pal+0), *p1=(uint8_t*)(pal+1);
                    pal[2] = (0xff<<24) |
                             (((2*p0[2]+p1[2])/3)<<16)|
                             (((2*p0[1]+p1[1])/3)<<8)|
                             ((2*p0[0]+p1[0])/3);
                    pal[3] = (0xff<<24) |
                             (((p0[2]+2*p1[2])/3)<<16)|
                             (((p0[1]+2*p1[1])/3)<<8)|
                             ((p0[0]+2*p1[0])/3);
                } else {
                    uint8_t *p0=(uint8_t*)(pal+0), *p1=(uint8_t*)(pal+1);
                    pal[2] = (0xff<<24) |
                             (((p0[2]+p1[2])/2)<<16)|
                             (((p0[1]+p1[1])/2)<<8)|
                             ((p0[0]+p1[0])/2);
                    pal[3] = 0; /* transparent black */
                }
                for (int r = 0; r < 4; r++) {
                    uint8_t row = (lu >> (24 - r*8)) & 0xff;
                    for (int c = 0; c < 4; c++) {
                        int px = bx + c, py = by + r;
                        if (px >= w || py >= h) continue;
                        uint32_t col = pal[(row >> (6-c*2)) & 3];
                        uint8_t *p = dst + (py*w+px)*4;
                        p[0] = (col>>16)&0xff;
                        p[1] = (col>>8)&0xff;
                        p[2] = col&0xff;
                        p[3] = (col>>24)&0xff;
                    }
                }
            }
            src += 32;
        }
    }
}

// ---- PNG dump (uncompressed) so every loaded texture can be eyeballed ------
static uint32_t png_crc32(const uint8_t *d, size_t n) {
    static uint32_t tab[256]; static int init;
    if (!init) { for (uint32_t i=0;i<256;i++){uint32_t c=i;for(int k=0;k<8;k++)c=(c&1)?0xEDB88320u^(c>>1):c>>1;tab[i]=c;} init=1; }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i=0;i<n;i++) crc = tab[(crc^d[i])&0xFF]^(crc>>8);
    return crc ^ 0xFFFFFFFFu;
}
static uint32_t png_adler32(const uint8_t *d, size_t n) {
    uint32_t a=1,b=0; for(size_t i=0;i<n;i++){a=(a+d[i])%65521u;b=(b+a)%65521u;} return (b<<16)|a;
}
static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t l[4]={(uint8_t)(len>>24),(uint8_t)(len>>16),(uint8_t)(len>>8),(uint8_t)len};
    fwrite(l,1,4,f);
    uint8_t *buf = (uint8_t*)malloc(4+len);
    memcpy(buf,type,4); if(len) memcpy(buf+4,data,len);
    uint32_t crc = png_crc32(buf,4+len);
    fwrite(buf,1,4+len,f); free(buf);
    uint8_t c[4]={(uint8_t)(crc>>24),(uint8_t)(crc>>16),(uint8_t)(crc>>8),(uint8_t)crc};
    fwrite(c,1,4,f);
}
static void dump_texture_png(const char *path, const uint8_t *rgba, int w, int h) {
    size_t stride = 1 + (size_t)w*4;
    size_t rawlen = (size_t)h * stride;
    uint8_t *raw = (uint8_t*)malloc(rawlen);
    if (!raw) return;
    for (int y=0;y<h;y++){ raw[(size_t)y*stride]=0; memcpy(raw+(size_t)y*stride+1, rgba+(size_t)y*w*4, (size_t)w*4); }
    size_t nblk = (rawlen + 65534)/65535; if (!nblk) nblk=1;
    uint8_t *z = (uint8_t*)malloc(2 + rawlen + nblk*5 + 4);
    size_t zp=0; z[zp++]=0x78; z[zp++]=0x01;
    size_t off=0;
    do { size_t blk = rawlen-off; if (blk>65535) blk=65535;
         z[zp++] = (off+blk>=rawlen)?1:0;
         z[zp++]=blk&0xff; z[zp++]=(blk>>8)&0xff;
         uint16_t nl=(uint16_t)~(uint16_t)blk; z[zp++]=nl&0xff; z[zp++]=(nl>>8)&0xff;
         memcpy(z+zp, raw+off, blk); zp+=blk; off+=blk;
    } while (off<rawlen);
    uint32_t ad=png_adler32(raw,rawlen);
    z[zp++]=ad>>24; z[zp++]=ad>>16; z[zp++]=ad>>8; z[zp++]=ad;
    free(raw);
    FILE *f=fopen(path,"wb"); if(!f){free(z);return;}
    static const uint8_t sig[8]={0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a};
    fwrite(sig,1,8,f);
    uint8_t ihdr[13]={(uint8_t)(w>>24),(uint8_t)(w>>16),(uint8_t)(w>>8),(uint8_t)w,
                      (uint8_t)(h>>24),(uint8_t)(h>>16),(uint8_t)(h>>8),(uint8_t)h,
                      8,6,0,0,0};
    png_chunk(f,"IHDR",ihdr,13);
    png_chunk(f,"IDAT",z,(uint32_t)zp);
    png_chunk(f,"IEND",NULL,0);
    fclose(f); free(z);
}

/* Read access to the BP shadow for diagnostics (peripherals.c draw dump). */
uint32_t gx_ogl_get_bp(uint32_t reg) { return reg < 0x100 ? g_bp[reg] : 0; }

/* Diagnostics accessors live below the state they read (see
 * gx_ogl_get_tex0_mtx / gx_ogl_get_tev_colors further down). */

/* ---- EFB->texture copy registry -----------------------------------------
 * GXCopyTex destinations, keyed by guest VA. When the game later binds a
 * texture at one of these VAs (shadow maps, reflections, refraction), the
 * texture cache returns the live GL texture instead of decoding the guest
 * memory (which we never fill). */
typedef struct { uint32_t va; GLuint tex; GLuint fbo; int w, h; } EfbCopyTex;
static EfbCopyTex g_efbcopy[32];
static GLuint gx_ogl_efbcopy_lookup(uint32_t va) {
    for (int i = 0; i < 32; i++)
        if (g_efbcopy[i].va == va) return g_efbcopy[i].tex;
    return 0;
}
void gx_ogl_efb_copy_tex(uint32_t dst_va, uint32_t sx, uint32_t sy,
                         uint32_t w, uint32_t h, int half_scale) {
    if (!g_ogl_ready || !w || !h) return;
    int dw = half_scale ? (int)w / 2 : (int)w;
    int dh = half_scale ? (int)h / 2 : (int)h;
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    EfbCopyTex *e = NULL;
    for (int i = 0; i < 32 && !e; i++)
        if (g_efbcopy[i].va == dst_va) e = &g_efbcopy[i];
    for (int i = 0; i < 32 && !e; i++)
        if (g_efbcopy[i].va == 0) e = &g_efbcopy[i];
    if (!e) e = &g_efbcopy[0];              /* wrap: reuse slot 0 */
    if (!e->tex) { glGenTextures(1, &e->tex); glGenFramebuffers(1, &e->fbo); }
    if (e->w != dw || e->h != dh || e->va != dst_va) {
        glBindTexture(GL_TEXTURE_2D, e->tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dw, dh, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        e->w = dw; e->h = dh; e->va = dst_va;
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, e->fbo);
    /* The EFB is about to be read: make sure every batched draw is in it. */
    gx_batch_flush();
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, e->tex, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_efb_fbo);
    /* GX copy rect is top-left based, GL FBO rows are bottom-up: flip the
     * source so the texture samples upright (v=0 = top, matching guest-
     * decoded textures).
     *
     * sx/sy/w/h arrive in NATIVE 640x480 GX pixels, so scale them up to index
     * the enlarged EFB. The destination stays native-sized (dw/dh), which makes
     * this a supersampling downscale of the region -- the copied texture is as
     * crisp as the higher internal resolution allows without changing the size
     * the game expects. */
    const int S = g_efb_scale;
    glBlitFramebuffer((GLint)(sx * S), (GLint)(g_efb_h - sy * S),
                      (GLint)((sx + w) * S), (GLint)(g_efb_h - (sy + h) * S),
                      0, 0, dw, dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    /* [BRIGHT] pipeline-split probe (once/sec, big copies only): average
     * brightness of the EFB *as copied* — i.e. the rendered scene BEFORE the
     * post pass consumes it. Compared against the present-time probe this
     * pins down exactly where black enters the frame. */
    if (gx_probe_enabled() && dw >= 320) {
        static uint64_t s_t;
        extern uint64_t ms_now(void);
        uint64_t now = ms_now();
        if (now - s_t >= 1000) {
            s_t = now;
            uint8_t px[64 * 64 * 4];
            glBindFramebuffer(GL_READ_FRAMEBUFFER, e->fbo);
            glReadPixels(dw / 2 - 32, dh / 2 - 32, 64, 64, GL_RGBA,
                         GL_UNSIGNED_BYTE, px);
            uint32_t sum = 0;
            for (int i = 0; i < 64 * 64; ++i)
                sum += px[i*4] + px[i*4+1] + px[i*4+2];
            fprintf(stderr, "[BRIGHT] scene-at-copy dst=0x%08x %dx%d avg=%u/255\n",
                    dst_va, dw, dh, sum / (64u * 64u * 3u));
            fflush(stderr);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
}

/* Standalone EFB clear (copy command's clear bit on a texture copy). */
/* Clear-colour override, for mods (sdk/robox_lua_api.c -> robox.video.clear).
 *
 * Both places that paint the whole EFB a flat colour run the guest's ARGB
 * through here first. Nothing in the port sets it -- a normal run is bit-for-
 * bit what the game asked for -- but a mod that moves the player outside the
 * level needs it: with no geometry left, the only thing on screen IS the clear
 * colour, and Robox's is 0x802b94, purple. An overlay rectangle cannot fix
 * that, because the overlay draws over the finished frame and would cover the
 * player along with the background. The colour has to change before the game
 * draws, which is here.
 *
 * -1 = no override, pass the guest's value through untouched. */
static int64_t g_clear_override = -1;

void gx_ogl_set_clear_override(int64_t argb_or_minus1) {
    g_clear_override = argb_or_minus1;
}

static uint32_t apply_clear_override(uint32_t guest_argb) {
    return g_clear_override < 0 ? guest_argb : (uint32_t)g_clear_override;
}

void gx_ogl_efb_clear(uint32_t clear_argb) {
    clear_argb = apply_clear_override(clear_argb);
    if (!g_ogl_ready) return;
    /* Report every distinct clear colour the game asks for. Boot flashes a
     * colour for about a second before the first real frame and the only way
     * to identify it is to see the value and when it is set. */
    {
        static uint32_t last = 0xDEADBEEFu;
        if (clear_argb != last) {
            last = clear_argb;
            fprintf(stderr, "[EFB-CLEAR] argb=0x%08x  (r=%u g=%u b=%u a=%u)\n",
                    clear_argb, (clear_argb >> 16) & 0xff, (clear_argb >> 8) & 0xff,
                    clear_argb & 0xff, (clear_argb >> 24) & 0xff);
            fflush(stderr);
        }
    }
    float r = ((clear_argb >> 16) & 0xff) / 255.0f;
    float g_ = ((clear_argb >>  8) & 0xff) / 255.0f;
    float b_ = ( clear_argb        & 0xff) / 255.0f;
    float a  = ((clear_argb >> 24) & 0xff) / 255.0f;
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    glClearColor(r, g_, b_, a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static void decode_gx_texture(uint8_t *dst, const uint8_t *src,
                               int w, int h, int fmt) {
    switch (fmt) {
        case 0x0: decode_i4(dst, src, w, h);      break;
        case 0x1: decode_i8(dst, src, w, h);      break;
        case 0x2: decode_ia4(dst, src, w, h);     break;
        case 0x3: decode_ia8(dst, src, w, h);     break;
        case 0x4: decode_rgb565(dst, src, w, h);  break;
        case 0x5: decode_rgb5a3(dst, src, w, h);  break;
        case 0x6: decode_rgba8(dst, src, w, h);   break;
        case 0xe: decode_cmpr(dst, src, w, h);    break;
        default: {
            /* Unknown format: fill solid magenta to make it obvious */
            static unsigned s_warned;
            if (s_warned++ < 8) {
                fprintf(stderr, "[gx_ogl] UNSUPPORTED texture fmt=%u (%dx%d) -> magenta\n",
                        fmt, w, h);
                fflush(stderr);
            }
            for (int i = 0; i < w*h*4; i+=4) {
                dst[i]=0xff; dst[i+1]=0; dst[i+2]=0xff; dst[i+3]=0xff;
            }
            break;
        }
    }
}

extern void *ppc_host_ptr(uint32_t guest_va);
// Shared: decode a guest GX texture and dump it as PNG (deduped by VA, capped).
// Called from BOTH draw-time upload AND GXInitTexObj (texture creation) so we
// catch textures the game loads into memory even if it never draws them.
void gx_ogl_dump_tex(uint32_t va, uint32_t w, uint32_t h, uint32_t fmt) {
    /* Opt-in with ROBOX_DUMPS, like the RAM and EFB dumps.
     *
     * This one hid for a while: it wrote to an absolute path on a machine that
     * no longer existed, so it silently failed and looked disabled. Correcting
     * the path to logs/tex_dumps/ turned it back on, and it then dropped a PNG
     * for every unique texture the game touched -- up to its 4096 cap -- on an
     * ordinary launch. */
    extern int robox_debug_dumps_wanted(void);
    if (!robox_debug_dumps_wanted()) return;
    if (!va || w == 0 || h == 0 || w > 2048 || h > 2048) return;
    static uint32_t s_dumped[4096]; static int s_nd; static int s_dirok;
    for (int i = 0; i < s_nd; ++i) if (s_dumped[i] == va) return;
    if (s_nd >= 4096) return;
    const uint8_t *src = (const uint8_t*)ppc_host_ptr(va);
    if (!src) return;
    uint8_t *rgba = (uint8_t*)malloc((size_t)w * h * 4);
    if (!rgba) return;
    decode_gx_texture(rgba, src, (int)w, (int)h, (int)fmt);
    /* Every debug dump in this file writes under logs/, relative to the working
     * directory. They used to be absolute paths into a different project on one
     * machine: an undefined symbol when linking for Android, and silently
     * nowhere for anybody else. */
    if (!s_dirok) { robox_mkdir("logs"); robox_mkdir("logs/tex_dumps"); s_dirok = 1; }
    s_dumped[s_nd++] = va;
    char path[192];
    snprintf(path, sizeof path,
        "logs/tex_dumps/tex_%04d_va%08x_%ux%u_fmt%u.png",
        s_nd, va, w, h, fmt);
    dump_texture_png(path, rgba, (int)w, (int)h);
    free(rgba);
}

/* =========================================================================
 * Texture cache lookup / upload
 * ========================================================================= */

/* Provided by runtime.h / ppc_host_ptr */
extern void *ppc_host_ptr(uint32_t guest_va);

/* Guest byte size of a texture image (enough for hashing). */
static size_t gx_tex_bytes(uint32_t w, uint32_t h, uint32_t fmt) {
    size_t np = (size_t)w * h;
    switch (fmt) {
        case 0x0: case 0x8: case 0xe: return np / 2;   /* I4 / C4 / CMPR */
        case 0x1: case 0x2: case 0x9: return np;       /* I8 / IA4 / C8 */
        case 0x3: case 0x4: case 0x5: case 0xa: return np * 2;
        case 0x6: return np * 4;                       /* RGBA8 */
        default:  return np;
    }
}

/* Content hash: sampled FNV-1a covering the ENTIRE image (dense first 1KB,
 * then stride 64). The old head+tail sampling missed mid-buffer rewrites —
 * LyN progressively CPU-bakes glyph/logo art into live atlases and the
 * cache served the stale first upload forever (solid-yellow logo blocks,
 * white icon cards, invisible text labels, hard banner edges). Cost is
 * bounded by the per-frame hash stamp in tex_cache_get. */
static uint32_t gx_tex_hash(const uint8_t *p, size_t sz) {
    uint32_t hh = 2166136261u;
    size_t head = sz < 1024 ? sz : 1024;
    for (size_t i = 0; i < head; i += 4)
        hh = (hh ^ p[i]) * 16777619u;
    for (size_t i = head; i < sz; i += 64)
        hh = (hh ^ p[i]) * 16777619u;
    return hh ^ (uint32_t)sz;
}

static GLuint tex_cache_get(uint32_t va, uint32_t w, uint32_t h, uint32_t fmt,
                              uint32_t wrap_s, uint32_t wrap_t) {
    if (!va || !w || !h) return 0;
    g_tex_lru++;

    /* Live EFB-copy target? Return the GL-side render texture directly. */
    {
        GLuint ct = gx_ogl_efbcopy_lookup(va);
        if (ct) return ct;
    }

    const uint8_t *src_probe = ppc_host_ptr(va);
    uint32_t chash = 0;
    int chash_done = 0;

    /* Search for existing entry. Re-hash a hit at most once per frame
     * (stamp) — the full-coverage hash is too hot to run per bind. */
    int best_lru = -1; uint32_t best_tick = 0xffffffff;
    for (int i = 0; i < TEX_CACHE_SIZE; i++) {
        if (g_tex_cache[i].guest_va == va && g_tex_cache[i].w == w &&
            g_tex_cache[i].h == h && g_tex_cache[i].fmt == fmt) {
            if (g_tex_cache[i].hash_frame == g_tex_frame_stamp) {
                g_tex_cache[i].lru_tick = g_tex_lru;
                return g_tex_cache[i].gl_id;
            }
            chash = src_probe ? gx_tex_hash(src_probe, gx_tex_bytes(w, h, fmt)) : 0;
            chash_done = 1;
            if (g_tex_cache[i].content_hash != chash) {
                /* game rewrote the texture memory: drop and re-upload */
                if (g_tex_cache[i].gl_id) glDeleteTextures(1, &g_tex_cache[i].gl_id);
                g_tex_cache[i].guest_va = 0;
                best_lru = i;
                break;
            }
            g_tex_cache[i].lru_tick = g_tex_lru;
            g_tex_cache[i].hash_frame = g_tex_frame_stamp;
            return g_tex_cache[i].gl_id;
        }
        if (g_tex_cache[i].guest_va == 0 || g_tex_cache[i].lru_tick < best_tick) {
            best_tick = g_tex_cache[i].lru_tick;
            best_lru  = i;
        }
    }
    if (!chash_done)
        chash = src_probe ? gx_tex_hash(src_probe, gx_tex_bytes(w, h, fmt)) : 0;

    /* Evict LRU / empty slot and upload */
    TexCacheEntry *e = &g_tex_cache[best_lru];
    if (e->gl_id) { glDeleteTextures(1, &e->gl_id); e->gl_id = 0; }

    const uint8_t *src = ppc_host_ptr(va);
    if (!src) return 0;

    uint8_t *rgba = (uint8_t*)malloc((size_t)w * h * 4);
    if (!rgba) return 0;
    decode_gx_texture(rgba, src, (int)w, (int)h, (int)fmt);

    gx_ogl_dump_tex(va, w, h, fmt);   /* dump drawn textures */

    {   /* INVESTIGATE: decoded-content check (avg RGB + alpha) */
        static int s_n;
        if (s_n < 24) {
            uint64_t sr=0, sg=0, sb=0, sa=0;
            size_t np = (size_t)w * h;
            for (size_t i = 0; i < np; ++i) {
                sr += rgba[i*4]; sg += rgba[i*4+1]; sb += rgba[i*4+2]; sa += rgba[i*4+3];
            }
            fprintf(stderr, "[TEX-UP#%d] va=0x%08x %ux%u fmt=%u avgRGBA=%u,%u,%u,%u\n",
                    s_n, va, w, h, fmt,
                    (unsigned)(sr/np), (unsigned)(sg/np), (unsigned)(sb/np), (unsigned)(sa/np));
            fflush(stderr);
            s_n++;
        }
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    GLint ws = (wrap_s == 1) ? GL_REPEAT : (wrap_s == 2) ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE;
    GLint wt = (wrap_t == 1) ? GL_REPEAT : (wrap_t == 2) ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ws);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(rgba);

    e->guest_va = va;
    e->w = w; e->h = h; e->fmt = fmt;
    e->gl_id = tex;
    e->lru_tick = g_tex_lru;
    e->content_hash = chash;
    e->hash_frame = g_tex_frame_stamp;
    return tex;
}

/* =========================================================================
 * Shader compilation helpers
 * ========================================================================= */

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048]; GLsizei len;
        glGetShaderInfoLog(s, sizeof(log), &len, log);
        fprintf(stderr, "[gx_ogl] shader compile error:\n%s\n", log);
    }
    return s;
}

static GLuint link_program(const char *vsrc, const char *fsrc) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   vsrc);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fsrc);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048]; GLsizei len;
        glGetProgramInfoLog(prog, sizeof(log), &len, log);
        fprintf(stderr, "[gx_ogl] program link error:\n%s\n", log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

/* =========================================================================
 * MVP matrix construction from GX XF state
 * ========================================================================= */

static void build_mvp(int mtx_idx, float out[16]) {
    /* Perspective projection from GX compact params [p0..p5, ortho] */
    float P[16] = {0};
    float *p = g_proj;
    /* TEMP(widescreen): census of distinct projections the game sets. */
    { static float seen[16][7]; static int ns;
      int dup = 0;
      for (int i = 0; i < ns && !dup; ++i) {
          dup = 1;
          for (int k = 0; k < 7; ++k)
              if (seen[i][k] != p[k]) { dup = 0; break; }
      }
      if (!dup && ns < 16) {
          for (int k = 0; k < 7; ++k) seen[ns][k] = p[k];
          ++ns;
          fprintf(stderr, "[PROJ] type=%d p=[%g %g %g %g %g %g]\n",
                  (int)p[6], p[0], p[1], p[2], p[3], p[4], p[5]);
          fflush(stderr);
      } }
    if ((int)p[6] == 0) { /* perspective */
        /* GX perspective: p0=A=2n/(r-l), p1=B=(r+l)/(r-l)... simplified: */
        P[0]  =  p[0];           /* X scale */
        P[5]  =  p[2];           /* Y scale */
        P[8]  =  p[1];           /* X offset */
        P[9]  =  p[3];           /* Y offset */
        P[10] =  p[4];           /* Z scale (near/far) */
        P[11] = -1.0f;           /* -W for perspective divide */
        P[14] =  p[5];           /* Z offset */
    } else { /* orthographic */
        /* NOTE (widescreen): do NOT scale X here. Robox has NATIVE 16:9
         * support — it asks SCGetAspectRatio (0x80147ec0) and renders a
         * widescreen frame itself. That function was auto-bound to a nop
         * returning 0 (=4:3), which is what forced the game into 4:3 (and
         * its 4:3 camera-pans-to-see-ahead behaviour); it is now bound in
         * hle_registry.json. The game renders anamorphic like real hardware
         * and gx_ogl_present's 16:9 un-squish finishes the job. */
        P[0]  =  p[0];
        P[5]  =  p[2];
        P[10] =  p[4];
        P[12] =  p[1];
        P[13] =  p[3];
        P[14] =  p[5];
        P[15] =  1.0f;
    }

    /* Modelview from xf_pos_mtx[slot/3] — it's a 3×4 matrix, expand to 4×4 */
    const float *m = g_pos_mtx[mtx_idx < N_POS_MTX ? mtx_idx : 0];
    float MV[16] = {
        m[0], m[4], m[8],  0,
        m[1], m[5], m[9],  0,
        m[2], m[6], m[10], 0,
        m[3], m[7], m[11], 1,
    };

    /* MVP = P * MV. P and MV are both laid out COLUMN-major (GL order:
     * translation in [12..14]); the product must use column-major indexing
     * too. The old row-major loop effectively computed (MV*P)^T — x/y
     * survived by symmetry but Z translation got scaled by the modelview,
     * pushing every vertex past the near plane (whole scene clipped). */
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += P[k*4+r] * MV[c*4+k];
            out[c*4+r] = v;
        }
    }

    /* Camera assist (robox_mario.c): a post-projection NDC shift -- exact
     * for both ortho and perspective (x' += s*w) -- applied only when the
     * current projection is the one the player renders with, so the HUD and
     * menus (their own projections) never move. Shift changes bump
     * g_gx_state_gen in gx_ogl_cam_assist, keeping the draw-key memo honest. */
    if ((g_cam_shift_x != 0.0f || g_cam_shift_y != 0.0f) && g_cam_world_proj_ok) {
        int same = 1;
        for (int i = 0; i < 7; i++)
            if (g_proj[i] != g_cam_world_proj[i]) { same = 0; break; }
        if (same) {
            for (int c = 0; c < 4; c++) {
                out[c*4+0] += g_cam_shift_x * out[c*4+3];
                out[c*4+1] += g_cam_shift_y * out[c*4+3];
            }
        }
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

/* ---------------------------------------------------------------------------
 * TEV shader specialisation.
 *
 * The uber-shader was ~97% of the Android GPU frame (measured: present 190 ms
 * -> 4 ms with a trivial fragment shader). It is one program, register-allocated
 * for the 8-stage worst case, so every draw ran at Midgard's lowest occupancy.
 *
 * The cheapest large win is to specialise on STAGE COUNT: compile one program
 * per num_stages (1..8) with ROBOX_NUM_STAGES fixed, so the driver unrolls the
 * TEV loop and frees the dead output registers. The combiner MATH is byte
 * identical across variants -- only the loop bound differs -- so this cannot
 * change rendering, it only changes register pressure. Variants are compiled
 * lazily (a scene touches only a few stage counts) and cached.
 *
 * Each variant caches its own uniform locations, since GLES 3.0 has no explicit
 * uniform locations and the linker may place them differently per program.
 * select_tev_variant() points the existing g_prog / g_u_* globals at the chosen
 * variant, so all the per-draw uniform-setting code stays unchanged.
 * ------------------------------------------------------------------------- */
/* Structural TEV state -- everything that changes the shader's SHAPE (and thus
 * which selector branches are live), as opposed to value uniforms (colors,
 * matrices) that change without changing shape. Two draws with the same TevState
 * can share a specialised program. */
typedef struct {
    int   num_stages;
    GLint tev_color[8], tev_alpha[8], tev_order[8], tev_ksel[8], swap[4];
} TevState;

/* Repack g_bp into the shader-local TEV layout. This is the SINGLE source of
 * truth for both the specialised-program key/preamble and the uniform upload in
 * setup_tev_uniforms, so the two can never disagree. Must mirror the GX->shader
 * bit layout documented in setup_tev_uniforms. */
static void compute_tev_state(TevState *st) {
    int ns = (int)(((g_bp[0x00] >> 10) & 0xF) + 1);
    if (ns < 1) ns = 1; if (ns > 8) ns = 8;
    st->num_stages = ns;
    for (int s = 0; s < ns; s++) {
        uint32_t cw = g_bp[0xC0 + 2*s], aw = g_bp[0xC1 + 2*s];
        st->tev_color[s] = (GLint)((cw & 0x00FF0000u)
                     | (((cw >> 12) & 0xFu) << 0) | (((cw >> 8) & 0xFu) << 4)
                     | (((cw >> 4) & 0xFu) << 8)  | ((cw & 0xFu) << 12));
        st->tev_alpha[s] = (GLint)((aw & 0x00FF0000u) | (aw & 0xFu)
                     | (((aw >> 13) & 0x7u) << 4) | (((aw >> 10) & 0x7u) << 7)
                     | (((aw >> 7) & 0x7u) << 10) | (((aw >> 4) & 0x7u) << 13));
        uint32_t tref = g_bp[0x28 + s/2];
        st->tev_order[s] = (GLint)((tref >> ((s & 1) ? 12 : 0)) & 0xFFF);
        uint32_t ksel_reg = g_bp[0xF6 + s/2];
        int sh = (s & 1) ? 8 : 0;
        st->tev_ksel[s] = (GLint)(((ksel_reg >> sh) & 0x1F)
                     | (((ksel_reg >> (sh + 5)) & 0x1F) << 5));
    }
    for (int s = ns; s < 8; s++) {
        st->tev_color[s] = st->tev_alpha[s] = st->tev_order[s] = st->tev_ksel[s] = 0;
    }
    for (int t = 0; t < 4; t++) {
        uint32_t rg = g_bp[0xF6 + 2*t], ba = g_bp[0xF7 + 2*t];
        st->swap[t] = (GLint)((rg & 3) | (((rg >> 2) & 3) << 2)
                            | ((ba & 3) << 4) | (((ba >> 2) & 3) << 6));
    }
}

static uint32_t tev_state_hash(const TevState *st) {
    uint32_t h = 2166136261u;
    const unsigned char *p = (const unsigned char *)st;
    /* Only the live portion matters, but unused stages are zeroed so hashing
     * the whole struct is stable. */
    for (size_t i = 0; i < sizeof *st; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

typedef struct {
    uint32_t hash; int used;
    GLuint prog;
    GLint  u_mvp, u_konst_color, u_tev_reg, u_alpha_test, u_uv_from_pos,
           u_texmtx0, u_texmtx1, u_tex[8], u_tex_en[8];
    /* Structural uniforms are const in a specialised program, so their
     * locations are -1 and setting them is a silent no-op -- kept only so the
     * shared g_u_* globals have somewhere to point. */
    GLint  u_num_stages, u_tev_color, u_tev_alpha, u_tev_order, u_tev_ksel, u_swap;
} TevVariant;

#define TEV_CACHE_SIZE 128           /* bounds programs; gameplay has tens */
static TevVariant g_tv[TEV_CACHE_SIZE];
static int        g_tv_count = 0;
static uint32_t   g_tv_cur = 0;      /* hash currently bound (0 = none) */

/* Emit `const int name[n] = int[n](v0,...);` into buf. */
static int emit_const_iarray(char *buf, size_t cap, const char *name,
                             const GLint *v, int n) {
    int off = snprintf(buf, cap, "const int %s[%d] = int[%d](", name, n, n);
    for (int i = 0; i < n && off < (int)cap; i++)
        off += snprintf(buf + off, cap - off, "%s%d", i ? "," : "", (int)v[i]);
    if (off < (int)cap) off += snprintf(buf + off, cap - off, ");\n");
    return off;
}

/* ---------------------------------------------------------------------------
 * TEV codegen.
 *
 * Emitting the structural words as `const` arrays was not enough: this Mali
 * r15p0 compiler does not fold indexing into them, so the big selector chains
 * (get_konst 32-way, cc_in 15-way x4, ca_in x4, sample_tex 8-way) stayed live
 * and the menu only went 4.4 -> 13 fps.
 *
 * So resolve every selector HERE, in C, and emit straight-line GLSL that
 * contains only the operations the stage actually performs. Nothing is left for
 * the shader compiler to figure out.
 *
 * Measured ceiling for this approach: a hand-written "texture x vertex colour"
 * shader runs the same 346-draw menu with present at 3-4 ms (vs 190 ms), i.e.
 * 31 fps and CPU-bound -- so a generated shader of similar size should land
 * near there.
 *
 * Compare-mode stages (bias==3) are rare and need bit-exact integer semantics,
 * so those fall back to calling the existing tev_color/tev_alpha with a literal
 * packed word rather than being inlined.
 * ------------------------------------------------------------------------- */
static const char *k_cc_in[16] = {
    "tev_prev.rgb", "vec3(tev_prev.a)", "tev_r0.rgb", "vec3(tev_r0.a)",
    "tev_r1.rgb",   "vec3(tev_r1.a)",   "tev_r2.rgb", "vec3(tev_r2.a)",
    "texc.rgb",     "vec3(texc.a)",     "ras.rgb",    "vec3(ras.a)",
    "vec3(1.0)",    "vec3(0.5)",        "konst",      "vec3(0.0)"
};
static const char *k_ca_in[8] = {
    "tev_prev.a", "tev_r0.a", "tev_r1.a", "tev_r2.a",
    "texc.a",     "ras.a",    "konst_a",  "0.0"
};
static const char *k_chan[4] = { ".r", ".g", ".b", ".a" };
static const char *k_dst[4]  = { "tev_prev", "tev_r0", "tev_r1", "tev_r2" };

/* GX konst selectors -> GLSL expression (see get_konst_rgb/get_konst_a). */
static void konst_rgb_expr(int sel, char *o, size_t n) {
    if (sel <= 7)                 snprintf(o, n, "vec3(%.6f)", 1.0 - 0.125 * sel);
    else if (sel >= 12 && sel<=15)snprintf(o, n, "u_konst_color[%d].rgb", sel-12);
    else if (sel >= 16 && sel<=19)snprintf(o, n, "vec3(u_konst_color[%d].r)", sel-16);
    else if (sel >= 20 && sel<=23)snprintf(o, n, "vec3(u_konst_color[%d].g)", sel-20);
    else if (sel >= 24 && sel<=27)snprintf(o, n, "vec3(u_konst_color[%d].b)", sel-24);
    else if (sel >= 28 && sel<=31)snprintf(o, n, "vec3(u_konst_color[%d].a)", sel-28);
    else                          snprintf(o, n, "vec3(0.0)");
}
static void konst_a_expr(int sel, char *o, size_t n) {
    if (sel <= 7)                 snprintf(o, n, "%.6f", 1.0 - 0.125 * sel);
    else if (sel >= 16 && sel<=19)snprintf(o, n, "u_konst_color[%d].r", sel-16);
    else if (sel >= 20 && sel<=23)snprintf(o, n, "u_konst_color[%d].g", sel-20);
    else if (sel >= 24 && sel<=27)snprintf(o, n, "u_konst_color[%d].b", sel-24);
    else if (sel >= 28 && sel<=31)snprintf(o, n, "u_konst_color[%d].a", sel-28);
    else                          snprintf(o, n, "0.0");
}

#define GEN(...) do { off += snprintf(buf+off, (off<(int)cap)?cap-off:0, __VA_ARGS__); } while (0)

static int gen_tev_main(const TevState *st, char *buf, size_t cap) {
    int off = 0;
    GEN("void main() {\n"
        "  vec4 tev_prev = u_tev_reg[0];\n  vec4 tev_r0 = u_tev_reg[1];\n"
        "  vec4 tev_r1 = u_tev_reg[2];\n  vec4 tev_r2 = u_tev_reg[3];\n"
        "  vec3 last_rgb = tev_prev.rgb; float last_a = tev_prev.a;\n");

    for (int s = 0; s < st->num_stages; s++) {
        int ord = st->tev_order[s], apk = st->tev_alpha[s], cpk = st->tev_color[s];
        int ksel = st->tev_ksel[s];
        int tmap = ord & 7, ten = (ord >> 6) & 1, rchan = (ord >> 7) & 7;
        int tsw = st->swap[(apk >> 2) & 3], rsw = st->swap[apk & 3];

        GEN("  {\n");
        /* Texture fetch: slot resolved; the "is a texture bound" test stays
         * dynamic because it depends on per-draw binds, not on TEV state. */
        if (ten) GEN("    vec4 texc = (u_tex_en[%d] != 0) ? texture(u_tex[%d], v_tex) : vec4(0.0);\n", tmap, tmap);
        else     GEN("    vec4 texc = vec4(0.0);\n");
        GEN("    vec4 ras = %s;\n", (rchan <= 1) ? "v_color" : "vec4(0.0)");
        /* Channel swaps, resolved to swizzles. */
        GEN("    texc = vec4(texc%s, texc%s, texc%s, texc%s);\n",
            k_chan[tsw & 3], k_chan[(tsw>>2)&3], k_chan[(tsw>>4)&3], k_chan[(tsw>>6)&3]);
        GEN("    ras = vec4(ras%s, ras%s, ras%s, ras%s);\n",
            k_chan[rsw & 3], k_chan[(rsw>>2)&3], k_chan[(rsw>>4)&3], k_chan[(rsw>>6)&3]);

        char kr[64], ka[64];
        konst_rgb_expr(ksel & 0x1F, kr, sizeof kr);
        konst_a_expr((ksel >> 5) & 0x1F, ka, sizeof ka);
        GEN("    vec3 konst = %s; float konst_a = %s;\n", kr, ka);

        /* ---- colour ---- */
        int cbias = (cpk>>16)&3, cop = (cpk>>18)&1, cclamp = (cpk>>19)&1, csc = (cpk>>20)&3;
        if (cbias == 3) {
            GEN("    vec3 nrgb = tev_color(%d, texc, ras, tev_prev, tev_r0, tev_r1, tev_r2, konst);\n", cpk);
        } else {
            const char *A = k_cc_in[cpk & 0xF], *B = k_cc_in[(cpk>>4)&0xF];
            const char *C = k_cc_in[(cpk>>8)&0xF], *D = k_cc_in[(cpk>>12)&0xF];
            double bo = (cbias==1) ? 0.5 : (cbias==2) ? -0.5 : 0.0;
            double sc = (csc==1) ? 2.0 : (csc==2) ? 4.0 : (csc==3) ? 0.5 : 1.0;
            GEN("    vec3 nrgb = clamp(((%s)%s%s %s mix((%s),(%s),(%s)))%s, vec3(%s), vec3(%s));\n",
                D,
                bo != 0.0 ? " + " : "", bo != 0.0 ? (bo > 0 ? "0.5" : "-0.5") : "",
                cop ? "-" : "+", A, B, C,
                sc != 1.0 ? (csc==1 ? " * 2.0" : csc==2 ? " * 4.0" : " * 0.5") : "",
                cclamp ? "0.0" : "-4.0", cclamp ? "1.0" : "4.0");
        }
        /* ---- alpha ---- */
        int abias = (apk>>16)&3, aop = (apk>>18)&1, aclamp = (apk>>19)&1, asc = (apk>>20)&3;
        if (abias == 3) {
            GEN("    float na = tev_alpha(%d, texc, ras, tev_prev, tev_r0, tev_r1, tev_r2, konst_a);\n", apk);
        } else {
            const char *A = k_ca_in[(apk>>4)&7], *B = k_ca_in[(apk>>7)&7];
            const char *C = k_ca_in[(apk>>10)&7], *D = k_ca_in[(apk>>13)&7];
            double bo = (abias==1) ? 0.5 : (abias==2) ? -0.5 : 0.0;
            double sc = (asc==1) ? 2.0 : (asc==2) ? 4.0 : (asc==3) ? 0.5 : 1.0;
            GEN("    float na = clamp(((%s)%s%s %s mix((%s),(%s),(%s)))%s, %s, %s);\n",
                D,
                bo != 0.0 ? " + " : "", bo != 0.0 ? (bo > 0 ? "0.5" : "-0.5") : "",
                aop ? "-" : "+", A, B, C,
                sc != 1.0 ? (asc==1 ? " * 2.0" : asc==2 ? " * 4.0" : " * 0.5") : "",
                aclamp ? "0.0" : "-4.0", aclamp ? "1.0" : "4.0");
        }
        GEN("    %s.rgb = nrgb; %s.a = na;\n", k_dst[(cpk>>22)&3], k_dst[(apk>>22)&3]);
        GEN("    last_rgb = nrgb; last_a = na;\n  }\n");
    }

    /* Alpha test kept uniform-driven: refs change per draw without changing
     * shader shape, so baking them would multiply the variant count. */
    GEN("  frag = vec4(last_rgb, last_a);\n"
        "  if (u_alpha_test != 0) {\n"
        "    int at = u_alpha_test;\n"
        "    bool c0 = alpha_cmp((at >> 8) & 0x7, frag.a, float((at) & 0xFF) / 255.0);\n"
        "    bool c1 = alpha_cmp((at >> 24) & 0x7, frag.a, float((at >> 16) & 0xFF) / 255.0);\n"
        "    int lg = (at >> 11) & 0x3;\n"
        "    bool pass;\n"
        "    if (lg == 0) pass = c0 && c1; else if (lg == 1) pass = c0 || c1;\n"
        "    else if (lg == 2) pass = c0 ^^ c1; else pass = !(c0 ^^ c1);\n"
        "    if (!pass) discard;\n  }\n}\n");
    return off;
}
#undef GEN

static GLuint compile_tev_program(const TevState *st) {
    /* Specialised fragment source: prologue + defines + const structural arrays
     * (same names as the guarded uniforms) + body. With ROBOX_NUM_STAGES fixed
     * the loop unrolls; with the structural words const, every selector chain in
     * the body folds to a single path. The body itself is byte-identical to the
     * uniform version, so the combiner MATH cannot differ. */
    /* Float combiner unless explicitly forced to the exact integer path. */
    static int int_tev = -1;
    if (int_tev < 0) { const char *e = getenv("RECOMP_TEV_INT"); int_tev = (e && e[0] && e[0] != '0'); }

    /* Reuse everything in k_frag_src up to main() (declarations plus the helper
     * functions the generated code still calls: alpha_cmp, and tev_color /
     * tev_alpha for compare-mode stages), then append a generated main() with
     * every selector already resolved. Splitting on "void main()" avoids having
     * to restructure the shader literal. */
    const char *mainp = strstr(k_frag_src, "void main()");
    if (!mainp) return 0;
    size_t helpers_len = (size_t)(mainp - k_frag_src);

    char *body = (char *)malloc(16384);
    if (!body) return 0;
    int blen = gen_tev_main(st, body, 16384);
    if (blen <= 0 || blen >= 16384) { free(body); return 0; }

    char hdr[128];
    snprintf(hdr, sizeof hdr, "#define ROBOX_SPECIALIZED\n#define ROBOX_NUM_STAGES %d\n%s",
             st->num_stages, int_tev ? "" : "#define ROBOX_FLOAT_TEV\n");

    size_t len = strlen(GLSL_PROLOGUE) + strlen(hdr) + helpers_len + (size_t)blen + 1;
    char *fsrc = (char *)malloc(len);
    if (!fsrc) { free(body); return 0; }
    size_t o = 0;
    o += (size_t)snprintf(fsrc + o, len - o, "%s%s", GLSL_PROLOGUE, hdr);
    memcpy(fsrc + o, k_frag_src, helpers_len); o += helpers_len;
    memcpy(fsrc + o, body, (size_t)blen);      o += (size_t)blen;
    fsrc[o] = 0;

    GLuint p = link_program(k_vert_src, fsrc);
    free(body);
    free(fsrc);
    return p;
}

static void query_tev_variant(TevVariant *v) {
    GLuint p = v->prog;
    v->u_mvp        = glGetUniformLocation(p, "u_mvp");
    v->u_konst_color= glGetUniformLocation(p, "u_konst_color");
    v->u_tev_reg    = glGetUniformLocation(p, "u_tev_reg");
    v->u_alpha_test = glGetUniformLocation(p, "u_alpha_test");
    v->u_uv_from_pos= glGetUniformLocation(p, "u_uv_from_pos");
    v->u_texmtx0    = glGetUniformLocation(p, "u_texmtx0");
    v->u_texmtx1    = glGetUniformLocation(p, "u_texmtx1");
    v->u_num_stages = glGetUniformLocation(p, "u_num_stages");
    v->u_tev_color  = glGetUniformLocation(p, "u_tev_color");   /* -1 when const */
    v->u_tev_alpha  = glGetUniformLocation(p, "u_tev_alpha");
    v->u_tev_order  = glGetUniformLocation(p, "u_tev_order");
    v->u_tev_ksel   = glGetUniformLocation(p, "u_tev_ksel");
    v->u_swap       = glGetUniformLocation(p, "u_swap");
    char name[16];
    glUseProgram(p);
    for (int i = 0; i < 8; i++) {
        snprintf(name, sizeof name, "u_tex[%d]", i);
        v->u_tex[i] = glGetUniformLocation(p, name);
        if (v->u_tex[i] >= 0) glUniform1i(v->u_tex[i], i);   /* sampler unit = slot */
        snprintf(name, sizeof name, "u_tex_en[%d]", i);
        v->u_tex_en[i] = glGetUniformLocation(p, name);
    }
}

static void bind_tev_variant(const TevVariant *v) {
    g_prog          = v->prog;
    g_u_mvp         = v->u_mvp;
    g_u_num_stages  = v->u_num_stages;
    g_u_tev_color   = v->u_tev_color;
    g_u_tev_alpha   = v->u_tev_alpha;
    g_u_tev_order   = v->u_tev_order;
    g_u_tev_ksel    = v->u_tev_ksel;
    g_u_swap        = v->u_swap;
    g_u_konst_color = v->u_konst_color;
    g_u_tev_reg     = v->u_tev_reg;
    g_u_alpha_test  = v->u_alpha_test;
    g_u_uv_from_pos = v->u_uv_from_pos;
    g_u_texmtx0     = v->u_texmtx0;
    g_u_texmtx1     = v->u_texmtx1;
    for (int i = 0; i < 8; i++) { g_u_tex[i] = v->u_tex[i]; g_u_tex_en[i] = v->u_tex_en[i]; }
    glUseProgram(g_prog);
}

/* Find or build the specialised program for this draw's TEV state, point the
 * g_prog / g_u_* globals at it and bind it. Returns 0 only on hard failure. */
static int select_tev_variant(const TevState *st) {
    uint32_t h = tev_state_hash(st);
    if (h == 0) h = 1;                     /* reserve 0 = "nothing bound" */
    if (h == g_tv_cur) return 1;           /* already bound this exact state */

    for (int i = 0; i < g_tv_count; i++) {
        /* Reached only past the `h == g_tv_cur` early-out above, so every
         * assignment here is a genuine change and always invalidates.
         * NOTE: g_tv_cur is a LAGGING input to the key -- gx_ogl_draw takes the
         * key before calling select_tev_current() -- so the bump lands after
         * this draw's key and forces the NEXT one to recompute. Conservative,
         * which is the safe direction. */
        if (g_tv[i].used && g_tv[i].hash == h) { bind_tev_variant(&g_tv[i]); g_tv_cur = h; GX_STATE_DIRTY(); return 1; }
    }
    if (g_tv_count >= TEV_CACHE_SIZE) {
        /* Cache full: reuse whatever program is already bound rather than thrash.
         * Correctness is preserved because the structural uniforms still exist as
         * a fallback only in the UNspecialised program; a specialised program with
         * the wrong constants would be wrong, so we must NOT bind a mismatched one.
         * Recompile into slot 0 as a last resort. */
        TevVariant *v = &g_tv[0];
        if (v->prog) { glDeleteProgram(v->prog); }
        v->prog = compile_tev_program(st);
        if (!v->prog) return 0;
        v->used = 1; v->hash = h;
        query_tev_variant(v);
        bind_tev_variant(v); g_tv_cur = h; GX_STATE_DIRTY();
        return 1;
    }
    TevVariant *v = &g_tv[g_tv_count];
    v->prog = compile_tev_program(st);
    if (!v->prog) {
        /* Loud: a codegen miss would otherwise leave a MISMATCHED program bound
         * and render silently wrong (or worse) for this state. Levels exercise
         * TEV states the menu never does, so this is exactly where a generator
         * bug would first appear. */
        static int reported;
        if (reported++ < 8) {
            fprintf(stderr, "[gx_ogl] TEV CODEGEN FAILED stages=%d c=%08x a=%08x o=%08x k=%08x"
                    " -- set RECOMP_TEV_INT=1 to fall back\n",
                    st->num_stages, (unsigned)st->tev_color[0], (unsigned)st->tev_alpha[0],
                    (unsigned)st->tev_order[0], (unsigned)st->tev_ksel[0]);
            fflush(stderr);
        }
        return 0;
    }
    v->used = 1; v->hash = h;
    query_tev_variant(v);
    g_tv_count++;
    bind_tev_variant(v); g_tv_cur = h; GX_STATE_DIRTY();
    return 1;
}

/* Convenience: build state from g_bp and select. */
static int select_tev_current(void) {
    TevState st; compute_tev_state(&st);
    return select_tev_variant(&st);
}

/* Called from video.c after creating the GL context */
void gx_ogl_init(void) {
    if (g_ogl_ready) return;

    load_gl_procs();

    /* Check we got the essential functions */
    if (!pglCreateProgram || !pglGenVertexArrays) {
        fprintf(stderr, "[gx_ogl] essential GL procs missing — falling back to SW\n");
        return;
    }

    /* Compile an initial specialised program (1 stage, neutral state) so the
     * globals point somewhere valid for the default-uniform setup below. Real
     * draws select the program matching their actual TEV state. */
    { TevState st0; memset(&st0, 0, sizeof st0); st0.num_stages = 1;
      if (!select_tev_variant(&st0)) return; }

    /* Flat measurement program: same vertex stage, trivial fragment. */
    g_prog_flat = link_program(k_vert_src,
        GLSL_PROLOGUE
        "in vec4 v_color;\nin vec2 v_tex;\nout vec4 frag;\n"
        "void main(){ frag = vec4(v_color.rgb, 1.0); }\n");
    g_u_mvp_flat = g_prog_flat ? glGetUniformLocation(g_prog_flat, "u_mvp") : -1;

    /* Minimal realistic program: one texture sample x vertex color. */
    g_prog_simple = link_program(k_vert_src,
        GLSL_PROLOGUE
        "in vec4 v_color;\nin vec2 v_tex;\nout vec4 frag;\n"
        "uniform sampler2D u_tex0s;\n"
        "void main(){ frag = texture(u_tex0s, v_tex) * v_color; }\n");
    if (g_prog_simple) {
        g_u_mvp_simple  = glGetUniformLocation(g_prog_simple, "u_mvp");
        g_u_tex0_simple = glGetUniformLocation(g_prog_simple, "u_tex0s");
        glUseProgram(g_prog_simple);
        if (g_u_tex0_simple >= 0) glUniform1i(g_u_tex0_simple, 0);
    }

    glUseProgram(g_prog);
    /* GXInit-equivalent BP defaults: LyN's UI never writes TREF/TEV --
     * it relies on the hardware defaults GXInit establishes. Stage 0:
     * texmap0/texcoord0 enabled, REPLACE-from-texture combiners. The game
     * overwrites these the moment it sets real material state. */
    g_bp[0x00] = 0x00000000;              /* genmode: 1 TEV stage */
    g_bp[0x28] = 0x00000040;              /* TREF s0: map0 coord0 enabled */
    /* NOTE: these use the SHADER's packing (a[3:0]..d[15:12] for color,
     * a@4..d@13 3-bit for alpha), not raw BP — setup_tev_uniforms feeds
     * these words to the shader untranslated. */
    /* raw BP encodings (setup_tev_uniforms repacks for the shader) */
    g_bp[0xC0] = 0x0008FFF8u;   /* color: a=b=c=ZERO(15) d=TEXC(8) clamp */
    g_bp[0xC1] = 0x0008FFC0u;   /* alpha: a=b=c=ZERO(7)  d=TEXA(4) clamp */
    /* KSEL mirror defaults: SDK swap tables (0=RGBA 1=RRRA 2=GGGA 3=BBBA)
     * in bits[3:0] of each word — matches the __GXData seeds in hle_GXInit
     * so stages resolve sane channels before the game programs the tables. */
    {
        static const uint8_t ksel_swap[8] = { 0x4, 0xE, 0x0, 0xC, 0x5, 0xD, 0xA, 0xE };
        for (int i = 0; i < 8; i++) g_bp[0xF6 + i] = ksel_swap[i];
    }
    fprintf(stderr, "[gx_ogl] prog=%u u_mvp=%d stages=%d tevc=%d alpha=%d\n",
            g_prog, g_u_mvp, g_u_num_stages, g_u_tev_color, g_u_alpha_test);
    fflush(stderr);
    /* bind sampler slots and cache their locations */
    char name[16];
    for (int i = 0; i < 8; i++) {
        snprintf(name, sizeof(name), "u_tex[%d]", i);
        g_u_tex[i] = glGetUniformLocation(g_prog, name);
        glUniform1i(g_u_tex[i], i);  /* sampler unit = slot index */
        snprintf(name, sizeof(name), "u_tex_en[%d]", i);
        g_u_tex_en[i] = glGetUniformLocation(g_prog, name);
    }
    /* Default TEV: 1 stage, tex*ras, output to PREV */
    /* color: d=CC_TEXC(8)<<12 | c=CC_ZERO<<8 | b=CC_RASC(10)<<4 | a=CC_ZERO
     *        Actually: a=CC_TEXC, b=CC_RASC, c=CC_RASC (lerp), d=CC_ZERO
     *        Simplest useful default: a=CC_TEXC(8), b=CC_ZERO, c=CC_ZERO, d=CC_RASC(10)
     *        → d + mix(a,b,c) = CC_RASC + mix(CC_TEXC,0,0) = ras
     *        Let me use: a=CC_ZERO, b=CC_ZERO, c=CC_ZERO, d=CC_TEXC (pass texture)
     *        for now, then multiply by ras separately...
     *        Actually the easiest: a=CC_TEXC(8) b=CC_ZERO c=CC_ZERO d=CC_RASC(10)
     *        → mix(8,0,0) + 10 = texc.rgb*0 + ras.rgb ... no
     *        mix(a,b,c) = a*(1-c)+b*c = a*1+b*0 = a when c=0
     *        So: d + mix(a,b,c) = CC_ZERO + CC_TEXC = texc.rgb (a=TEXC, b=0, c=0, d=0)
     *        To get tex*ras: use a=CC_ZERO, b=CC_RASC, c=CC_TEXC, d=CC_ZERO
     *        → mix(0, ras, texc) = ras * texc ✓
     */
    static const GLint def_color[8] = {
        /* a=CC_ZERO(15), b=CC_RASC(10)<<4, c=CC_TEXC(8)<<8, d=CC_ZERO(15)<<12, clamp[19]=1 */
        /* bits: d=0xF<<12, c=0x8<<8, b=0xA<<4, a=0xF, clamp=1<<19 */
        (0xF<<12)|(0x8<<8)|(0xA<<4)|0xF | (1<<19),
        0, 0, 0, 0, 0, 0, 0
    };
    static const GLint def_alpha[8] = {
        /* a=CA_ZERO(7)<<4, b=CA_RASA(5)<<7, c=CA_TEXA(4)<<10, d=CA_ZERO(7)<<13, clamp[19]=1 */
        /* mix(0, ras.a, tex.a) = ras.a * tex.a */
        (7<<13)|(4<<10)|(5<<7)|(7<<4) | (1<<19),
        0, 0, 0, 0, 0, 0, 0
    };
    static const GLint def_order[8] = { (1<<6)|0, 0, 0, 0, 0, 0, 0, 0 }; /* slot0 enabled */
    static const GLint def_ksel[8]  = { 0 };
    static const GLfloat def_konst[16] = {
        1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1
    };
    glUniform1i(g_u_num_stages, 1);
    glUniform1iv(g_u_tev_color, 8, def_color);
    glUniform1iv(g_u_tev_alpha, 8, def_alpha);
    glUniform1iv(g_u_tev_order, 8, def_order);
    glUniform1iv(g_u_tev_ksel,  8, def_ksel);
    {   /* swap tables from the KSEL mirror defaults seeded above */
        GLint sw[4];
        for (int t = 0; t < 4; t++) {
            uint32_t rg = g_bp[0xF6 + 2*t], ba = g_bp[0xF7 + 2*t];
            sw[t] = (GLint)((rg & 3) | (((rg >> 2) & 3) << 2)
                          | ((ba & 3) << 4) | (((ba >> 2) & 3) << 6));
        }
        glUniform1iv(g_u_swap, 4, sw);
    }
    glUniform4fv(g_u_konst_color, 4, def_konst);
    glUniform1i(g_u_alpha_test, 0);

    /* Clear shader */
    g_clear_prog = link_program(k_clear_vert_src, k_clear_frag_src);
    g_u_clear_color = glGetUniformLocation(g_clear_prog, "u_color");

    /* Full-screen quad VAO */
    static const float fsq_verts[] = {
        -1,-1,  1,-1,  -1,1,  1,-1,  1,1,  -1,1
    };
    glGenVertexArrays(1, &g_fsq_vao);
    glBindVertexArray(g_fsq_vao);
    glGenBuffers(1, &g_fsq_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_fsq_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(fsq_verts), fsq_verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    glEnableVertexAttribArray(0);

    /* Geometry VAO */
    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    /* pre-allocate; will reuse with BufferSubData */
    glBufferData(GL_ARRAY_BUFFER,
                 VBO_CAPACITY * (int)sizeof(GxOglVertex), NULL, GL_STREAM_DRAW);
    /* attrib 0: position (float3) */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                          sizeof(GxOglVertex), (void*)offsetof(GxOglVertex, x));
    glEnableVertexAttribArray(0);
    /* attrib 1: color (4 unsigned bytes, normalized) */
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                          sizeof(GxOglVertex), (void*)offsetof(GxOglVertex, color));
    glEnableVertexAttribArray(1);
    /* attrib 2: texcoord (float2) */
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE,
                          sizeof(GxOglVertex), (void*)offsetof(GxOglVertex, s));
    glEnableVertexAttribArray(2);

    /* Static quad->triangle index buffer, built once. Lets a GX QUADS batch
     * draw in one indexed call instead of one glDrawArrays per quad. Bound
     * into the VAO's element slot here so it persists with the VAO. */
    {
        static GLushort idx[QUAD_IBO_MAX_QUADS * 6];
        for (int q = 0; q < QUAD_IBO_MAX_QUADS; ++q) {
            GLushort b = (GLushort)(q * 4);
            idx[q*6+0] = b;     idx[q*6+1] = b + 1; idx[q*6+2] = b + 2;
            idx[q*6+3] = b;     idx[q*6+4] = b + 2; idx[q*6+5] = b + 3;
        }
        glGenBuffers(1, &g_quad_ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_quad_ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);
    }

    /* EFB framebuffer. Choose the internal resolution FIRST so the attachments
     * below are born at the scaled size (a later live change resizes them). */
    gx_ogl_apply_scale_dims(gx_ogl_internal_scale_default());
    fprintf(stderr, "[gx_ogl] internal resolution %dx (%dx%d)\n",
            g_efb_scale, g_efb_w, g_efb_h);

    glGenFramebuffers(1, &g_efb_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);

    glGenTextures(1, &g_efb_col);
    glBindTexture(GL_TEXTURE_2D, g_efb_col);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_efb_w, g_efb_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    /* The EFB itself is only ever blitted (never sampled as a texture with these
     * params), and the present/copy blits pass their own filter, so NEAREST
     * here is harmless; the scaling quality comes from the render size. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g_efb_col, 0);

    glGenRenderbuffers(1, &g_efb_dep);
    glBindRenderbuffer(GL_RENDERBUFFER, g_efb_dep);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, g_efb_w, g_efb_h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, g_efb_dep);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[gx_ogl] EFB framebuffer incomplete: 0x%x\n", status);
        return;
    }

    /* Start rendering to the EFB */
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    glViewport(0, 0, g_efb_w, g_efb_h);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    g_ogl_ready = 1;
    fprintf(stderr, "[gx_ogl] OpenGL GX renderer ready (GL %s)\n",
            (const char*)glGetString(GL_VERSION));
    fflush(stderr);
}

static int g_tex0_mtx_slot = -1;   /* -1 = identity */
/* Also selects WHICH g_pos_mtx rows the key hashes, so one write here silently
 * changes 8 further hashed words with no textual g_pos_mtx write anywhere. */
void gx_ogl_set_tex0_mtx_slot(int slot) {
    if (g_tex0_mtx_slot != slot) GX_STATE_DIRTY();
    g_tex0_mtx_slot = slot;
}

/* ---- Host-pixel texture override (movie-as-texture) ----------------------
 * raster_primitive() calls this when it identifies the game's fullscreen
 * video quad while a bink movie is open. The NEXT gx_ogl_draw binds these
 * BGRA pixels on unit 0 and forces a passthrough (REPLACE) TEV — the game's
 * own movie TEV is a YUV->RGB conversion that would mangle an RGB source.
 * One-shot: consumed by the next draw. */
static GLuint g_host_tex;
static int    g_host_tex_pending;
static int    g_host_tex_w, g_host_tex_h;
int gx_ogl_host_tex_pending(void) { return g_host_tex_pending; }
void gx_ogl_set_tex_host_bgra(const void *pix, int w, int h) {
    if (!g_ogl_ready || !pix || w <= 0 || h <= 0) return;
    if (!g_host_tex) glGenTextures(1, &g_host_tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_host_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GX_BGRA_SWIZZLE();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GX_BGRA_FORMAT,
                 GL_UNSIGNED_BYTE, pix);
    g_host_tex_w = w; g_host_tex_h = h;
    g_host_tex_pending = 1;
}
void gx_ogl_set_uv_from_pos(int on) {
    if (g_uv_from_pos != on) GX_STATE_DIRTY();   /* driven per primitive */
    g_uv_from_pos = on;
}
int gx_ogl_ready(void) { return g_ogl_ready; }

/* ---- State setters ----------------------------------------------------- */

void gx_ogl_set_pos_matrix(int slot, const float r0[4], const float r1[4],
                             const float r2[4]) {
    if (slot < 0 || slot >= N_POS_MTX) return;
    float *m = g_pos_mtx[slot];
    /* The game rewrites matrix slots IN PLACE between draws, which is why the
     * key hashes contents rather than the slot index -- so an in-place rewrite
     * must invalidate even though the slot number did not change. */
    if (m[0]!=r0[0] || m[1]!=r0[1] || m[2]!=r0[2] || m[3]!=r0[3] ||
        m[4]!=r1[0] || m[5]!=r1[1] || m[6]!=r1[2] || m[7]!=r1[3] ||
        m[8]!=r2[0] || m[9]!=r2[1] || m[10]!=r2[2] || m[11]!=r2[3])
        GX_STATE_DIRTY();
    m[0]=r0[0]; m[1]=r0[1]; m[2]=r0[2]; m[3]=r0[3];
    m[4]=r1[0]; m[5]=r1[1]; m[6]=r1[2]; m[7]=r1[3];
    m[8]=r2[0]; m[9]=r2[1]; m[10]=r2[2]; m[11]=r2[3];
}

void gx_ogl_set_proj(const float proj[7]) {
    for (int i = 0; i < 7; i++)
        if (g_proj[i] != proj[i]) { GX_STATE_DIRTY(); break; }
    for (int i = 0; i < 7; i++) g_proj[i] = proj[i];
}

void gx_ogl_set_cp_vtxdesc(uint32_t lo, uint32_t hi) { (void)lo; (void)hi; }
void gx_ogl_set_cp_vat(int s, uint32_t g0, uint32_t g1, uint32_t g2) {
    (void)s; (void)g0; (void)g1; (void)g2;
}
void gx_ogl_set_cp_array(int a, uint32_t base, uint32_t stride) {
    (void)a; (void)base; (void)stride;
}

/* TEV output registers (prev/c0/c1/c2) and konst colors, split at write time.
 * Ported from Dolphin BPStructs.cpp: BP 0xE0-0xE7 hold RA (even) / BG (odd)
 * words for register num=(reg>>1)&3; bit 23 (type) routes the write to the
 * KONST bank or the TEV-register bank — the game writes the same addresses as
 * both, so a single array (our old bug) loses one. red/blue=[0:10],
 * alpha/green=[12:22], 11-bit signed. */
int g_tev_reg[4][4];      /* [num][RGBA], signed 11-bit */
int g_tev_konst[4][4];    /* [num][RGBA], 0-255 */
static inline int gx_s11(uint32_t v) { return (int)(v << 21) >> 21; }

/* Diagnostics: active tex0 matrix slot + its first two rows; konst + tev
 * register banks (peripherals.c draw dump — menu-fidelity hunt). */
int gx_ogl_get_tex0_mtx(float rows[8]) {
    int slot = g_tex0_mtx_slot;
    const float *m = (slot >= 0 && slot < N_POS_MTX) ? g_pos_mtx[slot] : NULL;
    for (int i = 0; i < 8; ++i) rows[i] = m ? m[i] : (i == 0 || i == 5 ? 1.f : 0.f);
    return slot;
}
void gx_ogl_get_tev_colors(uint8_t konst[16], uint8_t treg[16]) {
    for (int i = 0; i < 4; ++i)
        for (int c = 0; c < 4; ++c) {
            konst[i*4+c] = (uint8_t)g_tev_konst[i][c];
            treg[i*4+c]  = (uint8_t)g_tev_reg[i][c];
        }
}

void gx_ogl_bp_write(uint32_t reg, uint32_t val) {
    /* Every mutation below invalidates the memoized draw key -- but ONLY when
     * the value actually changes. The game re-writes identical register values
     * constantly, and bumping the generation on every write would make the
     * memo miss every time and cost more than it saves. */
    if (reg < 0x100) {
        if (g_bp[reg] != val && GX_BP_REG_IS_HASHED(reg)) GX_STATE_DIRTY();
        g_bp[reg] = val;
    }
    if (reg >= 0xE0 && reg <= 0xE7) {
        int num = (int)((reg >> 1) & 3);
        int (*dst)[4] = ((val >> 23) & 1) ? g_tev_konst : g_tev_reg;
        if ((reg & 1) == 0) {                 /* RA word */
            int r = gx_s11(val), a = gx_s11(val >> 12);
            if (dst[num][0] != r || dst[num][3] != a) GX_STATE_DIRTY();
            dst[num][0] = r;                  /* red   */
            dst[num][3] = a;                  /* alpha */
        } else {                              /* BG word */
            int b = gx_s11(val), g = gx_s11(val >> 12);
            if (dst[num][2] != b || dst[num][1] != g) GX_STATE_DIRTY();
            dst[num][2] = b;                  /* blue  */
            dst[num][1] = g;                  /* green */
        }
    }
}

void gx_ogl_set_tex(int slot, uint32_t va, uint32_t w, uint32_t h,
                    uint32_t fmt, uint32_t ws, uint32_t wt) {
    if (slot < 0 || slot >= 8) return;
    OglTexObj *t = &g_tex_obj[slot];
    if (!t->valid || t->va != va || t->w != w || t->h != h ||
        t->fmt != fmt || t->ws != ws || t->wt != wt)
        GX_STATE_DIRTY();
    t->va = va; t->w = w; t->h = h;
    t->fmt = fmt; t->ws = ws; t->wt = wt;
    t->valid = 1;
}

/* ---- TEV uniform setup from BP state ---------------------------------- */

/* Extract the 11-bit signed TEV register value, convert to [0,1] float */
static float tev_reg_s10_to_f(uint32_t val, int shift) {
    int32_t v = (int32_t)((val >> shift) & 0x7FF);
    if (v & 0x400) v |= ~0x7FF;            /* sign-extend */
    return (float)v / 255.0f;
}

static void setup_tev_uniforms(void) {
    /* ---- Number of active TEV stages from BPMEM_GENMODE (BP[0x00]) ---- */
    int num_stages = (int)(((g_bp[0x00] >> 10) & 0xF) + 1);  /* genmode ntev = bits[13:10] */
    if (num_stages < 1) num_stages = 1;
    if (num_stages > 8) num_stages = 8;

    /* With a specialised program the structural TEV words are compile-time
     * constants, so their uniform locations are -1 and every upload below is a
     * silent no-op -- but the REPACKING still cost CPU on all ~346 draws a
     * frame. Skip it entirely; select_tev_current() already derived the same
     * values to pick the program. */
    int spec = (g_u_tev_color < 0);
    if (!spec) {
    glUniform1i(g_u_num_stages, num_stages);

    /* ---- Color / alpha combiners (BP[0xC0+2*s], BP[0xC1+2*s]) ---- */
    GLint tev_color[8], tev_alpha[8];
    for (int s = 0; s < num_stages; s++) {
        /* Repack raw BP combiner words into the shader's layout.
         * BP color: d[3:0] c[7:4] b[11:8] a[15:12]; shader: a@0 b@4 c@8 d@12.
         * BP alpha: rswap[1:0] tswap[3:2] d[6:4] c[9:7] b[12:10] a[15:13];
         * shader: rswap/tswap kept at [3:0], a@4 b@7 c@10 d@13.
         * Upper bits (bias/op/clamp/scale/dest 16..23) identical. */
        uint32_t cw = g_bp[0xC0 + 2*s];
        uint32_t aw = g_bp[0xC1 + 2*s];
        tev_color[s] = (GLint)((cw & 0x00FF0000u)
                     | (((cw >> 12) & 0xFu) << 0) | (((cw >> 8) & 0xFu) << 4)
                     | (((cw >> 4) & 0xFu) << 8)  | ((cw & 0xFu) << 12));
        tev_alpha[s] = (GLint)((aw & 0x00FF0000u) | (aw & 0xFu)
                     | (((aw >> 13) & 0x7u) << 4) | (((aw >> 10) & 0x7u) << 7)
                     | (((aw >> 7) & 0x7u) << 10) | (((aw >> 4) & 0x7u) << 13));
    }
    for (int s = num_stages; s < 8; s++) { tev_color[s] = 0; tev_alpha[s] = 0; }
    glUniform1iv(g_u_tev_color, 8, tev_color);
    glUniform1iv(g_u_tev_alpha, 8, tev_alpha);

    /* ---- Texture/rasterizer order (BPMEM_TREF 0x28+s/2) ---- */
    GLint tev_order[8];
    for (int s = 0; s < num_stages; s++) {
        uint32_t tref = g_bp[0x28 + s/2];
        int shift = (s & 1) ? 12 : 0;
        /* bits: texmap[2:0] texcoord[5:3] enable[6] colorchan[9:7] */
        tev_order[s] = (GLint)((tref >> shift) & 0xFFF);
    }
    for (int s = num_stages; s < 8; s++) tev_order[s] = 0;
    glUniform1iv(g_u_tev_order, 8, tev_order);

    /* ---- Konst selectors (BPMEM_TEV_KSEL 0xF6+s/2) ---- */
    GLint tev_ksel[8];
    for (int s = 0; s < num_stages; s++) {
        uint32_t ksel_reg = g_bp[0xF6 + s/2];
        int shift = (s & 1) ? 8 : 0;
        /* bits: kc_sel[4:0] ka_sel[9:5] */
        int kc = (ksel_reg >> shift) & 0x1F;
        int ka = (ksel_reg >> (shift + 5)) & 0x1F;
        tev_ksel[s] = (GLint)(kc | (ka << 5));
    }
    for (int s = num_stages; s < 8; s++) tev_ksel[s] = 0;
    glUniform1iv(g_u_tev_ksel, 8, tev_ksel);

    /* ---- Swap tables (bits[3:0] of each KSEL reg pair) ---- */
    {
        GLint sw[4];
        for (int t = 0; t < 4; t++) {
            uint32_t rg = g_bp[0xF6 + 2*t], ba = g_bp[0xF7 + 2*t];
            sw[t] = (GLint)((rg & 3) | (((rg >> 2) & 3) << 2)
                          | ((ba & 3) << 4) | (((ba >> 2) & 3) << 6));
        }
        glUniform1iv(g_u_swap, 4, sw);
    }
    }   /* end !spec */

    /* ---- Konst colors + TEV output-register init values ----
     * Now split correctly by the type bit at BP-write time (gx_ogl_bp_write,
     * ported from Dolphin BPStructs.cpp). konst = GXSetTevKColor bank; tev_reg =
     * GXSetTevColor bank (initial prev/c0/c1/c2 before any stage runs). */
    GLfloat konst[16], treg[16];
    for (int i = 0; i < 4; i++) {
        konst[i*4+0] = g_tev_konst[i][0] / 255.0f;
        konst[i*4+1] = g_tev_konst[i][1] / 255.0f;
        konst[i*4+2] = g_tev_konst[i][2] / 255.0f;
        konst[i*4+3] = g_tev_konst[i][3] / 255.0f;
        treg[i*4+0]  = g_tev_reg[i][0] / 255.0f;
        treg[i*4+1]  = g_tev_reg[i][1] / 255.0f;
        treg[i*4+2]  = g_tev_reg[i][2] / 255.0f;
        treg[i*4+3]  = g_tev_reg[i][3] / 255.0f;
    }
    /* RGH_FORCE_LIGHT (default ON, =0 to disable): the wiimote-interior
     * sequence that would ramp the post-pass darkness away never runs (its
     * driver is parked — see project memory). Until that sequencer works,
     * neutralize the post quad's subtractive fade (stage2: scene - K0.blue)
     * by zeroing K0's blue FOR THE POST PASS ONLY, so the rendered world is
     * visible instead of black. Materials elsewhere keep their real K0. */
    if (g_tex_obj[0].valid && gx_ogl_efbcopy_lookup(g_tex_obj[0].va)) {
        static int s_force = -1;
        if (s_force < 0) {
            const char *e = getenv("RGH_FORCE_LIGHT");
            s_force = !(e && e[0] == '0');
        }
        if (s_force) konst[0*4+2] = 0.0f;   /* K0.blue = 0: no darkness */
    }
    /* Redundant-upload cache. These are value uniforms that usually do not
     * change between consecutive draws, but were being re-uploaded ~346 times a
     * frame. Keyed on the bound program too, since locations are per-program. */
    {
        static GLuint c_prog; static GLfloat c_konst[16], c_treg[16]; static int c_valid;
        int same = c_valid && c_prog == g_prog
                && memcmp(c_konst, konst, sizeof konst) == 0
                && memcmp(c_treg,  treg,  sizeof treg)  == 0;
        if (!same) {
            glUniform4fv(g_u_konst_color, 4, konst);
            if (g_u_tev_reg >= 0) glUniform4fv(g_u_tev_reg, 4, treg);
            memcpy(c_konst, konst, sizeof konst);
            memcpy(c_treg,  treg,  sizeof treg);
            c_prog = g_prog; c_valid = 1;
        }
    }
    /* [POST-TEV] probe: the fullscreen POST-PROCESS pass (tex0 = a live
     * EFB-copy texture) multiplies/subtracts the scene by TEV constants —
     * the in-game fade lives here. Log its live konst/reg banks once/sec so
     * a fade stuck at black (scene - K = 0) is directly visible. */
    if (gx_probe_enabled() && g_tex_obj[0].valid && gx_ogl_efbcopy_lookup(g_tex_obj[0].va)) {
        static uint64_t s_t;
        extern uint64_t ms_now(void);
        extern uint32_t g_last_k0_lr, g_last_k0_val;
        uint64_t now = ms_now();
        if (now - s_t >= 1000) {
            s_t = now;
            fprintf(stderr, "[POST-TEV] last-k0-writer lr=0x%08x bg=0x%06x\n",
                    g_last_k0_lr, g_last_k0_val);
            fprintf(stderr,
                "[POST-TEV] k0=%02x%02x%02x%02x k1=%02x%02x%02x%02x k2=%02x%02x%02x%02x k3=%02x%02x%02x%02x "
                "r0=%02x%02x%02x%02x r1=%02x%02x%02x%02x r2=%02x%02x%02x%02x r3=%02x%02x%02x%02x (RGBA)\n",
                g_tev_konst[0][0], g_tev_konst[0][1], g_tev_konst[0][2], g_tev_konst[0][3],
                g_tev_konst[1][0], g_tev_konst[1][1], g_tev_konst[1][2], g_tev_konst[1][3],
                g_tev_konst[2][0], g_tev_konst[2][1], g_tev_konst[2][2], g_tev_konst[2][3],
                g_tev_konst[3][0], g_tev_konst[3][1], g_tev_konst[3][2], g_tev_konst[3][3],
                g_tev_reg[0][0], g_tev_reg[0][1], g_tev_reg[0][2], g_tev_reg[0][3],
                g_tev_reg[1][0], g_tev_reg[1][1], g_tev_reg[1][2], g_tev_reg[1][3],
                g_tev_reg[2][0], g_tev_reg[2][1], g_tev_reg[2][2], g_tev_reg[2][3],
                g_tev_reg[3][0], g_tev_reg[3][1], g_tev_reg[3][2], g_tev_reg[3][3]);
            fflush(stderr);
        }
    }
    /* DIAG (fade hunt): log distinct TEV reg/konst colors. A fade that ramps
     * a TEV konst/register color shows up here as a value marching to 0. */
    { static uint32_t s_last[4]; static uint32_t s_n;
      for (int i = 0; i < 4; i++) {
          uint32_t v = ((uint32_t)(konst[i*4+0]*255)<<16)|((uint32_t)(konst[i*4+1]*255)<<8)
                      |((uint32_t)(konst[i*4+2]*255))|((uint32_t)(konst[i*4+3]*255)<<24);
          if (v != s_last[i]) { s_last[i] = v;
              /* In-game materials churn konst colors PER DRAW — unthrottled
               * this logged (and fflushed) thousands of lines/sec, a real
               * fps drain. Keep the first few hundred for fade hunts. */
              if (++s_n <= 400 || (s_n & 0xFFFu) == 0) {
                  fprintf(stderr, "[FADE?] TevKColor[%d] = 0x%08x (ARGB)\n", i, v);
                  fflush(stderr);
              } }
      } }

    /* ---- Alpha compare (BPMEM_ALPHACOMPARE 0xF3) ----
     * REAL GX bit layout (Dolphin BPMemory.h AlphaTest):
     *   ref0[7:0] ref1[15:8] comp0[18:16] comp1[21:19] logic[23:22]
     * The old code decoded comp0 from bits 8-10 and passed the RAW register
     * to the shader (which unpacks a different custom layout) -- the game's
     * standard 0x780000 (NEVER OR ALWAYS = pass) decoded as NEVER AND NEVER,
     * discarding EVERY fragment in the game. Repack into the shader layout:
     *   ref0 | comp0<<8 | logic<<11 | ref1<<16 | comp1<<24 */
    uint32_t acomp = g_bp[0xF3];
    int ref0  =  acomp        & 0xFF;
    int ref1  = (acomp >> 8)  & 0xFF;
    int comp0 = (acomp >> 16) & 0x7;
    int comp1 = (acomp >> 19) & 0x7;
    int logic = (acomp >> 22) & 0x3;
    int always_pass = (logic == 0 && comp0 == 7 && comp1 == 7)
                   || (logic == 1 && (comp0 == 7 || comp1 == 7));
    GLint packed = (GLint)(ref0 | (comp0 << 8) | (logic << 11)
                           | (ref1 << 16) | (comp1 << 24));
    glUniform1i(g_u_alpha_test, always_pass ? 0 : packed);

    /* ---- Textures ---- */
    GLint tex_en[8] = {0};
    robox_prof_scope_begin(ROBOX_PROF_TEX);
    for (int slot = 0; slot < 8; slot++) {
        if (!g_tex_obj[slot].valid) continue;
        OglTexObj *t = &g_tex_obj[slot];
        GLuint tid = tex_cache_get(t->va, t->w, t->h, t->fmt, t->ws, t->wt);
        if (tid) {
            glActiveTexture(GL_TEXTURE0 + slot);
            glBindTexture(GL_TEXTURE_2D, tid);
            tex_en[slot] = 1;
        }
    }
    robox_prof_scope_end(ROBOX_PROF_TEX);
    for (int slot = 0; slot < 8; slot++)
        glUniform1i(g_u_tex_en[slot], tex_en[slot]);

    /* ---- Blend mode (BPMEM_BLENDMODE 0x41) ---- */
    /* HARDWARE bit layout (Dolphin BPMemory BlendMode): enable[0]
     * logic_op_enable[1] dither[2] colorupdate[3] alphaupdate[4]
     * dst_factor[7:5] src_factor[10:8] SUBTRACT[11] logic_op[15:12].
     * The old decode read subtract from bit 15 — that bit is the top of the
     * logic-op field, and the game's menu font CMODE0 (0xF4AD, logic mode
     * 0xF with logic DISABLED) has it set: every glyph fill pass rendered
     * reverse-subtract (dst - white = BLACK text). */
    uint32_t bm = g_bp[0x41];
    int blend_en  = bm & 1;
    int blend_sub = (bm >> 11) & 1;
    if (blend_sub && pglBlendEquation) {
        /* GX subtract mode: dst = dst - src, factors forced to ONE/ONE. */
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
        glBlendFunc(GL_ONE, GL_ONE);
    } else if (blend_en) {
        /* GX factor value 2/3 means the OTHER side's color: DSTCLR as a src
         * factor but SRCCLR as a dst factor (Dolphin BPStructs mapping). */
        static const GLenum GX_SRC_FACTOR[] = {
            GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
            GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA
        };
        static const GLenum GX_DST_FACTOR[] = {
            GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR,
            GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA
        };
        int src_f = (bm >> 8)  & 0x7;
        int dst_f = (bm >> 5)  & 0x7;
        glEnable(GL_BLEND);
        if (pglBlendEquation) glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GX_SRC_FACTOR[src_f & 7], GX_DST_FACTOR[dst_f & 7]);
        /* DIAG (fade hunt): a fade drawn as a blended overlay quad turns blend
         * ON. Log the first time we see it + the src/dst factors. */
        { static int s_seen; if (!s_seen) { s_seen = 1;
            fprintf(stderr, "[FADE?] BLEND ENABLED bm=0x%08x src=%d dst=%d\n",
                    bm, src_f, dst_f); fflush(stderr); } }
    } else {
        glDisable(GL_BLEND);
    }

    /* ---- Z-buffer (BPMEM_ZMODE 0x40) ---- */
    /* bits: enable[0] func[3:1] update[4] */
    uint32_t zm = g_bp[0x40];
    if (zm & 1) {
        static const GLenum GX_DEPTH_FUNC[] = {
            GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL,
            GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS
        };
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GX_DEPTH_FUNC[(zm >> 1) & 7]);
        glDepthMask((zm >> 4) & 1 ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
    }
}

/* ---- Layer-dump diagnostic (menu-fidelity hunt) -------------------------
 * Drop a file named build\dump_next_frame to capture the NEXT frame
 * draw-by-draw: one state line per draw in layer_dump\layers.txt, plus an
 * EFB snapshot (layer_NNN.png) for each draw that changed pixels, with the
 * changed-region bounding box. Checked once per present. */
static int      g_layer_dump, g_layer_idx;
static uint8_t *g_layer_prev;
static FILE    *g_layer_log;

/* Drop the layer-dump scratch so it is reallocated at the current EFB size.
 * Called when the internal resolution changes (gx_ogl_set_internal_scale); the
 * old buffer was sized to the previous EFB and would over/under-read. */
void gx_ogl_layer_free(void) {
    free(g_layer_prev);
    g_layer_prev = NULL;
}

void gx_ogl_layer_dump_check(void) {
    static const char *trig = "logs/dump_next_frame";
    if (g_layer_dump) {
        if (--g_layer_dump > 0) {
            /* keep capturing into the same log; a frame marker separates them */
            if (g_layer_log) { fprintf(g_layer_log, "==== frame boundary ====\n"); fflush(g_layer_log); }
            return;
        }
        if (g_layer_log) { fclose(g_layer_log); g_layer_log = NULL; }
        fprintf(stderr, "[LAYER] dump complete (%d draws)\n", g_layer_idx);
        fflush(stderr);
        return;
    }
    FILE *f = fopen(trig, "rb");
    if (f) {
        fclose(f); remove(trig);
        robox_mkdir("logs");
        robox_mkdir("logs/layer_dump");
        g_layer_log = fopen("logs/layer_dump/layers.txt", "w");
        if (!g_layer_prev) g_layer_prev = (uint8_t*)calloc(1, (size_t)g_efb_w*g_efb_h*4);
        if (g_layer_prev) memset(g_layer_prev, 0, (size_t)g_efb_w*g_efb_h*4);
        g_layer_dump = 2; g_layer_idx = 0;   /* capture TWO consecutive frames */
        fprintf(stderr, "[LAYER] dumping next 2 frames...\n");
        fflush(stderr);
    }
}

static void layer_dump_record(uint32_t prim_type, const GxOglVertex *verts,
                              int n_verts, int host_override) {
    if (!g_layer_log || !g_layer_prev) return;
    size_t npix = (size_t)g_efb_w * g_efb_h;
    uint8_t *cur = (uint8_t*)malloc(npix * 4);
    if (!cur) return;
    glReadPixels(0, 0, g_efb_w, g_efb_h, GL_RGBA, GL_UNSIGNED_BYTE, cur);
    int minx = g_efb_w, miny = g_efb_h, maxx = -1, maxy = -1;
    for (int y = 0; y < g_efb_h; y++) {
        const uint32_t *a = (const uint32_t*)(cur + (size_t)y*g_efb_w*4);
        const uint32_t *b = (const uint32_t*)(g_layer_prev + (size_t)y*g_efb_w*4);
        for (int x = 0; x < g_efb_w; x++) {
            if (a[x] != b[x]) {
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
        }
    }
    int changed = maxx >= 0;
    int ns = (int)(((g_bp[0x00] >> 10) & 0xF) + 1);
    fprintf(g_layer_log,
        "draw %03d prim=0x%02x n=%d %s bbox=(%d,%d)-(%d,%d)%s\n"
        "  v0 pos=(%.2f,%.2f,%.2f) col=%08x uv=(%.3f,%.3f) | v2 pos=(%.2f,%.2f,%.2f) col=%08x uv=(%.3f,%.3f)\n"
        "  stages=%d tref0=%06x tref1=%06x blend=%06x zmode=%06x acmp=%06x uvpos=%d\n"
        "  tevc=%06x/%06x/%06x/%06x teva=%06x/%06x/%06x/%06x ksel=%06x/%06x\n",
        g_layer_idx, prim_type, n_verts,
        changed ? "CHANGED" : "nochange",
        minx, miny, maxx, maxy, host_override ? " HOSTTEX" : "",
        verts[0].x, verts[0].y, verts[0].z, verts[0].color, verts[0].s, verts[0].t,
        n_verts > 2 ? verts[2].x : 0, n_verts > 2 ? verts[2].y : 0,
        n_verts > 2 ? verts[2].z : 0, n_verts > 2 ? verts[2].color : 0,
        n_verts > 2 ? verts[2].s : 0, n_verts > 2 ? verts[2].t : 0,
        ns, g_bp[0x28], g_bp[0x29], g_bp[0x41], g_bp[0x40], g_bp[0xF3],
        g_uv_from_pos,
        g_bp[0xC0], g_bp[0xC2], g_bp[0xC4], g_bp[0xC6],
        g_bp[0xC1], g_bp[0xC3], g_bp[0xC5], g_bp[0xC7],
        g_bp[0xF6], g_bp[0xF7]);
    for (int m = 0; m < 8; m++)
        if (g_tex_obj[m].valid)
            fprintf(g_layer_log, "  tex%d va=%08x %ux%u fmt=%u wrap=%u/%u\n",
                    m, g_tex_obj[m].va, g_tex_obj[m].w, g_tex_obj[m].h,
                    g_tex_obj[m].fmt, g_tex_obj[m].ws, g_tex_obj[m].wt);
    fflush(g_layer_log);
    /* Fresh decode of the two active texmaps (no dedup — the boot-time
     * tex_dumps snapshots go stale the moment the game re-bakes an atlas). */
    for (int m = 0; m < 2; m++) {
        OglTexObj *t = &g_tex_obj[m];
        if (!t->valid || !t->va || t->w > 2048 || t->h > 2048) continue;
        const uint8_t *src = (const uint8_t*)ppc_host_ptr(t->va);
        if (!src) continue;
        uint8_t *rgba = (uint8_t*)malloc((size_t)t->w * t->h * 4);
        if (!rgba) continue;
        decode_gx_texture(rgba, src, (int)t->w, (int)t->h, (int)t->fmt);
        char tp[160];
        snprintf(tp, sizeof tp,
                 "logs/layer_dump/d%03d_t%d_va%08x.png",
                 g_layer_idx, m, t->va);
        dump_texture_png(tp, rgba, (int)t->w, (int)t->h);
        free(rgba);
    }
    if (changed) {
        char path[128];
        snprintf(path, sizeof path,
                 "logs/layer_dump/layer_%03d.png",
                 g_layer_idx);
        /* EFB rows are bottom-up; flip so the PNG reads top-down. */
        uint8_t *flip = (uint8_t*)malloc(npix * 4);
        if (flip) {
            for (int y = 0; y < g_efb_h; y++)
                memcpy(flip + (size_t)y*g_efb_w*4,
                       cur + (size_t)(g_efb_h-1-y)*g_efb_w*4, (size_t)g_efb_w*4);
            dump_texture_png(path, flip, g_efb_w, g_efb_h);
            free(flip);
        }
        memcpy(g_layer_prev, cur, npix * 4);
    }
    free(cur);
    g_layer_idx++;
}

/* ---- Draw -------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * Draw batching.
 *
 * After the shader work the menu frame is CPU-bound: ~340 draw calls at ~30 us
 * of driver overhead each, which is what pins it to 33 ms (two vblanks) instead
 * of 16.7. Consecutive GX primitives very often share every piece of state, so
 * merge them into one buffer upload and one draw call.
 *
 * The merge test is a hash of everything the per-draw setup below applies. If
 * the hash is unchanged, the GL state currently bound is still correct for the
 * new vertices, so they can simply be appended -- no state is re-applied and
 * nothing is re-validated. When the hash changes, the pending batch is flushed
 * FIRST (while the state it was built under is still bound), and only then is
 * the new state applied.
 *
 * Only QUADS and TRIANGLES merge; strips/fans/lines cannot be concatenated
 * without changing topology, so they flush and draw alone.
 * ------------------------------------------------------------------------- */
#define BATCH_MAX_VERTS 16384
static GxOglVertex g_batch[BATCH_MAX_VERTS];
static int      g_batch_n     = 0;
static uint32_t g_batch_key   = 0;
static int      g_batch_quads = 0;

void gx_batch_flush(void) {
    if (g_batch_n <= 0) return;
    int n = g_batch_n;
    g_batch_n = 0;              /* reset first so nothing can recurse into here */

    robox_prof_scope_begin(ROBOX_PROF_DRAW);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    robox_prof_scope_begin(ROBOX_PROF_UPLOAD);
    glBufferData(GL_ARRAY_BUFFER, n * (GLsizeiptr)sizeof(GxOglVertex),
                 g_batch, GL_STREAM_DRAW);
    robox_prof_scope_end(ROBOX_PROF_UPLOAD);
    if (g_batch_quads) {
        int nq = n / 4;
        if (nq > QUAD_IBO_MAX_QUADS) nq = QUAD_IBO_MAX_QUADS;
        if (nq > 0) {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_quad_ibo);
            glDrawElements(GL_TRIANGLES, nq * 6, GL_UNSIGNED_SHORT, (void *)0);
        }
    } else {
        glDrawArrays(GL_TRIANGLES, 0, n);
    }
    robox_prof_scope_end(ROBOX_PROF_DRAW);
}

/* Hash of every input the per-draw GL setup consumes. Deliberately generous --
 * a false MISS only costs a flush, a false HIT renders wrong. */
/* ---------------------------------------------------------------------------
 * Draw-state key memoization.
 *
 * gx_draw_state_key_full() hashes ~125 values, and because MIX is a byte-wise
 * FNV step that is ~500 multiply-xors plus ~125 scattered loads -- PER DRAW.
 * At the 611 draws/frame measured in a busy level that is ~305k hash steps a
 * frame, and it was ~45% of the frame's work (gxfifo 6-8 ms of a 13-18 ms
 * frame, with the actual GL calls at 0.0 ms).
 *
 * The point of batching is that consecutive draws USUALLY share every piece of
 * state -- so nearly all of that work recomputes a hash that did not change.
 * Memoize it: every mutation of hashed state bumps g_gx_state_gen, and the key
 * is only recomputed when the generation (or the two per-draw inputs) differ.
 *
 * The hazard is a missed invalidation: a stale key merges draws that should not
 * merge, which corrupts the picture rather than crashing. So this ships with a
 * self-check -- RECOMP_GX_KEY_VERIFY=1 recomputes the full hash on every draw
 * and screams if the memo disagrees. That turns "I think I found every writer"
 * into something the running game can actually prove.
 * ------------------------------------------------------------------------- */
uint32_t g_gx_state_gen = 1;
/* GX_STATE_DIRTY() / GX_BP_REG_IS_HASHED() are declared near the top of this
 * file, because the setters that use them appear long before this point. */

static uint32_t gx_draw_state_key_full(const GxOglVertex *v0, int quads) {
    uint32_t h = 2166136261u;
    #define MIX(v) do { uint32_t _t = (uint32_t)(v); \
        for (int _i = 0; _i < 4; ++_i) { h = (h ^ ((_t >> (_i*8)) & 0xFF)) * 16777619u; } } while (0)
    MIX(g_tv_cur);                     /* TEV program = all structural TEV state */
    MIX((uint32_t)quads);
    MIX((uint32_t)(v0->mtx_idx));      /* selects the MVP */
    MIX(g_uv_from_pos);
    /* Matrix CONTENTS, not just the index. The game rewrites a slot in place
     * between draws, so keying the index alone merged draws that had different
     * transforms -- which rendered the menu starfield as a solid magenta
     * quad. Same for the texgen matrix and the projection. */
    {
        int mi = v0->mtx_idx / 3;
        const float *m = g_pos_mtx[(mi >= 0 && mi < N_POS_MTX) ? mi : 0];
        const uint32_t *mw = (const uint32_t *)m;
        for (int i = 0; i < 12; i++) MIX(mw[i]);
        MIX((uint32_t)g_tex0_mtx_slot);
        if (g_tex0_mtx_slot >= 0 && g_tex0_mtx_slot < N_POS_MTX) {
            const uint32_t *tw = (const uint32_t *)g_pos_mtx[g_tex0_mtx_slot];
            for (int i = 0; i < 8; i++) MIX(tw[i]);
        }
        const uint32_t *pw = (const uint32_t *)g_proj;
        for (int i = 0; i < 7; i++) MIX(pw[i]);
    }
    MIX(g_bp[0x00]); MIX(g_bp[0x28]); MIX(g_bp[0x40]); MIX(g_bp[0x41]); MIX(g_bp[0xF3]);
    for (int s = 0; s < 8; s++) {
        MIX(g_tex_obj[s].valid); MIX(g_tex_obj[s].va); MIX(g_tex_obj[s].w);
        MIX(g_tex_obj[s].h); MIX(g_tex_obj[s].fmt);
        MIX(g_tex_obj[s].ws); MIX(g_tex_obj[s].wt);
    }
    for (int i = 0; i < 4; i++)
        for (int c = 0; c < 4; c++) { MIX(g_tev_konst[i][c]); MIX(g_tev_reg[i][c]); }
    #undef MIX
    return h ? h : 1u;
}

static uint32_t gx_draw_state_key(const GxOglVertex *v0, int quads) {
    /* Only two inputs vary per draw independently of the generation counter;
     * everything else the full hash reads is covered by g_gx_state_gen. */
    uint32_t mtx = (uint32_t)v0->mtx_idx;
    uint32_t qz  = (uint32_t)quads;

    static uint32_t memo_gen, memo_mtx, memo_quads, memo_key;

    static int verify = -1;
    if (verify < 0) {
        /* g_gx_key_verify lets the web build set this without getenv: under
         * PROXY_TO_PTHREAD the guest runs on a worker whose ENV is empty, so
         * Module.ENV on the main thread never reaches getenv() here. */
        if (g_gx_key_verify >= 0) {
            verify = g_gx_key_verify != 0;
        } else {
            const char *e = getenv("RECOMP_GX_KEY_VERIFY");
            verify = (e && e[0] && e[0] != '0');
        }
        /* Announce either way: "no STALE KEY lines" is only evidence of
         * correctness if the check was actually running. */
        fprintf(stderr, "[GX-KEY] draw-key verification %s\n",
                verify ? "ON (slow -- every draw double-hashed)" : "off");
        fflush(stderr);
    }

    if (memo_key && memo_gen == g_gx_state_gen &&
        memo_mtx == mtx && memo_quads == qz) {
        if (!verify) return memo_key;
        /* Verification mode: a mismatch here means some write to hashed state
         * did not bump g_gx_state_gen. Report it loudly -- silently returning
         * the stale key is exactly the corruption this guards against. */
        uint32_t real = gx_draw_state_key_full(v0, quads);
        if (real != memo_key) {
            static unsigned bad;
            if (bad++ < 32)
                fprintf(stderr, "[GX-KEY] STALE KEY #%u: memo=%08x real=%08x "
                                "gen=%u mtx=%u quads=%u -- a writer is missing "
                                "GX_STATE_DIRTY()\n",
                        bad, memo_key, real, g_gx_state_gen, mtx, qz);
            memo_key = real;      /* prefer correctness over the memo */
        }
        return memo_key;
    }

    uint32_t k = gx_draw_state_key_full(v0, quads);
    memo_gen = g_gx_state_gen; memo_mtx = mtx; memo_quads = qz; memo_key = k;
    return k;
}

void gx_ogl_draw(uint32_t prim_type, const GxOglVertex *verts, int n_verts) {
    g_draw_calls++;
    g_frame_draw_calls++;
    if (!g_ogl_ready || n_verts <= 0) return;

    /* Player capture window (robox_mario.c): project this draw's vertices on
     * the CPU with the same MVP the shader would use and grow the screen
     * bounding box; optionally swallow the draw so the character is hidden
     * while its on-screen placement is still learned. */
    if (g_pcap_on) {
        float M[16];
        build_mvp(verts[0].mtx_idx / 3, M);
        for (int i = 0; i < n_verts; i++) {
            const GxOglVertex *v = &verts[i];
            float cx = M[0]*v->x + M[4]*v->y + M[8]*v->z  + M[12];
            float cy = M[1]*v->x + M[5]*v->y + M[9]*v->z  + M[13];
            float cw = M[3]*v->x + M[7]*v->y + M[11]*v->z + M[15];
            if (cw > -1e-6f && cw < 1e-6f) continue;
            float nx = cx / cw, ny = cy / cw;
            if (nx < -2.0f || nx > 2.0f || ny < -2.0f || ny > 2.0f) continue;
            if (!g_pcap_seen) {
                g_pcap_min_x = g_pcap_max_x = nx;
                g_pcap_min_y = g_pcap_max_y = ny;
                g_pcap_seen = 1;
                for (int k = 0; k < 7; k++) g_pcap_proj[k] = g_proj[k];
                {
                    int mi = verts[0].mtx_idx / 3;
                    const float *mm = g_pos_mtx[(mi >= 0 && mi < N_POS_MTX) ? mi : 0];
                    for (int k = 0; k < 12; k++) g_pcap_mv[k] = mm[k];
                }
                g_pcap_proj_ok = 1;
            } else {
                if (nx < g_pcap_min_x) g_pcap_min_x = nx;
                if (nx > g_pcap_max_x) g_pcap_max_x = nx;
                if (ny < g_pcap_min_y) g_pcap_min_y = ny;
                if (ny > g_pcap_max_y) g_pcap_max_y = ny;
            }
        }
        if (g_pcap_suppress) return;
    }

    /* RGH_SKIP_BLACKCOVER (default ON, =0 to disable): the wiimote-interior
     * "lights off" state is a fullscreen FLAT-BLACK video-surface quad
     * (unit-XY at z=-4095, 1-stage RASC-only TEV, vertex color 0 alpha 0)
     * painted over the fully-rendered scene — plus it stamps the depth
     * buffer, which is what killed the glowing-eyes draws. The light-switch
     * sequencing that would remove it is parked on the missing SND scene
     * (see project memory). Skip it so the interior (and the rabbid) are
     * visible; remove this once the sequencer runs. */
    if (n_verts == 4 && verts[0].z < -1000.0f &&
        (verts[0].color & 0xFFFFFFFFu) == 0 &&
        verts[0].x >= -0.01f && verts[0].x <= 1.01f) {
        static int s_skip = -1;
        if (s_skip < 0) {
            const char *e = getenv("RGH_SKIP_BLACKCOVER");
            s_skip = !(e && e[0] == '0');
            if (s_skip) {
                fprintf(stderr, "[BLACKCOVER] skipping flat-black interior cover quad (RGH_SKIP_BLACKCOVER=0 to keep)\n");
                fflush(stderr);
            }
        }
        if (s_skip) return;
    }

    /* POST-PASS BLIT OVERRIDE (RGH_POST_BLIT=0 to disable): the game's
     * fullscreen post-process quad — 4 clip-space verts spanning -1..1,
     * texturing from a >=320-wide live EFB-copy — resolves to BLACK through
     * the TEV path for a reason still unidentified (probes verify: correct
     * texture bound, sane combiner words and constants, no GL error, bright
     * source; output zero). Its intended result is the scene with subtle
     * grading, so render it as a direct 1:1 blit of the copy texture. The
     * grading subtlety is deferred until the TEV divergence is solved. */
    if (n_verts == 4 && g_tex_obj[0].valid && g_tex_obj[0].w >= 320) {
        GLuint ct = gx_ogl_efbcopy_lookup(g_tex_obj[0].va);
        /* Gate diagnostic: which condition fails when the post quad isn't
         * recognized (once/sec across all wide 4-vert draws). */
        if (gx_probe_enabled()) {
            static uint64_t s_t;
            extern uint64_t ms_now(void);
            uint64_t now = ms_now();
            if (now - s_t >= 1000) {
                s_t = now;
                fprintf(stderr, "[POST-GATE] n=4 tex0 va=0x%08x %ux%u ct=%u v0=(%.2f,%.2f,%.2f)\n",
                        g_tex_obj[0].va, g_tex_obj[0].w, g_tex_obj[0].h, ct,
                        verts[0].x, verts[0].y, verts[0].z);
                fflush(stderr);
            }
        }
        if (ct && verts[0].x <= -0.99f && verts[0].y <= -0.99f) {
            static int s_en = -1;
            if (s_en < 0) {
                const char *e = getenv("RGH_POST_BLIT");
                s_en = !(e && e[0] == '0');
            }
            if (s_en) {
                EfbCopyTex *e = NULL;
                for (int i = 0; i < 32 && !e; i++)
                    if (g_efbcopy[i].va == g_tex_obj[0].va) e = &g_efbcopy[i];
                if (e && e->fbo) {
                    static int s_logged;
                    if (!s_logged) {
                        s_logged = 1;
                        fprintf(stderr, "[POST-BLIT] fullscreen post quad -> direct blit (tex=%u %dx%d)\n",
                                ct, e->w, e->h);
                        fflush(stderr);
                    }
                    /* This path overwrites the EFB and returns, so anything
                     * still batched must land in the EFB first. */
                    gx_batch_flush();
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, e->fbo);
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_efb_fbo);
                    glBlitFramebuffer(0, 0, e->w, e->h, 0, 0, g_efb_w, g_efb_h,
                                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
                    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
                    return;
                }
            }
        }
    }
    if (n_verts > VBO_CAPACITY) n_verts = VBO_CAPACITY;

    /* Diagnostic: dump the first draws AND a periodic burst (~every 33s a
     * full frame's worth) so late-boot/menu frames are visible too. */
    extern int recomp_gx_trace(void);
    if (recomp_gx_trace() && (g_draw_calls <= 10 || (g_draw_calls % 10000u) < 8)) {
        int mtx_d = verts[0].mtx_idx / 3;
        const float *m = g_pos_mtx[mtx_d < N_POS_MTX ? mtx_d : 0];
        int ns = (int)(((g_bp[0x00] >> 10) & 0xF) + 1);
        fprintf(stderr,
            "[GX-DRAW#%u] prim=0x%02x n=%d mtx=%d\n"
            "  MV row0: %.3f %.3f %.3f %.3f\n"
            "  MV row1: %.3f %.3f %.3f %.3f\n"
            "  MV row2: %.3f %.3f %.3f %.3f\n"
            "  proj: %.3f %.3f %.3f %.3f %.3f %.3f type=%d\n"
            "  v[0]: pos=(%.3f,%.3f,%.3f) rgba=%08x\n"
            "  v[1]: pos=(%.3f,%.3f,%.3f) rgba=%08x\n"
            "  BP genmode=0x%08x stages=%d blend=0x%08x zmode=0x%08x\n"
            "  tev_color0=0x%08x tev_alpha0=0x%08x alpha_cmp=0x%08x\n",
            g_draw_calls, prim_type, n_verts, mtx_d,
            m[0],m[1],m[2],m[3], m[4],m[5],m[6],m[7], m[8],m[9],m[10],m[11],
            g_proj[0],g_proj[1],g_proj[2],g_proj[3],g_proj[4],g_proj[5],(int)g_proj[6],
            verts[0].x, verts[0].y, verts[0].z, verts[0].color,
            n_verts>1?verts[1].x:0.f, n_verts>1?verts[1].y:0.f,
            n_verts>1?verts[1].z:0.f, n_verts>1?verts[1].color:0u,
            g_bp[0x00], ns, g_bp[0x41], g_bp[0x40],
            g_bp[0xC0], g_bp[0xC1], g_bp[0xF3]);
        fflush(stderr);
    }

    int layer_host_override = g_host_tex_pending;

    /* ---- Batch merge test (must precede ALL GL state application) ----
     * Mergeable topologies only, and never the Bink/host-texture path, which
     * swaps BP state mid-draw. */
    int prim_class  = (int)(prim_type & 0xf8);
    int batchable   = !g_host_tex_pending &&
                      (prim_class == 0x80 || prim_class == 0x90);
    int want_quads  = (prim_class == 0x80);
    uint32_t key    = batchable ? gx_draw_state_key(&verts[0], want_quads) : 0;

    if (batchable && g_batch_n > 0 && key == g_batch_key &&
        want_quads == g_batch_quads && g_batch_n + n_verts <= BATCH_MAX_VERTS) {
        /* Same state as the pending batch: the bound GL state is still valid,
         * so just accumulate and skip the whole setup path. */
        memcpy(&g_batch[g_batch_n], verts, (size_t)n_verts * sizeof(GxOglVertex));
        g_batch_n += n_verts;
        return;
    }
    /* State is about to change (or this draw cannot batch): retire the pending
     * batch while the state it was recorded under is still bound. */
    gx_batch_flush();

    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    glViewport(0, 0, g_efb_w, g_efb_h);

    /* INVESTIGATE (remove later): one-time raw pipeline self-test. Draw a
     * fullscreen triangle with identity MVP and vertex color white through
     * the SAME program/VAO, probe center. Separates "pipeline broken" from
     * "game state broken". */
    {
        static int s_selftest;
        if (!s_selftest) {
            s_selftest = 1;
            GxOglVertex tv[3] = {
                { -1.f, -1.f, 0.f, 0xFFFFFFFFu, 0.f, 0.f, 0 },
                {  3.f, -1.f, 0.f, 0xFFFFFFFFu, 0.f, 0.f, 0 },
                { -1.f,  3.f, 0.f, 0xFFFFFFFFu, 0.f, 0.f, 0 },
            };
            float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            glUseProgram(g_prog);
            glUniformMatrix4fv(g_u_mvp, 1, GL_FALSE, ident);
            setup_tev_uniforms();
            glBindVertexArray(g_vao);
            glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof tv, tv);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_BLEND);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            uint8_t px0[4] = {0,0,0,0};
            glReadPixels(g_efb_w/2, g_efb_h/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px0);
            fprintf(stderr, "[GX-SELFTEST-TEV] center: %02x%02x%02x%02x efb=%dx%d\n",
                    px0[0], px0[1], px0[2], px0[3], g_efb_w, g_efb_h);
            /* second variant: trivial standalone white shader */
            GLuint wp = link_program(
                GLSL_PROLOGUE "layout(location=0) in vec3 a_pos;\n"
                "void main(){ gl_Position = vec4(a_pos,1.0); }\n",
                GLSL_PROLOGUE "out vec4 o;\n"
                "void main(){ o = vec4(1.0,0.0,0.0,1.0); }\n");
            glUseProgram(wp);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            uint8_t px[4] = {0,0,0,0};
            glReadPixels(g_efb_w/2, g_efb_h/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            fprintf(stderr, "[GX-SELFTEST] center after ident-tri: %02x%02x%02x%02x glerr=0x%x\n",
                    px[0], px[1], px[2], px[3], glGetError());
            fflush(stderr);
        }
    }

    /* MVP for the first vertex's matrix index */
    float mvp[16];
    int mtx_idx = verts[0].mtx_idx / 3;
    build_mvp(mtx_idx, mvp);

    if ((gpu_flat() && g_prog_flat) || (gpu_simple() && g_prog_simple)) {
        /* Measurement bypass: replace only the fragment shader, keeping the
         * same geometry, textures, blend and overdraw. */
        int simple = gpu_simple() && g_prog_simple;
        glUseProgram(simple ? g_prog_simple : g_prog_flat);
        glUniformMatrix4fv(simple ? g_u_mvp_simple : g_u_mvp_flat, 1, GL_FALSE, mvp);
        robox_prof_scope_begin(ROBOX_PROF_DRAW);
        glBindVertexArray(g_vao);
        glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
        GLsizeiptr vb = n_verts * (GLsizeiptr)sizeof(GxOglVertex);
        robox_prof_scope_begin(ROBOX_PROF_UPLOAD);
        glBufferData(GL_ARRAY_BUFFER, vb, verts, GL_STREAM_DRAW);
        robox_prof_scope_end(ROBOX_PROF_UPLOAD);
        if ((prim_type & 0xf8) == 0x80) {
            int nq = n_verts / 4; if (nq > QUAD_IBO_MAX_QUADS) nq = QUAD_IBO_MAX_QUADS;
            if (nq > 0) { glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_quad_ibo);
                glDrawElements(GL_TRIANGLES, nq*6, GL_UNSIGNED_SHORT, (void*)0); }
        } else {
            GLenum m = ((prim_type&0xf8)==0x98)?GL_TRIANGLE_STRIP:((prim_type&0xf8)==0xa0)?GL_TRIANGLE_FAN:GL_TRIANGLES;
            glDrawArrays(m, 0, n_verts);
        }
        robox_prof_scope_end(ROBOX_PROF_DRAW);
        return;
    }

    /* Pick the shader specialised for this draw's full TEV state, then bind it.
     * Repoints g_prog and every g_u_* at the variant, so the code below is
     * unchanged; structural uniform writes become no-ops (locations -1). */
    select_tev_current();
    glUniformMatrix4fv(g_u_mvp, 1, GL_FALSE, mvp);
    glUniform1i(g_u_uv_from_pos, g_uv_from_pos);
    if (g_uv_from_pos) {
        /* texgen substitute: uv = texmtx rows 0/1 dotted with (pos,1).
         * Slot -1 or unloaded -> identity mapping (uv = pos.xy). */
        static const float ident0[4] = {1,0,0,0}, ident1[4] = {0,1,0,0};
        const float *r0 = ident0, *r1 = ident1;
        if (g_tex0_mtx_slot >= 0 && g_tex0_mtx_slot < N_POS_MTX) {
            const float *m = g_pos_mtx[g_tex0_mtx_slot];
            /* only if the slot was ever written (heuristic: row0 not all 0) */
            if (m[0] != 0.f || m[1] != 0.f || m[2] != 0.f || m[3] != 0.f) {
                r0 = &m[0]; r1 = &m[4];
            }
        }
        glUniform4fv(g_u_texmtx0, 1, r0);
        glUniform4fv(g_u_texmtx1, 1, r1);
    }

    /* Set up full TEV pipeline from BP registers */
    setup_tev_uniforms();

    /* Movie-as-texture override: bind the DLL-decoded frame on unit 0 and
     * force a 1-stage REPLACE TEV + identity texture matrix for this draw
     * (see gx_ogl_set_tex_host_bgra). BP state is swapped in, uniforms
     * re-fed, then restored so the game's own state is untouched. */
    if (g_host_tex_pending) {
        g_host_tex_pending = 0;
        uint32_t s_gen = g_bp[0x00], s_tref = g_bp[0x28];
        uint32_t s_c0 = g_bp[0xC0], s_a0 = g_bp[0xC1], s_ac = g_bp[0xF3];
        g_bp[0x00] = 0x00000000;    /* 1 TEV stage */
        g_bp[0x28] = 0x00000040;    /* stage0: map0 coord0 enabled */
        g_bp[0xC0] = 0x0008FFF8u;   /* color: d=TEXC, a=b=c=ZERO */
        g_bp[0xC1] = 0x0008FFC0u;   /* alpha: d=TEXA, a=b=c=ZERO */
        g_bp[0xF3] = 0x00000000u;   /* alpha test off */
        select_tev_current();       /* re-select for the forced 1-stage state */
        setup_tev_uniforms();
        g_bp[0x00] = s_gen; g_bp[0x28] = s_tref;
        g_bp[0xC0] = s_c0; g_bp[0xC1] = s_a0; g_bp[0xF3] = s_ac;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_host_tex);
        if (g_u_tex_en[0] >= 0) glUniform1i(g_u_tex_en[0], 1);
        /* uv straight from the quad's 0..1 positions */
        static const float id0[4] = {1,0,0,0}, id1[4] = {0,1,0,0};
        glUniform1i(g_u_uv_from_pos, 1);
        glUniform4fv(g_u_texmtx0, 1, id0);
        glUniform4fv(g_u_texmtx1, 1, id1);
        /* Bink's BGRA alpha is undefined (often 0) — the quad must be
         * opaque regardless of the game's blend state. */
        glDisable(GL_BLEND);
    }

    /* ---- Upload vertices (streaming ring) -------------------------------
     * This used to be an unconditional glBufferSubData at offset 0 of the same
     * buffer, immediately followed by a draw from it. That is a pipeline stall
     * on every draw call: the driver cannot let the CPU overwrite bytes the GPU
     * has not finished reading, so it must either block or silently allocate
     * behind the scenes. Desktop drivers absorb it; Mali does not -- it cost
     * ~12 ms PER DRAW on the S6, which at the menu's ~346 draws is >4 s/frame.
     *
     * Instead, sub-allocate forward through the buffer so each draw writes
     * bytes the GPU is not touching, and only when the ring wraps do we orphan
     * it (glBufferData with NULL) -- which tells the driver to hand us fresh
     * storage and discard the old contents rather than wait for them.
     * ------------------------------------------------------------------- */
    robox_prof_scope_begin(ROBOX_PROF_DRAW);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);

    GLsizeiptr vbytes = n_verts * (GLsizeiptr)sizeof(GxOglVertex);
    GLsizeiptr vcap   = (GLsizeiptr)VBO_CAPACITY * (GLsizeiptr)sizeof(GxOglVertex);

    /* Vertex streaming. History, because three plausible approaches failed:
     *   glBufferSubData at offset 0     -> 267 ms of a 274 ms frame (GPU stall)
     *   ...plus a ring cursor           -> no change
     *   glMapBufferRange UNSYNCHRONIZED -> 271 ms of 285 ms (driver syncs anyway)
     *   glBufferData per draw           -> GPU fixed, but 336 allocations/frame
     *
     * A PARTIAL update of a large buffer makes the driver preserve the
     * untouched remainder, which is what stalled. glBufferData replacing the
     * whole store fixed that, but paid a driver allocation on every draw.
     *
     * MEASURED -- do NOT "optimise" this into a shared buffer. Orphaning once
     * per frame and sub-allocating forward looks strictly better and is
     * catastrophically worse (upload 4559 ms/frame), because every
     * glBufferSubData is again a PARTIAL update of a large store and this
     * driver synchronises on those. Replacing the whole store per draw is what
     * this Midgard driver actually wants; the per-draw allocation is the
     * cheaper of the two evils by three orders of magnitude. */
    (void)vcap;
    robox_prof_scope_begin(ROBOX_PROF_UPLOAD);
    glBufferData(GL_ARRAY_BUFFER, vbytes, verts, GL_STREAM_DRAW);
    robox_prof_scope_end(ROBOX_PROF_UPLOAD);

    GLint vbase = 0;   /* whole buffer is this draw's data */

    /* Map GX prim type to GL */
    GLenum gl_mode;
    switch (prim_type & 0xf8) {
        case 0x80: gl_mode = GL_TRIANGLES;      break; /* QUADS → split below */
        case 0x90: gl_mode = GL_TRIANGLES;      break; /* TRIANGLES */
        case 0x98: gl_mode = GL_TRIANGLE_STRIP; break;
        case 0xa0: gl_mode = GL_TRIANGLE_FAN;   break;
        case 0xa8: gl_mode = GL_LINES;          break;
        case 0xb0: gl_mode = GL_LINE_STRIP;     break;
        case 0xb8: gl_mode = GL_POINTS;         break;
        default:   gl_mode = GL_TRIANGLES;      break;
    }

    if (batchable && n_verts <= BATCH_MAX_VERTS) {
        /* Start a new batch under the state just applied. It is drawn when the
         * state next changes, or at flush (present / EFB copy). Quads expand to
         * triangles through the static index buffer, so a batch of N quads is
         * still ONE indexed draw. */
        memcpy(g_batch, verts, (size_t)n_verts * sizeof(GxOglVertex));
        g_batch_n     = n_verts;
        g_batch_key   = key;
        g_batch_quads = want_quads;
        robox_prof_scope_end(ROBOX_PROF_DRAW);
    } else if ((prim_type & 0xf8) == 0x80) {
        /* Non-batchable quads (Bink path): one indexed draw, as before. */
        int n_quads = n_verts / 4;
        if (n_quads > 0) {
            if (n_quads > QUAD_IBO_MAX_QUADS) n_quads = QUAD_IBO_MAX_QUADS;
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_quad_ibo);
            (void)vbase;   /* always 0: each draw replaces the whole buffer */
            glDrawElements(GL_TRIANGLES, n_quads * 6, GL_UNSIGNED_SHORT, (void *)0);
        }
        robox_prof_scope_end(ROBOX_PROF_DRAW);
    } else {
        glDrawArrays(gl_mode, vbase, n_verts);
        robox_prof_scope_end(ROBOX_PROF_DRAW);
    }

    /* [POST-DRAW] forensic: for the POST-PASS draw (tex0 = a live EFB-copy
     * texture), verify at the GL level what actually happened: which texture
     * object unit 0 has bound vs the copy texture, the center pixel the draw
     * just produced, and the exact combiner words uploaded. Distinguishes
     * "wrong texture bound" from "shader computes black" with no guesswork. */
    {
        GLuint want = (gx_probe_enabled() && g_tex_obj[0].valid && g_tex_obj[0].w >= 320)
                      ? gx_ogl_efbcopy_lookup(g_tex_obj[0].va) : 0;
        if (want) {
            static uint64_t s_t;
            extern uint64_t ms_now(void);
            uint64_t now = ms_now();
            if (now - s_t >= 1000) {
                s_t = now;
                GLint bound = 0;
                glActiveTexture(GL_TEXTURE0);
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
                uint8_t px[4] = {0,0,0,0};
                glReadPixels(g_efb_w/2, g_efb_h/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                int ns = (int)(((g_bp[0x00] >> 10) & 0xF) + 1);
                fprintf(stderr,
                    "[POST-DRAW] bound=%d want=%u out=%02x%02x%02x%02x stages=%d "
                    "cw=%06x/%06x/%06x k0=%02x%02x%02x%02x glerr=0x%x\n",
                    bound, want, px[0], px[1], px[2], px[3], ns,
                    g_bp[0xC0], g_bp[0xC2], g_bp[0xC4],
                    g_tev_konst[0][0], g_tev_konst[0][1], g_tev_konst[0][2], g_tev_konst[0][3],
                    glGetError());
                fflush(stderr);
            }
        }
    }

    if (g_layer_dump)
        layer_dump_record(prim_type, verts, n_verts, layer_host_override);

    /* INVESTIGATE (remove later): did fragments land? Sample a 16x16 patch
     * at the EFB center after each of the first draws; log deltas + GL
     * errors. Answers "clip/state kills fragments" vs "present bug". */
    /* glReadPixels mid-frame forces a full GPU sync — several ms each time.
     * Debug only (RECOMP_GX_TRACE=1). */
    if (recomp_gx_trace() && (g_draw_calls <= 24 || (g_draw_calls % 20000u) < 2)) {
        GLenum err = glGetError();
        uint8_t px[16*16*4];
        glReadPixels(g_efb_w/2 - 8, g_efb_h/2 - 8, 16, 16, GL_RGBA,
                     GL_UNSIGNED_BYTE, px);
        uint32_t first = ((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2];
        int diff = 0;
        for (int i = 1; i < 16*16; ++i) {
            uint32_t v = ((uint32_t)px[i*4] << 16) | ((uint32_t)px[i*4+1] << 8) | px[i*4+2];
            if (v != first) { diff = 1; break; }
        }
        fprintf(stderr, "[GX-PROBE#%u] center px=%06x uniform=%d glerr=0x%x depth=%d blend=%d"
                " tex0=%d(fmt%u) tref=%03x tevc=%06x teva=%06x stages=%d\n",
                g_draw_calls, first, !diff, err,
                glIsEnabled(GL_DEPTH_TEST), glIsEnabled(GL_BLEND),
                g_tex_obj[0].valid, g_tex_obj[0].fmt,
                g_bp[0x28] & 0xFFF, g_bp[0xC0], g_bp[0xC1],
                (int)(((g_bp[0x00] >> 10) & 0xF) + 1));
        fflush(stderr);
    }
}

/* ---- EFB copy / present ------------------------------------------------ */

void gx_ogl_efb_copy(int is_xfb, uint32_t dst_va, uint32_t w, uint32_t h,
                      uint32_t dst_stride, uint32_t clear_color_argb) {
    if (!g_ogl_ready) return;
    (void)dst_va; (void)dst_stride; (void)w; (void)h;
    clear_color_argb = apply_clear_override(clear_color_argb);
    {   /* Same reporting as gx_ogl_efb_clear -- this is the other path that
         * can paint the whole screen a flat colour. */
        static uint32_t last = 0xDEADBEEFu;
        if (clear_color_argb != last) {
            last = clear_color_argb;
            fprintf(stderr, "[EFB-COPY-CLEAR] argb=0x%08x xfb=%d draws_this_frame=%u\n",
                    clear_color_argb, is_xfb, g_frame_draw_calls);
            fflush(stderr);
        }
    }

    /* Boot loading gap: before the game has drawn a single frame it copies an
     * EFB cleared to its own purple (0x802b94) straight to the display, so the
     * port opens on about a second of flat purple between the splash and the
     * first real frame. Present black instead until geometry actually arrives.
     *
     * One-way latch on the first frame that draws anything, so this only ever
     * affects the initial gap -- in-game loading pauses keep whatever colour
     * the game asks for. Purely cosmetic: the clear still happens, only the
     * colour changes, and nothing downstream reads it back. */
    {
        static int seen_real_frame = 0, announced = 0;
        if (g_frame_draw_calls > 0) seen_real_frame = 1;
        else if (!seen_real_frame) {
            if (!announced) {
                announced = 1;
                fprintf(stderr, "[EFB-COPY-CLEAR] boot gap: forcing 0x%08x -> black "
                                "until the first frame with geometry\n", clear_color_argb);
                fflush(stderr);
            }
            clear_color_argb = 0xff000000u;
        }
    }

    if (is_xfb) {
        /* TEMP(dark-text hunt): where in the frame does the present fall?
         * If the game copies mid-frame, everything drawn after (the white
         * text pass) never reaches the window. */
        /* Blit the drawn EFB content to the window before clearing */
        gx_ogl_present();
    }

    /* Clear EFB with the clear colour */
    float r = ((clear_color_argb >> 16) & 0xff) / 255.0f;
    float g_ = ((clear_color_argb >>  8) & 0xff) / 255.0f;
    float b_ = ( clear_color_argb        & 0xff) / 255.0f;
    float a  = ((clear_color_argb >> 24) & 0xff) / 255.0f;

    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    glClearColor(r, g_, b_, a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

// Bink direct-display: blit the DLL-decoded movie frame into the WINDOW as
// the BACKGROUND layer (the game's own movie texture upload path is not
// implemented, so its in-EFB video quad is skipped while a movie is open —
// see raster_primitive). Returns 1 if a frame was laid down. NO swap here:
// gx_ogl_present() then alpha-blends the EFB (menus, fades, HUD) on top and
// swaps once. The old behavior — movie REPLACES the frame — hid the entire
// title menu behind the attract/background movie.
static GLuint g_bink_tex, g_bink_fbo;
static int gx_ogl_blit_bink_background(void) {
    extern const void *bink_hle_get_frame(int *w, int *h);
    int bw = 0, bh = 0;
    const void *pix = bink_hle_get_frame(&bw, &bh);
    if (!pix || bw <= 0 || bh <= 0) return 0;

    SDL_Window *win = SDL_GL_GetCurrentWindow();
    int ww = bw, wh = bh;
    if (win) SDL_GetWindowSize(win, &ww, &wh);

    if (!g_bink_tex) {
        glGenTextures(1, &g_bink_tex);
        glGenFramebuffers(1, &g_bink_fbo);
    }
    glBindTexture(GL_TEXTURE_2D, g_bink_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // Bink SURFACE32 is BGRA in memory.
    GX_BGRA_SWIZZLE();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bw, bh, 0, GX_BGRA_FORMAT,
                 GL_UNSIGNED_BYTE, pix);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_bink_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g_bink_tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    /* letterbox 4:3 */
    int dst_x = 0, dst_y = 0, dst_w = ww, dst_h = wh;
    float want_ar = (float)bw / (float)bh, have_ar = (float)ww / (float)wh;
    if (have_ar > want_ar) { dst_w = (int)(wh * want_ar); dst_x = (ww - dst_w) / 2; }
    else                   { dst_h = (int)(ww / want_ar); dst_y = (wh - dst_h) / 2; }
    glClearColor(0,0,0,1);
    glClear(GL_COLOR_BUFFER_BIT);
    // src is top-down (row 0 = top); flip V by swapping src y coords.
    glBlitFramebuffer(0, bh, bw, 0, dst_x, dst_y, dst_x+dst_w, dst_y+dst_h,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    return 1;
}

/* Minimal textured-quad pass: draws the EFB color texture over the current
 * DRAW framebuffer with standard alpha blending. Used to composite the
 * game's UI on top of the movie background. */
static GLuint g_ovl_prog, g_ovl_vao, g_ovl_vbo;
static void gx_ogl_overlay_efb(int dst_x, int dst_y, int dst_w, int dst_h) {
    if (!g_ovl_prog) {
        static const char *vs =
            GLSL_PROLOGUE
            "layout(location=0) in vec2 a_pos;\n"
            "out vec2 v_uv;\n"
            "void main(){ v_uv = a_pos * 0.5 + 0.5;\n"
            "  gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
        static const char *fs =
            GLSL_PROLOGUE
            "in vec2 v_uv;\n"
            "uniform sampler2D u_tex;\n"
            "out vec4 o_col;\n"
            "void main(){ o_col = texture(u_tex, v_uv); }\n";
        g_ovl_prog = link_program(vs, fs);
        if (!g_ovl_prog) return;
        glGenVertexArrays(1, &g_ovl_vao);
        glGenBuffers(1, &g_ovl_vbo);
        glBindVertexArray(g_ovl_vao);
        glBindBuffer(GL_ARRAY_BUFFER, g_ovl_vbo);
        static const float tri[6] = { -1.f, -1.f, 3.f, -1.f, -1.f, 3.f };
        glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
        glEnableVertexAttribArray(0);
        glUseProgram(g_ovl_prog);
        glUniform1i(glGetUniformLocation(g_ovl_prog, "u_tex"), 0);
    }
    if (!g_ovl_prog) return;
    glUseProgram(g_ovl_prog);
    glBindVertexArray(g_ovl_vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_efb_col);
    glViewport(dst_x, dst_y, dst_w, dst_h);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

void gx_ogl_present(void) {
    if (!g_ogl_ready) return;

    /* [BRIGHT] pipeline-split probe, present side: average brightness of the
     * FINAL EFB (after the post pass and all UI draws). Pairs with the
     * scene-at-copy probe in gx_ogl_efb_copy_tex. */
    if (gx_probe_enabled()) {
        static uint64_t s_t;
        extern uint64_t ms_now(void);
        uint64_t now = ms_now();
        if (now - s_t >= 1000) {
            s_t = now;
            uint8_t px[64 * 64 * 4];
            glBindFramebuffer(GL_READ_FRAMEBUFFER, g_efb_fbo);
            glReadPixels(g_efb_w / 2 - 32, g_efb_h / 2 - 32, 64, 64, GL_RGBA,
                         GL_UNSIGNED_BYTE, px);
            uint32_t sum = 0, asum = 0;
            for (int i = 0; i < 64 * 64; ++i) {
                sum += px[i*4] + px[i*4+1] + px[i*4+2];
                asum += px[i*4+3];
            }
            fprintf(stderr, "[BRIGHT] final-efb avg=%u/255 alpha=%u/255 draws/frame=%d\n",
                    sum / (64u * 64u * 3u), asum / (64u * 64u), g_frame_draw_calls);
            fflush(stderr);
        }
    }

    g_tex_frame_stamp++;   /* allow one content-hash check per texture per frame */
    gx_ogl_layer_dump_check();
    { extern void lyn_texlist_dump_check(void); lyn_texlist_dump_check(); }
    { extern void lyn_tex_sibling_heal(void); lyn_tex_sibling_heal(); }

    /* [BOOT-OK]: announce once when a real scene is rendering (>=8 draws
     * per frame for 2s). The intro-wedge diagnosis needs this because the
     * UNIV metalabel flashes 2/3 then settles at 0 on good AND wedged
     * boots; sustained per-frame draw count is what actually differs
     * (menu/strap ~12+, wedged black screen 1-2). LaunchRabbids.bat keys
     * its restart decision off this line. */
    {
        static int s_consec, s_announced;
        if (!s_announced) {
            if (g_frame_draw_calls >= 8) {
                if (++s_consec >= 120) {
                    s_announced = 1;
                    fprintf(stderr, "[BOOT-OK] scene rendering (%d draws/frame)\n",
                            g_frame_draw_calls);
                    fflush(stderr);
                }
            } else {
                s_consec = 0;
            }
        }
    }

    /* On the web the GL context is created directly (emscripten_webgl_create_context)
     * because SDL cannot make one on a worker, so SDL has no "current GL window"
     * and SDL_GL_GetCurrentWindow() returns NULL -- which silently skipped every
     * present below and left the page black while the game ran at full speed.
     * Fall back to the window we actually created. */
    extern SDL_Window *g_window;
    SDL_Window *win = SDL_GL_GetCurrentWindow();
    if (!win) win = g_window;
    int ww = g_efb_w, wh = g_efb_h;
    if (win) SDL_GetWindowSize(win, &ww, &wh);
    g_present_win_w = ww;
    g_present_win_h = wh;
    if (g_pcap_fresh > 0) g_pcap_fresh--;   /* player quad ages per present  */
    cam_assist_frame_decay();
#if defined(__EMSCRIPTEN__)
    /* SDL never learned the real canvas size here (the context was created
     * outside SDL), so it still reports the 300x150 default while the canvas is
     * 1280x720 -- the frame was being blitted into a small corner of the buffer.
     * Ask the canvas itself. */
    {
        int cw = 0, ch = 0;
        extern int robox_web_canvas_size(int *w, int *h);
        if (robox_web_canvas_size(&cw, &ch) && cw > 0 && ch > 0) { ww = cw; wh = ch; }
    }
#endif

    /* The movie now renders INSIDE the EFB as the video quad's texture
     * (gx_ogl_set_tex_host_bgra), so the game's own draw order layers it.
     * Only when the game drew NOTHING this frame (early boot, loading gaps)
     * fall back to blitting the movie frame directly so the screen isn't
     * blank. */
    if (g_frame_draw_calls == 0 && gx_ogl_blit_bink_background()) {
        g_frame_draw_calls = 0;
        /* The menu has to draw on THIS path too. It keeps taking input while
         * open regardless of which present path runs, so rendering it on only
         * one of them means that on a movie/loading frame it is invisible and
         * still swallowing every key -- which reads as the game hanging. */
        { extern void robox_mario_overlay_render(void); robox_mario_overlay_render(); }
        { extern void robox_lua_render(void); robox_lua_render(); }
        { extern void robox_menu_render(void); robox_menu_render(); }
        if (g_show_fps) gx_ogl_render_fps();
        glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
        if (win) robox_gl_swap(win);
        return;
    }


    /* Letterbox: maintain the game's output aspect. The 640x480 EFB is 4:3
     * geometrically, but in widescreen the game renders anamorphically (the
     * 16:9 image squished into 640 wide) and expects the display to un-squish
     * it to 16:9. recomp_widescreen() (RECOMP_WIDESCREEN=1) matches the 16:9
     * reported by SCGetAspectRatio. */
    extern int recomp_widescreen(void);
    int dst_x = 0, dst_y = 0, dst_w = ww, dst_h = wh;
    float want_ar = recomp_widescreen() ? (16.0f / 9.0f)
                                        : ((float)g_efb_w / (float)g_efb_h);
    float have_ar = (float)ww / (float)wh;
    if (have_ar > want_ar) {
        dst_w = (int)(wh * want_ar);
        dst_x = (ww - dst_w) / 2;
    } else {
        dst_h = (int)(ww / want_ar);
        dst_y = (wh - dst_h) / 2;
    }

    /* Publish the letterbox rect so input can map window coords into GAME
     * coords. Without this the IR pointer normalizes against the whole window
     * while the image occupies only part of it, so the cursor the player sees
     * and the pointer the game receives drift apart by the size of the bars.
     * Invisible at the default 1280x720 (already 16:9) and unavoidable on a
     * phone, where the panel aspect never matches the game's. */
    /* Everything still batched belongs to the frame being presented. */
    gx_batch_flush();

    gx_ogl_set_present_rect(dst_x, dst_y, dst_w, dst_h);

    /* New frame: force the vertex ring to orphan on its next use, so the
     * driver gives us storage the GPU has finished with rather than making us
     * wait for the frame just submitted. */
    g_vbo_frame_open = 0;

    /* Feed the on-screen FPS counter.
     *
     * video_set_fps() is called from K3D::Flip, which Robox never uses -- it
     * paces its sim inside VIWaitForRetrace instead -- so g_current_fps sat at
     * 0.0 and the overlay read "0.0 FPS". Measure here, where frames actually
     * get presented. Averaged over a second so the number is readable rather
     * than flickering every frame. */
    {
        extern uint64_t ms_now(void);
        extern void video_set_fps(double fps, double ms);
        static uint64_t s_win_start;
        static unsigned s_frames;
        uint64_t now = ms_now();
        if (!s_win_start) s_win_start = now;
        ++s_frames;
        uint64_t elapsed = now - s_win_start;
        if (elapsed >= 500) {
            double fps = (double)s_frames * 1000.0 / (double)elapsed;
            video_set_fps(fps, (double)elapsed / (double)s_frames);
            s_win_start = now;
            s_frames    = 0;
        }
    }

    robox_prof_scope_begin(ROBOX_PROF_PRESENT);
    /* Blit EFB → default framebuffer (window), stretch to window size */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_efb_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    /* Clear bars if letterboxed */
    glClearColor(0,0,0,1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBlitFramebuffer(0, 0, g_efb_w, g_efb_h,
                      dst_x, dst_y, dst_x+dst_w, dst_y+dst_h,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);

    gx_ogl_render_touch_overlay();
    /* Mario sprite overlay (robox_mario.c) draws under the menu/fps HUD. */
    { extern void robox_mario_overlay_render(void); robox_mario_overlay_render(); }
    /* Lua mods' "draw" handlers, under the settings menu: a mod HUD must not
     * cover the panel the player opened to turn that mod off. */
    { extern void robox_lua_render(void); robox_lua_render(); }
    /* Ungated on purpose: the FPS overlay next door hides itself on
     * low-geometry frames, which for a menu would mean it blinks out exactly
     * when the scene behind it is simple. */
    { extern void robox_menu_render(void); robox_menu_render(); }
    if (g_show_fps && g_frame_draw_calls > 10) gx_ogl_render_fps();
    /* latch for the frame-drop profiler: the retrace hook runs after this
     * reset, so it would otherwise always report "0 draws" */
    g_frame_draw_calls_last = g_frame_draw_calls;
    g_frame_draw_calls = 0;
    if (win) robox_gl_swap(win);
    robox_prof_scope_end(ROBOX_PROF_PRESENT);
}

/* The game's own menu font, extracted from Assets/fonts/title.brfna by
 * tools/extract_brfna.py. Single-channel coverage: the shader already samples
 * .r, so an R8 atlas is a quarter the size of the RGBA one it replaces. */
#include "../src/robox_font_metrics.h"
#include "../src/robox_font_a8.h"

/* The splash stills, baked in by tools/bin2c.py. Ours, not the game's -- see
 * splash_load_one for why they cannot be files on disk. */
#include "../src/generated_setup/splash_logo_rspl.h"
#include "../src/generated_setup/splash_by_rspl.h"
#include "../src/generated_setup/splash_name_rspl.h"

#define font_metrics robox_font_metrics
#define wii_font_w   robox_font_w
#define wii_font_h   robox_font_h

static GLuint crash_prog = 0;
static GLuint crash_vao = 0;
static GLuint crash_vbo = 0;
static GLuint crash_tex = 0;

static void draw_crash_text(int x, int y, const char *str, float r, float g, float b, float *verts, int *num_verts, float scale) {
    int cursor_x = x;
    int cursor_y = y;
    while (*str) {
        if (*str == '\n') {
            cursor_y += (int)(24 * scale);   /* was unscaled: rows overlapped
                                              * at any scale below ~2 */
            cursor_x = x;
        } else {
            unsigned char c = (unsigned char)*str;
            if (font_metrics[c].width > 0) {
                float u0 = (float)font_metrics[c].x / (float)wii_font_w;
                float v0 = (float)font_metrics[c].y / (float)wii_font_h;
                float u1 = (float)(font_metrics[c].x + font_metrics[c].width) / (float)wii_font_w;
                float v1 = (float)(font_metrics[c].y + font_metrics[c].height) / (float)wii_font_h;
                
                float x0 = (float)cursor_x;
                float y0 = (float)(cursor_y + font_metrics[c].yOffset * scale);
                float x1 = x0 + (float)(font_metrics[c].width * scale);
                float y1 = y0 + (float)(font_metrics[c].height * scale);

                float v[48] = {
                    x0, y0, u0, v0, r, g, b, 1.0f,
                    x1, y0, u1, v0, r, g, b, 1.0f,
                    x0, y1, u0, v1, r, g, b, 1.0f,
                    x1, y0, u1, v0, r, g, b, 1.0f,
                    x1, y1, u1, v1, r, g, b, 1.0f,
                    x0, y1, u0, v1, r, g, b, 1.0f
                };
                for (int i=0; i<48; i++) verts[(*num_verts)*8 + i] = v[i];
                *num_verts += 6;
                cursor_x += font_metrics[c].advance * scale;
            } else {
                cursor_x += (int)(6 * scale);   /* space; was an unscaled 12 */
            }
        }
        str++;
    }
}

extern SDL_Window *g_window;

static void init_text_renderer(void) {
    if (crash_prog) return;
    const char *vs_src = 
        GLSL_PROLOGUE
        "layout(location=0) in vec4 a_pos_uv;\n"
        "layout(location=1) in vec4 a_color;\n"
        "out vec2 v_uv;\n"
        "out vec4 v_color;\n"
        "void main() {\n"
        "  gl_Position = vec4((a_pos_uv.x / 640.0) - 1.0, 1.0 - (a_pos_uv.y / 360.0), 0.0, 1.0);\n"
        "  v_uv = a_pos_uv.zw;\n"
        "  v_color = a_color;\n"
        "}\n";
    const char *fs_src = 
        GLSL_PROLOGUE
        "in vec2 v_uv;\n"
        "in vec4 v_color;\n"
        "out vec4 FragColor;\n"
        "uniform sampler2D u_tex;\n"
        "void main() {\n"
        "  vec4 t = texture(u_tex, v_uv);\n"
        /* Vertex alpha multiplies the glyph coverage. It used to be dropped,
         * which made every drawn pixel fully opaque -- no translucent panel,
         * no fade, no dimmed row. Text passes a=1 and is unaffected. */
        "  FragColor = vec4(v_color.rgb, t.r * v_color.a);\n"
        "}\n";

    GLuint vs = pglCreateShader(GL_VERTEX_SHADER);
    pglShaderSource(vs, 1, &vs_src, NULL);
    pglCompileShader(vs);
    GLuint fs = pglCreateShader(GL_FRAGMENT_SHADER);
    pglShaderSource(fs, 1, &fs_src, NULL);
    pglCompileShader(fs);
    
    crash_prog = pglCreateProgram();
    pglAttachShader(crash_prog, vs);
    pglAttachShader(crash_prog, fs);
    pglLinkProgram(crash_prog);
    
    pglGenVertexArrays(1, &crash_vao);
    pglGenBuffers(1, &crash_vbo);
    
    pglBindVertexArray(crash_vao);
    pglBindBuffer(GL_ARRAY_BUFFER, crash_vbo);
    pglVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
    pglEnableVertexAttribArray(0);
    pglVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(4 * sizeof(float)));
    pglEnableVertexAttribArray(1);
    
    glGenTextures(1, &crash_tex);
    glBindTexture(GL_TEXTURE_2D, crash_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    /* R8: one byte of coverage per texel. GL_UNPACK_ALIGNMENT must drop to 1
     * or a width that is not a multiple of 4 is read with row padding that
     * does not exist, which shears the atlas diagonally. */
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, robox_font_w, robox_font_h, 0,
                 GL_RED, GL_UNSIGNED_BYTE, robox_font_a8);

    /* One opaque texel in the last corner, so solid rectangles can be drawn by
     * the same program as text -- panels and labels then share one buffer and
     * one draw call. The atlas ends at glyph row ~94 and the tail is empty, so
     * this cannot land on a glyph. GL_LINEAR would bleed the neighbouring
     * transparent texels in at the quad edges, hence sampling the centre only
     * (see OVL_WHITE_U/V). */
    {
        static const unsigned char white = 255;
        glTexSubImage2D(GL_TEXTURE_2D, 0, robox_font_w - 1, robox_font_h - 1, 1, 1,
                        GL_RED, GL_UNSIGNED_BYTE, &white);
    }
}

extern int g_show_fps;
extern double g_current_fps;

void gx_ogl_render_fps(void) {
    if (!g_ogl_ready || !g_show_fps) return;
    init_text_renderer();
    
    float verts[1024];
    int num_verts = 0;
    
    char fps_str[64];
    snprintf(fps_str, sizeof(fps_str), "%.1f FPS", g_current_fps);
    
    float r, g, b;
    if (g_current_fps >= 60.0) { r = 1.0f; g = 1.0f; b = 1.0f; }
    else if (g_current_fps >= 31.0) { r = 1.0f; g = 1.0f; b = 0.0f; }
    else { r = 1.0f; g = 0.0f; b = 0.0f; }
    
    draw_crash_text(920, 650, fps_str, r, g, b, verts, &num_verts, 4.0f);
    
    pglBindFramebuffer(GL_FRAMEBUFFER, 0); // screen
    int w, h;
    SDL_GetWindowSize(g_window, &w, &h);
    glViewport(0, 0, w, h);
    
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    
    pglUseProgram(crash_prog);
    glBindTexture(GL_TEXTURE_2D, crash_tex);
    
    pglBindVertexArray(crash_vao);
    pglBindBuffer(GL_ARRAY_BUFFER, crash_vbo);
    pglBufferData(GL_ARRAY_BUFFER, num_verts * 8 * sizeof(float), verts, GL_DYNAMIC_DRAW);
    
    glDrawArrays(GL_TRIANGLES, 0, num_verts);
}

/* ---------------------------------------------------------------------------
 * Overlay drawing API (panels + text), shared by anything that needs to draw
 * over the presented frame.
 *
 * Everything is in a virtual 1280x720 space: the vertex shader divides by
 * 640/360, so the layout is independent of the real window size. That matters
 * on web, where SDL never learns the true canvas size and reports a stale one
 * -- laying out in window pixels puts the result in the wrong place there.
 *
 * Panels and glyphs come from the same atlas and the same program, so a whole
 * menu is one buffer upload and one draw call.
 * ------------------------------------------------------------------------- */

/* A 20-row rebind list is ~800 glyphs; 2048 quads leaves generous headroom and
 * lives in .bss rather than on a stack that would blow up silently. */
#define OVL_MAX_QUADS 2048
static float g_ovl_verts[OVL_MAX_QUADS * 48];
static int   g_ovl_nverts;

/* Centre of the opaque texel written by init_text_renderer(). Sampling the
 * centre keeps GL_LINEAR from bleeding in the transparent neighbours. */
#define OVL_WHITE_U (((float)wii_font_w - 0.5f) / (float)wii_font_w)
#define OVL_WHITE_V (((float)wii_font_h - 0.5f) / (float)wii_font_h)

static void ovl_quad(float x0, float y0, float x1, float y1,
                     float u0, float v0, float u1, float v1,
                     float r, float g, float b, float a) {
    if (g_ovl_nverts + 6 > OVL_MAX_QUADS * 6) return;   /* drop, never scribble */
    float *p = &g_ovl_verts[g_ovl_nverts * 8];
    const float q[48] = {
        x0, y0, u0, v0, r, g, b, a,
        x1, y0, u1, v0, r, g, b, a,
        x0, y1, u0, v1, r, g, b, a,
        x1, y0, u1, v0, r, g, b, a,
        x1, y1, u1, v1, r, g, b, a,
        x0, y1, u0, v1, r, g, b, a
    };
    for (int i = 0; i < 48; ++i) p[i] = q[i];
    g_ovl_nverts += 6;
}

/* --- overlay images ------------------------------------------------------
 *
 * The batch above is one draw call with the font atlas bound, which is what
 * makes it cheap and is also why it can only draw glyphs and flat colour. Real
 * game art needs a different texture, so images are collected separately and
 * drawn after the batch, one quad each. That ordering is deliberate: an image
 * is nearly always a cursor or an icon that belongs on top.
 *
 * Textures are loaded from .tpl -- the game's own format -- through the same
 * decoder the renderer uses for guest textures, so anything in Assets/ can be
 * drawn by a mod without converting it first.
 * ------------------------------------------------------------------------ */
#define OVL_MAX_TEX     16
#define OVL_MAX_IMG     64

static GLuint g_ovl_tex[OVL_MAX_TEX];
static int    g_ovl_tex_n;
static struct { int tex; float x, y, w, h, r, g, b, a; } g_ovl_img[OVL_MAX_IMG];
static int    g_ovl_img_n;

/* Returns a handle for gx_ogl_overlay_image, or -1. Safe to call once at
 * startup; the GL context is up by then (video_init runs before the mods). */
int gx_ogl_overlay_load_tpl(const char *path) {
    if (g_ovl_tex_n >= OVL_MAX_TEX) return -1;

    /* Accept a path relative to the game folder or to Assets/, so a mod can
     * write the asset path the way it appears in the game's own data. */
    const char *tries[3];
    char a[512], b[512];
    snprintf(a, sizeof a, "Assets/%s", path);
    snprintf(b, sizeof b, "../Assets/%s", path);
    tries[0] = path; tries[1] = a; tries[2] = b;

    FILE *f = NULL;
    for (int i = 0; i < 3 && !f; ++i) f = fopen(tries[i], "rb");
    if (!f) { fprintf(stderr, "[ovl] tpl not found: %s\n", path); return -1; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 32 || sz > (16 << 20)) { fclose(f); return -1; }
    uint8_t *d = (uint8_t *)malloc((size_t)sz);
    if (!d) { fclose(f); return -1; }
    if (fread(d, 1, (size_t)sz, f) != (size_t)sz) { free(d); fclose(f); return -1; }
    fclose(f);

    #define BE32(p) ((uint32_t)((p)[0]<<24 | (p)[1]<<16 | (p)[2]<<8 | (p)[3]))
    #define BE16(p) ((uint16_t)((p)[0]<<8 | (p)[1]))
    const uint32_t tbl = BE32(d + 8);
    if (tbl + 8 > (uint32_t)sz) { free(d); return -1; }
    const uint32_t ih = BE32(d + tbl);
    if (ih + 12 > (uint32_t)sz) { free(d); return -1; }
    const int      th   = BE16(d + ih);
    const int      tw   = BE16(d + ih + 2);
    const uint32_t fmt  = BE32(d + ih + 4);
    const uint32_t doff = BE32(d + ih + 8);
    #undef BE32
    #undef BE16
    if (tw <= 0 || th <= 0 || tw > 2048 || th > 2048 || doff >= (uint32_t)sz) {
        free(d); return -1;
    }

    uint8_t *rgba = (uint8_t *)calloc((size_t)tw * th, 4);
    if (!rgba) { free(d); return -1; }
    decode_gx_texture(rgba, d + doff, tw, th, (int)fmt);
    free(d);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba);
    free(rgba);

    g_ovl_tex[g_ovl_tex_n] = tex;
    fprintf(stderr, "[ovl] loaded %s (%dx%d fmt %u) -> handle %d\n",
            path, tw, th, fmt, g_ovl_tex_n);
    return g_ovl_tex_n++;
}

void gx_ogl_overlay_image(int handle, float x, float y, float w, float h,
                          float r, float g, float b, float a) {
    if (handle < 0 || handle >= g_ovl_tex_n) return;
    if (g_ovl_img_n >= OVL_MAX_IMG) return;
    g_ovl_img[g_ovl_img_n].tex = handle;
    g_ovl_img[g_ovl_img_n].x = x; g_ovl_img[g_ovl_img_n].y = y;
    g_ovl_img[g_ovl_img_n].w = w; g_ovl_img[g_ovl_img_n].h = h;
    g_ovl_img[g_ovl_img_n].r = r; g_ovl_img[g_ovl_img_n].g = g;
    g_ovl_img[g_ovl_img_n].b = b; g_ovl_img[g_ovl_img_n].a = a;
    ++g_ovl_img_n;
}

void gx_ogl_overlay_begin(void) {
    init_text_renderer();
    g_ovl_nverts = 0;
    g_ovl_img_n  = 0;
}

/* Arbitrary triangle, colour per corner.
 *
 * The batcher's vertex is already x,y,u,v,r,g,b,a and it already draws
 * triangles -- a rect is just two of them pointed at the white texel. So this
 * adds no machinery, it only stops hiding what is there. It is the one
 * primitive worth exposing, because everything the rect API cannot do falls
 * out of it in a few lines of script: a gradient is two triangles with
 * different corner colours, a glow is a fan with an opaque centre and a
 * transparent rim, a light beam is a quad that fades along its length. Adding
 * disc() and beam() down here in C instead would have been more code and less
 * reach. */
void gx_ogl_overlay_tri(float x0, float y0, float r0, float g0, float b0, float a0,
                        float x1, float y1, float r1, float g1, float b1, float a1,
                        float x2, float y2, float r2, float g2, float b2, float a2) {
    if (g_ovl_nverts + 3 > OVL_MAX_QUADS * 6) return;   /* drop, never scribble */
    float *p = &g_ovl_verts[g_ovl_nverts * 8];
    const float v[24] = {
        x0, y0, OVL_WHITE_U, OVL_WHITE_V, r0, g0, b0, a0,
        x1, y1, OVL_WHITE_U, OVL_WHITE_V, r1, g1, b1, a1,
        x2, y2, OVL_WHITE_U, OVL_WHITE_V, r2, g2, b2, a2
    };
    for (int i = 0; i < 24; ++i) p[i] = v[i];
    g_ovl_nverts += 3;
}

void gx_ogl_overlay_rect(float x, float y, float w, float h,
                         float r, float g, float b, float a) {
    ovl_quad(x, y, x + w, y + h,
             OVL_WHITE_U, OVL_WHITE_V, OVL_WHITE_U, OVL_WHITE_V, r, g, b, a);
}

/* `track` is extra advance per glyph, in virtual px. The game's own menus are
 * set with wide letterspacing; matching it is most of what makes an overlay
 * read as part of the game instead of bolted on. */
float gx_ogl_overlay_text_width(const char *str, float scale, float track) {
    float w = 0.0f;
    for (const unsigned char *s = (const unsigned char *)str; *s; ++s) {
        if (font_metrics[*s].advance > 0) w += font_metrics[*s].advance * scale + track;
        else                             w += 6 * scale + track;
    }
    return w > 0.0f ? w - track : 0.0f;   /* no trailing gap, so right-align is exact */
}

void gx_ogl_overlay_text(float x, float y, const char *str,
                         float r, float g, float b, float a, float scale, float track) {
    float cx = x;
    for (const unsigned char *s = (const unsigned char *)str; *s; ++s) {
        if (font_metrics[*s].width > 0) {
            float u0 = (float)font_metrics[*s].x / (float)wii_font_w;
            float v0 = (float)font_metrics[*s].y / (float)wii_font_h;
            float u1 = (float)(font_metrics[*s].x + font_metrics[*s].width) / (float)wii_font_w;
            float v1 = (float)(font_metrics[*s].y + font_metrics[*s].height) / (float)wii_font_h;
            float y0 = y + font_metrics[*s].yOffset * scale;
            ovl_quad(cx, y0, cx + font_metrics[*s].width * scale,
                     y0 + font_metrics[*s].height * scale,
                     u0, v0, u1, v1, r, g, b, a);
            cx += font_metrics[*s].advance * scale + track;
        } else {
            cx += 6 * scale + track;
        }
    }
}

/* Hairline rectangle outline. The game frames its own selected menu entry this
 * way rather than filling it, so the menu borrows the same idiom. */
void gx_ogl_overlay_outline(float x, float y, float w, float h, float t,
                            float r, float g, float b, float a) {
    gx_ogl_overlay_rect(x,         y,         w, t,     r, g, b, a);
    gx_ogl_overlay_rect(x,         y + h - t, w, t,     r, g, b, a);
    gx_ogl_overlay_rect(x,         y,         t, h,     r, g, b, a);
    gx_ogl_overlay_rect(x + w - t, y,         t, h,     r, g, b, a);
}

void gx_ogl_overlay_end(void) {
    if (!g_ovl_nverts && !g_ovl_img_n) return;

    /* Snapshot every piece of GL state this function touches.
     *
     * The overlay draws in the middle of the game's own rendering, and on the
     * movie/logo path the NEXT frame reuses whatever was left bound -- there is
     * no full state reset between frames there. Leaving the overlay's program,
     * VAO, texture and viewport behind corrupted the intro logos: the white
     * background turned purple (sampling the font atlas instead of the movie
     * texture) and jumped to the top-left (our viewport, not theirs). */
    GLint prev_fbo = 0, prev_prog = 0, prev_vao = 0, prev_vbo = 0;
    GLint prev_tex = 0, prev_active = GL_TEXTURE0, prev_vp[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,    &prev_fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM,        &prev_prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING,   &prev_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING,   &prev_vbo);
    glGetIntegerv(GL_ACTIVE_TEXTURE,         &prev_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,     &prev_tex);
    glGetIntegerv(GL_VIEWPORT,               prev_vp);
    const GLboolean prev_blend = glIsEnabled(GL_BLEND);
    const GLboolean prev_depth = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean prev_cull  = glIsEnabled(GL_CULL_FACE);

    pglBindFramebuffer(GL_FRAMEBUFFER, 0);   /* the screen, not the EFB */

    /* SDL_GetWindowSize is wrong on web -- the GL context was created outside
     * SDL, so SDL never learns the real canvas size (see video.c). Ask the
     * page, exactly as gx_ogl_splash_begin does. */
    int w = 0, h = 0;
#if defined(__EMSCRIPTEN__)
    { extern int robox_web_canvas_size(int *, int *);
      if (!robox_web_canvas_size(&w, &h)) { w = 0; h = 0; } }
#endif
    if (w <= 0 || h <= 0) SDL_GetWindowSize(g_window, &w, &h);
    glViewport(0, 0, w, h);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    pglUseProgram(crash_prog);
    glBindTexture(GL_TEXTURE_2D, crash_tex);
    pglBindVertexArray(crash_vao);
    pglBindBuffer(GL_ARRAY_BUFFER, crash_vbo);
    if (g_ovl_nverts) {
        pglBufferData(GL_ARRAY_BUFFER, g_ovl_nverts * 8 * sizeof(float),
                      g_ovl_verts, GL_DYNAMIC_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, g_ovl_nverts);
    }

    /* Images, after the batch and so on top of it. Same program, same vertex
     * layout, same VAO -- only the bound texture and the UVs differ, so each
     * is a buffer upload and a draw with nothing else to reset. */
    for (int i = 0; i < g_ovl_img_n; ++i) {
        const float x0 = g_ovl_img[i].x, y0 = g_ovl_img[i].y;
        const float x1 = x0 + g_ovl_img[i].w, y1 = y0 + g_ovl_img[i].h;
        const float r = g_ovl_img[i].r, g = g_ovl_img[i].g;
        const float b = g_ovl_img[i].b, a = g_ovl_img[i].a;
        const float q[48] = {
            x0, y0, 0, 0, r, g, b, a,   x1, y0, 1, 0, r, g, b, a,
            x0, y1, 0, 1, r, g, b, a,   x1, y0, 1, 0, r, g, b, a,
            x1, y1, 1, 1, r, g, b, a,   x0, y1, 0, 1, r, g, b, a
        };
        glBindTexture(GL_TEXTURE_2D, g_ovl_tex[g_ovl_img[i].tex]);
        pglBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_DYNAMIC_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    g_ovl_img_n = 0;

    /* Put it all back, in the reverse order it was taken. */
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
    glActiveTexture((GLenum)prev_active);
    pglBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_vbo);
    pglBindVertexArray((GLuint)prev_vao);
    pglUseProgram((GLuint)prev_prog);
    pglBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    if (!prev_blend) glDisable(GL_BLEND);
    if (prev_depth)  glEnable(GL_DEPTH_TEST);
    if (prev_cull)   glEnable(GL_CULL_FACE);

    g_ovl_nverts = 0;
}

/* ---------------------------------------------------------------------------
 * On-screen touch controls.
 *
 * The hit-testing in video.c worked from the first Android build, but the
 * controls were invisible -- functional and unusable at the same time. This
 * draws them.
 *
 * Geometry comes straight from video_touch_overlay(), i.e. the same table the
 * hit-test uses, so the overlay cannot drift out of alignment with what is
 * actually pressable. Circles are drawn with a signed-distance field in the
 * fragment shader rather than tessellated geometry: one quad per control, and
 * it stays smooth at any DPI.
 *
 * Deliberately shares the text renderer's vertex format (vec4 pos_uv +
 * vec4 color) so it can reuse crash_vao/crash_vbo and only swap the program.
 * ------------------------------------------------------------------------- */
static GLuint touch_prog;

static void init_touch_renderer(void) {
    if (touch_prog) return;
    const char *vs_src =
        GLSL_PROLOGUE
        "layout(location=0) in vec4 a_pos_uv;\n"   /* xy = virtual px, zw = local uv */
        "layout(location=1) in vec4 a_color;\n"
        "out vec2 v_uv;\n"
        "out vec4 v_color;\n"
        "void main() {\n"
        "  gl_Position = vec4((a_pos_uv.x / 640.0) - 1.0, 1.0 - (a_pos_uv.y / 360.0), 0.0, 1.0);\n"
        "  v_uv = a_pos_uv.zw;\n"
        "  v_color = a_color;\n"
        "}\n";
    const char *fs_src =
        GLSL_PROLOGUE
        "in vec2 v_uv;\n"
        "in vec4 v_color;\n"
        "out vec4 FragColor;\n"
        "void main() {\n"
        "  float d = length(v_uv);\n"
        "  if (d > 1.0) discard;\n"
        /* Soft outer edge, and a brighter ring so the control reads as a
         * button outline rather than a flat blob over the game art. */
        "  float edge = smoothstep(1.0, 0.90, d);\n"
        "  float ring = smoothstep(0.70, 0.82, d);\n"
        "  float a = mix(v_color.a * 0.25, v_color.a, ring) * edge;\n"
        "  FragColor = vec4(v_color.rgb, a);\n"
        "}\n";

    GLuint vs = pglCreateShader(GL_VERTEX_SHADER);
    pglShaderSource(vs, 1, &vs_src, NULL);
    pglCompileShader(vs);
    GLuint fs = pglCreateShader(GL_FRAGMENT_SHADER);
    pglShaderSource(fs, 1, &fs_src, NULL);
    pglCompileShader(fs);

    touch_prog = pglCreateProgram();
    pglAttachShader(touch_prog, vs);
    pglAttachShader(touch_prog, fs);
    pglLinkProgram(touch_prog);
}

/* Emit one screen-space quad carrying local uv in [-1,1] for the SDF. */
static void touch_emit_quad(float cx, float cy, float r,
                            float cr, float cg, float cb, float ca,
                            float *verts, int *nv) {
    float x0 = cx - r, y0 = cy - r, x1 = cx + r, y1 = cy + r;
    const float q[48] = {
        x0, y0, -1.f, -1.f, cr, cg, cb, ca,
        x1, y0,  1.f, -1.f, cr, cg, cb, ca,
        x0, y1, -1.f,  1.f, cr, cg, cb, ca,
        x1, y0,  1.f, -1.f, cr, cg, cb, ca,
        x1, y1,  1.f,  1.f, cr, cg, cb, ca,
        x0, y1, -1.f,  1.f, cr, cg, cb, ca,
    };
    for (int i = 0; i < 48; ++i) verts[(*nv) * 8 + i] = q[i];
    *nv += 6;
}

void gx_ogl_render_touch_overlay(void) {
    if (!g_ogl_ready || !video_touch_active()) return;

    RoboxTouchCircle c[16];
    int n = video_touch_overlay(c, 16);
    if (n <= 0) return;

    init_touch_renderer();
    init_text_renderer();

    /* The overlay shaders work in a fixed 1280x720 virtual space (see the
     * vertex shader), which is also what draw_crash_text expects -- so labels
     * and circles line up without any extra transform. */
    const float VW = 1280.0f, VH = 720.0f, VMIN = 720.0f;

    float verts[16 * 6 * 8];
    int nv = 0;
    for (int i = 0; i < n; ++i) {
        float a = c[i].pressed ? 0.85f : 0.40f;
        touch_emit_quad(c[i].cx * VW, c[i].cy * VH, c[i].r * VMIN,
                        1.0f, 1.0f, 1.0f, a, verts, &nv);
    }

    pglBindFramebuffer(GL_FRAMEBUFFER, 0);
    int w, h;
    SDL_GetWindowSize(g_window, &w, &h);
    glViewport(0, 0, w, h);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    pglUseProgram(touch_prog);
    pglBindVertexArray(crash_vao);
    pglBindBuffer(GL_ARRAY_BUFFER, crash_vbo);
    pglBufferData(GL_ARRAY_BUFFER, nv * 8 * sizeof(float), verts, GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, nv);

    /* Labels, in the text program. Roughly centre each string on its circle:
     * the bitmap font is ~11 px per glyph at scale 1. */
    float tverts[1024];
    int tnv = 0;
    for (int i = 0; i < n; ++i) {
        if (!c[i].label) continue;
        float scale = 2.0f;
        int   len   = (int)strlen(c[i].label);
        float tw    = len * 11.0f * scale;
        draw_crash_text((int)(c[i].cx * VW - tw * 0.5f),
                        (int)(c[i].cy * VH - 10.0f * scale),
                        c[i].label, 1.0f, 1.0f, 1.0f, tverts, &tnv, scale);
    }
    if (tnv > 0) {
        pglUseProgram(crash_prog);
        glBindTexture(GL_TEXTURE_2D, crash_tex);
        pglBufferData(GL_ARRAY_BUFFER, tnv * 8 * sizeof(float), tverts, GL_DYNAMIC_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, tnv);
    }
}

void gx_ogl_render_crash_screen(const char *title, const char *details, const char *regs) {
    if (!g_ogl_ready) return;
    
    init_text_renderer();
    
    float verts[8192 * 8];
    int num_verts = 0;
    
    draw_crash_text(64, 64, title ? title : "Fatal error", 0.9f, 0.9f, 0.9f, verts, &num_verts, 2.0f);
    if (details) draw_crash_text(64, 128, details, 0.9f, 0.9f, 0.0f, verts, &num_verts, 2.0f);
    if (regs) draw_crash_text(64, 224, regs, 0.9f, 0.9f, 0.0f, verts, &num_verts, 2.0f);
    draw_crash_text(64, 520, "Press ENTER to save log and exit", 0.9f, 0.9f, 0.0f, verts, &num_verts, 2.0f);
    draw_crash_text(820, 520, "Game ded :p", 0.9f, 0.9f, 0.0f, verts, &num_verts, 4.0f);
    
    pglBindFramebuffer(GL_FRAMEBUFFER, 0); // screen
    int w, h;
    SDL_GetWindowSize(g_window, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    
    pglUseProgram(crash_prog);
    glBindTexture(GL_TEXTURE_2D, crash_tex);
    
    pglBindVertexArray(crash_vao);
    pglBindBuffer(GL_ARRAY_BUFFER, crash_vbo);
    pglBufferData(GL_ARRAY_BUFFER, num_verts * 8 * sizeof(float), verts, GL_DYNAMIC_DRAW);
    
    glDrawArrays(GL_TRIANGLES, 0, num_verts);
    
    robox_gl_swap(g_window);
}

/* ===========================================================================
 * Splash animation renderer.
 *
 * The splash is the animation in build/Assets/generate_mp4.py (and its web
 * twin index.html). It ships as its source images rather than as the rendered
 * MP4: an MP4 would need an H.264 decoder on Windows and a second path for the
 * browser, whereas replaying the animation natively needs neither, works
 * identically on every target, and renders at the window's real resolution
 * instead of a baked 1080p.
 *
 * Only the drawing lives here, because the GL entry points are pgl* wrappers
 * local to this file. The timeline and audio are in sdk/robox_splash.c.
 *
 * Coordinates are the generator's 1920x1080 design space; the vertex shader
 * maps that to NDC, so the layout is resolution-independent.
 * ========================================================================= */

#define SPLASH_IMAGES 3
static GLuint splash_prog, splash_vao, splash_vbo;
static GLuint splash_tex[SPLASH_IMAGES];
static int    splash_w[SPLASH_IMAGES], splash_h[SPLASH_IMAGES];
static int    splash_ready;

/* "RSPL" + w:u32 + h:u32 + RGBA8 rows. See tools/make_splash_assets.py. */
/* RSPL straight out of the binary rather than off disk.
 *
 * The splash runs before the game has drawn anything, and splash/ holds the
 * the port's OWN artwork -- produced by build/Assets/generate_mp4.py, not
 * extracted from the WAD. A fresh install therefore has a complete Assets/
 * tree and still no splash, so loading these from disk meant the credit
 * silently skipped on exactly the launch where someone sees it first. */
static int splash_load_one(const unsigned char *data, unsigned len,
                           const char *what, int idx)
{
    uint32_t w = 0, h = 0;
    if (len < 12 || memcmp(data, "RSPL", 4) != 0) {
        fprintf(stderr, "[splash] %s is not a valid RSPL blob\n", what);
        return 0;
    }
    memcpy(&w, data + 4, 4);
    memcpy(&h, data + 8, 4);
    if (w == 0 || h == 0 || w > 4096 || h > 4096) {
        fprintf(stderr, "[splash] %s has bad dimensions %ux%u\n", what, w, h);
        return 0;
    }
    size_t n = (size_t)w * h * 4;
    if (len - 12 < n) {
        fprintf(stderr, "[splash] %s truncated\n", what);
        return 0;
    }

    /* Uploaded straight from the read-only image; nothing to allocate or free,
     * which also retires the glFinish that used to guard a free() racing a
     * queued upload on the GL thread. */
    const uint8_t *px = data + 12;

    glGenTextures(1, &splash_tex[idx]);
    glBindTexture(GL_TEXTURE_2D, splash_tex[idx]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    /* Clamp: the slam scales past 1.0, and REPEAT would wrap the opposite
     * edge into frame at the overshoot. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, px);
    {
        GLenum err = glGetError();
        if (err) fprintf(stderr, "[splash] GL error 0x%x uploading %s\n", err, what);
    }

    splash_w[idx] = (int)w;
    splash_h[idx] = (int)h;
    return 1;
}

int gx_ogl_splash_load(void)
{
    if (splash_ready) return 1;
    if (!g_ogl_ready) return 0;

    {
        const unsigned char *blob[SPLASH_IMAGES] = {
            splash_logo_rspl, splash_by_rspl, splash_name_rspl,
        };
        unsigned blob_len[SPLASH_IMAGES] = {
            splash_logo_rspl_len, splash_by_rspl_len, splash_name_rspl_len,
        };
        static const char *what[SPLASH_IMAGES] = { "logo", "by", "name" };
        for (int i = 0; i < SPLASH_IMAGES; ++i)
            if (!splash_load_one(blob[i], blob_len[i], what[i], i)) return 0;
    }

    const char *vs_src =
        GLSL_PROLOGUE
        "layout(location=0) in vec4 a_pos_uv;\n"
        "out vec2 v_uv;\n"
        "void main() {\n"
        /* 1920x1080 design space -> NDC, y down. */
        "  gl_Position = vec4((a_pos_uv.x / 960.0) - 1.0,\n"
        "                     1.0 - (a_pos_uv.y / 540.0), 0.0, 1.0);\n"
        "  v_uv = a_pos_uv.zw;\n"
        "}\n";
    const char *fs_src =
        GLSL_PROLOGUE
        "in vec2 v_uv;\n"
        "out vec4 FragColor;\n"
        "uniform sampler2D u_tex;\n"
        /* vec4 rather than a float because glUniform4fv is already among the
         * resolved entry points and glUniform1f is not; .a carries the alpha. */
        "uniform vec4 u_tint;\n"
        "void main() {\n"
        "  vec4 t = texture(u_tex, v_uv);\n"
        "  FragColor = vec4(t.rgb * u_tint.rgb, t.a * u_tint.a);\n"
        "}\n";

    GLuint vs = pglCreateShader(GL_VERTEX_SHADER);
    pglShaderSource(vs, 1, &vs_src, NULL); pglCompileShader(vs);
    GLuint fs = pglCreateShader(GL_FRAGMENT_SHADER);
    pglShaderSource(fs, 1, &fs_src, NULL); pglCompileShader(fs);
    splash_prog = pglCreateProgram();
    pglAttachShader(splash_prog, vs);
    pglAttachShader(splash_prog, fs);
    pglLinkProgram(splash_prog);
    {
        GLint ok = 0;
        pglGetProgramiv(splash_prog, GL_LINK_STATUS, &ok);
        if (!ok) { fprintf(stderr, "[splash] shader link failed\n"); return 0; }
    }

    pglGenVertexArrays(1, &splash_vao);
    pglGenBuffers(1, &splash_vbo);
    pglBindVertexArray(splash_vao);
    pglBindBuffer(GL_ARRAY_BUFFER, splash_vbo);
    pglVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    pglEnableVertexAttribArray(0);

    splash_ready = 1;
    fprintf(stderr, "[splash] loaded %dx%d, %dx%d, %dx%d\n",
            splash_w[0], splash_h[0], splash_w[1], splash_h[1],
            splash_w[2], splash_h[2]);
    fflush(stderr);
    return 1;
}

void gx_ogl_splash_begin(void)
{
    if (!splash_ready) return;
    pglBindFramebuffer(GL_FRAMEBUFFER, 0);
    int w = 0, h = 0;
#if defined(__EMSCRIPTEN__)
    extern int robox_web_canvas_size(int *w, int *h);
    if (!robox_web_canvas_size(&w, &h)) { w = 1280; h = 720; }
#else
    SDL_GetWindowSize(g_window, &w, &h);
#endif
    if (w <= 0 || h <= 0) { w = 1280; h = 720; }
    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    pglUseProgram(splash_prog);
    pglBindVertexArray(splash_vao);
    pglBindBuffer(GL_ARRAY_BUFFER, splash_vbo);
}

/* cx,cy = centre in 1920x1080 space. rot_deg is COUNTER-CLOCKWISE on screen,
 * matching CSS rotate(-Ndeg) and PIL rotate(+N) in the two reference
 * implementations. */
void gx_ogl_splash_quad(int which, float cx, float cy,
                        float scale, float rot_deg, float alpha)
{
    if (!splash_ready || which < 0 || which >= SPLASH_IMAGES) return;
    if (alpha <= 0.0f || scale <= 0.0f) return;

    float hw = splash_w[which] * scale * 0.5f;
    float hh = splash_h[which] * scale * 0.5f;
    float a  = rot_deg * 3.14159265358979f / 180.0f;
    float ca = cosf(a), sa = sinf(a);

    /* y grows downward, so a counter-clockwise visual rotation negates the
     * sine term on y. */
    #define SPX(dx, dy) (cx + (dx) * ca + (dy) * sa)
    #define SPY(dx, dy) (cy - (dx) * sa + (dy) * ca)

    float corner[4][2] = { {-hw,-hh}, {hw,-hh}, {hw,hh}, {-hw,hh} };
    float uv[4][2]     = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    const int tri[6]   = { 0, 1, 2, 0, 2, 3 };

    float v[6 * 4];
    for (int i = 0; i < 6; ++i) {
        int c = tri[i];
        v[i*4+0] = SPX(corner[c][0], corner[c][1]);
        v[i*4+1] = SPY(corner[c][0], corner[c][1]);
        v[i*4+2] = uv[c][0];
        v[i*4+3] = uv[c][1];
    }
    #undef SPX
    #undef SPY

    {
        const float tint[4] = { 1.0f, 1.0f, 1.0f, alpha };
        pglUniform4fv(pglGetUniformLocation(splash_prog, "u_tint"), 1, tint);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, splash_tex[which]);
    pglUniform1i(pglGetUniformLocation(splash_prog, "u_tex"), 0);
    pglBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void gx_ogl_splash_end(void)
{
    if (!splash_ready) return;
    robox_gl_swap(g_window);
}

void gx_ogl_splash_free(void)
{
    if (!splash_ready) return;
    glDeleteTextures(SPLASH_IMAGES, splash_tex);
    pglDeleteProgram(splash_prog);
    splash_prog = 0;
    splash_ready = 0;
}

#endif /* !__3DS__ */
