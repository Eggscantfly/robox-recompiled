// sdk/robox_mario.c -- Super Mario Bros. movement for the Robox player.
//
// Replaces the player's movement with the *actual* SMB1 player character:
// sdk/mario.h is the NES game's player code extracted into one portable file
// (bit-exact physics -- verified frame-identical against the translated SMB
// engine, 543 frames / 0 mismatches). With this mod on, the robot walks,
// runs, skids, and does variable-height speed-dependent jumps exactly like
// Mario: $98/$E4 walk/run acceleration, $D0 release friction, the 10-frame
// B-run grace, 5 jump strength tiers, $20-held vs $70-released gravity.
//
// HOW IT DRIVES THE GAME (design notes, read before touching)
//
// The player entity (the type-0x32 entity registered at SDA[-0x4780] =
// 0x801FACE0 -- see robox_coop.c for that discovery) keeps its position in
// two floats at +0x08/+0x0c. We wrap the per-frame update (vtable slot 3,
// same slot the co-op mod profiles) and each frame:
//
//   1. SENSE. Call the game's own update with the input word forced neutral,
//      as a physics probe: from a supported position its gravity+collision
//      resolve moves the entity ~0; from the air it falls. That one number
//      answers "is there ground under the player" without us knowing
//      anything about the collision engine. The probe's motion is then
//      discarded -- only its dy is kept.
//   2. SIMULATE. Run mario.h at 1 tick per update with the game's own
//      already-computed action word mapped onto the NES pad (left/right/
//      down; jump = shake or [2]; run = [1]). The sim world is minimal:
//      a floor exists under Mario exactly when the probe said "supported".
//      Walls, ceilings, platforms stay the game's problem (step 3).
//   3. APPLY. Move the entity by the sim's dx/dy (scaled). The game's
//      collision resolves our write on the next frame's probe, and the
//      entity position is re-read as authority every frame, so penetration
//      never accumulates. Blocked X (wall) zeroes Mario's speed like
//      ImpedePlayerMove; a resolved push-down while rising is a head bump
//      (Y speed -> +1, NYSpd).
//
// The game's update still runs every frame (neutral input), so animation,
// state machine, camera, pickups and scripted logic keep working -- the
// robot just *moves* like it grew up in World 1-1.
//
// Env knobs:
//   ROBOX_MARIO_SCALE=<f>  game units per SMB pixel   (default 2.0)
//   ROBOX_MARIO_XSIGN=-1   flip screen-right           (default +1)
//   ROBOX_MARIO_SMALL=1    small Mario physics box     (default big)
//   ROBOX_MARIO_PROBE=1    once-a-second telemetry to stderr

#include "../src/runtime.h"
#include "gx_ogl.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define MARIO_IMPLEMENTATION
#include "mario.h"

// --- Guest addresses / layout (Robox USA.dol; see robox_coop.c) ------------

#define ENT_INIT_VA        0x800b3144u   // generic entity init (vtable-only)
#define PLAYER_TYPE_ID     0x32u
#define ENT_TYPE_OFF       0x1d8u
#define SDA_PLAYER_PTR_OFF (-0x4780)     // 0x801FACE0: the player singleton
#define SDA_INPUT_MASK_OFF (-0x46bc)     // 0x801FADA4: the action bitmask

#define OFF_POS_X 0x08u
#define OFF_POS_Y 0x0cu

// Action bits (rebuilt in robox_coop.c from the game's only input writer).
#define ACT_UP        0x00000001u
#define ACT_DOWN      0x00000002u
#define ACT_RIGHT     0x00000004u
#define ACT_LEFT      0x00000008u
#define ACT_TWO       0x00000010u
#define ACT_ONE       0x00000020u
#define ACT_SHAKE     0x00000100u
#define ACT_TILT_L    0x00000200u
#define ACT_TILT_R    0x00000400u
#define ACT_UP_T      0x00000800u
#define ACT_DOWN_T    0x00001000u
#define ACT_RIGHT_T   0x00002000u
#define ACT_LEFT_T    0x00004000u
#define ACT_TWO_T     0x00008000u
#define ACT_ONE_T     0x00010000u

// --- State -----------------------------------------------------------------

static int      g_probe;               // ROBOX_MARIO_PROBE
static float    g_scale = 2.0f;        // game units per SMB pixel
static float    g_xsign = 1.0f;        // +1: SMB-right is +X in the game
static int      g_mode;                // F2 state: 0 robot (all native),
                                       // 1 Mario (physics + sprite)
static int      g_live;                // >0: player updated this instant
                                       // (drains fast on pause/menus)
static int      g_armed;               // player found, vt[3]/vt[4] wrapped

static uint32_t g_player;              // current player entity (0 = none)
static uint32_t g_vt3_addr;            // guest addr of vtable slot 3
static PPC_Func g_vt3_orig;            // whatever was there before us
static int      g_in_probe;            // re-entry guard for the sensor call

static Mario    g_sim;                 // the NES player character
static int      g_sim_live;            // sim initialised for this player
static int      g_floor_on;            // probe verdict: ground under player
static float    g_down_sign = 1.0f;    // game-Y direction of falling
                                       // (+Y = down in this engine; see
                                       // ROBOX_MARIO_YSIGN to flip)

static int va_ok(uint32_t va);

