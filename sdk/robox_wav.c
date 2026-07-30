// sdk/robox_wav.c -- MOD: music packs (WAV / OGG / RBXS).
//
// Streams a replacement audio file in place of a game song, per song, driven
// by mods/wav_music.cfg:
//
//     comienzo.jul = "C:\Music\my title theme.rbxs"
//     bosque.dan   = mods/wavs/forest.ogg
//     * = mods/wavs/everything.wav   # any song without a line of its own
//     volume = 80                    # 0..100, gain for replacement music
//
// The file's CONTENT decides the format, not its extension:
//   RIFF -> WAV (PCM 8/16/24/32 or float32, mono/stereo, any rate), loops whole
//   OggS -> OGG Vorbis (stb_vorbis; any channels, downmixed), loops whole
//   RBXS -> Robox sound header + a WAV or OGG payload. Fixed 24-byte
//           header, little-endian, the smiley ALWAYS at offset 0xE:
//             0x00  "RBXS"
//             0x04  u32 sample rate    0 = use the audio's own rate
//             0x08  u32 channels       1 = downmix to mono, else as-is
//             0x0C  u16 loop flag      0 = play once, 1 = loop
//             0x0E  ":P"               the header must smile. No :P, no song.
//             0x10  u32 loop start     both points always present, in
//             0x14  u32 loop end       frames; ignored unless looping.
//                                      0,0 = whole song; end 0 = to the end.
//                                      The intro before loop start plays
//                                      once, then the loop region repeats.
//             0x18  payload (OGG or WAV bytes) starts right here
//           Build these with tools/make_rbxs.py.
//
// How a song buffer gets its name back: the game loads each song file
// through the CNT path (robox_io.c) into a guest buffer and hands that
// buffer's VA to SYNPrepare (hooked -> robox_midi_load). The CNT reads are
// recorded here as [va, va+size) -> basename; robox_midi_load asks us first.
// If the buffer matches a mapped song, the replacement streams and fmidi
// never starts for it; if not (or the file fails to open/parse), the normal
// MIDI + DLS path plays exactly as before.
//
// The decoder streams from disk -- no full preload, so album-length files
// cost a staging buffer, not tens of MB (matters on 3DS/Android). Everything
// runs on the emu thread (the same one that pumps ax_mixer_frame and the SYN
// hooks), so there is no locking.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"      /* implementation: sdk/stb_vorbis.c own TU */

extern int   robox_mod_enabled(const char *id); /* sdk/robox_mods.c */
extern void *ppc_host_ptr(uint32_t va);

// ---------------------------------------------------------------------------
// config: song name -> replacement path
// ---------------------------------------------------------------------------

#define WAV_MAX_SONGS 32

typedef struct {
    char song[64];     /* lowercased basename, e.g. "comienzo.jul" */
    char path[260];
} wav_map_t;

static wav_map_t g_map[WAV_MAX_SONGS];
static unsigned  g_map_n;
static char      g_default_path[260];   /* '*' entry */
static float     g_gain = 0.80f;        /* volume = 80 default */
static int       g_on;                  /* set by robox_wav_init */

static void lower_copy(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < cap; ++i) dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = '\0';
}

/* Match a lowercased basename against a config key: exact, or key without
 * an extension against the name's stem ("comienzo" matches "comienzo.jul"). */
static int key_matches(const char *key, const char *base) {
    if (!strcmp(key, base)) return 1;
    if (!strchr(key, '.')) {
        const char *dot = strrchr(base, '.');
        size_t stem = dot ? (size_t)(dot - base) : strlen(base);
        return strlen(key) == stem && !strncmp(key, base, stem);
    }
    return 0;
}

static const char *path_for_song(const char *base_lc) {
    for (unsigned i = 0; i < g_map_n; ++i)
        if (key_matches(g_map[i].song, base_lc)) return g_map[i].path;
    return g_default_path[0] ? g_default_path : NULL;
}

/* Map a song at runtime (robox.audio.music_set), for a mod that produced its
 * audio after startup -- a download, say. wav_music.cfg is read once in
 * robox_wav_init, so without this a mod's file could never be reached.
 *
 * OVERWRITES a matching key rather than appending, because path_for_song
 * returns the FIRST match: an appended duplicate would be dead, which is
 * exactly the trap the config file has. `path` NULL removes the entry, so a
 * mod can hand a song back to the game.
 *
 * Takes effect on the next song start, not mid-playback. */
