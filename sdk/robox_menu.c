#if !defined(__3DS__)  /* PICA200 has no OpenGL; sdk/gx_c3d.c + sdk/platform_3ds.c stand in */
/* ---------------------------------------------------------------------------
 * In-game settings menu (Esc, or backquote where a browser eats Esc).
 *
 * Drawn with the port's own overlay renderer (gx_ogl_overlay_*) in the game's
 * own font, not a UI toolkit -- see tools/extract_brfna.py. Layout is in the
 * overlay's virtual 1280x720 space, so it is correct at any window size. Do
 * NOT lay out in window pixels: on web SDL reports a stale canvas size.
 *
 * Four tabs. Everything here drives something real: no row is a placeholder.
 *   video     g_show_fps, g_web_present_diag
 *   audio     g_audio_volume, g_music_on   (sdk/peripherals.c mixer)
 *   controls  g_binds via controls_*()     (sdk/video.c)
 *   debug     live CPlayer fields, and an invincibility pin
 *
 * The debug tab reads the player through the pointer at 0x801FACE0 with the
 * field offsets from the project's Dolphin watch list (Robox.dmw). Every read
 * is bounds-checked: the pointer is null for most of the boot sequence and
 * whenever no level is loaded, and a menu must not be able to fault the game.
 * ------------------------------------------------------------------------- */

#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "gx_ogl.h"
#include "../src/runtime.h"     /* MEM_R32 / MEM_RF / MEM_WF */

/* Owned by sdk/video.c. */
extern SDL_Keycode controls_get_binding(unsigned int bit);
extern void        controls_set_binding(unsigned int bit, SDL_Keycode k);
extern int         controls_save(void);
extern void        controls_reset_defaults(void);
extern int         g_show_fps;
extern int         g_web_present_diag;
extern int         video_vsync_state(void);  /* 0 off, 1 on, -1 adaptive */
extern int         gx_ogl_get_internal_scale(void);        /* 1..4 */
extern void        gx_ogl_set_internal_scale(int scale);
extern uint32_t    video_input_hold(void);   /* after robot remap */
extern uint32_t    video_input_raw(void);    /* before it */
extern int         g_sideways_mode;          /* 0 off, 1 auto, 2/3/4 = 90/180/270 */
extern unsigned    video_sideways_q(void);
extern int         video_input_p1_tilt(void);
extern const char *robox_level_name(void);
extern int         robox_level_is_robot(void);
extern int         robox_level_id(void);

/* Same flag robox_coop.c rotates on. Shown live in the debug tab because the
 * robot-section remap depends entirely on it, and its polarity and timing are
 * the whole question when the controls feel wrong. */
#define ROBOT_FLAG_VA   0x8020876Cu

/* Owned by sdk/peripherals.c. Defined here so there is exactly one home for
 * the menu's settings state. */
int g_audio_volume = 100;   /* 0..100 */
int g_music_on     = 1;

/* --- the player object ---------------------------------------------------
 * Offsets are from Robox.dmw (the Dolphin watch list kept with the project).
 * 0x801FACE0 holds a POINTER to CPlayer; it is null until a level is up. */
#define PLAYER_PTR_VA   0x801FACE0u
#define OFF_X           0x038u   /* float */
#define OFF_Y           0x03Cu   /* float */
#define OFF_HEALTH      0x228u   /* int   */
#define OFF_STATE       0x240u   /* int   */
#define OFF_IFRAMES     0x268u   /* float */
#define OFF_ANIM        0x394u   /* int   */

static int va_ok(uint32_t va) { return va >= 0x80003000u && va < 0x94000000u; }

static uint32_t player_obj(void) {
    if (!va_ok(PLAYER_PTR_VA)) return 0;
    uint32_t p = MEM_R32(PLAYER_PTR_VA);
    return va_ok(p) ? p : 0;
}

/* --- rows ---------------------------------------------------------------- */

