// sdk/robox_synth.cc -- Host-side DLS sampler for Robox music (MOD #1).
//
// Plays MIDI events (from the fmidi host sequencer in robox_midi.cc) using
// the instrument bank in mods/robox.dls — generated from the game's own
// robox.wt + robox.pcm by build_soundfont.py. This replaces the game's SYN
// wavetable synth for music entirely: no guest code is involved between the
// sequencer and the speakers, so none of its envelope/instrument-state bugs
// apply. SFX still play through the game's AX voice path.
//
// The DLS reader matches exactly what build_soundfont.py emits:
//   RIFF('DLS ') { colh, LIST(lins){ LIST(ins ){ insh, LIST(lart){art1},
//   LIST(lrgn){ LIST(rgn ){rgnh, wsmp, wlnk} }, LIST(INFO) } },
//   ptbl, LIST(wvpl){ LIST(wave){fmt , wsmp, data} } }

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>

// ---------------------------------------------------------------------------
// DLS model
// ---------------------------------------------------------------------------

struct Wave {
    std::vector<int16_t> pcm;
    uint32_t rate = 22050;
};

struct Zone {
    uint8_t  key_lo = 0, key_hi = 127;
    uint8_t  root   = 60;
    int16_t  fine   = 0;          // cents
    uint32_t loop_start = 0, loop_len = 0;
    bool     has_loop = false;
    uint32_t wave = 0;
};

struct Instrument {
    int prog = -1;
    double release_ms = 200.0;
    std::vector<Zone> zones;
};

static std::vector<Wave>       g_waves;
static std::vector<Instrument> g_insts;   // indexed lookup by prog via find
static bool g_loaded;

static const Instrument *inst_for_prog(int prog)
{
    for (const Instrument &i : g_insts)
        if (i.prog == prog) return &i;
    return nullptr;
}

// ---- RIFF helpers ----------------------------------------------------------

struct Cursor { const uint8_t *p; size_t n; };

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// Walk chunks inside [p, p+n): calls fn(tag4, listtag4-or-null, data, size).
template <class F>
static void walk_chunks(const uint8_t *p, size_t n, F fn)
{
    size_t pos = 0;
    while (pos + 8 <= n) {
        const uint8_t *tag = p + pos;
        uint32_t sz = rd32(p + pos + 4);
        const uint8_t *data = p + pos + 8;
        if (pos + 8 + sz > n) break;
        if (!memcmp(tag, "LIST", 4) && sz >= 4)
            fn((const char *)data, data + 4, sz - 4, true);
        else
            fn((const char *)tag, data, sz, false);
        pos += 8 + sz + (sz & 1);
    }
}

static void parse_region(const uint8_t *p, size_t n, Instrument &inst)
{
    Zone z;
    walk_chunks(p, n, [&](const char *tag, const uint8_t *d, size_t sz, bool) {
        if (!memcmp(tag, "rgnh", 4) && sz >= 12) {
            z.key_lo = (uint8_t)rd16(d + 0);
            z.key_hi = (uint8_t)rd16(d + 2);
        } else if (!memcmp(tag, "wsmp", 4) && sz >= 20) {
            z.root = (uint8_t)rd16(d + 4);
            z.fine = (int16_t)rd16(d + 6);
            uint32_t nloops = rd32(d + 16);
            if (nloops >= 1 && sz >= 36) {
                z.loop_start = rd32(d + 28);
                z.loop_len   = rd32(d + 32);
                z.has_loop   = z.loop_len > 0;
            }
        } else if (!memcmp(tag, "wlnk", 4) && sz >= 12) {
            z.wave = rd32(d + 8);
        }
    });
    inst.zones.push_back(z);
}

