#if !defined(__3DS__)  /* PICA200 has no OpenGL; sdk/gx_c3d.c + sdk/platform_3ds.c stand in */
// sdk/robox_splash.c -- the "Eggscantfly / Recompiled By" splash animation.
//
// This is a direct port of build/Assets/generate_mp4.py, which is the
// authoritative version of the animation (its web twin, index.html, expresses
// the same timeline in CSS keyframes). The port replays it rather than playing
// the rendered MP4 for three reasons: an MP4 would need an H.264 decoder on
// Windows and a separate path in the browser; the still images plus this file
// are far smaller than the video; and rendering live means it comes out at the
// window's real resolution instead of upscaled from a baked 1080p.
//
// Every constant below -- timings, scales, rotations, positions -- is taken
// from make_frame() in the generator, so the two stay comparable frame for
// frame. Design space is 1920x1080; gx_ogl_splash_quad maps that to the window.
//
// Runs after video_init() and before the guest entry, so it owns the GL context
// and the audio device outright: the game has not started yet.

#include "hle.h"
#include "gx_ogl.h"

#include <SDL2/SDL.h>
#if defined(__EMSCRIPTEN__)
#  include <emscripten/threading.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The splash's own sound, baked in by tools/bin2c.py -- splash/ holds the
 * port's artwork rather than the game's, so it is absent on a fresh install
 * and cannot be read off disk. See splash_load_one in gx_ogl.c. */
#include "../src/generated_setup/splash_slap_rbxs.h"
#include "../src/generated_setup/splash_bass_rbxs.h"

/* Lives in robox_wav.c's translation unit; declared rather than included so a
 * second copy of the decoder does not land in the link. */
extern int stb_vorbis_decode_memory(const unsigned char *mem, int len,
                                    int *channels, int *sample_rate,
                                    short **output);

int  gx_ogl_splash_load(void);
void gx_ogl_splash_begin(void);
void gx_ogl_splash_quad(int which, float cx, float cy,
                        float scale, float rot_deg, float alpha);
void gx_ogl_splash_end(void);
void gx_ogl_splash_free(void);

/* Image slots, in the order gx_ogl_splash_load() fills them. */
enum { IMG_LOGO = 0, IMG_BY = 1, IMG_NAME = 2 };

/* Layout from the generator: logo occupies y=196..628 (height 432), then a
 * 20 px gap before each following line (108 tall). */
#define Y_LOGO_TOP   196.0f
#define H_LOGO       432.0f
#define H_LINE       108.0f
#define GAP           20.0f
#define CX           960.0f
#define CY_LOGO      (Y_LOGO_TOP + H_LOGO * 0.5f)                    /* 412 */
#define Y_BY_TOP     (Y_LOGO_TOP + H_LOGO + GAP)                     /* 648 */
#define CY_BY        (Y_BY_TOP + H_LINE * 0.5f)                      /* 702 */
#define Y_NAME_TOP   (Y_BY_TOP + H_LINE + GAP)                       /* 776 */
#define CY_NAME      (Y_NAME_TOP + H_LINE * 0.5f)                    /* 830 */

#define SPLASH_DURATION 7.5f

/* Black hold before the animation starts. Without it the slap and the logo
 * slam land on the very first frame the window exists, which reads as the
 * splash being already half over by the time you are looking at it. A beat of
 * black first gives the hit somewhere to land. */
#define SPLASH_LEAD_IN 0.5f

/* The "slam": scale 5 -> 0.8 -> 1.1 -> 1.0 across a 0.5 s window, exactly as
 * the generator piecewise-interpolates it. */
static float slam_scale(float progress)
{
    if (progress < 0.4f) return 5.0f - 4.2f * (progress / 0.4f);
    if (progress < 0.7f) return 0.8f + 0.3f * ((progress - 0.4f) / 0.3f);
    return 1.1f - 0.1f * ((progress - 0.7f) / 0.3f);
}

/* The logo lands crooked at -20 deg and is straightened between t=0.5 and
 * t=1.0, overshooting to +10 then -5 with a sine ease on each leg. */
static float fix_rotation(float progress)
{
    const float HALF_PI = 1.57079632679f;
    if (progress < 0.4f) return -20.0f + 30.0f * sinf((progress / 0.4f) * HALF_PI);
    if (progress < 0.7f) return  10.0f - 15.0f * sinf(((progress - 0.4f) / 0.3f) * HALF_PI);
    return -5.0f + 5.0f * sinf(((progress - 0.7f) / 0.3f) * HALF_PI);
}

