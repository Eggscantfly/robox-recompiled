// sdk/robox_lua_api.c -- the robox.* table handed to every Lua mod.
//
// This is the whole reason the runtime exists: a mod is only as interesting as
// what it can reach. The surface here is deliberately close to what the C mods
// in this tree already do, because those are the proof that it is enough --
// co-op replaces a vtable entry and rewrites an input word, the Mario mod
// hooks the player's update and integrates its own physics, the menu reads
// CPlayer fields through a pointer. All three are expressible below.
//
// Guest memory is little more than a big array with byte-swapping accessors
// (see src/runtime.h), so mem.* is thin. The two pieces with real machinery
// are hooks and calls:
//
//   HOOKS   ppc_patch_func() swaps one entry of the dispatch table, but the
//           entries are void(void) with no room for a payload -- so a fixed
//           pool of trampolines each carry their slot index as the only thing
//           that distinguishes them. Same trick sdk/robox_coop.c uses for its
//           vtable profiler, generalised.
//
//   CALLS   Calling back into the guest means standing up a PowerPC call frame
//           by hand: arguments into r3.. and f1.., a scratch area for strings
//           carved out of the guest stack, and the entire register file saved
//           and restored around it -- the tick this runs from is itself inside
//           guest execution, so anything left modified would land in the
//           middle of whatever function was interrupted.

#include "robox_lua.h"

#if !defined(__3DS__)

#include "robox_lua_int.h"
#include "../src/runtime.h"
#include "gx_ogl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <SDL2/SDL.h>

/* Owned elsewhere in sdk/. */
extern uint32_t    video_input_hold(void);
extern uint32_t    video_input_raw(void);
extern int         robox_level_id(void);
extern const char *robox_level_name(void);
extern int         robox_level_is_robot(void);
extern int         robox_mod_enabled(const char *id);
extern int         g_audio_volume;
extern int         g_music_on;
extern int         rlua_in_draw(void);

/* --- guest memory -------------------------------------------------------- */

/* Real RAM only. ppc_host_ptr never fails -- it folds anything unmapped into
 * a deadzone -- which is right for recompiled code and wrong here: a mod that
 * reads a bad pointer should be told, not handed a plausible zero. */
static int mem_ok(uint32_t va, uint32_t n) {
    if (n == 0) return 0;
    if (va + n < va) return 0;                       /* wrap */
    uint32_t a = va;
    if (a >= PPC_MEM1_UNCACHED_BASE && a < PPC_MEM1_UNCACHED_BASE + PPC_MEM1_SIZE)
        a = a - PPC_MEM1_UNCACHED_BASE + PPC_MEM1_BASE;
    else if (a >= PPC_MEM2_UNCACHED_BASE && a < PPC_MEM2_UNCACHED_BASE + PPC_MEM2_SIZE)
        a = a - PPC_MEM2_UNCACHED_BASE + PPC_MEM2_BASE;
    if (a >= PPC_MEM1_BASE && a + n <= PPC_MEM1_BASE + PPC_MEM1_SIZE) return 1;
    if (a >= PPC_MEM2_BASE && a + n <= PPC_MEM2_BASE + PPC_MEM2_SIZE) return 1;
    return 0;
}

static uint32_t arg_va(lua_State *L, int i) {
    return (uint32_t)(int64_t)luaL_checkinteger(L, i);
}

#define MEM_READER(name, size, expr, push)                              \
    static int l_mem_##name(lua_State *L) {                             \
        uint32_t va = arg_va(L, 1);                                     \
        if (!mem_ok(va, size)) return 0;                                \
        push(L, expr);                                                  \
        return 1;                                                       \
    }

MEM_READER(u8,  1, (lua_Integer)MEM_R8(va),           lua_pushinteger)
MEM_READER(u16, 2, (lua_Integer)MEM_R16(va),          lua_pushinteger)
MEM_READER(u32, 4, (lua_Integer)MEM_R32(va),          lua_pushinteger)
MEM_READER(i8,  1, (lua_Integer)(int8_t)MEM_R8(va),   lua_pushinteger)
MEM_READER(i16, 2, (lua_Integer)(int16_t)MEM_R16(va), lua_pushinteger)
MEM_READER(i32, 4, (lua_Integer)(int32_t)MEM_R32(va), lua_pushinteger)
MEM_READER(f32, 4, (lua_Number)MEM_RF(va),            lua_pushnumber)
MEM_READER(f64, 8, (lua_Number)MEM_RD(va),            lua_pushnumber)

#define MEM_WRITER(name, size, call)                                    \
    static int l_mem_write_##name(lua_State *L) {                       \
        uint32_t va = arg_va(L, 1);                                     \
        if (!mem_ok(va, size))                                          \
            return luaL_error(L, "write_" #name ": 0x%08x is not guest RAM", \
                              (unsigned)va);                            \
        call;                                                           \
        return 0;                                                       \
    }

MEM_WRITER(u8,  1, MEM_W8 (va, (uint8_t) luaL_checkinteger(L, 2)))
MEM_WRITER(u16, 2, MEM_W16(va, (uint16_t)luaL_checkinteger(L, 2)))
MEM_WRITER(u32, 4, MEM_W32(va, (uint32_t)(int64_t)luaL_checkinteger(L, 2)))
MEM_WRITER(f32, 4, MEM_WF (va, (float)   luaL_checknumber (L, 2)))
MEM_WRITER(f64, 8, MEM_WD (va, (double)  luaL_checknumber (L, 2)))

static int l_mem_valid(lua_State *L) {
    lua_pushboolean(L, mem_ok(arg_va(L, 1),
                              (uint32_t)luaL_optinteger(L, 2, 1)));
    return 1;
}

static int l_mem_read(lua_State *L) {
    uint32_t va = arg_va(L, 1);
    uint32_t n  = (uint32_t)luaL_checkinteger(L, 2);
    if (!mem_ok(va, n)) return 0;
    luaL_Buffer b;
    char *p = luaL_buffinitsize(L, &b, n);
    for (uint32_t i = 0; i < n; ++i) p[i] = (char)MEM_R8(va + i);
    luaL_pushresultsize(&b, n);
    return 1;
}

static int l_mem_write(lua_State *L) {
    uint32_t va = arg_va(L, 1);
    size_t n = 0;
    const char *s = luaL_checklstring(L, 2, &n);
    if (!mem_ok(va, (uint32_t)n))
        return luaL_error(L, "write: 0x%08x+%d is not guest RAM",
                          (unsigned)va, (int)n);
    for (size_t i = 0; i < n; ++i) MEM_W8(va + (uint32_t)i, (uint8_t)s[i]);
    return 0;
}

static int l_mem_cstr(lua_State *L) {
    uint32_t va  = arg_va(L, 1);
    uint32_t max = (uint32_t)luaL_optinteger(L, 2, 256);
    if (!mem_ok(va, 1)) return 0;
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (uint32_t i = 0; i < max && mem_ok(va + i, 1); ++i) {
        char c = (char)MEM_R8(va + i);
        if (!c) break;
        luaL_addchar(&b, c);
    }
    luaL_pushresult(&b);
    return 1;
}

/* follow(base, off1, off2, ...) -- dereference at each step but the last, the
 * shape every "player -> component -> field" lookup has. nil the moment the
 * chain leaves RAM, so a mod can test instead of guarding every hop. */
static int l_mem_follow(lua_State *L) {
    uint32_t va = arg_va(L, 1);
    const int n = lua_gettop(L);
    for (int i = 2; i <= n; ++i) {
        if (!mem_ok(va, 4)) return 0;
        va = MEM_R32(va);
        va += (uint32_t)(int64_t)luaL_checkinteger(L, i);
    }
    if (!mem_ok(va, 1)) return 0;
    lua_pushinteger(L, (lua_Integer)va);
    return 1;
}

/* find("48 ?? 00 3f", from, to) -- byte-signature scan with ?? wildcards.
 * The way anybody actually locates a structure they only know the shape of. */
static int l_mem_find(lua_State *L) {
    const char *pat = luaL_checkstring(L, 1);
    uint32_t from = (uint32_t)luaL_optinteger(L, 2, PPC_MEM1_BASE);
    uint32_t to   = (uint32_t)luaL_optinteger(L, 3, PPC_MEM1_BASE + PPC_MEM1_SIZE);

    uint8_t  bytes[64];
    uint8_t  mask [64];
    int      len = 0;
    for (const char *p = pat; *p && len < 64; ) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (p[0] == '?' ) { bytes[len] = 0; mask[len] = 0; ++len; p += (p[1] == '?') ? 2 : 1; continue; }
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1]))
            return luaL_error(L, "find: bad signature near '%s'", p);
        char hex[3] = { p[0], p[1], 0 };
        bytes[len] = (uint8_t)strtoul(hex, NULL, 16);
        mask[len]  = 1;
        ++len; p += 2;
    }
    if (!len) return luaL_error(L, "find: empty signature");
    if (to <= from || !mem_ok(from, 1)) return 0;

    for (uint32_t va = from; va + (uint32_t)len <= to; ++va) {
        if (!mem_ok(va, (uint32_t)len)) break;
        int ok = 1;
        for (int i = 0; i < len; ++i)
            if (mask[i] && MEM_R8(va + (uint32_t)i) != bytes[i]) { ok = 0; break; }
        if (ok) { lua_pushinteger(L, (lua_Integer)va); return 1; }
    }
    return 0;
}

