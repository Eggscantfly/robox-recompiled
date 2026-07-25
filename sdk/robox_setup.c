/* sdk/robox_setup.c -- the space-themed first-run setup.
 *
 * Desktop only, and deliberately so: Android extracts its data out of the APK
 * in robox_android_bootstrap(), the 3DS reads it from RomFS, and the web build
 * has it preloaded into MEMFS. All three arrive with the game data already in
 * place, so there is nothing for a setup screen to do -- it compiles out to a
 * function that returns 0.
 *
 * Drawn entirely with the overlay batcher in sdk/gx_ogl.c (rects, outlines and
 * text in a virtual 1280x720 space), the same one the crash screen and the FPS
 * counter use. No new renderer, no new shaders: a starfield is a few hundred
 * small rects, and that is genuinely all this needs.
 *
 * The splash does NOT play here. It stays where it has always been, after the
 * game data is loaded and just before the guest entry, so the port's credit
 * runs against a game that is actually about to start.
 */
#include "robox_setup.h"

#if defined(__3DS__) || defined(__ANDROID__) || defined(__EMSCRIPTEN__)

int robox_setup_run(const char *install_dir) { (void)install_dir; return 0; }

#else

#include "gx_ogl.h"
#include "robox_dls.h"
#include "robox_wad.h"

#include <SDL2/SDL.h>
#include <dirent.h>
#include <math.h>

/* Pointing the port at a folder elsewhere means chdir()ing into it: every path
 * below here is relative ("Robox USA.dol", "Assets/..."), and the Android and
 * 3DS bootstraps in main.c solve the same problem the same way. Remembering
 * the directory instead would mean threading it through ppc_load_image and
 * every fopen in the engine. */
#if defined(_WIN32)
#  include <direct.h>
#  define setup_chdir(p) _chdir(p)
#  define setup_mkdir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  include <unistd.h>
#  define setup_chdir(p) chdir(p)
#  define setup_mkdir(p) mkdir((p), 0755)
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Baked by tools/bin2c.py from the .rbxs files under Setup/. These are OUR
 * audio, authored for this project -- see .gitignore for why nothing from the
 * game itself is in here. */
#include "../src/generated_setup/setup_music_rbxs.h"
#include "../src/generated_setup/setup_select_rbxs.h"
#include "../src/generated_setup/setup_back_rbxs.h"
#include "../src/generated_setup/setup_move_rbxs.h"

/* Default mod configs, baked from the .cfg files under mods/. Text, not audio, but the same
 * reasoning applies: they have to exist on a fresh install and there is
 * nowhere to read them from yet. */
#include "../src/generated_setup/mods_cfg.h"
#include "../src/generated_setup/wav_music_cfg.h"

/* stb_vorbis is compiled as part of robox_wav.c's translation unit. Declared
 * rather than included: a third copy of the decoder in the link would be a
 * megabyte of duplicate code and a pile of duplicate symbols. */
extern int stb_vorbis_decode_memory(const unsigned char *mem, int len,
                                    int *channels, int *sample_rate,
                                    short **output);

/* video.c owns the window; gx_ogl.c and os.c reach for it the same way. */
extern SDL_Window *g_window;
extern void        frame_limiter(void);

/* ------------------------------------------------------------------------ */
/* palette + layout                                                          */
/* ------------------------------------------------------------------------ */

#define VW 1280.0f
#define VH  720.0f

/* Matched to the game's own main menu: near-black with a cold blue lift toward
 * the bottom where the planet horizon sits, and a dense field of small faint
 * stars over it. The menu is monochrome -- white text, a thin white rule around
 * whatever is selected -- so there is no accent colour here on purpose. An
 * earlier pass used cyan for everything interactive and read as a launcher
 * bolted onto the game rather than part of it. */
#define BG_TOP_R    0.008f
#define BG_TOP_G    0.009f
#define BG_TOP_B    0.018f
#define BG_BOT_R    0.014f
#define BG_BOT_G    0.016f
#define BG_BOT_B    0.030f

#define FG_R 1.00f      /* selected / primary text */
#define FG_G 1.00f
#define FG_B 1.00f

#define DIM_R 0.58f     /* unselected entries, and anything secondary */
#define DIM_G 0.62f
#define DIM_B 0.70f

#define FAINT_R 0.38f   /* prompts, copyright line */
#define FAINT_G 0.42f
#define FAINT_B 0.50f

#define WARN_R 1.00f    /* amber: "this is not the DOL we translated" */
#define WARN_G 0.72f
#define WARN_B 0.25f

#define BAD_R  1.00f
#define BAD_G  0.36f
#define BAD_B  0.36f

#define OK_R   0.42f
#define OK_G   1.00f
#define OK_B   0.62f

/* Measured out of src/robox_font_metrics.h rather than guessed: every glyph in
 * the game's title face is 33 px tall with a 23 px advance at scale 1, and the
 * quad it draws is 45 px wide. The quad being twice the advance is deliberate
 * -- the letters overlap, which is what gives the face its joined-up look, and
 * it means a string's painted width runs past what overlay_text_width (which
 * sums advances) reports. Centre off the reported width anyway: the overhang
 * is symmetric, so it still lands centred.
 *
 * Getting this number wrong is expensive. The first cut assumed 16 and every
 * scale below was picked to match, so the whole screen came out at roughly
 * double size with the title sitting on top of the subtitle. */
#define TEXT_EM 33.0f
#define LINE(s) (TEXT_EM * (s))

/* One scale per role, so nothing is a magic number at the call site. Sizes are
 * taken off the retail menu: its entries measure ~30 px tall in this virtual
 * space, which is scale 0.9 against a 33 px face. */
#define S_ITEM   0.92f      /* menu entries                          30 px */
#define S_SLATE  0.62f      /* the "robox recomp" mark, top left     20 px */
#define S_BODY   0.56f      /* status lines                          18 px */
#define S_SMALL  0.48f      /* prompts, copyright                    16 px */

#define TRACK_ITEM  4.0f    /* the menu is set wide; matching it matters */
#define TRACK_BODY  1.5f

/* The retail menu right-aligns its entries and hangs the selection box off the
 * same edge, overshooting the text by a fixed inset. Everything below is that
 * one edge plus offsets, so the column stays true if any of it moves. */
#define MENU_RIGHT   1150.0f
#define MENU_INSET     46.0f
#define MENU_BOX_MIN_W 300.0f
#define MENU_BOX_H      48.0f
#define MENU_Y0       286.0f
#define MENU_STEP      72.0f

#define SLATE_X       104.0f
#define SLATE_Y        84.0f

#define STATUS_X      104.0f            /* bottom-left, where the game puts  */
#define STATUS_Y      (VH - 132.0f)     /* its copyright line                */

#define PROMPT_RIGHT  (MENU_RIGHT + MENU_INSET)
#define PROMPT_Y      (VH - 128.0f)
#define PROMPT_STEP    38.0f

/* The finish, in one place so the whole beat can be retimed by reading three
 * numbers instead of hunting them across the draw and the loop.
 *
 * "done!" fades up over DONE_IN, sits until DONE_HOLD_MS, then the picture and
 * the music fade out together over DONE_OUT_MS. Roughly four seconds end to
 * end: the first attempt ran the whole thing in 1.7 s and there was no time to
 * register any of it before the screen was already black. */
#define DONE_IN         0.7f     /* seconds, text fade-in                    */
#define DONE_HOLD_MS   2600u     /* until the fade-out starts                */
#define DONE_OUT_MS    1400u     /* picture + music to black and silence     */

/* ------------------------------------------------------------------------ */
/* audio                                                                     */
/* ------------------------------------------------------------------------ */

