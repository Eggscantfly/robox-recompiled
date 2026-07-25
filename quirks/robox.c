// quirks/robox.c -- Robox (WiiWare) per-game profile + link shims.
//
// Robox needs no low-memory pokes, no safe vtables, and (so far) no worker
// threads: the game is effectively single-threaded, driven by VI retrace +
// AX frame callbacks. The profile exists as the hook point for whatever
// bring-up reveals.
//
// The second half of this file provides no-op definitions for symbols that
// sdk/ and src/runtime.c reference but whose real implementations were
// RGH-specific quirks (removed from this port):
//   * lyn_* hooks called from peripherals.c / gx_ogl.c
//   * bink_hle_* probes (Robox has no Bink video)
//   * __real_func_* IOS-async wrap fallbacks (RGH --wrap flags not used here)
//   * quirks_unknown_indirect (runtime.c indirect-call escape hatch)

#include "quirks.h"

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

const quirks_t quirks_robox = {
    .name        = "robox",
    .description = "Robox (WiiWare) profile",
    .worker_entries           = NULL,  .worker_entry_count        = 0,
    .safe_vtable_targets      = NULL,  .safe_vtable_target_count  = 0,
    .lowmem_pokes             = NULL,  .lowmem_poke_count         = 0,
    .osinit_post_hook         = NULL,
};

// ---------------------------------------------------------------------------
// Link shims for removed RGH quirk hooks
// ---------------------------------------------------------------------------

// peripherals.c calls this every frame to drain RGH's host-side OSAlarm
// mirror. Robox binds all OSSetAlarm flavors to nops, so nothing to pump.
void lyn_alarm_pump(void) {}

// gx_ogl.c texture debug hooks (RGH texture-list dump / sibling heal).
void lyn_texlist_dump_check(void) {}
void lyn_tex_sibling_heal(void) {}

// RGH main.c called this to patch func-table entries; kept for ABI parity.
void lyn_quirk_patch_funcs(void) {}

// runtime.c: last-chance handler for indirect calls that miss the function
// table. Return 0 = not handled (runtime prints its diagnostic).
int quirks_unknown_indirect(uint32_t addr) { (void)addr; return 0; }

// os.c's RECOMP_LOADCLR experiment reads RGH's WOR manager pointer; keep it
// zero so the (env-gated, default-off) path never fires for Robox.
uint32_t g_wor_mgr = 0;

// TEMP(bring-up): ring buffer of recent game-malloc sizes; dumped from
// hle_exit's lua-panic path via robox_malloc_dump().
static struct { unsigned size, lr; } g_mal_ring[32];
static unsigned g_mal_head;
void robox_malloc_probe(unsigned size, unsigned lr) {
    g_mal_ring[g_mal_head++ & 31].size = size;
    g_mal_ring[(g_mal_head-1) & 31].lr = lr;
    if (size > 0x01000000) {
        fprintf(stderr, "[malloc] HUGE request 0x%x from lr=0x%08x\n", size, lr);
    }
}
void robox_malloc_dump(void) {
    fprintf(stderr, "[malloc] last requests (newest last):");
    for (unsigned i = 0; i < 32; ++i) {
        unsigned idx = (g_mal_head + i) & 31;
        if (g_mal_ring[idx].size)
            fprintf(stderr, " 0x%x@%08x", g_mal_ring[idx].size, g_mal_ring[idx].lr);
    }
    fprintf(stderr, "\n");
}

