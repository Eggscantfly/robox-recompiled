// sdk/robox_lua_int.h -- shared between robox_lua.c (VM + loader) and
// robox_lua_api.c (the robox.* surface) and robox_lua_guest.c (guest bridge).
// Not for the rest of the tree; that gets robox_lua.h.
#ifndef ROBOX_LUA_INT_H
#define ROBOX_LUA_INT_H

#include <stdint.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/* Events a mod can subscribe to with robox.on(name, fn). Order matters only
 * in that RLUA_EV_COUNT closes the list. */
typedef enum {
    RLUA_EV_START,     /* once, after every mod has loaded            */
    RLUA_EV_FRAME,     /* every host frame; fn(dt_seconds)            */
    RLUA_EV_DRAW,      /* every presented frame, inside the overlay   */
    RLUA_EV_LEVEL,     /* level id changed; fn(id, name, is_robot)    */
    RLUA_EV_KEY,       /* keyboard edge; fn(keyname, down)            */
    RLUA_EV_TEXT,      /* typed text while capturing; fn(utf8)        */
    RLUA_EV_UNLOAD,    /* this mod is about to be reloaded or dropped */
    RLUA_EV_COUNT
} rlua_event_t;

const char *rlua_event_name(rlua_event_t ev);
int         rlua_event_from_name(const char *name);   /* -1 if unknown */

/* One loaded mod. `dir` is where its files live and doubles as the root for
 * robox.config / require. */
#define RLUA_MAX_MODS   32
#define RLUA_MAX_WATCH  16

typedef struct {
    char  id[64];
    char  dir[512];
    char  main[512];
    char  watch[RLUA_MAX_WATCH][512];
    long  watch_mtime[RLUA_MAX_WATCH];
    int   watch_count;
    int   env_ref;                 /* registry ref: the mod's _ENV table   */
    int   cb_ref[RLUA_EV_COUNT];   /* registry ref: array of handlers      */
    int   loaded;
    int   errors;                  /* runtime errors, for rate limiting    */
} rlua_mod_t;

extern lua_State *g_rlua;
extern rlua_mod_t g_rlua_mods[RLUA_MAX_MODS];
extern int        g_rlua_mod_count;

/* The mod whose code is currently running. -1 outside a callback. Bindings
 * that need to attribute something (a hook, a config file, a log line) read
 * this rather than guessing. */
extern int g_rlua_cur;
rlua_mod_t *rlua_current(void);

/* Registers the robox.* table into `env`, which is the mod's _ENV. Each mod
 * gets its OWN robox table so robox.name / robox.dir mean what they say. */
void rlua_open_api(lua_State *L, int mod_index);

/* Adds robox.game to the robox table on top of the stack. Lives in
 * robox_lua_guest.c because everything behind it does. */
void rlua_open_game_api(lua_State *L);

/* Once per host frame, before any handler runs: latches the button edges
 * input.pressed/released compare against and ages injected presses. */
void rlua_api_tick(void);

/* 1 while a "draw" handler is running, so the draw bindings can refuse to
 * paint into a batch that is not open. */
int  rlua_in_draw(void);

/* Undo everything mod `i` installed (guest-function hooks, watches, input
 * holds). Called before a reload and at shutdown. */
void rlua_api_unload_mod(int i);

/* --- modal keyboard ------------------------------------------------------ */
//
// One mod at a time can take the keyboard: a console wants every key you press,
// including the ones the port itself uses for fullscreen and the settings menu.
// While it is held, key edges and typed text go to the holder ALONE (the other
// mods' handlers do not run), sdk/video.c stands its own hotkeys down, and the
// guest sees no input at all.

/* Take or drop the keyboard for the mod currently running. */
void rlua_capture_set(int on);
/* Drop it whoever holds it -- a reload must never leave the keyboard owned by
 * a mod that no longer exists. */
void rlua_capture_release(void);

/* Print a Lua error with a traceback and show the first line on screen.
 * `where` is a short tag for the log ("frame", "load", ...). */
void rlua_report_error(int mod_index, const char *where, const char *msg);

/* On-screen toast queue -- robox.notify() and error reports share it. */
void rlua_toast(const char *text, float seconds, int is_error);
void rlua_toasts_render(void);
void rlua_toasts_tick(double dt);

/* Frame counter and seconds since the VM came up. */
extern uint64_t g_rlua_frame;
extern double   g_rlua_time;

/* --- guest-VM bridge, implemented in robox_lua_guest.c ------------------- */

/* Builds the merged script/game.lua once, at VM init. */
void        rlua_guest_init(void);
/* Is there a guest-side script at all? robox.game.available(). */
int         rlua_guest_available(void);
/* Answers one robox.host() call from the guest: runs the named handler and
 * writes its result as a chunk the guest's dofile() can read. Returns the
 * host path to serve, or NULL. */
const char *rlua_guest_rpc_answer(const char *path_after_prefix);
/* Handlers the runtime provides before any mod registers one (robox.log). */
void        rlua_guest_register_builtins(lua_State *L);

#endif /* ROBOX_LUA_INT_H */
