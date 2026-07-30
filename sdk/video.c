#if !defined(__3DS__)  /* PICA200 has no OpenGL; sdk/gx_c3d.c + sdk/platform_3ds.c stand in */
// sdk/video.c -- SDL2 framebuffer backend.
//
// Purpose: present whatever the guest game puts in its XFB to a host
// window. No overlays, no "helpful" pretend content -- if the game
// renders black, we show black.
//
// Built on SDL2 for portability and future draw-primitive upgrade: once we
// start intercepting individual GX draw calls, this window can own an
// SDL_Renderer / GL context to issue real textured quads. For now it only
// blits the XFB (decoded from YUYV 4:2:2) as an ARGB texture.

#include "hle.h"
#include "../src/runtime.h"   /* MEM_R32, for the robot-section flag */
#include "gx_ogl.h"
#include "robox_discord.h"

#include <SDL2/SDL.h>
#if defined(__EMSCRIPTEN__)
#  include <emscripten/emscripten.h>
#  include <emscripten/html5.h>
#  include <emscripten/html5_webgl.h>
#  include <emscripten/threading.h>   /* main-thread proxy for audio init */
#  include <GLES3/gl3.h>          /* for the present diagnostic below */
/* Context created directly (see video_init) because SDL cannot make one on the
 * worker that PROXY_TO_PTHREAD runs the guest on. Presented explicitly. */
EMSCRIPTEN_WEBGL_CONTEXT_HANDLE g_em_gl_ctx = 0;
#endif

/* Off unless ?presentdiag=1 or the settings menu turns it on. The present
 * diagnostic in robox_gl_swap() costs a glReadPixels and a glGetError once a
 * second, and both stall the pipeline waiting on the GPU -- a bad trade on a
 * public build where nobody reads the output, and it buries real errors under
 * a console line every second. Defined on every target so the menu can carry
 * one row list; only the web build acts on it. */
int g_web_present_diag = 0;

#if defined(__EMSCRIPTEN__)
/* Real drawing-buffer size of the page canvas. SDL reports its own stale window
 * size on this build because the GL context was created outside SDL. */
int robox_web_canvas_size(int *w, int *h) {
    return emscripten_get_canvas_element_size("#canvas", w, h) == EMSCRIPTEN_RESULT_SUCCESS;
}
#endif

/* Diagnostic build only (-DROBOX_DIAG): make a fatal startup failure visible.
 * A release is GUI-subsystem with stderr at the null device, so these paths
 * report to nobody and the process simply does nothing at all. Compiles away
 * entirely in a normal build. */
#ifdef ROBOX_DIAG
extern void robox_diag_fatal(const char *what, const char *detail);
#  define ROBOX_DIAG_FATAL(w, d) robox_diag_fatal((w), (d))
#else
#  define ROBOX_DIAG_FATAL(w, d) ((void)0)
#endif

/* Publish the frame. On the web the context is created with
 * explicitSwapControl, so the browser does NOT present automatically -- the
 * offscreen back buffer has to be committed by hand. */
void robox_gl_swap(SDL_Window *w) {
#if defined(__EMSCRIPTEN__)
    (void)w;
    if (!g_em_gl_ctx) return;
    /* Sample the default framebuffer BEFORE committing: after the commit the
     * back buffer may already have been swapped away, which would read black
     * regardless of what we drew. */
    static double s_pt; static unsigned char s_px[4]; static int s_w, s_h;
    if (g_web_present_diag) {
        double now = emscripten_get_now();
        if (now - s_pt >= 1000.0) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glReadPixels(320, 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, s_px);
            emscripten_get_canvas_element_size("#canvas", &s_w, &s_h);
        }
    }
    EMSCRIPTEN_RESULT r = emscripten_webgl_commit_frame();
    /* Diagnostic: is the frame reaching the page at all? Reports once a second
     * whether commit succeeded and whether the DEFAULT framebuffer (what the
     * canvas shows) actually has non-black pixels in it. Distinguishes "we
     * never present" from "we present a black buffer". */
    if (g_web_present_diag) {
        static double s_t; static unsigned s_n; ++s_n;
        double now = emscripten_get_now();
        if (now - s_t >= 1000.0) {
            s_t = now; s_pt = now;
            int ww = 0, wh = 0;
            extern SDL_Window *g_window;
            if (g_window) SDL_GetWindowSize(g_window, &ww, &wh);
            fprintf(stderr, "[web-present] commits=%u result=%d fb0(pre-commit)=%02x%02x%02x%02x "
                            "canvas=%dx%d sdlwin=%dx%d glerr=0x%x\n",
                    s_n, (int)r, s_px[0], s_px[1], s_px[2], s_px[3],
                    s_w, s_h, ww, wh, glGetError());
            fflush(stderr);
        }
    }
#else
    if (w) SDL_GL_SwapWindow(w);
#endif
}

#include <stdio.h>
#include <string.h>
#include <stdlib.h>


// ---------------------------------------------------------------------------
// SDL audio backend for the guest's AI (Audio Interface) subsystem.
// The Wii's AI generates an interrupt every time its DMA buffer empties;
// the guest refills and retriggers DMA. We approximate by:
//   * opening SDL audio at 48 kHz s16 stereo (AI default format)
//   * exposing audio_submit(guest_va, byte_count) for the AI HLE to call
//   * the SDL callback drains a ring of pending buffers
// If the game never submits anything, we output silence.
// ---------------------------------------------------------------------------

#define AUDIO_RING_BYTES (1u << 18)   // 256 KB of pending PCM

static SDL_AudioDeviceID g_audio_dev;
static uint8_t g_audio_ring[AUDIO_RING_BYTES];
SDL_AudioSpec g_audio_got_spec;
static volatile uint32_t g_audio_ring_head;  // read  cursor
static volatile uint32_t g_audio_ring_tail;  // write cursor
static SDL_mutex *g_audio_mu;

static void audio_callback(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    SDL_LockMutex(g_audio_mu);
    uint32_t avail = g_audio_ring_tail - g_audio_ring_head;
    uint32_t n = (avail > (uint32_t)len) ? (uint32_t)len : avail;
    for (uint32_t i = 0; i < n; ++i) {
        stream[i] = g_audio_ring[(g_audio_ring_head + i) & (AUDIO_RING_BYTES - 1)];
    }
    g_audio_ring_head += n;
    SDL_UnlockMutex(g_audio_mu);
    // Pad with silence if underrun.
    if (n < (uint32_t)len) memset(stream + n, 0, (uint32_t)len - n);
}

/* Open the output device. Split out of audio_init because on the web this has
 * to execute on the browser's MAIN thread: WebAudio lives there, and
 * PROXY_TO_PTHREAD runs the guest (and therefore audio_init) on a worker, where
 * SDL's Emscripten backend dereferences an audioContext that does not exist and
 * throws "Cannot read properties of undefined (reading 'audioContext')" --
 * which killed the process mid-boot.
 *
 * Running the callback on the main thread is safe: it only drains the ring
 * under g_audio_mu, and the producer already holds that mutex. */
static void audio_open_device(void) {
    if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
    SDL_AudioSpec want = { 0 }, got = { 0 };
    extern SDL_AudioSpec g_audio_got_spec;
    want.freq = 48000;
    want.format = AUDIO_S16MSB;       // Wii AX delivers big-endian
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_callback;
    g_audio_mu = SDL_CreateMutex();
    /* Format must stay S16MSB — the ring holds big-endian s16 from both the
     * AI path and the AX mixer; letting SDL switch the device to float32
     * would play our bytes as garbage. SDL converts internally. */
    g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &got,
                                      SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!g_audio_dev) {
        fprintf(stderr, "[audio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return;
    }
    fprintf(stderr, "[audio] SDL audio @ %d Hz, %d ch, %s, %u-sample buf\n",
            got.freq, got.channels,
            got.format == AUDIO_S16MSB ? "s16be" :
            got.format == AUDIO_S16LSB ? "s16le" : "??",
            got.samples);
    g_audio_got_spec = got;
    SDL_PauseAudioDevice(g_audio_dev, 0);
}

void audio_init(void) {
    static int inited;
    if (inited) return;
    inited = 1;
    if (getenv("RECOMP_NO_AUDIO")) {   // test: mimic the 3DS no-audio stub
        fprintf(stderr, "[audio] RECOMP_NO_AUDIO set -- audio device NOT opened\n");
        return;
    }
#if defined(__EMSCRIPTEN__)
    /* Open on the main thread (see audio_open_device). Synchronous so the
     * device is live before the guest starts submitting samples. */
    emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_V, audio_open_device);
    /* A browser AudioContext starts suspended and only resumes after a real
     * user gesture, so without this the ring fills and nothing is ever heard.
     * SDL's Emscripten backend installs its own resume-on-gesture handler, but
     * only for contexts it created on the main thread -- which is exactly what
     * we just did, so a click or keypress anywhere on the page starts audio. */
    fprintf(stderr, "[audio] web: device opened on main thread; "
                    "click the page once to start sound\n");
    fflush(stderr);
#else
    audio_open_device();
#endif
}

// Called from the AI HLE when the guest submits a DMA buffer. va/len point
// at guest memory holding signed-16be stereo at 48 kHz (or whatever the
// guest's AXSetMode set -- we don't track that yet).
/* Host-buffer variant for the AX voice mixer (sdk/peripherals.c): submits
 * interleaved s16 big-endian stereo straight into the ring. */
int audio_device_freq(void) {
    extern SDL_AudioSpec g_audio_got_spec;   /* set in audio_init */
    return g_audio_got_spec.freq ? g_audio_got_spec.freq : 48000;
}
/* Bytes currently buffered in the ring — the AX mixer paces itself against
 * this so the ring stays primed (fixed-rate production at the game's
 * 59.94fps against the device's true 48kHz starved the ring and every
 * callback underran = constant crackle). */
uint32_t audio_ring_fill_bytes(void) {
    if (!g_audio_mu) return 0;
    SDL_LockMutex(g_audio_mu);
    uint32_t fill = g_audio_ring_tail - g_audio_ring_head;
    SDL_UnlockMutex(g_audio_mu);
    return fill;
}

void audio_submit_host(const void *data, uint32_t len) {
    if (!g_audio_dev) audio_init();
    if (!g_audio_dev || !data) return;
    const uint8_t *src = (const uint8_t *)data;
    SDL_LockMutex(g_audio_mu);
    for (uint32_t i = 0; i < len; ++i) {
        g_audio_ring[(g_audio_ring_tail + i) & (AUDIO_RING_BYTES - 1)] = src[i];
    }
    g_audio_ring_tail += len;
    SDL_UnlockMutex(g_audio_mu);
}

void audio_submit(uint32_t va, uint32_t len) {
    if (!g_audio_dev) audio_init();
    if (!g_audio_dev) return;
    const uint8_t *src = (const uint8_t *)ppc_host_ptr(va);
    if (!src || len == 0) return;
    SDL_LockMutex(g_audio_mu);
    for (uint32_t i = 0; i < len; ++i) {
        g_audio_ring[(g_audio_ring_tail + i) & (AUDIO_RING_BYTES - 1)] = src[i];
    }
    g_audio_ring_tail += len;
    SDL_UnlockMutex(g_audio_mu);
    static int first = 1;
    if (first) { fprintf(stderr, "[audio] first DMA submit: va=0x%08x len=%u\n", va, len); fflush(stderr); first = 0; }
}


#define EFB_WIDTH   640
#define EFB_HEIGHT  480

/* The EFB (the game's render target) is 640x480, but the Wii outputs
 * widescreen anamorphically — so the host WINDOW is 16:9. Default 1280x720;
 * override with RECOMP_WIN_W / RECOMP_WIN_H. */
#define WIN_WIDTH   1280
#define WIN_HEIGHT  720


/* On desktop this is off until F12 toggles it. On Android there is no keyboard
 * and no console to read a profiler line from, so default it ON -- an
 * on-screen frame rate is the only practical way to see performance on the
 * device. The web build can also force it on with ?fps=1. */
#if defined(__ANDROID__)
int g_show_fps = 1;
#else
int g_show_fps = 0;
#endif
double g_current_fps = 0.0;
SDL_Window   *g_window;
SDL_Renderer *g_renderer;
static SDL_Texture  *g_tex;           // streaming ARGB8888, 640x480
static uint32_t     *g_efb_pixels;    // scratch in-CPU framebuffer
static int           g_video_inited;