static SDL_AudioDeviceID s_audio;
static SDL_AudioSpec     s_audio_spec;

/* Both clips are 48 kHz stereo s16 and never overlap (the slap ends at 1.7 s,
 * the bass starts at 2.8), so one device and one queue play the whole
 * soundtrack in order. */
static void splash_open_audio(void)
{
    if (s_audio) return;
    s_audio = SDL_OpenAudioDevice(NULL, 0, &s_audio_spec, NULL, 0);
    if (s_audio) SDL_PauseAudioDevice(s_audio, 0);
    else fprintf(stderr, "[splash] audio open failed: %s\n", SDL_GetError());
}

/* Both clips are decoded and the device opened during the black lead-in, never
 * at a cue. Doing it at t=0 meant a 329 KB SDL_LoadWAV plus an
 * SDL_OpenAudioDevice -- which on the web is a blocking round-trip to the main
 * thread -- landed on the exact frame the logo slams, and the animation visibly
 * hitched there. At the cue all that is left is a queue. */
static Uint8 *s_wav_buf[2];
static Uint32 s_wav_len[2];

static void splash_preload(void)
{
    const unsigned char *blob[2]  = { splash_slap_rbxs, splash_bass_rbxs };
    unsigned             blen[2]  = { splash_slap_rbxs_len, splash_bass_rbxs_len };
    static const char   *what[2]  = { "slap", "bass" };

    for (int i = 0; i < 2; ++i) {
        SDL_AudioSpec spec;
        const unsigned char *d = blob[i];
        unsigned n = blen[i];

        /* RBXS header, then OGG. Both clips are play-once, so the loop fields
         * are ignored here -- the splash runs one pass and exits. */
        if (n < 24 || memcmp(d, "RBXS", 4) != 0 || memcmp(d + 0x0E, ":P", 2) != 0) {
            fprintf(stderr, "[splash] %s clip is not RBXS\n", what[i]);
            s_wav_buf[i] = NULL;
            continue;
        }

        short *pcm = NULL;
        int ch = 0, rate = 0;
        int frames = stb_vorbis_decode_memory(d + 24, (int)(n - 24),
                                              &ch, &rate, &pcm);
        if (frames <= 0 || !pcm) {
            fprintf(stderr, "[splash] %s clip failed to decode\n", what[i]);
            s_wav_buf[i] = NULL;
            continue;
        }

        s_wav_buf[i] = (Uint8 *)pcm;
        s_wav_len[i] = (Uint32)((size_t)frames * (size_t)ch * sizeof(short));

        SDL_zero(spec);
        spec.freq     = rate;
        spec.format   = AUDIO_S16SYS;
        spec.channels = (Uint8)ch;
        spec.samples  = 1024;

        if (!s_audio) {
            s_audio_spec = spec;
#if defined(__EMSCRIPTEN__)
        /* Must happen on the main thread: emscripten's SDL audio backend
         * reaches for the page's AudioContext, which does not exist on this
         * worker -- opening it here threw
         * "Cannot read properties of undefined (reading 'audioContext')"
         * and killed the guest thread outright. Same treatment audio_init()
         * gives the game's own device. */
            emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_V, splash_open_audio);
#else
            splash_open_audio();
#endif
        }
    }
}

/* Cue: queue an already-decoded clip. Cheap enough to sit on any frame. */
static void splash_sound(int which)
{
    if (s_audio && s_wav_buf[which])
        SDL_QueueAudio(s_audio, s_wav_buf[which], s_wav_len[which]);
}

static void splash_free_audio(void)
{
    /* free(), not SDL_FreeWAV: these buffers come from stb_vorbis now, which
     * allocates with malloc. Handing them to SDL's allocator would be freeing
     * on the wrong heap. */
    for (int i = 0; i < 2; ++i)
        if (s_wav_buf[i]) { free(s_wav_buf[i]); s_wav_buf[i] = NULL; }
}

/* Draw one frame of the animation at time t (seconds).
 * t < 0 is the lead-in: clear to black and present nothing. */
