// sdk/rt.c -- C runtime functions (memset/memcpy/string ops).
//
// These are extremely hot. Going through MEM_R8/W8 per byte works but burns
// cycles on byte-swap + bounds-check overhead for every byte. We instead
// translate the guest VA to a host pointer once, then use host memcpy/memset
// -- which is 10-100x faster and still correct because the bytes we move are
// raw (no endianness involved for memcpy of opaque data).

#include "hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// ---------------------------------------------------------------------------
// memset(dst, c, n) -- r3 = dst VA, r4 = c (byte), r5 = n
// Returns r3 = original dst VA (per C convention).
// ---------------------------------------------------------------------------
void hle_memset(void) {
    uint32_t va   = HLE_ARG_U32(0);
    uint8_t  byte = (uint8_t)HLE_ARG_U32(1);
    uint32_t n    = HLE_ARG_U32(2);
    // safe_span() lives further down; forward-declare for this call.
    extern uint32_t safe_span(uint32_t);
    if (n > 0x01000000u) n = 0x01000000u;
    uint32_t cap = safe_span(va);
    if (n > cap) n = cap;
    if (n) {
        if (va <= 0x806c6b14u && va + n > 0x806c6b14u) {
            fprintf(stderr, "[WATCH-W] SLIB-SLOT covered by memset(0x%08x, 0x%02x, 0x%x) lr=0x%08x\n",
                    va, byte, n, g_cpu.lr);
            fflush(stderr);
        }
        uint8_t *dst = (uint8_t*)ppc_host_ptr(va);
        memset(dst, byte, n);
    }
    HLE_RET(va);
}

// Sanity cap: a single guest memcpy over ~16 MB is almost certainly a bad
// size argument (corrupt r5). Clipping avoids a host AV reading past mapped
// guest memory via the deadzone fallback.
#define HLE_MEMOP_MAX  0x01000000u

// Returns the largest contiguous region size (in bytes) that's safe to
// read/write starting at `va`. For unmapped VAs that fall into the 64 KB
// deadzone buffer, this is what remains in the deadzone -- not the caller's
// requested size. Prevents memcpy over 16 MB from spilling past the deadzone
// into unmapped host pages (observed host AV in rescue build).
uint32_t safe_span(uint32_t va) {
    // MEM1 (cached + uncached) and MEM2 both map to contiguous host arrays.
    // Use the declared ranges straight from runtime.h.
    if (va >= 0x80000000u && va < 0x80000000u + 0x01800000u)  // MEM1 cached
        return 0x80000000u + 0x01800000u - va;
    if (va >= 0xC0000000u && va < 0xC0000000u + 0x01800000u)  // MEM1 uncached
        return 0xC0000000u + 0x01800000u - va;
    if (va >= 0x90000000u && va < 0x90000000u + 0x04000000u)  // MEM2
        return 0x90000000u + 0x04000000u - va;
    // Anything else (null, low-MEM1 stragglers, garbage VAs) lands in the
    // 64 KB deadzone in ppc_host_ptr. Only the low 16 bits of va index the
    // deadzone, so the usable span is what's left of that 64 KB. Previously
    // we returned a 32 MB span for va < 0x02000000 -- that claim was wrong
    // (those VAs hit the deadzone too) and caused host segfaults on
    // memset/memcpy of "0x02000000 - va" bytes past a 64 KB buffer.
    return 0x10000u - (va & 0xFFFFu);
}

/* A copy whose destination is the write-gather pipe (0xCC008000..3F) is the
 * game streaming a precompiled GX command blob into the FIFO. ppc_host_ptr
 * maps that VA to the deadzone — the commands would vanish. Route the bytes
 * to the FIFO parser instead. */
