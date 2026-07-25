/*
================================================================================
 mario.h -- the Super Mario Bros. player character in one portable C file
================================================================================

 Everything that makes Mario feel like Mario, extracted from the original
 NES Super Mario Bros. game logic (via the doppelganger disassembly /
 SuperMarioBros-C translation) and re-expressed as clean, dependency-free C.

 This is not "inspired by" SMB physics -- it *is* SMB physics, bit-exact:

   * 8.8 fixed-point positions, 4.4 fixed-point horizontal velocity
   * walk/run acceleration ($98/$E4), release deceleration ($D0),
     skid (friction doubled), 10-frame B-button run grace timer
   * speed-dependent jump: 5 jump strength tiers picked from your takeoff
     speed, variable height from holding A (hold gravity $20 vs $70 falling)
   * air control rules (no passive air friction, walk-speed accel cap in
     air unless you took off at run speed, RunningSpeed carryover)
   * the skid-turnaround snap, the crouch rules, moving/facing direction
   * swimming (stroke gating, surface clamp, water X physics)
   * SMB collision: exact sensor points, head-bump nybble rule, the
     5-pixel landing tolerance, corner "impede" push-out, per-direction
     input masking (Player_CollisionBits)
   * the walk-cycle animation timing and the exact NES sprite tables

 Original routine names are kept in comments so you can diff this against
 the disassembly (PlayerPhysicsSub, ImposeFriction, MovePlayerHorizontally,
 ImposeGravity, PlayerBGCollision, GetPlayerAnimSpeed, ...).

 ------------------------------------------------------------------------------
 USAGE (stb-style single header):

     #define MARIO_IMPLEMENTATION
     #include "mario.h"

     uint8_t my_solid(void *user, int tx, int ty) {   // tile query callback
         return my_level_is_solid(tx, ty);            // 0 = empty, 1 = solid
     }

     Mario m;
     mario_init(&m, 40, 192);        // x in world pixels, y in playfield px
     m.tile_at = my_solid;
     m.user    = &my_level;

     // exactly once per 60Hz frame:
     mario_tick(&m, MARIO_JOY_RIGHT | MARIO_JOY_A);
     if (m.events & MARIO_EV_JUMP) play_jump_sound(m.size);

     MarioDraw d;
     mario_get_draw(&m, &d);         // 8 NES CHR tile ids + flip + position
     // ...or ignore the tiles and use d.action/d.frame with your own art.

 COORDINATES
     x: world pixels, grows right, unbounded (int32).
     y: screen-space pixels of the sprite TOP, exactly like the NES game:
        the tile playfield starts at y=32 (the status bar band) and is 13
        rows tall (y=32..239). Mario's feet are at y+32. y<0 means "above
        the screen" (allowed, collision off, just like SMB). y>=256 means
        "fell below the screen" (pit).
     tile_at() receives tx = world_x>>4, ty = (screen_y-32)>>4, so a flat
     floor at the bottom of the screen is ty == 12.

 WHAT'S NOT HERE (on purpose): enemies, blocks that bounce, fireballs,
 vines/climbing, pipes/flagpole -- that's level logic, not the player.
 Head bumps, wall pushes, landings etc. are reported via m.events so the
 host can implement those.
================================================================================
*/

#ifndef MARIO_H_INCLUDED
#define MARIO_H_INCLUDED

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- joypad bits (NES layout, same values SMB uses) ---------------------- */
#define MARIO_JOY_A      0x80u
#define MARIO_JOY_B      0x40u
#define MARIO_JOY_UP     0x08u
#define MARIO_JOY_DOWN   0x04u
#define MARIO_JOY_LEFT   0x02u
#define MARIO_JOY_RIGHT  0x01u

/* ---- events reported by mario_tick (bitfield, valid until next tick) ----- */
#define MARIO_EV_JUMP        0x01u  /* jumped (pick sfx by m.size)            */
#define MARIO_EV_SWIM_STROKE 0x02u  /* swim stroke (SMB plays the stomp sfx) */
#define MARIO_EV_BUMP_HEAD   0x04u  /* head hit a solid tile (bump sfx)      */
#define MARIO_EV_LAND        0x08u  /* feet landed on ground this frame      */
#define MARIO_EV_WALL        0x10u  /* pushed out of a wall this frame       */
#define MARIO_EV_FELL        0x20u  /* fell below the screen (pit)           */
#define MARIO_EV_FIREBALL    0x40u  /* threw a fireball (fireball sfx)       */
#define MARIO_EV_FB_EXPLODE  0x80u  /* a fireball exploded (bump sfx)        */

/* head-bump info: tile the head hit (valid when MARIO_EV_BUMP_HEAD set) */

/* ---- player states (Player_State) ---------------------------------------- */
#define MARIO_ST_GROUND   0u
#define MARIO_ST_JUMP     1u   /* jumping or swimming */
#define MARIO_ST_FALL     2u
#define MARIO_ST_CLIMB    3u   /* reserved; climbing not implemented */

/* ---- size ----------------------------------------------------------------- */
#define MARIO_BIG    0u        /* PlayerSize: 0 = big (16x32 sprite box)     */
#define MARIO_SMALL  1u        /* 1 = small (art in lower 16x16 of the box)  */

/* ---- animation actions (which pose mario_get_draw chose) ------------------ */
enum {
    MARIO_ACT_JUMP    = 0,     /* indexes into PlayerGfxTblOffsets           */
    MARIO_ACT_SWIM    = 1,
    MARIO_ACT_STAND   = 2,
    MARIO_ACT_SKID    = 3,
    MARIO_ACT_WALK    = 4,
    MARIO_ACT_CLIMB   = 5,
    MARIO_ACT_CROUCH  = 6,
    MARIO_ACT_THROW   = 7
};

/* tile query: return nonzero if the tile at (tx,ty) is solid.
   tx = world_pixel_x >> 4;  ty = (screen_pixel_y - 32) >> 4  (0..12).      */