// --- Entity tracking, stomps, fireball kills -------------------------------
//
// The function we wrapped as "the player's update" is a shared base-class
// method: every entity in the level passes through our trampoline. That
// makes an enemy radar free -- for each non-player `this`, remember its
// position (+0x08/+0x0c, same base layout) and how much it has moved
// lately. Stompable = a tracked entity that actually moves around (props,
// triggers and scenery hold still; creatures do not).
//
// Kills are dealt by teleporting the entity far below the world -- through
// its OWN +0x38 authority pair, the same derived-copy layout the player
// has -- and marking a smoke poof at the spot. Types are logged on every
// kill so a blocklist can be built if something dies that should not.

#define ENT_MAX   32
#define ENT_MOVE_MIN 6.0f              // lifetime travel to count as "alive"

typedef struct {
    uint32_t va;                       // 0 = free slot
    float    x, y, px, py;             // current and previous position
    float    moved;                    // accumulated |travel|
    uint32_t seen;                     // tick of last update
    uint32_t type;
} ent_t;
static ent_t    g_ents[ENT_MAX];
static uint32_t g_ent_tick;            // advances once per player update
static uint32_t g_killed[16];          // recently killed: don't re-track
static int      g_killed_n;

// World-anchored smoke poofs (stomps, fireball kills).
typedef struct { float x, y; int t; } wpoof_t;
static wpoof_t  g_wpoof[6];

static void wpoof_add(float x, float y) {
    for (int i = 0; i < 6; i++)
        if (g_wpoof[i].t <= 0) { g_wpoof[i].x = x; g_wpoof[i].y = y;
                                 g_wpoof[i].t = 26; return; }
}

static int ent_killed_recently(uint32_t va) {
    for (int i = 0; i < g_killed_n && i < 16; i++)
        if (g_killed[i] == va) return 1;
    return 0;
}

static void ent_track(uint32_t self) {
    if (ent_killed_recently(self)) return;
    float x = MEM_RF(self + OFF_POS_X), y = MEM_RF(self + OFF_POS_Y);
    if (!(x > -1e6f && x < 1e6f && y > -1e6f && y < 1e6f)) return;

    ent_t *slot = 0, *oldest = &g_ents[0];
    for (int i = 0; i < ENT_MAX; i++) {
        if (g_ents[i].va == self) { slot = &g_ents[i]; break; }
        if (!g_ents[i].va) { if (!slot) slot = &g_ents[i]; }
        else if (g_ents[i].seen < oldest->seen) oldest = &g_ents[i];
    }
    if (!slot) slot = oldest;
    if (slot->va != self) {
        slot->va = self;
        slot->moved = 0;
        slot->px = x; slot->py = y;
        slot->type = MEM_R32(self + ENT_TYPE_OFF);
    }
    slot->moved += fabsf(x - slot->px) + fabsf(y - slot->py);
    if (slot->moved > 1e6f) slot->moved = 1e6f;
    slot->px = slot->x; slot->py = slot->y;
    slot->x = x; slot->y = y;
    slot->seen = g_ent_tick;
}

static void ent_kill(ent_t *e, const char *how) {
    fprintf(stderr, "[MARIO] %s type=0x%x at (%.0f, %.0f) va=0x%08x\n",
            how, e->type, (double)e->x, (double)e->y, e->va);
    fflush(stderr);
    wpoof_add(e->x, e->y);
    // Drop it out of the world through both the derived floats and the
    // +0x38 authority pair (same base-class layout as the player).
    MEM_WF(e->va + OFF_POS_Y, e->y + 30000.0f);
    MEM_WF(e->va + 0x3cu,     e->y + 30000.0f);
    if (g_killed_n < 16) g_killed[g_killed_n++] = e->va;
    else { g_killed[g_ent_tick & 15u] = e->va; }
    e->va = 0;
}

// Falling onto a live, moving entity = SMB stomp: it dies, Mario bounces.
static void stomp_check(float ex, float ey) {
    if (g_sim.y_spd <= 0) return;              // must be moving down
    for (int i = 0; i < ENT_MAX; i++) {
        ent_t *e = &g_ents[i];
        if (!e->va || g_ent_tick - e->seen > 30u) continue;
        if (e->moved < ENT_MOVE_MIN) continue;  // scenery holds still
        float dx = e->x - ex, dy = e->y - ey;
        if (dx < -36.0f || dx > 36.0f) continue;
        if (dy < 6.0f || dy > 56.0f) continue;  // must be underneath
        ent_kill(e, "STOMP");
        // EnemyStomped: bounce. A fresh short hop; holding A stretches it
        // a little via the normal variable-height rules.
        g_sim.y_spd = (int8_t)0xfc;
        g_sim.y_frac = 0;
        g_sim.state = MARIO_ST_JUMP;
        g_sim.jump_origin_y = (uint8_t)g_sim.y;
        return;                                 // one stomp per frame
    }
}

// Fireballs live in the sim; their world position hangs off Mario's anchor.
static void fireball_checks(float ex, float ey) {
    for (int f = 0; f < 2; f++) {
        MarioFireball *fb = &g_sim.fb[f];
        if (fb->state == 0 || fb->state >= 0x80) continue;
        float fwx = ex + (float)(fb->x - g_sim.x) * g_scale * g_xsign;
        float fwy = ey + (float)(fb->y - g_sim.y) * g_scale * g_down_sign;
        for (int i = 0; i < ENT_MAX; i++) {
            ent_t *e = &g_ents[i];
            if (!e->va || g_ent_tick - e->seen > 30u) continue;
            if (e->moved < ENT_MOVE_MIN) continue;
            if (fabsf(e->x - fwx) > 30.0f || fabsf(e->y - fwy) > 34.0f)
                continue;
            ent_kill(e, "FIREBALL");
            fb->state = 0x80;                   // explode the ball
            g_sim.events |= MARIO_EV_FB_EXPLODE;
            break;
        }
    }
}