typedef enum { TAB_VIDEO, TAB_AUDIO, TAB_CONTROLS, TAB_DEBUG, TAB_COUNT } tab_t;
static const char *TAB_NAME[TAB_COUNT] = { "video", "audio", "controls", "debug" };

/* The Wii buttons the config format understands. `bit` is the WPAD mask
 * control_name_to_bit() produces. */
static const struct { const char *label; unsigned int bit; } BINDS[] = {
    { "up",           0x0008     }, { "down",         0x0004     },
    { "left",         0x0001     }, { "right",        0x0002     },
    { "jump",         0x0100     }, { "action",       0x0200     },
    { "plus",         0x0010     },
    /* Not WPAD buttons -- these drive host-side flags through the virtual
     * action bits in sdk/video.c. */
    { "tilt left",    0x00010000 }, { "tilt right",   0x00020000 },
    { "shake",        0x00040000 },
    /* No robot-mode rows: the quarter-turn the game applies inside the robot
     * is undone in video_input_hold(), so these same four directions already
     * do the right thing there. Asking the player to bind a second set would
     * be exposing our own workaround as a setting. */
};
#define BIND_COUNT ((int)(sizeof BINDS / sizeof BINDS[0]))

/* Shown when a button has no explicit binding, mirroring the fallback switch
 * in key_to_wpad() and the hardcoded keys in the event loop. The robot set has
 * no default at all -- unbound means "use the normal directions", which is the
 * behaviour that existed before. */
static const char *BIND_DEFAULT[BIND_COUNT] = {
    "Up", "Down", "Left", "Right", "X", "Z", "Return",
    "N", "M", "C"
};

#define VIDEO_ROWS 4
#define AUDIO_ROWS 2
#define DEBUG_ROWS 12

static int tab_rows(tab_t t) {
    switch (t) {
        case TAB_VIDEO:    return VIDEO_ROWS;
        case TAB_AUDIO:    return AUDIO_ROWS;
        case TAB_CONTROLS: return BIND_COUNT;
        default:           return DEBUG_ROWS;
    }
}

/* Only rows the player can act on; the debug readouts are display-only. */
static int row_actionable(tab_t t, int row) {
    return !(t == TAB_DEBUG && row > 1);
}

static int    g_open;
static tab_t  g_tab;
static int    g_sel;
static int    g_capturing;      /* waiting for the player to press a key */
static int    g_invincible;
static float  g_anim;           /* 0..1 fade-in, purely cosmetic */
static float  g_tab_anim = 1.0f;/* 0..1 tab-change transition */
static int    g_tab_prev;       /* tab the underline is sliding FROM */
static int    g_tab_dir;        /* -1 left, +1 right: which way rows slide in */
static char   g_notice[64];
static double g_notice_until;

/* The game's own menus are white lowercase text, widely letterspaced, framed
 * by a hairline rectangle when selected, sitting straight on the artwork with
 * no filled panel. Matching that idiom is the point -- a dark box with a
 * coloured accent bar reads as a dev tool bolted onto someone else's game.
 * The only saturated colour is amber, borrowed from the warning lights on the
 * game's own robots. */
#define COL_SCRIM 0.00f, 0.00f, 0.00f
#define COL_ON    1.00f, 1.00f, 1.00f
#define COL_OFF   0.62f, 0.65f, 0.68f
#define COL_FAINT 0.42f, 0.45f, 0.48f
#define COL_AMBER 1.00f, 0.62f, 0.18f

#define COL_X       330.0f
#define COL_W       620.0f
#define ROW_H        30.0f
#define ROW_TOP     246.0f
#define TRACK        1.5f
#define TRACK_HEAD   5.0f
#define S_TITLE      1.25f
#define S_TAB        0.66f
#define S_ROW        0.66f
#define S_FOOT       0.50f

int robox_menu_is_open(void) { return g_open; }

void robox_menu_toggle(void) {
    g_open = !g_open;
    g_capturing = 0;
    /* Only reset on the way IN. On the way out g_anim is the fade-out's
     * starting point, so zeroing it here would make the menu vanish instantly
     * -- which is exactly the asymmetry that made closing feel abrupt next to
     * opening. */
    if (g_open) { g_anim = 0.0f; g_sel = 0; }
}