static int copy_to_gather_pipe(uint32_t dst_va, uint32_t src_va, uint32_t n) {
    if ((dst_va & ~0x3Fu) != 0xCC008000u) return 0;
    extern void gx_fifo_push(const uint8_t *bytes, int n);
    const uint8_t *src = (const uint8_t *)ppc_host_ptr(src_va);
    if (src && n && n < 0x01000000u) gx_fifo_push(src, (int)n);
    static unsigned s_n;
    if (s_n++ < 8) {
        fprintf(stderr, "[GP-COPY] %u bytes src=0x%08x -> FIFO lr=0x%08x\n",
                n, src_va, g_cpu.lr);
        fflush(stderr);
    }
    return 1;
}

void hle_memcpy(void) {
    uint32_t dst_va = HLE_ARG_U32(0);
    uint32_t src_va = HLE_ARG_U32(1);
    uint32_t n      = HLE_ARG_U32(2);
    if (copy_to_gather_pipe(dst_va, src_va, n)) { HLE_RET(dst_va); return; }
    /* DIAG (audio): copies landing in the sound-ring region, or filling the
     * stream segment source buffer (~0x907ff7f0) that arrives as zeros */
    if (dst_va >= 0x907f0000u && dst_va < 0x90900000u && n > 64) {
        static int s_bank;
        if (++s_bank <= 24) {
            fprintf(stderr, "[SNDBANK-CPY] dst=0x%08x src=0x%08x n=%u lr=0x%08x\n",
                    dst_va, src_va, n, g_cpu.lr);
            fflush(stderr);
        }
    }
    if (src_va < 0x02000000u && n > 0x100) {
        HLE_RET(dst_va); return;
    }
    if (n > HLE_MEMOP_MAX) n = HLE_MEMOP_MAX;
    // Additional cap: don't let n exceed the safe span at either side.
    uint32_t cap = safe_span(dst_va); if (n > cap) n = cap;
    cap = safe_span(src_va); if (n > cap) n = cap;
    if (n) {
        uint8_t *dst = (uint8_t*)ppc_host_ptr(dst_va);
        uint8_t *src = (uint8_t*)ppc_host_ptr(src_va);
        if (dst && src) memcpy(dst, src, n);
    }
    HLE_RET(dst_va);
}

void hle_memmove(void) {
    uint32_t dst_va = HLE_ARG_U32(0);
    uint32_t src_va = HLE_ARG_U32(1);
    uint32_t n      = HLE_ARG_U32(2);
    if (copy_to_gather_pipe(dst_va, src_va, n)) { HLE_RET(dst_va); return; }
    if (n > HLE_MEMOP_MAX) n = HLE_MEMOP_MAX;
    if (n) {
        uint8_t *dst = (uint8_t*)ppc_host_ptr(dst_va);
        uint8_t *src = (uint8_t*)ppc_host_ptr(src_va);
        memmove(dst, src, n);
    }
    HLE_RET(dst_va);
}

void hle_memcmp(void) {
    uint32_t a_va = HLE_ARG_U32(0);
    uint32_t b_va = HLE_ARG_U32(1);
    uint32_t n    = HLE_ARG_U32(2);
    if (n == 0) { HLE_RET(0); return; }
    const uint8_t *a = (const uint8_t*)ppc_host_ptr(a_va);
    const uint8_t *b = (const uint8_t*)ppc_host_ptr(b_va);
    HLE_RET((uint32_t)(int32_t)memcmp(a, b, n));
}


// ---------------------------------------------------------------------------
// String ops. All strings live in guest memory; we read them via host ptrs.
// ---------------------------------------------------------------------------

void hle_strlen(void) {
    uint32_t va = HLE_ARG_U32(0);
    const char *s = (const char*)ppc_host_ptr(va);
    // Cap so a broken (non-terminated) string can't return a billion-byte
    // length that blows up downstream code. 64 KB is way more than any
    // reasonable game string.
    size_t n = 0;
    while (n < 0x10000 && s[n] != '\0') ++n;
    HLE_RET((uint32_t)n);
}

