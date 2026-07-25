// sdk/robox_coop.c -- Robox co-op mod.
//
// PHASE 0 (this file, so far): reconnaissance. Everything below exists to
// answer three questions that decide how player 2 gets built:
//
//   1. Which guest object is the player, and what is its class (vtable)?
//   2. Which vtable slot is the per-frame update? (that is the function we
//      must run a second time, with P2's input, to make a second robot move)
//   3. What do the player's spawn parameters look like? (so a second one can
//      be constructed through the game's own spawner rather than memcpy'd)
//
// WHY IT HOOKS THE WAY IT DOES
// The player is not a hardcoded singleton. func_800b3144 is a *generic*
// entity-init: it copies a type id out of the spawn-param blob into
// entity+0x1d8 and, only when that type == 0x32, stores `this` into
// SDA[r13-0x4780] (= 0x801FACE0). So "the player" is just "the entity whose
// level data happens to say type 0x32". That is why a second player is
// plausible at all -- we are adding an entity, not special-casing an engine.
//
// func_800b3144 is reached ONLY through a vtable bctrl (verified: the only
// references to it anywhere are its own definition, its decl, and its
// func_table entry). That means ppc_patch_func() can redirect it without
// touching src/generated/ -- so this hook survives a regen, unlike the
// QUIRK EDIT hooks that apply_music_hooks.py has to re-inject every time.
//
// Everything here is gated behind ROBOX_COOP_PROBE=1 and prints at most a
// few dozen lines per level load. Nothing runs on a hot path.

#include "../src/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

// --- Guest addresses (Robox USA.dol) ---------------------------------------

// Generic entity init. r3 = this, r4 = spawn-param blob.
#define ENT_INIT_VA        0x800b3144u

// Spawn-param type id that makes an entity register itself as the player.
#define PLAYER_TYPE_ID     0x32u

// Where that type id lands inside the entity.
#define ENT_TYPE_OFF       0x1d8u

// The player singleton, SDA-relative. r13 = 0x801FF460 (set by
// __init_registers in funcs_0000_80004000.c), so this is 0x801FACE0 --
// the address the mod request started from. 382 references across ~40
// functions: camera, HUD, entity AI, level logic.
#define SDA_PLAYER_PTR_OFF (-0x4780)

// Level spawn loop: zeroes the player pointer, then walks level->entities[]
// (array at +0xc, count at +0x10) dispatching vtable[2](entity, 1).
#define LEVEL_SPAWN_VA     0x800b91f8u

// --- Small helpers ---------------------------------------------------------

// Guest RAM spans MEM1 (0x8000xxxx) and MEM2-as-0x9xxxxxxx. HLE pools live
// above 0x94000000 and are not guest-visible; treat those as invalid here.
static int va_ok(uint32_t va) {
    return va >= 0x80003000u && va < 0x94000000u;
}

static uint32_t player_ptr_va(void) {
    return g_cpu.gpr[13] + (uint32_t)SDA_PLAYER_PTR_OFF;
}

uint32_t robox_coop_player(void) {
    uint32_t slot = player_ptr_va();
    return va_ok(slot) ? MEM_R32(slot) : 0;
}

// Exact-match lookup against the dispatch table, so vtable entries can be
// reported as func_XXXXXXXX instead of bare addresses. Vtable slots point at
// function starts, so an exact match is the right test -- a nearest-symbol
// search (like the profiler's) would silently turn a bogus slot into a
// plausible-looking name, which is exactly the wrong failure mode here.
static int is_known_func(uint32_t addr) {
    size_t lo = 0, hi = g_func_table_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint32_t m = g_func_table_addrs[mid];
        if (m == addr) return 1;
        if (m < addr) lo = mid + 1; else hi = mid;
    }
    return 0;
}

static int probe_enabled(void) {
    static int checked, on;
    if (!checked) {
        checked = 1;
        const char *e = getenv("ROBOX_COOP_PROBE");
        on = e && *e && *e != '0';
    }
    return on;
}

// --- Phase 0: entity-init hook ---------------------------------------------

extern void func_800b3144(void);   // the real generic entity init

// How many vtable slots to dump. The class is big (entity + player derived),
// but the first ~24 covers ctor/dtor/init/update/draw in every CW-built
// hierarchy I have seen in this binary.
#define VT_SLOTS 24

static int   g_dumped_player;      // dump the class once, not once per level
static uint32_t g_player_vtable;   // remembered for later phases
static uint32_t g_player_obj;      // the registered player entity