/* ---- vsync / frame pacing -------------------------------------------------
 *
 * Robox is a FIXED-TIMESTEP game: it advances exactly one simulation step per
 * VIWaitForRetrace, and frame_limiter() (sdk/peripherals.c) paces those steps
 * to a wall-clock 59.94 Hz for correct game speed. That is verified in the DOL
 * -- the player's own state timer at object+0x268 counts down by a constant
 * 1.0 per frame, not by elapsed seconds -- so the game produces 60 distinct
 * images per second and no more. Raising the limiter would not "unlock fps",
 * it would fast-forward the whole game (200/59.94 = 3.34x). So the sim rate is
 * deliberately left alone; the only thing worth tuning for a high-refresh
 * monitor is HOW that fixed 60 reaches the glass.
 *
 * vsync only decides WHEN a finished frame is shown, never the game speed:
 *   interval 1  (hard vsync) -- tear-free, but 59.94 into a refresh that is
 *                not a clean multiple of 60 (e.g. 200 Hz) makes each frame sit
 *                for an uneven 3-or-4 refreshes -> visible judder, and the
 *                blocking swap can also steal from the 16.68 ms sim budget on a
 *                heavy frame and drop a step.
 *   interval 0  (off)        -- frame_limiter() alone paces, evenly; a faint
 *                tear, but at high refresh the tear line is up so briefly it is
 *                hard to see, and the motion cadence is even.
 *
 * So: default to hard vsync when the panel is a clean multiple of 60 (60 / 120
 * / 240 -- even cadence, tear-free, the best case) and to OFF otherwise (200 Hz
 * and friends, where even pacing beats tear-free-but-juddering). The player can
 * flip it live from the Video menu to feel which their panel prefers, and
 * RECOMP_VSYNC overrides everything (0 off, 1 on, 2 adaptive). A VRR panel
 * (G-Sync / FreeSync) is the real fix -- with it, hard vsync paces perfectly at
 * 59.94 with no judder and no tear. */
static int g_vsync;                   /* applied interval: 0 off, 1 on, -1 adaptive */
int  video_vsync_state(void) { return g_vsync; }

/* Multiple of 60 within 1 Hz? 120/180/240 say yes; 200/144/75 say no. */
static int refresh_is_60_multiple(int hz) {
    if (hz <= 0) return 0;
    int r = hz % 60;
    return r <= 1 || r >= 59;
}

/* ---------------------------------------------------------------------------
 * nand/video.cfg -- display choices that survive a restart.
 *
 * Both settings here were adjustable and neither was remembered, so the fix
 * for a problem had to be reapplied on every launch:
 *
 *   fullscreen      F11 toggles it. Streaming a borderless-fullscreen window
 *                   over Discord can hang the present outright -- Windows
 *                   promotes a FOCUSED fullscreen window to independent flip,
 *                   bypassing the compositor, and a capture that needs a
 *                   composed copy then fights it every frame. The window
 *                   freezes while focused and runs while it is not, which is
 *                   the reverse of what anyone expects. Windowed avoids it,
 *                   and now stays avoided.
 *
 *   internal_scale  the resolution multiplier. Auto-picks from panel height,
 *                   but anyone raising it for capture wants it to stick.
 *
 * Under nand/ with controls.cfg, for the same reason: a real directory on
 * desktop and an IndexedDB-backed mount on web, so one path persists on every
 * target. Absent or unreadable means "no preference" and the defaults apply --
 * this file is never required.
 * ------------------------------------------------------------------------- */
#define VIDEO_CFG_PATH "nand/video.cfg"

/* Defined further down with the controls parser, which uses the same
 * "key = value, # comments" shape and the same trimming. */
static char *ctrl_trim(char *s);

static int s_cfg_fullscreen = -1;   /* -1 = unset, fall back to the default */
static int s_cfg_ir_scale   = -1;

static void video_cfg_load(void) {
    static int done;
    if (done) return;
    done = 1;
    FILE *f = fopen(VIDEO_CFG_PATH, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#'); if (hash) *hash = 0;
        char *eq = strchr(line, '=');   if (!eq)  continue;
        *eq = 0;
        char *k = ctrl_trim(line), *v = ctrl_trim(eq + 1);
        if (!*k || !*v) continue;
        if      (!strcmp(k, "fullscreen"))     s_cfg_fullscreen = atoi(v) ? 1 : 0;
        else if (!strcmp(k, "internal_scale")) s_cfg_ir_scale   = atoi(v);
    }
    fclose(f);
}

/* Called whenever one of them changes. Writes both every time so the file is
 * always complete and hand-editable. */
static void video_cfg_save(void) {
    extern void robox_nand_prepare(void);
    robox_nand_prepare();
    FILE *f = fopen(VIDEO_CFG_PATH, "w");
    if (!f) return;
    fprintf(f, "# Robox display settings. Written by the game; safe to edit.\n");
    fprintf(f, "# fullscreen     0 or 1   (F11 toggles it in game)\n");
    fprintf(f, "# internal_scale 1..4     resolution multiplier over 640x480\n");
    if (s_cfg_fullscreen >= 0) fprintf(f, "fullscreen = %d\n", s_cfg_fullscreen);
    if (s_cfg_ir_scale   >  0) fprintf(f, "internal_scale = %d\n", s_cfg_ir_scale);
    fclose(f);
}

/* Read by gx_ogl.c's scale picker; -1 means the user has expressed no
 * preference and the panel-height heuristic should decide. */
int video_cfg_get_ir_scale(void) { video_cfg_load(); return s_cfg_ir_scale; }

void video_cfg_set_ir_scale(int s) {
    if (s < 1 || s > 4) return;
    video_cfg_load();
    if (s_cfg_ir_scale == s) return;
    s_cfg_ir_scale = s;
    video_cfg_save();
}

/* Apply an interval to the live GL context. Called only on the present/GL
 * thread (init and the menu event loop both run there). Returns what stuck --
 * a driver may refuse adaptive, or refuse to turn vsync off. */
static int vsync_apply(int interval) {
    if (interval == -1 && SDL_GL_SetSwapInterval(-1) == 0) { g_vsync = -1; return -1; }
    if (interval != 0  && SDL_GL_SetSwapInterval(1)  == 0) { g_vsync =  1; return  1; }
    SDL_GL_SetSwapInterval(0); g_vsync = 0; return 0;
}

void video_vsync_init(void) {
    const char *e = getenv("RECOMP_VSYNC");
    int hz = 0;
    SDL_DisplayMode dm;
    int disp = g_window ? SDL_GetWindowDisplayIndex(g_window) : 0;
    if (disp >= 0 && SDL_GetCurrentDisplayMode(disp, &dm) == 0) hz = dm.refresh_rate;

    int want;
    if      (e && e[0] == '0') want = 0;    /* forced off */
    else if (e && e[0] == '2') want = -1;   /* forced adaptive */
    else if (e && e[0] == '1') want = 1;    /* forced on */
    else want = refresh_is_60_multiple(hz) ? 1 : 0;   /* auto by refresh rate */

    int got = vsync_apply(want);
    fprintf(stderr, "[video] vsync: %s (interval %d) for %d Hz display%s\n",
            got ? (got < 0 ? "adaptive" : "on") : "off", got, hz,
            (e ? "  [RECOMP_VSYNC set]"
               : (refresh_is_60_multiple(hz) ? "" : "  [not a 60x refresh -> off for even pacing]")));
    fflush(stderr);
}

/* Live toggle from the Video menu: cycle on -> off. (Adaptive is intentionally
 * not in the cycle -- it disables sync whenever fps < refresh, and the limiter
 * sits permanently just under, so it tears constantly; reachable only via
 * RECOMP_VSYNC=2 for anyone who wants it.) */
void video_toggle_vsync(void) { vsync_apply(g_vsync ? 0 : 1); }


/* ---- Real controller input -------------------------------------------------
 * The game reads buttons via WPADRead/KPADRead (sdk/peripherals.c). Those used
 * to inject a permanent fake A+2+PLUS so every confirm screen auto-advanced —
 * that's gone. This is the honest source: a live WPAD button bitmask updated
 * from real SDL keyboard events. Nothing is pressed unless the user presses it.
 *
 * WPAD button bits (Nintendo SDK):
 *   LEFT 0x0001 RIGHT 0x0002 DOWN 0x0004 UP 0x0008 PLUS 0x0010
 *   TWO  0x0100 ONE   0x0200 B    0x0400 A  0x0800 MINUS 0x1000 HOME 0x8000 */
static volatile uint32_t g_input_hold;   /* buttons currently held */
/* Virtual actions. These live ABOVE the 16-bit WPAD mask on purpose: the guest
 * only ever sees `(uint16_t)video_input_hold()` (hle_WPADRead), so anything up
 * here is invisible to it and can drive host-side state instead. That is what
 * lets tilt and shake -- which are not WPAD buttons at all -- go through the
 * same bind table, accumulator and KEYUP path as everything else. */
#define VB_TILT_L  0x00010000u
#define VB_TILT_R  0x00020000u
#define VB_SHAKE   0x00040000u
#define WPAD_DPAD  0x000Fu    /* DUP 8 | DDOWN 4 | DRIGHT 2 | DLEFT 1 */

/* Inside the robot the game rotates the d-pad a quarter turn, so the ordinary
 * directions read sideways and "up" walks you left. Rather than trying to
 * out-guess that rotation, let the player bind a SECOND set of directions that
 * is swapped in only while the robot section is up -- they set it to whatever
 * actually feels right, once, and it stops being a guessing game.
/* The accumulator before any robot-section remapping -- the settings menu
 * shows both so the rotation can be watched rather than guessed at. */
uint32_t video_input_raw(void) { return g_input_hold; }

/* --- which level are we in, RIGHT NOW ------------------------------------
 *
 * The game keeps the level it is in as one word of .sdata, and every level
 * transition writes it. Reading that word is therefore always the truth --
 * not a snapshot of whatever last happened to load off the disc.
 *
 * The word is SDA[-0x7f28]; r13 = 0x801FF460 (set at 0x80004294), so it lives
 * at 0x801F7538. Two writers, both of which mean "you are now here":
 *   0x80060c98  stw r3,-0x7f28(r13)   in func_80060bf0, the level transition;
 *                                     r3 is the level being entered
 *   0x80062f1c  stw r4,-0x7f28(r13)   the state restore (save load / return
 *                                     from a bonus), r4 = the level to resume
 * It is initialised to 0x64 in .sdata, and ~100 sites across the game read it.
 * This is the engine's own idea of where the player is, not a host invention.
 *
 * Ids are laid out 20 per world, starting at 0x64:
 *     0x64..0x77   world 1   01a .. 01m
 *     0x78..0x8b   world 2   02a .. 02j
 *     0x8c..0x9f   world 3   03a .. 03k43
 *     0xa0..0xb3   world 4   04a .. 04j      <-- the robot interiors
 *     0xb4..0xbd   world 5   05a .. 05j
 * The 0xa0 boundary is not a guess. func_80060bf0 loads the world's tileset by
 * exactly this test -- "addi r0,r3,-0xa0 / cmplwi r0,0x13" at 0x80060d2c picks
 * media/tileset_robot.lua -- and the jump table at 0x801CF028 (index id-0x64)
 * maps 0xa0..0xa9 onto lev/04a.lev .. lev/04j_doble_salto.lev.
 */
#define LEVEL_ID_VA     0x801F7538u
#define LEVEL_ID_FIRST  0x64u   /* lev/01a.lev */
#define LEVEL_ID_LAST   0xbdu   /* lev/05j.lev */
#define LEVEL_ID_ROBOT  0xa0u   /* first id of world 4 */
#define LEVEL_ID_SPAN   0x13u   /* the game's own per-world range check */

/* id -> level, transcribed straight from that jump table (index = id - 0x64).
 * The holes are real: every world reserves 20 ids and no world fills them. */
static const char *const LEVEL_NAMES[] = {
    /* 0x64 */ "01a", "01b", "01c", "01d", "01e_gusano", "01f", "01g_jefe",
               "01h_02a", "01i_03f", "01j", "01k_03k", "01l_credits",
               "01m_credits_2", 0, 0, 0, 0, 0, 0, 0,
    /* 0x78 */ "02a", "02b", "02c", "02d_gusano", "02e", "02f", "02g_01d",
               "02h", "02i_final", "02j_jefe", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x8c */ "03a", "03b", "03c", "03d", "03e", "03f", "03g", "03h",
               "03i_jefe", "03j_01e", "03k", "03k43", 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0xa0 */ "04a", "04b", "04c", "04d", "04e", "04f", "04g", "04h",
               "04i_culatazo", "04j_doble_salto", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0xb4 */ "05a", "05b", "05c", "05d", "05e", "05f", "05g", "05h",
               "05i", "05j",
};
_Static_assert(sizeof LEVEL_NAMES / sizeof *LEVEL_NAMES
                   == LEVEL_ID_LAST - LEVEL_ID_FIRST + 1,
               "level table must cover ids 0x64..0xbd exactly -- the id index "
               "into it is otherwise unbounded");