void hle_strcpy(void) {
    uint32_t dst_va = HLE_ARG_U32(0);
    uint32_t src_va = HLE_ARG_U32(1);
    strcpy((char*)ppc_host_ptr(dst_va), (char*)ppc_host_ptr(src_va));
    HLE_RET(dst_va);
}

void hle_strncpy(void) {
    uint32_t dst_va = HLE_ARG_U32(0);
    uint32_t src_va = HLE_ARG_U32(1);
    uint32_t n      = HLE_ARG_U32(2);
    strncpy((char*)ppc_host_ptr(dst_va), (char*)ppc_host_ptr(src_va), n);
    HLE_RET(dst_va);
}

void hle_strcmp(void) {
    const char *a = (const char*)ppc_host_ptr(HLE_ARG_U32(0));
    const char *b = (const char*)ppc_host_ptr(HLE_ARG_U32(1));
    // Cap compare length so a corrupt (unterminated) input can't walk host
    // memory for minutes. 64 KB is more than any plausible game string.
    int rv = strncmp(a, b, 0x10000);
    HLE_RET((uint32_t)(int32_t)rv);
}

void hle_strncmp(void) {
    const char *a = (const char*)ppc_host_ptr(HLE_ARG_U32(0));
    const char *b = (const char*)ppc_host_ptr(HLE_ARG_U32(1));
    uint32_t n    = HLE_ARG_U32(2);
    HLE_RET((uint32_t)(int32_t)strncmp(a, b, n));
}

void hle_strcat(void) {
    uint32_t dst_va = HLE_ARG_U32(0);
    const char *src = (const char*)ppc_host_ptr(HLE_ARG_U32(1));
    strcat((char*)ppc_host_ptr(dst_va), src);
    HLE_RET(dst_va);
}

void hle_strchr(void) {
    uint32_t s_va = HLE_ARG_U32(0);
    int      c    = (int)HLE_ARG_S32(1);
    const char *s = (const char*)ppc_host_ptr(s_va);
    const char *hit = strchr(s, c);
    HLE_RET(hit ? (uint32_t)(s_va + (hit - s)) : 0);
}

void hle_strstr(void) {
    uint32_t s_va = HLE_ARG_U32(0);
    const char *s = (const char*)ppc_host_ptr(s_va);
    const char *needle = (const char*)ppc_host_ptr(HLE_ARG_U32(1));
    const char *hit = strstr(s, needle);
    HLE_RET(hit ? (uint32_t)(s_va + (hit - s)) : 0);
}


// ---------------------------------------------------------------------------
// printf family -- sprintf / vsprintf write into a guest buffer.
// ---------------------------------------------------------------------------
//
// We implement sprintf/snprintf by reusing hle_printf_guest semantics: walk
// the guest format string and fetch args from registers. But instead of
// writing to stdout, we build the string in a host buffer and copy it back
// to guest memory.