static void parse_instrument(const uint8_t *p, size_t n)
{
    Instrument inst;
    walk_chunks(p, n, [&](const char *tag, const uint8_t *d, size_t sz, bool is_list) {
        if (!is_list && !memcmp(tag, "insh", 4) && sz >= 12) {
            uint32_t bank = rd32(d + 4);
            uint32_t prog = rd32(d + 8);
            inst.prog = (bank & 0x80000000u) ? -1 : (int)prog;  // skip GM drum alias
        } else if (is_list && !memcmp(tag, "lart", 4)) {
            walk_chunks(d, sz, [&](const char *t2, const uint8_t *d2, size_t s2, bool) {
                if (!memcmp(t2, "art1", 4) && s2 >= 8) {
                    uint32_t nconn = rd32(d2 + 4);
                    for (uint32_t c = 0; c < nconn && 8 + (c + 1) * 12 <= s2; ++c) {
                        const uint8_t *cn = d2 + 8 + c * 12;
                        uint16_t dst = rd16(cn + 4);
                        if (dst == 0x0209) {          // EG1 release time (timecents)
                            int32_t tc = (int32_t)rd32(cn + 8);
                            /* Per-instrument release from the DLS art1 EG1
                             * (derived from the game's envelope table by
                             * build_soundfont.py). Full length with the
                             * exponential curve; drums additionally ignore
                             * note-off for one-shot samples (see note_on). */
                            inst.release_ms = 1000.0 * pow(2.0, tc / 1200.0);
                            if (inst.release_ms < 30)   inst.release_ms = 30;
                            if (inst.release_ms > 1200) inst.release_ms = 1200;
                        }
                    }
                }
            });
        } else if (is_list && !memcmp(tag, "lrgn", 4)) {
            walk_chunks(d, sz, [&](const char *t2, const uint8_t *d2, size_t s2, bool l2) {
                if (l2 && !memcmp(t2, "rgn ", 4))
                    parse_region(d2, s2, inst);
            });
        }
    });
    if (inst.prog >= 0 && !inst.zones.empty())
        g_insts.push_back(std::move(inst));
}

static void parse_wave(const uint8_t *p, size_t n)
{
    Wave w;
    walk_chunks(p, n, [&](const char *tag, const uint8_t *d, size_t sz, bool) {
        if (!memcmp(tag, "fmt ", 4) && sz >= 16) {
            w.rate = rd32(d + 4);
        } else if (!memcmp(tag, "data", 4)) {
            w.pcm.resize(sz / 2);
            for (size_t i = 0; i < sz / 2; ++i)
                w.pcm[i] = (int16_t)rd16(d + i * 2);
        }
    });
    g_waves.push_back(std::move(w));
}

extern "C" int robox_synth_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf((size_t)len);
    if (fread(buf.data(), 1, (size_t)len, f) != (size_t)len) { fclose(f); return 0; }
    fclose(f);

    if (len < 12 || memcmp(buf.data(), "RIFF", 4) || memcmp(buf.data() + 8, "DLS ", 4))
        return 0;

    g_waves.clear();
    g_insts.clear();
    walk_chunks(buf.data() + 12, (size_t)len - 12,
                [&](const char *tag, const uint8_t *d, size_t sz, bool is_list) {
        if (is_list && !memcmp(tag, "lins", 4)) {
            walk_chunks(d, sz, [&](const char *t2, const uint8_t *d2, size_t s2, bool l2) {
                if (l2 && !memcmp(t2, "ins ", 4))
                    parse_instrument(d2, s2);
            });
        } else if (is_list && !memcmp(tag, "wvpl", 4)) {
            walk_chunks(d, sz, [&](const char *t2, const uint8_t *d2, size_t s2, bool l2) {
                if (l2 && !memcmp(t2, "wave", 4))
                    parse_wave(d2, s2);
            });
        }
    });
    g_loaded = !g_insts.empty() && !g_waves.empty();
    fprintf(stderr, "[SYNTH] %s: %zu instruments, %zu waves %s\n",
            path, g_insts.size(), g_waves.size(), g_loaded ? "(active)" : "(EMPTY)");
    fflush(stderr);
    return g_loaded;
}

extern "C" int robox_synth_active(void) { return g_loaded; }

// ---------------------------------------------------------------------------
// Sampler
// ---------------------------------------------------------------------------

#define NVOICES 128