typedef struct {
    short *pcm;        /* interleaved, owned                                 */
    int    frames;     /* samples per channel                                */
    int    channels;
    int    rate;
    int    loop;       /* RBXS loop flag                                     */
    int    loop_start; /* frames; 0,0 means "the whole thing"                */
    int    loop_end;
} clip;

/* RBXS is a 24-byte header in front of an OGG (see tools/make_rbxs.py). The
 * ":P" at 0x0E is not decoration -- it is the format's sanity check, and a
 * file that fails it is something else that happens to start with "RBXS". */
static int rbxs_decode(const unsigned char *data, unsigned len, clip *out)
{
    memset(out, 0, sizeof *out);
    if (len < 24 || memcmp(data, "RBXS", 4) != 0) return -1;
    if (memcmp(data + 0x0E, ":P", 2) != 0) return -1;

    out->loop = (data[0x0C] | (data[0x0D] << 8)) != 0;

    /* Loop points are frames (samples per channel), little-endian at 0x10/0x14.
     * Both zero means loop the whole thing. */
    unsigned ls = (unsigned)data[0x10] | ((unsigned)data[0x11] << 8) |
                  ((unsigned)data[0x12] << 16) | ((unsigned)data[0x13] << 24);
    unsigned le = (unsigned)data[0x14] | ((unsigned)data[0x15] << 8) |
                  ((unsigned)data[0x16] << 16) | ((unsigned)data[0x17] << 24);

    short *pcm = NULL;
    int ch = 0, rate = 0;
    int frames = stb_vorbis_decode_memory(data + 24, (int)(len - 24),
                                          &ch, &rate, &pcm);
    if (frames <= 0 || !pcm) return -1;

    out->pcm = pcm; out->frames = frames; out->channels = ch; out->rate = rate;

    /* Clamp rather than reject: a loop end past the decoded length is what you
     * get when the points were measured on the source WAV and the Vorbis
     * encoder landed a frame or two short. Silently trimming it is right; the
     * alternative is refusing to play the music over a rounding error. */
    out->loop_start = (int)ls;
    out->loop_end   = (int)le;
    if (out->loop_end <= 0 || out->loop_end > frames) out->loop_end = frames;
    if (out->loop_start < 0 || out->loop_start >= out->loop_end)
        out->loop_start = 0;
    return 0;
}

static void clip_free(clip *c)
{
    free(c->pcm);
    c->pcm = NULL;
}

static size_t clip_bytes(const clip *c)
{
    return (size_t)c->frames * (size_t)c->channels * sizeof(short);
}


/* Two devices rather than a mixer: SDL_QueueAudio appends, it does not blend,
 * so a one-shot queued onto the music device would play *after* the music
 * instead of over it. Opening a second device is three lines and the OS mixes
 * them for us. */
static SDL_AudioDeviceID s_dev_music, s_dev_sfx;
static clip s_music, s_select, s_back, s_move;

static SDL_AudioDeviceID open_for(const clip *c, SDL_AudioCallback cb)
{
    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq     = c->rate;
    want.format   = AUDIO_S16SYS;
    want.channels = (Uint8)c->channels;
    want.samples  = 1024;
    want.callback = cb;          /* NULL leaves the device queue-driven */
    SDL_AudioDeviceID d = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (d) SDL_PauseAudioDevice(d, 0);
    return d;
}

/* The music plays from a callback, not a queue.
 *
 * A queue has to be topped up from the main loop, and the native file dialogs
 * are modal: they block this thread for as long as the browser is open, the
 * queue drains, and the music cuts out exactly when the user is doing the one
 * thing the setup asked them to do. A callback runs on SDL's own audio thread
 * and does not care what the main thread is up to.
 *
 * Read from the audio thread, written from the main one. The position is only
 * touched here, and the gain is a float written in one store -- a torn read
 * during a fade would at worst cost one buffer at a slightly wrong volume. */
static size_t s_music_pos;
static float  s_music_gain = 1.0f;

static void SDLCALL music_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    const clip *c = &s_music;
    int16_t *out = (int16_t *)stream;
    int frames = len / (int)((size_t)c->channels * sizeof(int16_t));

    if (!c->pcm) { memset(stream, 0, (size_t)len); return; }

    float gain = s_music_gain;
    while (frames > 0) {
        int end = c->loop ? c->loop_end : c->frames;
        int avail = end - (int)s_music_pos;

        if (avail <= 0) {
            if (!c->loop) {
                memset(out, 0, (size_t)frames * c->channels * sizeof(int16_t));
                return;
            }
            /* Back to the loop point, never to the top: the intro before
             * loop_start is heard once and never again. */
            s_music_pos = (size_t)c->loop_start;
            continue;
        }

        int n = avail < frames ? avail : frames;
        const int16_t *src = c->pcm + s_music_pos * (size_t)c->channels;
        int samples = n * c->channels;

        if (gain >= 0.999f) {
            memcpy(out, src, (size_t)samples * sizeof(int16_t));
        } else {
            for (int i = 0; i < samples; ++i)
                out[i] = (int16_t)((float)src[i] * gain);
        }

        out += samples;
        s_music_pos += (size_t)n;
        frames -= n;
    }
}

static void audio_start(void)
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[setup] no audio subsystem: %s\n", SDL_GetError());
        return;
    }
    if (rbxs_decode(setup_music_rbxs,  setup_music_rbxs_len,  &s_music) == 0) {
        s_music_pos = 0;
        s_dev_music = open_for(&s_music, music_cb);
    }
    /* Effects stay queue-driven: they are one-shots fired by a keypress, and
     * no keypress can arrive while a modal dialog owns the thread. */
    if (rbxs_decode(setup_select_rbxs, setup_select_rbxs_len, &s_select) == 0)
        s_dev_sfx = open_for(&s_select, NULL);
    rbxs_decode(setup_back_rbxs, setup_back_rbxs_len, &s_back);
    rbxs_decode(setup_move_rbxs, setup_move_rbxs_len, &s_move);

}

static void audio_sfx(const clip *c)
{
    if (!s_dev_sfx || !c->pcm) return;
    SDL_ClearQueuedAudio(s_dev_sfx);       /* retrigger, do not stack */
    SDL_QueueAudio(s_dev_sfx, c->pcm, (Uint32)clip_bytes(c));
}

static void audio_stop(void)
{
    if (s_dev_music) { SDL_ClearQueuedAudio(s_dev_music); SDL_CloseAudioDevice(s_dev_music); s_dev_music = 0; }
    if (s_dev_sfx)   { SDL_ClearQueuedAudio(s_dev_sfx);   SDL_CloseAudioDevice(s_dev_sfx);   s_dev_sfx = 0; }
    clip_free(&s_music); clip_free(&s_select); clip_free(&s_back);
    clip_free(&s_move);
}

/* ------------------------------------------------------------------------ */
/* starfield                                                                 */
/* ------------------------------------------------------------------------ */

/* The retail menu's sky is dense and small-grained rather than a few bright
 * points, so this leans the same way: many stars, most of them one pixel. */
#define N_STARS 460

typedef struct {
    float x, y, size, base_a, phase, twinkle, drift;
} star;

static star s_stars[N_STARS];

static unsigned rng_next(unsigned *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s;
}

static float rng_f(unsigned *s)
{
    return (float)(rng_next(s) >> 8) / 16777216.0f;
}

/* Three depth bands. The near stars drift several times faster than the far
 * ones, which is the whole trick -- flat-speed stars read as noise, parallax
 * reads as motion. */