// --- Phase 0b: which vtable slot is the per-frame update? ------------------
//
// Knowing the class is not enough: to drive a second robot we have to re-run
// the player's *update* with P2's input, so we need that exact slot. Rather
// than guess from disassembly, count real calls.
//
// Each slot gets a trampoline that tallies invocations, split by whether
// `this` is the player. The split matters because several of these are
// shared base-class methods that every entity in the level also runs --
// a raw call count would be dominated by other entities and tell us nothing.
//
// Originals are captured via ppc_lookup_func BEFORE patching, so this chains
// correctly even onto a slot this file already hooked (vt[11]).

static PPC_Func g_vt_orig [VT_SLOTS];
static uint32_t g_vt_addr [VT_SLOTS];
static uint32_t g_vt_self [VT_SLOTS];   // calls where this == player
static uint32_t g_vt_other[VT_SLOTS];   // calls on any other object
static int      g_vt_armed;

static void vt_report(void);

// --- Phase 0c: which method actually mutates the player, and where? -------
//
// vt[3] and vt[4] both run once per frame on the player and nothing else, so
// one is the update and the other is almost certainly the draw. Reading the
// disassembly is awkward (the recompiler label-splits these into several
// generated functions), so decide it empirically: snapshot the object, run
// the method, diff it.
//
// This does double duty -- it also maps the object layout. The position
// fields have to be located anyway, both to place P2 at spawn and to drive
// a camera that tracks both robots.

// The player block is 0x39c bytes per its ExpHeap 'UD' header.
#define SNAP_WORDS 0xe8u

static uint32_t g_snap_pre[SNAP_WORDS];
static uint8_t  g_changed[VT_SLOTS][SNAP_WORDS];  // offset ever changed?
static uint32_t g_slot_mutations[VT_SLOTS];

// Only the per-frame, player-exclusive slots are worth diffing.
static int watch_slot(unsigned n) { return n == 3u || n == 4u || n == 20u; }

static void snap_take(uint32_t obj) {
    for (unsigned i = 0; i < SNAP_WORDS; ++i)
        g_snap_pre[i] = MEM_R32(obj + i * 4u);
}

static void snap_diff(unsigned slot, uint32_t obj) {
    for (unsigned i = 0; i < SNAP_WORDS; ++i) {
        if (MEM_R32(obj + i * 4u) != g_snap_pre[i]) {
            if (!g_changed[slot][i]) ++g_slot_mutations[slot];
            g_changed[slot][i] = 1;
        }
    }
}

// Reinterpret a guest word as float, for spotting coordinates by eye.
static float as_f32(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }

static void mutation_report(void) {
    fprintf(stderr, "[COOP] --- fields mutated per slot (offset = value) ---\n");
    for (unsigned s = 0; s < VT_SLOTS; ++s) {
        if (!watch_slot(s) || !g_slot_mutations[s]) continue;
        fprintf(stderr, "[COOP]  vt[%u] 0x%08x mutates %u words:\n",
                s, g_vt_addr[s], g_slot_mutations[s]);
        unsigned shown = 0;
        for (unsigned i = 0; i < SNAP_WORDS && shown < 28u; ++i) {
            if (!g_changed[s][i]) continue;
            uint32_t w = MEM_R32(g_player_obj + i * 4u);
            fprintf(stderr, "[COOP]    +0x%03x = 0x%08x  %.3f\n",
                    i * 4u, w, (double)as_f32(w));
            ++shown;
        }
        if (g_slot_mutations[s] > shown)
            fprintf(stderr, "[COOP]    ... %u more\n", g_slot_mutations[s] - shown);
    }
    fflush(stderr);
}

static void vt_tally(unsigned slot) {
    if (g_cpu.gpr[3] == g_player_obj && g_player_obj) {
        // Report on a cadence driven by the busiest player-facing slot, so
        // the histogram lands a few seconds into actual gameplay.
        if (++g_vt_self[slot] % 900u == 0u) vt_report();
    } else {
        ++g_vt_other[slot];
    }
}

// --- Phase 1: player 2 ----------------------------------------------------
//
// P2 is a byte copy of P1's 0x39c-byte object, ticked by hand. It is
// deliberately NOT registered in any engine list -- we call its update and
// draw ourselves, so nothing in the level/render/collision bookkeeping has to
// know it exists. That sidesteps the entity factory entirely.
//
// Each injected tick runs with two globals swapped:
//   SDA[-0x4780] (0x801FACE0) -> P2, so the engine's player code operates on
//                                P2 for the duration
//   SDA[-0x46bc] (0x801FADA4) -> P2's action mask, so it reads P2's input
// Both are restored immediately, so for every other consumer in the frame the
// player singleton still means P1 and all 382 references stay correct.
//
// Milestone 1a (this step) feeds P2 *P1's own* input mask, so a working build
// shows a second robot mirroring the first. That validates clone + manual
// tick + global swap in one shot, before separate controls are wired up.

#define PLAYER_OBJ_SIZE 0x39cu
#define SDA_INPUT_MASK_OFF (-0x46bc)   // = 0x801FADA4, the action bitmask