typedef uint8_t (*mario_tile_fn)(void *user, int32_t tx, int32_t ty);

typedef struct MarioDraw {
    uint8_t tiles[8];   /* NES CHR tile ids in final screen layout: for each
                           of 4 rows, [left sprite, right sprite]; row n at
                           (0|8, n*8) in the 16x32 box; 0xFC = blank        */
    uint8_t hflip[8];   /* per-sprite horizontal mirror, fully resolved
                           (facing + the symmetric-pose OAM attribute fix)  */
    uint8_t flip;       /* facing-left flag (informational)                 */
    int32_t x, y;       /* world position of the 16x32 sprite box top-left  */
    uint8_t action;     /* MARIO_ACT_* actually displayed                   */
    uint8_t frame;      /* animation frame within the action (0..2)         */
} MarioDraw;

/* fireballs (fire Mario) -- two slots, like the real game                   */
typedef struct MarioFireball {
    uint8_t state;      /* Fireball_State: 0 off, 1 flying, 2 spawning,
                           >= 0x80 exploding                                */
    int32_t x;          /* world px                                         */
    uint8_t x_sub;
    int8_t  x_spd;      /* 4.4 fixed: +-0x40 = +-4 px/frame                 */
    int32_t y;          /* screen px                                        */
    int8_t  y_spd;
    uint8_t y_frac, ymf_dummy;
    uint8_t bouncing;   /* FireballBouncingFlag                             */
} MarioFireball;

typedef struct MarioFireballDraw {
    uint8_t active;     /* 0 = don't draw                                   */
    uint8_t exploding;  /* 0 = ball (one 8x8 tile), 1 = boom (16x16)        */
    uint8_t tile;       /* ball tile ($64/$65) or explosion tile            */
    uint8_t hflip, vflip; /* for the ball tile only                         */
    int32_t x, y;       /* ball: tile top-left; boom: center point          */
} MarioFireballDraw;

typedef struct Mario {
    /* --- position / motion (SMB variable names in comments) --- */
    int32_t x;          /* Player_PageLoc:Player_X_Position (world pixels)  */
    uint8_t x_sub;      /* SprObject_X_MoveForce ($0400): POSITION subpixel */
    uint8_t x_frac;     /* Player_X_MoveForce ($0705): VELOCITY fraction --
                           the low byte of the 16-bit friction accumulator.
                           These really are two separate variables in SMB.  */
    int8_t  x_spd;      /* Player_X_Speed       (4.4 fixed px/frame)        */
    int32_t y;          /* Player_Y_HighPos:Player_Y_Position (screen px)   */
    int8_t  y_spd;      /* Player_Y_Speed       (whole px/frame)            */
    uint8_t y_frac;     /* Player_Y_MoveForce   (velocity fraction 1/256)   */
    uint8_t ymf_dummy;  /* Player_YMF_Dummy     (position fraction accum)   */

    uint8_t state;      /* Player_State: MARIO_ST_*                         */
    uint8_t size;       /* PlayerSize: MARIO_BIG / MARIO_SMALL              */
    uint8_t facing;     /* PlayerFacingDir: 1=right 2=left                  */
    uint8_t moving_dir; /* Player_MovingDir:  1=right 2=left                */
    uint8_t x_spd_abs;  /* Player_XSpeedAbsolute                            */
    uint8_t running_spd;/* RunningSpeed flag                                */
    uint8_t crouching;  /* CrouchingFlag                                    */
    uint8_t swimming;   /* SwimmingFlag (host sets for water areas)         */
    uint8_t whirlpool;  /* Whirlpool_Flag                                   */
    uint8_t coll_bits;  /* Player_CollisionBits (dir input mask)            */
    uint8_t fire;       /* PlayerStatus fiery (host sets; implies big)      */

    /* --- jump bookkeeping --- */
    uint8_t jump_origin_y;  /* JumpOrigin_Y_Position (low byte)             */
    uint8_t diff_to_halt;   /* DiffToHaltJump                               */
    uint8_t vforce;         /* VerticalForce      (current gravity, 1/256)  */
    uint8_t vforce_down;    /* VerticalForceDown  (release/fall gravity)    */
    uint8_t max_left;       /* MaximumLeftSpeed  (negative, e.g. 0xD8)      */
    uint8_t max_right;      /* MaximumRightSpeed                            */
    uint8_t fric_lo;        /* FrictionAdderLow                             */
    uint8_t fric_hi;        /* FrictionAdderHigh                            */

    /* --- frame timers (auto-decremented, like the $0780 timer block) --- */
    uint8_t anim_timer;     /* PlayerAnimTimer                              */
    uint8_t jumpswim_timer; /* JumpSwimTimer                                */
    uint8_t running_timer;  /* RunningTimer (10-frame B grace)              */
    uint8_t sidecoll_timer; /* SideCollisionTimer                           */

    /* --- animation --- */
    uint8_t anim_timer_set; /* PlayerAnimTimerSet                           */
    uint8_t anim_ctrl;      /* PlayerAnimCtrl (frame within cycle)          */
    uint8_t gfx_action;     /* chosen MARIO_ACT_* this frame                */
    uint8_t gfx_offset;     /* PlayerGfxOffset into PlayerGraphicsTable     */
    uint8_t frame_ctr;      /* FrameCounter (for the swim kick flutter)     */

    /* --- controller shadow --- */
    uint8_t joy;            /* SavedJoypadBits                              */
    uint8_t joy_ab;         /* A_B_Buttons                                  */
    uint8_t joy_prev_ab;    /* PreviousA_B_Buttons                          */
    uint8_t joy_lr;         /* Left_Right_Buttons                           */
    uint8_t joy_ud;         /* Up_Down_Buttons                              */

    /* --- fireballs (used when fire is set) --- */
    MarioFireball fb[2];
    uint8_t fireball_counter; /* FireballCounter (alternates slots)         */
    uint8_t fb_timer;         /* FireballThrowingTimer (throw pose)         */

    /* --- host interface --- */
    mario_tile_fn tile_at;
    void   *user;
    uint8_t events;         /* MARIO_EV_* set during the last tick          */
    int32_t bump_tx, bump_ty; /* tile coords the head bumped (EV_BUMP_HEAD) */
} Mario;

