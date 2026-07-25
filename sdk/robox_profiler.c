// sdk/robox_profiler.c -- frame-drop profiler + crash dump.
//
// The point: run SILENT while the game is healthy, and produce a precise
// report only when a frame actually misses its deadline (or when the game
// dies). Nothing here costs anything on the hot path — the guest is sampled
// by a separate host thread, and the per-frame bookkeeping is a couple of
// QueryPerformanceCounter reads.
//
// A dropped frame reports:
//   * how long the frame took vs the 16.7 ms budget, split into the host-side
//     blocks we time (audio mixing, GX FIFO, present) and "guest" (the rest,
//     i.e. recompiled game code)
//   * the guest addresses the sampler caught during that frame, resolved to
//     the nearest recompiled function (func_XXXXXXXX+off) so a hot spot can
//     be looked up in src/generated/ directly.
//
// Env:
//   RECOMP_FRAMEDROP_MS=n  report frames slower than n ms (default 22)
//   RECOMP_PROFILE=0       disable entirely
//
// Crash path: robox_prof_crash_dump() prints the guest registers, the crashing
// guest address, a guest backtrace, the recent indirect dispatches and the
// frame history. It always writes to stderr -- on Android that is logcat and on
// the web it is the browser console, and on both of those a crash_dump.txt in
// the (virtual) filesystem is something nobody can ever read. Desktop builds
// additionally get logs/crash_dump.txt.

#include "hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

static double qpc_freq_us;

static unsigned long long prof_now_us(void)
{
#if defined(_WIN32)
    LARGE_INTEGER t;
    if (qpc_freq_us == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpc_freq_us = (double)f.QuadPart / 1000000.0;
    }
    QueryPerformanceCounter(&t);
    return (unsigned long long)((double)t.QuadPart / qpc_freq_us);
#else
    extern uint64_t ms_now(void);
    return ms_now() * 1000ull;
#endif
}

// ---------------------------------------------------------------------------
// Host-side scope timers (audio / fifo / present)
// ---------------------------------------------------------------------------

static unsigned long long g_scope_us[ROBOX_PROF_SCOPES];
static unsigned long long g_scope_start[ROBOX_PROF_SCOPES];
static const char *g_scope_name[ROBOX_PROF_SCOPES] = { "audio", "gxfifo", "present",
                                                       "tex", "draw", "upload" };

void robox_prof_scope_begin(int id)
{
    if ((unsigned)id < ROBOX_PROF_SCOPES) g_scope_start[id] = prof_now_us();
}

void robox_prof_scope_end(int id)
{
    if ((unsigned)id < ROBOX_PROF_SCOPES && g_scope_start[id])
        g_scope_us[id] += prof_now_us() - g_scope_start[id];
}

// Set by frame_limiter: microseconds it slept waiting for the 60 Hz deadline.
// That is idle time, not work, so it must not count against the frame budget.
unsigned long long g_frame_sleep_us;

// ---------------------------------------------------------------------------
// Guest sampler: a host thread that snapshots the guest's most recent call
// return address. No instrumentation in the guest path at all.
// ---------------------------------------------------------------------------

#define SAMPLE_RING 4096
static uint32_t  g_samples[SAMPLE_RING];
static volatile unsigned g_sample_head;
static volatile int      g_sampler_stop;

/* Set while frame_limiter is parked waiting for the 60 Hz deadline. Samples
 * taken then are the game sitting in our own vsync wait, not doing work —
 * counting them made VIWaitForRetrace look like the hottest function in every
 * report. */
volatile int g_prof_in_wait;

#if defined(_WIN32)
static DWORD WINAPI sampler_thread(LPVOID unused)
{
    (void)unused;
    while (!g_sampler_stop) {
        g_samples[g_sample_head % SAMPLE_RING] = g_prof_in_wait ? 0 : g_cpu.lr;
        g_sample_head++;
        Sleep(1);                       /* ~1000 samples/sec */
    }
    return 0;
}
#endif

static int  g_prof_on = -1;
static unsigned g_drop_ms = 22;

static int prof_enabled(void)
{
    if (g_prof_on < 0) {
        const char *e = getenv("RECOMP_PROFILE");
        g_prof_on = !(e && e[0] == '0');
        const char *d = getenv("RECOMP_FRAMEDROP_MS");
        if (d && d[0]) {
            int v = atoi(d);
            if (v > 0) g_drop_ms = (unsigned)v;
        }
#if defined(_WIN32)
        if (g_prof_on) {
            HANDLE h = CreateThread(NULL, 0, sampler_thread, NULL, 0, NULL);
            if (h) CloseHandle(h);
        }
#endif
    }
    return g_prof_on;
}