/* Basename of the .lev the game most recently opened, recorded by
 * hle_contentOpenNAND(). Only a cross-check now: it is a one-shot latch, so it
 * still reads "04a" long after you have walked back out of the robot. Kept
 * because it is the one place a wrong id would show up as a mismatch. */
static char g_level_file[64];

/* -1 when the id is not a level we know: before the first transition writes it,
 * or if it ever holds one of the reserved slots. Callers must treat -1 as
 * "don't know", never as "not a robot". */
int robox_level_id(void) {
    uint32_t id = MEM_R32(LEVEL_ID_VA);
    if (id < LEVEL_ID_FIRST || id > LEVEL_ID_LAST) return -1;
    return LEVEL_NAMES[id - LEVEL_ID_FIRST] ? (int)id : -1;
}

/* The table itself, for anything that wants to enumerate levels rather than
 * ask about the current one (robox.level.list). Returns NULL for the reserved
 * ids -- every world sets aside 20 and none fills them -- so a caller walking
 * first..last skips the holes by testing for NULL. */
int robox_level_id_first(void) { return (int)LEVEL_ID_FIRST; }
int robox_level_id_last (void) { return (int)LEVEL_ID_LAST;  }

const char *robox_level_name_of(int id) {
    if (id < (int)LEVEL_ID_FIRST || id > (int)LEVEL_ID_LAST) return NULL;
    return LEVEL_NAMES[id - LEVEL_ID_FIRST];
}

/* Live, re-read on every call. Falls back to the loaded-file latch only while
 * the id is unknown, so the menu still has something to show at boot. */
const char *robox_level_name(void) {
    int id = robox_level_id();
    return id < 0 ? g_level_file : LEVEL_NAMES[id - LEVEL_ID_FIRST];
}

/* World 4 -- every 04 level is a robot interior, the sections where the game
 * turns the d-pad a quarter turn. Asked of the guest word every single time it
 * is called: the player walks in and out of robots constantly and a cached
 * answer strands the wrong mapping on the wrong side of the door.
 *
 * Tests the RAW word rather than robox_level_id(), so this stays the game's own
 * test character for character -- the reserved slots at 0xaa..0xb3 have no
 * level file but they are still world 4, and the game would still load the
 * robot tileset for them. */
int robox_level_is_robot(void) {
    uint32_t id = MEM_R32(LEVEL_ID_VA);
    if (id >= LEVEL_ID_FIRST && id <= LEVEL_ID_LAST)
        return (id - LEVEL_ID_ROBOT) <= LEVEL_ID_SPAN;
    return g_level_file[0] == '0' && g_level_file[1] == '4';
}

void robox_level_set(const char *name) {
    if (!name) return;
    snprintf(g_level_file, sizeof g_level_file, "%s", name);
    fprintf(stderr, "[level] loading %s  (id %d = %s)%s\n", g_level_file,
            robox_level_id(), robox_level_name(),
            robox_level_is_robot() ? "  [robot interior]" : "");
    fflush(stderr);
}

/* Quarter turns applied ON FOOT, where the Wiimote is held sideways: 1 = 90
 * clockwise, which is what turns the arrow keys back into the d-pad the game
 * expects. Inside a robot the player re-grips it vertical -- the same way round
 * as a keyboard -- and the rotation goes away entirely.
 *
 * The whole rotation lives here now, and this value is ABSOLUTE, not a delta.
 * It used to be split: controls.cfg baked the on-foot quarter turn into the
 * bindings (DUP = Left) and this carried only the extra turn for robots (270,
 * because 1 + 3 == 0). That made the settings menu show "up = Left" -- our
 * workaround, in the player's face -- so the bindings were made honest and the
 * compensation moved here in one piece. Both halves have to move together:
 * de-rotating controls.cfg while this still held the delta is exactly what
 * left the controls wrong in every level.
 *
 * Deliberately NOT exposed in the settings menu; a player has no way to tell
 * which of the four is right. Left as a variable rather than a constant so the
 * next person debugging a rotation has one obvious place to poke. */
int g_sideways_mode = 1;

/* F7: invert whatever the level id concluded, for the one context the level
 * name cannot express -- piloting the little guy inside a robot, which is a
 * section of an 04 level rather than a level of its own. It inverts rather
 * than forcing a value so it stays a one-key correction in both directions,
 * and it is off by default because the automatic answer is right everywhere
 * else. */
int g_sideways_force;

/* Rotate the d-pad by q quarter turns, clockwise: up -> right -> down -> left. */
static uint32_t dpad_rotate(uint32_t d, unsigned q) {
    static const uint32_t CW[4] = { 0x0008u, 0x0002u, 0x0004u, 0x0001u };
    uint32_t r = 0;
    for (unsigned i = 0; i < 4; ++i)
        if (d & CW[i]) r |= CW[(i + q) & 3u];
    return r;
}

/* Quarter turns currently being applied, 0 for none. Also drives the debug
 * readout, so what the menu claims and what the input path does cannot drift. */
unsigned video_sideways_q(void) {
    /* Rotate on foot, straight through inside a robot -- never the other way
     * round, whatever the setting says. This is the same answer sdk/robox_coop.c
     * reaches for player 2 (coop_rotate_now), and the two must agree or the two
     * players walk in different directions from the same key.
     *
     * Recomputed per call, never latched: robox_level_is_robot() reads the
     * game's own current-level word every time, so walking through a robot
     * door changes the mapping on the very next input read. */
    int robot = robox_level_is_robot();
    if (g_sideways_force) robot = !robot;     /* F7, see above */
    if (robot) return 0;
    return (unsigned)g_sideways_mode & 3u;    /* 0 off, 1/2/3 = 90/180/270 */
}

uint32_t video_input_hold(void) {
    uint32_t h = g_input_hold;

    /* robox.input.block(): a mod has taken control away from the player. Drop
     * the real buttons but keep going, so the injected mask below still
     * applies -- "block the player and drive him yourself" is one of the two
     * reasons to want this. */
    { extern int robox_lua_input_blocked(void); if (robox_lua_input_blocked()) h = 0; }

    /* Buttons a Lua mod is holding via robox.input.press(). Merged in HERE,
     * ahead of the rotation below, so injected input goes through the same
     * robot-section remap a real key does -- a mod that presses "left" means
     * the direction the player sees, not a raw d-pad bit. */
    { extern uint32_t robox_lua_input_mask(void); h |= robox_lua_input_mask(); }

    const uint32_t d = h & (uint32_t)WPAD_DPAD;
    if (!d) return h;

    /* Inside the robot the game turns the d-pad, so the stick reads sideways.
     * On hardware that is correct -- you physically rotate the Wiimote for
     * those sections -- but a keyboard does not turn, so it just feels broken.
     *
     * Which sections those are comes from the game's own current-level word
     * (see robox_level_is_robot): world 4 is the robot interiors. Read fresh
     * every frame, so it is right the instant you cross a door -- unlike the
     * flag at 0x8020876c, which reads 1 inside once anything updates and so
     * flipped the remap off mid-section. */
    const unsigned q = video_sideways_q();
    if (!q) return h;
    return (h & ~(uint32_t)WPAD_DPAD) | dpad_rotate(d, q);
}



/* Nunchuk stick (WASD) and shake gestures (E = Wiimote, Q = Nunchuk).
 * The stick is a unit-range Vec2 (+x right, +y up) fed to KPADStatus
 * ex_status.fs.stick; shake makes hle_KPADRead synthesize an oscillating
 * accelerometer so the game's shake detectors (|acc| spikes / acc_speed)
 * fire. Without these the "shake the Wiimote / Nunchuk" gates and all
 * stick-driven movement are unreachable. */
static volatile int g_key_w, g_key_a, g_key_s, g_key_d;
static volatile int g_shake_wm, g_shake_nk;

/* ---- Analog stick sources ------------------------------------------------
 * The nunchuk stick was pure digital WASD: strictly -1/0/+1 per axis, with
 * diagonals at magnitude sqrt(2) rather than clamped to the unit circle. That
 * left no path for a real analog stick, and on Android there is no keyboard at
 * all -- which matters because the game hard-requires a nunchuk (peripherals.c
 * forces dev_type = KPAD_DEV_TYPE_FS or the boot flow parks on a "You need a
 * Nunchuk" screen), so without a stick the game cannot clear its own gate.
 *
 * Three sources now feed the same unit-range vector. The strongest-magnitude
 * source wins rather than summing, so a resting gamepad stick cannot fight a
 * held key and two sources can never push past unit length.
 * ------------------------------------------------------------------------- */
static volatile float g_pad_stick_x,   g_pad_stick_y;    /* gamepad left stick */
static volatile float g_touch_stick_x, g_touch_stick_y;  /* on-screen thumbstick */

void video_input_stick(float *x, float *y) {
    /* robox.input.block(): the stick has to go too, or "controls disabled"
     * still leaves the player walking around on the nunchuk. */
    { extern int robox_lua_input_blocked(void);
      if (robox_lua_input_blocked()) { *x = 0.0f; *y = 0.0f; return; } }

    /* Keyboard: normalize the diagonal so it is not sqrt(2) long. */
    float kx = (float)(g_key_d - g_key_a);
    float ky = (float)(g_key_w - g_key_s);
    if (kx != 0.0f && ky != 0.0f) { kx *= 0.70710678f; ky *= 0.70710678f; }

    float px = g_pad_stick_x,   py = g_pad_stick_y;
    float tx = g_touch_stick_x, ty = g_touch_stick_y;

    float km = kx*kx + ky*ky, pm = px*px + py*py, tm = tx*tx + ty*ty;
    if (pm >= km && pm >= tm)      { *x = px; *y = py; }
    else if (tm >= km)             { *x = tx; *y = ty; }
    else                           { *x = kx; *y = ky; }
}

int video_input_shake(void) {
    return (g_shake_wm ? 1 : 0) | (g_shake_nk ? 2 : 0);
}

/* --- Co-op mod input (consumed by sdk/robox_coop.c) --------------------
 *
 * Directions are exported in a NEUTRAL up/down/left/right form rather than
 * as WPAD d-pad bits, because this game rotates the controller between
 * sections: the Wiimote is held sideways on foot and vertical inside the
 * robot, so the same d-pad bit means different world directions in each.
 * The mod applies that 90-degree rotation; raw key state has to stay
 * orientation-agnostic or the rotation cannot be undone later.
 *
 * IMPORTANT: none of this feeds hle_KPADRead. The KPAD emulation is left
 * exactly as it was so real controllers keep working through the game's
 * normal input path; the mod overrides at the action-mask level instead.
 *
 *   P1: WASD + arrows = move, Alt/Ctrl = tilt left/right
 *       (E = shake, 1/2 = buttons, mouse L/R = A/B all still go via KPAD)
 *   P2: IJKL = move, / = shake, , = button 1, . = button 2 */
#define DIR_UP    0x1u
#define DIR_DOWN  0x2u
#define DIR_LEFT  0x4u
#define DIR_RIGHT 0x8u

/* D-pad orientation. The game asks the player to physically re-grip the
 * Wiimote between sections, so the same d-pad bit means different world
 * directions depending on where you are -- and there are more contexts than
 * just on-foot/robot (piloting the little guy is a third).
 *
 * Rather than enumerate modes, expose a quarter-turn dial: 0 = follow the
 * game's section flag, 1..4 = force 0/90/180/270 degrees. F5 cycles, so any
 * orientation is reachable in at most four presses without a rebuild. */
static volatile int g_coop_rot_mode;   /* 0 = auto, 1..4 = forced turn+1 */
int video_coop_rot_mode(void) { return g_coop_rot_mode; }

static volatile uint32_t g_p1_dirs, g_p2_dirs;
static volatile int      g_p1_tilt_l, g_p1_tilt_r;
static volatile uint32_t g_p2_btn;      /* 0x100 = two, 0x200 = one */
static volatile int      g_p2_shake;

uint32_t video_input_p1_dirs(void)  { return g_p1_dirs; }
uint32_t video_input_p2_dirs(void)  { return g_p2_dirs; }
uint32_t video_input_p2_btn(void)   { return g_p2_btn; }
int      video_input_p2_shake(void) { return g_p2_shake ? 1 : 0; }
/* -1 = tilt left, +1 = tilt right, 0 = level. */
int      video_input_p1_tilt(void)  { return g_p1_tilt_l ? -1 : (g_p1_tilt_r ? 1 : 0); }