void mario_init(Mario *m, int32_t x, int32_t y);
void mario_tick(Mario *m, uint8_t joypad);
void mario_get_draw(const Mario *m, MarioDraw *d);
void mario_fireball_draw(const Mario *m, int slot, MarioFireballDraw *d);
void mario_fireball_cull(Mario *m, int32_t view_left, int32_t view_right);
const uint8_t *mario_palette(const Mario *m); /* 4 NES colors (PlayerColors) */

#ifdef __cplusplus
}
#endif

#endif /* MARIO_H_INCLUDED */

/* ========================================================================== */
/* ============================ IMPLEMENTATION ============================== */
/* ========================================================================== */
#ifdef MARIO_IMPLEMENTATION

/* --------------------------------------------------------------------------
   The sacred numbers. Byte-for-byte from the SMB ROM data tables.
   Jump/gravity index: picked from |x speed| at takeoff:
     <$09 -> 0, <$10 -> 1, <$19 -> 2, <$1C -> 3, else 4; swim=5, whirlpool=6
   -------------------------------------------------------------------------- */
static const uint8_t MARIO__JumpMForceData[7] =   /* gravity while A held    */
    { 0x20, 0x20, 0x1e, 0x28, 0x28, 0x0d, 0x04 };
static const uint8_t MARIO__FallMForceData[7] =   /* gravity when falling    */
    { 0x70, 0x70, 0x60, 0x90, 0x90, 0x0a, 0x09 };
static const uint8_t MARIO__PlayerYSpdData[7] =   /* initial jump Y speed    */
    { 0xfc, 0xfc, 0xfc, 0xfb, 0xfb, 0xfe, 0xff };
static const uint8_t MARIO__InitMForceData[7] =   /* initial Y speed frac    */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00 };
static const uint8_t MARIO__MaxLeftXSpdData[3] =  /* -40, -24, -16 (4.4)     */
    { 0xd8, 0xe8, 0xf0 };
static const uint8_t MARIO__MaxRightXSpdData[3] = /*  40,  24,  16 (4.4)     */
    { 0x28, 0x18, 0x10 };
static const uint8_t MARIO__FrictionData[3] =     /* run, walk, decel adders */
    { 0xe4, 0x98, 0xd0 };
static const uint8_t MARIO__PlayerAnimTmrData[3] =/* walk anim frame timing  */
    { 0x02, 0x04, 0x07 };

/* collision sensor points, {x,y} offsets from the sprite box top-left.
   From BlockBuffer_X_Adder/Y_Adder: base 0 = big, 7 = swimming, 14 = small
   or crouching. Order: head, foot L, foot R, side UL, side LL, side UR,
   side LR.                                                                  */
static const uint8_t MARIO__SensorX[3][7] = {
    { 8, 3, 12, 2, 2, 13, 13 },      /* big                                  */
    { 8, 3, 12, 2, 2, 13, 13 },      /* swimming                             */
    { 8, 3, 12, 2, 2, 13, 13 },      /* small / crouching                    */
};
static const uint8_t MARIO__SensorY[3][7] = {
    { 0x04, 0x20, 0x20, 0x08, 0x18, 0x08, 0x18 },
    { 0x02, 0x20, 0x20, 0x08, 0x18, 0x08, 0x18 },
    { 0x12, 0x20, 0x20, 0x18, 0x18, 0x18, 0x18 },
};
static const uint8_t MARIO__UpperExtent[2] = { 0x20, 0x10 }; /* head-check gate */

/* PlayerGfxTblOffsets: offset into PlayerGraphicsTable per action,
   big first 8, small next 8.                                                */
static const uint8_t MARIO__GfxTblOffsets[16] = {
    0x20, 0x28, 0xc8, 0x18, 0x00, 0x40, 0x50, 0x58,
    0x80, 0x88, 0xb8, 0x78, 0x60, 0xa0, 0xb0, 0xb8
};

/* PlayerGraphicsTable: 8 CHR tiles per pose, column pairs by row (L,R).
   (The ROM stores them in the same order the OAM rows are written.)         */
static const uint8_t MARIO__GraphicsTable[27 * 8] = {
    /* big player */
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,  /* walking frame 1   ($00) */
    0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,  /*         frame 2   ($08) */
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,  /*         frame 3   ($10) */
    0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,  /* skidding          ($18) */
    0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,  /* jumping           ($20) */
    0x08,0x09,0x28,0x29,0x2a,0x2b,0x2c,0x2d,  /* swimming frame 1  ($28) */
    0x08,0x09,0x0a,0x0b,0x0c,0x30,0x2c,0x2d,  /*          frame 2  ($30) */
    0x08,0x09,0x0a,0x0b,0x2e,0x2f,0x2c,0x2d,  /*          frame 3  ($38) */
    0x08,0x09,0x28,0x29,0x2a,0x2b,0x5c,0x5d,  /* climbing frame 1  ($40) */
    0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x5e,0x5f,  /*          frame 2  ($48) */
    0xfc,0xfc,0x08,0x09,0x58,0x59,0x5a,0x5a,  /* crouching         ($50) */
    0x08,0x09,0x28,0x29,0x2a,0x2b,0x0e,0x0f,  /* fireball throwing ($58) */
    /* small player */
    0xfc,0xfc,0xfc,0xfc,0x32,0x33,0x34,0x35,  /* walking frame 1   ($60) */
    0xfc,0xfc,0xfc,0xfc,0x36,0x37,0x38,0x39,  /*         frame 2   ($68) */
    0xfc,0xfc,0xfc,0xfc,0x3a,0x37,0x3b,0x3c,  /*         frame 3   ($70) */
    0xfc,0xfc,0xfc,0xfc,0x3d,0x3e,0x3f,0x40,  /* skidding          ($78) */
    0xfc,0xfc,0xfc,0xfc,0x32,0x41,0x42,0x43,  /* jumping           ($80) */
    0xfc,0xfc,0xfc,0xfc,0x32,0x33,0x44,0x45,  /* swimming frame 1  ($88) */
    0xfc,0xfc,0xfc,0xfc,0x32,0x33,0x44,0x47,  /*          frame 2  ($90) */
    0xfc,0xfc,0xfc,0xfc,0x32,0x33,0x48,0x49,  /*          frame 3  ($98) */
    0xfc,0xfc,0xfc,0xfc,0x32,0x33,0x90,0x91,  /* climbing frame 1  ($A0) */
    0xfc,0xfc,0xfc,0xfc,0x3a,0x37,0x92,0x93,  /*          frame 2  ($A8) */
    0xfc,0xfc,0xfc,0xfc,0x9e,0x9e,0x9f,0x9f,  /* killed            ($B0) */
    0xfc,0xfc,0xfc,0xfc,0x3a,0x37,0x4f,0x4f,  /* small standing    ($B8) */
    0xfc,0xfc,0x00,0x01,0x4c,0x4d,0x4e,0x4e,  /* intermediate grow ($C0) */
    0x00,0x01,0x4c,0x4d,0x4a,0x4a,0x4b,0x4b   /* big standing      ($C8) */
};
static const uint8_t MARIO__SwimKickTileNum[2] = { 0x31, 0x46 };