/* --- three-phase field diff, driven by F8 ---------------------------------
 *
 * Built to find the "am I in a robot" signal, which is now answered properly
 * by the game's own current-level word (see robox_level_is_robot). Kept
 * because the technique is the generic one for pinning down any CPlayer field:
 * it is how the next unknown gets found too.
 *
 *   press 1, standing OUTSIDE  -> capture A
 *   press 2, still OUTSIDE     -> capture B, learn which fields are just noisy
 *                                 (position, timers, animation counters)
 *   press 3, INSIDE the robot  -> report fields that differ from A but were
 *                                 stable across A/B
 *
 * The middle step is what makes it usable: a plain before/after diff of a live
 * game object is dozens of fields and tells you nothing. Subtracting the ones
 * that move on their own leaves a handful of candidates. */
#define SNAP_WORDS 0x100u          /* 1 KB; CPlayer fields of interest end ~0x394 */
static uint32_t g_snap_a[SNAP_WORDS];
static uint8_t  g_snap_noisy[SNAP_WORDS];
static int      g_snap_phase;      /* 0 none, 1 have A, 2 have noise map */
static int      g_snap_found;

void robox_menu_snapshot(void) {
    uint32_t o = player_obj();
    if (!o) { fprintf(stderr, "[snap] no player object\n"); fflush(stderr); return; }

    if (g_snap_phase == 0) {
        for (unsigned i = 0; i < SNAP_WORDS; ++i) g_snap_a[i] = MEM_R32(o + i * 4u);
        memset(g_snap_noisy, 0, sizeof g_snap_noisy);
        g_snap_phase = 1; g_snap_found = -1;
        fprintf(stderr, "[snap] A captured. Stay OUTSIDE, wait a moment, press F8 again.\n");
    } else if (g_snap_phase == 1) {
        unsigned n = 0;
        for (unsigned i = 0; i < SNAP_WORDS; ++i)
            if (MEM_R32(o + i * 4u) != g_snap_a[i]) { g_snap_noisy[i] = 1; ++n; }
        g_snap_phase = 2;
        fprintf(stderr, "[snap] %u fields move on their own; ignoring them. "
                        "Now go INSIDE the robot and press F8.\n", n);
    } else {
        unsigned n = 0;
        fprintf(stderr, "[snap] candidates for the robot flag:\n");
        for (unsigned i = 0; i < SNAP_WORDS; ++i) {
            if (g_snap_noisy[i]) continue;
            uint32_t now = MEM_R32(o + i * 4u);
            if (now == g_snap_a[i]) continue;
            if (n < 24) {
                /* Both interpretations: the flag could be an int or a float,
                 * and guessing wrong once already cost a round of this. */
                float fa, fb;
                memcpy(&fa, &g_snap_a[i], 4);
                memcpy(&fb, &now, 4);
                fprintf(stderr, "    +0x%03x  %08x -> %08x   int %d -> %d   float %.3f -> %.3f\n",
                        i * 4u, g_snap_a[i], now,
                        (int)g_snap_a[i], (int)now, (double)fa, (double)fb);
            }
            ++n;
        }
        fprintf(stderr, "[snap] %u candidate field(s). Press F8 to start over.\n", n);
        g_snap_found = (int)n;
        g_snap_phase = 0;
    }
    fflush(stderr);
}

int robox_menu_snap_phase(void) { return g_snap_phase; }
int robox_menu_snap_found(void) { return g_snap_found; }

static void notice(const char *s) {
    snprintf(g_notice, sizeof g_notice, "%s", s);
    g_notice_until = (double)SDL_GetTicks() + 2000.0;
}

/* Runs every host frame from video_present(), open or not: invincibility has
 * to keep holding once the menu is closed, which is the whole point of it. */
