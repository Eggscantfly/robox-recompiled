// sdk/robox_io.c -- Robox (WiiWare) file I/O HLE.
//
// Two subsystems, both backed by the host filesystem:
//
//  * CNT (content library): the game's asset path. On hardware the four
//    content archives (#2,#5,#4,#6, U8 format) live in the title's NAND
//    contents; the game reads whole files through contentOpenNAND /
//    contentReadNAND / contentGetLengthNAND / contentCloseNAND with
//    "/"-rooted archive paths ("/script/game.lua", "/music/robox.pcm").
//    Here every archive maps onto the extracted Assets/ tree.
//
//  * NAND (save data): slotN.dat (0x2800 bytes) + banner files, written
//    under "/tmp" then moved home. Backed by a "nand/" directory next to
//    the executable.
//
// Guest structs are opaque to the game (it only passes them back into
// this API), so we store our own host-slot index inside them:
//   CNTFileInfo:  +0x00 handleVA, +0x04 host slot, +0x08 len, +0x0c pos,
//                 +0x34 len (type-2 mirror), +0x3c pos, +0x40 type=1
//   CNTHandle:    +0x00 magic 0x0020af30 (guest code 0x80198af0 validates
//                 this directly!), +0x24 type=1, +0x28 contentNum (ours)
//   NANDFileInfo: +0x00 host slot, +0x04 magic 0x4E414E44 'NAND'
//
// Return conventions (verified against the game's own call sites):
//   contentOpenNAND        0 ok / negative error
//   contentReadNAND        bytes read / negative error
//   contentGetLengthNAND   length (no error path)
//   contentCloseNAND       0
//   NANDOpen/Close/Create/Delete/GetLength/GetType  0 ok / -12 NOEXISTS
//   NANDRead/NANDWrite     bytes / negative error

#include "hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <direct.h>
#  define rio_mkdir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  define rio_mkdir(p) mkdir(p, 0755)
#endif

// On the web the "nand" directory is an IDBFS mount (see tools/web_shell.html):
// the writes below still go through plain stdio, but the bytes only reach
// IndexedDB when someone syncs the mount. Publishing is async and runs on the
// browser main thread -- that is where both the filesystem and IndexedDB live,
// and it means the guest never blocks on a storage transaction.
//
// Only ever called once a save file is CLOSED. IDBFS's own autoPersist would
// publish on every mutation, including the truncate inside NANDCreate, which
// can leave a 0-byte slot over a good save if the tab dies in between. A save
// that vanishes is a bug; a save that comes back empty is a worse one.
#if defined(__EMSCRIPTEN__)
#  include <emscripten/em_asm.h>
#  include <emscripten/threading.h>
#  define ROBOX_NAND_PUBLISH()                                   \
      MAIN_THREAD_ASYNC_EM_ASM({                                 \
          if (typeof roboxNandFlush === 'function')              \
              roboxNandFlush();                                  \
      })
#else
#  define ROBOX_NAND_PUBLISH() ((void)0)
#endif

#define CNT_MAGIC        0x0020af30u
#define NAND_MAGIC       0x4E414E44u
#define NAND_RESULT_NOEXISTS   (-12)
#define NAND_RESULT_FATAL      (-128)
#define CNT_RESULT_NOEXISTS    (-12)

// ---------------------------------------------------------------------------
// host file slot tables
// ---------------------------------------------------------------------------

#define RIO_MAX_FILES 64

typedef struct {
    FILE    *fp;
    uint32_t size;
    char     path[260];
} rio_file_t;

static rio_file_t g_cnt_files[RIO_MAX_FILES];
static rio_file_t g_nand_files[RIO_MAX_FILES];

static int rio_slot_alloc(rio_file_t *tab) {
    for (int i = 0; i < RIO_MAX_FILES; ++i)
        if (!tab[i].fp) return i;
    return -1;
}