static void stars_init(void)
{
    unsigned seed = 0x0B0BA5E5u;
    for (int i = 0; i < N_STARS; ++i) {
        star *st = &s_stars[i];
        int band = i % 3;
        st->x       = rng_f(&seed) * VW;
        st->y       = rng_f(&seed) * VH;
        st->phase   = rng_f(&seed) * 6.2831853f;
        st->twinkle = 0.6f + rng_f(&seed) * 2.2f;
        if (band == 0)      { st->size = 1.0f; st->base_a = 0.12f + rng_f(&seed) * 0.16f; st->drift = 2.0f; }
        else if (band == 1) { st->size = 1.0f; st->base_a = 0.28f + rng_f(&seed) * 0.26f; st->drift = 5.0f; }
        else                { st->size = 2.0f; st->base_a = 0.50f + rng_f(&seed) * 0.35f; st->drift = 9.0f; }
    }
}

static void stars_draw(float t)
{
    for (int i = 0; i < N_STARS; ++i) {
        const star *st = &s_stars[i];
        float x = st->x - st->drift * t;
        x = fmodf(x, VW);
        if (x < 0.0f) x += VW;
        float a = st->base_a * (0.65f + 0.35f * sinf(t * st->twinkle + st->phase));
        gx_ogl_overlay_rect(x, st->y, st->size, st->size, 1.0f, 1.0f, 1.0f, a);
    }
}

/* Shooting stars.
 *
 * Stateless on purpose: each slot's whole life is a function of `t`, so there
 * is nothing to update, nothing to reset when the screen changes state, and
 * nothing that drifts if a frame is slow (the extract blocks for seconds at a
 * time and calls back to redraw, which would wreck an incrementally-stepped
 * animation). The cycle index seeds the trajectory, so every pass is different
 * but a given pass is always the same.
 *
 * The tail is a run of shrinking, fading quads behind the head -- the overlay
 * draws flat rects and nothing else, and at this size it reads as a streak. */
#define N_SHOOTERS 3

static void shooters_draw(float t)
{
    for (int i = 0; i < N_SHOOTERS; ++i) {
        /* Deliberately non-harmonic periods so the three never sync up into a
         * visible rhythm. */
        float period = 8.0f + 3.7f * (float)i;
        float phase  = t / period + 0.41f * (float)i;
        unsigned cycle = (unsigned)phase;
        float u = phase - (float)cycle;

        /* Only the first slice of each cycle is flight; the rest is the wait. */
        const float FLY = 0.11f;
        if (u > FLY) continue;
        float k = u / FLY;

        unsigned seed = cycle * 2654435761u + (unsigned)i * 40503u + 12345u;
        float sx   = 260.0f + rng_f(&seed) * VW;
        float sy   = -40.0f + rng_f(&seed) * VH * 0.5f;
        float dist = 380.0f + rng_f(&seed) * 320.0f;
        float tail = 70.0f  + rng_f(&seed) * 60.0f;

        /* Down and to the left, roughly with the drift of the starfield. */
        const float dx = -0.88f, dy = 0.47f;
        float hx = sx + dx * dist * k;
        float hy = sy + dy * dist * k;

        /* Fade in and out across the flight so nothing pops on or off. */
        float fade = sinf(k * 3.14159265f);

        const int SEG = 16;
        for (int s = 0; s < SEG; ++s) {
            float f  = (float)s / (float)(SEG - 1);
            float px = hx - dx * tail * f;
            float py = hy - dy * tail * f;
            float a  = (1.0f - f) * (1.0f - f) * fade * 0.95f;
            float sz = 2.2f * (1.0f - f) + 0.7f;
            gx_ogl_overlay_rect(px, py, sz, sz, 1.0f, 1.0f, 1.0f, a);
        }
    }
}

/* Background: a vertical gradient faked with bands, then the star field. The
 * overlay only draws flat quads, and at this alpha nobody can tell. */
static void background_draw(float t)
{
    const int BANDS = 36;
    float bh = VH / (float)BANDS;
    for (int i = 0; i < BANDS; ++i) {
        float k = (float)i / (float)(BANDS - 1);
        gx_ogl_overlay_rect(0.0f, (float)i * bh, VW, bh + 1.0f,
                            BG_TOP_R + (BG_BOT_R - BG_TOP_R) * k,
                            BG_TOP_G + (BG_BOT_G - BG_TOP_G) * k,
                            BG_TOP_B + (BG_BOT_B - BG_TOP_B) * k,
                            1.0f);
    }
    stars_draw(t);
    shooters_draw(t);
}

/* ------------------------------------------------------------------------ */
/* widgets                                                                   */
/* ------------------------------------------------------------------------ */

static void text_centre(float cx, float y, const char *s,
                        float r, float g, float b, float a,
                        float scale, float track)
{
    float w = gx_ogl_overlay_text_width(s, scale, track);
    gx_ogl_overlay_text(cx - w * 0.5f, y, s, r, g, b, a, scale, track);
}

/* Last path component. Files dropped on the window arrive with Windows
 * separators while everything built here uses '/', so both have to count --
 * checking only '/' left dropped WADs showing their whole path. */
static const char *path_basename(const char *p)
{
    const char *a = strrchr(p, '/');
    const char *b = strrchr(p, '\\');
    const char *s = a > b ? a : b;
    return s ? s + 1 : p;
}

/* Draw `s` from x, broken on spaces to fit `max_w`. The extractor's failures
 * are whole sentences ("the key is probably wrong, or the WAD is damaged") and
 * run well past any column; wrapping beats clipping the half that says why.
 * Returns the y just past the last line drawn. */
static float text_wrap_left(float x, float y, const char *s,
                            float r, float g, float b, float a,
                            float scale, float track, float max_w)
{
    char line[256] = {0};
    size_t len = 0;

    while (*s) {
        while (*s == ' ') ++s;
        if (!*s) break;

        const char *end = s;
        while (*end && *end != ' ') ++end;
        size_t wlen = (size_t)(end - s);
        if (wlen >= sizeof line) wlen = sizeof line - 1;   /* absurd word */

        char cand[sizeof line];
        size_t clen;
        if (len && len + 1 + wlen < sizeof cand) {
            memcpy(cand, line, len);
            cand[len] = ' ';
            memcpy(cand + len + 1, s, wlen);
            clen = len + 1 + wlen;
        } else {
            memcpy(cand, s, wlen);
            clen = wlen;
        }
        cand[clen] = 0;

        if (len && gx_ogl_overlay_text_width(cand, scale, track) > max_w) {
            gx_ogl_overlay_text(x, y, line, r, g, b, a, scale, track);
            y += LINE(scale) + 6.0f;
            memcpy(line, s, wlen);
            line[wlen] = 0;
            len = wlen;
        } else {
            memcpy(line, cand, clen + 1);
            len = clen;
        }
        s = end;
    }

    if (len) {
        gx_ogl_overlay_text(x, y, line, r, g, b, a, scale, track);
        y += LINE(scale) + 6.0f;
    }
    return y;
}

/* Right-align `s` so its last glyph ends on x_right. */
static void text_right(float x_right, float y, const char *s,
                       float r, float g, float b, float a,
                       float scale, float track)
{
    float w = gx_ogl_overlay_text_width(s, scale, track);
    gx_ogl_overlay_text(x_right - w, y, s, r, g, b, a, scale, track);
}

/* One menu entry, drawn the way the retail menu draws its own: the label
 * right-aligned to a shared edge, and the selection shown as a thin bright
 * outline over a dark fill that overshoots the text by MENU_INSET. Unselected
 * entries get no box at all -- that contrast IS the selection, so adding a
 * second cue (a marker, a glow) only muddies it. */