#define OFF_POS_X 0x08u
#define OFF_POS_Y 0x0cu

static uint32_t g_p2_obj;             // live clone (0 = none this zone)
static uint32_t g_p2_alloc;           // backing block, reused across zones
static uint32_t g_vt4_r4, g_vt4_r5;   // P1's draw args, reused for P2
static int      g_p2_ticking;         // re-entry guard
static int      g_coop_on;            // ROBOX_COOP=1
static int      g_coop_mirror;        // ROBOX_COOP_MIRROR=1 (P2 copies P1)

static uint32_t input_mask_va(void) {
    return g_cpu.gpr[13] + (uint32_t)SDA_INPUT_MASK_OFF;
}

static float mem_rf(uint32_t va) { return as_f32(MEM_R32(va)); }
static void  mem_wf(uint32_t va, float f) {
    uint32_t w; memcpy(&w, &f, 4); MEM_W32(va, w);
}

// Byte-clone P1. Internal pointers (e.g. the +0x118 sub-object list) are
// aliased by this copy -- acceptable for the mirror milestone, and the first
// thing to revisit if P2 misbehaves in ways P1 does not.
static void coop_spawn_p2(void) {
    extern uint32_t game_heap_alloc(uint32_t size, uint32_t align);
    if (g_p2_obj || !g_player_obj) return;

    // The block is allocated once and re-cloned into on every zone change.
    // Allocating per zone would leak, since there is no guest-heap free on
    // this side and the clone is not owned by the game's allocator.
    if (!g_p2_alloc) {
        g_p2_alloc = game_heap_alloc(PLAYER_OBJ_SIZE, 32);
        if (!g_p2_alloc) {
            fprintf(stderr, "[COOP] P2 alloc FAILED\n");
            return;
        }
    }
    g_p2_obj = g_p2_alloc;
    for (uint32_t o = 0; o < PLAYER_OBJ_SIZE; o += 4)
        MEM_W32(g_p2_obj + o, MEM_R32(g_player_obj + o));

    // Offset so the two robots are not exactly coincident. The body is 32
    // wide (+0xb0), so two widths clears it with room to see both.
    mem_wf(g_p2_obj + OFF_POS_X, mem_rf(g_p2_obj + OFF_POS_X) + 64.0f);

    fprintf(stderr, "[COOP] P2 spawned at 0x%08x (clone of 0x%08x) pos=(%.1f, %.1f)\n",
            g_p2_obj, g_player_obj,
            (double)mem_rf(g_p2_obj + OFF_POS_X),
            (double)mem_rf(g_p2_obj + OFF_POS_Y));
    fflush(stderr);
}

// --- P2 action mask --------------------------------------------------------
//
// Rebuilds the game's own input encoding rather than inventing a parallel
// one. func_80063fd8 is the only writer of SDA[-0x46bc], and that single word
// is everything the player reads, so the whole per-player input contract is
// reproduced here. Synthesising a KPADStatus and calling the guest function
// would work too, but this is exact and has no guest-side side effects.
//
// Held bits come straight from the WPAD word; trig bits are edges against
// the previous frame, matching the game's own hold/trig split.

#define ACT_UP        0x00000001u
#define ACT_DOWN      0x00000002u
#define ACT_RIGHT     0x00000004u
#define ACT_LEFT      0x00000008u
#define ACT_TWO       0x00000010u
#define ACT_ONE       0x00000020u
#define ACT_SHAKE     0x00000100u   /* KPAD+0x18 over threshold -> jump */
#define ACT_UP_T      0x00000800u
#define ACT_DOWN_T    0x00001000u
#define ACT_RIGHT_T   0x00002000u
#define ACT_LEFT_T    0x00004000u
#define ACT_TWO_T     0x00008000u
#define ACT_ONE_T     0x00010000u

#define ACT_TILT_L    0x00000200u   /* accel below threshold */
#define ACT_TILT_R    0x00000400u   /* accel above threshold */

#define ACT_DIR_HOLD  (ACT_UP | ACT_DOWN | ACT_LEFT | ACT_RIGHT)
#define ACT_DIR_TRIG  (ACT_UP_T | ACT_DOWN_T | ACT_LEFT_T | ACT_RIGHT_T)

// Neutral direction bits from video.c, in DIR_UP/DOWN/LEFT/RIGHT order.
#define CD_UP 0u
#define CD_DOWN 1u
#define CD_LEFT 2u
#define CD_RIGHT 3u

struct dir_map { uint32_t hold, trig; };

// Action bits in clockwise order, so a rotation is just an index shift.
static const struct dir_map ACT_CYCLE[4] = {
    { ACT_UP,    ACT_UP_T    },
    { ACT_RIGHT, ACT_RIGHT_T },
    { ACT_DOWN,  ACT_DOWN_T  },
    { ACT_LEFT,  ACT_LEFT_T  },
};

