// sdk/robox_lua.c -- the Lua mod runtime: VM, loader, hot reload, events.
//
// WHY THIS EXISTS
// Adding a mod to this port used to mean writing C in sdk/, rebuilding ~3000
// generated translation units, and relaunching. That is a fine workflow for
// the handful of people who already have the recompiler set up and a terrible
// one for anybody else -- which is most of the people who would actually want
// to make something. A scripting layer moves the loop from "twenty minutes and
// a toolchain" to "save the file".
//
// SHAPE
// One Lua 5.4 state for every mod. Each mod gets its own _ENV table whose
// __index falls through to the real globals, so two mods can both keep a
// global called `state` without meeting, but neither has to say `local` in
// front of every string function. Its robox table is per-mod too -- robox.name
// and robox.dir have to mean this mod, not whichever loaded last.
//
// Handlers are refs held in the registry, grouped per mod per event, so a
// reload drops exactly one mod's callbacks and leaves the rest alone.
//
// HOT RELOAD
// Every .lua under a mod's directory is stat()ed a few times a second. When
// one moves, the mod's "unload" handler runs, its hooks and held buttons are
// released, its env is thrown away, and the chunk is re-run from scratch. This
// is the whole point of the feature, so it is deliberately blunt: there is no
// attempt to preserve state across a reload. A mod that wants to survive one
// writes what matters into robox.config and reads it back in.
//
// TRUST
// A mod can write any byte of guest memory and replace any function in the
// game. There is no sandbox worth the name and pretending otherwise would be
// worse than saying so: the stdlib is open, io and os included. Install mods
// the way you install any other program.

#include "robox_lua.h"

#if !defined(__3DS__)   /* the 3DS build has no overlay renderer and no room */

#include "robox_lua_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#include <SDL2/SDL.h>

#include "hle.h"        /* robox_mkdir */

/* --- state --------------------------------------------------------------- */

lua_State *g_rlua;
rlua_mod_t g_rlua_mods[RLUA_MAX_MODS];
int        g_rlua_mod_count;
int        g_rlua_cur = -1;
uint64_t   g_rlua_frame;
double     g_rlua_time;

#define MODS_ROOT "mods/lua"

static double  g_last_ticks;      /* SDL_GetTicks at the previous tick   */
static double  g_reload_accum;    /* seconds since the last mtime scan   */
static int     g_reload_on = 1;
static int     g_in_draw;         /* robox.draw.* only works inside this */

rlua_mod_t *rlua_current(void) {
    return (g_rlua_cur >= 0 && g_rlua_cur < g_rlua_mod_count)
         ? &g_rlua_mods[g_rlua_cur] : NULL;
}

int robox_lua_active(void) { return g_rlua != NULL; }

/* --- events -------------------------------------------------------------- */

static const char *const EV_NAME[RLUA_EV_COUNT] = {
    "start", "frame", "draw", "level", "key", "text", "unload"
};

const char *rlua_event_name(rlua_event_t ev) {
    return (unsigned)ev < RLUA_EV_COUNT ? EV_NAME[ev] : "?";
}

int rlua_event_from_name(const char *name) {
    for (int i = 0; i < RLUA_EV_COUNT; ++i)
        if (!strcmp(name, EV_NAME[i])) return i;
    return -1;
}

/* --- on-screen toasts ---------------------------------------------------- */
//
// Errors have to be visible without a console. A mod author running the
// shipped build has stderr pointed at the null device (see src/main.c), so a
// syntax error would otherwise be a mod that silently does nothing -- the
// single most confusing failure a scripting layer can have.

#define TOAST_MAX 6
static struct { char text[192]; float ttl; int err; } g_toast[TOAST_MAX];

void rlua_toast(const char *text, float seconds, int is_error) {
    /* Oldest out. A burst of errors should show the newest, not the first. */
    for (int i = TOAST_MAX - 1; i > 0; --i) g_toast[i] = g_toast[i - 1];
    snprintf(g_toast[0].text, sizeof g_toast[0].text, "%s", text ? text : "");
    g_toast[0].ttl = seconds;
    g_toast[0].err = is_error;
}

void rlua_toasts_tick(double dt) {
    for (int i = 0; i < TOAST_MAX; ++i)
        if (g_toast[i].ttl > 0.0f) g_toast[i].ttl -= (float)dt;
}