static void menu_item(float y, const char *label, int selected, int enabled)
{
    float tw = gx_ogl_overlay_text_width(label, S_ITEM, TRACK_ITEM);

    if (selected) {
        /* The box hangs off the shared right edge and overshoots the label by
         * MENU_INSET on both sides, so it grows with the text instead of being
         * a fixed width the longest entry can outrun. A fixed 470 px box
         * clipped both "files already extracted" and "look for key.bin again". */
        float bw = tw + MENU_INSET * 2.0f;
        if (bw < MENU_BOX_MIN_W) bw = MENU_BOX_MIN_W;
        float bx = MENU_RIGHT + MENU_INSET - bw;
        float by = y - (MENU_BOX_H - LINE(S_ITEM)) * 0.5f;
        gx_ogl_overlay_rect(bx, by, bw, MENU_BOX_H, 0.0f, 0.0f, 0.0f, 0.62f);
        gx_ogl_overlay_outline(bx, by, bw, MENU_BOX_H, 1.5f,
                               0.94f, 0.96f, 1.0f, 0.90f);
    }

    float r = selected ? FG_R : DIM_R;
    float g = selected ? FG_G : DIM_G;
    float b = selected ? FG_B : DIM_B;
    float a = enabled ? (selected ? 1.0f : 0.85f) : 0.35f;

    text_right(MENU_RIGHT, y, label, r, g, b, a, S_ITEM, TRACK_ITEM);
}

/* A key prompt, bottom right: the key name boxed like a cap, then the action.
 * The retail menu uses circled Wii button numbers here; on a keyboard the
 * honest equivalent is the key itself, and the font has no circled digits to
 * borrow anyway. */
static void prompt_draw(float y, const char *key, const char *action)
{
    float aw = gx_ogl_overlay_text_width(action, S_SMALL, 1.0f);
    text_right(PROMPT_RIGHT, y, action, DIM_R, DIM_G, DIM_B, 0.85f, S_SMALL, 1.0f);

    float kw = gx_ogl_overlay_text_width(key, S_SMALL, 1.0f);
    float cap_w = kw + 20.0f;
    float cap_x = PROMPT_RIGHT - aw - 18.0f - cap_w;
    float cap_y = y - 5.0f;
    float cap_h = LINE(S_SMALL) + 10.0f;

    gx_ogl_overlay_outline(cap_x, cap_y, cap_w, cap_h, 1.0f,
                           FAINT_R, FAINT_G, FAINT_B, 0.75f);
    gx_ogl_overlay_text(cap_x + 10.0f, y, key,
                        DIM_R, DIM_G, DIM_B, 0.85f, S_SMALL, 1.0f);
}

/* Progress runs along the same left edge and baseline the status lines use, so
 * the working state is the menu state with the column swapped out rather than
 * a different screen. */
static void progress_draw(float y, const char *stage, int pct)
{
    const float bx = STATUS_X, bw = MENU_RIGHT + MENU_INSET - STATUS_X, bh = 10.0f;

    if (stage)
        gx_ogl_overlay_text(bx, y - LINE(S_BODY) - 14.0f, stage,
                            FG_R, FG_G, FG_B, 0.92f, S_BODY, TRACK_BODY);

    char pc[16];
    snprintf(pc, sizeof pc, "%d%%", pct);
    text_right(bx + bw, y - LINE(S_BODY) - 14.0f, pc,
               DIM_R, DIM_G, DIM_B, 0.92f, S_BODY, TRACK_BODY);

    gx_ogl_overlay_rect(bx, y, bw, bh, 0.0f, 0.0f, 0.0f, 0.55f);
    if (pct > 0) {
        float w = bw * (float)pct / 100.0f;
        if (w < 2.0f) w = 2.0f;
        gx_ogl_overlay_rect(bx, y, w, bh, 0.94f, 0.96f, 1.0f, 0.88f);
    }
    gx_ogl_overlay_outline(bx, y, bw, bh, 1.0f, FAINT_R, FAINT_G, FAINT_B, 0.8f);
}

/* ------------------------------------------------------------------------ */
/* native file pickers                                                       */
/* ------------------------------------------------------------------------ */

/* SDL2 has no file dialog -- that arrived in SDL3 -- so this is per-platform.
 * Windows is the only desktop target that ships today; elsewhere the setup
 * falls back to dragging a folder onto the window, which needs no toolkit and
 * pulls in no GTK/Qt dependency for one dialog. */
#if defined(_WIN32)
#  include <windows.h>
#  include <commdlg.h>
#  include <shlobj.h>
#  include <shobjidl.h>
#  include <SDL2/SDL_syswm.h>
#  define HAVE_PICKERS 1

/* Owning the dialog to the game window matters here: the port launches
 * fullscreen, and an unowned dialog can open behind it with nothing on screen
 * to say the game is waiting for input. FULLSCREEN_DESKTOP is a borderless
 * window rather than an exclusive mode, so an owned dialog draws over it. */
static HWND setup_hwnd(void)
{
    SDL_SysWMinfo wmi;
    SDL_VERSION(&wmi.version);
    if (g_window && SDL_GetWindowWMInfo(g_window, &wmi))
        return wmi.info.win.window;
    return NULL;
}

/* Double-NUL terminated pairs; the array's implicit final NUL supplies the
 * second one. */
static const char DOL_FILTER[] =
    "DOL executable (*.dol)\0*.dol\0All files (*.*)\0*.*\0";

static int pick_file(const char *title, char *out, size_t out_size)
{
    char buf[1024] = {0};
    OPENFILENAMEA ofn;

    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner   = setup_hwnd();
    ofn.lpstrFilter = DOL_FILTER;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = sizeof buf;
    ofn.lpstrTitle  = title;
    /* NOCHANGEDIR because the engine resolves everything relative to the
     * working directory -- letting the dialog move it would break every
     * later open. */
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameA(&ofn)) return -1;   /* cancelled */
    snprintf(out, out_size, "%s", buf);
    return 0;
}

/* IFileDialog with FOS_PICKFOLDERS -- the Explorer-style common item dialog,
 * so picking the Assets folder looks like picking the DOL did. The older
 * SHBrowseForFolder is a cramped tree widget from a different decade and
 * looked out of place next to GetOpenFileName.
 *
 * COM from C means going through lpVtbl by hand; there is no smart pointer to
 * lean on, so every Release below is load-bearing. */
static int pick_folder(const char *title, char *out, size_t out_size)
{
    /* SDL may already have COM up; S_FALSE means "already initialised, still
     * yours to balance", RPC_E_CHANGED_MODE means someone chose a different
     * apartment model and we must not tear theirs down. */
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    int owns_com = (hr == S_OK || hr == S_FALSE);
    int rc = -1;

    IFileDialog *fd = NULL;
    hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IFileDialog, (void **)&fd);
    if (SUCCEEDED(hr) && fd) {
        DWORD opts = 0;
        if (SUCCEEDED(fd->lpVtbl->GetOptions(fd, &opts))) {
            /* FORCEFILESYSTEM keeps the result to something with a real path:
             * without it the user can pick a virtual shell location like a
             * library or "This PC", and GetDisplayName then has no filesystem
             * path to hand back. */
            fd->lpVtbl->SetOptions(fd, opts | FOS_PICKFOLDERS |
                                            FOS_FORCEFILESYSTEM |
                                            FOS_PATHMUSTEXIST);
        }

        wchar_t wtitle[256];
        if (MultiByteToWideChar(CP_ACP, 0, title, -1, wtitle,
                                (int)(sizeof wtitle / sizeof wtitle[0])) > 0)
            fd->lpVtbl->SetTitle(fd, wtitle);

        if (SUCCEEDED(fd->lpVtbl->Show(fd, setup_hwnd()))) {
            IShellItem *item = NULL;
            if (SUCCEEDED(fd->lpVtbl->GetResult(fd, &item)) && item) {
                PWSTR wpath = NULL;
                if (SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH,
                                                           &wpath)) && wpath) {
                    /* Back to the ANSI code page, not UTF-8: everything that
                     * later opens these paths goes through plain fopen, which
                     * on Windows takes ANSI. Converting to UTF-8 here would
                     * break any path with a non-ASCII character in it. */
                    if (WideCharToMultiByte(CP_ACP, 0, wpath, -1, out,
                                            (int)out_size, NULL, NULL) > 0)
                        rc = 0;
                    CoTaskMemFree(wpath);
                }
                item->lpVtbl->Release(item);
            }
        }
        fd->lpVtbl->Release(fd);
    }

    if (owns_com) CoUninitialize();
    return rc;
}
#else
#  define HAVE_PICKERS 0
static int pick_file(const char *t, char *o, size_t n)
{ (void)t; (void)o; (void)n; return -1; }
static int pick_folder(const char *t, char *o, size_t n)
{ (void)t; (void)o; (void)n; return -1; }
#endif