// Neutral DIR_UP/DOWN/LEFT/RIGHT (bits 0..3 from video.c) -> cycle index.
static const unsigned DIR_TO_CYCLE[4] = { 0u, 2u, 3u, 1u };

// Quarter turns to apply, 0..3.
//
// AUTO asks the same question video.c does: is the current level in world 4?
// The Wiimote is held sideways on foot and vertical inside a robot, so on foot
// the d-pad needs the quarter turn and inside a robot it does not.
// robox_level_is_robot() reads the game's own current-level word (0x801F7538)
// on every call, so this follows the player through a door with no latch to go
// stale -- which the old heuristic here could not do. That was the byte at
// 0x8020876c, picked by elimination because it toggled 0 -> 1 -> 0 across one
// in/out cycle; it turned out to read 1 inside a robot as soon as anything
// updated, flipping the rotation off in the middle of a section.
//
// It still does not cover piloting the little guy, which is a context WITHIN
// an 04 level rather than a level of its own, so F5 keeps forcing any of the
// four.
static unsigned coop_rotate_now(void) {
    extern int video_coop_rot_mode(void);
    extern int robox_level_is_robot(void);
    int mode = video_coop_rot_mode();
    if (mode > 0) return (unsigned)(mode - 1) & 3u;
    return robox_level_is_robot() ? 0u : 1u;
}

static uint32_t dirs_to_actions(uint32_t dirs, uint32_t trig, unsigned q) {
    uint32_t m = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const struct dir_map *e = &ACT_CYCLE[(DIR_TO_CYCLE[i] + q) & 3u];
        if (dirs & (1u << i)) m |= e->hold;
        if (trig & (1u << i)) m |= e->trig;
    }
    return m;
}

// Recomputed from live key state every frame -- never carried over. The only
// thing retained is last frame's held set, and only because trig/edge bits
// are defined as a difference against it. That retained state is reset
// whenever the player is rebuilt (see coop_refresh_player): a key held across
// a zone change must not fake a press, or swallow a real one, in the new one.
static uint32_t g_p2_mask;
static uint32_t g_p1_prev_dirs, g_p2_prev_dirs, g_p2_prev_btn;

static void coop_reset_input_edges(void) {
    g_p1_prev_dirs = g_p2_prev_dirs = g_p2_prev_btn = 0;
}

static uint32_t coop_build_p2_mask(void) {
    extern uint32_t video_input_p2_dirs(void);
    extern uint32_t video_input_p2_btn(void);
    extern int      video_input_p2_shake(void);

    uint32_t d = video_input_p2_dirs(), b = video_input_p2_btn();
    uint32_t dtrig = d & ~g_p2_prev_dirs, btrig = b & ~g_p2_prev_btn;
    g_p2_prev_dirs = d; g_p2_prev_btn = b;

    uint32_t m = dirs_to_actions(d, dtrig, coop_rotate_now());
    if (b     & 0x0100u) m |= ACT_TWO;
    if (b     & 0x0200u) m |= ACT_ONE;
    if (btrig & 0x0100u) m |= ACT_TWO_T;
    if (btrig & 0x0200u) m |= ACT_ONE_T;

    // Shake is a level in the original (recomputed from the current accel
    // magnitude each frame), not an edge -- so hold-to-repeat is faithful.
    if (video_input_p2_shake()) m |= ACT_SHAKE;

    return m;
}

// Player 1: rewrite only the direction (and tilt) bits of the action word
// the game just computed, leaving every other bit -- and hle_KPADRead, and
// the whole KPAD path -- untouched. When no movement key is held we leave
// the word entirely alone, so a real controller still drives the game
// normally through its own route.
// Returns 1 if it modified the word, so the caller can put it back.
//
// The override MUST be reverted immediately after the player's update runs.
// That one word is the game's universal input state -- menus, pause and HUD
// all read it through func_80063a04 -- so leaving it rewritten meant every
// menu saw rotated directions instead of the keys actually pressed. Scoping
// it to the player's own update keeps the rotation where it belongs (world
// movement) and leaves everything else reading raw input.
static int coop_apply_p1_input(void) {
    extern uint32_t video_input_p1_dirs(void);
    extern int      video_input_p1_tilt(void);

    uint32_t d = video_input_p1_dirs();
    uint32_t trig = d & ~g_p1_prev_dirs;
    g_p1_prev_dirs = d;

    int tilt = video_input_p1_tilt();
    if (!d && !tilt) return 0;

    uint32_t va = input_mask_va();
    uint32_t m  = MEM_R32(va) & ~(ACT_DIR_HOLD | ACT_DIR_TRIG);
    m |= dirs_to_actions(d, trig, coop_rotate_now());
    if      (tilt < 0) m |= ACT_TILT_L;
    else if (tilt > 0) m |= ACT_TILT_R;
    MEM_W32(va, m);
    return 1;
}