void robox_menu_tick(void) {
    if (!g_invincible) return;
    uint32_t obj = player_obj();
    if (!obj) return;
    /* The game counts this down; parking it high means it never reaches the
     * "can be hit" state. Re-written every frame rather than once, because the
     * game reloads the field on respawn and level change. */
    MEM_WF(obj + OFF_IFRAMES, 600.0f);
}

static void activate(void) {
    switch (g_tab) {
        case TAB_VIDEO:
            if      (g_sel == 0) { g_show_fps = !g_show_fps; }
            else if (g_sel == 1) { extern void video_toggle_vsync(void);
                                   video_toggle_vsync(); }
            else if (g_sel == 2) { /* cycle native -> 2x -> 3x -> 4x -> native */
                                   int s = gx_ogl_get_internal_scale() + 1;
                                   if (s > 4) s = 1;
                                   gx_ogl_set_internal_scale(s); }
            else                 { g_web_present_diag = !g_web_present_diag; }
            break;
        case TAB_AUDIO:
            if (g_sel == 0) {
                /* Cycles rather than sliding: left/right already means "change
                 * tab", and a five-step cycle is less fiddly than a slider on
                 * a d-pad anyway. */
                g_audio_volume = (g_audio_volume >= 100) ? 0 : g_audio_volume + 25;
            } else {
                g_music_on = !g_music_on;
            }
            break;
        case TAB_CONTROLS:
            g_capturing = 1;
            break;
        default:
            if (g_sel == 0) {
                g_invincible = !g_invincible;
                notice(g_invincible ? "invincible" : "mortal");
            } else if (g_sel == 1) {
                g_sideways_mode = (g_sideways_mode + 1) & 3;
            }
            break;
    }
}

/* Returns 1 if the key was consumed by the menu and must not reach the game. */
int robox_menu_key(SDL_Keycode k) {
    if (!g_open) return 0;

    if (g_capturing) {
        /* Escape cancels rather than binding itself -- otherwise the player can
         * bind the one key they need to get back out. */
        if (k == SDLK_ESCAPE) { g_capturing = 0; notice("cancelled"); return 1; }
        controls_set_binding(BINDS[g_sel].bit, k);
        g_capturing = 0;
        notice(controls_save() ? "saved" : "could not save");
        return 1;
    }

    switch (k) {
        case SDLK_ESCAPE:
            g_open = 0;
            return 1;
        case SDLK_LEFT:
            g_tab_prev = (int)g_tab; g_tab_anim = 0.0f; g_tab_dir = -1;
            g_tab = (tab_t)((g_tab + TAB_COUNT - 1) % TAB_COUNT); g_sel = 0; return 1;
        case SDLK_RIGHT:
            g_tab_prev = (int)g_tab; g_tab_anim = 0.0f; g_tab_dir = +1;
            g_tab = (tab_t)((g_tab + 1) % TAB_COUNT);             g_sel = 0; return 1;
        case SDLK_UP: {
            int n = tab_rows(g_tab);
            do { g_sel = (g_sel + n - 1) % n; } while (!row_actionable(g_tab, g_sel));
            return 1;
        }
        case SDLK_DOWN: {
            int n = tab_rows(g_tab);
            do { g_sel = (g_sel + 1) % n; } while (!row_actionable(g_tab, g_sel));
            return 1;
        }
        case SDLK_RETURN:
        case SDLK_SPACE:
            activate();
            return 1;
        case SDLK_F9:
            if (g_tab == TAB_CONTROLS) {
                controls_reset_defaults();
                notice(controls_save() ? "reset" : "could not save");
            }
            return 1;
        default:
            /* Swallow everything while the menu is up: a stray keypress should
             * not steer the player behind the panel. */
            return 1;
    }
}

/* --- rendering ----------------------------------------------------------- */