/* ------------------------------------------------------------------------ */
/* remembered paths                                                          */
/* ------------------------------------------------------------------------ */

/* Where the DOL and Assets live, when they are not simply beside the exe.
 * Written after "locate files manually" so the pickers are a one-time cost
 * rather than something to sit through on every launch. */
#define PATHS_CFG "robox_paths.cfg"

static char s_dol_path[1024];
static char s_assets_path[1024];

/* robox_io.c's assets_root() already honours RECOMP_ASSETS, so pointing the
 * engine at an Assets tree elsewhere is one setenv rather than a base path
 * threaded through every fopen in the port. */
static void paths_apply(void)
{
    if (!s_assets_path[0]) return;
#if defined(_WIN32)
    _putenv_s("RECOMP_ASSETS", s_assets_path);
#else
    setenv("RECOMP_ASSETS", s_assets_path, 1);
#endif
    fprintf(stderr, "[setup] assets -> %s\n", s_assets_path);
}

static void paths_save(void)
{
    FILE *f = fopen(PATHS_CFG, "w");
    if (!f) {
        fprintf(stderr, "[setup] could not write %s\n", PATHS_CFG);
        return;
    }
    fprintf(f, "# Written by the Robox Recomp setup. Delete to run it again.\n");
    fprintf(f, "dol = %s\n", s_dol_path);
    fprintf(f, "assets = %s\n", s_assets_path);
    fclose(f);
    fprintf(stderr, "[setup] wrote %s\n", PATHS_CFG);
}

/* Returns 0 when the config named a DOL and an Assets tree that are both still
 * valid. A stale config (drive unplugged, folder moved) is not an error worth
 * stopping for -- it just means the setup runs again. */