static uint32_t key_to_dir_p1(SDL_Keycode k) {
    switch (k) {
        case SDLK_w: case SDLK_UP:    return DIR_UP;
        case SDLK_s: case SDLK_DOWN:  return DIR_DOWN;
        case SDLK_a: case SDLK_LEFT:  return DIR_LEFT;
        case SDLK_d: case SDLK_RIGHT: return DIR_RIGHT;
        default:                      return 0;
    }
}

static uint32_t key_to_dir_p2(SDL_Keycode k) {
    switch (k) {
        case SDLK_i: return DIR_UP;
        case SDLK_k: return DIR_DOWN;
        case SDLK_j: return DIR_LEFT;
        case SDLK_l: return DIR_RIGHT;
        default:     return 0;
    }
}

static uint32_t key_to_btn_p2(SDL_Keycode k) {
    switch (k) {
        case SDLK_COMMA:  return 0x0200; /* button 1 */
        case SDLK_PERIOD: return 0x0100; /* button 2 */
        default:          return 0;
    }
}

/* Wii IR pointer from the host mouse: normalized to the window as
 * x,y in [-1,+1] with +y DOWN (the KPAD `pos` convention). `valid`
 * mirrors the sensor-bar "pointer on screen" state. */
static volatile float g_mouse_nx, g_mouse_ny;
static volatile int   g_mouse_valid;
void video_input_pointer(float *x, float *y, int *valid) {
    *x = g_mouse_nx; *y = g_mouse_ny; *valid = g_mouse_valid;
}

/* Map a window pixel to the IR pointer's [-1,+1] range THROUGH the renderer's
 * letterbox rect, not the raw window.
 *
 * The old mapping divided by the window size, but gx_ogl_present blits the
 * game into a centred sub-rectangle and paints the rest black. Whenever the
 * window aspect differs from the game's, the cursor under the player's finger
 * and the pointer the game receives were offset and scaled apart. That was
 * masked on desktop because the default window is already 16:9; it is
 * unavoidable on a phone.
 *
 * Landing in a black bar clears `valid`, which is also the correct semantics:
 * it is exactly "aiming off the sensor bar". */
static void pointer_from_window_px(float px, float py) {
    int rx, ry, rw, rh;
    gx_ogl_get_present_rect(&rx, &ry, &rw, &rh);

    if (rw <= 0 || rh <= 0) {           /* before the first present */
        int ww = 0, wh = 0;
        if (g_window) SDL_GetWindowSize(g_window, &ww, &wh);
        if (ww <= 0 || wh <= 0) return;
        rx = 0; ry = 0; rw = ww; rh = wh;
    }

    float nx = ((px - (float)rx) / (float)rw) * 2.0f - 1.0f;
    float ny = ((py - (float)ry) / (float)rh) * 2.0f - 1.0f;

    if (nx < -1.0f || nx > 1.0f || ny < -1.0f || ny > 1.0f) {
        g_mouse_valid = 0;              /* in a letterbox bar = off-sensor */
        return;
    }
    g_mouse_nx = nx;
    g_mouse_ny = ny;
    g_mouse_valid = 1;
}

/* Current frame rate, for the on-screen counter (F12).
 *
 * This used to rewrite the window title every second, which put a live number
 * in the title bar (and, on the web, in the browser tab). The title now stays
 * the plain application name; the frame rate is drawn in the corner by
 * gx_ogl_render_fps() when g_show_fps is on. */
void video_set_fps(double fps, double ms) {
    (void)ms;
    /* Guard against nan/inf (e.g. a zero-length sample window) so the counter
     * never shows "nan": keep the last good value instead. */
    if (!(fps == fps) || fps < 0.0 || fps > 100000.0) return;
    g_current_fps = fps;
}

/* ---- Gamepad -------------------------------------------------------------
 * There was no gamepad support of any kind: SDL_Init requested only
 * SDL_INIT_VIDEO and nothing in the tree referenced SDL_GameController. Every
 * button the game could see came from the keyboard or the two mouse buttons,
 * so on any device without a keyboard hle_KPADRead reported permanently-zero
 * hold/trig and the game sat on its first confirm screen forever.
 *
 * Mapping targets the WPAD bits documented above. The menu prompts for the
 * physical "1" and "2" buttons, which have no natural gamepad analogue, so
 * they go on X/Y where they are reachable without a legend.
 * ------------------------------------------------------------------------- */
#define PAD_AXIS_DEADZONE 8000    /* of 32767; ~24%, generous for worn sticks */

static float clampf(float v, float lo, float hi) {
    return (v < lo) ? lo : (v > hi ? hi : v);
}

static SDL_GameController *g_pad;      /* first opened controller; NULL = none */
static SDL_JoystickID      g_pad_id = -1;

static uint32_t pad_button_to_wpad(Uint8 b) {
    switch (b) {
        case SDL_CONTROLLER_BUTTON_A:             return 0x0800; /* A */
        case SDL_CONTROLLER_BUTTON_B:             return 0x0400; /* B */
        case SDL_CONTROLLER_BUTTON_Y:             return 0x0200; /* 1 */
        case SDL_CONTROLLER_BUTTON_X:             return 0x0100; /* 2 */
        case SDL_CONTROLLER_BUTTON_START:         return 0x0010; /* + */
        case SDL_CONTROLLER_BUTTON_BACK:          return 0x1000; /* - */
        case SDL_CONTROLLER_BUTTON_GUIDE:         return 0x8000; /* HOME */
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return 0x0001;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return 0x0002;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return 0x0004;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:       return 0x0008;
        default:                                  return 0;
    }
}

/* Neutral direction word for the co-op mod, which needs orientation-agnostic
 * directions rather than WPAD d-pad bits (see the co-op comment above). */
static uint32_t pad_button_to_dir(Uint8 b) {
    switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    return DIR_UP;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  return DIR_DOWN;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  return DIR_LEFT;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return DIR_RIGHT;
        default:                               return 0;
    }
}

static float pad_axis_norm(Sint16 v) {
    if (v > -PAD_AXIS_DEADZONE && v < PAD_AXIS_DEADZONE) return 0.0f;
    /* Rescale past the deadzone so the usable range still reaches +/-1. */
    float f = (float)v / 32767.0f;
    float dz = (float)PAD_AXIS_DEADZONE / 32767.0f;
    float s  = (f > 0.0f) ? (f - dz) : (f + dz);
    s /= (1.0f - dz);
    return (s > 1.0f) ? 1.0f : (s < -1.0f ? -1.0f : s);
}

static void pad_open_first(void) {
    if (g_pad) return;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i)) continue;
        g_pad = SDL_GameControllerOpen(i);
        if (g_pad) {
            g_pad_id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad));
            fprintf(stderr, "[input] gamepad connected: %s\n",
                    SDL_GameControllerName(g_pad));
            fflush(stderr);
            return;
        }
    }
}

static void pad_close(SDL_JoystickID id) {
    if (!g_pad || id != g_pad_id) return;
    fprintf(stderr, "[input] gamepad disconnected\n");
    fflush(stderr);
    SDL_GameControllerClose(g_pad);
    g_pad = NULL;
    g_pad_id = -1;
    g_pad_stick_x = g_pad_stick_y = 0.0f;   /* never leave the stick deflected */
}

/* ---- Touch ---------------------------------------------------------------
 * Android has no keyboard, so without this the game cannot reach its own menu:
 * it needs an IR pointer to aim and a "2" press to confirm, and in play it needs
 * the nunchuk stick and the shake gesture.
 *
 * SDL synthesizes mouse events from touch by default, which is actively wrong
 * here: a tap would set the IR pointer AND press A at the same instant, so the
 * pointer could never move without confirming, and SDL_WINDOWEVENT_LEAVE (the
 * only thing that clears g_mouse_valid) is never generated by touch, so
 * dpd_valid_fg would latch on forever after the first tap. SDL_HINT_TOUCH_MOUSE_EVENTS
 * is turned off in video_init and SDL_FINGER* is handled explicitly instead.
 *
 * Layout is expressed as fractions of the window so it survives any screen
 * shape, and lives in one table so the on-screen overlay renderer can draw
 * exactly what the hit-test uses -- one source of truth, no drift.
 * ------------------------------------------------------------------------- */
#define TOUCH_MAX_FINGERS 8

typedef enum {
    TROLE_NONE = 0,
    TROLE_STICK,      /* floating thumbstick */
    TROLE_POINTER,    /* IR aiming */
    TROLE_BUTTON      /* one of k_touch_buttons */
} TouchRole;

typedef struct {
    uint32_t    wpad;    /* WPAD bit to hold, or 0 when `shake` is set */
    int         shake;   /* 1 = trigger the Wiimote shake gesture instead */
    float       cx, cy;  /* centre as a fraction of window width / height */
    float       r;       /* radius as a fraction of min(width, height) */
    const char *label;
} TouchButton;

/* A is largest and lowest-right (most-used, under the thumb); "2" is the menu
 * confirm the game prompts for, so it sits where it is easy to hit blind. */
static const TouchButton k_touch_buttons[] = {
    { 0x0800, 0, 0.880f, 0.780f, 0.085f, "A"     },
    { 0x0400, 0, 0.745f, 0.870f, 0.065f, "B"     },
    { 0x0100, 0, 0.745f, 0.640f, 0.065f, "2"     },
    { 0x0200, 0, 0.880f, 0.545f, 0.065f, "1"     },
    { 0,      1, 0.600f, 0.830f, 0.070f, "SHAKE" },
};
#define TOUCH_NUM_BUTTONS ((int)(sizeof k_touch_buttons / sizeof k_touch_buttons[0]))

/* Thumbstick lives in the bottom-left quadrant and FLOATS: it centres wherever
 * the finger lands rather than at a fixed spot, so the player never has to look
 * down to find it. Full deflection at this fraction of min(w,h) from centre. */
#define TOUCH_STICK_ZONE_X   0.40f
#define TOUCH_STICK_ZONE_Y   0.45f
#define TOUCH_STICK_RADIUS   0.13f

typedef struct {
    SDL_FingerID id;
    int          active;
    TouchRole    role;
    int          button;      /* index into k_touch_buttons when role==BUTTON */
    float        ox, oy;      /* thumbstick origin, window px */
} TouchFinger;

static TouchFinger g_fingers[TOUCH_MAX_FINGERS];
static int         g_touch_seen;    /* 1 once any finger has arrived */

/* Should the on-screen controls be drawn?
 *
 * On Android touch is the PRIMARY input, so they must be visible from the first
 * frame -- gating them on "a touch has happened" was backwards: the player
 * would have to tap blind before the buttons appeared. They are hidden only
 * when a gamepad is attached, which supersedes them.
 *
 * On desktop it stays opt-in: nothing is drawn until a real touch arrives, so a
 * mouse-and-keyboard session is never cluttered. */
int video_touch_active(void) {
    if (g_pad) return 0;              /* a real controller wins */
#if defined(__ANDROID__)
    return 1;
#else
    return g_touch_seen;
#endif
}

/* Snapshot the controls for the renderer. Reads the same k_touch_buttons table
 * and the same live state the hit-test uses, so the drawn overlay and the
 * touchable regions cannot disagree. */
int video_touch_overlay(RoboxTouchCircle *out, int max) {
    if (!out || max <= 0 || !video_touch_active()) return 0;
    int n = 0;

    for (int i = 0; i < TOUCH_NUM_BUTTONS && n < max; ++i) {
        const TouchButton *b = &k_touch_buttons[i];
        out[n].cx      = b->cx;
        out[n].cy      = b->cy;
        out[n].r       = b->r;
        out[n].label   = b->label;
        out[n].pressed = b->shake ? (g_shake_wm != 0)
                                  : ((g_input_hold & b->wpad) != 0);
        ++n;
    }

    /* Thumbstick: only while a finger owns it, since it floats to wherever the
     * finger landed rather than living at a fixed spot. */
    int ww = 0, wh = 0;
    if (g_window) SDL_GetWindowSize(g_window, &ww, &wh);
    if (ww > 0 && wh > 0) {
        for (int i = 0; i < TOUCH_MAX_FINGERS && n + 1 < max; ++i) {
            if (!g_fingers[i].active || g_fingers[i].role != TROLE_STICK) continue;

            float ox = g_fingers[i].ox / (float)ww;      /* origin, fractions */
            float oy = g_fingers[i].oy / (float)wh;

            out[n].cx = ox; out[n].cy = oy; out[n].r = TOUCH_STICK_RADIUS;
            out[n].pressed = 0; out[n].label = NULL;     /* base ring */
            ++n;

            /* Knob: offset by the current deflection. g_touch_stick_y is +up
             * (KPAD convention) but the screen is +down, so negate it back. */
            float mn = (float)((ww < wh) ? ww : wh);
            out[n].cx = ox + (g_touch_stick_x * TOUCH_STICK_RADIUS * mn) / (float)ww;
            out[n].cy = oy - (g_touch_stick_y * TOUCH_STICK_RADIUS * mn) / (float)wh;
            out[n].r  = TOUCH_STICK_RADIUS * 0.45f;
            out[n].pressed = 1; out[n].label = NULL;     /* filled knob */
            ++n;
            break;
        }
    }
    return n;
}