static int printf_to_buf(char *outbuf, size_t outcap, int first_reg, int first_freg) {
    const char *fmt = (const char *)ppc_host_ptr(g_cpu.gpr[first_reg]);
    if (!fmt || outcap == 0) return 0;

    int reg = first_reg + 1;
    int freg = first_freg;
    size_t pos = 0;

    #define EMIT(c) do { if (pos + 1 < outcap) outbuf[pos++] = (c); } while (0)

    for (const char *p = fmt; *p && pos + 1 < outcap; ++p) {
        if (*p != '%') { EMIT(*p); continue; }
        char spec[32]; size_t sp = 0; spec[sp++] = '%';
        ++p;
        while (*p && strchr("-+ #0", *p) && sp < sizeof spec - 2) spec[sp++] = *p++;
        while (*p >= '0' && *p <= '9' && sp < sizeof spec - 2) spec[sp++] = *p++;
        if (*p == '.') { spec[sp++] = *p++;
            while (*p >= '0' && *p <= '9' && sp < sizeof spec - 2) spec[sp++] = *p++; }
        while (*p && strchr("hlLzjt", *p) && sp < sizeof spec - 2) spec[sp++] = *p++;
        char conv = *p;
        spec[sp++] = conv; spec[sp] = 0;

        char part[256]; int plen = 0;
        switch (conv) {
            case 'd': case 'i':
                plen = snprintf(part, sizeof part, spec, (int)g_cpu.gpr[reg]); reg += (reg<11) ? 1:0; break;
            case 'u': case 'x': case 'X': case 'o': case 'p': case 'c':
                plen = snprintf(part, sizeof part, spec, (unsigned)g_cpu.gpr[reg]); reg += (reg<11) ? 1:0; break;
            case 's': {
                uint32_t va = g_cpu.gpr[reg]; reg += (reg<11) ? 1:0;
                const char *s = va ? (const char*)ppc_host_ptr(va) : "(null)";
                plen = snprintf(part, sizeof part, spec, s);
                break;
            }
            case 'f': case 'e': case 'g': case 'F': case 'E': case 'G':
                plen = snprintf(part, sizeof part, spec, g_cpu.fpr[1 + freg].f64);
                if (freg < 7) freg++;
                break;
            case '%': part[0] = '%'; part[1] = 0; plen = 1; break;
            default:  plen = snprintf(part, sizeof part, "%%%c?", conv); break;
        }
        for (int i = 0; i < plen && pos + 1 < outcap; ++i) EMIT(part[i]);
    }
    if (pos < outcap) outbuf[pos] = 0;
    return (int)pos;
    #undef EMIT
}

void hle_sprintf(void) {
    uint32_t dst_va = HLE_ARG_U32(0);
    char   *dst = (char*)ppc_host_ptr(dst_va);
    char host_buf[2048];
    // format is at r4, args from r5; pass first_reg=4.
    int n = printf_to_buf(host_buf, sizeof host_buf, 4, 0);
    memcpy(dst, host_buf, (size_t)n + 1);
    HLE_RET((uint32_t)n);
}

void hle_snprintf(void) {
    uint32_t dst_va = HLE_ARG_U32(0);
    uint32_t cap    = HLE_ARG_U32(1);
    char  *dst = (char*)ppc_host_ptr(dst_va);
    char host_buf[2048];
    int n = printf_to_buf(host_buf, sizeof host_buf, 5, 0);  // fmt at r5
    size_t copy = (cap && (uint32_t)n < cap) ? (size_t)n : (cap ? cap - 1 : 0);
    if (cap) { memcpy(dst, host_buf, copy); dst[copy] = 0; }
    HLE_RET((uint32_t)n);
}

/* CodeWarrior PPC EABI va_list (layout confirmed from STD_sprintf's builder
 * at 0x80082a0c):
 *   +0  u8  gpr_idx      (starts at 2: r3,r4 consumed by dst/fmt)
 *   +1  u8  fpr_idx      (f1..f8)
 *   +4  u32 overflow_ptr (caller stack, args past the registers)
 *   +8  u32 reg_save_ptr (spilled r3..r10 at +0x00..0x1f, f1..f8 doubles
 *                         at +0x20..0x5f)
 * The old implementation ignored this struct and read raw registers, which
 * dropped/garbled every %s (asset names like "_0.tpl" without the prefix —
 * years of black screens traced back here). */
typedef struct { uint32_t va; uint32_t gpr, fpr, ovfl, save; } GuestVaList;

static void valist_load(GuestVaList *v, uint32_t va) {
    v->va   = va;
    v->gpr  = MEM_R8(va);
    v->fpr  = MEM_R8(va + 1);
    v->ovfl = MEM_R32(va + 4);
    v->save = MEM_R32(va + 8);
}
static uint32_t valist_arg32(GuestVaList *v) {
    if (v->gpr < 8) return MEM_R32(v->save + v->gpr++ * 4u);
    uint32_t val = MEM_R32(v->ovfl); v->ovfl += 4; return val;
}
static double valist_f64(GuestVaList *v) {
    if (v->fpr < 8) return MEM_RD(v->save + 0x20u + v->fpr++ * 8u);
    v->ovfl = (v->ovfl + 7u) & ~7u;
    double d = MEM_RD(v->ovfl); v->ovfl += 8; return d;
}