int robox_wav_set_mapping(const char *song, const char *path) {
    if (!song || !*song) return 0;
    char key[64];
    lower_copy(key, sizeof key, song);

    if (!strcmp(key, "*")) {
        snprintf(g_default_path, sizeof g_default_path, "%s", path ? path : "");
        return 1;
    }
    for (unsigned i = 0; i < g_map_n; ++i) {
        if (!key_matches(g_map[i].song, key) && strcmp(g_map[i].song, key)) continue;
        if (path) {
            snprintf(g_map[i].path, sizeof g_map[i].path, "%s", path);
        } else {                       /* remove: shuffle the tail down */
            for (unsigned k = i + 1; k < g_map_n; ++k) g_map[k - 1] = g_map[k];
            --g_map_n;
        }
        return 1;
    }
    if (!path) return 1;               /* nothing to remove */
    if (g_map_n >= WAV_MAX_SONGS) return 0;
    lower_copy(g_map[g_map_n].song, sizeof g_map[g_map_n].song, key);
    snprintf(g_map[g_map_n].path, sizeof g_map[g_map_n].path, "%s", path);
    ++g_map_n;
    return 1;
}

void robox_wav_init(void) {
    FILE *f = fopen("mods/wav_music.cfg", "r");
    if (!f) return;                     /* registry forces us off anyway */
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';

        char *k = line;
        while (*k && isspace((unsigned char)*k)) ++k;
        char *end = k + strlen(k);
        while (end > k && isspace((unsigned char)end[-1])) *--end = '\0';
        if (!*k) continue;

        char *v = eq + 1;
        while (*v && isspace((unsigned char)*v)) ++v;
        end = v + strlen(v);
        while (end > v && isspace((unsigned char)end[-1])) *--end = '\0';
        if (end - v >= 2 && (*v == '"' || *v == '\'') && end[-1] == *v) {
            *--end = '\0';
            ++v;
        }
        if (!*v) continue;

        if (!strcmp(k, "volume")) {
            int vol = atoi(v);
            if (vol < 0) vol = 0;
            if (vol > 100) vol = 100;
            g_gain = (float)vol / 100.0f;
        } else if (!strcmp(k, "*")) {
            snprintf(g_default_path, sizeof g_default_path, "%s", v);
        } else if (g_map_n < WAV_MAX_SONGS) {
            lower_copy(g_map[g_map_n].song, sizeof g_map[g_map_n].song, k);
            snprintf(g_map[g_map_n].path, sizeof g_map[g_map_n].path, "%s", v);
            ++g_map_n;
        }
    }
    fclose(f);
    g_on = 1;
    fprintf(stderr, "[WAV] wav_music.cfg: %u song%s mapped%s, volume=%d\n",
            g_map_n, g_map_n == 1 ? "" : "s",
            g_default_path[0] ? " + '*' default" : "",
            (int)(g_gain * 100.0f + 0.5f));
    fflush(stderr);
}

// ---------------------------------------------------------------------------
// song-buffer tracking: which CNT file landed where in guest memory
// ---------------------------------------------------------------------------

#define WAV_TRACK 16
#define WAV_FP_BYTES 64

typedef struct {
    uint32_t lo, hi;    /* guest VA range the file occupies */
    uint32_t fp;        /* FNV-1a of the file's first fp_len bytes */
    uint32_t fp_len;
    char     song[64];
} wav_seen_t;

static wav_seen_t g_seen[WAV_TRACK];
static unsigned   g_seen_next;

