// sdk/robox_mods.c -- Robox mod registry / loader.
//
// Resolution order for each mod, last wins:
//   1. built-in default below
//   2. mods/mods.cfg          ("id = 0|1", '#' comments, blank lines ok)
//   3. environment            ROBOX_MOD_<ID>=0|1   (uppercased id)
// A mod whose `needs` file is missing is forced off no matter what, since
// enabling it would just fail later somewhere less obvious.
//
// Adding a mod = one row in g_mods plus its init function. Nothing else in
// the tree needs to know it exists.

#include "robox_mods.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Mod entry points. */
extern void robox_coop_init(void);
extern void robox_discord_init(void);
extern void robox_wav_init(void);
extern void robox_mario_init(void);
static void mod_music_init(void);

static robox_mod_t g_mods[] = {
    { "music", "Host MIDI + DLS music",
      "Replaces the game's wavetable synth with a host DLS sampler.",
      "robox.dls", mod_music_init, 1, 0 },

    { "wavmusic", "Music packs (WAV/OGG/RBXS)",
      "Streams WAV/OGG/RBXS files in place of mapped songs (mods/wav_music.cfg).",
      "wav_music.cfg", robox_wav_init, 1, 0 },

    { "coop",  "Local / online co-op",
      "Adds a second player: clone entity, own controls, own input word.",
      NULL,      robox_coop_init,  0, 0 },

    { "mario", "Super Mario Bros. movement",
      "The player moves with bit-exact SMB1 physics (walk/run/skid/jump).",
      NULL,      robox_mario_init, 0, 0 },

    { "discord", "Discord Rich Presence",
      "Shows \"Playing ROBOX Recompiled\" on your Discord profile.",
      NULL,      robox_discord_init, 1, 0 },
};

#define MOD_COUNT ((unsigned)(sizeof g_mods / sizeof g_mods[0]))

/* The music mod has no init of its own -- it is entirely gated by
 * robox_mod_enabled("music") at the points that care (asset hiding in
 * robox_io.c, keyon suppression, event routing in robox_midi.cc). The row
 * exists so it is listed, configurable, and logged like everything else. */
static void mod_music_init(void) {}

static robox_mod_t *find(const char *id) {
    for (unsigned i = 0; i < MOD_COUNT; ++i)
        if (!strcmp(g_mods[i].id, id)) return &g_mods[i];
    return NULL;
}

int robox_mod_enabled(const char *id) {
    const robox_mod_t *m = find(id);
    return m && m->enabled;
}

unsigned           robox_mods_count(void)         { return MOD_COUNT; }
const robox_mod_t *robox_mods_get(unsigned index) {
    return index < MOD_COUNT ? &g_mods[index] : NULL;
}

static int truthy(const char *s) {
    while (*s && isspace((unsigned char)*s)) ++s;
    return !(*s == '0' || *s == 'n' || *s == 'N' || *s == 'f' || *s == 'F');
}

static void apply_cfg(void) {
    FILE *f = fopen("mods/mods.cfg", "r");
    if (!f) return;
    char line[256];
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

        robox_mod_t *m = find(k);
        if (m) m->enabled = truthy(eq + 1);
    }
    fclose(f);
}

static void apply_env(robox_mod_t *m) {
    char var[64];
    snprintf(var, sizeof var, "ROBOX_MOD_%s", m->id);
    for (char *p = var; *p; ++p) *p = (char)toupper((unsigned char)*p);
    const char *v = getenv(var);
    if (v && *v) m->enabled = truthy(v);
}

/* Back-compat: co-op predates the registry and its env var is in the .bat
 * files and muscle memory. Honour it as an alias. */
static void apply_legacy_env(void) {
    const char *v = getenv("ROBOX_COOP");
    robox_mod_t *m = find("coop");
    if (v && *v && m) m->enabled = truthy(v);
}

/* Web: ?coop=1 / ?coop=0 (and ?music=..) in the URL, applied last so it wins.
 * getenv is useless on that target -- the guest runs on a worker whose ENV is
 * empty -- and being able to A/B a mod with a reload instead of a rebuild is
 * the difference between a one-minute test and a ten-minute one. */
static void apply_web_url(robox_mod_t *m) {
#if defined(__EMSCRIPTEN__)
    extern int robox_web_url_flag(const char *name);   /* sdk/robox_web_debug.c */
    int v = robox_web_url_flag(m->id);
    if (v >= 0) m->enabled = v != 0;
#else
    (void)m;
#endif
}

void robox_mods_init(void) {
    apply_cfg();
    for (unsigned i = 0; i < MOD_COUNT; ++i) apply_env(&g_mods[i]);
    apply_legacy_env();
    for (unsigned i = 0; i < MOD_COUNT; ++i) apply_web_url(&g_mods[i]);

    for (unsigned i = 0; i < MOD_COUNT; ++i) {
        robox_mod_t *m = &g_mods[i];
        m->available = 1;
        if (m->needs) {
            char path[256];
            snprintf(path, sizeof path, "mods/%s", m->needs);
            FILE *f = fopen(path, "rb");
            m->available = f != NULL;
            if (f) fclose(f);
            if (!m->available && m->enabled) {
                fprintf(stderr, "[MODS] %-7s disabled: mods/%s not found\n",
                        m->id, m->needs);
                m->enabled = 0;
            }
        }
    }

    fprintf(stderr, "[MODS] ---- mod loader ----\n");
    for (unsigned i = 0; i < MOD_COUNT; ++i) {
        const robox_mod_t *m = &g_mods[i];
        fprintf(stderr, "[MODS]  %-7s %-3s  %s\n",
                m->id, m->enabled ? "ON" : "off", m->name);
    }
    fprintf(stderr, "[MODS] --------------------\n");
    fflush(stderr);

    for (unsigned i = 0; i < MOD_COUNT; ++i)
        if (g_mods[i].enabled && g_mods[i].init) g_mods[i].init();
}