static int printf_valist_to_buf(char *outbuf, size_t outcap,
                                uint32_t fmt_va, GuestVaList *v) {
    const char *fmt = (const char *)ppc_host_ptr(fmt_va);
    if (!fmt || outcap == 0) return 0;
    size_t pos = 0;
    #define EMITV(c) do { if (pos + 1 < outcap) outbuf[pos++] = (c); } while (0)
    for (const char *p = fmt; *p && pos + 1 < outcap; ++p) {
        if (*p != '%') { EMITV(*p); continue; }
        char spec[32]; size_t sp = 0; spec[sp++] = '%';
        ++p;
        while (*p && strchr("-+ #0", *p) && sp < sizeof spec - 2) spec[sp++] = *p++;
        while (*p >= '0' && *p <= '9' && sp < sizeof spec - 2) spec[sp++] = *p++;
        if (*p == '.') { spec[sp++] = *p++;
            while (*p >= '0' && *p <= '9' && sp < sizeof spec - 2) spec[sp++] = *p++; }
        while (*p && strchr("hlLzjt", *p)) ++p;   /* drop length mods */
        char conv = *p;
        spec[sp++] = conv; spec[sp] = 0;

        char part[512]; int plen = 0;
        switch (conv) {
            case 'd': case 'i':
                plen = snprintf(part, sizeof part, spec, (int)valist_arg32(v)); break;
            case 'u': case 'x': case 'X': case 'o': case 'p': case 'c':
                plen = snprintf(part, sizeof part, spec, (unsigned)valist_arg32(v)); break;
            case 's': {
                uint32_t sva = valist_arg32(v);
                const char *s = (sva >= 0x80000000u && sva < 0x94000000u)
                                ? (const char*)ppc_host_ptr(sva) : "(null)";
                /* (this path only runs under RGH_FIXED_SPRINTF=1 — see
                 * hle_vsprintf. Default mode uses printf_to_buf, the
                 * historic register-misread, so ALL conversions stay
                 * bit-identical to what script registration produces.) */
                plen = snprintf(part, sizeof part, spec, s);
                break;
            }
            case 'f': case 'e': case 'g': case 'F': case 'E': case 'G':
                plen = snprintf(part, sizeof part, spec, valist_f64(v)); break;
            case '%': part[0] = '%'; part[1] = 0; plen = 1; break;
            default:  plen = snprintf(part, sizeof part, "%%%c?", conv); break;
        }
        for (int i = 0; i < plen && pos + 1 < outcap; ++i) EMITV(part[i]);
    }
    if (pos < outcap) outbuf[pos] = 0;
    return (int)pos;
    #undef EMITV
}