static TouchFinger *finger_slot(SDL_FingerID id, int create) {
    for (int i = 0; i < TOUCH_MAX_FINGERS; ++i)
        if (g_fingers[i].active && g_fingers[i].id == id) return &g_fingers[i];
    if (!create) return NULL;
    for (int i = 0; i < TOUCH_MAX_FINGERS; ++i)
        if (!g_fingers[i].active) {
            g_fingers[i].active = 1;
            g_fingers[i].id     = id;
            g_fingers[i].role   = TROLE_NONE;
            g_fingers[i].button = -1;
            return &g_fingers[i];
        }
    return NULL;
}

/* Which control does this window-pixel land on? */
static TouchRole touch_hit_test(float px, float py, int ww, int wh, int *out_btn) {
    float mn = (float)((ww < wh) ? ww : wh);
    for (int i = 0; i < TOUCH_NUM_BUTTONS; ++i) {
        float bx = k_touch_buttons[i].cx * (float)ww;
        float by = k_touch_buttons[i].cy * (float)wh;
        float br = k_touch_buttons[i].r  * mn;
        float dx = px - bx, dy = py - by;
        if (dx*dx + dy*dy <= br*br) { *out_btn = i; return TROLE_BUTTON; }
    }
    if (px < TOUCH_STICK_ZONE_X * (float)ww &&
        py > TOUCH_STICK_ZONE_Y * (float)wh) return TROLE_STICK;
    return TROLE_POINTER;
}

static void touch_apply_button(int idx, int down) {
    if (idx < 0 || idx >= TOUCH_NUM_BUTTONS) return;
    const TouchButton *b = &k_touch_buttons[idx];
    if (b->shake) { g_shake_wm = down; return; }
    if (down) g_input_hold |=  b->wpad;
    else      g_input_hold &= ~b->wpad;
}

static void touch_update_stick(TouchFinger *f, float px, float py, int ww, int wh) {
    float mn  = (float)((ww < wh) ? ww : wh);
    float rad = TOUCH_STICK_RADIUS * mn;
    float dx  = (px - f->ox) / rad;
    float dy  = (py - f->oy) / rad;
    float m   = dx*dx + dy*dy;
    if (m > 1.0f) { float inv = 1.0f / SDL_sqrtf(m); dx *= inv; dy *= inv; }
    g_touch_stick_x =  dx;
    g_touch_stick_y = -dy;    /* KPAD stick is +y UP, screen is +y DOWN */
}

static void touch_event(const SDL_TouchFingerEvent *t, int type) {
    int ww = 0, wh = 0;
    if (g_window) SDL_GetWindowSize(g_window, &ww, &wh);
    if (ww <= 0 || wh <= 0) return;

    /* tfinger x/y are normalized 0..1 over the window. */
    float px = t->x * (float)ww;
    float py = t->y * (float)wh;

    if (type == SDL_FINGERDOWN) {
        g_touch_seen = 1;
        TouchFinger *f = finger_slot(t->fingerId, 1);
        if (!f) return;
        int btn = -1;
        f->role   = touch_hit_test(px, py, ww, wh, &btn);
        f->button = btn;
        switch (f->role) {
            case TROLE_BUTTON:  touch_apply_button(btn, 1); break;
            case TROLE_STICK:   f->ox = px; f->oy = py;
                                g_touch_stick_x = g_touch_stick_y = 0.0f; break;
            case TROLE_POINTER: pointer_from_window_px(px, py); break;
            default: break;
        }
    } else if (type == SDL_FINGERMOTION) {
        TouchFinger *f = finger_slot(t->fingerId, 0);
        if (!f) return;
        if (f->role == TROLE_STICK)        touch_update_stick(f, px, py, ww, wh);
        else if (f->role == TROLE_POINTER) pointer_from_window_px(px, py);
        /* A finger that slides off a button keeps holding it: chasing a moving
         * finger between buttons produces accidental presses in play. */
    } else { /* SDL_FINGERUP */
        TouchFinger *f = finger_slot(t->fingerId, 0);
        if (!f) return;
        if (f->role == TROLE_BUTTON)      touch_apply_button(f->button, 0);
        else if (f->role == TROLE_STICK)  g_touch_stick_x = g_touch_stick_y = 0.0f;
        else if (f->role == TROLE_POINTER) g_mouse_valid = 0;  /* lifted = off-sensor */
        f->active = 0;
        f->role   = TROLE_NONE;
    }
}

/* ---------------------------------------------------------------------------
 * Remappable controls (controls.cfg, "BUTTON = key" per line).
 *
 * Entries ADD a key rather than replacing the built-in table, so a missing or
 * partial file still leaves every button working -- the config can only ever
 * extend what already functions, never strand the player without an A button.
 *
 * Key names go through SDL_GetKeyFromName, so anything SDL can name works
 * ("Return", "Space", "Left Shift", "F5", "a") without a lookup table here.
 * ------------------------------------------------------------------------- */
#define CTRL_MAX_BINDS 64
static struct { SDL_Keycode key; uint32_t bit; } g_binds[CTRL_MAX_BINDS];
static int g_bind_count;

static uint32_t control_name_to_bit(const char *s) {
    if (!SDL_strcasecmp(s, "A"))      return 0x0800;
    if (!SDL_strcasecmp(s, "B"))      return 0x0400;
    if (!SDL_strcasecmp(s, "1"))      return 0x0200;
    if (!SDL_strcasecmp(s, "2"))      return 0x0100;
    if (!SDL_strcasecmp(s, "PLUS"))   return 0x0010;
    if (!SDL_strcasecmp(s, "MINUS"))  return 0x1000;
    if (!SDL_strcasecmp(s, "HOME"))   return 0x8000;
    if (!SDL_strcasecmp(s, "DUP"))    return 0x0008;
    if (!SDL_strcasecmp(s, "DDOWN"))  return 0x0004;
    if (!SDL_strcasecmp(s, "DLEFT"))  return 0x0001;
    if (!SDL_strcasecmp(s, "DRIGHT")) return 0x0002;
    if (!SDL_strcasecmp(s, "TILTL"))  return VB_TILT_L;
    if (!SDL_strcasecmp(s, "TILTR"))  return VB_TILT_R;
    if (!SDL_strcasecmp(s, "SHAKE"))  return VB_SHAKE;
    return 0;
}

/* Tilt and shake are plain host flags, not mask bits, so a virtual binding has
 * to drive them by hand on both edges. */
static void apply_virtual(uint32_t bits, int down) {
    if (bits & VB_TILT_L) g_p1_tilt_l = down;
    if (bits & VB_TILT_R) g_p1_tilt_r = down;
    if (bits & VB_SHAKE)  g_shake_wm  = down;
}

static char *ctrl_trim(char *s) {
    while (*s == ' ' || *s == '\t') ++s;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) --e;
    *e = 0;
    return s;
}

/* Where a rebind from the in-game menu is written.
 *
 * Under nand/ on purpose: that directory is a real folder next to the
 * executable on desktop AND an IndexedDB-backed mount on web (see
 * tools/web_shell.html), so one path persists on every target. The shipped
 * controls.cfg sits in the read-only preload bundle on web and cannot be
 * written at all. */
#define CTRL_USER_PATH "nand/controls.cfg"

static int controls_load_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    int n = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#'); if (hash) *hash = 0;
        char *eq = strchr(line, '=');   if (!eq)  continue;
        *eq = 0;
        char *name = ctrl_trim(line), *keyname = ctrl_trim(eq + 1);
        if (!*name || !*keyname) continue;

        uint32_t bit = control_name_to_bit(name);
        SDL_Keycode k = SDL_GetKeyFromName(keyname);
        if (!bit)                { fprintf(stderr, "[controls] unknown button '%s'\n", name);   ++bad; continue; }
        if (k == SDLK_UNKNOWN)   { fprintf(stderr, "[controls] unknown key '%s'\n", keyname);    ++bad; continue; }
        if (g_bind_count >= CTRL_MAX_BINDS) { fprintf(stderr, "[controls] too many bindings\n"); break; }
        g_binds[g_bind_count].key = k;
        g_binds[g_bind_count].bit = bit;
        ++g_bind_count; ++n;
    }
    fclose(f);
    fprintf(stderr, "[controls] loaded %d binding%s from %s%s\n",
            n, n == 1 ? "" : "s", path, bad ? " (some lines ignored)" : "");
    fflush(stderr);
    return 1;
}

void controls_load(void) {
    /* The player's own config wins outright rather than stacking on top of the
     * shipped one: key_to_wpad() returns the FIRST entry matching a key, so
     * merging the two would let a stale default shadow a fresh rebind. */
    g_bind_count = 0;
    if (controls_load_file(CTRL_USER_PATH)) return;
    if (controls_load_file("controls.cfg"))  return;
    fprintf(stderr, "[controls] no config -- using built-in defaults\n");
}

/* --- accessors for the in-game menu (sdk/robox_menu.c) ------------------- */

SDL_Keycode controls_get_binding(unsigned int bit) {
    for (int i = 0; i < g_bind_count; ++i)
        if (g_binds[i].bit == bit) return g_binds[i].key;
    return SDLK_UNKNOWN;
}

void controls_set_binding(unsigned int bit, SDL_Keycode k) {
    /* A key can only mean one thing. Drop every OTHER action already using it
     * first -- key_to_wpad() matches by key and returns the first hit, so a
     * leftover entry silently wins and the rebind looks like it did nothing.
     * The shipped config makes this the common case, not a corner one: it
     * binds A to Return, Space AND Z, so binding jump onto Z left Z still
     * answering "A" from the earlier entry. */
    for (int i = 0; i < g_bind_count; ) {
        if (g_binds[i].key == k && g_binds[i].bit != bit)
            g_binds[i] = g_binds[--g_bind_count];   /* order does not matter */
        else
            ++i;
    }
    for (int i = 0; i < g_bind_count; ++i)
        if (g_binds[i].bit == bit) { g_binds[i].key = k; return; }
    if (g_bind_count < CTRL_MAX_BINDS) {
        g_binds[g_bind_count].key = k;
        g_binds[g_bind_count].bit = bit;
        ++g_bind_count;
    }
}

void controls_reset_defaults(void) {
    /* Emptying the table is enough: key_to_wpad() falls through to its built-in
     * switch for anything not bound, so "no config" IS the default layout. */
    g_bind_count = 0;
    remove(CTRL_USER_PATH);
}

/* Names must round-trip through control_name_to_bit(). */
static const char *control_bit_to_name(unsigned int bit) {
    switch (bit) {
        case 0x0800: return "A";      case 0x0400: return "B";
        case 0x0200: return "1";      case 0x0100: return "2";
        case 0x0010: return "PLUS";   case 0x1000: return "MINUS";
        case 0x8000: return "HOME";   case 0x0008: return "DUP";
        case 0x0004: return "DDOWN";  case 0x0001: return "DLEFT";
        case 0x0002: return "DRIGHT";
        case VB_TILT_L: return "TILTL";  case VB_TILT_R: return "TILTR";
        case VB_SHAKE:  return "SHAKE";
        default:     return NULL;
    }
}

int controls_save(void) {
    /* nand/ may not exist yet -- the guest creates it lazily on its first save,
     * and the player can reach this menu before that ever happens. */
    extern void robox_nand_prepare(void);
    extern void robox_nand_publish(void);
    robox_nand_prepare();

    FILE *f = fopen(CTRL_USER_PATH, "w");
    if (!f) { fprintf(stderr, "[controls] cannot write %s\n", CTRL_USER_PATH); return 0; }
    fprintf(f, "# Written by the in-game controls menu.\n");
    for (int i = 0; i < g_bind_count; ++i) {
        const char *name = control_bit_to_name(g_binds[i].bit);
        const char *key  = SDL_GetKeyName(g_binds[i].key);
        if (name && key && *key) fprintf(f, "%-6s = %s\n", name, key);
    }
    fclose(f);

    /* On web the bytes are only in the in-memory filesystem until the mount is
     * synced; without this the rebind is lost on reload exactly like saves were.
     * No-op off the web build. */
    robox_nand_publish();
    fprintf(stderr, "[controls] saved %d binding(s) to %s\n", g_bind_count, CTRL_USER_PATH);
    return 1;
}