static void coop_reset_fields(void);
static void coop_reset_input_edges(void);

// The player entity is rebuilt on every zone change: the level tears down,
// a fresh type-0x32 entity spawns and re-registers, and 0x801FACE0 is
// repointed at it. So the singleton must be re-read, never cached across
// frames -- and P2, being a clone of one specific object, has to be rebuilt
// from the new player or it would go on ticking a freed one.
//
// Must not run while P2's injected tick has the singleton swapped, or the
// clone would be mistaken for the player. Callers guard on g_p2_ticking.
static void coop_refresh_player(void) {
    uint32_t cur = robox_coop_player();
    if (cur == g_player_obj) return;

    uint32_t prev = g_player_obj;
    g_player_obj = cur;
    g_p2_obj = 0;                 // stale clone; rebuilt on the next update

    // Nothing derived from the old player survives the swap: the field-hunt
    // baseline described a different object, and retained input edges would
    // fire against the new one.
    coop_reset_fields();
    coop_reset_input_edges();

    if (!cur) return;             // between zones: nothing to do yet

    uint32_t vt = MEM_R32(cur);
    fprintf(stderr, "[COOP] player changed 0x%08x -> 0x%08x (vtable 0x%08x)%s\n",
            prev, cur, vt,
            (g_player_vtable && vt != g_player_vtable) ? "  *** VTABLE DIFFERS"
                                                       : "");
    fflush(stderr);
}

// Probe: hunt the in-robot flag by elimination.
//
// The object and vtable are identical in both modes (proven across a full
// in/out cycle: same object, same vtable 0x801cf440, same type 0x32), so the
// distinction has to be a field on the player. Position and physics churn
// every frame; a mode flag flips a handful of times in a whole session.
//
// So: run a silent warm-up counting how often each word changes, then report
// only changes in words that stayed quiet throughout. After warm-up almost
// nothing should print -- and getting in or out of the robot should stand out
// as a small, specific set of offsets.
#define COOP_WARMUP_FRAMES 600u   /* ~10s; long enough to saturate physics */
#define COOP_QUIET_CHURN   4u     /* a real mode flag stays under this */

static uint32_t g_field_prev[SNAP_WORDS];
static uint16_t g_field_churn[SNAP_WORDS];
static uint32_t g_field_warm;
static int      g_field_primed;

static void coop_reset_fields(void) { g_field_primed = 0; g_field_warm = 0; }

static void coop_watch_fields(void) {
    if (!g_player_obj) return;

    if (!g_field_primed) {
        for (unsigned i = 0; i < SNAP_WORDS; ++i) {
            g_field_prev[i]  = MEM_R32(g_player_obj + i * 4u);
            g_field_churn[i] = 0;
        }
        g_field_primed = 1;
        return;
    }

    int warm = g_field_warm < COOP_WARMUP_FRAMES;
    if (warm) ++g_field_warm;

    for (unsigned i = 0; i < SNAP_WORDS; ++i) {
        uint32_t w = MEM_R32(g_player_obj + i * 4u);
        if (w == g_field_prev[i]) continue;
        if (!warm && g_field_churn[i] <= COOP_QUIET_CHURN) {
            fprintf(stderr, "[COOP] FIELD +0x%03x: 0x%08x -> 0x%08x  (%.3f -> %.3f)\n",
                    i * 4u, g_field_prev[i], w,
                    (double)as_f32(g_field_prev[i]), (double)as_f32(w));
            fflush(stderr);
        }
        if (g_field_churn[i] < 0xffffu) ++g_field_churn[i];
        g_field_prev[i] = w;
    }
}

// Second hunt, over GLOBALS instead of the player object.
//
// The object, vtable and type are byte-identical on foot and in the robot,
// so the orientation is far more likely a property of the section than of
// the entity. Same elimination, applied to two windows: the SDA neighbourhood
// of the player/input globals, and the byte-flag block the player update
// gates on (it reads 0x80208780 / 0x80208790 before doing anything).
//
// Unlike the object watcher this deliberately NEVER resets. Globals outlive
// zone changes, and resetting would blind the hunt for ten seconds after
// every transition -- precisely when a section flag is most likely to flip.
#define GLOB_WORDS 0x100u

struct coop_win {
    uint32_t base;            /* 0 = resolve lazily from r13 */
    int32_t  sda_off;         /* used when base is 0 */
    const char *label;
    uint32_t prev [GLOB_WORDS];
    uint16_t churn[GLOB_WORDS];
    uint32_t warm;
    int      primed;
};

