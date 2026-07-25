/* sdk/robox_setup.h -- first-run setup, the first thing the port ever shows.
 *
 * Runs before ppc_load_image(): its whole job is to make sure a DOL and an
 * Assets/ tree exist to load. It needs a window, so main() brings video_init()
 * up ahead of the image load; it does NOT play the splash, which stays where
 * it has always been -- after the game data is in place, just before the guest
 * entry, as the port's own credit.
 *
 * Everything it draws and plays is baked into the binary (see tools/bin2c.py),
 * because on a first run there is nothing else on disk to read.
 */
#pragma once
#ifndef ROBOX_SETUP_H
#define ROBOX_SETUP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Make `install_dir` runnable, asking the user for a WAD if it is not already.
 *
 * Returns 0 when the game can start. Returns non-zero only if the user closed
 * the window, in which case main() should exit without loading anything.
 *
 * A no-op (returns 0 immediately, no window, no music) when the install is
 * already good, so this costs nothing on every launch after the first. */
int robox_setup_run(const char *install_dir);

/* The DOL to load, valid once robox_setup_run() has returned 0.
 *
 * NULL means "the usual relative path" -- the file is in the working directory
 * and nothing special is needed. Non-NULL is an absolute path the user chose
 * through "locate files manually", where the DOL and the Assets tree can live
 * anywhere and need not be together. The Assets half of that choice is applied
 * by setting RECOMP_ASSETS, which robox_io.c already reads. */
const char *robox_setup_dol_path(void);

#ifdef __cplusplus
}
#endif
#endif /* ROBOX_SETUP_H */