/* PlayerColors: mario, luigi, fiery (NES palette indices; [0] is bg/unused) */
static const uint8_t MARIO__PlayerColors[3][4] = {
    { 0x22, 0x16, 0x27, 0x18 },
    { 0x22, 0x30, 0x27, 0x19 },
    { 0x22, 0x37, 0x27, 0x16 },
};
static const uint8_t MARIO__FireballXSpdData[2] = { 0x40, 0xc0 };
static const uint8_t MARIO__ExplosionTiles[3]   = { 0x68, 0x67, 0x66 };

/* -------------------------------------------------------------------------- */

static uint8_t mario__solid(Mario *m, int32_t px, int32_t py,
                            int32_t *otx, int32_t *oty)
{
    /* BlockBufferCollision: tile lookup for a sensor point (world px,
       screen py). Playfield tile rows start 32px down, 16px tiles. */
    int32_t tx = px >> 4;
    int32_t ty = (py - 0x20) >> 4;
    if (otx) *otx = tx;
    if (oty) *oty = ty;
    if (!m->tile_at) return 0;
    return m->tile_at(m->user, tx, ty);
}

static uint8_t mario__sensor_base(const Mario *m)
{
    /* ChkCollSize: pick sensor set: small or crouching -> 2, swim -> 1 */
    if (m->crouching || m->size != MARIO_BIG) return 2;
    if (m->swimming) return 1;
    return 0;
}

/* === GetPlayerAnimSpeed =================================================== */
static void mario__get_anim_speed(Mario *m)
{
    uint8_t y = 0, a;
    if (m->x_spd_abs < 0x1c) {
        y = 1;
        if (m->x_spd_abs < 0x0e) y = 2;
        a = m->joy & 0x7f;                    /* everything but A            */
        if (a != 0) {
            a &= 0x03;                        /* left/right only             */
            if (a != m->moving_dir) {
                /* ProcSkid: skidding; snap turnaround at low speed */
                if (m->x_spd_abs < 0x0b) {
                    m->moving_dir = m->facing;
                    m->x_spd = 0;             /* nullify horizontal speed    */
                    m->x_frac = 0;            /* and movement force ($0705)  */
                }
            } else {
                m->running_spd = 0;
            }
        }
    } else {
        m->running_spd = m->x_spd_abs;        /* >= $1C: remember run speed  */
    }
    m->anim_timer_set = MARIO__PlayerAnimTmrData[y];
}

/* === ImposeFriction ======================================================= */
static void mario__impose_friction(Mario *m, uint8_t input_lr)
{
    uint8_t a = input_lr & m->coll_bits;
    int8_t  spd = m->x_spd;
    uint16_t adder = (uint16_t)m->fric_lo | ((uint16_t)m->fric_hi << 8);
    int8_t  maxr = (int8_t)m->max_right;
    int8_t  maxl = (int8_t)m->max_left;
    int     accel_right;

    if (a != 0) {
        accel_right = (a & MARIO_JOY_RIGHT) != 0;  /* both pressed -> right  */
    } else {
        if (spd == 0) { m->x_spd_abs = 0; return; } /* nothing to slow       */
        accel_right = (spd < 0);                   /* decelerate toward 0    */
    }

    if (accel_right) {
        /* LeftFrict: 16-bit add of (spd:x_frac) += adder, clamp to max_right */
        uint16_t lo = (uint16_t)m->x_frac + (uint16_t)(adder & 0xff);
        m->x_frac = (uint8_t)lo;
        spd = (int8_t)((uint8_t)spd + (uint8_t)(adder >> 8) + (lo >> 8));
        if ((int8_t)(spd - maxr) >= 0) spd = maxr;     /* cmp/bmi semantics  */
    } else {
        /* RghtFrict: 16-bit subtract, clamp to max_left */
        int16_t lo = (int16_t)m->x_frac - (int16_t)(adder & 0xff);
        m->x_frac = (uint8_t)lo;
        spd = (int8_t)((uint8_t)spd - (uint8_t)(adder >> 8) - (lo < 0 ? 1 : 0));
        if ((int8_t)(spd - maxl) < 0) spd = maxl;      /* cmp/bpl semantics  */
    }
    m->x_spd = spd;
    m->x_spd_abs = (uint8_t)(spd < 0 ? -spd : spd);    /* XSpdSign/SetAbsSpd */
}

/* === MovePlayerHorizontally (MoveObjectHorizontally) ====================== */
static void mario__move_horizontally(Mario *m)
{
    /* x speed is 4.4 fixed point; position is 8.8. pos += spd << 4          */
    int32_t fx = (m->x << 8) | m->x_sub;
    fx += (int32_t)m->x_spd * 16;
    m->x = fx >> 8;
    m->x_sub = (uint8_t)fx;
}