static uint32_t key_to_wpad_builtin(SDL_Keycode k);

static uint32_t key_to_wpad(SDL_Keycode k) {
    for (int i = 0; i < g_bind_count; ++i)
        if (g_binds[i].key == k) return g_binds[i].bit;

    /* Fall back to the built-in layout -- but only for buttons the player has
     * NOT rebound. Without this, rebinding jump onto K leaves 2 jumping as
     * well, because the default table never stopped claiming it, and the menu
     * looks like it did nothing. */
    uint32_t def = key_to_wpad_builtin(k);
    if (!def) return 0;
    for (int i = 0; i < g_bind_count; ++i)
        if (g_binds[i].bit == def) return 0;   /* superseded by the player */
    return def;
}

static uint32_t key_to_wpad_builtin(SDL_Keycode k) {
    switch (k) {
        case SDLK_RETURN: case SDLK_SPACE: case SDLK_z: return 0x0800; /* A */
        case SDLK_x:      case SDLK_b:                   return 0x0400; /* B */
        case SDLK_UP:                                    return 0x0008;
        case SDLK_DOWN:                                  return 0x0004;
        case SDLK_LEFT:                                  return 0x0001;
        case SDLK_RIGHT:                                 return 0x0002;
        case SDLK_1:                                     return 0x0200; /* 1 */
        case SDLK_2:                                     return 0x0100; /* 2 */
        case SDLK_EQUALS: case SDLK_p:                   return 0x0010; /* + */
        case SDLK_MINUS:                                 return 0x1000; /* - */
        case SDLK_HOME:   case SDLK_h:                   return 0x8000; /* HOME */
        default: return 0;
    }
}

void video_init(void) {
    if (g_video_inited) return;
    g_video_inited = 1;

    /* Stop SDL turning touches into mouse events. Left on, a single tap would
     * set the IR pointer AND press A simultaneously, and nothing would ever
     * clear g_mouse_valid (touch generates no SDL_WINDOWEVENT_LEAVE), so the
     * game's dpd_valid_fg would latch on permanently after the first tap.
     * Must be set before SDL_Init to take effect. */
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

    /* Force landscape. The manifest's android:screenOrientation is not enough:
     * SDLActivity calls setRequestedOrientation() at runtime from the native
     * side and overrides it, which left the phone rendering a 16:9 game into a
     * letterboxed band in the middle of a portrait screen. This hint is what
     * SDL consults when it makes that call. */
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");

    /* Tell Windows we handle our own scaling.
     *
     * Without this the process is DPI-virtualised: on a 1920x1080 panel at
     * 125% scaling the whole desktop reports 1536x864, so the game renders
     * into a 1536x864 buffer and Windows stretches it up to the real 1920x1080
     * panel. Every pixel gets resampled by the compositor, and the result
     * looks soft and blocky no matter how high the internal resolution is set
     * -- the detail is thrown away at the very last step.
     *
     * Deliberately WITHOUT SDL_WINDOW_ALLOW_HIGHDPI. That flag makes
     * SDL_GetWindowSize report logical points while the drawable stays in
     * pixels, and ten places in gx_ogl.c and this file size their viewport and
     * present rect from SDL_GetWindowSize. Declaring awareness alone removes
     * the virtualisation, so window size and drawable size stay identical and
     * every one of those sites keeps working -- they just get true pixels now.
     *
     * Must precede SDL_Init: SDL sets the process awareness during video init
     * and Windows only lets it be set once. */
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");

    /* SDL_INIT_GAMECONTROLLER is required separately from SDL_INIT_VIDEO --
     * without it no controller events are ever delivered. It is non-fatal:
     * a machine with no controller subsystem should still boot to keyboard. */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[video] SDL_Init failed: %s\n", SDL_GetError());
        ROBOX_DIAG_FATAL("SDL could not start its video system.", SDL_GetError());
        return;
    }
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[input] gamepad subsystem unavailable: %s\n", SDL_GetError());
    } else {
        int njoy = SDL_NumJoysticks(), npad = 0;
        for (int i = 0; i < njoy; ++i) if (SDL_IsGameController(i)) ++npad;
        fprintf(stderr, "[input] gamepad subsystem up: %d joystick(s), %d controller(s)\n",
                njoy, npad);
        fflush(stderr);
        pad_open_first();
    }

#ifndef RECOMP_PROJECT_TITLE
#  define RECOMP_PROJECT_TITLE "PPC recomp"
#endif
    /* GL 3.3 core on desktop, GLES 3.0 on Android/handhelds. The renderer's
     * shaders are written to the common subset of the two (see GLSL_PROLOGUE in
     * gx_ogl.c), so this attribute block is the only place the target differs.
     *
     * ROBOX_GLES can also be defined on a desktop build to exercise the ES path
     * against a real driver (or ANGLE) before any device is involved -- that is
     * how the ES renderer gets validated without an Android device in the loop. */
#if defined(ROBOX_GLES)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    int win_w = WIN_WIDTH, win_h = WIN_HEIGHT;
    { const char *e;
      if ((e = getenv("RECOMP_WIN_W")) && atoi(e) > 0) win_w = atoi(e);
      if ((e = getenv("RECOMP_WIN_H")) && atoi(e) > 0) win_h = atoi(e); }

    /* Fullscreen on launch. This is a console game; a 720p window in the
     * corner of someone's desktop is not how it is meant to be played.
     *
     * FULLSCREEN_DESKTOP rather than FULLSCREEN: it borderless-fills the mode
     * already set instead of forcing a video mode change, so alt-tab is clean
     * and there is no resolution switch (and no black flicker) on every
     * launch. win_w/win_h still matter -- SDL keeps them as the size to
     * restore to when fullscreen is toggled off.
     *
     * RECOMP_WINDOWED=1 opts out, which is what you want when debugging with a
     * log window visible. Never applied on the web, where going fullscreen
     * requires a user gesture that does not exist at init. */
    Uint32 win_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
    int want_fullscreen = 1;
    /* A remembered F11 choice beats the default; RECOMP_WINDOWED beats both,
     * so there is always a way in from outside if a saved setting ever leaves
     * the window somewhere unusable. */
    video_cfg_load();
    if (s_cfg_fullscreen >= 0) want_fullscreen = s_cfg_fullscreen;
    { const char *e = getenv("RECOMP_WINDOWED");
      if (e && e[0] && e[0] != '0') want_fullscreen = 0; }
#if defined(__EMSCRIPTEN__)
    want_fullscreen = 0;
#endif
    if (want_fullscreen) win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    controls_load();   /* before any input is read */
    fprintf(stderr, "[video] creating window %dx%d%s...\n", win_w, win_h,
            want_fullscreen ? " (fullscreen)" : ""); fflush(stderr);
    g_window = SDL_CreateWindow(
        RECOMP_PROJECT_TITLE,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h,
        win_flags);
    SDL_RaiseWindow(g_window);
    {   /* What we actually got, in real pixels. Worth logging: if DPI
         * awareness ever fails to take, this is the line that shows it --
         * a drawable smaller than the panel means the compositor is
         * upscaling and the picture will look soft however it is configured. */
        int dw = 0, dh = 0;
        SDL_GL_GetDrawableSize(g_window, &dw, &dh);
        SDL_DisplayMode dm;
        if (SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(g_window), &dm) == 0)
            fprintf(stderr, "[video] drawable %dx%d on a %dx%d display\n",
                    dw, dh, dm.w, dm.h);
        fflush(stderr);
    }
    /* The game draws its own Wii pointer, so the host arrow on top of it is
     * just a second cursor that does not match where you are aiming. On web
     * the shell also sets `cursor:none` on the canvas: SDL's Emscripten
     * backend routes this through pointer-lock rather than the CSS property,
     * and pointer-lock needs a user gesture we do not have at init. */
    SDL_ShowCursor(SDL_DISABLE);
    if (!g_window) {
        fprintf(stderr, "[video] SDL_CreateWindow failed: %s\n", SDL_GetError());
        ROBOX_DIAG_FATAL("Could not create an OpenGL 3.3 window. The graphics driver is most likely too old, or not installed.", SDL_GetError());
        return;
    }

    /* Try to create a GL context first; if it works, init the OGL renderer. */
    fprintf(stderr, "[video] window ok, creating GL context...\n"); fflush(stderr);
#if defined(__EMSCRIPTEN__)
    /* SDL2's Emscripten video backend cannot create a WebGL context off the
     * main thread, and PROXY_TO_PTHREAD puts main() -- and therefore the guest,
     * which issues every GL call -- on a worker. SDL_GL_CreateContext simply
     * never returns there. Create the context directly instead.
     *
     * explicitSwapControl + renderViaOffscreenBackBuffer is the combination that
     * makes a WebGL context usable from a worker: rendering goes to an offscreen
     * back buffer and is published by emscripten_webgl_commit_frame() rather
     * than by the browser's implicit end-of-task present. */
    SDL_GLContext gl_ctx = NULL;
    {
        EmscriptenWebGLContextAttributes attrs;
        emscripten_webgl_init_context_attributes(&attrs);
        attrs.majorVersion = 2;            /* WebGL2 == GLES 3.0 */
        attrs.minorVersion = 0;
        attrs.depth        = EM_TRUE;
        attrs.alpha        = EM_FALSE;
        attrs.antialias    = EM_FALSE;
        attrs.explicitSwapControl        = EM_TRUE;
        attrs.renderViaOffscreenBackBuffer = EM_TRUE;
        /* ALWAYS, not FALLBACK. With FALLBACK the worker got a context that
         * rendered correctly (pixel readback proved it) but was not connected to
         * the page's canvas, so the page stayed black no matter what we drew.
         * Forcing the proxy routes every GL call to the main thread, where the
         * real canvas lives. Slower, but it is the only path that actually
         * displays. */
        attrs.proxyContextToMainThread   = EMSCRIPTEN_WEBGL_CONTEXT_PROXY_ALWAYS;
        /* Size the actual <canvas> element. It defaults to 300x150, and the
         * renderer blits the EFB using SDL's window size (1280x720), so without
         * this the frame is presented outside the visible canvas and the page
         * just shows black while the game runs happily at full speed. */
        emscripten_set_canvas_element_size("#canvas", win_w, win_h);
        EMSCRIPTEN_WEBGL_CONTEXT_HANDLE c = emscripten_webgl_create_context("#canvas", &attrs);
        if (c > 0 && emscripten_webgl_make_context_current(c) == EMSCRIPTEN_RESULT_SUCCESS) {
            g_em_gl_ctx = c;
            gl_ctx = (SDL_GLContext)(intptr_t)c;   /* non-NULL sentinel */
        }
        fprintf(stderr, "[video] emscripten WebGL2 context handle=%d\n", (int)c);
        fflush(stderr);
    }
#else
    SDL_GLContext gl_ctx = SDL_GL_CreateContext(g_window);