/* --- CPU registers ------------------------------------------------------- */

static int l_cpu_gpr(lua_State *L) {
    int i = (int)luaL_checkinteger(L, 1);
    if (i < 0 || i > 31) return luaL_error(L, "gpr: index %d out of range", i);
    lua_pushinteger(L, (lua_Integer)g_cpu.gpr[i]);
    if (!lua_isnoneornil(L, 2))
        g_cpu.gpr[i] = (uint32_t)(int64_t)luaL_checkinteger(L, 2);
    return 1;                       /* the value BEFORE any write */
}

static int l_cpu_fpr(lua_State *L) {
    int i = (int)luaL_checkinteger(L, 1);
    if (i < 0 || i > 31) return luaL_error(L, "fpr: index %d out of range", i);
    lua_pushnumber(L, (lua_Number)g_cpu.fpr[i].f64);
    if (!lua_isnoneornil(L, 2)) {
        double v = (double)luaL_checknumber(L, 2);
        g_cpu.fpr[i].ps[0] = v;
        g_cpu.fpr[i].ps[1] = v;     /* scalar ops replicate; see runtime.h */
    }
    return 1;
}

static int l_cpu_lr (lua_State *L) { lua_pushinteger(L, g_cpu.lr);  return 1; }
static int l_cpu_ctr(lua_State *L) { lua_pushinteger(L, g_cpu.ctr); return 1; }

/* --- calling guest functions --------------------------------------------- */

/* Scratch for string arguments, carved out of the guest stack exactly the way
 * a C caller carves out a local buffer: lower r1 past it for the duration of
 * the call, so the callee's own frames land below and cannot reach it. */
#define CALL_SCRATCH  512

static int guest_call(lua_State *L, uint32_t va,
                      int gpr_first, int gpr_last, int fpr_tbl) {
    PPC_Func fn = ppc_lookup_func(va);
    if (!fn) return luaL_error(L, "call: 0x%08x is not a known guest function",
                               (unsigned)va);

    const uint32_t sp     = g_cpu.gpr[1];
    const uint32_t newsp  = (sp - CALL_SCRATCH) & ~0xfu;
    uint32_t       strtop = newsp;          /* strings fill upward from here */

    /* Marshal into locals FIRST and only then commit to g_cpu.
     *
     * Everything in this loop can raise -- luaL_checkinteger on a float, the
     * scratch overflowing -- and a Lua error is a longjmp straight out of this
     * function. Writing argument registers as we went would leave the guest
     * holding half an argument list in the middle of whatever call the frame
     * hook interrupted, with no path left that could put them back. Writing
     * bytes into the stack scratch below sp is fine to do early: that memory
     * is dead either way. */
    uint32_t gv[8];
    double   fv[8];
    int      ngv = 0, nfv = 0;

    for (int i = gpr_first; i <= gpr_last && ngv < 8; ++i, ++ngv) {
        if (lua_type(L, i) == LUA_TSTRING) {
            size_t n = 0;
            const char *s = lua_tolstring(L, i, &n);
            if (strtop + n + 1 > sp)
                return luaL_error(L, "call: string arguments exceed %d bytes",
                                  CALL_SCRATCH);
            for (size_t k = 0; k <= n; ++k) MEM_W8(strtop + (uint32_t)k, (uint8_t)s[k]);
            gv[ngv] = strtop;
            strtop  = (strtop + (uint32_t)n + 1 + 3u) & ~3u;
        } else if (lua_isboolean(L, i)) {
            gv[ngv] = (uint32_t)lua_toboolean(L, i);
        } else {
            gv[ngv] = (uint32_t)(int64_t)luaL_checkinteger(L, i);
        }
    }

    if (fpr_tbl) {
        lua_Integer n = (lua_Integer)lua_rawlen(L, fpr_tbl);
        for (lua_Integer k = 1; k <= n && nfv < 8; ++k, ++nfv) {
            lua_rawgeti(L, fpr_tbl, k);
            fv[nfv] = (double)lua_tonumber(L, -1);
            lua_pop(L, 1);
        }
    }

    const PPCContext saved = g_cpu;

    for (int i = 0; i < ngv; ++i) g_cpu.gpr[3 + i] = gv[i];
    for (int i = 0; i < nfv; ++i) {
        g_cpu.fpr[1 + i].ps[0] = fv[i];
        g_cpu.fpr[1 + i].ps[1] = fv[i];     /* scalar ops replicate; runtime.h */
    }
    g_cpu.gpr[1] = newsp;
    g_cpu.lr     = 0;               /* recompiled bodies return through C */

    fn();

    const uint32_t r3 = g_cpu.gpr[3];
    const double   f1 = g_cpu.fpr[1].f64;
    g_cpu = saved;

    lua_pushinteger(L, (lua_Integer)r3);
    lua_pushnumber (L, (lua_Number)f1);
    return 2;
}

static int l_call(lua_State *L) {
    return guest_call(L, arg_va(L, 1), 2, lua_gettop(L), 0);
}

/* call_ex(va, {ints...}, {floats...}) for the EABI cases plain call cannot
 * express: a float in f1 alongside a pointer in r3. */
static int l_call_ex(lua_State *L) {
    uint32_t va = arg_va(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    const int has_f = !lua_isnoneornil(L, 3);
    if (has_f) luaL_checktype(L, 3, LUA_TTABLE);

    /* Flatten the int table onto the stack so guest_call sees plain args. */
    const int base = lua_gettop(L);
    lua_Integer n = (lua_Integer)lua_rawlen(L, 2);
    if (n > 8) n = 8;
    for (lua_Integer k = 1; k <= n; ++k) lua_rawgeti(L, 2, k);
    return guest_call(L, va, base + 1, base + (int)n, has_f ? 3 : 0);
}

/* --- hooks --------------------------------------------------------------- */

#define RLUA_MAX_HOOKS 64

enum { HK_REPLACE, HK_BEFORE, HK_AFTER };

typedef struct {
    uint32_t va;
    PPC_Func orig;
    int      mod;
    int      ref;
    int      kind;
    int      used;
} rlua_hook_t;

static rlua_hook_t g_hooks[RLUA_MAX_HOOKS];
static int         g_hook_depth;        /* re-entry guard */
static int         g_hook_cur = -1;     /* slot whose handler is running */

static int find_hook(uint32_t va) {
    for (int i = 0; i < RLUA_MAX_HOOKS; ++i)
        if (g_hooks[i].used && g_hooks[i].va == va) return i;
    return -1;
}

/* Run the Lua side of hook `i`. Returns 0 if the handler asked to suppress
 * the original (returned exactly `false`).
 *
 * `args` is a snapshot taken before anything else ran, not a read of r3..r6
 * now: an "after" handler runs once the original has returned, and by then
 * those registers hold its result and whatever it left behind. Reading them
 * live would hand an after-hook garbage where it asked for `this`. */
static int hook_invoke(int i, const uint32_t args[4]) {
    lua_State *L = g_rlua;
    rlua_hook_t *h = &g_hooks[i];
    if (!L || h->ref == LUA_NOREF) return 1;

    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, h->ref);
    if (!lua_isfunction(L, -1)) { lua_settop(L, top); return 1; }

    /* The first four argument registers, because that is what almost every
     * hook wants and reading them back through robox.cpu.gpr costs a call
     * each. Anything beyond is still there for the asking. */
    for (int r = 0; r < 4; ++r) lua_pushinteger(L, (lua_Integer)args[r]);

    const int saved_mod  = g_rlua_cur;
    const int saved_hook = g_hook_cur;
    g_rlua_cur  = h->mod;
    g_hook_cur  = i;
    const int rc = lua_pcall(L, 4, 1, 0);
    g_rlua_cur  = saved_mod;
    g_hook_cur  = saved_hook;

    int run_original = 1;
    if (rc != LUA_OK) {
        rlua_report_error(h->mod, "hook", lua_tostring(L, -1));
    } else if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
        run_original = 0;
    } else if (lua_isinteger(L, -1)) {
        g_cpu.gpr[3] = (uint32_t)(int64_t)lua_tointeger(L, -1);
    }
    lua_settop(L, top);
    return run_original;
}