void hle_vsprintf(void) {
    uint32_t dst_va = HLE_ARG_U32(0);
    char *dst = (char*)ppc_host_ptr(dst_va);
    char host_buf[2048];
    int n;
    /* DEFAULT: the HISTORIC behavior, bit-for-bit — fmt from r4, args
     * misread from raw registers r5.. (the va_list is ignored). Every name
     * the game builds through here must keep matching names built by the
     * RSO's own (equally broken) sprintf — see reference_sprintf_two_sided
     * memory + the comment in printf_valist_to_buf's %s case. Fixing ANY
     * conversion one-sided (even %d) unbinds script tracks (LoadBink etc.).
     * RGH_FIXED_SPRINTF=1 switches to the correct CW va_list path. */
    static int s_fixed = -1;
    if (s_fixed < 0) { const char *e = getenv("RGH_FIXED_SPRINTF"); s_fixed = (e && e[0]=='1'); }
    /* SURGICAL: save-file NAMES must be formatted correctly — the historic
     * register-misread produced 'slt_-2123499504_0.sav' (a pointer as the
     * slot id) on the profile-click path, so the game wrote one name and
     * re-read another -> NAND err 5 -> save flow parked in state 31 (the
     * frozen menu). Route ONLY formats mentioning the save files through
     * the correct va_list path; everything else keeps the historic misread
     * (a GLOBAL fix un-skips the strap/legal screens and script-name
     * matching — see the black-screen finding + sprintf-two-sided memory). */
    int use_fixed = s_fixed;
    if (!use_fixed) {
        const char *fmt = (const char *)ppc_host_ptr(HLE_ARG_U32(1));
        if (fmt && (strstr(fmt, ".sav") || strstr(fmt, ".dat") ||
                    strstr(fmt, "slt_"))) {
            use_fixed = 1;
            static int s_log;
            if (s_log < 8) { s_log++;
                fprintf(stderr, "[VSPRINTF] save-name fmt '%s' -> fixed path\n", fmt);
                fflush(stderr); }
        }
    }
    if (!use_fixed) {
        n = printf_to_buf(host_buf, sizeof host_buf, 4, 0);
    } else {
        uint32_t fmt_va = HLE_ARG_U32(1);
        uint32_t val_va = HLE_ARG_U32(2);
        GuestVaList v;
        valist_load(&v, val_va);
        n = printf_valist_to_buf(host_buf, sizeof host_buf, fmt_va, &v);
    }
    memcpy(dst, host_buf, (size_t)n + 1);
    {
        static int s_n;
        if (s_n < 12) { s_n++;
            fprintf(stderr, "[VSPRINTF] -> '%s'\n", host_buf); fflush(stderr); }
    }
    HLE_RET((uint32_t)n);
}


// ---------------------------------------------------------------------------
// atoi/atof
// ---------------------------------------------------------------------------

void hle_atoi(void) {
    const char *s = (const char*)ppc_host_ptr(HLE_ARG_U32(0));
    HLE_RET((uint32_t)atoi(s));
}

void hle_atof(void) {
    const char *s = (const char*)ppc_host_ptr(HLE_ARG_U32(0));
    HLE_RET_F64(atof(s));
}


// qsort(base, nmemb, size, cmp). Implemented as a host-side qsort that
// trampolines into the guest comparator via ppc_call_indirect. Required
// because the BIG file's TOOsarray uses qsort to sort its (key,position)
// FAT after bulk-loading via SetFastMode(1) -> bulk add -> SetFastMode(0).
// Without sort, binary search by key returns NOT FOUND for everything,
// which prevents script lookup, SCR universe load, asset streaming, etc.
//
// Implementation: simple insertion sort on the guest buffer in place.
// (Not a real qsort, but stable, tiny, and good enough for the FAT sizes
// we deal with -- 4330 entries on RGH.) Each comparison sets r3=a, r4=b
// and dispatches to the guest comparator via ppc_call_indirect, then
// reads r3 as the int result.
#include <string.h>
// Thread-local state for the qsort PPC comparator trampoline.
// qsort() is not reentrant with different comparators, but PPC code never
// calls qsort recursively, so a single global is fine.
static uint32_t  s_qsort_cmp_va;
static uint32_t  s_qsort_base_va;
static uint32_t  s_qsort_elem_size;
static PPC_Func  s_qsort_cmp_fn;   // cached direct pointer, avoids binary search per call