static uint32_t fnv1a(const uint8_t *p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* Called from hle_contentReadNAND for every CNT read. `pos` is the file
 * offset of this chunk, so lo = dst - pos is the buffer base even when the
 * game reads a file in pieces.
 *
 * The fingerprint guards against guest heap reuse: leaving a level frees its
 * song buffer, and the next song (e.g. the title theme) can be loaded at the
 * SAME guest VA. The recorded name would then misattach to whatever plays
 * there now -- the WAV of the old song restarting over the new one. Hashing
 * the file's opening bytes at read time lets robox_wav_music_start prove the
 * buffer still holds the song it was recorded as. */
void robox_wav_note_read(const char *path, uint32_t dst, uint32_t pos,
                         uint32_t len, uint32_t size) {
    if (!g_on || !path || pos > dst || !size) return;
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    char base_lc[64];
    lower_copy(base_lc, sizeof base_lc, base);

    /* Only remember files we could ever remap: an explicitly mapped name,
     * or (with a '*' default) anything under music/. */
    int mapped = 0;
    for (unsigned i = 0; i < g_map_n && !mapped; ++i)
        mapped = key_matches(g_map[i].song, base_lc);
    if (!mapped && !(g_default_path[0] && strstr(path, "music/"))) return;

    uint32_t lo = dst - pos;
    uint32_t fp = 0, fp_len = 0;
    if (pos == 0) {                      /* first chunk: bytes 0..len are in */
        fp_len = size < WAV_FP_BYTES ? size : WAV_FP_BYTES;
        if (fp_len > len) fp_len = len;
        const uint8_t *p = fp_len ? (const uint8_t *)ppc_host_ptr(dst) : NULL;
        if (p) fp = fnv1a(p, fp_len);
        else   fp_len = 0;
    }

    for (unsigned i = 0; i < WAV_TRACK; ++i) {
        if (g_seen[i].hi && !strcmp(g_seen[i].song, base_lc)) {
            g_seen[i].lo = lo;
            g_seen[i].hi = lo + size;
            if (pos == 0) { g_seen[i].fp = fp; g_seen[i].fp_len = fp_len; }
            return;
        }
    }
    wav_seen_t *s = &g_seen[g_seen_next++ % WAV_TRACK];
    s->lo = lo;
    s->hi = lo + size;
    s->fp = fp;
    s->fp_len = fp_len;
    snprintf(s->song, sizeof s->song, "%s", base_lc);
}

// ---------------------------------------------------------------------------
// audio stream (WAV or OGG payload, optional RBXS header on top)
// ---------------------------------------------------------------------------

enum { SRC_NONE = 0, SRC_WAV, SRC_OGG };
enum { LOOP_ALL = 0, LOOP_POINTS, LOOP_NONE };   /* LOOP_ALL = whole song */

static struct {
    FILE       *fp;         /* WAV: owned here. OGG: owned by stb_vorbis.  */
    stb_vorbis *ogg;
    int         kind;       /* SRC_*                                       */
    /* playback format */
    uint32_t rate;          /* rate the resampler treats frames as        */
    uint16_t chans;         /* frames in staging: 1..2 (OGG always 2)     */
    uint16_t force_mono;    /* RBXS channels=1: average L+R               */
    /* WAV payload specifics */
    uint32_t data_off;      /* absolute file offset of payload PCM        */
    uint16_t bits, isfloat;
    uint32_t frame_bytes;
    /* position / looping, all in frames */
    uint32_t frames;        /* total frames in the payload                */
    uint32_t frame_cur;     /* frames fetched from the source so far      */
    int      loop_mode;
    uint32_t loop_start, loop_end;
    int      at_end;        /* LOOP_NONE: played cleanly to the end       */
    /* staging */
    uint8_t  buf[16384];    /* WAV: raw file bytes, whole frames          */
    uint32_t buf_off, buf_len;
    int16_t  pcm[8192];     /* OGG: decoded interleaved s16               */
    uint32_t pcm_off, pcm_frames;
    /* resampler */
    double   acc;
    float    prevL, prevR, curL, curR;
    int      playing, paused;
    int      direct;        /* started by a mod, not by the game's SYNPrepare */
    char     song[64];
} g_s;

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

void robox_wav_stop(void) {
    if (g_s.ogg) stb_vorbis_close(g_s.ogg);   /* closes its FILE too */
    else if (g_s.fp) fclose(g_s.fp);
    memset(&g_s, 0, sizeof g_s);
}

/* The frame index playback wraps (or ends) at. */
static uint32_t wrap_end(void) {
    if (g_s.loop_mode == LOOP_POINTS && g_s.loop_end &&
        g_s.loop_end <= g_s.frames)
        return g_s.loop_end;
    return g_s.frames;
}

static uint32_t wrap_start(void) {
    uint32_t ls = g_s.loop_mode == LOOP_POINTS ? g_s.loop_start : 0;
    return ls < wrap_end() ? ls : 0;    /* degenerate points: whole song */
}

/* One channel's WAV sample -> int16 full-scale float. */
static float wav_sample(const uint8_t *p) {
    switch (g_s.bits) {
    case 8:  return (float)(((int)p[0] - 128) << 8);
    case 16: return (float)(int16_t)rd16le(p);
    case 24: {
        int32_t v = (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) |
                              ((uint32_t)p[2] << 24)) >> 8;
        return (float)v / 256.0f;
    }
    case 32:
        if (g_s.isfloat) {
            union { uint32_t u; float f; } c;
            c.u = rd32le(p);
            return c.f * 32767.0f;
        }
        return (float)(int32_t)rd32le(p) / 65536.0f;
    default: return 0.0f;
    }
}