struct SVoice {
    bool     on = false;
    uint8_t  ch = 0, note = 0;
    const Wave *wave = nullptr;
    double   pos = 0, step = 0;      // step at device rate incl. pitch
    double   base_step = 0;          // without pitch bend
    uint32_t loop_start = 0, loop_end = 0;
    bool     has_loop = false;
    float    amp = 0;                // velocity * channel gains
    bool     releasing = false;
    bool     nocut = false;          // one-shot percussion: ignore note-off
    float    relgain = 1.0f, reldec = 0;
    uint32_t age = 0;
};

struct Channel {
    uint8_t prog = 0;
    float   vol = 100.f / 127.f;     // CC7
    float   expr = 1.0f;             // CC11
    float   bend = 0;                // semitones
};

static SVoice   g_v[NVOICES];
static Channel  g_ch[16];
static uint32_t g_age;

// Silence every sounding voice but KEEP each channel's program / volume /
// expression / bend. This is what a PAUSE needs.
//
// Nothing re-sends program changes on resume: the fmidi player picks up from
// the middle of the song, long past the program-change events that sit at the
// top of every track, so the channel state we have IS the only record of which
// instrument each channel plays. Forgetting it here was the "instruments all
// turn into the same one after the music stops and restarts" bug -- every
// channel reverted to the default program 0 (which the DLS bank does provide,
// so it plays audibly rather than dropping out: every part comes back as that
// one instrument -- "everything set to 1" counting programs from 1). It stays
// wrong until the song loops back to the top and the file's program changes
// re-fire.
extern "C" void robox_synth_silence(void)
{
    for (SVoice &v : g_v) v.on = false;
}

// Full reset: silence AND forget channel state. For a real STOP or a NEW song
// load -- the incoming song re-sends its own program changes from the top, so
// starting from defaults is correct there (and only there).
extern "C" void robox_synth_all_off(void)
{
    robox_synth_silence();
    for (Channel &c : g_ch) c = Channel();
}

extern "C" void robox_synth_note_on(int ch, int note, int vel, int freq)
{
    if (!g_loaded || vel <= 0) return;
    Channel &C = g_ch[ch & 15];
    const Instrument *inst = inst_for_prog(C.prog);
    if (!inst) {
        /* Program not in the bank — every note on this channel would be
         * silently dropped (a whole part missing). Fall back to the closest
         * program number that does exist. */
        int best = 1 << 30;
        for (const Instrument &i : g_insts) {
            int dist = i.prog > C.prog ? i.prog - C.prog : C.prog - i.prog;
            if (dist < best) { best = dist; inst = &i; }
        }
        { static int warned[128];
          if (C.prog < 128 && !warned[C.prog]) { warned[C.prog] = 1;
            fprintf(stderr, "[SYNTH] prog %d not in bank -> using prog %d\n",
                    C.prog, inst ? inst->prog : -1);
            fflush(stderr); } }
        if (!inst) return;
    }
    const Zone *zone = nullptr;
    for (const Zone &z : inst->zones)
        if (note >= z.key_lo && note <= z.key_hi) { zone = &z; break; }
    if (!zone) {
        /* No zone covers this key. The bank only maps the ranges the game's
         * own songs used, but transposed/edited parts do stray outside —
         * dropping those was the "notes skipping". Fall back to the nearest
         * zone and let the pitch shift cover the gap. */
        int best = 1 << 30;
        for (const Zone &z : inst->zones) {
            int dist = note < z.key_lo ? (z.key_lo - note)
                     : note > z.key_hi ? (note - z.key_hi) : 0;
            if (dist < best) { best = dist; zone = &z; }
        }
    }
    if (!zone || zone->wave >= g_waves.size()) return;
    const Wave &w = g_waves[zone->wave];
    if (w.pcm.empty()) return;

    /* Retrigger: the same key sounding on the same channel is one voice on
     * hardware — release the old one instead of stacking a second. */
    for (SVoice &c : g_v)
        if (c.on && !c.releasing && c.ch == (ch & 15) && c.note == note)
            c.releasing = true;

    // steal: free voice, else quietest releasing one, else oldest
    SVoice *v = nullptr;
    for (SVoice &c : g_v) if (!c.on) { v = &c; break; }
    if (!v) { float best = 1e9f;
        for (SVoice &c : g_v)
            if (c.releasing && c.relgain < best) { best = c.relgain; v = &c; } }
    if (!v) { uint32_t best = 0xffffffffu;
        for (SVoice &c : g_v) if (c.age < best) { best = c.age; v = &c; } }

    double semis = (double)note - zone->root + zone->fine / 100.0;
    v->base_step = (double)w.rate / (double)freq * pow(2.0, semis / 12.0);
    v->step  = v->base_step * pow(2.0, C.bend / 12.0);
    v->wave  = &w;
    v->pos   = 0;
    v->loop_start = zone->loop_start;
    v->loop_end   = zone->has_loop ? zone->loop_start + zone->loop_len
                                   : (uint32_t)w.pcm.size();
    if (v->loop_end > w.pcm.size()) v->loop_end = (uint32_t)w.pcm.size();
    v->has_loop = zone->has_loop && v->loop_end > v->loop_start;
    v->amp = (vel / 127.0f) * C.vol * C.expr;
    v->ch = (uint8_t)ch; v->note = (uint8_t)note;
    v->releasing = false; v->relgain = 1.0f;
    /* drums (prog 39): one-shot hits ring out fully, note-off is ignored */
    v->nocut = (inst->prog == 39) && !zone->has_loop;
    /* exponential release: reach -60 dB in release_ms (ln(1000) = 6.9078) */
    v->reldec = expf((float)(-6.9078 / (inst->release_ms * 0.001 * freq)));
    v->age = ++g_age;
    v->on = true;
}