// --- Authority hunt --------------------------------------------------------
//
// The +0x08/+0x0c floats are a *derived copy*: the update recomputes them
// from an internal transform every frame, so writing them moves the player
// for exactly one frame before it snaps back (observed: pbx cancels our step
// exactly). The real store is found empirically at level start: collect
// every float pair equal to the current position -- inside the entity and
// one pointer-hop away -- then test-write each candidate (+8 units) for one
// frame and keep the one the entity's own floats follow. During the hunt
// the game runs normally on real input.
#define HUNT_MAX 64
typedef struct { uint32_t va; float orig, base_ex; } hunt_cand_t;
static hunt_cand_t g_cand[HUNT_MAX];
static int      g_cand_n, g_cand_i;
static int      g_hunt;                // 0 build, 1 test, 2 found, 3 failed
static int      g_hunt_pending;        // a test write awaits its verdict
static uint32_t g_auth_va;             // authoritative float pair (x, then y)

static void hunt_build(uint32_t ent, float ex, float ey) {
    g_cand_n = 0; g_cand_i = 0; g_hunt_pending = 0;
    for (uint32_t off = 0; off + 8 <= 0x39cu && g_cand_n < HUNT_MAX; off += 4) {
        if (off == OFF_POS_X) continue;                  // the copy itself
        if (fabsf(MEM_RF(ent + off) - ex) < 0.5f &&
            fabsf(MEM_RF(ent + off + 4) - ey) < 0.5f)
            g_cand[g_cand_n++].va = ent + off;
    }
    for (uint32_t off = 0; off + 4 <= 0x39cu && g_cand_n < HUNT_MAX; off += 4) {
        uint32_t p = MEM_R32(ent + off);
        if (!va_ok(p) || p == ent) continue;
        for (uint32_t so = 0; so + 8 <= 0x180u && g_cand_n < HUNT_MAX; so += 4) {
            if (fabsf(MEM_RF(p + so) - ex) < 0.5f &&
                fabsf(MEM_RF(p + so + 4) - ey) < 0.5f) {
                int dup = 0;
                for (int k = 0; k < g_cand_n; k++)
                    if (g_cand[k].va == p + so) dup = 1;
                if (!dup) g_cand[g_cand_n++].va = p + so;
            }
        }
    }
    fprintf(stderr, "[MARIO] authority hunt: %d candidate position stores\n",
            g_cand_n);
    fflush(stderr);
    g_hunt = g_cand_n ? 1 : 3;
}

// One hunt step per frame, BEFORE the game update runs. Returns while the
// hunt is live; sets g_hunt=2/g_auth_va when the authority is confirmed.
static void hunt_step(uint32_t ent) {
    float ex = MEM_RF(ent + OFF_POS_X);
    if (g_hunt_pending) {
        hunt_cand_t *c = &g_cand[g_cand_i];
        float moved = ex - c->base_ex;
        MEM_WF(c->va, c->orig);                  // restore no matter what
        g_hunt_pending = 0;
        if (moved > 5.0f) {                      // entity followed our +8
            g_auth_va = c->va;
            g_hunt = 2;
            fprintf(stderr, "[MARIO] authority found at 0x%08x "
                            "(entity moved %+.1f on test write)\n",
                    c->va, (double)moved);
            fflush(stderr);
            return;
        }
        if (++g_cand_i >= g_cand_n) {
            g_hunt = 3;
            fprintf(stderr, "[MARIO] authority hunt FAILED -- falling back "
                            "to direct float writes (movement will fight "
                            "the engine)\n");
            fflush(stderr);
            return;
        }
    }
    hunt_cand_t *c = &g_cand[g_cand_i];
    c->orig = MEM_RF(c->va);
    c->base_ex = ex;
    MEM_WF(c->va, c->orig + 8.0f);
    g_hunt_pending = 1;
}

// telemetry
static uint32_t g_ticks, g_last_report;

static uint32_t player_slot_va(void) {
    return g_cpu.gpr[13] + (uint32_t)SDA_PLAYER_PTR_OFF;
}
static uint32_t input_slot_va(void) {
    return g_cpu.gpr[13] + (uint32_t)SDA_INPUT_MASK_OFF;
}
static int va_ok(uint32_t va) {
    return va >= 0x80003000u && va < 0x94000000u;
}

// --- The sim's world: a floor that exists only when the game says so -------
//
// mario.h asks about 16x16 tiles; the only question that matters here is
// "is there ground under my feet". Everything else (walls, ceilings, moving
// platforms) is handled by reconciling against the game's collision in
// APPLY/readback, so the sim world is deliberately this dumb.
static uint8_t mario_tile_cb(void *user, int32_t tx, int32_t ty) {
    (void)user; (void)tx;
    // Feet at y+32: standing on SIM_STAND_Y=176 puts them at 208, which is
    // the TOP of tile row (208-32)>>4 = 11 -- so the floor is row 11 down.
    return g_floor_on && ty >= 11;
}