static void hook_run(int i) {
    rlua_hook_t *h = &g_hooks[i];

    /* A handler that calls back into the hooked function -- directly, or via
     * anything the game does underneath it -- would recurse forever. Past the
     * first level, be the original function and nothing else. */
    if (g_hook_depth > 0 || !h->used) { if (h->orig) h->orig(); return; }

    const uint32_t args[4] = {
        g_cpu.gpr[3], g_cpu.gpr[4], g_cpu.gpr[5], g_cpu.gpr[6]
    };

    ++g_hook_depth;
    switch (h->kind) {
        case HK_AFTER:
            if (h->orig) h->orig();
            hook_invoke(i, args);
            break;
        case HK_BEFORE:
            if (hook_invoke(i, args) && h->orig) h->orig();
            break;
        default:                        /* HK_REPLACE */
            hook_invoke(i, args);
            break;
    }
    --g_hook_depth;
}

/* ppc_patch_func takes a bare void(void), so the only way to tell one hook
 * from another is to have a distinct function per slot. Spelled out rather
 * than generated by a counting macro: ## pastes tokens, not arithmetic, so
 * anything cleverer here silently produces `hook_tramp_0+1`. */
#define HOOK_TRAMP(n) static void hook_tramp_##n(void) { hook_run(n); }
HOOK_TRAMP(0)  HOOK_TRAMP(1)  HOOK_TRAMP(2)  HOOK_TRAMP(3)
HOOK_TRAMP(4)  HOOK_TRAMP(5)  HOOK_TRAMP(6)  HOOK_TRAMP(7)
HOOK_TRAMP(8)  HOOK_TRAMP(9)  HOOK_TRAMP(10) HOOK_TRAMP(11)
HOOK_TRAMP(12) HOOK_TRAMP(13) HOOK_TRAMP(14) HOOK_TRAMP(15)
HOOK_TRAMP(16) HOOK_TRAMP(17) HOOK_TRAMP(18) HOOK_TRAMP(19)
HOOK_TRAMP(20) HOOK_TRAMP(21) HOOK_TRAMP(22) HOOK_TRAMP(23)
HOOK_TRAMP(24) HOOK_TRAMP(25) HOOK_TRAMP(26) HOOK_TRAMP(27)
HOOK_TRAMP(28) HOOK_TRAMP(29) HOOK_TRAMP(30) HOOK_TRAMP(31)
HOOK_TRAMP(32) HOOK_TRAMP(33) HOOK_TRAMP(34) HOOK_TRAMP(35)
HOOK_TRAMP(36) HOOK_TRAMP(37) HOOK_TRAMP(38) HOOK_TRAMP(39)
HOOK_TRAMP(40) HOOK_TRAMP(41) HOOK_TRAMP(42) HOOK_TRAMP(43)
HOOK_TRAMP(44) HOOK_TRAMP(45) HOOK_TRAMP(46) HOOK_TRAMP(47)
HOOK_TRAMP(48) HOOK_TRAMP(49) HOOK_TRAMP(50) HOOK_TRAMP(51)
HOOK_TRAMP(52) HOOK_TRAMP(53) HOOK_TRAMP(54) HOOK_TRAMP(55)
HOOK_TRAMP(56) HOOK_TRAMP(57) HOOK_TRAMP(58) HOOK_TRAMP(59)
HOOK_TRAMP(60) HOOK_TRAMP(61) HOOK_TRAMP(62) HOOK_TRAMP(63)

static const PPC_Func g_hook_tramps[RLUA_MAX_HOOKS] = {
    hook_tramp_0,  hook_tramp_1,  hook_tramp_2,  hook_tramp_3,
    hook_tramp_4,  hook_tramp_5,  hook_tramp_6,  hook_tramp_7,
    hook_tramp_8,  hook_tramp_9,  hook_tramp_10, hook_tramp_11,
    hook_tramp_12, hook_tramp_13, hook_tramp_14, hook_tramp_15,
    hook_tramp_16, hook_tramp_17, hook_tramp_18, hook_tramp_19,
    hook_tramp_20, hook_tramp_21, hook_tramp_22, hook_tramp_23,
    hook_tramp_24, hook_tramp_25, hook_tramp_26, hook_tramp_27,
    hook_tramp_28, hook_tramp_29, hook_tramp_30, hook_tramp_31,
    hook_tramp_32, hook_tramp_33, hook_tramp_34, hook_tramp_35,
    hook_tramp_36, hook_tramp_37, hook_tramp_38, hook_tramp_39,
    hook_tramp_40, hook_tramp_41, hook_tramp_42, hook_tramp_43,
    hook_tramp_44, hook_tramp_45, hook_tramp_46, hook_tramp_47,
    hook_tramp_48, hook_tramp_49, hook_tramp_50, hook_tramp_51,
    hook_tramp_52, hook_tramp_53, hook_tramp_54, hook_tramp_55,
    hook_tramp_56, hook_tramp_57, hook_tramp_58, hook_tramp_59,
    hook_tramp_60, hook_tramp_61, hook_tramp_62, hook_tramp_63,
};

