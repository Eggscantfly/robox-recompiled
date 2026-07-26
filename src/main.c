// src/main.c -- Host entry for the Robox (WiiWare) PC port.
//
// Pipeline:
//   1. Allocate guest memory.
//   2. Load "Robox USA.dol" into guest memory at its linked VAs.
//   3. Seed stack + WiiWare-channel low-memory boot block.
//   4. Initialize the HLE OS (arena bounds, low-mem globals).
//   5. Jump to __start. The recompiled crt0 sets r1/r2/r13 itself
//      (__init_registers runs; only __init_hardware is stubbed).

#include "runtime.h"
#include "../quirks/quirks.h"
#include "../sdk/hle.h"        /* robox_mkdir */
#include "../sdk/robox_setup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif
#include <signal.h>

// Give a crash dump somewhere to land.
//
// Normal runs send stderr to the null device (see main), so without this the
// post-mortem would be written straight into nothing. Called from the fatal
// paths only -- reopening here rather than keeping a log file open all the
// time is what lets a clean run leave no files behind at all.
//
// Idempotent, and deliberately does the least possible: a crash handler is not
// a good place to be allocating or locking.
static void crash_log_open(void) {
    static int done;
    if (done) return;
    done = 1;
    robox_mkdir("logs");
    freopen("logs/crash.log", "w", stderr);
}