static void row_text(tab_t t, int row, char *label, size_t lcap,
                     char *value, size_t vcap) {
    label[0] = value[0] = 0;
    switch (t) {
        case TAB_VIDEO:
            if (row == 0) { snprintf(label, lcap, "fps counter");
                            snprintf(value, vcap, "%s", g_show_fps ? "on" : "off"); }
            else if (row == 1) {
                /* The game is locked to 60 steps/sec; this only changes whether
                 * that 60 is shown tear-free (on) or evenly paced (off). On a
                 * refresh that is not a multiple of 60, off usually looks
                 * smoother. */
                int v = video_vsync_state();
                snprintf(label, lcap, "vsync");
                snprintf(value, vcap, "%s", v ? (v < 0 ? "adaptive" : "on") : "off (even pacing)");
            }
            else if (row == 2) {
                /* Dolphin-style internal resolution: render the scene bigger
                 * than 640x480 and downscale, so edges are crisp instead of
                 * blocky. Costs GPU fill, not game speed. */
                int s = gx_ogl_get_internal_scale();
                snprintf(label, lcap, "internal resolution");
                if (s <= 1) snprintf(value, vcap, "native (640x480)");
                else        snprintf(value, vcap, "%dx  (%dx%d)", s, 640 * s, 480 * s);
            }
            else          { snprintf(label, lcap, "present diagnostic");
                            snprintf(value, vcap, "%s", g_web_present_diag ? "on" : "off"); }
            return;
        case TAB_AUDIO:
            if (row == 0) { snprintf(label, lcap, "volume");
                            snprintf(value, vcap, "%d%%", g_audio_volume); }
            else          { snprintf(label, lcap, "music");
                            snprintf(value, vcap, "%s", g_music_on ? "on" : "off"); }
            return;
        case TAB_CONTROLS: {
            snprintf(label, lcap, "%s", BINDS[row].label);
            SDL_Keycode k = controls_get_binding(BINDS[row].bit);
            if (k == SDLK_UNKNOWN) snprintf(value, vcap, "%s", BIND_DEFAULT[row]);
            else {
                const char *n = SDL_GetKeyName(k);
                snprintf(value, vcap, "%s", (n && *n) ? n : "?");
            }
            return;
        }
        default: {
            uint32_t o = player_obj();
            if (row == 0) {
                snprintf(label, lcap, "invincibility");
                snprintf(value, vcap, "%s", g_invincible ? "on" : "off");
                return;
            }
            if (row == 1) {
                static const char *R[4] = { "0", "90", "180", "270" };
                snprintf(label, lcap, "d-pad rotation");
                snprintf(value, vcap, "%s deg %s   (f7 inverts)",
                         R[g_sideways_mode & 3],
                         video_sideways_q() ? "APPLIED" : "idle");
                return;
            }
            if (!o) {   /* no level loaded: say so once rather than 6 dashes */
                if (row == 2) snprintf(label, lcap, "no player  (not in a level)");
                return;
            }
            switch (row) {
                case 2: snprintf(label, lcap, "x");
                        snprintf(value, vcap, "%.1f", (double)MEM_RF(o + OFF_X)); break;
                case 3: snprintf(label, lcap, "y");
                        snprintf(value, vcap, "%.1f", (double)MEM_RF(o + OFF_Y)); break;
                case 4: snprintf(label, lcap, "health");
                        snprintf(value, vcap, "%d", (int)MEM_R32(o + OFF_HEALTH)); break;
                case 5: snprintf(label, lcap, "state");
                        snprintf(value, vcap, "%d", (int)MEM_R32(o + OFF_STATE)); break;
                case 6: snprintf(label, lcap, "i-frames");
                        snprintf(value, vcap, "%.1f", (double)MEM_RF(o + OFF_IFRAMES)); break;
                case 7: snprintf(label, lcap, "anim timer");
                        snprintf(value, vcap, "%d", (int)MEM_R32(o + OFF_ANIM)); break;
                case 8: snprintf(label, lcap, "level");
                        snprintf(value, vcap, "%s  (id %d%s)", robox_level_name(),
                                 robox_level_id(),
                                 robox_level_is_robot() ? ", robot" : ""); break;
                case 9: {
                        const unsigned in  = video_input_raw()  & 0xFu;
                        const unsigned out = video_input_hold() & 0xFu;
                        snprintf(label, lcap, "dpad in / out / rot");
                        snprintf(value, vcap, "%u / %u / %u deg",
                                 in, out, video_sideways_q() * 90u);
                        break; }
                case 10: {
                        const int t = video_input_p1_tilt();
                        snprintf(label, lcap, "tilt key");
                        snprintf(value, vcap, "%s",
                                 t < 0 ? "left" : t > 0 ? "right" : "none");
                        break; }
                default:
                        snprintf(label, lcap, "f8 field scan");
                        switch (g_snap_phase) {
                            case 0: snprintf(value, vcap, "%s",
                                        g_snap_found >= 0 ? "done - see console" : "press f8 outside");
                                    break;
                            case 1: snprintf(value, vcap, "wait, press f8 again"); break;
                            default: snprintf(value, vcap, "now go inside, press f8"); break;
                        }
                        break;
            }
            return;
        }
    }
}