static int hook_install(lua_State *L, int kind) {
    uint32_t va = arg_va(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    PPC_Func orig = ppc_lookup_func(va);
    if (!orig)
        return luaL_error(L, "hook: 0x%08x is not in the dispatch table",
                          (unsigned)va);

    int slot = find_hook(va);
    if (slot < 0) {
        for (int i = 0; i < RLUA_MAX_HOOKS; ++i)
            if (!g_hooks[i].used) { slot = i; break; }
        if (slot < 0) return luaL_error(L, "hook: all %d slots are taken",
                                        RLUA_MAX_HOOKS);
        g_hooks[slot].va   = va;
        g_hooks[slot].orig = orig;
        g_hooks[slot].ref  = LUA_NOREF;
        g_hooks[slot].used = 1;
        ppc_patch_func(va, g_hook_tramps[slot]);
    }

    if (g_hooks[slot].ref != LUA_NOREF)
        luaL_unref(L, LUA_REGISTRYINDEX, g_hooks[slot].ref);
    lua_pushvalue(L, 2);
    g_hooks[slot].ref  = luaL_ref(L, LUA_REGISTRYINDEX);
    g_hooks[slot].kind = kind;
    g_hooks[slot].mod  = g_rlua_cur;

    lua_pushinteger(L, (lua_Integer)va);
    return 1;
}

static int l_hook        (lua_State *L) { return hook_install(L, HK_REPLACE); }
static int l_hook_before (lua_State *L) { return hook_install(L, HK_BEFORE);  }
static int l_hook_after  (lua_State *L) { return hook_install(L, HK_AFTER);   }

static void hook_drop(int i) {
    if (!g_hooks[i].used) return;
    ppc_patch_func(g_hooks[i].va, g_hooks[i].orig);
    if (g_hooks[i].ref != LUA_NOREF)
        luaL_unref(g_rlua, LUA_REGISTRYINDEX, g_hooks[i].ref);
    memset(&g_hooks[i], 0, sizeof g_hooks[i]);
    g_hooks[i].ref = LUA_NOREF;
}

static int l_unhook(lua_State *L) {
    int i = find_hook(arg_va(L, 1));
    if (i >= 0) hook_drop(i);
    lua_pushboolean(L, i >= 0);
    return 1;
}

/* Only meaningful inside a replace hook: run the function we displaced. */
static int l_original(lua_State *L) {
    if (g_hook_cur < 0)
        return luaL_error(L, "original() outside a hook handler");
    if (g_hooks[g_hook_cur].orig) g_hooks[g_hook_cur].orig();
    lua_pushinteger(L, (lua_Integer)g_cpu.gpr[3]);
    return 1;
}

/* --- the player ---------------------------------------------------------- */
//
// Offsets from Robox.dmw, the Dolphin watch list kept with the project, and
// the same ones the settings menu's debug tab reads. Exposed by name AND with
// player.addr() so a mod that finds a field we do not know about is not stuck
// waiting for this table to grow.

#define PLAYER_PTR_VA   0x801FACE0u

static const struct { const char *name; uint32_t off; char type; } PLAYER_FIELD[] = {
    { "x",       0x038u, 'f' }, { "y",       0x03Cu, 'f' },
    { "health",  0x228u, 'i' }, { "state",   0x240u, 'i' },
    { "iframes", 0x268u, 'f' }, { "anim",    0x394u, 'i' },
};
#define PLAYER_FIELD_COUNT ((int)(sizeof PLAYER_FIELD / sizeof PLAYER_FIELD[0]))

static uint32_t player_obj(void) {
    if (!mem_ok(PLAYER_PTR_VA, 4)) return 0;
    uint32_t p = MEM_R32(PLAYER_PTR_VA);
    return mem_ok(p, 0x400) ? p : 0;
}

static int l_player_addr(lua_State *L) {
    uint32_t p = player_obj();
    if (!p) return 0;
    lua_pushinteger(L, (lua_Integer)p);
    return 1;
}

static int l_player_get(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    uint32_t p = player_obj();
    if (!p) return 0;
    for (int i = 0; i < PLAYER_FIELD_COUNT; ++i) {
        if (strcmp(name, PLAYER_FIELD[i].name)) continue;
        if (PLAYER_FIELD[i].type == 'f')
            lua_pushnumber(L, (lua_Number)MEM_RF(p + PLAYER_FIELD[i].off));
        else
            lua_pushinteger(L, (lua_Integer)(int32_t)MEM_R32(p + PLAYER_FIELD[i].off));
        return 1;
    }
    return luaL_error(L, "player.get: no field '%s'", name);
}

static int l_player_set(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    uint32_t p = player_obj();
    if (!p) { lua_pushboolean(L, 0); return 1; }
    for (int i = 0; i < PLAYER_FIELD_COUNT; ++i) {
        if (strcmp(name, PLAYER_FIELD[i].name)) continue;
        if (PLAYER_FIELD[i].type == 'f')
            MEM_WF(p + PLAYER_FIELD[i].off, (float)luaL_checknumber(L, 2));
        else
            MEM_W32(p + PLAYER_FIELD[i].off, (uint32_t)(int64_t)luaL_checkinteger(L, 2));
        lua_pushboolean(L, 1);
        return 1;
    }
    return luaL_error(L, "player.set: no field '%s'", name);
}

static int l_player_fields(lua_State *L) {
    lua_createtable(L, 0, PLAYER_FIELD_COUNT);
    for (int i = 0; i < PLAYER_FIELD_COUNT; ++i) {
        lua_pushinteger(L, (lua_Integer)PLAYER_FIELD[i].off);
        lua_setfield(L, -2, PLAYER_FIELD[i].name);
    }
    return 1;
}

/* --- input --------------------------------------------------------------- */
//
// Names match sdk/video.c's control config so "jump" means the same thing in a
// mod, in controls.cfg and in the settings menu.

static const struct { const char *name; uint32_t bit; } BTN[] = {
    { "left",  0x0001u }, { "right", 0x0002u }, { "down",  0x0004u },
    { "up",    0x0008u }, { "plus",  0x0010u }, { "jump",  0x0100u },
    { "action",0x0200u }, { "b",     0x0400u }, { "a",     0x0800u },
    { "minus", 0x1000u }, { "home",  0x8000u },
    { "tiltl", 0x00010000u }, { "tiltr", 0x00020000u }, { "shake", 0x00040000u },
};
#define BTN_COUNT ((int)(sizeof BTN / sizeof BTN[0]))

static uint32_t btn_bit(lua_State *L, int idx) {
    const char *n = luaL_checkstring(L, idx);
    for (int i = 0; i < BTN_COUNT; ++i)
        if (!strcmp(n, BTN[i].name)) return BTN[i].bit;
    luaL_error(L, "input: no button named '%s'", n);
    return 0;
}

/* Buttons a mod is holding. Merged into video_input_hold() rather than poked
 * into the game's input word directly, so an injected press goes through the
 * same d-pad rotation and robot-section remap a real key does. */
static uint32_t g_inject_mask;
static int      g_inject_frames[32];
static int      g_inject_owner [32];    /* mod index, for unload */

static uint32_t g_btn_prev, g_btn_now;

/* Hand control away from the player entirely. Applied in video_input_hold()
 * and video_input_stick() -- i.e. to what the GAME reads -- so it drops
 * buttons and the stick without touching the host event loop. Esc, F6 and
 * every other host key still work, which matters: a mod must never be able to
 * lock someone out of the menu that turns it off. Injected presses still get
 * through, so a mod can block the player and drive him at the same time. */
static int g_input_block;
int robox_lua_input_blocked(void) {
    /* Capture implies it: while a console is eating the keyboard, typing
     * "walk" must not walk. OR'd rather than assigned so closing the console
     * cannot clear a block some other mod is deliberately holding. */
    return g_input_block || robox_lua_capture_active();
}

uint32_t robox_lua_input_mask(void) { return g_inject_mask; }

void rlua_api_tick(void) {
    g_btn_prev = g_btn_now;
    g_btn_now  = video_input_hold();

    for (int b = 0; b < 32; ++b) {
        if (g_inject_frames[b] <= 0) continue;
        if (--g_inject_frames[b] == 0) g_inject_mask &= ~(1u << b);
    }
}

static int l_input_held(lua_State *L) {
    lua_pushboolean(L, (g_btn_now & btn_bit(L, 1)) != 0);
    return 1;
}
static int l_input_pressed(lua_State *L) {
    uint32_t b = btn_bit(L, 1);
    lua_pushboolean(L, (g_btn_now & b) && !(g_btn_prev & b));
    return 1;
}
static int l_input_released(lua_State *L) {
    uint32_t b = btn_bit(L, 1);
    lua_pushboolean(L, !(g_btn_now & b) && (g_btn_prev & b));
    return 1;
}
static int l_input_mask(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)video_input_hold());
    lua_pushinteger(L, (lua_Integer)video_input_raw());
    return 2;
}

/* press(name[, frames]) -- frames defaults to 1, i.e. "this frame only",
 * which is what a mod driving the player from a script wants. */
static int l_input_press(lua_State *L) {
    uint32_t bit = btn_bit(L, 1);
    int frames = (int)luaL_optinteger(L, 2, 1);
    if (frames < 1) frames = 1;
    for (int b = 0; b < 32; ++b) {
        if (!(bit & (1u << b))) continue;
        g_inject_mask |= (1u << b);
        g_inject_frames[b] = frames;
        g_inject_owner [b] = g_rlua_cur;
    }
    return 0;
}

static int l_input_release(lua_State *L) {
    uint32_t bit = btn_bit(L, 1);
    g_inject_mask &= ~bit;
    for (int b = 0; b < 32; ++b) if (bit & (1u << b)) g_inject_frames[b] = 0;
    return 0;
}

/* block(true|false) -> the state it was in before. */
static int l_input_block(lua_State *L) {
    lua_pushboolean(L, g_input_block);
    if (!lua_isnoneornil(L, 1)) g_input_block = lua_toboolean(L, 1);
    return 1;
}

/* capture([bool]) -> was_capturing
 *
 * Take the keyboard: every key edge and every typed character comes to THIS
 * mod and to nobody else, the port's own hotkeys (Escape, the F-keys) stand
 * down, and the guest stops seeing input for as long as it is held. It is what
 * separates a console from a mod that happens to draw a text box.
 *
 * Held by one mod at a time, and dropped automatically if that mod is reloaded
 * or unloaded -- there is no way to leave the keyboard owned by nothing. */
static int l_input_capture(lua_State *L) {
    lua_pushboolean(L, robox_lua_capture_active());
    if (!lua_isnoneornil(L, 1)) rlua_capture_set(lua_toboolean(L, 1));
    return 1;
}

static int l_input_key(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    SDL_Scancode sc = SDL_GetScancodeFromName(name);
    if (sc == SDL_SCANCODE_UNKNOWN)
        return luaL_error(L, "input.key: SDL does not know a key called '%s'", name);
    const Uint8 *st = SDL_GetKeyboardState(NULL);
    lua_pushboolean(L, st && st[sc]);
    return 1;
}

/* mouse() -> x, y, left, right, middle
 *
 * Position is in the overlay's 1280x720 space, not window pixels, so it lines
 * up with robox.draw without the mod redoing the transform. The overlay is
 * drawn over the whole window (gx_ogl_overlay_end sets glViewport to the full
 * size and the shader maps 1280x720 onto it), so this is a straight scale --
 * no letterbox correction, unlike the game's own framebuffer.
 *
 * Note the port hides the system cursor (SDL_ShowCursor(SDL_DISABLE) in
 * video.c), so a mod that wants one draws it. */
static int l_input_mouse(lua_State *L) {
    extern SDL_Window *g_window;
    int mx = 0, my = 0;
    const Uint32 b = SDL_GetMouseState(&mx, &my);
    int ww = 0, wh = 0;
    if (g_window) SDL_GetWindowSize(g_window, &ww, &wh);
    lua_pushnumber(L, ww > 0 ? (lua_Number)mx * 1280.0 / ww : 0.0);
    lua_pushnumber(L, wh > 0 ? (lua_Number)my *  720.0 / wh : 0.0);
    lua_pushboolean(L, (b & SDL_BUTTON(SDL_BUTTON_LEFT))   != 0);
    lua_pushboolean(L, (b & SDL_BUTTON(SDL_BUTTON_RIGHT))  != 0);
    lua_pushboolean(L, (b & SDL_BUTTON(SDL_BUTTON_MIDDLE)) != 0);
    return 5;
}

static int l_input_stick(lua_State *L) {
    extern void video_input_stick(float *x, float *y);
    float x = 0.0f, y = 0.0f;
    video_input_stick(&x, &y);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    return 2;
}