// The sim lives on a fixed virtual playfield: floor row 12 (feet y=208,
// standing y=176). X free-runs from the middle of the int32 range; only
// per-frame deltas leave the sim.
#define SIM_STAND_Y 176
#define SIM_HOME_X  0x40000000

static int g_power = 2;                // 0 small, 1 big, 2 fire (F3 cycles)

static void sim_reset(void) {
    mario_init(&g_sim, SIM_HOME_X, SIM_STAND_Y);
    g_sim.tile_at = mario_tile_cb;
    const char *sm = getenv("ROBOX_MARIO_SMALL");
    if (sm && *sm && *sm != '0') g_power = 0;
    g_sim.size = g_power == 0 ? MARIO_SMALL : MARIO_BIG;
    g_sim.fire = g_power == 2;
    g_sim_live = 1;
    g_floor_on = 0;
}

void robox_mario_power_cycle(void) {           /* F3 */
    static const char *names[3] = { "small", "big", "FIRE" };
    if (!g_armed || !g_sim_live) return;
    g_power = (g_power + 1) % 3;
    g_sim.size = g_power == 0 ? MARIO_SMALL : MARIO_BIG;
    g_sim.fire = g_power == 2;
    fprintf(stderr, "[MARIO] F3: %s Mario\n", names[g_power]);
    fflush(stderr);
}

// --- Input: game's action word -> NES pad ----------------------------------
//
// The action word is in WIIMOTE space: the host pre-rotates the keys a
// quarter turn in sideways sections (video_sideways_q), and the game's
// player code interprets e.g. ACT_DOWN as "walk screen-right" there. Undo
// that rotation so the NES pad always gets SCREEN directions.
//
// ACT bits clockwise from UP (same cycle robox_coop.c uses forward):
static const uint32_t ACT_CYCLE_HOLD[4] = { ACT_UP, ACT_RIGHT, ACT_DOWN, ACT_LEFT };

static uint8_t act_to_joy(uint32_t m) {
    extern unsigned video_sideways_q(void);
    extern uint32_t video_input_raw(void);
    unsigned q = video_sideways_q() & 3u;
    uint8_t joy = 0;
    // screen direction i (0=up,1=right,2=down,3=left) reads the ACT bit the
    // rotation put it on: (i + q) mod 4.
    static const uint8_t SCREEN_JOY[4] =
        { MARIO_JOY_UP, MARIO_JOY_RIGHT, MARIO_JOY_DOWN, MARIO_JOY_LEFT };
    for (unsigned i = 0; i < 4; ++i)
        if (m & ACT_CYCLE_HOLD[(i + q) & 3u]) joy |= SCREEN_JOY[i];
    // Jump is held-state on purpose: SMB variable jump height needs the
    // level, and mario.h does its own new-press edge detection.
    if (m & (ACT_SHAKE | ACT_TWO)) joy |= MARIO_JOY_A;
    // Run comes from the RAW host button state: the ONE button is masked out
    // of everything the guest sees in Mario mode (it is the robot's fire
    // button), so the action word never carries it here.
    if (video_input_raw() & 0x0200u) joy |= MARIO_JOY_B;
    // A real d-pad cannot do left+right; resolve like the NES would not:
    // drop both rather than let ImposeFriction see facing=3.
    if ((joy & (MARIO_JOY_LEFT | MARIO_JOY_RIGHT))
            == (MARIO_JOY_LEFT | MARIO_JOY_RIGHT))
        joy &= (uint8_t)~(MARIO_JOY_LEFT | MARIO_JOY_RIGHT);
    return joy;
}

// --- Per-zone player tracking (same rules as co-op: never cache) -----------

static void refresh_player(void) {
    uint32_t slot = player_slot_va();
    uint32_t cur = va_ok(slot) ? MEM_R32(slot) : 0;
    if (cur == g_player) return;
    g_player = cur;
    g_sim_live = 0;                    // re-anchor on the new entity
    g_hunt = 0;                        // re-find the authority: new object,
    g_auth_va = 0;                     // new sub-objects, new addresses
    if (g_probe) {
        fprintf(stderr, "[MARIO] player now 0x%08x -- sim reset\n", cur);
        fflush(stderr);
    }
}

// --- The wrapped update ----------------------------------------------------