static void splash_frame(float t)
{
    if (t < 0.0f) { gx_ogl_splash_begin(); gx_ogl_splash_end(); return; }

    /* Everything fades out together over the last 1.5 s. */
    float global = 1.0f;
    if (t > 6.0f) {
        global = 1.0f - (t - 6.0f) / 1.5f;
        if (global < 0.0f) global = 0.0f;
    }
    gx_ogl_splash_begin();
    if (global <= 0.0f) { gx_ogl_splash_end(); return; }

    /* Logo: slams in crooked over 0-0.5 s, straightens over 0.5-1.0 s. */
    {
        float scale = 1.0f, rot = 0.0f, opacity = 1.0f;
        if (t < 0.5f) {
            float p = t / 0.5f;
            opacity = p * 4.0f; if (opacity > 1.0f) opacity = 1.0f;
            scale   = slam_scale(p);
            rot     = -20.0f;
        } else if (t < 1.0f) {
            rot = fix_rotation((t - 0.5f) / 0.5f);
        }
        /* Negated because the references express this as CSS rotate(-20deg) /
         * PIL rotate(+20), i.e. counter-clockwise on screen. */
        gx_ogl_splash_quad(IMG_LOGO, CX, CY_LOGO, scale, -rot, global * opacity);
    }

    /* "Recompiled By": fades in over 1.2 s starting at t=1.4. */
    if (t > 1.4f) {
        float o = (t - 1.4f) / 1.2f; if (o > 1.0f) o = 1.0f;
        gx_ogl_splash_quad(IMG_BY, CX, CY_BY, 1.0f, 0.0f, global * o);
    }

    /* Name: slams in on the bass hit at t=2.8. */
    if (t > 2.8f) {
        float p = (t - 2.8f) / 0.5f; if (p > 1.0f) p = 1.0f;
        float o = p * 4.0f; if (o > 1.0f) o = 1.0f;
        gx_ogl_splash_quad(IMG_NAME, CX, CY_NAME, slam_scale(p), 0.0f, global * o);
    }

    gx_ogl_splash_end();
}

/* Blocking: plays the whole animation, then returns so boot continues.
 * Any button or key skips it, as does RECOMP_NO_SPLASH=1. */
void robox_splash_play(void)
{
    {
        const char *e = getenv("RECOMP_NO_SPLASH");
        if (e && e[0] && e[0] != '0') return;
    }
    if (!gx_ogl_splash_load()) {
        fprintf(stderr, "[splash] assets unavailable -- skipping\n");
        fflush(stderr);
        return;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        fprintf(stderr, "[splash] no audio subsystem: %s\n", SDL_GetError());

    fprintf(stderr, "[splash] playing (%.1fs black + %.1fs animation, any key skips)\n",
            SPLASH_LEAD_IN, SPLASH_DURATION);
    fflush(stderr);

    splash_preload();          /* decode + open device before any cue */

    Uint32 start = SDL_GetTicks();
    int played_slap = 0, played_bass = 0;

    for (;;) {
        /* Animation time: negative during the lead-in, so every cue below
         * stays on the generator's original timeline. */
        float t = (float)(SDL_GetTicks() - start) / 1000.0f - SPLASH_LEAD_IN;
        if (t >= SPLASH_DURATION) break;

        if (!played_slap && t >= 0.0f) { played_slap = 1; splash_sound(0); }
        if (!played_bass && t >= 2.8f) { played_bass = 1; splash_sound(1); }

        /* Pumped here rather than through the game's handler: the guest is not
         * running yet, so video.c's event loop is not being serviced. */
        /* Drained, but no longer a skip. The splash is the port's own credit
         * and it runs once per launch; letting any keypress kill it meant it
         * usually died to a stray key before anyone read it. Events are still
         * pumped so the window stays responsive and a close still works. */
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) exit(0);
        }

        splash_frame(t);

        /* Paced by the game's own limiter rather than SDL_Delay. SDL_Delay's
         * granularity on Windows is coarse enough (and unaligned to the
         * display) that frames landed at irregular intervals -- the animation
         * maths is time-based so nothing drifted, but it read as stutter.
         * frame_limiter() holds an absolute 59.94 Hz deadline, which is what
         * the game itself runs on. */
        extern void frame_limiter(void);
        frame_limiter();
    }

    splash_free_audio();
    if (s_audio) {
        SDL_ClearQueuedAudio(s_audio);
        SDL_CloseAudioDevice(s_audio);
        s_audio = 0;
    }
    gx_ogl_splash_free();
    fprintf(stderr, "[splash] done\n");
    fflush(stderr);
}

#endif /* !__3DS__ */