static int l_input_buttons(lua_State *L) {
    lua_createtable(L, 0, BTN_COUNT);
    for (int i = 0; i < BTN_COUNT; ++i) {
        lua_pushinteger(L, (lua_Integer)BTN[i].bit);
        lua_setfield(L, -2, BTN[i].name);
    }
    return 1;
}

/* --- drawing ------------------------------------------------------------- */
//
// Virtual 1280x720, the overlay's own space -- correct at any window size and
// on web, where SDL reports a stale canvas size. See sdk/gx_ogl.h.

static int draw_guard(lua_State *L, const char *what) {
    if (!rlua_in_draw())
        return luaL_error(L, "draw.%s: only valid inside robox.on(\"draw\", ...)",
                          what);
    return 0;
}

static int l_draw_rect(lua_State *L) {
    draw_guard(L, "rect");
    gx_ogl_overlay_rect((float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2),
                        (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4),
                        (float)luaL_optnumber(L, 5, 1.0), (float)luaL_optnumber(L, 6, 1.0),
                        (float)luaL_optnumber(L, 7, 1.0), (float)luaL_optnumber(L, 8, 1.0));
    return 0;
}

static int l_draw_outline(lua_State *L) {
    draw_guard(L, "outline");
    gx_ogl_overlay_outline((float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2),
                           (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4),
                           (float)luaL_optnumber(L, 5, 2.0),
                           (float)luaL_optnumber(L, 6, 1.0), (float)luaL_optnumber(L, 7, 1.0),
                           (float)luaL_optnumber(L, 8, 1.0), (float)luaL_optnumber(L, 9, 1.0));
    return 0;
}

static int l_draw_text(lua_State *L) {
    draw_guard(L, "text");
    gx_ogl_overlay_text((float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2),
                        luaL_checkstring(L, 3),
                        (float)luaL_optnumber(L, 4, 1.0), (float)luaL_optnumber(L, 5, 1.0),
                        (float)luaL_optnumber(L, 6, 1.0), (float)luaL_optnumber(L, 7, 1.0),
                        (float)luaL_optnumber(L, 8, 0.6), (float)luaL_optnumber(L, 9, 0.0));
    return 0;
}

/* tri(x0,y0, x1,y1, x2,y2, r,g,b,a [, r1,g1,b1,a1, r2,g2,b2,a2])
 * One colour for a flat triangle, three for a gradient. Everything the rect
 * API cannot express is built from this in script -- see robox.draw notes in
 * MODDING.md. */
static int l_draw_tri(lua_State *L) {
    draw_guard(L, "tri");
    const float x0 = (float)luaL_checknumber(L, 1), y0 = (float)luaL_checknumber(L, 2);
    const float x1 = (float)luaL_checknumber(L, 3), y1 = (float)luaL_checknumber(L, 4);
    const float x2 = (float)luaL_checknumber(L, 5), y2 = (float)luaL_checknumber(L, 6);
    const float r0 = (float)luaL_optnumber(L, 7,  1.0), g0 = (float)luaL_optnumber(L, 8,  1.0);
    const float b0 = (float)luaL_optnumber(L, 9,  1.0), a0 = (float)luaL_optnumber(L, 10, 1.0);
    /* Corners 1 and 2 default to corner 0, so the common flat case is short. */
    const float r1 = (float)luaL_optnumber(L, 11, r0), g1 = (float)luaL_optnumber(L, 12, g0);
    const float b1 = (float)luaL_optnumber(L, 13, b0), a1 = (float)luaL_optnumber(L, 14, a0);
    const float r2 = (float)luaL_optnumber(L, 15, r1), g2 = (float)luaL_optnumber(L, 16, g1);
    const float b2 = (float)luaL_optnumber(L, 17, b1), a2 = (float)luaL_optnumber(L, 18, a1);
    gx_ogl_overlay_tri(x0, y0, r0, g0, b0, a0,
                       x1, y1, r1, g1, b1, a1,
                       x2, y2, r2, g2, b2, a2);
    return 0;
}

/* load(path) -> handle. One of the game's own .tpl textures, path as it
 * appears in the game data ("media/gui/botones/cr_normal.tpl"). Call it once
 * from a "start" handler, not per frame -- it decodes and uploads. Returns nil
 * if the file is missing, so a mod can fall back instead of dying. */
static int l_draw_load(lua_State *L) {
    const int h = gx_ogl_overlay_load_tpl(luaL_checkstring(L, 1));
    if (h < 0) return 0;
    lua_pushinteger(L, h);
    return 1;
}

/* image(handle, x, y, w, h [, r, g, b, a]) -- tinted, drawn over the rects
 * and text of the same frame. */
static int l_draw_image(lua_State *L) {
    draw_guard(L, "image");
    gx_ogl_overlay_image((int)luaL_checkinteger(L, 1),
                         (float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3),
                         (float)luaL_checknumber(L, 4), (float)luaL_checknumber(L, 5),
                         (float)luaL_optnumber(L, 6, 1.0), (float)luaL_optnumber(L, 7, 1.0),
                         (float)luaL_optnumber(L, 8, 1.0), (float)luaL_optnumber(L, 9, 1.0));
    return 0;
}

static int l_draw_text_width(lua_State *L) {
    lua_pushnumber(L, gx_ogl_overlay_text_width(luaL_checkstring(L, 1),
                                                (float)luaL_optnumber(L, 2, 0.6),
                                                (float)luaL_optnumber(L, 3, 0.0)));
    return 1;
}

/* --- level, audio, misc -------------------------------------------------- */

static int l_level_id      (lua_State *L) { lua_pushinteger(L, robox_level_id()); return 1; }
static int l_level_name    (lua_State *L) { lua_pushstring (L, robox_level_name()); return 1; }
static int l_level_is_robot(lua_State *L) { lua_pushboolean(L, robox_level_is_robot()); return 1; }

/* Ask the game to change level: func_80064954(level_id, param).
 *
 * Two instructions -- `stw r3,-0x7ea0(r13); stw r4,-0x46ac(r13); blr` -- and
 * that is the whole thing. It does not perform the transition, it REQUESTS
 * one: the engine's own update reads that slot every frame, and when it is not
 * the 0xff "nothing pending" sentinel it runs the change and writes 0xff back.
 *
 * Which is why this and not func_80060bf0, the function that does the actual
 * work. Calling that directly loads the level file and moves the current-level
 * word, and in gameplay that is enough -- but from the main menu it changed
 * the level underneath a state machine that was still running the menu, so the
 * game stayed exactly where it was. Going through the request lets the engine
 * do the transition when IT is ready, out of whatever state it is in, running
 * the same code path a door or a level-end does.
 *
 * Found by walking back from the current-level word at 0x801F7538 (r13-0x7f28)
 * to the two functions that store to it, then to the pending slot the second
 * of them reads, then to the only function that writes that. */
#define LEVEL_REQUEST_VA 0x80064954u
#define GAMESTATE_GET_VA 0x800ddabcu   /* the game-state singleton  */
#define GAMESTATE_SET_VA 0x800ddd34u   /* singleton->0x1c = mode    */

/* ...except the request alone does nothing, and the reason is the second half
 * of this. The consumer gates on the singleton's mode field:
 *
 *     80063dcc  lwz  r0,-0x7ea0(r13)   ; pending level
 *     80063dd0  cmpwi r0,0xff          ; 0xff = nothing pending
 *     80063dd4  beq  skip
 *     80063dd8  bl   0x800ddabc
 *     80063ddc  lwz  r0,0x1c(r3)
 *     80063de0  cmpwi r0,0x2
 *     80063de4  beq  skip              ; mode 2 -> no transitions AT ALL
 *
 * Mode 2 is "not in gameplay" -- the main menu. So a request made from the
 * menu was being thrown away every frame while the menu carried on, which is
 * exactly the symptom.
 *
 * The fix is to do what the game does. func_800c91d4 is its play / load-save
 * path, and it is three calls: request the level from the save
 * ([0x800e46dc()+0xa30] is where the saved level id lives), then clear the
 * mode, which is what re-arms the state machine:
 *
 *     800c93ac  bl 0x80064954    ; request, param -2 or 0
 *     800c93c4  bl 0x800ddabc
 *     800c93cc  bl 0x800ddd34    ; mode = 0
 *
 * Mode 1 rather than 0 for ids 0xa0..0xb3: the transition code itself picks
 * between them on exactly that range (80063e94), because world 4 is the robot
 * interiors and they load a different tileset.
 *
 * Doing it this way means the engine performs the transition itself, on its
 * own fade, from whatever state it is in -- the same path a door takes. */

/* Every level the game knows, as { {id=, name=}, ... } in id order. The table
 * is video.c's, transcribed from the engine's own jump table at 0x801CF028,
 * so this is the real list and not a directory scan of Assets/lev. */