extern "C" void robox_synth_note_off(int ch, int note)
{
    for (SVoice &v : g_v)
        if (v.on && !v.releasing && !v.nocut && v.ch == (ch & 15) && v.note == note)
            v.releasing = true;
}

extern "C" void robox_synth_cc(int ch, int cc, int val)
{
    Channel &C = g_ch[ch & 15];
    switch (cc) {
        case 7:  C.vol  = val / 127.0f; break;
        case 11: C.expr = val / 127.0f; break;
        case 120: case 123:                       // all sound / notes off
            for (SVoice &v : g_v) if (v.on && v.ch == (ch & 15)) v.releasing = true;
            break;
        default: break;
    }
}

extern "C" void robox_synth_prog(int ch, int prog)
{
    g_ch[ch & 15].prog = (uint8_t)prog;
}

extern "C" void robox_synth_bend(int ch, int lsb, int msb)
{
    int v14 = ((msb & 0x7F) << 7) | (lsb & 0x7F);
    Channel &C = g_ch[ch & 15];
    C.bend = ((v14 - 8192) / 8192.0f) * 2.0f;     // ±2 semitones
    float f = powf(2.0f, C.bend / 12.0f);
    for (SVoice &v : g_v)
        if (v.on && v.ch == (ch & 15)) v.step = v.base_step * f;
}

// Mix into the AX mixer's accumulation buffer (interleaved stereo int32).
extern "C" void robox_synth_render(int32_t *mix, int nout, int freq)
{
    (void)freq;
    if (!g_loaded) return;
    for (SVoice &v : g_v) {
        if (!v.on) continue;
        const int16_t *d = v.wave->pcm.data();
        size_t dn = v.wave->pcm.size();
        for (int i = 0; i < nout; ++i) {
            v.pos += v.step;
            if (v.has_loop) {
                while (v.pos >= v.loop_end)
                    v.pos -= (v.loop_end - v.loop_start);
            } else if (v.pos >= (double)dn - 1) {
                v.on = false;
                break;
            }
            size_t i0 = (size_t)v.pos;
            double fr = v.pos - (double)i0;
            size_t i1 = i0 + 1 < dn ? i0 + 1 : i0;
            float smp = (float)(d[i0] + (d[i1] - d[i0]) * fr);
            if (v.releasing) {
                v.relgain *= v.reldec;              /* exponential decay */
                if (v.relgain <= 0.001f) { v.on = false; break; }
                smp *= v.relgain;
            }
            int32_t out = (int32_t)(smp * v.amp * 0.35f);
            mix[i * 2]     += out;
            mix[i * 2 + 1] += out;
        }
    }
}