static int paths_load(char *err, size_t err_size)
{
    FILE *f = fopen(PATHS_CFG, "r");
    if (!f) return -1;

    char line[1200];
    s_dol_path[0] = s_assets_path[0] = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\n' || *p == '\r' || !*p) continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = p, *val = eq + 1;

        for (char *e = key + strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t'); --e)
            e[-1] = 0;
        while (*val == ' ' || *val == '\t') ++val;
        for (char *e = val + strlen(val);
             e > val && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'); --e)
            e[-1] = 0;

        if (!strcmp(key, "dol"))    snprintf(s_dol_path,    sizeof s_dol_path,    "%s", val);
        else if (!strcmp(key, "assets")) snprintf(s_assets_path, sizeof s_assets_path, "%s", val);
    }
    fclose(f);

    if (!s_dol_path[0] || !s_assets_path[0]) {
        if (err && err_size) snprintf(err, err_size, "%s is incomplete", PATHS_CFG);
        goto stale;
    }
    if (robox_wad_check_dol(s_dol_path, err, err_size) != 0) goto stale;
    if (robox_wad_check_assets(s_assets_path, err, err_size) != 0) goto stale;

    paths_apply();
    return 0;

stale:
    s_dol_path[0] = s_assets_path[0] = 0;
    return -1;
}

const char *robox_setup_dol_path(void)
{
    return s_dol_path[0] ? s_dol_path : NULL;
}

/* ------------------------------------------------------------------------ */
/* finding things on disk                                                    */
/* ------------------------------------------------------------------------ */

static int ends_with_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    if (lx > ls) return 0;
    const char *p = s + ls - lx;
    for (size_t i = 0; i < lx; ++i) {
        char a = p[i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

/* Most people will just drop the WAD next to the exe, so look there before
 * asking for anything. First match wins; there is normally only one. */
static int find_wad_nearby(const char *dir, char *out, size_t out_size)
{
    DIR *d = opendir(dir && dir[0] ? dir : ".");
    if (!d) return -1;
    int found = -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (!ends_with_ci(e->d_name, ".wad")) continue;
        snprintf(out, out_size, "%s/%s", dir && dir[0] ? dir : ".", e->d_name);
        found = 0;
        break;
    }
    closedir(d);
    return found;
}

static int find_key_nearby(const char *dir, char *out, size_t out_size)
{
    static const char *names[] = { "key.bin", "common-key.bin" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        snprintf(out, out_size, "%s/%s", dir && dir[0] ? dir : ".", names[i]);
        FILE *f = fopen(out, "rb");
        if (f) { fclose(f); return 0; }
    }
    out[0] = 0;
    return -1;
}

/* ------------------------------------------------------------------------ */
/* the screen                                                                */
/* ------------------------------------------------------------------------ */

typedef enum {
    ST_MENU,        /* pick a WAD, or go looking for an existing install      */
    ST_LOCATE,      /* "locate files manually": waiting for a folder          */
    ST_NEED_KEY,    /* found a WAD, no key.bin -- ask for one                 */
    ST_WORKING,     /* extracting                                             */
    ST_ERROR,       /* something failed, message shown, any key returns       */
    ST_DONE         /* success beat, then hand off to the game                */
} setup_state;

typedef struct {
    setup_state state;
    const char *stage;
    int         pct;
    char        err[512];
    char        wad[1024];
    char        key[1024];
    char        note[256];      /* e.g. "modded DOL" after a good extract     */
    int         selection;      /* 0 = use WAD, 1 = locate manually           */
    int         locate_step;    /* ST_LOCATE: 0 = the DOL, 1 = Assets         */
    float       t;
    float       done_t;         /* seconds inside ST_DONE                     */
    float       fade;           /* 0..1 black over everything, for the handoff*/
    int         quit;
} setup_ctx;

static setup_ctx s_ui;

/* Called from inside robox_wad_extract on the same thread, so the window would
 * otherwise be frozen for the whole 32 MB. Draw a frame from here instead. */
static void draw_frame(void);

static int on_progress(const char *stage, int pct, void *user)
{
    (void)user;
    s_ui.stage = stage;
    s_ui.pct   = pct;

    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_QUIT) { s_ui.quit = 1; return 1; }

    draw_frame();
    return 0;
}

static void draw_frame(void)
{
    gx_ogl_overlay_begin();
    background_draw(s_ui.t);

    /* Identity, top left. The retail menu carries no title -- the scene is the
     * title -- so this stays a quiet slate rather than a banner, and leaves the
     * right-hand column as the only loud thing on screen. */
    gx_ogl_overlay_text(SLATE_X, SLATE_Y, "robox recomp",
                        FG_R, FG_G, FG_B, 0.95f, S_SLATE, 5.0f);
    gx_ogl_overlay_rect(SLATE_X, SLATE_Y + LINE(S_SLATE) + 10.0f, 210.0f, 1.0f,
                        FAINT_R, FAINT_G, FAINT_B, 0.55f);
    gx_ogl_overlay_text(SLATE_X, SLATE_Y + LINE(S_SLATE) + 22.0f, "first time setup",
                        DIM_R, DIM_G, DIM_B, 0.80f, S_BODY, TRACK_BODY);

    /* Wrapped status text stops short of the prompt column rather than running
     * the full width, or a long error slides underneath "esc quit". */
    const float status_w = 880.0f;
    float sy = STATUS_Y;

    switch (s_ui.state) {
    case ST_MENU:
        menu_item(MENU_Y0,
                  s_ui.wad[0] ? "install from wad" : "waiting for a wad",
                  s_ui.selection == 0, s_ui.wad[0] != 0);
        menu_item(MENU_Y0 + MENU_STEP, "locate files manually",
                  s_ui.selection == 1, 1);

        if (s_ui.wad[0]) {
            /* Label and value drawn separately rather than concatenated: the
             * path can be any length, and a snprintf here would silently eat
             * the end of the filename -- the one part worth reading. */
            gx_ogl_overlay_text(STATUS_X, sy, "found",
                                FAINT_R, FAINT_G, FAINT_B, 0.90f, S_BODY, TRACK_BODY);
            float lw = gx_ogl_overlay_text_width("found", S_BODY, TRACK_BODY);
            gx_ogl_overlay_text(STATUS_X + lw + 18.0f, sy, path_basename(s_ui.wad),
                                FG_R, FG_G, FG_B, 0.92f, S_BODY, TRACK_BODY);
        } else {
            gx_ogl_overlay_text(STATUS_X, sy, "no wad here yet",
                                DIM_R, DIM_G, DIM_B, 0.90f, S_BODY, TRACK_BODY);
        }
        gx_ogl_overlay_text(STATUS_X, sy + LINE(S_BODY) + 8.0f,
                            "drop a wad or key.bin on this window",
                            FAINT_R, FAINT_G, FAINT_B, 0.85f, S_SMALL, 1.0f);
        break;

    case ST_LOCATE:
        if (!HAVE_PICKERS) {
            menu_item(MENU_Y0, "check this folder again", 1, 1);
            gx_ogl_overlay_text(STATUS_X, sy,
                                "drop the folder holding Robox USA.dol and Assets",
                                FG_R, FG_G, FG_B, 0.92f, S_BODY, TRACK_BODY);
            break;
        }

        if (s_ui.locate_step == 0) {
            menu_item(MENU_Y0, "select the game file", 1, 1);
            gx_ogl_overlay_text(STATUS_X, sy, "step 1 of 2",
                                FAINT_R, FAINT_G, FAINT_B, 0.85f, S_SMALL, 1.0f);
            gx_ogl_overlay_text(STATUS_X, sy + LINE(S_SMALL) + 10.0f,
                                "the game's .dol file, whatever you named it",
                                FG_R, FG_G, FG_B, 0.92f, S_BODY, TRACK_BODY);
        } else {
            menu_item(MENU_Y0, "select the assets folder", 1, 1);
            gx_ogl_overlay_text(STATUS_X, sy, "step 2 of 2",
                                FAINT_R, FAINT_G, FAINT_B, 0.85f, S_SMALL, 1.0f);
            gx_ogl_overlay_text(STATUS_X, sy + LINE(S_SMALL) + 10.0f,
                                "the folder holding anim, lev, script and sound",
                                FG_R, FG_G, FG_B, 0.92f, S_BODY, TRACK_BODY);

            /* Confirm step 1 so it is obvious the first pick took. */
            gx_ogl_overlay_text(STATUS_X, sy - LINE(S_SMALL) - 14.0f, "game file",
                                FAINT_R, FAINT_G, FAINT_B, 0.85f, S_SMALL, 1.0f);
            float lw = gx_ogl_overlay_text_width("game file", S_SMALL, 1.0f);
            gx_ogl_overlay_text(STATUS_X + lw + 18.0f, sy - LINE(S_SMALL) - 14.0f,
                                path_basename(s_dol_path),
                                OK_R, OK_G, OK_B, 0.90f, S_SMALL, 1.0f);
        }

        /* The prompt says what Enter is about to do -- a file dialog opening
         * over a fullscreen game should never be a surprise. */
        text_wrap_left(STATUS_X, sy + LINE(S_SMALL) + LINE(S_BODY) + 20.0f,
                       s_ui.err[0] ? s_ui.err : "enter opens a file browser",
                       s_ui.err[0] ? BAD_R : FAINT_R,
                       s_ui.err[0] ? BAD_G : FAINT_G,
                       s_ui.err[0] ? BAD_B : FAINT_B,
                       0.88f, S_SMALL, 1.0f, status_w);
        break;

    case ST_NEED_KEY:
        menu_item(MENU_Y0, "check for key.bin again", 1, 1);
        gx_ogl_overlay_text(STATUS_X, sy, "this wad is encrypted and needs a key",
                            FG_R, FG_G, FG_B, 0.92f, S_BODY, TRACK_BODY);
        gx_ogl_overlay_text(STATUS_X, sy + LINE(S_BODY) + 8.0f,
                            "drop key.bin on this window, or put it beside the game",
                            FAINT_R, FAINT_G, FAINT_B, 0.85f, S_SMALL, 1.0f);
        break;

    case ST_WORKING:
        text_right(MENU_RIGHT, MENU_Y0, "installing",
                   FG_R, FG_G, FG_B, 1.0f, S_ITEM, TRACK_ITEM);
        progress_draw(STATUS_Y + 26.0f, s_ui.stage, s_ui.pct);
        break;

    case ST_ERROR:
        menu_item(MENU_Y0, "try again", 1, 1);
        sy = text_wrap_left(STATUS_X, sy - LINE(S_BODY), s_ui.err,
                            FG_R, FG_G, FG_B, 0.92f, S_BODY, TRACK_BODY, status_w);
        break;

    case ST_DONE: {
        /* Centre stage, not the menu column: the setup is over, so the layout
         * it lived in goes with it. Fades up over DONE_IN, drifting very
         * slightly larger as it arrives -- smoothstep rather than linear so it
         * does not start and stop abruptly at either end. */
        float in = s_ui.done_t / DONE_IN;
        if (in > 1.0f) in = 1.0f;
        float ease = in * in * (3.0f - 2.0f * in);

        float sc = S_ITEM * 2.4f * (0.95f + 0.05f * ease);
        text_centre(VW * 0.5f, VH * 0.5f - LINE(sc) * 0.5f, "done!",
                    1.0f, 1.0f, 1.0f, ease, sc, 10.0f);

        if (s_ui.note[0])
            text_centre(VW * 0.5f, VH * 0.5f + LINE(sc) * 0.5f + 26.0f, s_ui.note,
                        DIM_R, DIM_G, DIM_B, ease * 0.85f, S_SMALL, 1.0f);
        break;
    }
    }

    /* Prompts, bottom right, in the retail menu's order: back above confirm.
     * Neither state has anything left to press. */
    if (s_ui.state != ST_WORKING && s_ui.state != ST_DONE) {
        prompt_draw(PROMPT_Y, "esc",
                    s_ui.state == ST_MENU ? "quit" : "back");
        prompt_draw(PROMPT_Y + PROMPT_STEP, "enter", "confirm");
    }

    /* Last thing in the batch, so it covers everything: the hand-off to the
     * splash. Ending on a full black frame means the DOL load and the splash's
     * own black lead-in continue the same fade instead of cutting. */
    if (s_ui.fade > 0.0f)
        gx_ogl_overlay_rect(0.0f, 0.0f, VW, VH, 0.0f, 0.0f, 0.0f, s_ui.fade);

    gx_ogl_overlay_end();
    robox_gl_swap(g_window);
}

/* ------------------------------------------------------------------------ */

/* Write `data` to `path` unless something is already there. Never clobbering
 * is the whole point: those configs are settings, and a user who has turned mods
 * off or pointed a song at their own file must not lose that because the
 * setup ran again. */
static void write_if_absent(const char *path, const unsigned char *data,
                            unsigned len)
{
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return; }

    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[setup] could not create %s\n", path);
        return;
    }
    if (len && fwrite(data, 1, len, f) != len)
        fprintf(stderr, "[setup] short write on %s\n", path);
    fclose(f);
    fprintf(stderr, "[setup] wrote %s\n", path);
}