static int l_level_list(lua_State *L) {
    extern int robox_level_id_first(void);
    extern int robox_level_id_last(void);
    extern const char *robox_level_name_of(int id);

    lua_newtable(L);
    int n = 0;
    for (int id = robox_level_id_first(); id <= robox_level_id_last(); ++id) {
        const char *name = robox_level_name_of(id);
        if (!name) continue;                 /* reserved id, no level */
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, id);   lua_setfield(L, -2, "id");
        lua_pushstring(L, name);  lua_setfield(L, -2, "name");
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

/* Call a guest function with two integer arguments and hand back r3. The Lua
 * marshalling in guest_call is not wanted here -- these are internal calls
 * whose result feeds the next one. */
static uint32_t guest_call2(uint32_t va, uint32_t a0, uint32_t a1) {
    PPC_Func fn = ppc_lookup_func(va);
    if (!fn) return 0;
    const PPCContext saved = g_cpu;
    g_cpu.gpr[3] = a0;
    g_cpu.gpr[4] = a1;
    g_cpu.gpr[1] = (saved.gpr[1] - CALL_SCRATCH) & ~0xfu;
    g_cpu.lr     = 0;
    fn();
    const uint32_t r3 = g_cpu.gpr[3];
    g_cpu = saved;
    return r3;
}

static int l_level_load(lua_State *L) {
    const lua_Integer id  = luaL_checkinteger(L, 1);
    const lua_Integer arg = luaL_optinteger(L, 2, 0);
    if (rlua_in_draw())
        return luaL_error(L, "level.load: not from a draw handler -- use \"frame\"");
    if (!ppc_lookup_func(LEVEL_REQUEST_VA) || !ppc_lookup_func(GAMESTATE_SET_VA))
        return luaL_error(L, "level.load: the engine entry points are not in "
                             "the dispatch table");

    guest_call2(LEVEL_REQUEST_VA, (uint32_t)(int64_t)id, (uint32_t)(int64_t)arg);

    const uint32_t st   = guest_call2(GAMESTATE_GET_VA, 0, 0);
    const uint32_t mode = (id >= 0xa0 && id <= 0xb3) ? 1u : 0u;
    guest_call2(GAMESTATE_SET_VA, st, mode);

    lua_pushboolean(L, 1);
    return 1;
}

static int l_audio_volume(lua_State *L) {
    lua_pushinteger(L, g_audio_volume);
    if (!lua_isnoneornil(L, 1)) {
        int v = (int)luaL_checkinteger(L, 1);
        g_audio_volume = v < 0 ? 0 : v > 100 ? 100 : v;
    }
    return 1;
}

static int l_audio_music(lua_State *L) {
    lua_pushboolean(L, g_music_on);
    if (!lua_isnoneornil(L, 1)) g_music_on = lua_toboolean(L, 1);
    return 1;
}

/* Live off the mixer (sdk/peripherals.c), so visuals can follow whatever is
 * actually playing -- the game's own music, a music-pack track, or a sound
 * effect -- rather than a tempo written into the mod and slowly drifting. */
static int l_audio_level(lua_State *L) {
    extern float robox_audio_level(void);
    lua_pushnumber(L, (lua_Number)robox_audio_level());
    return 1;
}
static int l_audio_beat(lua_State *L) {
    extern float robox_audio_beat(void);
    lua_pushnumber(L, (lua_Number)robox_audio_beat());
    return 1;
}

/* --- spectrum ------------------------------------------------------------
 *
 * A real analyser, not a decorative wiggle: Hann-windowed 1024-point FFT over
 * the newest audio, folded into log-spaced bands. Log spacing because linear
 * bins put nine tenths of the display above 5 kHz, where there is nothing to
 * see -- an octave is an octave whether it is 80 Hz or 8 kHz, and the ear
 * agrees. Magnitudes go to dB for the same reason.
 *
 * Cost is one FFT per frame at most (cached on the frame counter, since a mod
 * will call this from both "frame" and "draw"), which is microseconds.
 */
#define FFT_N 1024

static void fft_1024(float *re, float *im) {
    /* Decimation in time: bit-reverse, then log2(N) butterfly passes. */
    for (int i = 1, j = 0; i < FFT_N; ++i) {
        int bit = FFT_N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t;
            t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= FFT_N; len <<= 1) {
        const float ang = -6.28318530717958647692f / (float)len;
        const float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < FFT_N; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; ++k) {
                const int a = i + k, b = a + len / 2;
                const float vr = re[b] * cr - im[b] * ci;
                const float vi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - vr; im[b] = im[a] - vi;
                re[a] += vr;        im[a] += vi;
                /* Twiddle by recurrence. It drifts a little over the longest
                 * pass; for a visualiser that is far below what anyone can
                 * see, and it keeps a sin/cos out of the inner loop. */
                const float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

#define SPEC_MAX_BANDS 64
static float    g_spec[SPEC_MAX_BANDS];      /* smoothed, 0..1 */
static int      g_spec_bands;
static uint64_t g_spec_frame = (uint64_t)-1;

static void spectrum_update(int bands) {
    extern int robox_audio_capture(float *out, int n, int *rate_out);

    static float re[FFT_N], im[FFT_N], win[FFT_N];
    static int   win_ready;
    if (!win_ready) {
        win_ready = 1;
        for (int i = 0; i < FFT_N; ++i)
            win[i] = 0.5f * (1.0f - cosf(6.28318530717958647692f * i / (FFT_N - 1)));
    }

    int rate = 32000;
    const int got = robox_audio_capture(re, FFT_N, &rate);
    for (int i = got; i < FFT_N; ++i) re[i] = 0.0f;
    for (int i = 0; i < FFT_N; ++i) { re[i] *= win[i]; im[i] = 0.0f; }

    fft_1024(re, im);

    const float nyquist = (float)rate * 0.5f;
    const float f_lo = 45.0f;
    const float f_hi = nyquist < 16000.0f ? nyquist : 16000.0f;
    const float ratio = f_hi / f_lo;

    /* Scale bins to dBFS. A Hann window has a coherent gain of 0.5, so a
     * full-scale sine lands in one bin at about N/4 -- without dividing that
     * out, every band reads +48 dB before the mapping even starts and the
     * whole display pins to the ceiling regardless of the volume. */
    const float FULL_SCALE = (float)(FFT_N / 4);

    float db[SPEC_MAX_BANDS];
    float loudest = -200.0f;

    for (int b = 0; b < bands; ++b) {
        const float a = powf(ratio, (float)b / (float)bands) * f_lo;
        const float z = powf(ratio, (float)(b + 1) / (float)bands) * f_lo;
        int i0 = (int)(a * FFT_N / (float)rate);
        int i1 = (int)(z * FFT_N / (float)rate);
        if (i0 < 1) i0 = 1;
        if (i1 <= i0) i1 = i0 + 1;
        if (i1 > FFT_N / 2) i1 = FFT_N / 2;

        float peak = 0.0f;
        for (int i = i0; i < i1; ++i) {
            const float m = re[i] * re[i] + im[i] * im[i];
            if (m > peak) peak = m;
        }

        /* Pink tilt: music falls off roughly 3 dB per octave, so without this
         * the bass bands tower over everything and the top half of the
         * display never moves. Every hardware analyser does the same. */
        const float octaves = log2f((a + 1.0f) / f_lo);
        db[b] = 20.0f * log10f(sqrtf(peak) / FULL_SCALE + 1e-9f) + 3.0f * octaves;
        if (db[b] > loudest) loudest = db[b];
    }

    /* Auto-gain, so the display fills whatever the volume happens to be --
     * the game's mixer, the music-pack volume and the settings-menu slider all
     * multiply into this and none of them should mean a dead analyser.
     *
     * Rises quickly so a loud passage does not clip for a second, falls slowly
     * so a quiet bar does not get winched up into noise, and stops falling at
     * a floor so silence stays silent instead of amplifying the dither. */
    static float ref_db = -25.0f;
    ref_db += (loudest - ref_db) * (loudest > ref_db ? 0.25f : 0.010f);
    if (ref_db < -55.0f) ref_db = -55.0f;

    const float top    = ref_db + 4.0f;      /* headroom, so bars rarely pin */
    const float range  = 42.0f;              /* dB from empty to full        */

    for (int b = 0; b < bands; ++b) {
        float v = (db[b] - (top - range)) / range;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        /* Snap up, ease down: how every analyser that reads well behaves. */
        g_spec[b] = v > g_spec[b] ? v : g_spec[b] + (v - g_spec[b]) * 0.28f;
    }
    g_spec_bands = bands;
}

static int l_audio_spectrum(lua_State *L) {
    int bands = (int)luaL_optinteger(L, 1, 24);
    if (bands < 1) bands = 1;
    if (bands > SPEC_MAX_BANDS) bands = SPEC_MAX_BANDS;

    if (g_spec_frame != g_rlua_frame || bands != g_spec_bands) {
        g_spec_frame = g_rlua_frame;
        spectrum_update(bands);
    }
    lua_createtable(L, bands, 0);
    for (int b = 0; b < bands; ++b) {
        lua_pushnumber(L, (lua_Number)g_spec[b]);
        lua_rawseti(L, -2, b + 1);
    }
    return 1;
}

static int l_mod_enabled(lua_State *L) {
    lua_pushboolean(L, robox_mod_enabled(luaL_checkstring(L, 1)));
    return 1;
}

/* How much geometry the game submitted last frame. The one honest answer to
 * "is anything actually being rendered right now" -- which a mod that moves
 * the player or the camera somewhere unusual needs, because a black screen
 * and a correctly-drawn black scene look identical. */
static int l_video_draws(lua_State *L) {
    extern uint32_t g_frame_draw_calls_last;   /* sdk/gx_ogl.c */
    lua_pushinteger(L, (lua_Integer)g_frame_draw_calls_last);
    return 1;
}

/* video.clear(r, g, b) forces the EFB clear colour; video.clear() hands it
 * back to the game. This is the only way to change what shows where the game
 * draws nothing -- see gx_ogl_set_clear_override. */
static int l_video_clear(lua_State *L) {
    if (lua_isnoneornil(L, 1)) { gx_ogl_set_clear_override(-1); return 0; }
    double r = luaL_checknumber(L, 1);
    double g = luaL_checknumber(L, 2);
    double b = luaL_checknumber(L, 3);
    #define C8(v) ((uint32_t)((v) <= 0 ? 0 : (v) >= 1 ? 255 : (v) * 255.0 + 0.5))
    gx_ogl_set_clear_override((int64_t)(0xff000000u | (C8(r) << 16) |
                                        (C8(g) << 8) | C8(b)));
    #undef C8
    return 0;
}

static int l_time (lua_State *L) { lua_pushnumber (L, g_rlua_time);  return 1; }
static int l_frame(lua_State *L) { lua_pushinteger(L, (lua_Integer)g_rlua_frame); return 1; }

/* --- logging and toasts -------------------------------------------------- */

static void concat_args(lua_State *L, char *out, size_t cap) {
    const int n = lua_gettop(L);
    size_t used = 0;
    out[0] = 0;
    for (int i = 1; i <= n && used + 1 < cap; ++i) {
        const char *s;
        if (lua_type(L, i) == LUA_TSTRING || lua_isnumber(L, i)) {
            s = lua_tostring(L, i);
        } else {
            luaL_tolstring(L, i, NULL);         /* honours __tostring */
            s = lua_tostring(L, -1);
        }
        int w = snprintf(out + used, cap - used, "%s%s", i > 1 ? "  " : "", s ? s : "?");
        if (lua_type(L, i) != LUA_TSTRING && !lua_isnumber(L, i)) lua_pop(L, 1);
        if (w < 0) break;
        used += (size_t)w;
    }
}

static int l_log(lua_State *L) {
    char buf[1024];
    concat_args(L, buf, sizeof buf);
    rlua_mod_t *m = rlua_current();
    fprintf(stderr, "[lua:%s] %s\n", m ? m->id : "?", buf);
    fflush(stderr);
    return 0;
}

static int l_notify(lua_State *L) {
    char buf[192];
    const char *s = luaL_checkstring(L, 1);
    snprintf(buf, sizeof buf, "%s", s);
    rlua_toast(buf, (float)luaL_optnumber(L, 2, 3.0), 0);
    return 0;
}

/* --- per-mod config ------------------------------------------------------ */
//
// Flat "key = value" next to the mod, because a reload throws every Lua value
// away and a mod that wants a setting to survive one needs somewhere outside
// the VM to put it. Deliberately not a general serialiser: strings, numbers
// and booleans cover settings, and anything richer belongs in the mod's own
// file format.

static void config_path(char *out, size_t cap) {
    rlua_mod_t *m = rlua_current();
    if (!m) { snprintf(out, cap, "mods/lua/unknown.cfg"); return; }
    /* Folder mods keep it inside; single-file mods get <id>.cfg beside them. */
    if (strcmp(m->dir, "mods/lua") != 0) snprintf(out, cap, "%s/config.cfg", m->dir);
    else                                 snprintf(out, cap, "mods/lua/%s.cfg", m->id);
}

static int l_config_load(lua_State *L) {
    char path[600];
    config_path(path, sizeof path);
    lua_newtable(L);

    FILE *f = fopen(path, "r");
    if (!f) return 1;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k && isspace((unsigned char)*k)) ++k;
        for (char *e = k + strlen(k); e > k && isspace((unsigned char)e[-1]); ) *--e = 0;
        while (*v && isspace((unsigned char)*v)) ++v;
        for (char *e = v + strlen(v); e > v && isspace((unsigned char)e[-1]); ) *--e = 0;
        if (!*k) continue;

        if (!strcmp(v, "true"))       lua_pushboolean(L, 1);
        else if (!strcmp(v, "false")) lua_pushboolean(L, 0);
        else {
            char *end = NULL;
            double d = strtod(v, &end);
            if (end && *end == 0 && end != v) lua_pushnumber(L, d);
            else                              lua_pushstring(L, v);
        }
        lua_setfield(L, -2, k);
    }
    fclose(f);
    return 1;
}

static int l_config_save(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    char path[600];
    config_path(path, sizeof path);

    FILE *f = fopen(path, "w");
    if (!f) return luaL_error(L, "config.save: cannot write %s", path);
    rlua_mod_t *m = rlua_current();
    fprintf(f, "# %s -- written by robox.config.save()\n", m ? m->id : "mod");

    lua_pushnil(L);
    while (lua_next(L, 1)) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            const char *k = lua_tostring(L, -2);
            switch (lua_type(L, -1)) {
                case LUA_TBOOLEAN: fprintf(f, "%s = %s\n", k, lua_toboolean(L, -1) ? "true" : "false"); break;
                case LUA_TNUMBER:  fprintf(f, "%s = %.14g\n", k, (double)lua_tonumber(L, -1)); break;
                case LUA_TSTRING:  fprintf(f, "%s = %s\n", k, lua_tostring(L, -1)); break;
                default: break;    /* tables and functions are not settings */
            }
        }
        lua_pop(L, 1);
    }
    fclose(f);
    lua_pushboolean(L, 1);
    return 1;
}