void rlua_toasts_render(void) {
    extern void gx_ogl_overlay_rect(float, float, float, float, float, float, float, float);
    extern void gx_ogl_overlay_text(float, float, const char *, float, float, float, float, float, float);
    extern float gx_ogl_overlay_text_width(const char *, float, float);

    float y = 640.0f;
    for (int i = 0; i < TOAST_MAX; ++i) {
        if (g_toast[i].ttl <= 0.0f) continue;
        /* Fade over the last half second so they leave instead of vanishing. */
        float a = g_toast[i].ttl < 0.5f ? g_toast[i].ttl / 0.5f : 1.0f;
        float w = gx_ogl_overlay_text_width(g_toast[i].text, 0.55f, 0.0f);
        gx_ogl_overlay_rect(24.0f, y - 6.0f, w + 24.0f, 30.0f,
                            g_toast[i].err ? 0.35f : 0.0f, 0.0f, 0.0f, 0.72f * a);
        gx_ogl_overlay_text(36.0f, y, g_toast[i].text,
                            g_toast[i].err ? 1.0f : 0.85f,
                            g_toast[i].err ? 0.45f : 1.0f,
                            g_toast[i].err ? 0.40f : 0.85f, a, 0.55f, 0.0f);
        y -= 34.0f;
    }
}

/* --- errors -------------------------------------------------------------- */

void rlua_report_error(int mod_index, const char *where, const char *msg) {
    const char *id = (mod_index >= 0 && mod_index < g_rlua_mod_count)
                   ? g_rlua_mods[mod_index].id : "?";
    fprintf(stderr, "[lua] %s (%s):\n%s\n", id, where, msg ? msg : "(no message)");
    fflush(stderr);

    if (mod_index >= 0 && mod_index < g_rlua_mod_count) {
        rlua_mod_t *m = &g_rlua_mods[mod_index];
        /* A mod erroring every frame must not turn into a 60 Hz wall of
         * toasts. Show the first few, then only every 300th. */
        if (m->errors > 4 && (m->errors % 300) != 0) { ++m->errors; return; }
        ++m->errors;
    }

    /* Just the first line on screen; the traceback is for the log. */
    char one[192];
    const char *nl = msg ? strchr(msg, '\n') : NULL;
    size_t n = msg ? (nl ? (size_t)(nl - msg) : strlen(msg)) : 0;
    if (n >= sizeof one) n = sizeof one - 1;
    snprintf(one, sizeof one, "%.*s", (int)n, msg ? msg : "");
    char line[192];
    snprintf(line, sizeof line, "%s: %s", id, one);
    rlua_toast(line, 8.0f, 1);
}

/* Message handler for every pcall: turns the error value into a string with a
 * traceback attached, which is the difference between "attempt to index a nil
 * value" and knowing which line did it. */
static int traceback_handler(lua_State *L) {
    const char *msg = lua_tostring(L, 1);
    if (!msg) {
        if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING)
            return 1;
        msg = lua_pushfstring(L, "(error object is a %s value)",
                              luaL_typename(L, 1));
    }
    luaL_traceback(L, L, msg, 1);
    return 1;
}

/* --- dispatch ------------------------------------------------------------ */

/* Fire `ev` on every loaded mod. The arguments are already on the stack, the
 * top `nargs` values; they are copied for each handler and popped at the end.
 *
 * `only_mod` >= 0 restricts the fire to one mod (used for "unload" and for
 * "start" on a hot-reloaded mod, which must not re-run everyone else's). */