/* === MovePlayerVertically / ImposeGravity ================================= */
static void mario__move_vertically(Mario *m)
{
    uint16_t sum;
    int8_t max_spd = 0x04;                    /* player max downward speed   */

    /* position += whole speed + carry from (dummy += velocity fraction)     */
    sum = (uint16_t)m->ymf_dummy + m->y_frac;
    m->ymf_dummy = (uint8_t)sum;
    m->y += m->y_spd + (int32_t)(sum >> 8);

    /* velocity += gravity (VerticalForce), 8.8                              */
    sum = (uint16_t)m->y_frac + m->vforce;
    m->y_frac = (uint8_t)sum;
    m->y_spd = (int8_t)((uint8_t)m->y_spd + (uint8_t)(sum >> 8));

    /* clamp downward speed: only once fraction passes $80, snap to max      */
    if ((int8_t)(m->y_spd - max_spd) >= 0 && m->y_frac >= 0x80) {
        m->y_spd = max_spd;
        m->y_frac = 0;
    }
}

/* === PlayerPhysicsSub: jump initiation + X physics selection ============== */
static void mario__physics_sub(Mario *m)
{
    uint8_t y, c0;

    /* --- CheckForJumping / ProcJumping --- */
    if ((m->joy_ab & MARIO_JOY_A) && !(m->joy_prev_ab & MARIO_JOY_A)) {
        int do_jump = 0;
        if (m->state == MARIO_ST_GROUND) do_jump = 1;
        else if (m->swimming) {
            /* swim: re-stroke only when timer active or not still rising    */
            if (m->jumpswim_timer != 0 || m->y_spd >= 0) do_jump = 1;
        }
        if (do_jump) {
            /* InitJS */
            m->jumpswim_timer = 0x20;
            m->ymf_dummy = 0;
            m->y_frac = 0;
            m->jump_origin_y = (uint8_t)m->y;
            m->state = MARIO_ST_JUMP;
            y = 0;
            if (m->x_spd_abs >= 0x09) { y++;
                if (m->x_spd_abs >= 0x10) { y++;
                    if (m->x_spd_abs >= 0x19) { y++;
                        if (m->x_spd_abs >= 0x1c) y++; } } }
            m->diff_to_halt = 0x01;
            if (m->swimming) y = (uint8_t)(m->whirlpool ? 6 : 5);
            /* GetYPhy */
            m->vforce      = MARIO__JumpMForceData[y];
            m->vforce_down = MARIO__FallMForceData[y];
            m->y_frac      = MARIO__InitMForceData[y];
            m->y_spd       = (int8_t)MARIO__PlayerYSpdData[y];
            if (m->swimming) {
                m->events |= MARIO_EV_SWIM_STROKE;
                if ((uint8_t)m->y < 0x14)
                    m->y_spd = 0;             /* don't swim above the surface */
            } else {
                m->events |= MARIO_EV_JUMP;
            }
        }
    }

    /* --- X_Physics: select max speed + friction strength --- */
    y = 0; c0 = 0;
    if (m->state != MARIO_ST_GROUND) {
        if (m->x_spd_abs >= 0x19) goto get_x_phy;   /* fast air: run physics */
        goto chk_r_fast;
    }
    /* ProcPRun (on the ground) */
    y = 1;
    if (m->swimming) goto chk_r_fast;               /* water area            */
    y = 0;
    if (m->joy_lr != m->moving_dir) goto chk_r_fast;/* pressing against run  */
    if (m->joy_ab & MARIO_JOY_B) {
        m->running_timer = 0x0a;                    /* SetRTmr               */
        goto get_x_phy;
    }
    if (m->running_timer != 0) goto get_x_phy;      /* 10-frame B grace      */

chk_r_fast:
    y++; c0++;
    if (m->running_spd != 0 || m->x_spd_abs >= 0x21)
        c0++;                                       /* FastXSp               */
get_x_phy:
    m->max_left  = MARIO__MaxLeftXSpdData[y];
    m->max_right = MARIO__MaxRightXSpdData[y];
    m->fric_lo   = MARIO__FrictionData[c0];
    m->fric_hi   = 0;
    if (m->facing != m->moving_dir) {               /* skid: double friction */
        m->fric_hi = (uint8_t)((m->fric_lo & 0x80) ? 1 : 0);
        m->fric_lo <<= 1;
    }
}

/* === PlayerBGCollision (head / feet / side sensors) ======================= */
static void mario__impede(Mario *m, uint8_t side)
{
    /* ImpedePlayerMove: side 1 = wall on the right, 2 = wall on the left    */
    if (side == 1) {
        if (m->x_spd >= 0) {
            m->sidecoll_timer = 0x10;
            m->x_spd = 0;
            m->x -= 1;                        /* push away from right wall   */
            m->events |= MARIO_EV_WALL;
        }
        m->coll_bits &= (uint8_t)~MARIO_JOY_RIGHT;
    } else {
        if (m->x_spd < 1) {
            m->sidecoll_timer = 0x10;
            m->x_spd = 0;
            m->x += 1;                        /* push away from left wall    */
            m->events |= MARIO_EV_WALL;
        }
        m->coll_bits &= (uint8_t)~MARIO_JOY_LEFT;
    }
}