/* --- events -------------------------------------------------------------- */

static int l_on(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    const int ev = rlua_event_from_name(name);
    if (ev < 0) return luaL_error(L, "on: no event called '%s'", name);
    rlua_mod_t *m = rlua_current();
    if (!m) return luaL_error(L, "on: no mod context");

    if (m->cb_ref[ev] == LUA_NOREF) {
        lua_newtable(L);
        m->cb_ref[ev] = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, m->cb_ref[ev]);
    const lua_Integer n = (lua_Integer)lua_rawlen(L, -1);
    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, n + 1);
    lua_pop(L, 1);
    return 0;
}

static int l_watch(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    rlua_mod_t *m = rlua_current();
    if (!m) return 0;
    if (m->watch_count >= RLUA_MAX_WATCH)
        return luaL_error(L, "watch: already watching %d files", RLUA_MAX_WATCH);
    snprintf(m->watch[m->watch_count], sizeof m->watch[0], "%s", path);
    m->watch_mtime[m->watch_count] = -1;      /* forces one reload, then settles */
    ++m->watch_count;
    return 0;
}

/* --- unload -------------------------------------------------------------- */

void rlua_api_unload_mod(int i) {
    /* Unconditionally, not just for the mod that set it: a reload must never
     * be able to leave the player unable to move with no mod left running to
     * undo it. Anything that still wants the block re-asserts it next frame. */
    g_input_block = 0;
    /* Same for the keyboard, and it matters more -- a console reloaded while
     * open would otherwise leave every key going to a mod that is gone. */
    rlua_capture_release();

    for (int h = 0; h < RLUA_MAX_HOOKS; ++h)
        if (g_hooks[h].used && g_hooks[h].mod == i) hook_drop(h);
    for (int b = 0; b < 32; ++b)
        if (g_inject_frames[b] > 0 && g_inject_owner[b] == i) {
            g_inject_frames[b] = 0;
            g_inject_mask &= ~(1u << b);
        }
}

/* --- table construction -------------------------------------------------- */

static void set_fn(lua_State *L, const char *name, lua_CFunction fn) {
    lua_pushcfunction(L, fn);
    lua_setfield(L, -2, name);
}

static void sub_table(lua_State *L, const char *name, const luaL_Reg *fns) {
    lua_newtable(L);
    for (const luaL_Reg *r = fns; r->name; ++r) set_fn(L, r->name, r->func);
    lua_setfield(L, -2, name);
}

