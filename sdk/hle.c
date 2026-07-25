// sdk/hle.c -- framework bits: tracing, stub registration, guest-printf.

#include "hle.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>


// ---------------------------------------------------------------------------
// Tracing + stub warnings
// ---------------------------------------------------------------------------

void hle_trace_(const char *fn) {
    fprintf(stderr, "[hle] %s\n", fn);
    fflush(stderr);
}

// Dirt-cheap thunks used for mapping early-boot CPU-init chains to no-ops.
void hle_nop_ret0(void)    { HLE_RET(0); }
void hle_nop_ret1(void)    { HLE_RET(1); }
void hle_nop_ret3(void)    { HLE_RET(3); }    // WUDGetStatus "ready"
void hle_nop_retneg1(void) { HLE_RET((uint32_t)-1); }
void hle_nop_noret(void)   { /* preserve r3 */ }

// A very small "seen before" table so each stub prints once, not per-call.
// Linear search on a 256-entry array; fine because we never expect thousands
// of unique stubs per run.
#define STUB_CACHE 256
static const char *stub_seen[STUB_CACHE];
static size_t stub_seen_count;

void hle_stub_(const char *fn) {
    for (size_t i = 0; i < stub_seen_count; ++i) {
        if (stub_seen[i] == fn) return;       // pointer-equality works: string literals
    }
    if (stub_seen_count < STUB_CACHE) {
        stub_seen[stub_seen_count++] = fn;
    }
    fprintf(stderr, "[hle] STUB: %s (returning 0)\n", fn);
}


// ---------------------------------------------------------------------------
// Guest-memory printf -- used by OSReport/OSPanic/OSFatal. The guest's fmt
// string lives in guest VA space and args come from r3..r10 + stack. We
// rebuild a host-friendly string by interpreting the format directives.
//
// Covered: %s %d %u %x %X %c %p %ld %lu %lx %f %08x-etc. width/precision.
// Uncovered: %*s, positional args, floating widths -- good enough for
// typical Nintendo SDK diagnostics.
// ---------------------------------------------------------------------------

static uint32_t fetch_int_arg(int *reg_idx) {
    // r3 = fmt (arg 0). Additional args start at r4.
    uint32_t v = g_cpu.gpr[3 + *reg_idx];
    if (*reg_idx < 7) (*reg_idx)++;     // up to r10
    return v;
}

static double fetch_float_arg(int *freg_idx) {
    double v = g_cpu.fpr[1 + *freg_idx].f64;
    if (*freg_idx < 7) (*freg_idx)++;
    return v;
}

int hle_printf_guest(const char *fmt_unused, uint32_t first_arg_reg_unused) {
    // fmt is actually at r3. We ignore the parameters above (left in the sig
    // for callers that want to be explicit) and re-read from g_cpu.
    const char *fmt = (const char *)ppc_host_ptr(g_cpu.gpr[3]);
    if (!fmt) return 0;

    // reg_idx starts at 1 (r4 is the first "real" arg after fmt)
    int reg = 1;
    int freg = 0;
    char buf[1024];
    size_t pos = 0;

    #define EMIT(c) do { if (pos + 1 < sizeof buf) buf[pos++] = (c); } while (0)

    for (const char *p = fmt; *p && pos + 1 < sizeof buf; ++p) {
        if (*p != '%') { EMIT(*p); continue; }
        // Parse format spec: %[flags][width][.precision][length]conv
        char spec[32]; size_t sp = 0; spec[sp++] = '%';
        ++p;
        while (*p && strchr("-+ #0", *p) && sp < sizeof spec - 2) spec[sp++] = *p++;
        while (*p >= '0' && *p <= '9' && sp < sizeof spec - 2) spec[sp++] = *p++;
        if (*p == '.') { spec[sp++] = *p++;
            while (*p >= '0' && *p <= '9' && sp < sizeof spec - 2) spec[sp++] = *p++; }
        while (*p && strchr("hlLzjt", *p) && sp < sizeof spec - 2) spec[sp++] = *p++;
        char conv = *p;
        spec[sp++] = conv; spec[sp] = 0;

        char out[256];
        switch (conv) {
            case 'd': case 'i': snprintf(out, sizeof out, spec, (int)fetch_int_arg(&reg)); break;
            case 'u':           snprintf(out, sizeof out, spec, (unsigned)fetch_int_arg(&reg)); break;
            case 'x': case 'X':
            case 'o': case 'p': snprintf(out, sizeof out, spec, (unsigned)fetch_int_arg(&reg)); break;
            case 'c':           snprintf(out, sizeof out, spec, (int)fetch_int_arg(&reg)); break;
            case 's': {
                uint32_t va = fetch_int_arg(&reg);
                const char *gs = va ? (const char*)ppc_host_ptr(va) : "(null)";
                snprintf(out, sizeof out, spec, gs ? gs : "(badptr)");
                break;
            }
            case 'f': case 'e': case 'g': case 'F': case 'E': case 'G': {
                double v = fetch_float_arg(&freg);
                snprintf(out, sizeof out, spec, v);
                break;
            }
            case '%': out[0] = '%'; out[1] = 0; break;
            default:  snprintf(out, sizeof out, "%%%c?", conv); break;
        }
        for (char *q = out; *q && pos + 1 < sizeof buf; ++q) EMIT(*q);
    }
    buf[pos] = 0;
    #undef EMIT

    // Write to stdout (trailing newline added only if caller didn't).
    fputs(buf, stdout);
    return (int)pos;
}