#endif
    fprintf(stderr, "[video] GL context = %p (%s)\n", (void*)gl_ctx,
            gl_ctx ? "ok" : SDL_GetError()); fflush(stderr);
    if (gl_ctx) {
        /* Who is actually driving. Logged the moment a context exists and
         * before any shader is compiled, because the interesting failures
         * happen during shader compilation -- and by then the "renderer ready"
         * line that normally carries this has already been skipped. Vendor is
         * the first thing worth knowing when a build runs on one machine and
         * does nothing on another.
         *
         * Typed by hand rather than via the GL headers, which this file does
         * not include. GL uses __stdcall on Windows; getting that wrong here
         * would crash rather than misprint. */
#if defined(_WIN32)
#  define ROBOX_GLCALL __stdcall
#else
#  define ROBOX_GLCALL
#endif
        typedef const unsigned char *(ROBOX_GLCALL *robox_glGetString_fn)(unsigned int);
        robox_glGetString_fn gl_get_string =
            (robox_glGetString_fn)SDL_GL_GetProcAddress("glGetString");
        if (gl_get_string) {
            static const struct { unsigned int e; const char *label; } q[] = {
                { 0x1F00, "vendor  " }, { 0x1F01, "renderer" },
                { 0x1F02, "version " }, { 0x8B8C, "glsl    " },
            };
            for (unsigned i = 0; i < sizeof q / sizeof q[0]; ++i) {
                const unsigned char *v = gl_get_string(q[i].e);
                fprintf(stderr, "[video] GL %s: %s\n", q[i].label,
                        v ? (const char *)v : "(null)");
            }
            fflush(stderr);
        }
        video_vsync_init();
        gx_ogl_init();
        if (gx_ogl_ready()) {
            fprintf(stderr, "[video] OpenGL renderer active — bypassing SDL_Renderer\n");
            fflush(stderr);
            return;
        }
        /* GL init failed — fall through to SDL_Renderer path */
        SDL_GL_DeleteContext(gl_ctx);
    } else {
        fprintf(stderr, "[video] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
#ifdef ROBOX_DIAG
        /* 3.3 core was refused. The useful question is not "why" but "then
         * what CAN this driver do" -- and the only way to find out is to ask
         * for nothing in particular and read back what arrives.
         *
         * A fresh window is needed rather than reusing g_window: on Windows a
         * window's pixel format is set once and cannot be renegotiated, so a
         * second CreateContext against the same window would fail for reasons
         * that have nothing to do with the driver's real capability.
         *
         * This is what separates the two answers that look identical from the
         * outside. "GL 1.1 / GDI Generic" means no graphics driver is
         * installed at all and Windows is falling back to its software
         * rasteriser. "GL 3.1 / Intel HD Graphics 3000" means a real driver
         * that genuinely tops out below what the game needs. */
        {
            char cap[512];
            snprintf(cap, sizeof cap, "%s", "could not be determined");
            SDL_GL_ResetAttributes();
            SDL_Window *probe = SDL_CreateWindow(
                "probe", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
            if (probe) {
                SDL_GLContext pctx = SDL_GL_CreateContext(probe);
                if (pctx) {
                    typedef const unsigned char *(ROBOX_GLCALL *gs_fn)(unsigned int);
                    gs_fn gs = (gs_fn)SDL_GL_GetProcAddress("glGetString");
                    if (gs) {
                        const unsigned char *ven = gs(0x1F00);
                        const unsigned char *ren = gs(0x1F01);
                        const unsigned char *ver = gs(0x1F02);
                        snprintf(cap, sizeof cap, "%s / %s / OpenGL %s",
                                 ven ? (const char *)ven : "?",
                                 ren ? (const char *)ren : "?",
                                 ver ? (const char *)ver : "?");
                    }
                    SDL_GL_DeleteContext(pctx);
                } else {
                    snprintf(cap, sizeof cap,
                             "no OpenGL context of any version (%s)", SDL_GetError());
                }
                SDL_DestroyWindow(probe);
            }
            fprintf(stderr, "[video] driver actually offers: %s\n", cap);
            fflush(stderr);
            {
                char detail[768];
                snprintf(detail, sizeof detail,
                         "%s\n\nThis machine offers:\n%s", SDL_GetError(), cap);
                ROBOX_DIAG_FATAL("This machine cannot provide OpenGL 3.3, which the game requires.", detail);
            }
        }
#else
        ROBOX_DIAG_FATAL("Could not create an OpenGL 3.3 context. The graphics driver is most likely too old, or not installed.", SDL_GetError());
#endif
    }

    // Default to SOFTWARE renderer -- the direct3d backend on Windows
    // accepts UpdateTexture + RenderPresent (readback confirms the
    // correct pixels landed on the render target) but the visible
    // window stays black. Software renderer bypasses that bug.
    // Override with RECOMP_SDL_HW=1 to try the GPU path.
    const char *hw_env = getenv("RECOMP_SDL_HW");
    int want_hw = (hw_env && hw_env[0] == '1');
    uint32_t flags = want_hw ? (SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC)
                             : SDL_RENDERER_SOFTWARE;
    g_renderer = SDL_CreateRenderer(g_window, -1, flags);
    if (!g_renderer) {
        g_renderer = SDL_CreateRenderer(g_window, -1, 0);
    }
    fprintf(stderr, "[video] renderer: %s\n", want_hw ? "HW" : "SW");
    if (!g_renderer) {
        fprintf(stderr, "[video] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return;
    }

    g_tex = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING,
                              EFB_WIDTH, EFB_HEIGHT);
    int ww, wh, rw, rh;
    SDL_GetWindowSize(g_window, &ww, &wh);
    SDL_GetRendererOutputSize(g_renderer, &rw, &rh);
    fprintf(stderr, "[video] window=%dx%d renderer_output=%dx%d tex=%dx%d\n",
            ww, wh, rw, rh, EFB_WIDTH, EFB_HEIGHT);
    SDL_RendererInfo ri;
    if (SDL_GetRendererInfo(g_renderer, &ri) == 0) {
        fprintf(stderr, "[video] renderer_name='%s' flags=0x%x\n",
                ri.name, ri.flags);
    }
    if (!g_tex) {
        fprintf(stderr, "[video] SDL_CreateTexture failed: %s\n", SDL_GetError());
        return;
    }

    g_efb_pixels = (uint32_t *)calloc(EFB_WIDTH * EFB_HEIGHT, sizeof(uint32_t));

    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    SDL_RenderPresent(g_renderer);

    fprintf(stderr, "[video] SDL2 framebuffer ready (%dx%d)\n", EFB_WIDTH, EFB_HEIGHT);
    fflush(stderr);
}


/* Drop every held input.
 *
 * The button/direction words are accumulators -- a bit goes in on KEYDOWN and
 * only ever comes out on the matching KEYUP -- so any lost release latches
 * that input on for the rest of the session. This is the escape hatch for the
 * cases where the release provably never arrives (focus loss). It must clear
 * everything the KEYUP path clears, or the leftovers latch instead. */
/* sdk/robox_menu.c */
extern int  robox_menu_is_open(void);
extern void robox_menu_toggle(void);
extern int  robox_menu_key(SDL_Keycode k);

static void input_release_all(void) {
    g_input_hold = 0;
    g_p1_dirs = g_p2_dirs = 0;
    g_p2_btn = 0;
    g_key_w = g_key_a = g_key_s = g_key_d = 0;
    g_shake_wm = g_shake_nk = g_p2_shake = 0;
    g_p1_tilt_l = g_p1_tilt_r = 0;
}

// Pump SDL events so the window stays responsive, then upload the CPU
// framebuffer and present. Cheap to call every retrace.
void video_present(void) {
    if (!g_video_inited) { video_init(); }

    // Discord Rich Presence. Rides the frame pump because it needs a periodic
    // host-side tick and this is the only one the port has -- the guest never
    // returns to a host loop. Time-gated internally, so the per-frame cost is
    // a clock read and a compare.
    robox_discord_tick();

    /* Settings menu: holds the invincibility pin. Runs whether the menu is
     * open or not -- the point of the toggle is that it keeps holding after
     * you close it. Returns immediately when nothing is pinned. */
    { extern void robox_menu_tick(void); robox_menu_tick(); }

    /* One line per level change, from the per-frame read rather than from the
     * file load -- so the log shows the mapping following the player through
     * every door, which is the whole claim being made about this pointer. */
    {
        static uint32_t last_id = 0xffffffffu;
        uint32_t id = MEM_R32(LEVEL_ID_VA);
        if (id != last_id) {
            last_id = id;
            fprintf(stderr, "[level] now id %u (%s)  d-pad rotation %u deg\n",
                    id, robox_level_name(), video_sideways_q() * 90u);
            fflush(stderr);
        }
    }

    /* Lua mods (sdk/robox_lua.c). Rides the same pump for the same reason
     * Discord does -- the guest never returns to a host loop, so this is the
     * only per-frame moment there is. Returns immediately when the mod is off.
     * After the level read above, so a "level" handler sees the new id on the
     * frame it changed. */
    { extern void robox_lua_tick(void); robox_lua_tick(); }

#if defined(__EMSCRIPTEN__)
    /* Deliver input independently of frame pacing.
     *
     * SDL2's emscripten backend registers its keydown/keyup handlers with
     * EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD, i.e. on the guest worker, so
     * every key event arrives as a proxied call on THIS thread's queue rather
     * than as something SDL_PollEvent can see. SDL cannot drain that queue --
     * its Emscripten_PumpEvents is literally an empty function. The only thing
     * that drained it was the nanosleep inside frame_limiter().
     *
     * A late frame skips that sleep entirely (peripherals.c: the
     * `if (next_us <= now_us) { next_us = now_us; return; }` resync path), so
     * on exactly the frames the game is already struggling, NO key event was
     * delivered at all: SDL_PollEvent returned 0 and input went dead. Worse,
     * a KEYUP that landed during the spike stayed queued while g_input_hold
     * kept the bit from before it -- so a held direction latched ON and stayed
     * on, because the only event that could clear it was stuck in the queue.
     *
     * Draining here costs nothing when the queue is empty and makes input
     * arrival independent of whether the limiter slept. */
    emscripten_current_thread_process_queued_calls();
#endif

    /* A mod holding the keyboard (the console) gets everything, and this pump
     * touches nothing: no menu, no fullscreen toggle, no F-key mods. Otherwise
     * typing at a prompt would trip half the port's hotkeys, and Escape --
     * which every console in the world closes on -- would open the settings
     * menu instead. Quitting still works; the window close box is not a key. */
    extern int  robox_lua_capture_active(void);      /* sdk/robox_lua.h */
    extern void robox_lua_text_input(const char *utf8);
    const int captured = robox_lua_capture_active();

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            fprintf(stderr, "[video] window closed, exiting\n");
            fflush(stderr);
            exit(0);
        } else if (e.type == SDL_TEXTINPUT) {
            /* Typed characters, with shift, symbols and the user's own layout
             * already applied -- none of which scancodes can tell you. */
            robox_lua_text_input(e.text.text);
        } else if (captured && e.type == SDL_KEYDOWN) {
            /* The holder sees keys through robox.on("key"), which is edge
             * detection over SDL's keyboard state and needs nothing from here.
             * Swallowing them here is the point: nobody else acts on them.
             *
             * KEYDOWN only. Every branch below that acts on a key acts on the
             * press, while KEYUP does nothing but CLEAR held-input bits -- so
             * ups have to keep flowing. Swallow those and a direction held
             * when the console opened stays held in g_input_hold, and the
             * robot walks off on his own the moment you close it. Mouse,
             * window focus and controller events are not the keyboard and are
             * none of capture's business either. */
        } else if (e.type == SDL_KEYDOWN && !e.key.repeat &&
                   (e.key.keysym.sym == SDLK_ESCAPE ||
                    e.key.keysym.sym == SDLK_F1 ||          /* browsers can eat Esc */
                    robox_menu_is_open())) {
            /* Escape opens the controls menu; while it is up the menu eats
             * every key so nothing steers the player behind the panel. Held
             * inputs are dropped on the way in, or whatever was down when the
             * menu opened would latch (the button words are accumulators). */
            if (!robox_menu_is_open()) input_release_all();
            if (!robox_menu_key(e.key.keysym.sym)) robox_menu_toggle();
            if (!robox_menu_is_open()) input_release_all();
        } else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            /* F12 toggles the on-screen frame counter. F4 stays as an alias:
             * in a browser F12 is swallowed by devtools before the page ever
             * sees it, so the web build needs a key that actually arrives
             * (or ?fps=1 in the URL). */
            if (e.key.keysym.sym == SDLK_F12 || e.key.keysym.sym == SDLK_F4) {
                g_show_fps = !g_show_fps;
                fprintf(stderr, "[video] fps counter %s\n",
                        g_show_fps ? "ON" : "off");
                fflush(stderr);
            } else if (e.key.keysym.sym == SDLK_F11) {
                /* The game launches fullscreen, so there has to be a way back
                 * out that is not "quit and set an environment variable". */
                Uint32 on = SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                SDL_SetWindowFullscreen(g_window,
                                        on ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                /* Remembered, so someone who had to leave fullscreen to make
                 * streaming work does not have to do it again every launch. */
                s_cfg_fullscreen = on ? 0 : 1;
                video_cfg_save();
                fprintf(stderr, "[video] fullscreen %s (saved)\n", on ? "off" : "ON");
                fflush(stderr);
            } else if (e.key.keysym.sym == SDLK_F2) {
                /* Mario mod: poof the robot away and spawn Mario (and back). */
                extern void robox_mario_visual_toggle(void);
                robox_mario_visual_toggle();
            } else if (e.key.keysym.sym == SDLK_F3) {
                /* Mario mod: cycle small / big / fire. */
                extern void robox_mario_power_cycle(void);
                robox_mario_power_cycle();
            } else if (e.key.keysym.sym == SDLK_F8) {
                extern void robox_menu_snapshot(void);
                robox_menu_snapshot();
            } else if (e.key.keysym.sym == SDLK_F7) {
                /* The level id gets this right at every door; this is the
                 * escape hatch for sections inside an 04 level that grip the
                 * Wiimote differently again. Takes effect on the next read. */
                g_sideways_force = !g_sideways_force;
                fprintf(stderr, "[input] d-pad rotation %u quarter turns"
                        " (level %s, robot=%d%s)\n",
                        video_sideways_q(), robox_level_name(),
                        robox_level_is_robot(),
                        g_sideways_force ? ", F7 inverted" : "");
                fflush(stderr);
            } else if (e.key.keysym.sym == SDLK_F5) {
                static const char *names[5] = {
                    "AUTO (follow game flag)", "forced 0 deg (vertical)",
                    "forced 90 deg (sideways)", "forced 180 deg",
                    "forced 270 deg"
                };
                g_coop_rot_mode = (g_coop_rot_mode + 1) % 5;
                fprintf(stderr, "[COOP] d-pad orientation: %s\n",
                        names[g_coop_rot_mode]);
                fflush(stderr);
            } else {
                /* Movement is collected outside the switch: WASD is consumed
                 * by the nunchuk-stick cases below and the arrows fall to
                 * default, but both must reach the co-op mod's direction
                 * word. */
                g_p1_dirs |= key_to_dir_p1(e.key.keysym.sym);
                g_p2_dirs |= key_to_dir_p2(e.key.keysym.sym);
                /* A user binding beats the built-in special-purpose keys.
                 * The switch below claims W/A/S/D (nunchuk stick), E/Q and U
                 * (shakes) and the modifiers, and it used to run FIRST -- so
                 * rebinding anything onto one of those keys in the controls
                 * menu silently did nothing, because key_to_wpad() was only
                 * consulted in the default arm it never reached. */
                uint32_t bound_dn = key_to_wpad(e.key.keysym.sym);
                if (bound_dn) {
                    g_input_hold |= bound_dn;
                    apply_virtual(bound_dn, 1);
                    g_p2_btn     |= key_to_btn_p2(e.key.keysym.sym);
                } else switch (e.key.keysym.sym) {
                    case SDLK_w: g_key_w = 1; break;      /* nunchuk stick */
                    case SDLK_a: g_key_a = 1; break;
                    case SDLK_s: g_key_s = 1; break;
                    case SDLK_d: g_key_d = 1; break;
                    case SDLK_e: g_shake_wm = 1; break;   /* shake Wiimote */
                    case SDLK_q: g_shake_nk = 1; break;   /* shake Nunchuk */
                    case SDLK_u: case SDLK_SLASH: g_p2_shake = 1; break;
                    case SDLK_LALT:  case SDLK_RALT:  g_p1_tilt_l = 1; break;
                    case SDLK_LCTRL: case SDLK_RCTRL: g_p1_tilt_r = 1; break;
                    default:
                        g_input_hold |= key_to_wpad(e.key.keysym.sym);
                        g_p2_btn     |= key_to_btn_p2(e.key.keysym.sym);
                }
            }
        } else if (e.type == SDL_KEYUP) {
            g_p1_dirs &= ~key_to_dir_p1(e.key.keysym.sym);
            g_p2_dirs &= ~key_to_dir_p2(e.key.keysym.sym);
            /* Mirror of the KEYDOWN path: the release has to take the same
             * branch the press did, or a rebound key sets a bit that nothing
             * ever clears. */
            uint32_t bound_up = key_to_wpad(e.key.keysym.sym);
            if (bound_up) {
                g_input_hold &= ~bound_up;
                apply_virtual(bound_up, 0);
                g_p2_btn     &= ~key_to_btn_p2(e.key.keysym.sym);
            } else switch (e.key.keysym.sym) {
                case SDLK_w: g_key_w = 0; break;
                case SDLK_a: g_key_a = 0; break;
                case SDLK_s: g_key_s = 0; break;
                case SDLK_d: g_key_d = 0; break;
                case SDLK_e: g_shake_wm = 0; break;
                case SDLK_q: g_shake_nk = 0; break;
                case SDLK_u: case SDLK_SLASH: g_p2_shake = 0; break;
                case SDLK_LALT:  case SDLK_RALT:  g_p1_tilt_l = 0; break;
                case SDLK_LCTRL: case SDLK_RCTRL: g_p1_tilt_r = 0; break;
                default:
                    g_input_hold &= ~key_to_wpad(e.key.keysym.sym);
                    g_p2_btn     &= ~key_to_btn_p2(e.key.keysym.sym);
            }
        } else if (e.type == SDL_MOUSEMOTION) {
            pointer_from_window_px((float)e.motion.x, (float)e.motion.y);
        } else if (e.type == SDL_MOUSEBUTTONDOWN) {
            if (e.button.button == SDL_BUTTON_LEFT)  g_input_hold |= 0x0800; /* A */
            if (e.button.button == SDL_BUTTON_RIGHT) g_input_hold |= 0x0400; /* B */
        } else if (e.type == SDL_MOUSEBUTTONUP) {
            if (e.button.button == SDL_BUTTON_LEFT)  g_input_hold &= ~0x0800u;
            if (e.button.button == SDL_BUTTON_RIGHT) g_input_hold &= ~0x0400u;
        } else if (e.type == SDL_WINDOWEVENT &&
                   e.window.event == SDL_WINDOWEVENT_LEAVE) {
            g_mouse_valid = 0;   /* pointer off-screen, like aiming away */
        } else if (e.type == SDL_WINDOWEVENT &&
                   e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            /* Alt-tab away mid-press and the KEYUP goes to whatever took
             * focus, never to us -- the bit would stay held for the rest of
             * the session. SDL2 does synthesise releases on blur, but the
             * game has to actually act on losing focus for that to matter.
             * Nothing here is a real input, so dropping it all is safe. */
            input_release_all();
        } else if (e.type == SDL_FINGERDOWN || e.type == SDL_FINGERMOTION ||
                   e.type == SDL_FINGERUP) {
            touch_event(&e.tfinger, (int)e.type);
        } else if (e.type == SDL_CONTROLLERDEVICEADDED) {
            pad_open_first();
        } else if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
            pad_close(e.cdevice.which);
        } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
            g_input_hold |= pad_button_to_wpad(e.cbutton.button);
            g_p1_dirs    |= pad_button_to_dir(e.cbutton.button);
            /* Shoulders drive the two shake gestures: the jump tutorial asks
             * the player to shake, and there is no other gamepad idiom for it. */
            if (e.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) g_shake_wm = 1;
            if (e.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER)  g_shake_nk = 1;
        } else if (e.type == SDL_CONTROLLERBUTTONUP) {
            g_input_hold &= ~pad_button_to_wpad(e.cbutton.button);
            g_p1_dirs    &= ~pad_button_to_dir(e.cbutton.button);
            if (e.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) g_shake_wm = 0;
            if (e.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER)  g_shake_nk = 0;
        } else if (e.type == SDL_CONTROLLERAXISMOTION) {
            switch (e.caxis.axis) {
                /* KPAD stick convention is +y UP, SDL is +y DOWN -> negate. */
                case SDL_CONTROLLER_AXIS_LEFTX:
                    g_pad_stick_x =  pad_axis_norm(e.caxis.value); break;
                case SDL_CONTROLLER_AXIS_LEFTY:
                    g_pad_stick_y = -pad_axis_norm(e.caxis.value); break;
                /* Right stick aims the IR pointer, so a pad can drive the menu
                 * cursor that otherwise only the mouse could reach. */
                case SDL_CONTROLLER_AXIS_RIGHTX: {
                    float v = pad_axis_norm(e.caxis.value);
                    if (v != 0.0f) { g_mouse_nx = clampf(g_mouse_nx + v * 0.04f, -1.0f, 1.0f);
                                     g_mouse_valid = 1; }
                    break;
                }
                case SDL_CONTROLLER_AXIS_RIGHTY: {
                    float v = pad_axis_norm(e.caxis.value);
                    if (v != 0.0f) { g_mouse_ny = clampf(g_mouse_ny + v * 0.04f, -1.0f, 1.0f);
                                     g_mouse_valid = 1; }
                    break;
                }
                default: break;
            }
        }
    }

    /* OGL renderer handles its own swap via gx_ogl_present() called from
     * gx_execute_copy; video_present just pumps events in that case. */
    if (gx_ogl_ready()) return;

    if (!g_renderer || !g_tex || !g_efb_pixels) return;

    // Debug mode: RECOMP_SDL_TEST=1 forces a solid bright-red fill so we
    // can confirm the SDL window is reachable independent of the rasterizer
    // pipeline.
    static int test_mode = -1;
    if (test_mode < 0) {
        const char *e = getenv("RECOMP_SDL_TEST");
        test_mode = (e && e[0] == '1');
    }
    if (test_mode) {
        SDL_SetRenderDrawColor(g_renderer, 255, 0, 0, 255);
        SDL_RenderClear(g_renderer);
        SDL_RenderPresent(g_renderer);
        return;
    }

    int rc = SDL_UpdateTexture(g_tex, NULL, g_efb_pixels, EFB_WIDTH * 4);
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_tex, NULL, NULL);
    SDL_RenderPresent(g_renderer);
    static int log_n;
    if (log_n < 4 || (log_n & 0x1FF) == 0) {
        // Read back what the renderer thinks is on screen after present.
        uint32_t readback = 0;
        SDL_Rect r = {100, 100, 1, 1};
        SDL_RenderReadPixels(g_renderer, &r, SDL_PIXELFORMAT_ARGB8888,
                              &readback, 4);
        fprintf(stderr, "[video] present #%d updateTex=%d src=%08x readback=%08x err=%s\n",
                log_n, rc, g_efb_pixels[100*EFB_WIDTH + 100],
                readback, SDL_GetError());
        fflush(stderr);
    }
    log_n++;
}