static void mario__bg_collision(Mario *m)
{
    uint8_t base, sx, sy, ylow;
    int32_t tx, ty;
    uint8_t prev_state = m->state;

    /* default the state: swimming -> jump/swim, grounded -> falling         */
    if (m->swimming)                    m->state = MARIO_ST_JUMP;
    else if (m->state == MARIO_ST_GROUND) m->state = MARIO_ST_FALL;

    /* ChkOnScr: only collide while vertically on the screen                 */
    if (m->y < 0 || m->y >= 0x100) return;
    m->coll_bits = 0xff;
    if ((uint8_t)m->y >= 0xcf) return;

    base = mario__sensor_base(m);

    /* --- HeadChk --- */
    {
        uint8_t ext_idx = (uint8_t)((m->size != MARIO_BIG) || m->crouching);
        if ((uint8_t)m->y >= MARIO__UpperExtent[ext_idx]) {
            sx = MARIO__SensorX[base][0]; sy = MARIO__SensorY[base][0];
            if (mario__solid(m, m->x + sx, m->y + sy, &tx, &ty)) {
                if (m->y_spd < 0) {
                    ylow = (uint8_t)m->y & 0x0f;
                    if (ylow >= 0x04) {           /* deep enough to count    */
                        m->y_spd = 0x01;          /* NYSpd: kill the jump    */
                        m->events |= MARIO_EV_BUMP_HEAD;
                        m->bump_tx = tx; m->bump_ty = ty;
                    }
                }
            }
        }
    }

    /* --- DoFootCheck --- */
    if ((uint8_t)m->y < 0xcf) {
        uint8_t left, right;
        sx = MARIO__SensorX[base][1]; sy = MARIO__SensorY[base][1];
        left  = mario__solid(m, m->x + sx, m->y + sy, 0, 0);
        sx = MARIO__SensorX[base][2];
        right = mario__solid(m, m->x + sx, m->y + sy, 0, 0);
        if ((left || right) && m->y_spd >= 0) {   /* not while moving up     */
            ylow = (uint8_t)m->y & 0x0f;
            if (ylow >= 0x05) {
                /* buried too deep in the tile: treat as a wall instead      */
                mario__impede(m, m->moving_dir);
            } else {
                /* LandPlyr */
                m->y &= ~(int32_t)0x0f;           /* snap feet to tile top   */
                m->y_spd = 0;
                m->y_frac = 0;
                m->state = MARIO_ST_GROUND;       /* InitSteP               */
                if (prev_state != MARIO_ST_GROUND) m->events |= MARIO_EV_LAND;
            }
        }
    }

    /* --- DoPlayerSideCheck: left pair first, then right pair --- */
    {
        uint8_t pair, hit, yb = (uint8_t)m->y;
        for (pair = 0; pair < 2; pair++) {        /* 0 = left, 1 = right     */
            uint8_t s_up = (uint8_t)(3 + pair * 2);
            uint8_t s_lo = (uint8_t)(4 + pair * 2);
            hit = 0;
            if (yb >= 0x20) {                     /* below status bar band   */
                if (yb >= 0xe4) return;           /* too far down: bail out  */
                sx = MARIO__SensorX[base][s_up]; sy = MARIO__SensorY[base][s_up];
                hit = mario__solid(m, m->x + sx, m->y + sy, 0, 0);
            }
            if (!hit) {                           /* BHalf: lower point      */
                if (yb < 0x08 || yb >= 0xd0) return;
                sx = MARIO__SensorX[base][s_lo]; sy = MARIO__SensorY[base][s_lo];
                hit = mario__solid(m, m->x + sx, m->y + sy, 0, 0);
            }
            if (hit) {
                /* CheckSideMTiles -> StopPlayerMove -> ImpedePlayerMove.
                   First (left) pair runs with counter=2 -> "wall on left",
                   second (right) pair with counter=1 -> "wall on right".    */
                mario__impede(m, pair == 0 ? 2 : 1);
                return;
            }
        }
    }
}

/* === fireballs (ProcFireball_Bubble / FireballObjCore) ==================== */
static void mario__fireball_throw_check(Mario *m)
{
    uint8_t slot;
    if (!m->fire) return;                          /* PlayerStatus < fiery   */
    if (!(m->joy_ab & MARIO_JOY_B)) return;        /* B button new press?    */
    if (m->joy_prev_ab & MARIO_JOY_B) return;
    slot = m->fireball_counter & 0x01;
    if (m->fb[slot].state != 0) return;            /* that slot busy: no go  */
    if (m->y < 0 || m->y >= 0x100) return;         /* Y_HighPos must be 1    */
    if (m->crouching) return;
    if (m->state == MARIO_ST_CLIMB) return;
    m->events |= MARIO_EV_FIREBALL;                /* Sfx_Fireball           */
    m->fb[slot].state = 0x02;
    m->fb_timer = m->anim_timer_set;               /* copy anim frame timing */
    m->anim_timer = (uint8_t)(m->anim_timer_set - 1);
    m->fireball_counter++;
}

static void mario__fireball_tick(Mario *m, int slot)
{
    MarioFireball *f = &m->fb[slot];
    uint16_t sum;

    if (f->state == 0) return;

    if (f->state & 0x80) {                         /* explosion animation    */
        uint8_t idx = (uint8_t)((f->state >> 1) & 0x07);
        f->state++;
        if (idx >= 0x03) f->state = 0;             /* KillFireBall           */
        return;
    }

    if (f->state == 0x02) {                        /* fresh throw: spawn     */
        f->x = m->x + 0x04;                        /* player x + 4           */
        f->y = m->y;                               /* player y               */
        f->x_spd = (int8_t)
            MARIO__FireballXSpdData[(m->facing - 1) & 0x01];
        f->y_spd = 0x04;                           /* moving down at first   */
        f->state--;                                /* now in flight          */
        /* (y_frac/ymf_dummy deliberately NOT cleared -- neither does SMB)   */
    }

    /* RunFB: gravity (force $50, max speed 3), then horizontal movement     */
    sum = (uint16_t)f->ymf_dummy + f->y_frac;
    f->ymf_dummy = (uint8_t)sum;
    f->y += f->y_spd + (int32_t)(sum >> 8);
    sum = (uint16_t)f->y_frac + 0x50;
    f->y_frac = (uint8_t)sum;
    f->y_spd = (int8_t)((uint8_t)f->y_spd + (uint8_t)(sum >> 8));
    if ((int8_t)(f->y_spd - 0x03) >= 0 && f->y_frac >= 0x80) {
        f->y_spd = 0x03;
        f->y_frac = 0;
    }
    {
        int32_t fx = (f->x << 8) | f->x_sub;       /* MoveObjectHorizontally */
        fx += (int32_t)f->x_spd * 16;
        f->x = fx >> 8;
        f->x_sub = (uint8_t)fx;
    }

    /* FireballBGCollision: check the tile under the fireball               */
    if ((uint8_t)f->y >= 0x18 && f->y >= 0 && f->y < 0x100) {
        if (m->tile_at &&
            m->tile_at(m->user, (f->x + 0x04) >> 4, (f->y + 0x08 - 0x20) >> 4)) {
            if (f->y_spd < 0 || f->bouncing) {     /* InitFireballExplode    */
                f->state = 0x80;
                m->events |= MARIO_EV_FB_EXPLODE;  /* Sfx_Bump               */
            } else {
                f->y_spd = (int8_t)0xfd;           /* bounce upwards         */
                f->bouncing = 1;
                f->y &= ~(int32_t)0x07;            /* land it properly       */
            }
        } else {
            f->bouncing = 0;                       /* ClearBounceFlag        */
        }
    } else if (f->y >= 0x100) {
        f->state = 0;                              /* fell out of the world  */
    }
}