static int qsort_ppc_cmp(const void *a, const void *b) {
    // a and b are host pointers into the guest buffer. Convert to guest VAs.
    uint32_t va_a = s_qsort_base_va + (uint32_t)((const uint8_t *)a - (const uint8_t *)ppc_host_ptr(s_qsort_base_va));
    uint32_t va_b = s_qsort_base_va + (uint32_t)((const uint8_t *)b - (const uint8_t *)ppc_host_ptr(s_qsort_base_va));
    uint32_t saved_r3 = g_cpu.gpr[3];
    uint32_t saved_r4 = g_cpu.gpr[4];
    uint32_t saved_lr = g_cpu.lr;
    g_cpu.gpr[3] = va_a;
    g_cpu.gpr[4] = va_b;
    g_cpu.lr = 0;
    // Call via cached direct pointer — no binary search overhead.
    if (s_qsort_cmp_fn) s_qsort_cmp_fn();
    else                 ppc_call_indirect(s_qsort_cmp_va);
    int32_t result = (int32_t)g_cpu.gpr[3];
    g_cpu.gpr[3] = saved_r3;
    g_cpu.gpr[4] = saved_r4;
    g_cpu.lr = saved_lr;
    return result;
}

void hle_qsort(void) {
    uint32_t base_va = g_cpu.gpr[3];
    uint32_t nmemb   = g_cpu.gpr[4];
    uint32_t size    = g_cpu.gpr[5];
    uint32_t cmp_va  = g_cpu.gpr[6];
    if (!base_va || !size || nmemb < 2 || !cmp_va) { HLE_RET(0); return; }
    uint8_t *base = (uint8_t *)ppc_host_ptr(base_va);
    if (!base) { HLE_RET(0); return; }

    static unsigned logs;
    if (logs < 20) {
        fprintf(stderr, "[hle_qsort] elem_size=%u count=%u cmp=0x%08x\n", size, nmemb, cmp_va);
        ++logs;
    }

    // Set up trampoline state. Cache the direct function pointer so the
    // comparator bypasses the binary-search table lookup on every call.
    s_qsort_cmp_va    = cmp_va;
    s_qsort_base_va   = base_va;
    s_qsort_elem_size = size;
    s_qsort_cmp_fn    = ppc_lookup_func(cmp_va);  // NULL if not in table (rare)

    // Host qsort: O(n log n) with at most n*log2(n) PPC comparator calls.
    qsort(base, nmemb, size, qsort_ppc_cmp);

    HLE_RET(0);
}


// Route libc malloc/free/calloc/realloc to our bump allocator.
// The CW runtime's malloc needs a default heap initialized via
// `InitDefaultHeap` which fails on our setup ("No Heap Available" in the
// log). Without this HLE, `__nwa` (operator new) catches the NULL return
// and throws bad_alloc into std::terminate.

extern uint32_t game_heap_alloc(uint32_t size, uint32_t align);

void hle_malloc(void) {
    uint32_t size = HLE_ARG_U32(0);
    HLE_RET(game_heap_alloc(size ? size : 4, 16));
}
void hle_calloc(void) {
    uint32_t n    = HLE_ARG_U32(0);
    uint32_t sz   = HLE_ARG_U32(1);
    uint32_t tot  = n * sz;
    if (tot == 0) tot = 4;
    uint32_t va = game_heap_alloc(tot, 16);
    if (va) {
        uint8_t *p = (uint8_t *)ppc_host_ptr(va);
        if (p) memset(p, 0, tot);
    }
    HLE_RET(va);
}
void hle_realloc(void) {
    // (ptr, size) -- best-effort: always hand out a fresh block; old block
    // stays allocated. Game's own allocator chain also leaks in this port.
    uint32_t old_va = HLE_ARG_U32(0);
    uint32_t size   = HLE_ARG_U32(1);
    if (size == 0) { HLE_RET(0); return; }
    uint32_t new_va = game_heap_alloc(size, 16);
    if (new_va && old_va) {
        // Copy as much as we can; we don't know the old size, so copy up
        // to the new size (the old allocation was at least this big unless
        // the caller shrank -- copy is read-only and safe).
        uint8_t *dst = (uint8_t *)ppc_host_ptr(new_va);
        uint8_t *src = (uint8_t *)ppc_host_ptr(old_va);
        if (dst && src) memcpy(dst, src, size);
    }
    HLE_RET(new_va);
}
void hle_free(void) {
    // No-op; bump allocator never frees.
    HLE_RET(0);
}