static void rlua_fire_n(rlua_event_t ev, int nargs, int only_mod) {
    lua_State *L = g_rlua;
    if (!L) return;

    const int base = lua_gettop(L) - nargs;     /* args at base+1 .. base+nargs */
    const int saved_cur = g_rlua_cur;

    lua_pushcfunction(L, traceback_handler);
    const int msgh = lua_gettop(L);

    for (int i = 0; i < g_rlua_mod_count; ++i) {
        if (only_mod >= 0 && i != only_mod) continue;
        rlua_mod_t *m = &g_rlua_mods[i];
        if (!m->loaded || m->cb_ref[ev] == LUA_NOREF) continue;

        lua_rawgeti(L, LUA_REGISTRYINDEX, m->cb_ref[ev]);   /* handler array */
        const int arr = lua_gettop(L);
        const lua_Integer n = (lua_Integer)lua_rawlen(L, arr);

        for (lua_Integer k = 1; k <= n; ++k) {
            lua_rawgeti(L, arr, k);
            if (!lua_isfunction(L, -1)) { lua_pop(L, 1); continue; }
            for (int a = 1; a <= nargs; ++a) lua_pushvalue(L, base + a);

            g_rlua_cur = i;
            const int rc = lua_pcall(L, nargs, 0, msgh);
            g_rlua_cur = saved_cur;

            if (rc != LUA_OK) {
                rlua_report_error(i, rlua_event_name(ev), lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);          /* handler array */
    }

    lua_pop(L, 1);              /* msgh */
    lua_pop(L, nargs);
}

/* --- mod loading --------------------------------------------------------- */

static long file_mtime(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_mtime : -1;
}

static int has_suffix(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && !strcmp(s + ls - lf, suf);
}

/* Remember `path` as one of the files whose mtime decides a reload. */
static void watch_add(rlua_mod_t *m, const char *path) {
    if (m->watch_count >= RLUA_MAX_WATCH) return;
    for (int i = 0; i < m->watch_count; ++i)
        if (!strcmp(m->watch[i], path)) return;
    snprintf(m->watch[m->watch_count], sizeof m->watch[0], "%s", path);
    m->watch_mtime[m->watch_count] = file_mtime(path);
    ++m->watch_count;
}

/* A single-file mod's `dir` is mods/lua itself, shared with every other
 * single-file mod -- so it must never adopt the whole directory as its watch
 * set or editing one script would reload all of them. */
static int folder_mod(const rlua_mod_t *m) {
    return strcmp(m->dir, MODS_ROOT) != 0;
}

/* Every .lua directly inside the mod's own folder. Not recursive: a mod that
 * wants a deeper layout can call robox.watch() on the extra files, and
 * walking an arbitrary tree several times a second is a poor trade. */
static void watch_scan_dir(rlua_mod_t *m) {
    if (!folder_mod(m)) return;
    DIR *d = opendir(m->dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (!has_suffix(e->d_name, ".lua")) continue;
        char p[512];
        snprintf(p, sizeof p, "%s/%s", m->dir, e->d_name);
        watch_add(m, p);
    }
    closedir(d);
}

static void clear_callbacks(rlua_mod_t *m) {
    for (int e = 0; e < RLUA_EV_COUNT; ++e) {
        if (m->cb_ref[e] != LUA_NOREF) {
            luaL_unref(g_rlua, LUA_REGISTRYINDEX, m->cb_ref[e]);
            m->cb_ref[e] = LUA_NOREF;
        }
    }
}

/* Run the mod's main chunk in a fresh environment. Returns 1 on success. */
static int load_mod_chunk(int index) {
    lua_State *L = g_rlua;
    rlua_mod_t *m = &g_rlua_mods[index];

    lua_pushcfunction(L, traceback_handler);
    const int msgh = lua_gettop(L);

    if (luaL_loadfilex(L, m->main, "t") != LUA_OK) {
        rlua_report_error(index, "load", lua_tostring(L, -1));
        lua_pop(L, 2);                  /* error + msgh */
        return 0;
    }

    /* Fresh _ENV: own globals, falling through to the real ones for read. */
    lua_newtable(L);                    /* env */
    lua_newtable(L);                    /* meta */
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);

    rlua_open_api(L, index);            /* env.robox = { ... } */

    lua_pushvalue(L, -1);               /* keep a copy to ref */
    m->env_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    /* _ENV is upvalue 1 of any main chunk. */
    if (!lua_setupvalue(L, -2, 1)) lua_pop(L, 1);

    const int saved_cur = g_rlua_cur;
    g_rlua_cur = index;
    const int rc = lua_pcall(L, 0, 0, msgh);
    g_rlua_cur = saved_cur;

    if (rc != LUA_OK) {
        rlua_report_error(index, "load", lua_tostring(L, -1));
        lua_pop(L, 1);
        lua_pop(L, 1);                  /* msgh */
        return 0;
    }
    lua_pop(L, 1);                      /* msgh */
    m->loaded = 1;
    m->errors = 0;
    return 1;
}

/* Forget everything this mod pulled in with require(), so a reload actually
 * re-reads it. package.loaded is a cache by design -- correct for a program
 * that loads its modules once, exactly wrong for a mod whose second file is
 * the one being edited. Matches `<id>` and `<id>.anything`, which is what
 * package.path resolves a folder mod's own modules to. */
static void clear_required(const char *id) {
    lua_State *L = g_rlua;
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }

    const size_t idlen = strlen(id);
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            const char *k = lua_tostring(L, -2);
            if (!strncmp(k, id, idlen) && (k[idlen] == '\0' || k[idlen] == '.')) {
                /* Clearing an existing field mid-traversal is the one
                 * mutation lua_next explicitly permits. */
                lua_pushvalue(L, -2);
                lua_pushnil(L);
                lua_rawset(L, -5);
            }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static void unload_mod(int index) {
    rlua_mod_t *m = &g_rlua_mods[index];
    if (m->loaded) rlua_fire_n(RLUA_EV_UNLOAD, 0, index);
    clear_required(m->id);
    rlua_api_unload_mod(index);
    clear_callbacks(m);
    if (m->env_ref != LUA_NOREF) {
        luaL_unref(g_rlua, LUA_REGISTRYINDEX, m->env_ref);
        m->env_ref = LUA_NOREF;
    }
    m->loaded = 0;
}

static void register_mod(const char *id, const char *dir, const char *main) {
    for (int i = 0; i < g_rlua_mod_count; ++i)
        if (!strcmp(g_rlua_mods[i].id, id)) return;      /* already known */
    if (g_rlua_mod_count >= RLUA_MAX_MODS) {
        fprintf(stderr, "[lua] too many mods, skipping '%s'\n", id);
        return;
    }
    rlua_mod_t *m = &g_rlua_mods[g_rlua_mod_count];
    memset(m, 0, sizeof *m);
    snprintf(m->id,   sizeof m->id,   "%s", id);
    snprintf(m->dir,  sizeof m->dir,  "%s", dir);
    snprintf(m->main, sizeof m->main, "%s", main);
    m->env_ref = LUA_NOREF;
    for (int e = 0; e < RLUA_EV_COUNT; ++e) m->cb_ref[e] = LUA_NOREF;
    watch_add(m, main);
    watch_scan_dir(m);
    ++g_rlua_mod_count;
}

/* mods/lua/<name>.lua        -- single file
 * mods/lua/<name>/init.lua   -- folder, may contain more files and assets
 *
 * A leading '_' means "off": renaming is the fastest possible disable and
 * needs no config file to explain itself. mods/lua/disabled/ works the same
 * way for people who would rather move than rename. */
static void discover_mods(void) {
    DIR *d = opendir(MODS_ROOT);
    if (!d) {
        fprintf(stderr, "[lua] no %s/ directory -- nothing to load\n", MODS_ROOT);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *name = e->d_name;
        if (name[0] == '.' || name[0] == '_') continue;
        if (!strcmp(name, "disabled")) continue;

        char path[512];
        snprintf(path, sizeof path, "%s/%s", MODS_ROOT, name);

        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (st.st_mode & S_IFDIR) {
            char main[512];
            snprintf(main, sizeof main, "%s/init.lua", path);
            if (file_mtime(main) < 0) continue;      /* not a mod folder */
            register_mod(name, path, main);
        } else if (has_suffix(name, ".lua")) {
            char id[64];
            snprintf(id, sizeof id, "%.*s", (int)(strlen(name) - 4), name);
            /* A single-file mod's "directory" is mods/lua itself for require,
             * but its config belongs to it alone -- see robox.config. */
            register_mod(id, MODS_ROOT, path);
        }
    }
    closedir(d);
}

/* --- hot reload ---------------------------------------------------------- */

static void reload_mod(int index) {
    rlua_mod_t *m = &g_rlua_mods[index];
    fprintf(stderr, "[lua] reloading %s\n", m->id);
    fflush(stderr);

    unload_mod(index);
    m->watch_count = 0;
    watch_add(m, m->main);
    watch_scan_dir(m);

    if (load_mod_chunk(index)) {
        char msg[128];
        snprintf(msg, sizeof msg, "reloaded %s", m->id);
        rlua_toast(msg, 2.0f, 0);
        rlua_fire_n(RLUA_EV_START, 0, index);
    }
}

static void check_reloads(void) {
    for (int i = 0; i < g_rlua_mod_count; ++i) {
        rlua_mod_t *m = &g_rlua_mods[i];

        /* Deleted. Take it out quietly rather than reporting the failed open
         * as a load error -- deleting a mod is a thing people do on purpose. */
        if (file_mtime(m->main) < 0) {
            if (m->loaded) {
                fprintf(stderr, "[lua] %s removed\n", m->id);
                unload_mod(i);
                for (int w = 0; w < m->watch_count; ++w) m->watch_mtime[w] = -1;
            }
            continue;
        }

        int changed = 0;
        for (int w = 0; w < m->watch_count; ++w) {
            long t = file_mtime(m->watch[w]);
            if (t != m->watch_mtime[w]) { m->watch_mtime[w] = t; changed = 1; }
        }
        /* A file appearing next to a folder mod that has never loaded (the
         * one being added to fix the error that killed it) has to count too. */
        if (!changed && !m->loaded) {
            const int before = m->watch_count;
            watch_scan_dir(m);
            if (m->watch_count != before) changed = 1;
        }
        if (changed) reload_mod(i);
    }

    /* Mods that did not exist when the game started. Without this, writing a
     * brand new script means restarting -- which is most of what the reload
     * was supposed to save you from, since the first version of a mod is
     * exactly the one you are writing. */
    const int before = g_rlua_mod_count;
    discover_mods();
    for (int i = before; i < g_rlua_mod_count; ++i) {
        fprintf(stderr, "[lua] new mod %s\n", g_rlua_mods[i].id);
        if (load_mod_chunk(i)) {
            char msg[128];
            snprintf(msg, sizeof msg, "loaded %s", g_rlua_mods[i].id);
            rlua_toast(msg, 2.5f, 0);
            rlua_fire_n(RLUA_EV_START, 0, i);
        }
    }
}

/* --- keyboard edges ------------------------------------------------------ */
//
// Polled rather than hooked into the SDL event chain in sdk/video.c. That
// chain already arbitrates the settings menu against the game and threading a
// third claimant through it buys nothing: a mod that wants a key wants to know
// it went down, and diffing the keyboard state says exactly that without
// touching anyone else's code path.

static Uint8 g_keys_prev[SDL_NUM_SCANCODES];

/* Which mod owns the keyboard, or -1. See the note in robox_lua_int.h. */
static int g_capture_mod = -1;

int robox_lua_capture_active(void) { return g_capture_mod >= 0; }

void rlua_capture_set(int on) {
    const int who = rlua_current() ? g_rlua_cur : -1;
    if (on) {
        if (who < 0) return;              /* nobody to give it to */
        g_capture_mod = who;
        SDL_StartTextInput();
    } else if (g_capture_mod == who || who < 0) {
        /* Only the holder can drop it -- another mod calling capture(false)
         * must not be able to pull the keyboard out from under a console. */
        rlua_capture_release();
    }
}

void rlua_capture_release(void) {
    if (g_capture_mod < 0) return;
    g_capture_mod = -1;
    SDL_StopTextInput();
    /* g_keys_prev is deliberately left alone. It is a snapshot of the physical
     * keyboard and it stays accurate across a capture change -- fire_key_edges
     * keeps updating it while captured, it just delivers to one mod.
     *
     * Clearing it here looked tidy and was a bug: the key that drops capture is
     * still physically DOWN for the next few frames, so a cleared snapshot
     * reports it as a fresh press. For the console's ` that meant close and
     * instantly reopen -- the key appeared not to work at all, and only Escape
     * seemed to close it. Never fabricate an edge for a key nobody touched. */
}

/* Typed characters, from the SDL pump in sdk/video.c. Only the holder hears
 * them -- text with no capture is just someone playing the game. */
void robox_lua_text_input(const char *utf8) {
    if (!g_rlua || g_capture_mod < 0 || !utf8 || !*utf8) return;
    lua_pushstring(g_rlua, utf8);
    rlua_fire_n(RLUA_EV_TEXT, 1, g_capture_mod);
}

static void fire_key_edges(void) {
    /* Only ask SDL for the array once; it is a pointer into SDL's own state
     * and stays valid, but the length is what we actually need pinned. */
    int n = 0;
    const Uint8 *now = SDL_GetKeyboardState(&n);
    if (!now) return;
    if (n > SDL_NUM_SCANCODES) n = SDL_NUM_SCANCODES;

    for (int sc = 0; sc < n; ++sc) {
        if (now[sc] == g_keys_prev[sc]) continue;
        g_keys_prev[sc] = now[sc];
        const char *name = SDL_GetScancodeName((SDL_Scancode)sc);
        if (!name || !name[0]) continue;
        lua_pushstring(g_rlua, name);
        lua_pushboolean(g_rlua, now[sc] != 0);
        /* g_capture_mod is -1 when nobody holds the keyboard, which is
         * rlua_fire_n's "every mod" -- so the modal case is the same call. */
        rlua_fire_n(RLUA_EV_KEY, 2, g_capture_mod);
    }
}

/* --- public entry points ------------------------------------------------- */

void robox_lua_init(void) {
    g_rlua = luaL_newstate();
    if (!g_rlua) {
        fprintf(stderr, "[lua] could not create a Lua state\n");
        return;
    }
    luaL_openlibs(g_rlua);

    /* require() should find files next to the mods without every mod having
     * to rebuild package.path itself. */
    lua_getglobal(g_rlua, "package");
    if (lua_istable(g_rlua, -1)) {
        lua_pushstring(g_rlua,
            MODS_ROOT "/?.lua;" MODS_ROOT "/?/init.lua;" MODS_ROOT "/?/?.lua;./?.lua");
        lua_setfield(g_rlua, -2, "path");
    }
    lua_pop(g_rlua, 1);

    robox_mkdir("mods");
    robox_mkdir(MODS_ROOT);

    rlua_guest_init();
    rlua_guest_register_builtins(g_rlua);

    discover_mods();
    for (int i = 0; i < g_rlua_mod_count; ++i) load_mod_chunk(i);

    fprintf(stderr, "[lua] ---- %s ----\n", LUA_RELEASE);
    for (int i = 0; i < g_rlua_mod_count; ++i)
        fprintf(stderr, "[lua]  %-20s %-3s  %s\n",
                g_rlua_mods[i].id, g_rlua_mods[i].loaded ? "ok" : "ERR",
                g_rlua_mods[i].main);
    if (!g_rlua_mod_count)
        fprintf(stderr, "[lua]  (no mods in %s/)\n", MODS_ROOT);
    fprintf(stderr, "[lua] ------------------------\n");
    fflush(stderr);

    { const char *e = getenv("ROBOX_LUA_RELOAD"); if (e && *e == '0') g_reload_on = 0; }

    g_last_ticks = (double)SDL_GetTicks() / 1000.0;
    rlua_fire_n(RLUA_EV_START, 0, -1);
}

void robox_lua_tick(void) {
    if (!g_rlua) return;

    const double now = (double)SDL_GetTicks() / 1000.0;
    double dt = now - g_last_ticks;
    /* A load screen or a breakpoint should not hand every mod a two-second
     * dt and teleport whatever it was integrating. */
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.25) dt = 0.25;
    g_last_ticks = now;
    g_rlua_time += dt;
    ++g_rlua_frame;

    rlua_toasts_tick(dt);
    rlua_api_tick();          /* button edges + injected-press countdown */

    if (g_reload_on) {
        g_reload_accum += dt;
        if (g_reload_accum >= 0.35) { g_reload_accum = 0.0; check_reloads(); }
    }

    /* Level changes, from the game's own current-level word. */
    {
        extern int robox_level_id(void);
        extern const char *robox_level_name(void);
        extern int robox_level_is_robot(void);
        static int last_id = -1;
        const int id = robox_level_id();
        if (id != last_id) {
            last_id = id;
            lua_pushinteger(g_rlua, id);
            lua_pushstring(g_rlua, robox_level_name());
            lua_pushboolean(g_rlua, robox_level_is_robot());
            rlua_fire_n(RLUA_EV_LEVEL, 3, -1);
        }
    }

    fire_key_edges();

    lua_pushnumber(g_rlua, dt);
    rlua_fire_n(RLUA_EV_FRAME, 1, -1);

    /* Mods leak stack slots only through bugs in the bindings, but a slow
     * leak here would be invisible for hours and then fatal. Cheap to check. */
    const int top = lua_gettop(g_rlua);
    if (top != 0) {
        fprintf(stderr, "[lua] stack left at %d after tick -- clearing\n", top);
        lua_settop(g_rlua, 0);
    }
}

void robox_lua_render(void) {
    if (!g_rlua) return;
    extern void gx_ogl_overlay_begin(void);
    extern void gx_ogl_overlay_end(void);

    gx_ogl_overlay_begin();
    g_in_draw = 1;
    rlua_fire_n(RLUA_EV_DRAW, 0, -1);
    g_in_draw = 0;
    rlua_toasts_render();
    gx_ogl_overlay_end();
}

/* Bindings ask this before drawing so a stray robox.draw.rect() from a frame
 * handler reports itself instead of silently landing in the next batch. */
int rlua_in_draw(void) { return g_in_draw; }

#else   /* __3DS__ : no overlay renderer, and 26 MB of BSS already trimmed */

void     robox_lua_init(void)   {}
int      robox_lua_active(void) { return 0; }
void     robox_lua_tick(void)   {}
void     robox_lua_render(void) {}
uint32_t robox_lua_input_mask(void) { return 0; }
const char *robox_lua_guest_redirect(const char *p) { (void)p; return 0; }

#endif  /* !__3DS__ */