static const luaL_Reg MEM_FNS[] = {
    { "u8", l_mem_u8 }, { "u16", l_mem_u16 }, { "u32", l_mem_u32 },
    { "i8", l_mem_i8 }, { "i16", l_mem_i16 }, { "i32", l_mem_i32 },
    { "f32", l_mem_f32 }, { "f64", l_mem_f64 },
    { "write_u8", l_mem_write_u8 }, { "write_u16", l_mem_write_u16 },
    { "write_u32", l_mem_write_u32 },
    { "write_f32", l_mem_write_f32 }, { "write_f64", l_mem_write_f64 },
    { "read", l_mem_read }, { "write", l_mem_write }, { "cstr", l_mem_cstr },
    { "follow", l_mem_follow }, { "find", l_mem_find }, { "valid", l_mem_valid },
    { NULL, NULL }
};

static const luaL_Reg CPU_FNS[] = {
    { "gpr", l_cpu_gpr }, { "fpr", l_cpu_fpr },
    { "lr", l_cpu_lr }, { "ctr", l_cpu_ctr },
    { NULL, NULL }
};

static const luaL_Reg PLAYER_FNS[] = {
    { "addr", l_player_addr }, { "get", l_player_get },
    { "set", l_player_set },   { "fields", l_player_fields },
    { NULL, NULL }
};

static const luaL_Reg INPUT_FNS[] = {
    { "held", l_input_held }, { "pressed", l_input_pressed },
    { "released", l_input_released }, { "mask", l_input_mask },
    { "press", l_input_press }, { "release", l_input_release },
    { "block", l_input_block }, { "capture", l_input_capture },
    { "key", l_input_key }, { "stick", l_input_stick },
    { "mouse", l_input_mouse },
    { "buttons", l_input_buttons },
    { NULL, NULL }
};

static const luaL_Reg DRAW_FNS[] = {
    { "rect", l_draw_rect }, { "outline", l_draw_outline },
    { "text", l_draw_text }, { "text_width", l_draw_text_width },
    { "tri", l_draw_tri },
    { "load", l_draw_load }, { "image", l_draw_image },
    { NULL, NULL }
};

static const luaL_Reg LEVEL_FNS[] = {
    { "id", l_level_id }, { "name", l_level_name },
    { "is_robot", l_level_is_robot }, { "load", l_level_load },
    { "list", l_level_list },
    { NULL, NULL }
};

/* music_set(song, path) -- point one of the game's songs at a file, now.
 * mods/wav_music.cfg is read once at startup, so a mod that produced its audio
 * later (a download) needs this to be heard. path nil hands the song back. */
static int l_audio_music_set(lua_State *L) {
    extern int robox_wav_set_mapping(const char *song, const char *path);
    const char *song = luaL_checkstring(L, 1);
    const char *path = lua_isnoneornil(L, 2) ? NULL : luaL_checkstring(L, 2);
    lua_pushboolean(L, robox_wav_set_mapping(song, path));
    return 1;
}

/* music_play(path [, loop]) -- put a file on NOW, without waiting for the game
 * to start a song. True if it opened (WAV/OGG/RBXS, same as a music pack).
 *
 * loop nil/false means play once and stop, and that stop is what makes a
 * PLAYLIST possible: music_playing() goes nil at the end of the track and the
 * mod puts the next one on. While a mod's track runs, the game's own music is
 * muted rather than stopped, so it picks up again by itself afterwards. */
static int l_audio_music_play(lua_State *L) {
    extern int robox_wav_play_file(const char *path, int loop);
    const char *path = luaL_checkstring(L, 1);
    lua_pushboolean(L, robox_wav_play_file(path, lua_toboolean(L, 2)));
    return 1;
}

/* music_playing() -> nil, or name, is_mine. `name` is the song for a mapped
 * game song and the file's basename for a mod's own track; `is_mine` is true
 * only for the latter -- which is how a playlist tells "my track is still
 * running" from "the game started a song over the top of me". */
static int l_audio_music_playing(lua_State *L) {
    extern const char *robox_wav_current(void);
    extern int robox_wav_is_direct(void);
    const char *cur = robox_wav_current();
    if (!cur) return 0;
    lua_pushstring(L, cur);
    lua_pushboolean(L, robox_wav_is_direct());
    return 2;
}

/* music_stop() -- stop the replacement stream, whoever started it. The game's
 * own music is audible again on the next frame; a mapped song is not restarted
 * until the game starts one. */
static int l_audio_music_stop(lua_State *L) {
    extern void robox_wav_stop(void);
    (void)L;
    robox_wav_stop();
    return 0;
}

/* --- net ----------------------------------------------------------------- */

static int l_net_fetch(lua_State *L) {
    extern int robox_net_fetch(const char *url, const char *dest);
    const int h = robox_net_fetch(luaL_checkstring(L, 1), luaL_checkstring(L, 2));
    if (h < 0) return 0;
    lua_pushinteger(L, h);
    return 1;
}

/* status(handle) -> "working"|"done"|"error", bytes_got, bytes_total, message */
static int l_net_status(lua_State *L) {
    extern int robox_net_status(int handle, uint64_t *got, uint64_t *total);
    extern const char *robox_net_error(int handle);
    const int h = (int)luaL_checkinteger(L, 1);
    uint64_t got = 0, total = 0;
    const int st = robox_net_status(h, &got, &total);
    lua_pushstring(L, st == 0 ? "working" : st == 1 ? "done" : "error");
    lua_pushnumber(L, (lua_Number)got);
    lua_pushnumber(L, (lua_Number)total);
    lua_pushstring(L, robox_net_error(h));
    return 4;
}

static int l_net_release(lua_State *L) {
    extern void robox_net_release(int handle);
    robox_net_release((int)luaL_checkinteger(L, 1));
    return 0;
}

static const luaL_Reg NET_FNS[] = {
    { "fetch", l_net_fetch }, { "status", l_net_status },
    { "release", l_net_release },
    { NULL, NULL }
};

static const luaL_Reg AUDIO_FNS[] = {
    { "volume", l_audio_volume }, { "music", l_audio_music },
    { "level", l_audio_level },   { "beat", l_audio_beat },
    { "spectrum", l_audio_spectrum }, { "music_set", l_audio_music_set },
    { "music_play", l_audio_music_play },
    { "music_playing", l_audio_music_playing },
    { "music_stop", l_audio_music_stop },
    { NULL, NULL }
};

static const luaL_Reg VIDEO_FNS[] = {
    { "draws", l_video_draws }, { "clear", l_video_clear },
    { NULL, NULL }
};

static const luaL_Reg CONFIG_FNS[] = {
    { "load", l_config_load }, { "save", l_config_save },
    { NULL, NULL }
};

void rlua_open_api(lua_State *L, int mod_index) {
    /* Called with the mod's env table on top; leaves it there. */
    rlua_mod_t *m = &g_rlua_mods[mod_index];

    lua_newtable(L);                    /* robox */

    set_fn(L, "on",      l_on);
    set_fn(L, "watch",   l_watch);
    set_fn(L, "log",     l_log);
    set_fn(L, "notify",  l_notify);
    set_fn(L, "call",    l_call);
    set_fn(L, "call_ex", l_call_ex);
    set_fn(L, "hook",        l_hook);
    set_fn(L, "hook_before", l_hook_before);
    set_fn(L, "hook_after",  l_hook_after);
    set_fn(L, "unhook",   l_unhook);
    set_fn(L, "original", l_original);
    set_fn(L, "enabled",  l_mod_enabled);
    set_fn(L, "time",     l_time);
    set_fn(L, "frame",    l_frame);

    sub_table(L, "mem",    MEM_FNS);
    sub_table(L, "cpu",    CPU_FNS);
    sub_table(L, "player", PLAYER_FNS);
    sub_table(L, "input",  INPUT_FNS);
    sub_table(L, "draw",   DRAW_FNS);
    sub_table(L, "level",  LEVEL_FNS);
    sub_table(L, "audio",  AUDIO_FNS);
    sub_table(L, "video",  VIDEO_FNS);
    sub_table(L, "net",    NET_FNS);
    sub_table(L, "config", CONFIG_FNS);

    rlua_open_game_api(L);              /* robox.game -- the guest VM bridge */

    lua_pushstring(L, m->id);   lua_setfield(L, -2, "name");
    lua_pushstring(L, m->dir);  lua_setfield(L, -2, "dir");
    lua_pushstring(L, m->main); lua_setfield(L, -2, "file");

    /* The overlay's own space, so a mod never has to guess the resolution. */
    lua_getfield(L, -1, "draw");
    lua_pushnumber(L, 1280.0); lua_setfield(L, -2, "W");
    lua_pushnumber(L,  720.0); lua_setfield(L, -2, "H");
    lua_pop(L, 1);

    lua_setfield(L, -2, "robox");        /* env.robox = robox */

    /* print() should reach the log rather than a null stdout. */
    lua_pushcfunction(L, l_log);
    lua_setfield(L, -2, "print");
}

#endif  /* !__3DS__ */