// ---------------------------------------------------------------------------
// Nearest recompiled function for a guest address (g_func_table_addrs is
// sorted ascending — the same table ppc_lookup_func binary-searches).
// ---------------------------------------------------------------------------

static uint32_t nearest_func(uint32_t addr, uint32_t *off_out)
{
    if (g_func_table_count == 0 || addr < g_func_table_addrs[0]) {
        *off_out = 0;
        return 0;
    }
    size_t lo = 0, hi = g_func_table_count - 1, best = 0;
    while (lo <= hi) {
        size_t mid = (lo + hi) / 2;
        if (g_func_table_addrs[mid] <= addr) {
            best = mid;
            lo = mid + 1;
        } else {
            if (mid == 0) break;
            hi = mid - 1;
        }
    }
    *off_out = addr - g_func_table_addrs[best];
    return g_func_table_addrs[best];
}

// Rank the samples taken in [from_head, to_head) and print the top few.
static void report_hot_addresses(FILE *out, unsigned from_head, unsigned to_head)
{
    struct { uint32_t fn; uint32_t hits; uint32_t example; } top[8];
    int ntop = 0;
    unsigned n = to_head - from_head;
    if (n > SAMPLE_RING) { from_head = to_head - SAMPLE_RING; n = SAMPLE_RING; }

    for (unsigned i = 0; i < n; ++i) {
        uint32_t addr = g_samples[(from_head + i) % SAMPLE_RING];
        if (!addr) continue;
        uint32_t off, fn = nearest_func(addr, &off);
        if (!fn) continue;
        int j = 0;
        for (; j < ntop; ++j) if (top[j].fn == fn) { top[j].hits++; break; }
        if (j == ntop && ntop < 8) { top[ntop].fn = fn; top[ntop].hits = 1;
                                     top[ntop].example = addr; ntop++; }
    }
    /* selection sort, tiny N */
    for (int a = 0; a < ntop; ++a)
        for (int b = a + 1; b < ntop; ++b)
            if (top[b].hits > top[a].hits) {
                typeof(top[0]) t = top[a]; top[a] = top[b]; top[b] = t;
            }
    if (!ntop) {
        fprintf(out, "         (no guest samples in this frame)\n");
        return;
    }
    for (int i = 0; i < ntop && i < 5; ++i) {
        uint32_t off;
        nearest_func(top[i].example, &off);
        fprintf(out, "         %2u samples  func_%08x+0x%-4x  (lr 0x%08x)\n",
                top[i].hits, top[i].fn, off, top[i].example);
    }
}

// ---------------------------------------------------------------------------
// Frame boundary
// ---------------------------------------------------------------------------

#define FRAME_HIST 64
static struct {
    unsigned long long total_us, work_us;
    unsigned long long scope_us[ROBOX_PROF_SCOPES];
    unsigned draws;
} g_hist[FRAME_HIST];
static unsigned g_frame_no;