/* === animation (ProcessPlayerAction / AnimationControl) =================== */
static void mario__animate(Mario *m)
{
    uint8_t act, extent = 0, animate = 0, use_cur = 0;

    if (m->state == MARIO_ST_FALL) {
        act = MARIO_ACT_WALK; use_cur = 1;         /* freeze walk frame      */
    } else if (m->state == MARIO_ST_JUMP) {
        if (m->swimming) {
            act = MARIO_ACT_SWIM;
            if (m->jumpswim_timer || m->anim_ctrl || (m->joy_ab & MARIO_JOY_A))
                { animate = 1; extent = 3; }
            else use_cur = 1;
        }
        else if (m->crouching) act = MARIO_ACT_CROUCH;
        else act = MARIO_ACT_JUMP;
    } else { /* ground */
        if (m->crouching) act = MARIO_ACT_CROUCH;
        else if (m->x_spd == 0 && m->joy_lr == 0) act = MARIO_ACT_STAND;
        else {
            act = MARIO_ACT_WALK;
            if (m->x_spd_abs >= 0x09 && (m->moving_dir & m->facing) == 0)
                act = MARIO_ACT_SKID;
            else { animate = 1; extent = 3; }
        }
    }

    if (animate || use_cur) {
        uint8_t off = MARIO__GfxTblOffsets[act + (m->size ? 8 : 0)];
        m->gfx_offset = (uint8_t)(off + (m->anim_ctrl << 3));
        if (animate && m->anim_timer == 0) {       /* AnimationControl       */
            m->anim_timer = m->anim_timer_set;
            m->anim_ctrl = (uint8_t)(m->anim_ctrl + 1 < extent
                                     ? m->anim_ctrl + 1 : 0);
        }
    } else {
        m->anim_ctrl = 0;                          /* NonAnimatedActs        */
        m->gfx_offset = MARIO__GfxTblOffsets[act + (m->size ? 8 : 0)];
    }
    m->gfx_action = act;

    /* fireball-throwing pose (PlayerGfxProcessing): while the throw timer
       outlasts the animation timer, show the throw frame instead           */
    if (m->fb_timer != 0) {
        if (m->anim_timer >= m->fb_timer) {
            m->fb_timer = 0;
        } else {
            m->fb_timer = m->anim_timer;
            m->gfx_offset = MARIO__GfxTblOffsets[MARIO_ACT_THROW
                                                 + (m->size ? 8 : 0)];
            m->gfx_action = MARIO_ACT_THROW;
        }
    }
}

/* === public API =========================================================== */

void mario_init(Mario *m, int32_t x, int32_t y)
{
    Mario z = {0};
    *m = z;
    m->x = x;
    m->y = y;
    m->facing = 1;
    m->moving_dir = 1;
    m->coll_bits = 0xff;
    m->state = MARIO_ST_FALL;
    m->vforce = MARIO__FallMForceData[0];
    m->vforce_down = MARIO__FallMForceData[0];
    m->max_left = 0xe8;
    m->max_right = 0x18;
    m->fric_lo = 0x98;
    m->anim_timer_set = 0x07;
}

