// quirks/quirks.c -- Profile dispatch.
//
// Lists every quirks profile compiled into the build, picks one by env var
// at first access if main() didn't set one, and serves it through
// quirks_active().

#include "quirks.h"

#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Built-in profiles. Add `extern const quirks_t quirks_<n>;` for each new
// quirks/<n>.c file, and append a pointer to it in `g_profiles`.
// ---------------------------------------------------------------------------

const quirks_t quirks_none = {
    .name        = "none",
    .description = "no per-game patches (default)",
    .worker_entries           = NULL,  .worker_entry_count        = 0,
    .safe_vtable_targets      = NULL,  .safe_vtable_target_count  = 0,
    .lowmem_pokes             = NULL,  .lowmem_poke_count         = 0,
    .osinit_post_hook         = NULL,
};

extern const quirks_t quirks_robox;   // quirks/robox.c

static const quirks_t *const g_profiles[] = {
    &quirks_none,
    &quirks_robox,
    NULL
};

// ---------------------------------------------------------------------------
// Active profile state
// ---------------------------------------------------------------------------

static const quirks_t *g_active = NULL;
static int             g_active_resolved = 0;

const quirks_t *quirks_find_by_name(const char *name) {
    if (!name || !*name) return NULL;
    for (const quirks_t *const *p = g_profiles; *p; ++p) {
        if ((*p)->name && strcmp((*p)->name, name) == 0) return *p;
    }
    return NULL;
}

void quirks_set_active(const quirks_t *q) {
    g_active = q;
    g_active_resolved = 1;
}

const quirks_t *quirks_active(void) {
    if (g_active_resolved) return g_active;
    g_active_resolved = 1;
    const char *env = getenv("RECOMP_QUIRKS_PROFILE");
    if (env && *env) {
        const quirks_t *q = quirks_find_by_name(env);
        if (q) { g_active = q; return g_active; }
    }
    g_active = &quirks_none;
    return g_active;
}