static void on_fatal_signal(int sig) {
    crash_log_open();
    fprintf(stderr, "\n[signal] caught signal %d -- guest register snapshot:\n", sig);
    ppc_dump_registers();
    fflush(stderr);
    {   /* full post-mortem: registers, guest backtrace, recent guest
         * addresses and the last 64 frame times */
        extern void robox_prof_crash_dump(const char *reason);
        char why[32];
        snprintf(why, sizeof why, "signal %d", sig);
        robox_prof_crash_dump(why);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

#if defined(_WIN32)
static LONG WINAPI on_first_chance(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR kind = ep->ExceptionRecord->ExceptionInformation[0];
        ULONG_PTR va   = ep->ExceptionRecord->ExceptionInformation[1];
        HMODULE base = GetModuleHandleA(NULL);
        fprintf(stderr, "\n[first-chance-AV] tid=%lu host_pc=%p (exe+0x%llx) %s 0x%p\n",
                (unsigned long)GetCurrentThreadId(),
                ep->ExceptionRecord->ExceptionAddress,
                (unsigned long long)((char*)ep->ExceptionRecord->ExceptionAddress - (char*)base),
                kind == 0 ? "READ" : kind == 1 ? "WRITE" : "EXEC",
                (void*)va);
        fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI on_win32_exception(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    crash_log_open();
    fprintf(stderr, "\n[win32-seh] code=0x%08lx host_pc=%p\n",
            (unsigned long)code, ep->ExceptionRecord->ExceptionAddress);
    ppc_dump_registers();
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

extern const quirks_t quirks_robox;   // quirks/robox.c
extern void hle_OSInit(void);
extern void video_init(void);
extern void os_set_arena_bounds(uint32_t mem1_used_top,
                                uint32_t mem2_reserved_low,
                                uint32_t stack_reserve_bytes);

#ifndef DEFAULT_ELF_PATH
#define DEFAULT_ELF_PATH "Robox USA.dol"
#endif

// The game's own crt0 moves r1 to 0x802b9998 (bss_end + 64 KB) immediately,
// so this host-side seed only needs to cover the first few instructions.
#define GUEST_STACK_TOP   (PPC_MEM1_BASE + PPC_MEM1_SIZE - 0x100000u)

// WiiWare-channel boot block: what the NAND loader/apploader leaves in low
// MEM1 before __start. Robox reads console type, memory sizes and the
// BI2 debug flags from here.
static void setup_wii_boot_environment(void) {
    MEM_W32(0x80000000, 0x57524245u);   // game ID "WRBE" (WiiWare Robox USA)
    MEM_W32(0x80000004, 0x30310000u);   // maker "01"

    MEM_W32(0x80000020, 0x0d15ea5eu);   // boot magic
    MEM_W32(0x80000024, 0x00000001u);   // version
    MEM_W32(0x80000028, 0x01800000u);   // MEM1 size = 24 MB
    MEM_W32(0x8000002c, 0x00000023u);   // console type: retail Wii
    MEM_W32(0x80000034, 0x817FE000u);   // arena hi
    MEM_W32(0x800000F8, 0x0E7BE2C0u);   // bus speed
    MEM_W32(0x800000FC, 0x2B73A840u);   // CPU speed

    // BI2/apploader extension block.
    MEM_W16(0x800030E4, 0x0000u);       // PAD3 boot flag: none
    MEM_W16(0x800030E6, 0x0001u);       // console type PROD retail (checked by __start)
    MEM_W32(0x800030F0, 0x00000000u);   // ExecParams
    MEM_W32(0x80003104, 0x01800000u);   // simulated MEM1 size
    MEM_W32(0x80003118, 0x04000000u);   // physical MEM2 size = 64 MB
    MEM_W32(0x8000311C, 0x04000000u);   // simulated MEM2 size
    MEM_W32(0x80003120, 0x90000800u);   // MEM2 arena lo
    MEM_W32(0x80003124, 0x933E0000u);   // MEM2 arena hi
    MEM_W8 (0x80003184, 0x81u);         // AppType = NAND (channel) boot
}

// Watchdog: reports LIVE fps once the flip counter moves, flags FREEZE with
// a register dump when it stops, and dumps memory for post-mortem.
#if defined(_WIN32)
static DWORD WINAPI recomp_watchdog(LPVOID arg) {
    (void)arg;
    const char *off = getenv("RECOMP_WATCHDOG");
    if (off && off[0] == '0') return 0;
    extern volatile uint32_t g_flip_count;
    uint32_t last_flip = 0, last_lr = 0, stuck = 0;
    ULONGLONG last_flip_ms = GetTickCount64(), hb_ms = last_flip_ms;
    int frozen_reported = 0;
    for (;;) {
        Sleep(500);
        uint32_t cur_flip = g_flip_count;
        ULONGLONG now = GetTickCount64();
        if (cur_flip != last_flip) {
            if (now - hb_ms >= 2000) {
                double fps = (double)(cur_flip - last_flip) * 1000.0 / (double)(now - last_flip_ms);
                fprintf(stderr, "[LIVE] flips=%u (%.1f fps)\n", cur_flip, fps);
                fflush(stderr);
                hb_ms = now;
            }
            last_flip = cur_flip; last_flip_ms = now;
            frozen_reported = 0;
        } else if (cur_flip != 0 && !frozen_reported && (now - last_flip_ms) >= 3000) {
            frozen_reported = 1;
            fprintf(stderr, "\n[FREEZE] flips stalled at #%u for %llums; lr=0x%08x sp=0x%08x\n",
                    cur_flip, (unsigned long long)(now - last_flip_ms),
                    g_cpu.lr, g_cpu.gpr[1]);
            ppc_dump_registers();
        }
        uint32_t lr = g_cpu.lr;
        uint32_t d = (lr > last_lr) ? (lr - last_lr) : (last_lr - lr);
        if (d < 0x100u) ++stuck; else stuck = 0;
        last_lr = lr;
        if (stuck == 20) {
            fprintf(stderr, "[watchdog] guest lr stuck near 0x%08x; dumping registers + RAM\n", lr);
            ppc_dump_registers();
            extern uint8_t *g_mem1, *g_mem2;
            FILE *f = fopen("logs/mem1.bin", "wb");
            if (f) { fwrite(g_mem1, 1, 0x01800000, f); fclose(f); }
            f = fopen("logs/mem2.bin", "wb");
            if (f) { fwrite(g_mem2, 1, 0x04000000, f); fclose(f); }
            fflush(stderr);
        }
    }
}
#endif

static int robox_main(int argc, char **argv) {
#if defined(__ANDROID__)
    // Must run before anything touches the filesystem: extracts the packaged
    // game data out of the APK to internal storage and chdir()s there, so every
    // relative path in the engine resolves exactly as it does on desktop.
    extern void robox_android_bootstrap(void);
    robox_android_bootstrap();
#endif
#if defined(__3DS__)
    // Must run before ppc_load_image() below opens "Robox USA.dol": that is a
    // relative fopen, and the game data lives in the .3dsx's RomFS. This mounts
    // romfs, chdir()s into it, brings up gfx + the debug console (so stderr is
    // captured from here on), and switches the New 3DS to 804 MHz. Without it,
    // the DOL open fails on the SD root and main() returns straight to the
    // Homebrew Launcher / Citra list -- exactly the "loads then exits" symptom.
    extern void robox_3ds_bootstrap(void);
    robox_3ds_bootstrap();
#endif

    // The logs/ directory used to be created only on Windows while the freopen
    // below ran unconditionally. Per C99 the stream is closed whether or not
    // the reopen succeeds, so on every other platform that silently killed
    // stderr -- taking the signal handlers' register dumps with it. Create the
    // directory portably, and keep the terminal if the redirect fails.
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && !defined(__3DS__)
    // Normal runs are silent.
    //
    // The engine writes a great deal to stderr -- 430-odd call sites -- which
    // is exactly what you want while porting and pure noise once it works. It
    // goes to the null device unless RECOMP_LOG asks for it, so a clean run
    // leaves no logs/ directory behind at all.
    //
    // Crash output is NOT lost by this. The fatal handlers call
    // crash_log_open() to reopen stderr onto logs/crash.log before dumping, so
    // a post-mortem still reaches disk on a build that is otherwise quiet.
    //
    // Skipped entirely on Android (robox_android_bootstrap already points
    // stderr at logcat), Emscripten (stderr goes to the browser console, and a
    // file would land in MEMFS where nothing can read it), and 3DS (video_init
    // routes stderr to the bottom-screen console; a file would need SD-card
    // access this port deliberately avoids).
    if (getenv("RECOMP_LOG")) {
        robox_mkdir("logs");
        if (!freopen("logs/run.log", "w", stderr))
            fprintf(stdout, "[main] could not redirect stderr to logs/run.log\n");
    } else {
#if defined(_WIN32)
        freopen("NUL", "w", stderr);
#else
        freopen("/dev/null", "w", stderr);
#endif
    }
#endif

    // Note for the web target: these are installed but never fire. A wasm trap
    // is a thrown JS exception, not a signal -- sdk/robox_web_debug.c catches
    // it at the JS boundary instead.
    signal(SIGSEGV, on_fatal_signal);
    signal(SIGABRT, on_fatal_signal);
    signal(SIGILL,  on_fatal_signal);
    signal(SIGFPE,  on_fatal_signal);
#if defined(_WIN32)
    SetUnhandledExceptionFilter(on_win32_exception);
    AddVectoredExceptionHandler(1, on_first_chance);
#endif

    const char *image_path = (argc > 1) ? argv[1] : DEFAULT_ELF_PATH;

    quirks_set_active(&quirks_robox);
    ppc_runtime_init();

    // First-run setup (sdk/robox_setup.c). It has to run before the load below
    // because its whole job is to produce the file that load opens -- on a
    // fresh copy there is no DOL and no Assets/ yet.
    //
    // That is also why video_init() moved up here from its old spot after the
    // image load: the setup screen needs a window to draw in. The call further
    // down is now a no-op (it is guarded by g_video_inited), and video_init
    // touches no guest state, so nothing else cares which side of the load it
    // runs on.
    //
    // The splash deliberately did NOT move. It still plays further down, after
    // the game data is in place and just before the guest entry, so the port's
    // credit runs against a game that is actually about to start.
    //
    // Skipped when an explicit image path was given: someone passing their own
    // DOL on the command line has already done the setup's job by hand.
    if (argc <= 1) {
        video_init();
        if (robox_setup_run(".") != 0) {
            fprintf(stderr, "[main] setup cancelled -- nothing loaded\n");
            return 0;
        }
        // "Locate files manually" can put the DOL anywhere, so take the path
        // the setup resolved. NULL means it is in the working directory under
        // its usual name and DEFAULT_ELF_PATH already covers it. The Assets
        // half of that choice arrives via RECOMP_ASSETS, which robox_io.c
        // reads on its own.
        const char *located = robox_setup_dol_path();
        if (located) image_path = located;

        // video_init() read the control map before setup ran, and setup is
        // what writes controls.cfg on a first launch -- so that first run
        // would silently use built-in defaults and only pick the file up on
        // the next start. Re-read it now that it is definitely there.
        { extern void controls_load(void); controls_load(); }
    }

    uint32_t entry = 0;
    int load_rc = ppc_load_image(image_path, &entry);
    if (load_rc != 0) {
        fprintf(stderr, "[main] failed to load image: %s (rc=%d)\n", image_path, load_rc);
        return 1;
    }

    g_cpu.gpr[1] = GUEST_STACK_TOP;
    g_cpu.gpr[3] = 0;
    g_cpu.gpr[4] = 0;
    g_cpu.lr     = 0;

    // The crt0 (__init_registers) parks the game stack at bss_end + 0x10000
    // (r1 = 0x802b9998 for this DOL). The MEM1 arena must start ABOVE that
    // stack or the CW default heap hands out blocks underneath live frames
    // (proven: the iostream-Init ctor's console buffer wiped the ctor
    // walker's frames). +0x10000 covers the stack, +0x1000 is guard.
    os_set_arena_bounds(ppc_loaded_mem1_top() + 0x10000u + 0x1000u,
                        /*mem2_reserved_low =*/ 0x100000u,
                        /*stack_reserve     =*/ 0x100000u);

    hle_OSInit();
    setup_wii_boot_environment();

    video_init();

#if defined(_WIN32)
    CreateThread(NULL, 0, recomp_watchdog, NULL, 0, NULL);
#endif

    // Mod loader (sdk/robox_mods.c). Must run after the image is loaded so
    // the dispatch table is populated -- mods patch entries -- and before the
    // guest entry point.
    {
        extern void robox_mods_init(void);
        robox_mods_init();
    }
#if defined(__3DS__)
    // Runtime func-table patches for the 3DS (no regen). Currently: OSLockMutex
    // non-blocking, to break the ExpHeap-mutex boot deadlock. Must run after the
    // image is loaded (func table populated) and before the guest entry.
    { extern void robox_3ds_apply_patches(void); robox_3ds_apply_patches(); }
#endif

    // Splash animation. Here because it needs the GL context (video_init) but
    // must finish before the guest starts: it owns the screen and the audio
    // device outright, with nothing to interleave against.
    {
        extern void robox_splash_play(void);
        robox_splash_play();
    }

    PPC_Func entry_fn = ppc_lookup_func(entry);
    if (!entry_fn) {
        fprintf(stderr, "[main] entry 0x%08x not in function table\n", entry);
        return 2;
    }
    fprintf(stderr, "[main] jumping to entry 0x%08x\n", entry);
    fflush(stderr);
#if defined(__EMSCRIPTEN__)
    // Call the guest through a JS trampoline so a wasm trap is caught and
    // reported with the crashing guest address instead of silently killing
    // the worker and freezing the page.
    {
        extern void robox_web_watchdog_start(void);
        extern void robox_web_run_guest(PPC_Func entry);
        robox_web_watchdog_start();
        robox_web_run_guest(entry_fn);
    }
#else
    entry_fn();
#endif

    fprintf(stderr, "[main] guest returned from entry -- exiting cleanly\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Entry point.
//
// Android does not call main(): SDLActivity dlsym()s a symbol literally named
// "SDL_main" out of libmain.so and calls that. Normally SDL_main.h supplies it
// by doing `#define main SDL_main`, but this project defines SDL_MAIN_HANDLED
// (needed on desktop, where we own the real main), and that macro is precisely
// what suppresses the rename. The result was a library that loaded correctly
// and then died with:
//     E SDL: nativeRunMain(): Couldn't find function SDL_main in libmain.so
// i.e. a black screen and an immediate exit.
//
// Define the symbol explicitly rather than depending on header macro ordering.
// visibility("default") keeps it in the dynamic symbol table so dlsym can
// actually find it even under -fvisibility=hidden.
// ---------------------------------------------------------------------------
#if defined(__ANDROID__)
__attribute__((visibility("default")))
int SDL_main(int argc, char **argv) { return robox_main(argc, argv); }
#else
int main(int argc, char **argv) { return robox_main(argc, argv); }
#endif
