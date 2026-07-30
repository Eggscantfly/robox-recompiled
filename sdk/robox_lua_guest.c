// sdk/robox_lua_guest.c -- running code inside the GAME's own Lua VM.
//
// Robox ships a complete Lua 5.1 in Robox USA.dol: LUA_PATH, the whole
// standard library (string/table/math/io/os/debug/coroutine), and the engine's
// own bindings -- loadMap, loadMap2, newFpg, unloadFpg, unloadTiles,
// loadTileset, cargaAnimacion, renderQuad, getText. All it is ever used for is
// listing tilesets and holding translated strings:
//
//     textureDir  = "media/spr/";
//     textureList = { "bone" };
//
// So there is a scripting engine already inside the game, wired to the
// renderer and the asset loader, and it has never run a line of gameplay code.
// This file hands it to mods.
//
// HOW, WITHOUT TOUCHING THE GUEST
// Everything the guest's Lua reads goes through the content system --
// script/game.lua, and every file its dofile() pulls in after that:
//
//     [CNT] open 'script/game.lua'          -> slot 0, 186 bytes
//     [CNT] open 'script/platformSettings.lua'
//     [CNT] open 'script/anim.lua'
//
// That is the seam. sdk/robox_io.c asks robox_lua_guest_redirect() for every
// path the guest opens, so:
//
//   1. script/game.lua is served from a merged copy -- the real 186 bytes,
//      then a preamble, then every mods/lua/guest/*.lua concatenated. The
//      game's own boot runs it; nothing in the DOL is patched, and the merge
//      survives a recompile because it happens at the file layer.
//
//   2. A path starting __robox_host__/ is not a file at all. The guest reaches
//      it through dofile(), which means it is a synchronous call that returns
//      a value -- so the answer is written as a one-line chunk `return <x>` and
//      served. That gives the guest VM a way to call the host VM: arguments in
//      the path, result through dofile's return. See robox.host() below.
//
// The RPC is not fast -- a file write and a parse per call -- and it is not
// meant to be. It is for a guest script that wants to ask the host something
// once in a while, not something to do sixty times a second.

#include "robox_lua.h"

#if !defined(__3DS__)

#include "robox_lua_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "hle.h"        /* robox_mkdir */

#define GUEST_DIR    "mods/lua/guest"
#define CACHE_DIR    "mods/lua/.cache"
#define MERGED_PATH  CACHE_DIR "/game.lua"
#define RPC_PATH     CACHE_DIR "/rpc.lua"
#define RPC_PREFIX   "__robox_host__/"

static int g_guest_ready;        /* the merged game.lua exists and has mods  */
static int g_rpc_ref = LUA_NOREF;/* registry: { name -> function }           */

int rlua_guest_available(void) { return g_guest_ready; }

/* The shim the guest side gets for free. Lua 5.1: no goto, no integer
 * division, `#` on tables, select("#", ...) is available. Keep it to that. */
static const char *GUEST_PREAMBLE =
"\n-- ---- injected by the Robox port (sdk/robox_lua_guest.c) ----\n"
"robox = robox or {}\n"
"local function _hex(s)\n"
"  return (string.gsub(tostring(s), \".\", function(c)\n"
"    return string.format(\"%02x\", string.byte(c)) end))\n"
"end\n"
"function robox.host(name, ...)\n"
"  local p = \"" RPC_PREFIX "\" .. _hex(name)\n"
"  for i = 1, select(\"#\", ...) do\n"
"    local v = select(i, ...)\n"
"    p = p .. \"/\" .. string.sub(type(v), 1, 1) .. _hex(v)\n"
"  end\n"
"  -- pcall, because this is a dofile of a path that is not a file: if the\n"
"  -- port cannot answer, the read fails and a bare dofile would raise --\n"
"  -- inside the game's boot, which must not be breakable from a mod.\n"
"  local ok, v = pcall(dofile, p)\n"
"  if ok then return v end\n"
"  return nil\n"
"end\n"
"function robox.log(...)\n"
"  local t = {}\n"
"  for i = 1, select(\"#\", ...) do t[i] = tostring((select(i, ...))) end\n"
"  robox.host(\"log\", table.concat(t, \"\\t\"))\n"
"end\n"
"-- ------------------------------------------------------------\n";

/* --- building the merged script/game.lua --------------------------------- */

static int append_file(FILE *out, const char *path) {
    FILE *in = fopen(path, "rb");
    if (!in) return 0;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fputc('\n', out);
    return 1;
}

/* The real asset, wherever sdk/robox_io.c would have found it. Duplicated
 * rather than exported because assets_root() is that file's private business
 * and the probe is two fopens on one boot. */
static const char *real_game_lua(void) {
    static const char *const CANDIDATES[] = {
        "Assets/script/game.lua", "../Assets/script/game.lua", NULL
    };
    for (int i = 0; CANDIDATES[i]; ++i) {
        FILE *f = fopen(CANDIDATES[i], "rb");
        if (f) { fclose(f); return CANDIDATES[i]; }
    }
    const char *env = getenv("RECOMP_ASSETS");
    if (env && *env) {
        static char p[512];
        snprintf(p, sizeof p, "%s/script/game.lua", env);
        FILE *f = fopen(p, "rb");
        if (f) { fclose(f); return p; }
    }
    return NULL;
}