static inline uint8_t clamp_u8(int v) {
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}


// Direct path: copy a 640x480 ARGB framebuffer straight into the SDL
// texture without going through YUYV. The rasterizer uses this to avoid
// YUYV lossiness on bright hash-shade colors.
void video_blit_argb(const uint32_t *pixels) {
    if (!g_efb_pixels || !pixels) return;
    memcpy(g_efb_pixels, pixels, EFB_WIDTH * EFB_HEIGHT * sizeof(uint32_t));
    static int log_n;
    if (log_n < 8 || (log_n & 0xFF) == 0) {
        fprintf(stderr, "[video] direct blit #%d pixel(100,100)=%08x\n",
                log_n, g_efb_pixels[100*EFB_WIDTH + 100]);
        fflush(stderr);
    }
    log_n++;
}

// Decode the Wii XFB (YUYV 4:2:2, big-endian pairs) at guest_xfb_va into
// the host ARGB streaming texture. Each 4-byte group: Y0 Cb Y1 Cr ->
// two RGB pixels. 640*480 == 614400 bytes.
void video_copy_efb_to_window(uint32_t guest_xfb_va) {
    if (!g_efb_pixels) return;
    if (!guest_xfb_va) return;
    const uint8_t *src = (const uint8_t*)ppc_host_ptr(guest_xfb_va);
    if (!src) return;
    for (int y = 0; y < EFB_HEIGHT; ++y) {
        const uint8_t *row = src + y * EFB_WIDTH * 2;
        uint32_t *dst = &g_efb_pixels[y * EFB_WIDTH];
        for (int x = 0; x < EFB_WIDTH; x += 2) {
            int Y0 = row[x * 2 + 0];
            int Cb = row[x * 2 + 1];
            int Y1 = row[x * 2 + 2];
            int Cr = row[x * 2 + 3];
            int cb = Cb - 128;
            int cr = Cr - 128;
            int c0 = Y0 - 16;
            int c1 = Y1 - 16;
            int r0 = (298 * c0           + 409 * cr + 128) >> 8;
            int g0 = (298 * c0 - 100*cb - 208 * cr + 128) >> 8;
            int b0 = (298 * c0 + 516*cb           + 128) >> 8;
            int r1 = (298 * c1           + 409 * cr + 128) >> 8;
            int g1 = (298 * c1 - 100*cb - 208 * cr + 128) >> 8;
            int b1 = (298 * c1 + 516*cb           + 128) >> 8;
            dst[x]     = (0xFFu << 24) | (clamp_u8(r0) << 16) | (clamp_u8(g0) << 8) | clamp_u8(b0);
            dst[x + 1] = (0xFFu << 24) | (clamp_u8(r1) << 16) | (clamp_u8(g1) << 8) | clamp_u8(b1);
        }
    }
}

#endif /* !__3DS__ */