/* Lay down mods/ next to the install.
 *
 * robox_mods.c reads mods/mods.cfg at boot and, with no file there, comes up
 * with everything at its built-in default and no hint that a mods system
 * exists at all. Shipping the configs means a fresh install arrives with the
 * mod list documented and editable.
 *
 * mods/robox.dls cannot be shipped either -- it is derived from the game's own
 * robox.wt and robox.pcm -- but it does not have to be: both inputs come out
 * of the WAD, so it is built here instead (sdk/robox_dls.c, ~70 ms). That is
 * not a nicety. Without the DLS the music mod is forced off and the game falls
 * back to decoding the wavetable directly, which comes out wrong on the PC
 * port; the DLS is what makes the music sound right.
 *
 * mods/mario.chr is the one thing still absent by design: it is SMB1 character
 * data. The mario mod runs without it and only loses its F2 sprite swap. */
static void install_default_mods(const char *dir)
{
    char path[1024];

    snprintf(path, sizeof path, "%s/mods", dir);
    setup_mkdir(path);
    snprintf(path, sizeof path, "%s/mods/wavs", dir);
    setup_mkdir(path);

    snprintf(path, sizeof path, "%s/mods/mods.cfg", dir);
    write_if_absent(path, mods_cfg, mods_cfg_len);

    snprintf(path, sizeof path, "%s/mods/wav_music.cfg", dir);
    write_if_absent(path, wav_music_cfg, wav_music_cfg_len);

    /* No-op once it exists, so this is free on every launch after the first.
     * The Assets root has to be passed explicitly: after "locate files
     * manually" it is wherever the user pointed, not dir/Assets, and building
     * the wavetable path from dir alone silently found nothing -- the mod
     * loader then forced the music mod off and the game fell back to its own
     * broken wavetable decode. */
    char why[256];
    if (robox_dls_ensure(dir, s_assets_path[0] ? s_assets_path : NULL,
                         why, sizeof why) != 0)
        fprintf(stderr, "[setup] soundfont unavailable: %s "
                        "-- music mod will stay off\n", why);
}

/* "Locate files manually" is two steps, and each one is announced on screen
 * before its dialog opens rather than the pair firing off back to back. A
 * file picker appearing unannounced over a fullscreen game reads as the game
 * having done something odd; saying "step 1 of 2, the game executable" first
 * means the dialog is expected when it arrives.
 *
 * The DOL and the Assets tree are asked for separately because they need not
 * live together -- anyone who unpacked the game with an earlier tool may well
 * have them in different places.
 *
 * Both return 0 on success. A cancel is not a failure: it leaves the error
 * line alone and stays on the same step. */
static int locate_pick_dol(void)
{
    char dol[1024];

    if (!HAVE_PICKERS) {
        snprintf(s_ui.err, sizeof s_ui.err,
                 "drop the folder on this window instead");
        return -1;
    }
    if (pick_file("Select the game's .dol file", dol, sizeof dol) != 0)
        return -1;                       /* cancelled */
    if (robox_wad_check_dol(dol, s_ui.err, sizeof s_ui.err) != 0)
        return -1;

    snprintf(s_dol_path, sizeof s_dol_path, "%s", dol);
    return 0;
}

static int locate_pick_assets(void)
{
    char assets[1024];

    if (!HAVE_PICKERS) {
        snprintf(s_ui.err, sizeof s_ui.err,
                 "drop the folder on this window instead");
        return -1;
    }
    if (pick_folder("Select the Assets folder", assets, sizeof assets) != 0)
        return -1;                       /* cancelled */
    if (robox_wad_check_assets(assets, s_ui.err, sizeof s_ui.err) != 0)
        return -1;

    snprintf(s_assets_path, sizeof s_assets_path, "%s", assets);

    /* Only now is the pair worth remembering -- writing the config after the
     * first step would leave a half-finished one behind if the user backed
     * out of the second. */
    paths_apply();
    paths_save();
    return 0;
}

static void do_extract(const char *install_dir)
{
    unsigned char key[16];
    if (robox_wad_load_key(s_ui.key, key, s_ui.err, sizeof s_ui.err) != 0) {
        s_ui.state = ST_ERROR;
        return;
    }

    s_ui.state = ST_WORKING;
    s_ui.pct = 0;
    s_ui.stage = "starting";

    if (robox_wad_extract(s_ui.wad, install_dir, key, on_progress, NULL,
                          s_ui.err, sizeof s_ui.err) != 0) {
        s_ui.state = s_ui.quit ? ST_DONE : ST_ERROR;
        return;
    }

    /* Extraction succeeded. Say so if what came out is not what this recomp
     * was generated from -- it will still boot, and people mod this game
     * constantly, but a surprise is worse than a note. */
    char dol[1024], sha[41];
    snprintf(dol, sizeof dol, "%s/%s", install_dir, ROBOX_DOL_NAME);
    if (robox_wad_file_sha1(dol, sha) == 0 &&
        strcmp(sha, ROBOX_DOL_SHA1_USA) != 0) {
        snprintf(s_ui.note, sizeof s_ui.note,
                 "note: this is a modified DOL, not the retail one");
    }
    s_ui.state = ST_DONE;
}