void mario_tick(Mario *m, uint8_t joypad)
{
    m->events = 0;
    m->frame_ctr++;

    /* frame timers ($0780 block: decrement every frame when nonzero)        */
    if (m->anim_timer)     m->anim_timer--;
    if (m->jumpswim_timer) m->jumpswim_timer--;
    if (m->running_timer)  m->running_timer--;
    if (m->sidecoll_timer) m->sidecoll_timer--;

    /* --- PlayerCtrlRoutine: split the controller --- */
    m->joy    = joypad;
    m->joy_ab = joypad & 0xc0;
    m->joy_lr = joypad & 0x03;
    m->joy_ud = joypad & 0x0c;
    if ((m->joy_ud & MARIO_JOY_DOWN) && m->state == MARIO_ST_GROUND
        && m->joy_lr != 0) {
        m->joy_lr = 0;                    /* down beats steering on ground   */
        m->joy_ud = 0;
    }

    /* --- PlayerMovementSubs --- */
    {   /* crouch flag: big only; frozen while airborne                      */
        if (m->size != MARIO_BIG) m->crouching = 0;
        else if (m->state == MARIO_ST_GROUND)
            m->crouching = (uint8_t)(m->joy_ud & MARIO_JOY_DOWN);
    }
    mario__physics_sub(m);

    switch (m->state) {
    case MARIO_ST_GROUND:                 /* OnGroundStateSub                */
        mario__get_anim_speed(m);
        if (m->joy_lr) m->facing = m->joy_lr;
        mario__impose_friction(m, m->joy_lr);
        mario__move_horizontally(m);
        break;

    case MARIO_ST_JUMP:                   /* JumpSwimSub                     */
        if (m->y_spd >= 0) {
            m->vforce = m->vforce_down;   /* falling side of the arc         */
        } else if (!((m->joy_ab & MARIO_JOY_A) && (m->joy_prev_ab & MARIO_JOY_A))) {
            /* A released: if jump has risen >= DiffToHaltJump, fall faster  */
            uint8_t diff = (uint8_t)(m->jump_origin_y - (uint8_t)m->y);
            if (diff >= m->diff_to_halt)
                m->vforce = m->vforce_down;
        }
        if (m->swimming) {
            mario__get_anim_speed(m);
            if ((uint8_t)m->y < 0x14)
                m->vforce = 0x18;         /* pushed down near the surface    */
            if (m->joy_lr) m->facing = m->joy_lr;
        }
        if (m->joy_lr) mario__impose_friction(m, m->joy_lr);
        mario__move_horizontally(m);
        mario__move_vertically(m);
        break;

    case MARIO_ST_FALL:                   /* FallingSub                      */
        m->vforce = m->vforce_down;
        if (m->joy_lr) mario__impose_friction(m, m->joy_lr);
        mario__move_horizontally(m);
        mario__move_vertically(m);
        break;

    default:                              /* climbing: not implemented       */
        break;
    }

    /* moving direction from the speed we ended up with                      */
    if (m->x_spd != 0)
        m->moving_dir = (uint8_t)(m->x_spd > 0 ? 1 : 2);

    /* --- PlayerBGCollision --- */
    mario__bg_collision(m);

    if (m->y >= 0x100) m->events |= MARIO_EV_FELL;

    /* --- ProcFireball_Bubble: throw check + run both fireball slots --- */
    mario__fireball_throw_check(m);
    mario__fireball_tick(m, 0);
    mario__fireball_tick(m, 1);

    /* --- render-side bookkeeping (PlayerGfxHandler cadence) --- */
    mario__animate(m);

    /* SaveAB: remember A/B for next frame's edge detection                  */
    m->joy_prev_ab = m->joy_ab;
}

void mario_get_draw(const Mario *m, MarioDraw *d)
{
    uint8_t row, left;
    const uint8_t *src = &MARIO__GraphicsTable[m->gfx_offset];

    /* DrawSpriteObject: facing left swaps each row's tile pair and sets the
       horizontal flip bit on both sprites                                   */
    left = (uint8_t)(m->facing == 2);
    for (row = 0; row < 4; row++) {
        uint8_t tl = src[row * 2], tr = src[row * 2 + 1];
        if (left) {
            d->tiles[row * 2]     = tr;
            d->tiles[row * 2 + 1] = tl;
        } else {
            d->tiles[row * 2]     = tl;
            d->tiles[row * 2 + 1] = tr;
        }
        d->hflip[row * 2] = d->hflip[row * 2 + 1] = left;
    }

    /* ChkForPlayerAttrib: symmetric poses use one tile mirrored in hardware.
       crouch ($50), small standing ($B8) and grow frame ($C0) branch to
       C_S_IGAtt: fourth row forced to [no flip, flip] regardless of facing.
       big standing ($C8) *falls through* KilledAtt first (the disassembly's
       "fourth row only" comment is wrong!), so it - and the killed pose -
       get the third row mirrored the same way too.                          */
    if (m->gfx_offset == 0x50 || m->gfx_offset == 0xb8 ||
        m->gfx_offset == 0xc0 || m->gfx_offset == 0xc8 ||
        m->gfx_offset == 0xb0) {
        if (m->gfx_offset == 0xc8 || m->gfx_offset == 0xb0) {
            d->hflip[4] = 0;                       /* KilledAtt              */
            d->hflip[5] = 1;
        }
        d->hflip[6] = 0;                           /* C_S_IGAtt              */
        d->hflip[7] = 1;
    }

    /* swim kick flutter: replace the trailing foot tile every 8 frames      */
    if (m->swimming && m->state != MARIO_ST_GROUND && !(m->frame_ctr & 0x04)) {
        uint8_t slot = (uint8_t)((m->facing & 0x01) ? 6 : 7);
        if (m->size == MARIO_BIG)
            d->tiles[slot] = MARIO__SwimKickTileNum[0];
        else if (d->tiles[slot] != 0x44)
            d->tiles[slot] = MARIO__SwimKickTileNum[1];
    }

    d->flip = left;
    d->x = m->x;
    d->y = m->y;
    d->action = m->gfx_action;
    d->frame = m->anim_ctrl;
}

void mario_fireball_draw(const Mario *m, int slot, MarioFireballDraw *d)
{
    const MarioFireball *f = &m->fb[slot & 1];
    d->active = 0;
    if (f->state == 0 || f->state == 0x02) return;
    d->active = 1;
    d->x = f->x;
    d->y = f->y;
    if (f->state & 0x80) {                         /* DrawExplosion_Fireball */
        uint8_t idx = (uint8_t)((f->state >> 1) & 0x07);
        d->exploding = 1;
        d->tile = MARIO__ExplosionTiles[idx < 3 ? idx : 2];
        d->hflip = d->vflip = 0;
    } else {                                       /* DrawFireball           */
        d->exploding = 0;
        d->tile = (uint8_t)(0x64 ^ ((m->frame_ctr >> 2) & 0x01));
        d->hflip = d->vflip = (uint8_t)((m->frame_ctr >> 4) & 0x01);
    }
}

void mario_fireball_cull(Mario *m, int32_t view_left, int32_t view_right)
{
    int i;
    for (i = 0; i < 2; i++) {
        MarioFireball *f = &m->fb[i];
        if (f->state != 0 &&
            (f->x < view_left - 0x08 || f->x > view_right + 0x08))
            f->state = 0;                          /* offscreen: erase       */
    }
}

const uint8_t *mario_palette(const Mario *m)
{
    return MARIO__PlayerColors[m->fire ? 2 : 0];
}

#endif /* MARIO_IMPLEMENTATION */