void robox_menu_render(void) {
    /* Keep drawing after g_open clears so the close can fade. Input stops
     * immediately (robox_menu_key returns 0 once closed), so the game is
     * already listening again while the panel is still on its way out --
     * which is what makes it feel responsive rather than laggy. */
    if (!g_open && g_anim <= 0.0f) return;

    /* Cheap ease. Appearing or vanishing instantly reads as a glitch; ~120 ms
     * reads as deliberate. Out is slightly quicker than in: a slow dismiss
     * feels like the UI is arguing with you. */
    if (g_open) { g_anim += 0.14f; if (g_anim > 1.0f) g_anim = 1.0f; }
    else        { g_anim -= 0.20f; if (g_anim < 0.0f) { g_anim = 0.0f; return; } }
    const float a = g_anim;
    const float top = (1.0f - a) * 18.0f;   /* drifts up into place */

    gx_ogl_overlay_begin();

    /* Deep scrim instead of a panel: the game keeps its own menus on the
     * artwork, and dimming far enough to read white text preserves that
     * without introducing a box the game never uses. */
    gx_ogl_overlay_rect(0, 0, 1280, 720, COL_SCRIM, 0.90f * a);

    gx_ogl_overlay_text(COL_X, 120.0f + top, "settings", COL_ON, a, S_TITLE, TRACK_HEAD);
    gx_ogl_overlay_rect(COL_X, 162.0f + top, COL_W, 1, COL_ON, 0.28f * a);

    /* Tab transition. Smoothstep so it eases at both ends; linear reads
     * mechanical at this duration. */
    if (g_tab_anim < 1.0f) { g_tab_anim += 0.16f; if (g_tab_anim > 1.0f) g_tab_anim = 1.0f; }
    const float t  = g_tab_anim;
    const float ts = t * t * (3.0f - 2.0f * t);

    /* Tab strip. The selected tab is underlined and the rule SLIDES between
     * tabs rather than jumping, which is most of what makes the switch read as
     * one motion. Lay the strip out first so the slide has both endpoints. */
    float tab_x[TAB_COUNT], tab_w[TAB_COUNT];
    { float tx = COL_X;
      for (int i = 0; i < TAB_COUNT; ++i) {
          tab_w[i] = gx_ogl_overlay_text_width(TAB_NAME[i], S_TAB, TRACK);
          tab_x[i] = tx;
          tx += tab_w[i] + 34.0f;
      } }

    const float TAB_Y = 200.0f;
    for (int i = 0; i < TAB_COUNT; ++i) {
        /* Fade the label between dim and bright over the same transition, so
         * the outgoing tab does not blink off a frame before the rule moves. */
        float lit = (i == (int)g_tab) ? ts : (i == g_tab_prev ? 1.0f - ts : 0.0f);
        float r = 0.42f + 0.58f * lit, g = 0.45f + 0.55f * lit, b = 0.48f + 0.52f * lit;
        gx_ogl_overlay_text(tab_x[i], TAB_Y + top, TAB_NAME[i], r, g, b, a, S_TAB, TRACK);
    }
    {   /* Clear of the glyph box: the cell is 33 px tall before scaling, so an
         * underline at a fixed 214 ran straight THROUGH the text. */
        const float ul_y = TAB_Y + 33.0f * S_TAB + 5.0f;
        const float x0 = tab_x[g_tab_prev] + (tab_x[g_tab] - tab_x[g_tab_prev]) * ts;
        const float w0 = tab_w[g_tab_prev] + (tab_w[g_tab] - tab_w[g_tab_prev]) * ts;
        gx_ogl_overlay_rect(x0, ul_y + top, w0, 2, COL_ON, 0.9f * a);
    }

    /* Rows slide in from the side you came from, and fade as they arrive. */
    const float slide = (1.0f - ts) * 46.0f * (float)(g_tab_dir ? g_tab_dir : 1);
    const float rowa  = a * (0.25f + 0.75f * ts);

    const int n = tab_rows(g_tab);
    for (int i = 0; i < n; ++i) {
        const float ry = ROW_TOP + top + i * ROW_H;
        const float rx = COL_X + slide;
        const int   on = (i == g_sel);
        const int   act = row_actionable(g_tab, i);

        char label[64], value[48];
        row_text(g_tab, i, label, sizeof label, value, sizeof value);
        if (!label[0]) continue;

        /* Selected row gets a hairline frame -- exactly how the game's own
         * main menu marks "play". No fill, no accent colour. */
        if (on && act) {
            /* Centre the frame on the GLYPH BOX rather than guessing an
             * offset. The font cell is 33 px tall before scaling, and the text
             * is drawn from its top edge, so a hardcoded inset only looks right
             * at one value of S_ROW -- and was 6 px out at this one. */
            const float cell  = 33.0f * S_ROW;
            const float box_h = ROW_H - 2.0f;
            gx_ogl_overlay_outline(rx - 16, ry + (cell - box_h) * 0.5f,
                                   COL_W + 32, box_h, 1.0f,
                                   COL_ON, (g_capturing ? 0.35f : 0.85f) * rowa);
        }

        if (on && act) gx_ogl_overlay_text(rx, ry, label, COL_ON,    rowa, S_ROW, TRACK);
        else if (act)  gx_ogl_overlay_text(rx, ry, label, COL_OFF,   rowa, S_ROW, TRACK);
        else           gx_ogl_overlay_text(rx, ry, label, COL_FAINT, rowa, S_ROW, TRACK);

        if (!value[0]) continue;
        const int   cap   = (on && g_capturing);
        const char *shown = cap ? "press a key" : value;
        const float tw    = gx_ogl_overlay_text_width(shown, S_ROW, TRACK);
        if (cap)            gx_ogl_overlay_text(rx + COL_W - tw, ry, shown, COL_AMBER, rowa, S_ROW, TRACK);
        else if (on && act) gx_ogl_overlay_text(rx + COL_W - tw, ry, shown, COL_ON,    rowa, S_ROW, TRACK);
        else                gx_ogl_overlay_text(rx + COL_W - tw, ry, shown, COL_FAINT, rowa, S_ROW, TRACK);
    }

    /* Pinned rather than trailing the last row: the tabs have different row
     * counts, and a footer that jumps around between them reads as a bug. */
    const float fy = 690.0f + top;
    gx_ogl_overlay_rect(COL_X, fy - 24, COL_W, 1, COL_ON, 0.18f * a);
    gx_ogl_overlay_text(COL_X, fy,
                        g_tab == TAB_CONTROLS
                            ? "left/right tab   enter rebind   f9 reset   esc close"
                            : "left/right tab   enter change   esc close",
                        COL_FAINT, a, S_FOOT, TRACK);

    if (g_notice[0] && (double)SDL_GetTicks() < g_notice_until) {
        const float tw = gx_ogl_overlay_text_width(g_notice, S_FOOT, TRACK);
        gx_ogl_overlay_text(COL_X + COL_W - tw, fy, g_notice, COL_ON, a, S_FOOT, TRACK);
    }

    gx_ogl_overlay_end();
}

#endif /* !__3DS__ */