// Resolve the Assets root once: when run from build/ it's "Assets",
// when run from the repo root it's also "Assets" (copied), fall back to
// "../Assets". Overridable with RECOMP_ASSETS.
static const char *assets_root(void) {
    static char root[260];
    if (root[0]) return root;
    const char *env = getenv("RECOMP_ASSETS");
    if (env && *env) { snprintf(root, sizeof root, "%s", env); return root; }
    FILE *probe = fopen("Assets/script/game.lua", "rb");
    if (probe) { fclose(probe); snprintf(root, sizeof root, "Assets"); return root; }
    probe = fopen("../Assets/script/game.lua", "rb");
    if (probe) { fclose(probe); snprintf(root, sizeof root, "../Assets"); return root; }
    snprintf(root, sizeof root, "Assets");   // last resort; opens will log misses
    return root;
}

static void nand_root_prepare(void) {
    static int done;
    if (done) return;
    done = 1;
    rio_mkdir("nand");
    rio_mkdir("nand/tmp");
}

/* Exposed for the controls menu, which writes nand/controls.cfg: it needs the
 * directory to exist before the guest has ever saved, and it needs the same
 * publish-to-IndexedDB step the guest's saves use or the rebind is lost on
 * reload. Both are no-ops off the web build except for the mkdir. */
void robox_nand_prepare(void) { nand_root_prepare(); }
void robox_nand_publish(void) { ROBOX_NAND_PUBLISH(); }

// join + normalize: strip leading '/', forbid "..", convert to host path.
// For NAND paths the "/tmp/" staging dir is collapsed into the home dir:
// the game writes saves to /tmp/slotN.dat and reads them back as
// /slotN.dat (hardware moves them via a commit step we don't model).
static void rio_join(char *out, size_t cap, const char *root, const char *guest_path) {
    while (*guest_path == '/') ++guest_path;
    if (strcmp(root, "nand") == 0 && strncmp(guest_path, "tmp/", 4) == 0)
        guest_path += 4;
    snprintf(out, cap, "%s/%s", root, guest_path);
}

// ---------------------------------------------------------------------------
// CNT — content archives -> Assets/
// ---------------------------------------------------------------------------

// contentInitHandleNAND(contentNum r3, CNTHandle* r4, MEMAllocator* r5)
void hle_contentInitHandleNAND(void) {
    uint32_t num    = HLE_ARG_U32(0);
    uint32_t handle = HLE_ARG_U32(1);
    if (handle) {
        MEM_W32(handle + 0x00, CNT_MAGIC);
        MEM_W8 (handle + 0x24, 1);
        MEM_W32(handle + 0x28, num);
    }
    fprintf(stderr, "[CNT] contentInitHandleNAND(#%u, handle=0x%08x) -> 0\n", num, handle);
    HLE_RET(0);
}