static void wav_refill(void) {
    g_s.buf_off = g_s.buf_len = 0;
    if (!g_s.fp) return;
    uint32_t we = wrap_end();
    if (g_s.frame_cur >= we) {
        if (g_s.loop_mode == LOOP_NONE) { g_s.at_end = 1; return; }
        uint32_t ls = wrap_start();
        fseek(g_s.fp, (long)(g_s.data_off + ls * (uint64_t)g_s.frame_bytes),
              SEEK_SET);
        g_s.frame_cur = ls;
        we = wrap_end();
    }
    uint32_t want = (uint32_t)(sizeof g_s.buf / g_s.frame_bytes);
    if (want > we - g_s.frame_cur) want = we - g_s.frame_cur;
    size_t got = fread(g_s.buf, g_s.frame_bytes, want, g_s.fp);
    g_s.frame_cur += (uint32_t)got;
    g_s.buf_len = (uint32_t)(got * g_s.frame_bytes);
}

static void ogg_refill(void) {
    g_s.pcm_off = g_s.pcm_frames = 0;
    if (!g_s.ogg) return;
    for (int attempt = 0; attempt < 2; ++attempt) {
        uint32_t we = wrap_end();
        if (g_s.frame_cur >= we) {
            if (g_s.loop_mode == LOOP_NONE) { g_s.at_end = 1; return; }
            uint32_t ls = wrap_start();
            stb_vorbis_seek(g_s.ogg, ls);
            g_s.frame_cur = ls;
            we = wrap_end();
        }
        uint32_t cap = (uint32_t)(sizeof g_s.pcm / sizeof g_s.pcm[0]) / g_s.chans;
        uint32_t want = we - g_s.frame_cur;
        if (want > cap) want = cap;
        int n = stb_vorbis_get_samples_short_interleaved(
                    g_s.ogg, g_s.chans, g_s.pcm, (int)(want * g_s.chans));
        if (n > 0) {
            g_s.pcm_frames = (uint32_t)n;
            g_s.frame_cur += (uint32_t)n;
            return;
        }
        /* Decoder dry before the declared boundary (the stream's length and
         * its last packet can disagree by a hair): act as if we reached it,
         * so the next pass loops or ends. Twice dry = a real failure. */
        g_s.frame_cur = we;
    }
}

/* 1 = frame produced, 0 = clean end (no loop), -1 = read/decode failure. */
static int src_next_frame(float *L, float *R) {
    if (g_s.kind == SRC_WAV) {
        if (g_s.buf_off + g_s.frame_bytes > g_s.buf_len) {
            wav_refill();
            if (g_s.buf_len < g_s.frame_bytes) return g_s.at_end ? 0 : -1;
        }
        const uint8_t *p = g_s.buf + g_s.buf_off;
        unsigned bs = g_s.bits / 8u;
        *L = wav_sample(p);
        *R = g_s.chans >= 2 ? wav_sample(p + bs) : *L;
        g_s.buf_off += g_s.frame_bytes;
    } else {
        if (g_s.pcm_off >= g_s.pcm_frames) {
            ogg_refill();
            if (!g_s.pcm_frames) return g_s.at_end ? 0 : -1;
        }
        const int16_t *p = g_s.pcm + (size_t)g_s.pcm_off * g_s.chans;
        *L = (float)p[0];
        *R = g_s.chans >= 2 ? (float)p[1] : *L;
        ++g_s.pcm_off;
    }
    if (g_s.force_mono) *L = *R = (*L + *R) * 0.5f;
    return 1;
}