int robox_setup_run(const char *install_dir)
{
    if (!install_dir || !install_dir[0]) install_dir = ".";

    char why[256];
    snprintf(why, sizeof why, "forced");

    /* RECOMP_FORCE_SETUP shows the screen even on a good install. Without it
     * there is no way to look at the setup once you have the game working,
     * which is every developer of this port and most of its modders. */
    const char *force = getenv("RECOMP_FORCE_SETUP");
    int forced = force && force[0] && force[0] != '0';

    /* Already playable: no window, no music, no delay. This is every launch
     * after the first, so it must cost nothing -- two stats and two mkdirs to
     * top up mods/ is the whole budget. That top-up matters: an install made
     * before mods/ was a thing would otherwise never get the configs, since
     * this path returns before the setup screen ever runs. */
    if (!forced) {
        /* Files chosen through "locate files manually" on a previous run. Try
         * this before the working directory: someone who deliberately pointed
         * the port somewhere else should not be silently overridden by a
         * stray DOL that happens to be sitting next to the exe. */
        if (paths_load(why, sizeof why) == 0) {
            install_default_mods(install_dir);
            return 0;
        }
        if (robox_wad_have_install(install_dir, why, sizeof why) == 0) {
            install_default_mods(install_dir);
            return 0;
        }
    }

    {
        const char *e = getenv("RECOMP_NO_SETUP");
        if (e && e[0] && e[0] != '0') {
            fprintf(stderr, "[setup] skipped by RECOMP_NO_SETUP (%s)\n", why);
            return 0;
        }
    }

    if (forced)
        fprintf(stderr, "[setup] forced by RECOMP_FORCE_SETUP -- running setup\n");
    else
        fprintf(stderr, "[setup] no install in '%s' (%s) -- running setup\n",
                install_dir, why);
    fflush(stderr);

    /* Runs fullscreen like the rest of the port. An earlier version dropped to
     * windowed here so a WAD could be dragged onto it -- you cannot drag a
     * file onto a window covering the screen -- but a setup that opens in a
     * small window when everything else is fullscreen looks broken, which is
     * worse than losing a convenience. The path that matters still works: a
     * WAD sitting beside the exe is found without any dragging at all, and
     * F11 is there for anyone who does want to drop one in. */
    memset(&s_ui, 0, sizeof s_ui);
    s_ui.state = ST_MENU;
    stars_init();
    audio_start();

    if (find_wad_nearby(install_dir, s_ui.wad, sizeof s_ui.wad) != 0)
        s_ui.wad[0] = 0;
    find_key_nearby(install_dir, s_ui.key, sizeof s_ui.key);

    Uint32 start = SDL_GetTicks();
    Uint32 done_at = 0;
    int rc = 1;

    for (;;) {
        s_ui.t = (float)(SDL_GetTicks() - start) / 1000.0f;

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { s_ui.quit = 1; goto finish; }

            if (e.type == SDL_DROPFILE) {
                char *dropped = e.drop.file;
                if (ends_with_ci(dropped, ".wad")) {
                    snprintf(s_ui.wad, sizeof s_ui.wad, "%s", dropped);
                    s_ui.selection = 0;
                    audio_sfx(&s_select);
                    if (s_ui.state == ST_ERROR) s_ui.state = ST_MENU;
                } else if (ends_with_ci(dropped, ".bin")) {
                    snprintf(s_ui.key, sizeof s_ui.key, "%s", dropped);
                    audio_sfx(&s_select);
                    if (s_ui.state == ST_NEED_KEY && s_ui.wad[0]) {
                        SDL_free(dropped);
                        do_extract(install_dir);
                        if (s_ui.state == ST_DONE) {
                            rc = 0;
                            done_at = SDL_GetTicks();
                        }
                        continue;
                    }
                } else {
                    /* A folder: an existing install somewhere else on disk. */
                    if (robox_wad_have_install(dropped, why, sizeof why) == 0) {
                        if (setup_chdir(dropped) == 0) {
                            fprintf(stderr, "[setup] using install at '%s'\n", dropped);
                            audio_sfx(&s_select);
                            SDL_free(dropped);
                            rc = 0;
                            s_ui.state = ST_DONE;
                            done_at = SDL_GetTicks();
                            continue;
                        }
                        snprintf(s_ui.err, sizeof s_ui.err,
                                 "cannot open that folder");
                    } else {
                        snprintf(s_ui.err, sizeof s_ui.err, "%s", why);
                    }
                    s_ui.state = ST_LOCATE;
                    s_ui.locate_step = 0;
                    audio_sfx(&s_back);
                }
                SDL_free(dropped);
                continue;
            }

            if (e.type != SDL_KEYDOWN) continue;
            SDL_Keycode k = e.key.keysym.sym;

            if (k == SDLK_ESCAPE) {
                if (s_ui.state == ST_MENU) { s_ui.quit = 1; goto finish; }
                audio_sfx(&s_back);
                /* Back one step within locate, rather than all the way out --
                 * getting the wrong DOL should not cost you the whole flow. */
                if (s_ui.state == ST_LOCATE && s_ui.locate_step > 0) {
                    s_ui.locate_step = 0;
                    s_ui.err[0] = 0;
                } else {
                    s_ui.state = ST_MENU;
                }
                continue;
            }

            if (s_ui.state == ST_MENU) {
                if (k == SDLK_UP || k == SDLK_DOWN) {
                    s_ui.selection ^= 1;
                    audio_sfx(&s_move);
                } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    if (s_ui.selection == 1) {
                        /* Into the locate screen, which explains step 1 and
                         * waits. Opening a dialog straight off this keypress
                         * would put a file browser on screen before anything
                         * had said one was coming. */
                        audio_sfx(&s_select);
                        s_ui.err[0] = 0;
                        s_ui.locate_step = 0;
                        s_ui.state = ST_LOCATE;
                    } else if (s_ui.wad[0]) {
                        audio_sfx(&s_select);
                        if (!s_ui.key[0] &&
                            find_key_nearby(install_dir, s_ui.key, sizeof s_ui.key) != 0) {
                            s_ui.state = ST_NEED_KEY;
                        } else {
                            do_extract(install_dir);
                            if (s_ui.state == ST_DONE) {
                                rc = 0;
                                done_at = SDL_GetTicks();
                            }
                        }
                    } else {
                        audio_sfx(&s_back);
                    }
                }
            } else if (s_ui.state == ST_LOCATE) {
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    audio_sfx(&s_select);
                    s_ui.err[0] = 0;

                    if (!HAVE_PICKERS) {
                        /* No native dialogs here; the entry re-checks the
                         * working directory instead. */
                        if (robox_wad_have_install(install_dir, why, sizeof why) == 0) {
                            rc = 0;
                            s_ui.state = ST_DONE;
                            done_at = SDL_GetTicks();
                        } else {
                            snprintf(s_ui.err, sizeof s_ui.err, "%s", why);
                            audio_sfx(&s_back);
                        }
                    } else if (s_ui.locate_step == 0) {
                        if (locate_pick_dol() == 0) s_ui.locate_step = 1;
                        else                        audio_sfx(&s_back);
                    } else {
                        if (locate_pick_assets() == 0) {
                            rc = 0;
                            s_ui.state = ST_DONE;
                            done_at = SDL_GetTicks();
                        } else {
                            audio_sfx(&s_back);
                        }
                    }
                }
            } else if (s_ui.state == ST_NEED_KEY) {
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    if (find_key_nearby(install_dir, s_ui.key, sizeof s_ui.key) == 0) {
                        audio_sfx(&s_select);
                        do_extract(install_dir);
                        if (s_ui.state == ST_DONE) {
                            rc = 0;
                            done_at = SDL_GetTicks();
                        }
                    } else {
                        audio_sfx(&s_back);
                    }
                }
            } else if (s_ui.state == ST_ERROR) {
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    audio_sfx(&s_select);
                    s_ui.state = ST_MENU;
                }
            }
        }

        if (s_ui.quit) goto finish;

        draw_frame();
        frame_limiter();

        /* "done!" holds for a beat so the success registers, then the picture
         * and the music fade out together and the splash takes over. Without
         * the hold the screen vanished the instant the last file landed, which
         * read as a crash rather than a finish. */
        if (s_ui.state == ST_DONE && done_at) {
            Uint32 el = SDL_GetTicks() - done_at;
            s_ui.done_t = (float)el / 1000.0f;

            if (el > DONE_HOLD_MS) {
                float f = (float)(el - DONE_HOLD_MS) / (float)DONE_OUT_MS;
                if (f > 1.0f) f = 1.0f;
                /* Same value drives both, so the picture and the music land on
                 * silence and black together. */
                s_ui.fade    = f;
                s_music_gain = 1.0f - f;
            }
            if (el > DONE_HOLD_MS + DONE_OUT_MS) break;
        }
    }

finish:
    /* One call site for every success path -- extract, locate, or a dropped
     * folder. After a chdir the install is the working directory, so "." is
     * right in all three cases. */
    if (rc == 0) install_default_mods(".");

    audio_stop();
    fprintf(stderr, "[setup] %s\n", rc == 0 ? "install ready" : "cancelled");
    fflush(stderr);
    return rc;
}

#endif /* desktop only */