static void mario_vt3_tramp(void) {
    uint32_t self = g_cpu.gpr[3];

    if (g_in_probe) {                  // our own sensor call: pass through
        if (g_vt3_orig) g_vt3_orig();
        return;
    }

    refresh_player();

    if (!g_player || self != g_player) {
        if (g_player && g_mode) ent_track(self);   // free enemy radar
        if (g_vt3_orig) g_vt3_orig();  // other entities share this method
        return;
    }

    if (!g_sim_live) sim_reset();
    g_live = 3;                        // gameplay is running right now
    g_ent_tick++;

    // Position we intended last frame (what APPLY wrote).
    float ex0 = MEM_RF(g_player + OFF_POS_X);
    float ey0 = MEM_RF(g_player + OFF_POS_Y);

    // Authority not confirmed yet: hunt one candidate per frame while the
    // game runs normally on real input.
    if (g_hunt < 2) {
        if (g_hunt == 0) hunt_build(g_player, ex0, ey0);
        if (g_hunt == 1) hunt_step(g_player);
        if (g_hunt < 2) {
            g_cpu.gpr[3] = self;
            if (g_vt3_orig) g_vt3_orig();
            return;
        }
    }

    // Robot mode: everything native -- real input, the game's own movement,
    // animations, camera. Mario is one F2 away.
    if (!g_mode) {
        g_cpu.gpr[3] = self;
        if (g_vt3_orig) g_vt3_orig();
        return;
    }

    // Camera: fully native. The game's own camera follows the player
    // entity, and the player entity is Mario -- nothing to do.

    // 1. SENSE -- neutral-input probe through the game's own physics. Its
    // collision RESOLVES our intended position: pushed back up = there is
    // ground; allowed to fall further = air. The resolved position is
    // adopted as this frame's base, so penetration can never accumulate.
    uint32_t islot = input_slot_va();
    uint32_t in_save = MEM_R32(islot);
    MEM_W32(islot, 0);
    g_in_probe = 1;
    g_cpu.gpr[3] = self;
    if (g_vt3_orig) g_vt3_orig();
    g_in_probe = 0;
    MEM_W32(islot, in_save);

    float ex1 = MEM_RF(g_player + OFF_POS_X);   // resolved = ground truth
    float ey1 = MEM_RF(g_player + OFF_POS_Y);
    float pdy = (ey1 - ey0) * g_down_sign;      // + = fell further, - = pushed up
    float pbx = (ex1 - ex0) * g_xsign;          // horizontal correction

    g_floor_on = pdy <= 0.20f * g_scale;        // did NOT keep falling

    // Wall: the game moved us back horizontally; stop like ImpedePlayerMove.
    // The threshold must clear the engine's small constant animation-root
    // pull (~1 unit/frame observed) or walking gets speed-zeroed forever;
    // a real wall eats the whole step (several units at walk/run speed).
    // A push-back PLUS a push-up is a SLOPE, not a wall -- keep the speed,
    // the resolve already carried us up the incline.
    if (fabsf(pbx) > 2.5f * g_scale && pdy > -0.2f * g_scale) {
        if ((pbx < 0 && g_sim.x_spd > 0) || (pbx > 0 && g_sim.x_spd < 0))
            g_sim.x_spd = 0;
    }
    // Head bump: we were rising but got pushed back down.
    if (g_sim.y_spd < 0 && pdy > 0.5f * g_scale)
        g_sim.y_spd = 1;                        // NYSpd

    // 2. SIMULATE -- NES frames at NES speed. The game updates once per
    // RENDERED frame, so on a 120/144Hz monitor this trampoline runs that
    // often -- ticking the sim every call made Mario live in fast-forward.
    // A real-time accumulator dispenses ticks at 60.0988Hz (NTSC) no matter
    // what the host renders at; frames between ticks just hold position.
    static uint64_t s_last_ms;
    static double   s_acc_ms;
    extern uint64_t ms_now(void);
    {
        uint64_t now = ms_now();
        if (!s_last_ms || now - s_last_ms > 250u) { s_last_ms = now; s_acc_ms = 0; }
        s_acc_ms += (double)(now - s_last_ms);
        s_last_ms = now;
        if (s_acc_ms > 100.0) s_acc_ms = 100.0;
    }

    int32_t pre_fx = (g_sim.x << 8) | g_sim.x_sub;
    int32_t pre_y  = g_sim.y;

    {
        const double NES_MS = 1000.0 / 60.0988139;
        int   ticks = 0;
        uint8_t joy = act_to_joy(in_save);
        while (s_acc_ms >= NES_MS && ticks < 3) {
            s_acc_ms -= NES_MS;
            mario_tick(&g_sim, joy);
            ticks++;
        }
    }

    // Landing assist: a fall the game stopped on a ledge leaves the sim
    // airborne over its own (virtual) floor row. Snap it onto the row; its
    // own foot collision lands it -- with the real LandPlyr snap -- next tick.
    if (g_floor_on && g_sim.state != MARIO_ST_GROUND && g_sim.y_spd >= 0
        && (g_sim.y < SIM_STAND_Y - 8 || g_sim.y > SIM_STAND_Y))
        g_sim.y = SIM_STAND_Y;

    int32_t post_fx = (g_sim.x << 8) | g_sim.x_sub;
    float dx_px = (float)(post_fx - pre_fx) / 256.0f;
    float dy_px = (float)(g_sim.y - pre_y);

    // 3. APPLY -- resolved base + the sim's motion, in game units. Written
    // to the hunted authoritative store (the one the engine integrates
    // from); the entity floats are kept in sync for anything reading them
    // before the next update refreshes them.
    //
    // Ground stick: while walking, pre-embed the write a few units DOWN.
    // On flat ground and uphill the resolve pushes straight back up (that
    // upward push is also the strongest possible "grounded" signal); going
    // DOWNHILL it keeps the feet glued to the surface instead of popping
    // airborne every frame, which is what made slopes feel like stairs.
    // Over a real ledge there is nothing to resolve against, the probe
    // falls, and the sensor reads air exactly as before.
    float stick = 0.0f;
    if (g_floor_on && g_sim.state == MARIO_ST_GROUND && g_sim.y_spd >= 0) {
        stick = fabsf(dx_px) * g_scale + 4.0f;    // cover run-speed descents
        if (stick > 14.0f) stick = 14.0f;
    }
    float wx = ex1 + dx_px * g_scale * g_xsign;
    float wy = ey1 + dy_px * g_scale * g_down_sign + stick * g_down_sign;
    if (g_auth_va) {
        MEM_WF(g_auth_va,     wx);
        MEM_WF(g_auth_va + 4, wy);
    }
    MEM_WF(g_player + OFF_POS_X, wx);
    MEM_WF(g_player + OFF_POS_Y, wy);

    // Enemies: stomp anything alive under a falling Mario; fireballs kill
    // whatever they touch.
    stomp_check(wx, wy);
    fireball_checks(wx, wy);

    if (g_probe && ++g_ticks - g_last_report >= 60u) {
        extern uint32_t video_input_raw(void);
        g_last_report = g_ticks;
        fprintf(stderr, "[MARIO] t=%u pos=(%.1f,%.1f) pdy=%+.2f pbx=%+.2f "
                        "floor=%d joy=%02x raw=%05x spd=%d yspd=%d "
                        "st=%u act=%u\n",
                g_ticks, (double)wx, (double)wy, (double)pdy, (double)pbx,
                g_floor_on, act_to_joy(in_save), video_input_raw(),
                g_sim.x_spd, g_sim.y_spd, g_sim.state, g_sim.gfx_action);
        fflush(stderr);
    }
}