static int cmp_names(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void rlua_guest_init(void) {
    /* Which guest scripts are there? Sorted, so load order is something a mod
     * author can control with a 10_ prefix rather than guess at. */
    char  names[32][128];
    char *ptrs[32];
    int   count = 0;

    DIR *d = opendir(GUEST_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && count < 32) {
            size_t n = strlen(e->d_name);
            if (e->d_name[0] == '.' || e->d_name[0] == '_') continue;
            if (n < 5 || strcmp(e->d_name + n - 4, ".lua")) continue;
            snprintf(names[count], sizeof names[0], "%s", e->d_name);
            ptrs[count] = names[count];
            ++count;
        }
        closedir(d);
    }
    if (!count) return;                 /* nothing to inject; stay out of the way */
    qsort(ptrs, (size_t)count, sizeof ptrs[0], cmp_names);

    const char *base = real_game_lua();
    if (!base) {
        fprintf(stderr, "[lua] guest scripts found but Assets/script/game.lua is not "
                        "where robox_io looks -- skipping the guest bridge\n");
        return;
    }

    robox_mkdir(CACHE_DIR);
    FILE *out = fopen(MERGED_PATH, "wb");
    if (!out) {
        fprintf(stderr, "[lua] cannot write %s -- skipping the guest bridge\n",
                MERGED_PATH);
        return;
    }

    append_file(out, base);
    fputs(GUEST_PREAMBLE, out);
    for (int i = 0; i < count; ++i) {
        char p[512];
        snprintf(p, sizeof p, "%s/%s", GUEST_DIR, ptrs[i]);
        fprintf(out, "\n-- ==== %s ====\n", ptrs[i]);
        if (!append_file(out, p))
            fprintf(stderr, "[lua] guest: cannot read %s\n", p);
        else
            fprintf(stderr, "[lua] guest: injecting %s\n", ptrs[i]);
    }
    fclose(out);

    g_guest_ready = 1;
    fprintf(stderr, "[lua] guest bridge armed: %d script(s) merged into "
                    "script/game.lua\n", count);
    fflush(stderr);
}

/* --- the RPC ------------------------------------------------------------- */

static int unhex(const char *in, size_t len, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i + 1 < len && o + 1 < cap; i += 2) {
        char h[3] = { in[i], in[i + 1], 0 };
        char *end = NULL;
        long v = strtol(h, &end, 16);
        if (end != h + 2) return -1;
        out[o++] = (char)v;
    }
    out[o] = 0;
    return (int)o;
}

/* Write `return <value at stack top>` so the guest's dofile() hands it back.
 * Only the types that survive the trip: a function or userdata means nothing
 * on the other side, and a table is emitted one level deep because that is
 * where a path-based protocol stops being worth it. */
static void serialise(lua_State *L, int idx, FILE *f) {
    switch (lua_type(L, idx)) {
        case LUA_TNIL:
        case LUA_TNONE:    fputs("nil", f); break;
        case LUA_TBOOLEAN: fputs(lua_toboolean(L, idx) ? "true" : "false", f); break;
        case LUA_TNUMBER:  fprintf(f, "%.14g", (double)lua_tonumber(L, idx)); break;
        case LUA_TSTRING: {
            size_t n = 0;
            const char *s = lua_tolstring(L, idx, &n);
            fputc('"', f);
            for (size_t i = 0; i < n; ++i) {
                unsigned char c = (unsigned char)s[i];
                if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
                else if (c == '\n')        fputs("\\n", f);
                else if (c < 32 || c == 127) fprintf(f, "\\%03u", c);
                else fputc(c, f);
            }
            fputc('"', f);
            break;
        }
        case LUA_TTABLE: {
            /* idx is always an absolute index here -- both call sites pass
             * lua_gettop() -- which matters because lua_next pushes and pops
             * around it and a relative index would slide. */
            fputs("{", f);
            lua_pushnil(L);
            int first = 1;
            while (lua_next(L, idx)) {
                if (!first) fputs(",", f);
                first = 0;
                if (lua_type(L, -2) == LUA_TSTRING) {
                    fprintf(f, "[\"%s\"]=", lua_tostring(L, -2));
                } else if (lua_type(L, -2) == LUA_TNUMBER) {
                    fprintf(f, "[%.14g]=", (double)lua_tonumber(L, -2));
                } else {
                    lua_pop(L, 1); continue;
                }
                /* One level: nested tables come out as nil rather than
                 * recursing into something that might be cyclic. */
                if (lua_type(L, -1) == LUA_TTABLE) fputs("nil", f);
                else serialise(L, lua_gettop(L), f);
                lua_pop(L, 1);
            }
            fputs("}", f);
            break;
        }
        default: fputs("nil", f); break;
    }
}