void robox_prof_frame(unsigned draws_this_frame)
{
    if (!prof_enabled()) return;

    static unsigned long long last_us;
    static unsigned last_sample_head;
    unsigned long long now = prof_now_us();
    unsigned sample_head = g_sample_head;

    if (last_us) {
        unsigned long long total = now - last_us;
        unsigned long long sleep = g_frame_sleep_us;
        unsigned long long work  = total > sleep ? total - sleep : 0;

        unsigned slot = g_frame_no % FRAME_HIST;
        g_hist[slot].total_us = total;
        g_hist[slot].work_us  = work;
        g_hist[slot].draws    = draws_this_frame;
        for (int i = 0; i < ROBOX_PROF_SCOPES; ++i)
            g_hist[slot].scope_us[i] = g_scope_us[i];
        g_frame_no++;

        if (total >= (unsigned long long)g_drop_ms * 1000ull) {
            static unsigned reported;
            /* Report the first 20 drops, then one in every 64, so a
             * persistently slow section still shows up without flooding. */
            if (reported < 20 || (reported & 63) == 0) {
                unsigned long long known = 0;
                for (int i = 0; i < ROBOX_PROF_SCOPES; ++i) known += g_scope_us[i];
                fprintf(stderr,
                    "[FRAMEDROP] frame %u took %.1f ms (budget 16.7) "
                    "work %.1f ms, %u draws\n",
                    g_frame_no, total / 1000.0, work / 1000.0, draws_this_frame);
                fprintf(stderr, "         host:");
                for (int i = 0; i < ROBOX_PROF_SCOPES; ++i)
                    fprintf(stderr, " %s %.1f ms", g_scope_name[i],
                            g_scope_us[i] / 1000.0);
                fprintf(stderr, "  guest %.1f ms\n",
                        (work > known ? work - known : 0) / 1000.0);
                report_hot_addresses(stderr, last_sample_head, sample_head);
                fflush(stderr);
            }
            reported++;
        }

        /* Optional steady-state summary (RECOMP_PROFILE_SUMMARY=1): one line
         * every 300 frames with the achieved rate and where the time goes.
         * Off by default — a healthy game should print nothing at all. */
        static int summary = -1;
        if (summary < 0) {
            const char *e = getenv("RECOMP_PROFILE_SUMMARY");
            summary = (e && e[0] && e[0] != '0');
        }
        if (summary && (g_frame_no % 300) == 0) {
            unsigned n = g_frame_no < FRAME_HIST ? g_frame_no : FRAME_HIST;
            unsigned long long sum = 0, worst = 0, wsum = 0;
            for (unsigned i = 0; i < n; ++i) {
                unsigned s = (g_frame_no - n + i) % FRAME_HIST;
                sum  += g_hist[s].total_us;
                wsum += g_hist[s].work_us;
                if (g_hist[s].total_us > worst) worst = g_hist[s].total_us;
            }
            if (n && sum) {
                fprintf(stderr,
                    "[PROFILE] %.1f fps (avg frame %.2f ms, work %.2f ms, "
                    "worst %.2f ms) draws/frame %u\n",
                    1000000.0 * n / (double)sum, sum / 1000.0 / n,
                    wsum / 1000.0 / n, worst / 1000.0, draws_this_frame);
                fflush(stderr);
            }
        }
    }

    for (int i = 0; i < ROBOX_PROF_SCOPES; ++i) g_scope_us[i] = 0;
    g_frame_sleep_us = 0;
    last_us = now;
    last_sample_head = sample_head;
}

// ---------------------------------------------------------------------------
// Crash dump
// ---------------------------------------------------------------------------

// Resolve a guest VA to "func_XXXXXXXX+0xNN" in a caller-supplied buffer.
// Returns buf so it can be used inline in an fprintf argument list.
static const char *describe_addr(char *buf, size_t n, uint32_t addr)
{
    uint32_t off, fn = nearest_func(addr, &off);
    if (fn) snprintf(buf, n, "func_%08x+0x%x", fn, off);
    else    snprintf(buf, n, "<not in any recompiled function>");
    return buf;
}