// --- The visible Mario -----------------------------------------------------
//
// F2 poofs the robot away and spawns Mario (and back). Mechanism:
//  * vtable slot 4 is the player's draw. While Mario mode is on, the whole
//    call runs inside gx_ogl_player_capture_begin(suppress=1): every vertex
//    the robot would have drawn is projected on the CPU into a screen
//    bounding box and then swallowed -- the robot vanishes but its exact
//    on-screen placement is still learned every frame.
//  * At present time (robox_mario_overlay_render, called from gx_ogl.c next
//    to the settings menu), the NES sprite -- straight from mods/mario.chr,
//    with mario.h's own animation state -- is drawn into that box with
//    overlay rects, feet-aligned, run-length merged per row.
//  * The toggle plays a little smoke poof either way.

#include "gx_ogl.h"

static int      g_poof;                // poof frames remaining
static int      g_chr_ok;
static uint8_t  g_chr[4096];           // NES sprite pattern table (256 tiles)
static uint32_t g_vt4_addr;
static PPC_Func g_vt4_orig;

// NES master palette (same table the SMBtest demo uses).
static const uint32_t nes_pal[64] = {
0x7C7C7C,0x0000FC,0x0000BC,0x4428BC,0x940084,0xA80020,0xA81000,0x881400,
0x503000,0x007800,0x006800,0x005800,0x004058,0x000000,0x000000,0x000000,
0xBCBCBC,0x0078F8,0x0058F8,0x6844FC,0xD800CC,0xE40058,0xF83800,0xE45C10,
0xAC7C00,0x00B800,0x00A800,0x00A844,0x008888,0x000000,0x000000,0x000000,
0xF8F8F8,0x3CBCFC,0x6888FC,0x9878F8,0xF878F8,0xF85898,0xF87858,0xFCA044,
0xF8B800,0xB8F818,0x58D854,0x58F898,0x00E8D8,0x787878,0x000000,0x000000,
0xFCFCFC,0xA4E4FC,0xB8B8F8,0xD8B8F8,0xF8B8F8,0xF8A4C0,0xF0D0B0,0xFCE0A8,
0xF8D878,0xD8F878,0xB8F8B8,0xB8F8D8,0x00FCFC,0xF8D8F8,0x000000,0x000000
};

static void chr_load(void) {
    FILE *f = fopen("mods/mario.chr", "rb");
    if (!f) {
        fprintf(stderr, "[MARIO] mods/mario.chr missing -- F2 sprite swap "
                        "unavailable (physics unaffected)\n");
        return;
    }
    g_chr_ok = fread(g_chr, 1, sizeof g_chr, f) == sizeof g_chr;
    fclose(f);
    if (!g_chr_ok)
        fprintf(stderr, "[MARIO] mods/mario.chr short read -- ignored\n");
}

/* Buttons to hide from the guest (both WPAD and KPAD paths in
 * peripherals.c call this). Only while Mario mode is on AND the player is
 * actually updating -- the instant the game pauses, g_live drains and the
 * menus get the real buttons back ("1 = back" keeps working). */
uint32_t robox_mario_button_mask(void) {
    return (g_mode && g_live > 0) ? 0x0200u : 0u;   /* WPAD_BUTTON_ONE */
}

void robox_mario_visual_toggle(void) {
    if (!g_armed) {
        fprintf(stderr, "[MARIO] F2: no player yet\n");
        fflush(stderr);
        return;
    }
    if (!g_mode && !g_chr_ok) {
        fprintf(stderr, "[MARIO] F2: need mods/mario.chr for the sprite\n");
        fflush(stderr);
        return;
    }
    g_mode = !g_mode;
    g_poof = 26;
    g_sim_live = 0;      /* entering Mario mode re-anchors a fresh sim      */
    fprintf(stderr, "[MARIO] F2: %s\n",
            g_mode ? "poof -- it's-a me!"
                   : "poof -- robot restored (native controls)");
    fflush(stderr);
}