// TEMP(bring-up): walk every CW MEM ExpHeap ("EXPH") found in MEM1/MEM2 and
// report free/used totals. Called from hle_exit's lua-panic path to answer
// "was the 16MB game pool actually exhausted, or is the free list corrupt?"
#include "../src/runtime.h"
static void expheap_walk_list(uint32_t head_va, uint16_t want_tag,
                              const char *label) {
    uint32_t total = 0, maxblk = 0, count = 0, bad = 0;
    uint32_t b = head_va;
    while (b >= 0x80003000u && (b < 0x81800000u ||
           (b >= 0x90000000u && b < 0x94000000u)) && count < 400000) {
        uint16_t tag = MEM_R16(b);
        if (tag != want_tag) { bad = b; break; }
        uint32_t sz = MEM_R32(b + 4);
        total += sz;
        if (sz > maxblk) maxblk = sz;
        ++count;
        uint32_t next = MEM_R32(b + 0xc);
        if (next == b) { bad = b; break; }
        b = next;
        if (b == 0) break;
    }
    fprintf(stderr, "    %s: blocks=%u total=0x%x largest=0x%x%s",
            label, count, total, maxblk, bad ? " BAD@" : "\n");
    if (bad) fprintf(stderr, "0x%08x tag=0x%04x\n", bad, MEM_R16(bad));
}
void robox_expheap_dump(void) {
    for (uint32_t va = 0x80004000u; va < 0x81800000u - 16; va += 4) {
        if (MEM_R32(va) == 0x45585048u) { // 'EXPH'
            uint32_t start = MEM_R32(va + 0x18), end = MEM_R32(va + 0x1c);
            fprintf(stderr, "[expheap] head=0x%08x range=0x%08x..0x%08x (0x%x bytes)\n",
                    va, start, end, end - start);
            expheap_walk_list(MEM_R32(va + 0x3c), 0x4652u, "free ('FR')");
            expheap_walk_list(MEM_R32(va + 0x44), 0x5544u, "used ('UD')");
        }
    }
    // OSAlloc heap (the one Lua's l_alloc uses via OSAllocFromHeap):
    //   SDA[-0x427c]=HeapArray  SDA[-0x4280]=NumHeaps
    //   SDA[-0x4284]=ArenaStart SDA[-0x4288]=ArenaEnd  SDA[-0x4ee8]=CurrentHeap
    {
        uint32_t r13 = g_cpu.gpr[13];
        uint32_t harr = MEM_R32(r13 - 0x427c), nheaps = MEM_R32(r13 - 0x4280);
        fprintf(stderr, "[osheap] r13=0x%08x HeapArray=0x%08x NumHeaps=%d "
                        "ArenaStart=0x%08x ArenaEnd=0x%08x CurrentHeap=%d\n",
                r13, harr, (int)nheaps,
                MEM_R32(r13 - 0x4284), MEM_R32(r13 - 0x4288),
                (int)MEM_R32(r13 - 0x4ee8));
        fprintf(stderr, "[osheap] lowmem MEM1 arena lo/hi = 0x%08x/0x%08x  "
                        "legacy 0x80000030/34 = 0x%08x/0x%08x\n",
                MEM_R32(0x80003100u), MEM_R32(0x80003104u),
                MEM_R32(0x80000030u), MEM_R32(0x80000034u));
        for (uint32_t i = 0; i < nheaps && i < 4 && harr >= 0x80003000u; ++i) {
            uint32_t hd = harr + i * 0xc;
            uint32_t hsz = MEM_R32(hd);
            fprintf(stderr, "  heap %u: size=0x%x (%s)\n", i, hsz,
                    (int32_t)hsz < 0 ? "inactive" : "active");
            if ((int32_t)hsz < 0) continue;
            static const char *ln[2] = { "free", "used" };
            for (int l = 0; l < 2; ++l) {
                uint32_t total = 0, maxblk = 0, count = 0;
                uint32_t c = MEM_R32(hd + 4 + 4u * l);
                while (c >= 0x80003000u && c < 0x94000000u && count < 200000) {
                    uint32_t sz = MEM_R32(c + 8);
                    total += sz; if (sz > maxblk) maxblk = sz;
                    ++count;
                    uint32_t next = MEM_R32(c + 4);
                    if (next == c) break;
                    c = next;
                }
                fprintf(stderr, "    %s: cells=%u total=0x%x largest=0x%x\n",
                        ln[l], count, total, maxblk);
            }
        }
    }
}

// TEMP(bring-up): script-load request probe (func_80064168 entry).
void robox_fxload_probe(uint32_t path_va, uint32_t lr) {
    const char *p = (const char *)ppc_host_ptr(path_va);
    fprintf(stderr, "[FXLOAD] '%.96s' lr=0x%08x\n", p ? p : "<null>", lr);
}

// TEMP(bring-up): luaD_throw probe. Prints the error object on the Lua
// stack top (TValue = 16 bytes, tt at +8, string payload at +0x10) so
// pre-panic errors (swallowed by pcall) are visible.
void robox_luathrow_probe(uint32_t L, uint32_t errcode, uint32_t lr) {
    static int n;
    if (++n > 100) return;
    const char *msg = "";
    if (L >= 0x80003000u && L < 0x94000000u) {
        uint32_t top = MEM_R32(L + 0x8);
        if (top > 16) {
            uint32_t value = MEM_R32(top - 16);
            uint32_t tt    = MEM_R32(top - 16 + 8);
            if (tt == 4 && value >= 0x80003000u && value < 0x94000000u)
                msg = (const char *)ppc_host_ptr(value + 0x10);
        }
    }
    fprintf(stderr, "[luaD_throw] #%d code=%d lr=0x%08x msg='%.128s'\n",
            n, (int)errcode, lr, msg ? msg : "");
}