static struct coop_win g_wins[2] = {
    { 0u,          -0x4800, "sda",  {0}, {0}, 0, 0 },
    { 0x80208600u,  0,      "flag", {0}, {0}, 0, 0 },
};

static void coop_watch_globals(void) {
    for (unsigned k = 0; k < 2; ++k) {
        struct coop_win *w = &g_wins[k];
        uint32_t base = w->base ? w->base
                                : (g_cpu.gpr[13] + (uint32_t)w->sda_off);
        if (!va_ok(base)) continue;

        if (!w->primed) {
            for (unsigned i = 0; i < GLOB_WORDS; ++i)
                w->prev[i] = MEM_R32(base + i * 4u);
            w->primed = 1;
            continue;
        }

        int warm = w->warm < COOP_WARMUP_FRAMES;
        if (warm) ++w->warm;

        for (unsigned i = 0; i < GLOB_WORDS; ++i) {
            uint32_t v = MEM_R32(base + i * 4u);
            if (v == w->prev[i]) continue;
            if (!warm && w->churn[i] <= COOP_QUIET_CHURN) {
                fprintf(stderr, "[COOP] GLOB %s 0x%08x: 0x%08x -> 0x%08x\n",
                        w->label, base + i * 4u, w->prev[i], v);
                fflush(stderr);
            }
            if (w->churn[i] < 0xffffu) ++w->churn[i];
            w->prev[i] = v;
        }
    }
}

// Probe: find what distinguishes on-foot from in-robot sections, so the
// d-pad rotation can stop being a manual F5 toggle. Logs the player's
// state-machine value (+0x240) and the singleton pointer whenever either
// changes -- entering or leaving the robot should show up as a discrete
// event in one of them. Covers both plausible shapes: a state transition on
// the same object, or the singleton being repointed at a different entity.
static void coop_watch_mode(void) {
    static uint32_t last_state = 0xffffffffu, last_player;
    uint32_t st = MEM_R32(g_player_obj + 0x240u);
    uint32_t pl = MEM_R32(player_ptr_va());
    if (st == last_state && pl == last_player) return;
    fprintf(stderr, "[COOP] MODE state=%u (was %d)  player=0x%08x  type=0x%x\n",
            st, (int)last_state, pl, MEM_R32(g_player_obj + ENT_TYPE_OFF));
    fflush(stderr);
    last_state  = st;
    last_player = pl;
}

// Run one guest virtual on P2 with the globals swapped. `slot` indexes the
// captured originals, so this never re-enters our own trampolines.
static void p2_run_slot(unsigned slot) {
    if (!g_vt_orig[slot]) return;

    uint32_t pslot = player_ptr_va(), islot = input_mask_va();
    uint32_t save_player = MEM_R32(pslot);
    uint32_t save_input  = MEM_R32(islot);
    uint32_t save_lr = g_cpu.lr, save_r1 = g_cpu.gpr[1];

    MEM_W32(pslot, g_p2_obj);
    // ROBOX_COOP_MIRROR=1 falls back to driving P2 with P1's input, which is
    // the milestone-1a behaviour and a useful A/B when P2 misbehaves.
    MEM_W32(islot, g_coop_mirror ? save_input : g_p2_mask);

    g_cpu.gpr[3] = g_p2_obj;
    if (slot == 4u) { g_cpu.gpr[4] = g_vt4_r4; g_cpu.gpr[5] = g_vt4_r5; }
    g_vt_orig[slot]();

    MEM_W32(pslot, save_player);
    MEM_W32(islot, save_input);
    g_cpu.lr = save_lr;
    g_cpu.gpr[1] = save_r1;
}

// Called at the tail of each trampoline, after P1's own call returned.
static void coop_after_slot(unsigned n, uint32_t self) {
    if (!g_coop_on || g_p2_ticking) return;
    if (!g_player_obj || self != g_player_obj) return;
    if (n != 3u && n != 4u && n != 20u) return;   // update, draw, generic tick

    // Spawn lazily on the first real update rather than during entity init:
    // at init time the object is still being constructed, so a clone taken
    // then would capture a half-built player.
    if (!g_p2_obj) {
        if (n != 3u) return;
        coop_spawn_p2();
        return;                      // let P1 finish this frame first
    }

    // Sample P2's controls once per frame, on the update, so all three
    // injected calls in a frame see one consistent input snapshot (and the
    // trig/edge bits fire exactly once).
    if (n == 3u) {
        g_p2_mask = coop_build_p2_mask();
        if (probe_enabled()) {
            coop_watch_mode();
            coop_watch_fields();
            coop_watch_globals();
        }
    }

    g_p2_ticking = 1;
    p2_run_slot(n);
    g_p2_ticking = 0;

    // Heartbeat: tells "P2 is simulating but not visible" apart from "P2 is
    // not running", which are very different bugs to chase.
    if (n == 3u) {
        static uint32_t ticks;
        if (++ticks % 300u == 0u) {
            fprintf(stderr, "[COOP] tick %u  P1=(%.1f, %.1f) state=%u  "
                            "P2=(%.1f, %.1f) state=%u\n",
                    ticks,
                    (double)mem_rf(g_player_obj + OFF_POS_X),
                    (double)mem_rf(g_player_obj + OFF_POS_Y),
                    MEM_R32(g_player_obj + 0x240u),
                    (double)mem_rf(g_p2_obj + OFF_POS_X),
                    (double)mem_rf(g_p2_obj + OFF_POS_Y),
                    MEM_R32(g_p2_obj + 0x240u));
            fflush(stderr);
        }
    }
}