// contentOpenNAND(CNTHandle* r3, const char* path r4, CNTFileInfo* r5)
void hle_contentOpenNAND(void) {
    uint32_t handle = HLE_ARG_U32(0);
    const char *path = HLE_STR(1);
    uint32_t info   = HLE_ARG_U32(2);
    static int log_count;

    if (!path || !info || !path[0]) {
        fprintf(stderr, "[CNT] open with bad args: handle=0x%08x path_va=0x%08x ('%s') info=0x%08x\n",
                handle, HLE_ARG_U32(1), path ? path : "(null)", info);
        HLE_RET(CNT_RESULT_NOEXISTS);
        return;
    }

    /* MOD #1 (music): when mods/robox.dls exists, the host DLS sampler is
     * the music synth — hide the game's wavetable + samples so its own
     * engine can't initialize and double-play. Delete/rename the mods
     * folder to restore the original music path; Assets/ is untouched. */
    {
        extern int robox_mod_music_active(void);
        if (robox_mod_music_active() &&
            (strstr(path, "robox.wt") || strstr(path, "robox.pcm"))) {
            fprintf(stderr, "[MOD] hiding '%s' (music mod active)\n", path);
            HLE_RET(CNT_RESULT_NOEXISTS);
            return;
        }
    }

    /* Note which level is loading. Only a log line and a cross-check now: the
     * host reads the game's own current-level word instead (0x801F7538, see
     * robox_level_is_robot in sdk/video.c), because this fires once per load
     * and so still says "04a" long after the player has walked back out. */
    {
        const char *dot = strrchr(path, '.');
        if (dot && !strcmp(dot, ".lev")) {
            const char *slash = strrchr(path, '/');
            const char *base  = slash ? slash + 1 : path;
            extern void robox_level_set(const char *name);
            robox_level_set(base);
        }
    }

    char host[300];
    /* MOD (Lua): the guest's own Lua reads every one of its files through
     * here -- script/game.lua and everything its dofile() pulls in after it.
     * That makes this the one place a mod can reach the game's embedded Lua
     * 5.1 without patching the DOL: the runtime serves a merged game.lua with
     * the guest-side scripts appended, and answers robox.host() calls, which
     * arrive as dofile() of a path that was never a file. NULL means "an
     * ordinary asset", which is every path but those. */
    {
        extern const char *robox_lua_guest_redirect(const char *guest_path);
        const char *served = robox_lua_guest_redirect(path);
        if (served) snprintf(host, sizeof host, "%s", served);
        else        rio_join(host, sizeof host, assets_root(), path);
    }
    FILE *fp = fopen(host, "rb");
    if (!fp) {
        fprintf(stderr, "[CNT] open MISS '%s' (host '%s')\n", path, host);
        HLE_RET(CNT_RESULT_NOEXISTS);
        return;
    }
    int slot = rio_slot_alloc(g_cnt_files);
    if (slot < 0) { fclose(fp); fprintf(stderr, "[CNT] open '%s': no free slots!\n", path); HLE_RET(-1); return; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    g_cnt_files[slot].fp = fp;
    g_cnt_files[slot].size = (uint32_t)sz;
    snprintf(g_cnt_files[slot].path, sizeof g_cnt_files[slot].path, "%s", path);

    MEM_W32(info + 0x00, handle);
    MEM_W32(info + 0x04, (uint32_t)slot);
    MEM_W32(info + 0x08, (uint32_t)sz);
    MEM_W32(info + 0x0c, 0);
    MEM_W32(info + 0x34, (uint32_t)sz);
    MEM_W32(info + 0x3c, 0);
    MEM_W8 (info + 0x40, 1);

    if (log_count < 200) {
        fprintf(stderr, "[CNT] open '%s' -> slot %d, %ld bytes\n", path, slot, sz);
        ++log_count;
    }
    HLE_RET(0);
}

// contentReadNAND(CNTFileInfo* r3, void* dst r4, s32 len r5)
void hle_contentReadNAND(void) {
    uint32_t info = HLE_ARG_U32(0);
    uint32_t dst  = HLE_ARG_U32(1);
    int32_t  len  = HLE_ARG_S32(2);
    static int log_count;

    uint32_t slot = MEM_R32(info + 0x04);
    if (MEM_R8(info + 0x40) != 1 || slot >= RIO_MAX_FILES || !g_cnt_files[slot].fp) {
        fprintf(stderr, "[CNT] read on bad info=0x%08x slot=%u\n", info, slot);
        HLE_RET(-1);
        return;
    }
    if (len < 0) { HLE_RET(-1); return; }
    uint32_t pos = MEM_R32(info + 0x0c);
    rio_file_t *f = &g_cnt_files[slot];
    fseek(f->fp, (long)pos, SEEK_SET);
    void *host_dst = ppc_host_ptr(dst);
    { extern void robox_hostwrite_check(const char *, uint32_t, uint32_t);
      robox_hostwrite_check("CNT-read", dst, (uint32_t)len); }
    size_t n = host_dst ? fread(host_dst, 1, (size_t)len, f->fp) : 0;
    // U8-archive semantics: files are 32-byte aligned inside the content,
    // so reading past a file's true end succeeds into padding on hardware.
    // Loaders read the aligned size and check for a full-length result
    // (Font.cpp panics on short reads). Zero-pad and report the full len.
    if (host_dst && n < (size_t)len) {
        memset((uint8_t *)host_dst + n, 0, (size_t)len - n);
        n = (size_t)len;
    }
    MEM_W32(info + 0x0c, pos + (uint32_t)n);
    MEM_W32(info + 0x3c, pos + (uint32_t)n);
    /* MOD (WAV music): remember where song files land in guest memory so the
     * SYNPrepare hook can name the buffer it is handed (sdk/robox_wav.c). */
    { extern void robox_wav_note_read(const char *path, uint32_t dst,
                                      uint32_t pos, uint32_t len, uint32_t size);
      robox_wav_note_read(f->path, dst, pos, (uint32_t)n, f->size); }
    if (log_count < 64 || (log_count & 0xFF) == 0) {
        fprintf(stderr, "[CNT] read '%s' pos=%u len=%d -> %zu\n", f->path, pos, len, n);
    }
    ++log_count;
    HLE_RET((int32_t)n);
}

// contentGetLengthNAND(CNTFileInfo* r3) -> length
void hle_contentGetLengthNAND(void) {
    uint32_t info = HLE_ARG_U32(0);
    HLE_RET(MEM_R32(info + 0x08));
}

// contentCloseNAND(CNTFileInfo* r3)
void hle_contentCloseNAND(void) {
    uint32_t info = HLE_ARG_U32(0);
    uint32_t slot = MEM_R32(info + 0x04);
    if (slot < RIO_MAX_FILES && g_cnt_files[slot].fp) {
        fclose(g_cnt_files[slot].fp);
        g_cnt_files[slot].fp = NULL;
    }
    HLE_RET(0);
}

// contentFastOpenNAND(entrynum flavor) — nothing in the game calls this
// directly; loud-stub so we notice if an indirect path reaches it.
void hle_contentFastOpenNAND(void) {
    fprintf(stderr, "[CNT] UNEXPECTED contentFastOpenNAND(r3=0x%08x r4=0x%08x r5=0x%08x)\n",
            HLE_ARG_U32(0), HLE_ARG_U32(1), HLE_ARG_U32(2));
    HLE_RET(-1);
}

// ---------------------------------------------------------------------------
// GX copy-scale helpers. The SDK's GXGetNumXfbLines runs an exact-float
// p/q search that never converges under translated float semantics (the
// low-bit divisibility check depends on hardware-exact fdivs). Semantics:
// number of XFB lines produced when copying efbHeight lines at yScale.
// ---------------------------------------------------------------------------

// GX draw-sync token writer at 0x80115b50: writes the PE token then spins
// on the SDA flag (0x801fb6d8) that only the PE token INTERRUPT sets on
// hardware. Serve the flag immediately; the FIFO interpreter renders
// synchronously so the sync point is already satisfied.
void hle_GXTokenSyncStub(void) {
    MEM_W8(0x801fb6d8u, 1);
    HLE_RET(HLE_ARG_U32(0));
}

// __OSReadSramViaIpc(buf r3, len r4, mode r5): the Wii SRAM/RTC compat
// shim does a raw IOS IPC transaction and spins on the ACK register we
// never assert. Serve zeros (sane defaults) and report success.
void hle_OSSramReadViaIpc(void) {
    uint32_t buf = HLE_ARG_U32(0);
    uint32_t len = HLE_ARG_U32(1);
    if (buf && len && len <= 0x1000) {
        uint8_t *p = (uint8_t *)ppc_host_ptr(buf);
        if (p) memset(p, 0, len);
    }
    fprintf(stderr, "[OS] SramReadViaIpc(buf=0x%08x len=%u mode=%u) -> zeros\n",
            buf, len, HLE_ARG_U32(2));
    HLE_RET(1);
}

void hle_GXGetNumXfbLines(void) {
    uint32_t efb_lines = HLE_ARG_U32(0);
    double   yscale    = HLE_ARG_F32(0);
    if (yscale < 1.0) yscale = 1.0;      // scale-down copies still emit >= efb lines? (scale>=1 typical)
    uint32_t lines = (uint32_t)(efb_lines * yscale + 0.5);
    if (lines > 1024u) lines = 1024u;
    fprintf(stderr, "[GX] GXGetNumXfbLines(%u, %.3f) -> %u\n",
            efb_lines, (float)yscale, lines);
    HLE_RET(lines);
}

// ---------------------------------------------------------------------------
// NAND — save files -> nand/
// ---------------------------------------------------------------------------

// NANDOpen(const char* path r3, NANDFileInfo* r4, u8 accType r5)
void hle_NANDOpen(void) {
    const char *path = HLE_STR(0);
    uint32_t info    = HLE_ARG_U32(1);
    uint32_t acc     = HLE_ARG_U32(2);

    nand_root_prepare();
    if (!path || !info) { HLE_RET(NAND_RESULT_FATAL); return; }
    char host[300];
    rio_join(host, sizeof host, "nand", path);
    // acc: 1=read, 2=write, 3=rw. NAND files must already exist (NANDCreate).
    const char *mode = (acc == 1) ? "rb" : "r+b";
    FILE *fp = fopen(host, mode);
    if (!fp) {
        fprintf(stderr, "[NAND] open MISS '%s' acc=%u\n", path, acc);
        HLE_RET(NAND_RESULT_NOEXISTS);
        return;
    }
    int slot = rio_slot_alloc(g_nand_files);
    if (slot < 0) { fclose(fp); HLE_RET(NAND_RESULT_FATAL); return; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    g_nand_files[slot].fp = fp;
    g_nand_files[slot].size = (uint32_t)sz;
    snprintf(g_nand_files[slot].path, sizeof g_nand_files[slot].path, "%s", path);
    MEM_W32(info + 0x00, (uint32_t)slot);
    MEM_W32(info + 0x04, NAND_MAGIC);
    fprintf(stderr, "[NAND] open '%s' acc=%u -> slot %d (%ld bytes)\n", path, acc, slot, sz);
    HLE_RET(0);
}

static rio_file_t *nand_info_file(uint32_t info) {
    if (!info || MEM_R32(info + 0x04) != NAND_MAGIC) return NULL;
    uint32_t slot = MEM_R32(info + 0x00);
    if (slot >= RIO_MAX_FILES || !g_nand_files[slot].fp) return NULL;
    return &g_nand_files[slot];
}

// NANDClose(NANDFileInfo* r3)
void hle_NANDClose(void) {
    rio_file_t *f = nand_info_file(HLE_ARG_U32(0));
    if (f) {
        fclose(f->fp); f->fp = NULL;
        // The game writes a whole slot and then closes it, so close is the
        // commit boundary -- the first moment the file on disk is a save
        // rather than a half-written one.
        fprintf(stderr, "[NAND] close '%s'\n", f->path);
        ROBOX_NAND_PUBLISH();
    }
    HLE_RET(0);
}

// NANDRead(NANDFileInfo* r3, void* buf r4, u32 len r5) -> bytes read
void hle_NANDRead(void) {
    rio_file_t *f = nand_info_file(HLE_ARG_U32(0));
    uint32_t dst = HLE_ARG_U32(1);
    uint32_t len = HLE_ARG_U32(2);
    if (!f) { HLE_RET(NAND_RESULT_FATAL); return; }
    void *host_dst = ppc_host_ptr(dst);
    { extern void robox_hostwrite_check(const char *, uint32_t, uint32_t);
      robox_hostwrite_check("NAND-read", dst, len); }
    size_t n = host_dst ? fread(host_dst, 1, len, f->fp) : 0;
    fprintf(stderr, "[NAND] read '%s' len=%u -> %zu\n", f->path, len, n);
    HLE_RET((int32_t)n);
}

// NANDWrite(NANDFileInfo* r3, const void* buf r4, u32 len r5) -> bytes written
void hle_NANDWrite(void) {
    rio_file_t *f = nand_info_file(HLE_ARG_U32(0));
    uint32_t src = HLE_ARG_U32(1);
    uint32_t len = HLE_ARG_U32(2);
    if (!f) { HLE_RET(NAND_RESULT_FATAL); return; }
    const void *host_src = ppc_host_ptr(src);
    size_t n = host_src ? fwrite(host_src, 1, len, f->fp) : 0;
    fflush(f->fp);
    fprintf(stderr, "[NAND] write '%s' len=%u -> %zu\n", f->path, len, n);
    HLE_RET((int32_t)n);
}

// NANDGetLength(NANDFileInfo* r3, u32* out r4)
void hle_NANDGetLength(void) {
    rio_file_t *f = nand_info_file(HLE_ARG_U32(0));
    uint32_t out = HLE_ARG_U32(1);
    if (!f) { HLE_RET(NAND_RESULT_FATAL); return; }
    long cur = ftell(f->fp);
    fseek(f->fp, 0, SEEK_END);
    long sz = ftell(f->fp);
    fseek(f->fp, cur, SEEK_SET);
    if (out) MEM_W32(out, (uint32_t)sz);
    HLE_RET(0);
}

// NANDGetType(const char* path r3, u8* type r4): 0 + *type when it exists
void hle_NANDGetType(void) {
    const char *path = HLE_STR(0);
    uint32_t out     = HLE_ARG_U32(1);
    nand_root_prepare();
    if (!path) { HLE_RET(NAND_RESULT_FATAL); return; }
    char host[300];
    rio_join(host, sizeof host, "nand", path);
    FILE *fp = fopen(host, "rb");
    fprintf(stderr, "[NAND] getType '%s' -> %s\n", path, fp ? "file" : "NOEXISTS");
    if (!fp) { HLE_RET(NAND_RESULT_NOEXISTS); return; }
    fclose(fp);
    if (out) MEM_W8(out, 1);   // 1 = file
    HLE_RET(0);
}

// NANDCreate(const char* path r3, u8 perm r4, u8 attr r5)
void hle_NANDCreate(void) {
    const char *path = HLE_STR(0);
    nand_root_prepare();
    if (!path) { HLE_RET(NAND_RESULT_FATAL); return; }
    char host[300];
    rio_join(host, sizeof host, "nand", path);
    FILE *fp = fopen(host, "wb");
    fprintf(stderr, "[NAND] create '%s' -> %s\n", path, fp ? "ok" : "FAIL");
    if (!fp) { HLE_RET(NAND_RESULT_FATAL); return; }
    fclose(fp);
    HLE_RET(0);
}

// NANDDelete(const char* path r3)
void hle_NANDDelete(void) {
    const char *path = HLE_STR(0);
    nand_root_prepare();
    if (!path) { HLE_RET(NAND_RESULT_FATAL); return; }
    char host[300];
    rio_join(host, sizeof host, "nand", path);
    int rc = remove(host);
    fprintf(stderr, "[NAND] delete '%s' -> %d\n", path, rc);
    // A deletion has to be published too, or an erased slot reappears on the
    // next load. No truncate hazard here: the file is already gone.
    if (rc == 0) ROBOX_NAND_PUBLISH();
    HLE_RET(rc == 0 ? 0 : NAND_RESULT_NOEXISTS);
}

// NANDCheck(u32 blocks r3, u32* out1 r4, u32* out2 r5): free-space /
// integrity check before saving. Report "plenty of space, all clear".
void hle_NANDCheck(void) {
    uint32_t blocks = HLE_ARG_U32(0);
    uint32_t out1   = HLE_ARG_U32(1);
    uint32_t out2   = HLE_ARG_U32(2);
    if (out1) MEM_W32(out1, 0);
    if (out2) MEM_W32(out2, 0);
    fprintf(stderr, "[NAND] check(blocks=%u) -> ok\n", blocks);
    HLE_RET(0);
}