// TEMP(bring-up): log every store into the ctor-walker frame region.
void robox_frame_watch(uint32_t va, uint32_t v, int width) {
    static int n;
    if (n < 64) {
        ++n;
        fprintf(stderr, "[FRAME-W%d] [0x%08x] = 0x%08x  lr=0x%08x sp=0x%08x\n",
                width, va, v, g_cpu.lr, g_cpu.gpr[1]);
    }
}

// TEMP(bring-up): guest stores into the MEM2 OSHeap descriptor + first cell
// (0x90100000..0x90100030). HeapDesc field writes are frequent and legit
// during alloc/free, so only zero-writes and size-field writes are printed.
void robox_heap_watch(uint32_t va, uint32_t v, int width) {
    static int n;
    if (v != 0 && va != 0x90100000u) return;
    if (++n > 300) return;
    fprintf(stderr, "[HEAP-W%d] [0x%08x] = 0x%08x  lr=0x%08x sp=0x%08x  bt:",
            width, va, v, g_cpu.lr, g_cpu.gpr[1]);
    uint32_t sp = g_cpu.gpr[1];
    for (int fi = 0; fi < 8 && sp >= 0x80003000u && sp < 0x94000000u; ++fi) {
        uint32_t up = MEM_R32(sp);
        if (up <= sp || up < 0x80003000u || up >= 0x94000000u) break;
        fprintf(stderr, " %08x", MEM_R32(up + 4));
        sp = up;
    }
    fprintf(stderr, "\n");
}

// TEMP(bring-up): recompiled GXSetBlendMode (0x80118d60) entry probe.
// r3-r6 = type/src/dst/op; __GXData ptr in SDA2[r2-0x64b0], cmode0 shadow
// word (BP reg byte in bits 31-24) at +0x220.
void robox_gxblend_probe(void) {
    static int n;
    if (n >= 16) return;
    ++n;
    uint32_t gxd = MEM_R32(g_cpu.gpr[2] - 0x64b0);
    fprintf(stderr, "[GXBLEND] type=%u src=%u dst=%u op=%u gxdata=0x%08x "
                    "shadow220=0x%08x lr=0x%08x\n",
            g_cpu.gpr[3], g_cpu.gpr[4], g_cpu.gpr[5], g_cpu.gpr[6], gxd,
            gxd >= 0x80003000u && gxd < 0x94000000u ? MEM_R32(gxd + 0x220)
                                                    : 0xdeaddeadu,
            g_cpu.lr);
}

// TEMP(menu-input hunt): reads of the input ring/mirror hold+trig words.
// Logs each distinct lr once (the set of consumers, not the traffic).
void robox_inputread_watch(uint32_t va, uint32_t v) {
    static uint32_t seen[64]; static int n;
    uint32_t lr = g_cpu.lr;
    for (int i = 0; i < n; ++i) if (seen[i] == lr) return;
    if (n >= 64) return;
    seen[n++] = lr;
    fprintf(stderr, "[INPUT-R] va=0x%08x v=0x%08x lr=0x%08x  bt:", va, v, lr);
    uint32_t sp = g_cpu.gpr[1];
    for (int fi = 0; fi < 6 && sp >= 0x80003000u && sp < 0x94000000u; ++fi) {
        uint32_t up; __builtin_memcpy(&up, ppc_host_ptr(sp), 4);
        up = _ppc_bswap32(up);
        if (up <= sp || up < 0x80003000u || up >= 0x94000000u) break;
        uint32_t l2; __builtin_memcpy(&l2, ppc_host_ptr(up + 4), 4);
        fprintf(stderr, " %08x", _ppc_bswap32(l2));
        sp = up;
    }
    fprintf(stderr, "\n");
}

// TEMP(menu-input hunt): stores into the game's per-channel wiimote connect
// state array (0x80240870..0x80240880) with backtrace.
void robox_chanstate_watch(uint32_t va, uint32_t v) {
    static int n;
    if (++n > 40) return;
    fprintf(stderr, "[CHANSTATE] [0x%08x] = 0x%08x lr=0x%08x  bt:", va, v, g_cpu.lr);
    uint32_t sp = g_cpu.gpr[1];
    for (int fi = 0; fi < 8 && sp >= 0x80003000u && sp < 0x94000000u; ++fi) {
        uint32_t up = MEM_R32(sp);
        if (up <= sp || up < 0x80003000u || up >= 0x94000000u) break;
        fprintf(stderr, " %08x", MEM_R32(up + 4));
        sp = up;
    }
    fprintf(stderr, "\n");
}