static const char *rpc_fail(const char *why) {
    FILE *f = fopen(RPC_PATH, "wb");
    if (!f) return NULL;
    fprintf(f, "-- %s\nreturn nil\n", why);
    fclose(f);
    fprintf(stderr, "[lua] guest RPC: %s\n", why);
    fflush(stderr);
    return RPC_PATH;
}

/* path is everything after __robox_host__/ :
 *   <hex name> [ "/" <type letter> <hex value> ]... */
const char *rlua_guest_rpc_answer(const char *path) {
    lua_State *L = g_rlua;
    if (!L) return NULL;
    if (g_rpc_ref == LUA_NOREF) return rpc_fail("no host handlers registered");

    const int top = lua_gettop(L);

    /* Name. */
    const char *slash = strchr(path, '/');
    const size_t nlen = slash ? (size_t)(slash - path) : strlen(path);
    char name[128];
    if (unhex(path, nlen, name, sizeof name) < 0)
        return rpc_fail("malformed handler name");

    lua_rawgeti(L, LUA_REGISTRYINDEX, g_rpc_ref);
    lua_getfield(L, -1, name);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, top);
        char why[192];
        snprintf(why, sizeof why, "no handler called '%s'", name);
        return rpc_fail(why);
    }
    lua_remove(L, -2);                  /* drop the handler table */

    /* Arguments. */
    int nargs = 0;
    const char *p = slash;
    while (p && *p == '/' && nargs < 16) {
        ++p;
        const char *end = strchr(p, '/');
        const size_t seg = end ? (size_t)(end - p) : strlen(p);
        if (seg < 1) break;
        const char kind = p[0];
        char val[512];
        if (unhex(p + 1, seg - 1, val, sizeof val) < 0) { lua_settop(L, top); return rpc_fail("malformed argument"); }
        switch (kind) {
            case 'n': lua_pushnumber (L, strtod(val, NULL)); break;
            case 'b': lua_pushboolean(L, strcmp(val, "true") == 0); break;
            case 'i': lua_pushnil(L); break;                   /* nil */
            default:  lua_pushstring (L, val); break;          /* 's' and anything else */
        }
        ++nargs;
        p = end;
    }

    const int saved = g_rlua_cur;
    const int rc = lua_pcall(L, nargs, 1, 0);
    g_rlua_cur = saved;

    if (rc != LUA_OK) {
        char why[256];
        snprintf(why, sizeof why, "handler '%s' failed: %s", name, lua_tostring(L, -1));
        lua_settop(L, top);
        return rpc_fail(why);
    }

    FILE *f = fopen(RPC_PATH, "wb");
    if (!f) { lua_settop(L, top); return NULL; }
    fputs("return ", f);
    serialise(L, lua_gettop(L), f);
    fputs("\n", f);
    fclose(f);

    lua_settop(L, top);
    return RPC_PATH;
}

/* --- the seam sdk/robox_io.c calls --------------------------------------- */

const char *robox_lua_guest_redirect(const char *guest_path) {
    if (!guest_path || !guest_path[0]) return NULL;

    if (!strncmp(guest_path, RPC_PREFIX, sizeof RPC_PREFIX - 1))
        return rlua_guest_rpc_answer(guest_path + sizeof RPC_PREFIX - 1);

    if (g_guest_ready && !strcmp(guest_path, "script/game.lua"))
        return MERGED_PATH;

    return NULL;
}

/* --- robox.game, the host side ------------------------------------------- */

/* robox.game.handle(name, fn) -- fn becomes callable from the guest VM as
 * robox.host(name, ...). One shared table across mods; a later registration
 * of the same name wins, same as any other hook. */
static int l_game_handle(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    if (g_rpc_ref == LUA_NOREF) {
        lua_newtable(L);
        g_rpc_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_rpc_ref);
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
    return 0;
}

static int l_game_available(lua_State *L) {
    lua_pushboolean(L, g_guest_ready);
    return 1;
}

static int l_game_dir(lua_State *L) {
    lua_pushstring(L, GUEST_DIR);
    return 1;
}

void rlua_open_game_api(lua_State *L) {
    /* Called with the robox table on top; leaves it there. */
    lua_newtable(L);
    lua_pushcfunction(L, l_game_handle);    lua_setfield(L, -2, "handle");
    lua_pushcfunction(L, l_game_available); lua_setfield(L, -2, "available");
    lua_pushcfunction(L, l_game_dir);       lua_setfield(L, -2, "dir");
    lua_setfield(L, -2, "game");
}

/* Registered by the runtime itself so robox.log() works on the guest side
 * before any mod has registered anything. */
static int l_builtin_log(lua_State *L) {
    fprintf(stderr, "[lua:guest] %s\n", luaL_optstring(L, 1, ""));
    fflush(stderr);
    return 0;
}

void rlua_guest_register_builtins(lua_State *L) {
    if (g_rpc_ref == LUA_NOREF) {
        lua_newtable(L);
        g_rpc_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_rpc_ref);
    lua_pushcfunction(L, l_builtin_log);
    lua_setfield(L, -2, "log");
    lua_pop(L, 1);
}

#endif  /* !__3DS__ */