// Player draw wrapper: always learn the on-screen quad (the poof and the
// camera need it in both skins); suppress the draws only in Mario mode.
static void mario_vt4_tramp(void) {
    uint32_t self = g_cpu.gpr[3];
    int cap = !g_in_probe && g_player && self == g_player && g_sim_live;
    if (cap) gx_ogl_player_capture_begin(g_mode);
    if (g_vt4_orig) g_vt4_orig();
    if (cap) gx_ogl_player_capture_end();
}

// One 8x8 CHR tile as overlay rects (run-merged rows), for fireballs.
static void ovl_tile(uint8_t tno, int hflip, int vflip, float x, float y,
                     float s, const uint8_t *pal) {
    const uint8_t *tp = &g_chr[(size_t)tno * 16];
    for (int ty = 0; ty < 8; ty++) {
        int ry = vflip ? 7 - ty : ty;
        uint8_t p0 = tp[ry], p1 = tp[ry + 8];
        int tx = 0;
        while (tx < 8) {
            int bit = hflip ? tx : 7 - tx;
            uint8_t c = (uint8_t)(((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1));
            if (!c) { tx++; continue; }
            int tx1 = tx + 1;
            while (tx1 < 8) {
                int b1 = hflip ? tx1 : 7 - tx1;
                uint8_t c1 = (uint8_t)(((p0 >> b1) & 1) | (((p1 >> b1) & 1) << 1));
                if (c1 != c) break;
                tx1++;
            }
            uint32_t rgb = nes_pal[pal[c] & 0x3f];
            gx_ogl_overlay_rect(x + tx * s, y + ty * s, (float)(tx1 - tx) * s, s,
                                (float)((rgb >> 16) & 0xff) / 255.0f,
                                (float)((rgb >>  8) & 0xff) / 255.0f,
                                (float)( rgb        & 0xff) / 255.0f, 1.0f);
            tx = tx1;
        }
    }
}

// Little cartoon puff: a ring of blobs flying outward, fading.
static void poof_draw(float cx, float cy, float size, float t) {
    float a = (1.0f - t);
    float r = size * (0.35f + 0.85f * t);
    for (int i = 0; i < 8; i++) {
        float ang = (float)i * 0.7853982f + t * 1.2f;
        float bx = cx + cosf(ang) * r;
        float by = cy + sinf(ang) * r * 0.8f;
        float b = size * (0.34f - 0.20f * t);
        gx_ogl_overlay_rect(bx - b, by - b, 2*b, 2*b, 0.82f, 0.82f, 0.84f,
                            0.55f * a);
        gx_ogl_overlay_rect(bx - b*0.55f, by - b*0.55f, 1.1f*b, 1.1f*b,
                            1.0f, 1.0f, 1.0f, 0.75f * a);
    }
}

void robox_mario_overlay_render(void) {
    float qx, qy, qw, qh;
    if (g_live > 0) g_live--;          /* drains fast when gameplay pauses  */
    if (!g_armed || !g_sim_live) return;
    if (!gx_ogl_player_quad(&qx, &qy, &qw, &qh)) { g_poof = 0; return; }

    int poofing = g_poof > 0;
    int any_wpoof = 0;
    for (int i = 0; i < 6; i++) if (g_wpoof[i].t > 0) any_wpoof = 1;
    if (!g_mode && !poofing && !any_wpoof) return;

    // Sim -> overlay transform, shared by the sprite, fireballs and poofs:
    // the 16x32 sim box maps onto the captured robot quad, feet-aligned.
    float s = qh / 32.0f;
    if (s < 0.5f) s = 0.5f;
    float ox = qx + qw * 0.5f - 8.0f * s;         // sim box top-left on
    float oy = qy + qh - 32.0f * s;               // screen
    float ppu = qw / 32.0f;                       // overlay px per world unit
    float pex = g_player ? MEM_RF(g_player + OFF_POS_X) : 0;
    float pey = g_player ? MEM_RF(g_player + OFF_POS_Y) : 0;

    gx_ogl_overlay_begin();

    // Mario himself (hidden for the first half of the spawn poof).
    if (g_mode && g_chr_ok && !(poofing && g_poof > 13)) {
        MarioDraw d;
        uint8_t grid[32][16];
        memset(grid, 0, sizeof grid);
        mario_get_draw(&g_sim, &d);
        for (int slot = 0; slot < 8; slot++) {
            uint8_t tno = d.tiles[slot];
            if (tno == 0xfc) continue;
            const uint8_t *tp = &g_chr[(size_t)tno * 16];
            int bx = (slot & 1) * 8, by = (slot >> 1) * 8;
            for (int y = 0; y < 8; y++) {
                uint8_t p0 = tp[y], p1 = tp[y + 8];
                for (int x = 0; x < 8; x++) {
                    int bit = d.hflip[slot] ? x : 7 - x;
                    grid[by + y][bx + x] = (uint8_t)
                        (((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1));
                }
            }
        }
        const uint8_t *pal = mario_palette(&g_sim);
        for (int y = 0; y < 32; y++) {
            int x = 0;
            while (x < 16) {
                uint8_t c = grid[y][x];
                if (!c) { x++; continue; }
                int x1 = x + 1;
                while (x1 < 16 && grid[y][x1] == c) x1++;
                uint32_t rgb = nes_pal[pal[c] & 0x3f];
                gx_ogl_overlay_rect(ox + x * s, oy + y * s,
                                    (float)(x1 - x) * s, s,
                                    (float)((rgb >> 16) & 0xff) / 255.0f,
                                    (float)((rgb >>  8) & 0xff) / 255.0f,
                                    (float)( rgb        & 0xff) / 255.0f,
                                    1.0f);
                x = x1;
            }
        }
    }

    // Fireballs and their explosions, straight from the sim.
    if (g_mode && g_chr_ok) {
        static const uint8_t fb_pal[4] = { 0x00, 0x16, 0x30, 0x27 };
        for (int f = 0; f < 2; f++) {
            MarioFireballDraw fd;
            mario_fireball_draw(&g_sim, f, &fd);
            if (!fd.active) continue;
            float fx = ox + (float)(fd.x - g_sim.x) * s;
            float fy = oy + (float)(fd.y - g_sim.y) * s;
            if (!fd.exploding) {
                ovl_tile(fd.tile, fd.hflip, fd.vflip, fx, fy, s, fb_pal);
            } else {
                ovl_tile(fd.tile, 0, 0, fx - 4*s, fy - 4*s, s, fb_pal);
                ovl_tile(fd.tile, 0, 1, fx - 4*s, fy + 4*s, s, fb_pal);
                ovl_tile(fd.tile, 1, 0, fx + 4*s, fy - 4*s, s, fb_pal);
                ovl_tile(fd.tile, 1, 1, fx + 4*s, fy + 4*s, s, fb_pal);
            }
        }
    }

    if (poofing) {
        float t = (float)(26 - g_poof) / 26.0f;
        poof_draw(qx + qw * 0.5f, qy + qh * 0.5f,
                  (qh > qw ? qh : qw) * 0.75f, t);
        g_poof--;
    }

    // Smoke at stomped / fireballed enemies, anchored in world space.
    for (int i = 0; i < 6; i++) {
        if (g_wpoof[i].t <= 0) continue;
        float t = (float)(26 - g_wpoof[i].t) / 26.0f;
        float px = qx + qw * 0.5f + (g_wpoof[i].x - pex) * ppu * g_xsign;
        float py = qy + qh * 0.5f + (g_wpoof[i].y - pey) * ppu * g_down_sign;
        poof_draw(px, py, qh * 0.7f, t);
        g_wpoof[i].t--;
    }

    gx_ogl_overlay_end();
}

// --- Arming ----------------------------------------------------------------
//
// Chained exactly like co-op: hook the generic entity init (vtable-called
// only, so ppc_patch_func survives regens), wait for the type-0x32 entity,
// then wrap its vtable slot 3. Originals are captured with ppc_lookup_func
// *before* patching, so this stacks correctly on top of the co-op mod's own
// trampolines when both are enabled.

static PPC_Func g_entinit_orig;

static void mario_ent_init_hook(void) {
    uint32_t self = g_cpu.gpr[3];

    if (g_entinit_orig) g_entinit_orig();

    if (g_armed || !va_ok(self)) return;
    if (MEM_R32(self + ENT_TYPE_OFF) != PLAYER_TYPE_ID) return;

    uint32_t vt = MEM_R32(self);
    if (!va_ok(vt)) return;
    uint32_t fn = MEM_R32(vt + 3u * 4u);
    if (!fn) return;

    g_vt3_addr = fn;
    g_vt3_orig = ppc_lookup_func(fn);
    if (!g_vt3_orig) {
        fprintf(stderr, "[MARIO] vt[3]=0x%08x not in dispatch table -- "
                        "mod inert\n", fn);
        fflush(stderr);
        return;
    }
    ppc_patch_func(fn, mario_vt3_tramp);

    // The draw (vt[4]) too, for the F2 sprite swap.
    g_vt4_addr = MEM_R32(vt + 4u * 4u);
    g_vt4_orig = g_vt4_addr ? ppc_lookup_func(g_vt4_addr) : 0;
    if (g_vt4_orig) ppc_patch_func(g_vt4_addr, mario_vt4_tramp);
    else fprintf(stderr, "[MARIO] vt[4] unhookable -- F2 swap unavailable\n");

    g_armed = 1;

    fprintf(stderr, "[MARIO] it's-a me: player 0x%08x, update 0x%08x wrapped "
                    "(scale=%.2f xsign=%+.0f %s)\n",
            self, fn, (double)g_scale, (double)g_xsign,
            getenv("ROBOX_MARIO_SMALL") ? "small" : "big");
    fflush(stderr);
}

void robox_mario_init(void) {
    const char *s = getenv("ROBOX_MARIO_SCALE");
    if (s && *s) {
        float f = (float)atof(s);
        if (f > 0.01f && f < 100.0f) g_scale = f;
    }
    const char *x = getenv("ROBOX_MARIO_XSIGN");
    if (x && *x == '-') g_xsign = -1.0f;
    const char *y = getenv("ROBOX_MARIO_YSIGN");
    if (y && *y == '-') g_down_sign = -1.0f;
    const char *p = getenv("ROBOX_MARIO_PROBE");
    g_probe = p && *p && *p != '0';

    chr_load();

    g_entinit_orig = ppc_lookup_func(ENT_INIT_VA);
    ppc_patch_func(ENT_INIT_VA, mario_ent_init_hook);

    fprintf(stderr, "[MARIO] armed: watching entity-init 0x%08x for the "
                    "type-0x%x player\n", ENT_INIT_VA, PLAYER_TYPE_ID);
    fflush(stderr);
}