static void crash_dump_write(FILE *f, const char *reason)
{
    char sym[64];

    // Markers the web shell greps for to pull the report onto the page (see
    // tools/web_shell.html). Harmless noise everywhere else.
    fprintf(f, "!!!ROBOX-CRASH-BEGIN!!!\n");
    fprintf(f, "Robox recomp crash dump\n");
    fprintf(f, "reason: %s\n", reason ? reason : "(unknown)");

    // THE headline. Generated code assigns g_cpu.lr immediately before every
    // direct `bl`, so at any point during execution LR is the return address of
    // the most recent call -- i.e. it names the guest function the crash
    // happened in, to within one call.
    fprintf(f, "\nCRASH SITE\n");
    fprintf(f, "  guest address : 0x%08x  %s\n",
            g_cpu.lr, describe_addr(sym, sizeof sym, g_cpu.lr));
    fprintf(f, "  guest ctr     : 0x%08x  %s\n",
            g_cpu.ctr, describe_addr(sym, sizeof sym, g_cpu.ctr));
    fprintf(f, "  guest sp (r1) : 0x%08x\n", g_cpu.gpr[1]);
    fprintf(f, "\n");

    fprintf(f, "guest registers:\n");
    for (int i = 0; i < 32; i += 4)
        fprintf(f, "  r%-2d=%08x  r%-2d=%08x  r%-2d=%08x  r%-2d=%08x\n",
                i, g_cpu.gpr[i], i+1, g_cpu.gpr[i+1],
                i+2, g_cpu.gpr[i+2], i+3, g_cpu.gpr[i+3]);
    fprintf(f, "  lr=%08x ctr=%08x cr=%08x xer=%08x\n\n",
            g_cpu.lr, g_cpu.ctr, g_cpu.cr, g_cpu.xer);

    fprintf(f, "guest backtrace (r1 frame chain):\n");
    {
        uint32_t sp = g_cpu.gpr[1];
        int printed = 0;
        for (int i = 0; i < 16 && sp >= 0x80003000u && sp < 0x94000000u; ++i) {
            uint32_t up = MEM_R32(sp);
            if (up <= sp || up < 0x80003000u || up >= 0x94000000u) break;
            uint32_t lr = MEM_R32(up + 4);
            fprintf(f, "  #%-2d %08x  %s\n", i, lr,
                    describe_addr(sym, sizeof sym, lr));
            sp = up;
            ++printed;
        }
        if (!printed)
            fprintf(f, "  (frame chain unreadable -- r1=0x%08x is not a guest stack)\n",
                    g_cpu.gpr[1]);
    }

    // The sampler is a Windows-only host thread; everywhere else this ring is
    // empty and the indirect-dispatch history below is the substitute.
    if (g_sample_head) {
        fprintf(f, "\nrecent sampled guest addresses (newest last):\n");
        unsigned head = g_sample_head;
        unsigned n = head < 64 ? head : 64;
        for (unsigned i = 0; i < n; ++i) {
            uint32_t a = g_samples[(head - n + i) % SAMPLE_RING];
            if (!a) continue;
            fprintf(f, "  %08x  %s\n", a, describe_addr(sym, sizeof sym, a));
        }
    }

    // Available on every platform: the ring src/runtime.c fills on each
    // bctrl. For a C++ game this is effectively the recent call history.
    fprintf(f, "\nlast indirect dispatches (newest last, %u total):\n",
            g_ind_ring_head);
    {
        unsigned head = g_ind_ring_head;
        unsigned n = head < PPC_IND_RING ? head : PPC_IND_RING;
        for (unsigned i = 0; i < n; ++i) {
            unsigned slot = (head - n + i) & (PPC_IND_RING - 1);
            char from[64];
            fprintf(f, "  from %08x %-34s -> %08x  %s\n",
                    g_ind_ring_lr[slot],
                    describe_addr(from, sizeof from, g_ind_ring_lr[slot]),
                    g_ind_ring_target[slot],
                    describe_addr(sym, sizeof sym, g_ind_ring_target[slot]));
        }
        if (!n) fprintf(f, "  (none)\n");
    }

    // Dereferences of addresses that are neither guest RAM nor hardware MMIO.
    // A burst of these immediately before a crash IS the bug.
    fprintf(f, "\nwild pointer dereferences: %llu total\n",
            (unsigned long long)g_deadzone_hits);
    {
        unsigned head = g_wild_ring_head;
        unsigned n = head < PPC_WILD_RING ? head : PPC_WILD_RING;
        for (unsigned i = 0; i < n; ++i) {
            unsigned slot = (head - n + i) & (PPC_WILD_RING - 1);
            fprintf(f, "  va %08x  from %08x  %s\n",
                    g_wild_ring_va[slot], g_wild_ring_lr[slot],
                    describe_addr(sym, sizeof sym, g_wild_ring_lr[slot]));
        }
        if (!n) fprintf(f, "  (none -- every guest access was to mapped memory)\n");
    }

    fprintf(f, "\nlast frames (most recent last):\n");
    {
        unsigned n = g_frame_no < FRAME_HIST ? g_frame_no : FRAME_HIST;
        for (unsigned i = 0; i < n; ++i) {
            unsigned slot = (g_frame_no - n + i) % FRAME_HIST;
            fprintf(f, "  %6.2f ms total, %6.2f ms work, %u draws\n",
                    g_hist[slot].total_us / 1000.0,
                    g_hist[slot].work_us / 1000.0, g_hist[slot].draws);
        }
        if (!n) fprintf(f, "  (no frame completed)\n");
    }

    fprintf(f, "!!!ROBOX-CRASH-END!!!\n");
    fflush(f);
}

void robox_prof_crash_dump(const char *reason)
{
    // Re-entrancy guard: the report itself reads guest memory through
    // MEM_R32, and a second fault while walking a corrupt frame chain would
    // recurse forever instead of printing anything.
    static int busy;
    if (busy) return;
    busy = 1;

    // stderr first and always. It is the console on desktop, logcat on
    // Android and the browser console (and the on-page overlay) on the web --
    // the only sink that exists on every target.
    crash_dump_write(stderr, reason);

#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
    // Desktop keeps the file copy: it survives the terminal scrollback.
    // Skipped where the "filesystem" is a virtual one nobody can open.
    {
        FILE *f = fopen("logs/crash_dump.txt", "w");
        if (!f) f = fopen("crash_dump.txt", "w");
        if (f) {
            crash_dump_write(f, reason);
            fclose(f);
            fprintf(stderr, "[crash] also wrote logs/crash_dump.txt\n");
            fflush(stderr);
        }
    }
#endif
    busy = 0;
}