// `watch` must be decided up front: the original clobbers r3.
#define COOP_TRAMP(n)                                              \
    static void coop_vt_tramp_##n(void) {                          \
        uint32_t self = g_cpu.gpr[3];                              \
        if (!g_p2_ticking) coop_refresh_player();                  \
        int watch = g_player_obj && self == g_player_obj           \
                    && watch_slot(n);                              \
        if (n == 4u && self == g_player_obj) {                     \
            g_vt4_r4 = g_cpu.gpr[4]; g_vt4_r5 = g_cpu.gpr[5];      \
        }                                                          \
        if (watch) snap_take(g_player_obj);                        \
        vt_tally(n);                                               \
        uint32_t in_save = 0; int in_over = 0;                      \
        if (n == 3u && g_coop_on && self == g_player_obj           \
            && !g_p2_ticking) {                                    \
            in_save = MEM_R32(input_mask_va());                    \
            in_over = coop_apply_p1_input();                       \
        }                                                          \
        if (g_vt_orig[n]) g_vt_orig[n]();                          \
        if (in_over) MEM_W32(input_mask_va(), in_save);            \
        if (watch) snap_diff(n, g_player_obj);                     \
        coop_after_slot(n, self);                                  \
    }

COOP_TRAMP(1)  COOP_TRAMP(2)  COOP_TRAMP(3)  COOP_TRAMP(4)  COOP_TRAMP(5)
COOP_TRAMP(6)  COOP_TRAMP(7)  COOP_TRAMP(8)  COOP_TRAMP(9)  COOP_TRAMP(10)
COOP_TRAMP(11) COOP_TRAMP(12) COOP_TRAMP(13) COOP_TRAMP(14) COOP_TRAMP(15)
COOP_TRAMP(16) COOP_TRAMP(17) COOP_TRAMP(18) COOP_TRAMP(19) COOP_TRAMP(20)
COOP_TRAMP(21) COOP_TRAMP(22) COOP_TRAMP(23)

static const PPC_Func g_tramps[VT_SLOTS] = {
    0,                 coop_vt_tramp_1,  coop_vt_tramp_2,  coop_vt_tramp_3,
    coop_vt_tramp_4,   coop_vt_tramp_5,  coop_vt_tramp_6,  coop_vt_tramp_7,
    coop_vt_tramp_8,   coop_vt_tramp_9,  coop_vt_tramp_10, coop_vt_tramp_11,
    coop_vt_tramp_12,  coop_vt_tramp_13, coop_vt_tramp_14, coop_vt_tramp_15,
    coop_vt_tramp_16,  coop_vt_tramp_17, coop_vt_tramp_18, coop_vt_tramp_19,
    coop_vt_tramp_20,  coop_vt_tramp_21, coop_vt_tramp_22, coop_vt_tramp_23,
};

static void vt_report(void) {
    fprintf(stderr, "\n[COOP] --- vtable call histogram (this==player | others) ---\n");
    for (unsigned i = 0; i < VT_SLOTS; ++i) {
        if (!g_vt_addr[i]) continue;
        if (!g_vt_self[i] && !g_vt_other[i]) continue;
        fprintf(stderr, "[COOP]   vt[%2u] 0x%08x  player=%-8u other=%u\n",
                i, g_vt_addr[i], g_vt_self[i], g_vt_other[i]);
    }
    mutation_report();
    fprintf(stderr, "[COOP] ------------------------------------------------------\n\n");
    fflush(stderr);
}

static void arm_vtable_profile(uint32_t vt) {
    if (g_vt_armed || !va_ok(vt)) return;
    g_vt_armed = 1;
    for (unsigned i = 1; i < VT_SLOTS; ++i) {
        uint32_t fn = MEM_R32(vt + i * 4u);
        if (!fn || !is_known_func(fn)) continue;
        g_vt_addr[i] = fn;
        g_vt_orig[i] = ppc_lookup_func(fn);
        ppc_patch_func(fn, g_tramps[i]);
    }
    fprintf(stderr, "[COOP] vtable profiling armed on %u slots\n", VT_SLOTS - 1);
    fflush(stderr);
}