// TEMP(music hunt): voice-slot-0 field writes. Logs each distinct offset's
// first few writes so the VPB layout the SYN engine maintains is visible.
void robox_vpb_watch(uint32_t va, uint32_t v, int width) {
    static uint8_t cnt[0x200];
    static int volcnt;
    uint32_t off = va - 0x94000000u;
    if (off >= 0x200u) return;
    /* volume/delta writes: log only CHANGES so ramps are visible */
    if (off == 0x40u || off == 0x42u || off == 0x38u) {
        static uint32_t last[3] = {0xffffffffu, 0xffffffffu, 0xffffffffu};
        int idx = off == 0x40u ? 0 : off == 0x42u ? 1 : 2;
        if ((v & 0xFFFFu) == last[idx]) return;
        last[idx] = v & 0xFFFFu;
        if (volcnt < 120) { ++volcnt;
            fprintf(stderr, "[VPB-VOL] +0x%02x = 0x%04x lr=0x%08x\n",
                    off, v & 0xFFFF, g_cpu.lr);
            fflush(stderr);
        }
        return;
    }
    if (cnt[off] >= 3) return;
    ++cnt[off];
    fprintf(stderr, "[VPB] +0x%03x = 0x%0*x (w%d) lr=0x%08x\n",
            off, width * 2, v, width, g_cpu.lr);
    fflush(stderr);
}

// TEMP(music hunt): keyon/keyoff probe (func_8010ffc0: r4=midi ch, r5=note,
// r6=velocity; velocity 0 = note-off).
void robox_keyon_probe(void) {
    fprintf(stderr, "[KEY] ch=%u note=%u vel=%u lr=0x%08x\n",
            g_cpu.gpr[4], g_cpu.gpr[5], g_cpu.gpr[6], g_cpu.lr);
    fflush(stderr);
}

// TEMP(music hunt): generic tagged probe (capped at call sites).
void robox_syn_tag_probe(const char *tag) {
    uint32_t init = MEM_R32(g_cpu.gpr[13] - 0x3e00);
    uint32_t ctx  = MEM_R32(g_cpu.gpr[13] - 0x3dfc);
    uint32_t st   = (ctx >= 0x80003000u && ctx < 0x94000000u)
                        ? MEM_R32(ctx + 4) : 0xdeaddeadu;
    fprintf(stderr, "[SYN] %s r3=0x%08x r4=0x%08x lr=0x%08x init=%u ctx=0x%08x ctx.state=%u "
                    "g3e24=%u g3e28=0x%08x g3e0c=%u\n",
            tag, g_cpu.gpr[3], g_cpu.gpr[4], g_cpu.lr, init, ctx, st,
            MEM_R32(g_cpu.gpr[13] - 0x3e24), MEM_R32(g_cpu.gpr[13] - 0x3e28),
            MEM_R32(g_cpu.gpr[13] - 0x3e0c));
    fflush(stderr);
}

// TEMP(music hunt): flag set once the game starts a song, so tick-path
// probes log the interesting window instead of burning their caps at boot.
static int g_syn_started;
int robox_syn_started(void) { return g_syn_started; }

// --- Host MIDI player bridges (sdk/robox_midi.cc drives these) -------------

// MOD #1 (music): active when mods/robox.dls exists next to the exe.
// Gates: hiding robox.wt/.pcm from the game (robox_io.c) and blocking the
// game's SYN music keyon (generated hook) so only the host sampler sounds.
int robox_mod_music_active(void) {
    extern int robox_mod_enabled(const char *id);
    return robox_mod_enabled("music");
}

// Feed a raw MIDI message (note-on / CC / program / pitchbend) to the game's
// own dispatcher func_801102a0(ctx, msg_bytes). ctx = SYN context from
// SDA[r13-0x3dfc]. Called from the audio pump (same guest-callback sync
// point the AX frame callback uses, so clobbering volatile regs is fine).
void robox_guest_msg(const uint8_t *msg, unsigned len) {
    static uint32_t scratch;
    if (!scratch) {
        extern uint32_t game_heap_alloc(uint32_t size, uint32_t align);
        scratch = game_heap_alloc(16, 16);
        if (!scratch) return;
    }
    uint32_t ctx = MEM_R32(g_cpu.gpr[13] - 0x3dfc);
    if (!ctx) return;
    /* Always 3 bytes: the queue slots are fixed-size (unused data byte is
     * ignored by the dispatcher for 2-byte messages). */
    MEM_W8(scratch + 0, msg[0]);
    MEM_W8(scratch + 1, len > 1 ? msg[1] : 0);
    MEM_W8(scratch + 2, len > 2 ? msg[2] : 0);
    /* Enqueue via the engine's own producer (func_8010f870: 3 bytes at
     * ctx->writeptr[+0xd28], count[+0xd2c]++). The SYN tick drains the queue
     * itself — the exact environment the original sequencer delivered in.
     * (Calling the dispatcher 0x801102a0 directly keyed voices with wrong
     * instrument-bank state: unbased sample addresses = noise.) */
    g_cpu.gpr[3] = ctx;
    g_cpu.gpr[4] = scratch;
    ppc_call_indirect(0x8010f870u);
}