/* Parse a WAV payload's RIFF chunks; `base` = file offset of "RIFF". */
static int wav_open_at(FILE *f, uint32_t base, const char *path) {
    uint8_t h[40];
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, (long)base + 12, SEEK_SET);

    uint16_t tag = 0, chans = 0, bits = 0;
    uint32_t rate = 0, data_off = 0, data_len = 0;
    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8) break;
        uint32_t sz = rd32le(ch + 4);
        long here = ftell(f);
        if (!memcmp(ch, "fmt ", 4)) {
            size_t take = sz < sizeof h ? sz : sizeof h;
            if (fread(h, 1, take, f) != take) break;
            tag   = rd16le(h + 0);
            chans = rd16le(h + 2);
            rate  = rd32le(h + 4);
            bits  = rd16le(h + 14);
            if (tag == 0xFFFE && take >= 26)        /* EXTENSIBLE: real tag */
                tag = rd16le(h + 24);               /* leads the SubFormat GUID */
        } else if (!memcmp(ch, "data", 4)) {
            data_off = (uint32_t)here;
            data_len = sz;
            if (data_off + (uint64_t)data_len > (uint64_t)fsize)
                data_len = (uint32_t)(fsize - data_off);   /* streamed/lying size */
            if (rate) break;                        /* fmt already seen: done */
        }
        if (fseek(f, here + (long)sz + (long)(sz & 1), SEEK_SET) != 0) break;
    }

    int pcm_ok   = tag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    int float_ok = tag == 3 && bits == 32;
    uint32_t frame_bytes = (uint32_t)chans * (bits / 8u);
    if ((!pcm_ok && !float_ok) || chans < 1 || !rate || !data_off ||
        !frame_bytes || data_len < frame_bytes) {
        fprintf(stderr, "[WAV] '%s' unsupported (fmt=%u chans=%u bits=%u rate=%u data=%u)\n",
                path, tag, chans, bits, rate, data_len);
        return 0;
    }

    g_s.fp          = f;
    g_s.kind        = SRC_WAV;
    g_s.data_off    = data_off;
    g_s.frames      = data_len / frame_bytes;
    g_s.chans       = chans;
    g_s.bits        = bits;
    g_s.isfloat     = (uint16_t)float_ok;
    g_s.rate        = rate;
    g_s.frame_bytes = frame_bytes;
    fseek(f, (long)data_off, SEEK_SET);
    return 1;
}

/* Open an OGG Vorbis payload at `off`. The FILE is consumed in EVERY
 * outcome: on success stb_vorbis owns it, on failure stb has already closed
 * it (close_on_free is honored even by the open-failed deinit path). */
static int ogg_open_at(FILE *f, uint32_t off, const char *path) {
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, (long)off, SEEK_SET);
    int err = 0;
    stb_vorbis *v = stb_vorbis_open_file_section(f, 1, &err, NULL,
                                                 (unsigned)(fsize - (long)off));
    if (!v) {
        fprintf(stderr, "[WAV] '%s': OGG open failed (stb_vorbis error %d)\n",
                path, err);
        return 0;
    }
    stb_vorbis_info info = stb_vorbis_get_info(v);
    uint32_t frames = stb_vorbis_stream_length_in_samples(v);
    if (!frames) {
        fprintf(stderr, "[WAV] '%s': OGG has no determinable length\n", path);
        stb_vorbis_close(v);
        return 0;
    }
    g_s.ogg    = v;
    g_s.kind   = SRC_OGG;
    g_s.rate   = info.sample_rate;
    g_s.chans  = 2;         /* stb downmixes/upmixes any layout to stereo */
    g_s.frames = frames;
    return 1;
}

/* Open + dispatch a replacement file by content: RBXS header, bare OGG, or
 * bare WAV. Fills g_s (stopped/cleared by the caller beforehand). */
