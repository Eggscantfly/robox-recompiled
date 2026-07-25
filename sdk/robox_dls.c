/* sdk/robox_dls.c -- port of build_soundfont.py's DLS writer. See the header
 * for why this runs on the user's machine instead of shipping a file.
 *
 * The layout below is not a clean-room DLS implementation; it reproduces the
 * Python byte for byte, quirks included, because the Python's output is what
 * the music mod has been tuned against. Two of those quirks are marked where
 * they occur -- they look like bugs and are load-bearing.
 */
#include "robox_dls.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <direct.h>
#  define dls_mkdir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  define dls_mkdir(p) mkdir((p), 0755)
#endif

static void set_err(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------------ */
/* growable output buffer                                                    */
/* ------------------------------------------------------------------------ */

typedef struct { uint8_t *p; size_t n, cap; int bad; } buf;

static void buf_reserve(buf *b, size_t extra)
{
    if (b->bad) return;
    if (b->n + extra <= b->cap) return;
    size_t cap = b->cap ? b->cap : 1u << 20;
    while (cap < b->n + extra) cap *= 2;
    uint8_t *q = (uint8_t *)realloc(b->p, cap);
    if (!q) { b->bad = 1; return; }
    b->p = q; b->cap = cap;
}

static void buf_raw(buf *b, const void *src, size_t n)
{
    buf_reserve(b, n);
    if (b->bad) return;
    memcpy(b->p + b->n, src, n);
    b->n += n;
}

static void buf_u8(buf *b, uint8_t v)   { buf_raw(b, &v, 1); }
static void buf_u16(buf *b, uint16_t v) { uint8_t t[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; buf_raw(b, t, 2); }
static void buf_u32(buf *b, uint32_t v)
{
    uint8_t t[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    buf_raw(b, t, 4);
}
static void buf_i16(buf *b, int16_t v)  { buf_u16(b, (uint16_t)v); }
static void buf_i32(buf *b, int32_t v)  { buf_u32(b, (uint32_t)v); }
static void buf_tag(buf *b, const char *t) { buf_raw(b, t, 4); }

/* A chunk whose size is only known once its body is written: emit the tag and
 * a placeholder, then patch the placeholder at the end. */
static size_t chunk_open(buf *b, const char *tag)
{
    buf_tag(b, tag);
    buf_u32(b, 0);
    return b->n;
}

static void chunk_close(buf *b, size_t body)
{
    if (b->bad) return;
    uint32_t size = (uint32_t)(b->n - body);
    uint8_t *at = b->p + body - 4;
    at[0] = (uint8_t)size; at[1] = (uint8_t)(size >> 8);
    at[2] = (uint8_t)(size >> 16); at[3] = (uint8_t)(size >> 24);
}

/* LIST is a chunk whose body starts with a four-character form type, and whose
 * declared size includes that form type. */
static size_t list_open(buf *b, const char *form)
{
    size_t body = chunk_open(b, "LIST");
    buf_tag(b, form);
    return body;
}
#define list_close(b, body) chunk_close((b), (body))

/* ------------------------------------------------------------------------ */
/* wavetable input                                                           */
/* ------------------------------------------------------------------------ */

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

typedef struct { uint32_t rate, offset, length; } pcm_entry;

typedef struct {
    uint8_t  root_key;
    int16_t  fine_tune;
    uint32_t loop_start, loop_len, inst_type, pcm_idx;
} wt_desc;

typedef struct {
    int      key_lo, key_hi;
    uint32_t pcm_idx;
    uint8_t  root_key;
    int16_t  fine_tune;
    uint32_t loop_start, loop_len;
    int      has_loop;
    int      release_tc;
} zone;

typedef struct {
    char  name[24];
    int   prog;          /* DLS program number                              */
    uint32_t ul_bank;    /* 0, or 0x80000000 for the drum bank              */
    int   release_tc;
    zone *zones;
    int   nzones;
} dls_inst;

static uint8_t *read_file(const char *path, size_t *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0) { fclose(f); return NULL; }
    rewind(f);
    uint8_t *p = (uint8_t *)malloc((size_t)n);
    if (!p) { fclose(f); return NULL; }
    if (fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    fclose(f);
    if (size_out) *size_out = (size_t)n;
    return p;
}

/* ------------------------------------------------------------------------ */

int robox_dls_build(const char *wt_path, const char *pcm_path,
                    const char *out_path, char *err, size_t err_size)
{
    int rc = -1;
    uint8_t *wt = NULL, *pcm = NULL;
    size_t wt_size = 0, pcm_size = 0;
    pcm_entry *pcms = NULL;
    wt_desc *descs = NULL;
    int *env_tc = NULL;
    dls_inst *insts = NULL;
    int n_insts = 0;
    int *pi_to_si = NULL;
    uint32_t *sorted_pi = NULL;
    buf out = { 0 };

    wt = read_file(wt_path, &wt_size);
    if (!wt) { set_err(err, err_size, "cannot read '%s'", wt_path); goto done; }
    pcm = read_file(pcm_path, &pcm_size);
    if (!pcm) { set_err(err, err_size, "cannot read '%s'", pcm_path); goto done; }

    /* Section table: big-endian offsets terminated by 0xFFFFFFFF. Slots 1..5
     * bound the keymap, descriptors, envelopes and PCM table. */
    uint32_t off[8];
    size_t n_off = 0;
    for (size_t i = 0; i + 4 <= wt_size && n_off < 8; i += 4) {
        uint32_t v = be32(wt + i);
        if (v == 0xFFFFFFFFu) break;
        off[n_off++] = v;
    }
    if (n_off < 6) {
        set_err(err, err_size, "robox.wt has %zu sections, expected at least 6", n_off);
        goto done;
    }
    for (size_t i = 1; i < 6; ++i) {
        if (off[i] > wt_size || off[i] < off[i - 1]) {
            set_err(err, err_size, "robox.wt section %zu is out of range", i);
            goto done;
        }
    }

    const uint8_t *sec_keymap = wt + off[1];
    const uint8_t *sec_desc   = wt + off[2];
    const uint8_t *sec_env    = wt + off[3];
    const uint8_t *sec_pcmtbl = wt + off[4];
    size_t keymap_len = off[2] - off[1];
    int num_desc = (int)((off[3] - off[2]) / 24);
    int num_env  = (int)((off[4] - off[3]) / 80);
    int num_pcm  = (int)((off[5] - off[4]) / 16);

    if (keymap_len < 128u * 256u) {
        set_err(err, err_size, "robox.wt keymap is short (%zu bytes)", keymap_len);
        goto done;
    }
    if (num_pcm <= 0 || num_desc <= 0) {
        set_err(err, err_size, "robox.wt has no descriptors or samples");
        goto done;
    }

    pcms = (pcm_entry *)calloc((size_t)num_pcm, sizeof *pcms);
    descs = (wt_desc *)calloc((size_t)num_desc, sizeof *descs);
    env_tc = (int *)calloc((size_t)(num_env > 0 ? num_env : 1), sizeof *env_tc);
    if (!pcms || !descs || !env_tc) { set_err(err, err_size, "out of memory"); goto done; }

    for (int i = 0; i < num_pcm; ++i) {
        const uint8_t *e = sec_pcmtbl + (size_t)i * 16;
        pcms[i].rate   = be32(e) & 0xFFFFu;
        pcms[i].offset = be32(e + 4);
        pcms[i].length = be32(e + 8);
    }
    for (int i = 0; i < num_desc; ++i) {
        const uint8_t *e = sec_desc + (size_t)i * 24;
        descs[i].root_key   = e[0];
        descs[i].fine_tune  = (int16_t)be16(e + 2);
        descs[i].loop_start = be32(e + 8);
        descs[i].loop_len   = be32(e + 12);
        descs[i].inst_type  = be32(e + 16);
        descs[i].pcm_idx    = be32(e + 20);
    }

    /* Envelope release, mapped from the raw word the same way the Python does:
     * a log curve fitted to milliseconds, then converted to timecents. int()
     * in Python truncates toward zero and so does this cast -- the values are
     * negative here (ms < 1000), so floor() would be off by one. */
    for (int i = 0; i < num_env; ++i) {
        int64_t w9 = (int32_t)be32(sec_env + (size_t)i * 80 + 36);
        if (w9 < 0) w9 = -w9;
        if (w9 < 1) w9 = 1;
        double ms = 1500.0 - (log((double)w9) - 10.3) * (1450.0 / 3.4);
        if (ms < 30.0) ms = 30.0;
        if (ms > 2000.0) ms = 2000.0;
        env_tc[i] = (int)(1200.0 * log2(ms / 1000.0));
    }

    /* Walk each program's keymap, coalescing runs of notes that point at the
     * same descriptor into one zone. */
    int *used = (int *)calloc((size_t)num_pcm, sizeof *used);
    insts = (dls_inst *)calloc(129, sizeof *insts);   /* +1 for the drum bank */
    if (!used || !insts) { free(used); set_err(err, err_size, "out of memory"); goto done; }

    int drum_inst = -1;
    for (int prog = 0; prog < 128; ++prog) {
        zone *zs = (zone *)calloc(128, sizeof *zs);
        if (!zs) { free(used); set_err(err, err_size, "out of memory"); goto done; }
        int nz = 0;

        int n = 0;
        while (n < 128) {
            uint16_t di = be16(sec_keymap + (size_t)prog * 256 + (size_t)n * 2);
            if (di == 0xFFFFu) { ++n; continue; }

            int hi = n;
            while (hi + 1 < 128 &&
                   be16(sec_keymap + (size_t)prog * 256 + (size_t)(hi + 1) * 2) == di)
                ++hi;

            if (di >= num_desc)              { n = hi + 1; continue; }
            const wt_desc *d = &descs[di];
            if (d->pcm_idx >= (uint32_t)num_pcm) { n = hi + 1; continue; }

            used[d->pcm_idx] = 1;

            zone *z = &zs[nz++];
            z->key_lo    = n;
            z->key_hi    = hi;
            z->pcm_idx   = d->pcm_idx;
            z->root_key  = d->root_key;
            z->fine_tune = d->fine_tune;
            z->loop_start = d->loop_start;
            z->loop_len   = d->loop_len;
            z->has_loop   = (d->loop_start > 0 && d->loop_len > 0);
            z->release_tc = (d->inst_type < (uint32_t)num_env) ? env_tc[d->inst_type] : 0;

            n = hi + 1;
        }

        if (!nz) { free(zs); continue; }

        dls_inst *ins = &insts[n_insts];
        int is_drum = (prog == 39);
        if (is_drum) {
            snprintf(ins->name, sizeof ins->name, "Drums");
            ins->prog = 39;          /* the melodic entry keeps program 39 */
            drum_inst = n_insts;
        } else {
            snprintf(ins->name, sizeof ins->name, "Prog%03d", prog);
            ins->prog = prog;
        }
        ins->ul_bank    = 0;
        ins->zones      = zs;
        ins->nzones     = nz;
        ins->release_tc = zs[0].release_tc;
        ++n_insts;
    }

    /* The drum program is emitted twice: once melodically on program 39, and
     * once more in the percussion bank so a channel-10 track finds it. */
    if (drum_inst >= 0) {
        dls_inst *ins = &insts[n_insts++];
        snprintf(ins->name, sizeof ins->name, "Drum Kit");
        ins->prog       = 0;
        ins->ul_bank    = 0x80000000u;
        ins->zones      = insts[drum_inst].zones;   /* shared, freed once */
        ins->nzones     = insts[drum_inst].nzones;
        ins->release_tc = insts[drum_inst].release_tc;
    }

    /* Sample indices, ascending, matching sorted(decoded.keys()). */
    sorted_pi = (uint32_t *)calloc((size_t)num_pcm, sizeof *sorted_pi);
    pi_to_si  = (int *)calloc((size_t)num_pcm, sizeof *pi_to_si);
    if (!sorted_pi || !pi_to_si) { free(used); set_err(err, err_size, "out of memory"); goto done; }
    int n_waves = 0;
    for (int i = 0; i < num_pcm; ++i) {
        pi_to_si[i] = -1;
        if (used[i]) { pi_to_si[i] = n_waves; sorted_pi[n_waves++] = (uint32_t)i; }
    }
    free(used);

    if (!n_waves) { set_err(err, err_size, "no samples referenced by any program"); goto done; }

    /* ---- wave pool ---------------------------------------------------- */

    uint32_t *wave_off = (uint32_t *)calloc((size_t)n_waves, sizeof *wave_off);
    if (!wave_off) { set_err(err, err_size, "out of memory"); goto done; }

    buf wvpl = { 0 };
    for (int si = 0; si < n_waves; ++si) {
        uint32_t pi = sorted_pi[si];
        const pcm_entry *pe = &pcms[pi];

        /* Root key, tuning and loop come from the FIRST descriptor naming this
         * sample -- including when that descriptor has no loop. Breaking on
         * the first match rather than the first looped match is what the
         * Python does, and changing it moves sample tuning. */
        uint8_t  def_root = 60;
        int16_t  def_ft = 0;
        uint32_t def_ls = 0, def_ll = 0;
        for (int i = 0; i < num_desc; ++i) {
            if (descs[i].pcm_idx != pi) continue;
            def_root = descs[i].root_key;
            def_ft   = descs[i].fine_tune;
            if (descs[i].loop_start > 0 && descs[i].loop_len > 0) {
                def_ls = descs[i].loop_start;
                def_ll = descs[i].loop_len;
            }
            break;
        }
        int has_loop = (def_ls > 0 && def_ll > 0);

        if ((uint64_t)pe->offset + pe->length > pcm_size) {
            free(wave_off); free(wvpl.p);
            set_err(err, err_size, "sample %u runs past the end of robox.pcm", pi);
            goto done;
        }

        wave_off[si] = (uint32_t)wvpl.n;

        size_t wave_body = list_open(&wvpl, "wave");

        size_t c = chunk_open(&wvpl, "fmt ");
        buf_u16(&wvpl, 1);                    /* PCM                        */
        buf_u16(&wvpl, 1);                    /* mono                       */
        buf_u32(&wvpl, pe->rate);
        buf_u32(&wvpl, pe->rate * 2);         /* bytes/sec at 16-bit mono   */
        buf_u16(&wvpl, 2);
        buf_u16(&wvpl, 16);
        buf_u16(&wvpl, 0);                    /* cbSize                     */
        chunk_close(&wvpl, c);

        c = chunk_open(&wvpl, "wsmp");
        buf_u32(&wvpl, 20);
        buf_u16(&wvpl, def_root);
        buf_i16(&wvpl, def_ft);
        buf_i32(&wvpl, 0);                    /* gain                       */
        buf_u32(&wvpl, 0);                    /* options                    */
        buf_u32(&wvpl, (uint32_t)(has_loop ? 1 : 0));
        if (has_loop) {
            buf_u32(&wvpl, 16);
            buf_u32(&wvpl, 0);                /* forward loop               */
            buf_u32(&wvpl, def_ls);
            buf_u32(&wvpl, def_ll);
        }
        chunk_close(&wvpl, c);

        /* The samples are signed 8-bit on disc; widen to 16-bit by <<8. */
        c = chunk_open(&wvpl, "data");
        buf_reserve(&wvpl, (size_t)pe->length * 2);
        if (wvpl.bad) { free(wave_off); free(wvpl.p); set_err(err, err_size, "out of memory"); goto done; }
        for (uint32_t i = 0; i < pe->length; ++i) {
            int16_t s = (int16_t)((int8_t)pcm[pe->offset + i] * 256);
            wvpl.p[wvpl.n++] = (uint8_t)s;
            wvpl.p[wvpl.n++] = (uint8_t)(s >> 8);
        }
        chunk_close(&wvpl, c);
        /* Python pads the sample data to an even length; 16-bit data never is
         * odd, so this never fires -- kept so the two cannot diverge. */
        if (((size_t)pe->length * 2) % 2) buf_u8(&wvpl, 0);

        list_close(&wvpl, wave_body);
    }

    if (wvpl.bad) { free(wave_off); free(wvpl.p); set_err(err, err_size, "out of memory"); goto done; }

    /* ---- assemble ------------------------------------------------------ */

    size_t riff = chunk_open(&out, "RIFF");
    buf_tag(&out, "DLS ");

    size_t c = chunk_open(&out, "colh");
    buf_u32(&out, (uint32_t)n_insts);
    chunk_close(&out, c);

    size_t lins = list_open(&out, "lins");
    for (int i = 0; i < n_insts; ++i) {
        const dls_inst *ins = &insts[i];
        size_t ins_body = list_open(&out, "ins ");

        c = chunk_open(&out, "insh");
        buf_u32(&out, (uint32_t)ins->nzones);
        buf_u32(&out, ins->ul_bank);
        buf_u32(&out, (uint32_t)ins->prog);
        chunk_close(&out, c);

        /* Instrument-level articulation: LFO rate, vibrato depth, and the
         * release time this program's envelope mapped to. */
        {
            int rtc = ins->release_tc;
            if (rtc < -12000) rtc = -12000;
            if (rtc > 8000)   rtc = 8000;

            size_t lart = list_open(&out, "lart");
            c = chunk_open(&out, "art1");
            buf_u32(&out, 8);
            buf_u32(&out, 3);
            buf_u16(&out, 0); buf_u16(&out, 0); buf_u16(&out, 0x0104); buf_u16(&out, 0);
            buf_i32(&out, 58272);
            buf_u16(&out, 0x0001); buf_u16(&out, 0x0081); buf_u16(&out, 0x0003); buf_u16(&out, 0);
            buf_i32(&out, 3276800);
            buf_u16(&out, 0); buf_u16(&out, 0); buf_u16(&out, 0x0209); buf_u16(&out, 0);
            buf_i32(&out, rtc);
            chunk_close(&out, c);
            list_close(&out, lart);
        }

        size_t lrgn = list_open(&out, "lrgn");
        for (int zi = 0; zi < ins->nzones; ++zi) {
            const zone *z = &ins->zones[zi];
            size_t rgn = list_open(&out, "rgn ");

            c = chunk_open(&out, "rgnh");
            buf_u16(&out, (uint16_t)z->key_lo);
            buf_u16(&out, (uint16_t)z->key_hi);
            buf_u16(&out, 0);
            buf_u16(&out, 127);
            buf_u16(&out, 0);
            buf_u16(&out, 0);
            chunk_close(&out, c);

            c = chunk_open(&out, "wsmp");
            buf_u32(&out, 20);
            buf_u16(&out, z->root_key);
            buf_i16(&out, z->fine_tune);
            buf_i32(&out, 0);
            buf_u32(&out, 0);
            buf_u32(&out, (uint32_t)(z->has_loop ? 1 : 0));
            if (z->has_loop) {
                buf_u32(&out, 16);
                buf_u32(&out, 0);
                buf_u32(&out, z->loop_start);
                buf_u32(&out, z->loop_len);
            }
            chunk_close(&out, c);

            c = chunk_open(&out, "wlnk");
            buf_u16(&out, 0);
            buf_u16(&out, 0);
            buf_u32(&out, 1);
            buf_u32(&out, (uint32_t)pi_to_si[z->pcm_idx]);
            chunk_close(&out, c);

            list_close(&out, rgn);
        }
        list_close(&out, lrgn);

        {
            size_t info = list_open(&out, "INFO");
            size_t name_len = strlen(ins->name) + 1;
            /* Padded to even, and the declared size includes the pad byte --
             * technically wrong (RIFF pads outside the size) but it is what
             * the Python emits and what the mod has always loaded. */
            size_t padded = name_len + (name_len % 2);
            buf_tag(&out, "INAM");
            buf_u32(&out, (uint32_t)padded);
            buf_raw(&out, ins->name, name_len);
            if (padded != name_len) buf_u8(&out, 0);
            list_close(&out, info);
        }

        list_close(&out, ins_body);
    }
    list_close(&out, lins);

    c = chunk_open(&out, "ptbl");
    buf_u32(&out, 8);
    buf_u32(&out, (uint32_t)n_waves);
    for (int i = 0; i < n_waves; ++i) buf_u32(&out, wave_off[i]);
    chunk_close(&out, c);

    buf_tag(&out, "LIST");
    buf_u32(&out, (uint32_t)(4 + wvpl.n));
    buf_tag(&out, "wvpl");
    buf_raw(&out, wvpl.p, wvpl.n);

    chunk_close(&out, riff);

    free(wave_off);
    free(wvpl.p);

    if (out.bad) { set_err(err, err_size, "out of memory"); goto done; }

    FILE *f = fopen(out_path, "wb");
    if (!f) { set_err(err, err_size, "cannot write '%s'", out_path); goto done; }
    if (fwrite(out.p, 1, out.n, f) != out.n) {
        fclose(f);
        set_err(err, err_size, "short write on '%s' (disk full?)", out_path);
        goto done;
    }
    fclose(f);

    fprintf(stderr, "[dls] built %s: %d instruments, %d samples, %zu bytes\n",
            out_path, n_insts, n_waves, out.n);
    rc = 0;

done:
    /* The drum bank shares the drum program's zone array; free it once. */
    if (insts) {
        for (int i = 0; i < n_insts; ++i) {
            if (i > 0 && insts[i].zones == insts[i - 1].zones) continue;
            if (drum_inst >= 0 && i == n_insts - 1 &&
                insts[i].zones == insts[drum_inst].zones) continue;
            free(insts[i].zones);
        }
        free(insts);
    }
    free(sorted_pi);
    free(pi_to_si);
    free(env_tc);
    free(descs);
    free(pcms);
    free(pcm);
    free(wt);
    free(out.p);
    return rc;
}

/* ------------------------------------------------------------------------ */

int robox_dls_ensure(const char *dir, const char *assets_dir,
                     char *err, size_t err_size)
{
    char dls[1024], wt[1100], pcm[1100], mods[1024], assets[1024];
    if (!dir || !dir[0]) dir = ".";

    snprintf(dls, sizeof dls, "%s/mods/robox.dls", dir);
    FILE *f = fopen(dls, "rb");
    if (f) { fclose(f); return 0; }

    /* Resolve the Assets root the same way robox_io.c does, so a tree the user
     * pointed at through the setup is found here too. */
    if (assets_dir && assets_dir[0]) {
        snprintf(assets, sizeof assets, "%s", assets_dir);
    } else {
        const char *env = getenv("RECOMP_ASSETS");
        if (env && *env) snprintf(assets, sizeof assets, "%s", env);
        else             snprintf(assets, sizeof assets, "%s/Assets", dir);
    }

    snprintf(wt,  sizeof wt,  "%s/music/robox.wt",  assets);
    snprintf(pcm, sizeof pcm, "%s/music/robox.pcm", assets);

    f = fopen(wt, "rb");
    if (!f) {
        set_err(err, err_size, "%s is missing", wt);
        return -1;
    }
    fclose(f);
    f = fopen(pcm, "rb");
    if (!f) {
        set_err(err, err_size, "%s is missing", pcm);
        return -1;
    }
    fclose(f);

    snprintf(mods, sizeof mods, "%s/mods", dir);
    dls_mkdir(mods);

    return robox_dls_build(wt, pcm, dls, err, err_size);
}
