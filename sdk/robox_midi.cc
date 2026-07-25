// sdk/robox_midi.cc -- Host-side MIDI sequencer for Robox's music, built on
// fmidi (fmidi-master/). The game's own SYN sequencer keys notes correctly
// but its envelope/release path never silences them (notes hang forever).
//
// Split of responsibilities:
//   * fmidi parses the song (a standard MIDI file the game loads into guest
//     memory as e.g. music/comienzo.jul) and streams events with correct
//     tempo, driven from audio_pump_frame at the AX 3 ms rate.
//   * NOTE-ON / CC / PROGRAM / PITCHBEND events are handed to the game's own
//     raw-MIDI dispatcher (func_801102a0) so its wavetable synth voices the
//     notes exactly like real hardware.
//   * NOTE-OFF bypasses the game entirely: robox_note_off() (quirks/robox.c)
//     looks the voice up in the synth's [channel][note] table and hands it
//     to the host mixer's release fade.
//   * The game's own sequencer advance is disabled (SDA[-0x3e24]=0) the
//     moment the first song starts, so events don't double-fire.
//
// Hooks (QUIRK EDITs in generated code):
//   func_80112020 (SYNPrepare)  -> robox_syn_start_probe -> robox_midi_load
//   func_80112150 (SYN stop)    -> robox_midi_stop

#include "fmidi/fmidi.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern "C" {
void *ppc_host_ptr(uint32_t va);
void  robox_guest_msg(const uint8_t *msg, unsigned len);   // quirks/robox.c
void  robox_note_off(unsigned ch, unsigned note);          // quirks/robox.c
// MOD #1: host DLS sampler (sdk/robox_synth.cc + mods/robox.dls)
int   robox_synth_load(const char *path);
int   robox_synth_active(void);
void  robox_synth_all_off(void);   // stop/new song: silence + forget programs
void  robox_synth_silence(void);   // pause: silence but KEEP programs
void  robox_synth_note_on(int ch, int note, int vel, int freq);
void  robox_synth_note_off(int ch, int note);
void  robox_synth_cc(int ch, int cc, int val);
void  robox_synth_prog(int ch, int prog);
void  robox_synth_bend(int ch, int lsb, int msb);
int   audio_device_freq(void);
// MOD: WAV music packs (sdk/robox_wav.c + mods/wav_music.cfg)
int   robox_wav_music_start(uint32_t song_va);
void  robox_wav_stop(void);
void  robox_wav_setstate(unsigned playing);
}

// fmidi_seq.cc (vendored, modified): lock the sequencer's tempo and ignore
// every "set tempo" meta event. The game's own music engine plays every song
// at a fixed 120 BPM no matter what the file says, so matching that is what
// makes the music sound like the real game.
extern "C" uint32_t fmidi_fixed_tempo;
#define ROBOX_TEMPO_US_PER_QN 500000u   /* 120 BPM */

static fmidi_smf_t    *g_smf;
static fmidi_player_t *g_player;
static bool            g_paused;

// A standard MIDI file is self-describing: header chunk + per-track chunks
// with explicit lengths. Walk them to learn the total byte size (the guest
// buffer length isn't passed to the SYNPrepare hook).
static size_t smf_total_size(const uint8_t *d)
{
    if (memcmp(d, "MThd", 4) != 0) return 0;
    uint32_t hlen = (d[4] << 24) | (d[5] << 16) | (d[6] << 8) | d[7];
    uint16_t ntrk = (uint16_t)((d[10] << 8) | d[11]);
    size_t pos = 8 + hlen;
    for (unsigned t = 0; t < ntrk; ++t) {
        if (memcmp(d + pos, "MTrk", 4) != 0) return pos;   // tolerate junk tail
        uint32_t tlen = (uint32_t)((d[pos+4] << 24) | (d[pos+5] << 16) |
                                   (d[pos+6] << 8)  |  d[pos+7]);
        pos += 8 + tlen;
        if (pos > 4u * 1024u * 1024u) return 0;            // sanity
    }
    return pos;
}