static int src_open(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t magic[4];
    if (fread(magic, 1, 4, f) != 4) { fclose(f); return 0; }

    uint32_t hdr_rate = 0, hdr_chans = 0;
    int      loop_mode = LOOP_ALL;      /* bare files: loop the whole song */
    uint32_t loop_start = 0, loop_end = 0;
    uint32_t payload = 0;

    if (!memcmp(magic, "RBXS", 4)) {
        /* Fixed 24-byte header: b[] holds offsets 0x04..0x17. */
        uint8_t b[20];
        if (fread(b, 1, 20, f) != 20) { fclose(f); return 0; }
        hdr_rate  = rd32le(b + 0);                 /* 0x04 */
        hdr_chans = rd32le(b + 4);                 /* 0x08 */
        uint16_t loop = rd16le(b + 8);             /* 0x0C */
        if (b[10] != ':' || b[11] != 'P') {        /* 0x0E: always here */
            fprintf(stderr, "[RBXS] '%s': header refuses to smile "
                            "(\":P\" must be at offset 0xE) -- rejected\n", path);
            fclose(f);
            return 0;
        }
        loop_start = rd32le(b + 12);               /* 0x10 */
        loop_end   = rd32le(b + 16);               /* 0x14 */
        loop_mode  = !(loop & 1) ? LOOP_NONE
                   : (loop_start || loop_end) ? LOOP_POINTS : LOOP_ALL;
        payload = 24;
        fprintf(stderr, "[RBXS] '%s': rate=%u chans=%u loop=%s",
                path, hdr_rate, hdr_chans,
                loop_mode == LOOP_NONE ? "no" :
                loop_mode == LOOP_ALL  ? "all" : "points");
        if (loop_mode == LOOP_POINTS)
            fprintf(stderr, " %u..%u", loop_start, loop_end);
        fprintf(stderr, "  :P right back at you\n");
        fflush(stderr);
        if (fread(magic, 1, 4, f) != 4) { fclose(f); return 0; }
    }

    int ok = 0;
    if (!memcmp(magic, "OggS", 4)) {
        ok = ogg_open_at(f, payload, path);
        f = NULL;                       /* consumed either way, see above */
    } else if (!memcmp(magic, "RIFF", 4)) {
        ok = wav_open_at(f, payload, path);
        if (ok) f = NULL;               /* g_s.fp owns it */
    } else {
        fprintf(stderr, "[WAV] '%s': not WAV, OGG, or RBXS "
                        "(starts %02x %02x %02x %02x)\n",
                path, magic[0], magic[1], magic[2], magic[3]);
    }
    if (!ok) {
        if (f) fclose(f);
        memset(&g_s, 0, sizeof g_s);
        return 0;
    }

    /* RBXS overrides on top of what the payload said. */
    if (hdr_rate) g_s.rate = hdr_rate;
    g_s.force_mono = hdr_chans == 1;
    g_s.loop_mode  = loop_mode;
    g_s.loop_start = loop_start;
    g_s.loop_end   = loop_end;

    /* Prime the resampler so playback starts on the first real frame. */
    if (src_next_frame(&g_s.curL, &g_s.curR) != 1) {
        robox_wav_stop();
        return 0;
    }
    g_s.prevL = g_s.curL;
    g_s.prevR = g_s.curR;
    g_s.acc   = 0.0;
    return 1;
}

/* SYNPrepare handed us a song buffer. 1 = we took over, keep fmidi idle. */
int robox_wav_music_start(uint32_t song_va) {
    if (!g_on) return 0;
    const wav_seen_t *hit = NULL;
    for (unsigned i = 1; i <= WAV_TRACK && !hit; ++i) {
        wav_seen_t *s = &g_seen[(g_seen_next + WAV_TRACK - i) % WAV_TRACK];
        if (s->hi <= s->lo || song_va < s->lo || song_va >= s->hi) continue;
        if (s->fp_len) {
            /* Heap reuse check: is this still the song we recorded, or has
             * another file been loaded over the same buffer since? */
            const uint8_t *p = (const uint8_t *)ppc_host_ptr(s->lo);
            if (!p || fnv1a(p, s->fp_len) != s->fp) {
                static int logs;
                if (logs < 8) { ++logs;
                    fprintf(stderr, "[WAV] range 0x%08x was '%s' but holds "
                            "other data now, dropping stale record\n",
                            s->lo, s->song);
                    fflush(stderr);
                }
                s->lo = s->hi = 0;      /* dead entry: buffer was recycled */
                continue;
            }
        }
        hit = s;
    }
    if (!hit) return 0;
    const char *path = path_for_song(hit->song);
    if (!path) return 0;

    robox_wav_stop();
    if (!src_open(path)) {
        char alt[300];
        snprintf(alt, sizeof alt, "mods/%s", path);
        if (!src_open(alt)) {
            fprintf(stderr, "[WAV] '%s' -> '%s': can't play, falling back to MIDI\n",
                    hit->song, path);
            fflush(stderr);
            return 0;
        }
    }
    snprintf(g_s.song, sizeof g_s.song, "%s", hit->song);
    g_s.playing = 1;
    fprintf(stderr, "[WAV] '%s' -> '%s' (%s, %u Hz, %s, %u frames, loop=%s)\n",
            hit->song, path, g_s.kind == SRC_OGG ? "ogg" : "wav",
            g_s.rate, g_s.force_mono ? "forced mono"
                      : g_s.chans >= 2 ? "stereo" : "mono",
            g_s.frames,
            g_s.loop_mode == LOOP_NONE ? "no" :
            g_s.loop_mode == LOOP_ALL  ? "all" : "points");
    fflush(stderr);
    return 1;
}