static void dump_player_class(uint32_t self, uint32_t blob) {
    uint32_t vt = MEM_R32(self);
    g_player_vtable = vt;
    g_player_obj   = self;

    if (!probe_enabled()) {          // co-op only: arm, stay quiet
        fprintf(stderr, "[COOP] player=0x%08x vtable=0x%08x\n", self, vt);
        fflush(stderr);
        arm_vtable_profile(vt);
        return;
    }

    fprintf(stderr, "\n[COOP] ===== PLAYER ENTITY REGISTERED =====\n");
    fprintf(stderr, "[COOP] this=0x%08x  vtable=0x%08x  type=0x%x  blob=0x%08x\n",
            self, vt, MEM_R32(self + ENT_TYPE_OFF), blob);
    fprintf(stderr, "[COOP] singleton slot 0x%08x now = 0x%08x\n",
            player_ptr_va(), robox_coop_player());

    if (!va_ok(vt)) {
        fprintf(stderr, "[COOP] vtable VA out of range -- not dumping slots\n");
        fflush(stderr);
        return;
    }

    // CodeWarrior vtables carry an offset-to-top / RTTI pair before the first
    // method on some hierarchies, so slot 0 is not always a function. Report
    // what each word actually is rather than assuming.
    fprintf(stderr, "[COOP] vtable slots (only real entries are callable):\n");
    for (unsigned i = 0; i < VT_SLOTS; ++i) {
        uint32_t slot = MEM_R32(vt + i * 4u);
        if (slot == 0) continue;
        fprintf(stderr, "[COOP]   vt[%2u] (+0x%02x) = 0x%08x  %s\n",
                i, i * 4u, slot,
                is_known_func(slot) ? "func_ (in table)" : "<not a known func>");
    }

    // The spawn-param blob is what we would have to synthesise (or copy) to
    // build a second player through the game's own spawner. Dump enough to
    // recognise its shape; func_800b3144 reads a type at blob[off], a bitmask
    // at blob[off+4], then pairs of ints it converts to floats.
    if (va_ok(blob)) {
        fprintf(stderr, "[COOP] spawn blob[0x00..0x5f]:\n");
        for (unsigned r = 0; r < 6; ++r) {
            fprintf(stderr, "[COOP]   +0x%02x:", r * 16u);
            for (unsigned c = 0; c < 16; ++c)
                fprintf(stderr, " %02x", MEM_R8(blob + r * 16u + c));
            fprintf(stderr, "\n");
        }
    }
    // Allocator header immediately below the object. Cloning P2 needs the
    // block size, and CW heaps keep it in the header rather than anywhere
    // reachable from the object itself.
    fprintf(stderr, "[COOP] alloc header [-0x20..0):");
    for (unsigned i = 0; i < 8; ++i)
        fprintf(stderr, " %08x", MEM_R32(self - 0x20u + i * 4u));
    fprintf(stderr, "\n");

    fprintf(stderr, "[COOP] ======================================\n\n");
    fflush(stderr);

    arm_vtable_profile(vt);
}

// Replacement for func_800b3144 installed into the dispatch table.
// Runs the real init first (so entity+0x1d8 and the singleton are already
// set), then inspects the result. Args must be captured up front: the real
// function clobbers r3/r4.
static void coop_ent_init_hook(void) {
    uint32_t self = g_cpu.gpr[3];
    uint32_t blob = g_cpu.gpr[4];

    func_800b3144();

    if (g_dumped_player || !va_ok(self)) return;
    if (MEM_R32(self + ENT_TYPE_OFF) != PLAYER_TYPE_ID) return;

    g_dumped_player = 1;
    // dump_player_class also arms the vtable trampolines, which capture the
    // originals that Phase 1 calls on P2 -- so it runs even when only the
    // co-op flag is set and the diagnostics are not wanted.
    dump_player_class(self, blob);
}

// --- Init ------------------------------------------------------------------

// Called by the mod loader only when this mod is enabled, so there is no
// env check here any more -- enablement is the registry's job.
void robox_coop_init(void) {
    g_coop_on = 1;
    const char *m = getenv("ROBOX_COOP_MIRROR");
    g_coop_mirror = m && *m && *m != '0';

    ppc_patch_func(ENT_INIT_VA, coop_ent_init_hook);
    fprintf(stderr, "[COOP] armed: entity-init 0x%08x hooked, watching for "
                    "spawn type 0x%x (probe=%d coop=%d)\n",
            ENT_INIT_VA, PLAYER_TYPE_ID, probe_enabled(), g_coop_on);
    fflush(stderr);
}