// Note-off: find the playing voice in the synth's [channel][note] table and
// hand it to the mixer's host-side release fade. Bypasses the game's keyoff
// (whose sustain/envelope path is the reason notes hung forever).
void robox_note_off(unsigned ch, unsigned note) {
    uint32_t ctx = MEM_R32(g_cpu.gpr[13] - 0x3dfc);
    if (!ctx) return;
    uint32_t slot = ctx + ((ch & 0xFFu) << 9) + ((note & 0xFFu) << 2) + 0x1134u;
    uint32_t syn = MEM_R32(slot);
    if (!syn) return;
    extern void robox_ax_note_release(uint32_t axvoice_va);
    robox_ax_note_release(MEM_R32(syn + 4));
}

// SYN song start (func_80112020) hook: hand the song to the host player and
// permanently disable the game's own sequencer advance (SDA[-0x3e24]) so
// events don't double-fire. r3=player r4=song r5=wavetable r6=pcm.
void robox_syn_start_probe(void) {
    static int logs;
    g_syn_started = 1;
    MEM_W32(g_cpu.gpr[13] - 0x3e24, 0);   /* game sequencer OFF */
    {
        extern void robox_midi_load(uint32_t song_va);
        robox_midi_load(g_cpu.gpr[4]);
    }
    if (logs >= 12) return;               /* work above runs every start */
    ++logs;
    fprintf(stderr, "[SYN] start player=0x%08x song=0x%08x wt=0x%08x pcm=0x%08x "
                    "r7=%u r8=%u r9=%u r10=%u lr=0x%08x\n",
            g_cpu.gpr[3], g_cpu.gpr[4], g_cpu.gpr[5], g_cpu.gpr[6],
            g_cpu.gpr[7], g_cpu.gpr[8], g_cpu.gpr[9], g_cpu.gpr[10], g_cpu.lr);
    if (g_cpu.gpr[4] >= 0x80003000u && g_cpu.gpr[4] < 0x94000000u) {
        uint32_t s = g_cpu.gpr[4];
        fprintf(stderr, "[SYN]   song[0..15]:");
        for (int i = 0; i < 16; ++i) fprintf(stderr, " %02x", MEM_R8(s + i));
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

// TEMP(bring-up): host-side bulk writes (fread/memset in sdk io paths) call
// this to flag any range that covers the MEM2 OSHeap descriptor.
void robox_hostwrite_check(const char *tag, uint32_t dst_va, uint32_t len) {
    if (dst_va < 0x90100030u && dst_va + len > 0x90100000u)
        fprintf(stderr, "[HEAP-HOSTW] %s dst=0x%08x len=0x%x COVERS HEAPDESC\n",
                tag, dst_va, len);
}

// Bink probes referenced by peripherals.c / gx_ogl.c movie-quad detection.
const void *bink_hle_get_frame(int *w, int *h) {
    if (w) *w = 0;
    if (h) *h = 0;
    return 0;
}
int bink_hle_is_open(void)   { return 0; }
int bink_hle_is_active(void) { return 0; }
int bink_hle_has_frame(void) { return 0; }
const void *bink_hle_peek_frame(int *w, int *h) {
    if (w) *w = 0;
    if (h) *h = 0;
    return 0;
}

// peripherals.c defines __wrap_func_80405xxx IOS-async wrappers (RGH link
// option). Without -Wl,--wrap those wrappers are dead code, but their
// __real_ externs still need definitions to link. They must never run.
#define RIO_DEAD_REAL(name) \
    void name(void) { \
        fprintf(stderr, "[robox] FATAL: RGH wrap fallback " #name " called\n"); \
    }
RIO_DEAD_REAL(__real_func_80405130)
RIO_DEAD_REAL(__real_func_80405380)
RIO_DEAD_REAL(__real_func_804054f0)
RIO_DEAD_REAL(__real_func_80405700)
RIO_DEAD_REAL(__real_func_80405910)
RIO_DEAD_REAL(__real_func_80405ae0)
RIO_DEAD_REAL(__real_func_80405e90)