static void on_event(const fmidi_event_t *evt, void *)
{
    if (evt->type != fmidi_event_message || evt->datalen < 1)
        return;
    const uint8_t *d = evt->data;
    uint8_t status = d[0];
    if (status >= 0xF0)                       // sysex/realtime: not for the synth
        return;
    uint8_t kind = status & 0xF0;
    int ch = status & 0x0F;
    uint8_t d1 = evt->datalen > 1 ? d[1] : 0;
    uint8_t d2 = evt->datalen > 2 ? d[2] : 0;

    if (robox_synth_active()) {
        // MOD #1: the host DLS sampler voices everything.
        switch (kind) {
            case 0x80: robox_synth_note_off(ch, d1); break;
            case 0x90:
                if (d2 == 0) robox_synth_note_off(ch, d1);
                else         robox_synth_note_on(ch, d1, d2, audio_device_freq());
                break;
            case 0xB0: robox_synth_cc(ch, d1, d2); break;
            case 0xC0: robox_synth_prog(ch, d1); break;
            case 0xE0: robox_synth_bend(ch, d1, d2); break;
            default: break;
        }
        return;
    }

    // Fallback (no mods/robox.dls): feed the game's own synth queue.
    if (kind == 0x80 || (kind == 0x90 && evt->datalen >= 3 && d2 == 0)) {
        robox_note_off(ch, d1);
        return;
    }
    unsigned len = evt->datalen > 3 ? 3 : (unsigned)evt->datalen;
    robox_guest_msg(d, len);
}

static void on_finish(void *)
{
    if (g_player) {
        fmidi_player_rewind(g_player);        // game music loops
        fmidi_player_start(g_player);
    }
}

extern "C" void robox_midi_stop(void)
{
    if (g_player) { fmidi_player_free(g_player); g_player = nullptr; }
    if (g_smf)    { fmidi_smf_free(g_smf);       g_smf    = nullptr; }
    robox_wav_stop();
    robox_synth_all_off();
}

extern "C" void robox_midi_load(uint32_t song_va)
{
    /* MOD #1: instrument bank from the mods folder (next to the exe). */
    { static int tried;
      if (!tried) { tried = 1; robox_synth_load("mods/robox.dls"); } }
    fmidi_fixed_tempo = ROBOX_TEMPO_US_PER_QN;   /* before the seq is built */
    robox_midi_stop();
    g_paused = false;
    /* MOD: WAV music packs -- when mods/wav_music.cfg maps this song to a
     * WAV, stream that instead; fmidi stays idle for this song. */
    if (robox_wav_music_start(song_va))
        return;
    const uint8_t *bytes = (const uint8_t *)ppc_host_ptr(song_va);
    if (!bytes) return;
    size_t size = smf_total_size(bytes);
    if (!size) {
        fprintf(stderr, "[MIDI] song at 0x%08x is not SMF, host player idle\n", song_va);
        return;
    }
    g_smf = fmidi_smf_mem_read(bytes, size);
    if (!g_smf) {
        fprintf(stderr, "[MIDI] fmidi parse failed (%s)\n",
                fmidi_strerror(fmidi_errno()));
        return;
    }
    g_player = fmidi_player_new(g_smf);
    if (!g_player) { robox_midi_stop(); return; }
    fmidi_player_event_callback(g_player, &on_event, nullptr);
    fmidi_player_finish_callback(g_player, &on_finish, nullptr);
    fmidi_player_start(g_player);
    fprintf(stderr, "[MIDI] host player started: song=0x%08x size=%u\n",
            song_va, (unsigned)size);
    fflush(stderr);
}

// Game's SYN play-state (func_801121f0): 0 = stop/pause, nonzero = play.
extern "C" void robox_midi_setstate(unsigned playing)
{
    if (!playing) {
        if (!g_paused) {
            g_paused = true;
            if (g_player) fmidi_player_stop(g_player);
            robox_wav_setstate(0);
            /* Silence, do NOT reset: resume continues from mid-song, so the
             * channel programs we hold are the only record of the instruments.
             * A hard stop / song change goes through robox_midi_stop, which
             * does the full reset. */
            robox_synth_silence();
            fprintf(stderr, "[MIDI] paused (game stopped music)\n");
            fflush(stderr);
        }
    } else if (g_paused) {
        g_paused = false;
        if (g_player) fmidi_player_start(g_player);
        robox_wav_setstate(1);
        fprintf(stderr, "[MIDI] resumed\n");
        fflush(stderr);
    }
}

extern "C" void robox_midi_pump(double dt_seconds)
{
    if (g_player && !g_paused)
        fmidi_player_tick(g_player, dt_seconds);
}
