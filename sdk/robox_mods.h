// sdk/robox_mods.h -- Robox mod registry.
//
// One place that knows what mods exist, whether each is enabled, and how to
// start it. Before this, each mod gated itself ad hoc (the music mod did a
// bare fopen("mods/robox.dls") wherever it happened to need the answer, and
// co-op read its own env var), which meant nothing could enumerate them --
// no load order, no single log line, and nothing for an in-game options
// screen to list.
#ifndef ROBOX_MODS_H
#define ROBOX_MODS_H

typedef struct robox_mod {
    const char *id;        /* stable key: config file, env var, queries   */
    const char *name;      /* display name (for an options screen)        */
    const char *desc;      /* one line, same                              */
    const char *needs;     /* file under mods/ that must exist, or NULL   */
    void      (*init)(void);
    int         enabled;   /* resolved by robox_mods_init()               */
    int         available; /* `needs` satisfied (or nothing needed)       */
} robox_mod_t;

/* Resolve enable/disable for every mod, then init the enabled ones.
 * Call after the image is loaded (mods patch the dispatch table) and
 * before the guest entry point. */
void robox_mods_init(void);

/* Query by id. Safe to call before robox_mods_init() -- returns 0. */
int robox_mod_enabled(const char *id);

/* Enumeration, for an options screen. */
unsigned            robox_mods_count(void);
const robox_mod_t  *robox_mods_get(unsigned index);

#endif /* ROBOX_MODS_H */
