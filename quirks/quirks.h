// quirks/quirks.h -- Per-game patch / hook surface.
//
// Each Wii game ported through this recompiler can register a quirks_t with
// game-specific:
//   * Worker thread entry VAs (for OSResumeThread real-host-thread dispatch)
//   * Safe-vtable target VAs (object-pointer slots whose ctor never ran)
//   * Low-memory pokes (specific (va, value) pairs the game reads early)
//   * An optional post-OSInit hook for anything else weird
//
// At runtime, exactly one quirks_t is "active". By default it's
// `quirks_none`, which is fully inert -- nothing patched, no fake vtables,
// no pokes. To enable a per-game profile, the program either:
//   1. Calls quirks_set_active(&quirks_<name>) from main(), OR
//   2. Sets RECOMP_QUIRKS_PROFILE=<name> in the environment before main runs.
//
// Adding a new game: drop quirks/<game>.c, define a `quirks_t quirks_<game>`,
// and (optionally) a registration helper. Don't put any patches into
// sdk/os.c or templates/runtime.c -- they MUST go here.

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t va;
    uint32_t value;
} quirks_lowmem_poke_t;

typedef struct {
    const char *name;            // short identifier, e.g. "robox"
    const char *description;     // for log lines

    // Worker thread entries that should run on a real host thread when
    // OSResumeThread fires. Anything not listed here is run inline (or
    // skipped, depending on RECOMP_RUN_THREADS).
    const uint32_t *worker_entries;
    size_t          worker_entry_count;

    // Object-pointer VAs that should be patched to point at the safe vtable.
    // OSInit lays the vtable down at SAFE_VTABLE_VA (0x80003500) and writes
    // SAFE_VTABLE_VA into each of these. If left empty, NO vtable is laid
    // down at all -- it's only installed when at least one target exists.
    const uint32_t *safe_vtable_targets;
    size_t          safe_vtable_target_count;

    // Specific (VA, value) pairs to poke after OSInit's standard setup.
    // Use this for things like seeding IPC buffer pointers or game-specific
    // sentinels that the recompiler can't compute on its own.
    const quirks_lowmem_poke_t *lowmem_pokes;
    size_t                      lowmem_poke_count;

    // Called at the end of hle_OSInit, after all of the above. Use for
    // anything that doesn't fit the table-driven hooks.
    void (*osinit_post_hook)(void);
} quirks_t;

// The currently-active quirks profile. Returns NULL when no profile is set
// (everything inert). Callers MUST tolerate NULL.
const quirks_t *quirks_active(void);

// Set the active profile programmatically. Pass NULL to disable.
void quirks_set_active(const quirks_t *q);

// Look up a profile by name (matches q->name). Returns NULL if not found.
// Used by the env-var path: RECOMP_QUIRKS_PROFILE=<name>.
const quirks_t *quirks_find_by_name(const char *name);

// Built-in inert profile. Active by default.
extern const quirks_t quirks_none;

#ifdef __cplusplus
}
#endif