// ---------------------------------------------------------------------------
// direct playback: a mod puts a file on, right now
// ---------------------------------------------------------------------------
//
// robox_wav_music_start above is driven by the GAME -- a song buffer reaches
// SYNPrepare and whatever wav_music.cfg maps it to plays. A mod with a
// PLAYLIST needs the other direction: play this file now, and tell me when it
// is over, so it can put the next one on. That is all this is -- the same
// src_open and the same render path, started by hand.
//
// The stream is flagged `direct`, which buys two things:
//   - sdk/peripherals.c skips the DLS sampler while it runs, so the game's own
//     song does not sit underneath the mod's track. It is a MUTE, not a stop:
//     fmidi keeps its place and is simply heard again the moment the mod's
//     track ends, so this is reversible with nothing to put back.
//   - robox_wav_playing/robox_wav_is_direct let a mod tell its own track from
//     the game starting a song over the top of it, which is what makes
//     "has my track finished?" answerable at all.
//
// loop = 0 stops at the end of the file -- that stop IS the cue to advance.
// loop = 1 leaves whatever the file's own header asked for (bare WAV/OGG loop
// whole; RBXS says for itself).
int robox_wav_play_file(const char *path, int loop) {
    if (!path || !*path) return 0;
    robox_wav_stop();
    if (!src_open(path)) {
        char alt[300];                  /* same mods/-relative retry as above */
        snprintf(alt, sizeof alt, "mods/%s", path);
        if (!src_open(alt)) {
            fprintf(stderr, "[WAV] direct play '%s': cannot open\n", path);
            fflush(stderr);
            return 0;
        }
    }
    if (!loop) g_s.loop_mode = LOOP_NONE;
    g_s.playing = 1;
    g_s.direct  = 1;

    const char *base = path;            /* either slash: paths come from mods */
    for (const char *p = path; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    snprintf(g_s.song, sizeof g_s.song, "%s", base);
    fprintf(stderr, "[WAV] direct play '%s' (%s, %u Hz, %u frames, loop=%s)\n",
            path, g_s.kind == SRC_OGG ? "ogg" : "wav", g_s.rate, g_s.frames,
            g_s.loop_mode == LOOP_NONE ? "no" :
            g_s.loop_mode == LOOP_ALL  ? "all" : "points");
    fflush(stderr);
    return 1;
}

/* What is coming out of this module right now: the song name for a mapped
 * game song, the file's basename for a direct one, NULL for neither. */
const char *robox_wav_current(void) { return g_s.playing ? g_s.song : NULL; }

/* 1 while the current stream is a mod's, not the game's. Read every audio
 * callback by peripherals.c, so it stays a plain field read. */
int robox_wav_is_direct(void) { return g_s.playing && g_s.direct; }

/* Game SYN play-state: 0 = pause, nonzero = resume (robox_midi_setstate). */
void robox_wav_setstate(unsigned playing) {
    if (g_s.playing) g_s.paused = !playing;
}

/* Mix into the AX accumulation buffer (interleaved stereo int32), resampling
 * from the file rate to the device rate. Same slot as robox_synth_render. */
void robox_wav_render(int32_t *mix, int nout, int freq) {
    if (!g_s.playing || g_s.paused || freq <= 0) return;
    double step = (double)g_s.rate / (double)freq;
    for (int i = 0; i < nout; ++i) {
        g_s.acc += step;
        while (g_s.acc >= 1.0) {
            g_s.acc -= 1.0;
            g_s.prevL = g_s.curL;
            g_s.prevR = g_s.curR;
            int rc = src_next_frame(&g_s.curL, &g_s.curR);
            if (rc != 1) {
                if (rc == 0)
                    fprintf(stderr, "[WAV] '%s' finished (header says no loop)\n",
                            g_s.song);
                else
                    fprintf(stderr, "[WAV] stream read failed, stopping '%s'\n",
                            g_s.song);
                fflush(stderr);
                robox_wav_stop();
                return;
            }
        }
        float t = (float)g_s.acc;
        float L = g_s.prevL + (g_s.curL - g_s.prevL) * t;
        float R = g_s.prevR + (g_s.curR - g_s.prevR) * t;
        mix[i * 2]     += (int32_t)(L * g_gain);
        mix[i * 2 + 1] += (int32_t)(R * g_gain);
    }
}
