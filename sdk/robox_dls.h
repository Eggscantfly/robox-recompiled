/* sdk/robox_dls.h -- build mods/robox.dls from the game's own wavetable.
 *
 * A C port of build_soundfont.py's DLS half (the SF2 half is not ported: the
 * music mod loads the DLS, and nothing reads the SF2).
 *
 * Why this is in the program instead of a build step:
 *
 *   - The DLS is derived from Assets/music/robox.wt and robox.pcm, so it is
 *     retail game data and cannot ship in the repo or the release.
 *   - Without it the music mod is forced off, and the game falls back to
 *     decoding robox.wt/robox.pcm directly -- which comes out wrong on the PC
 *     port. The DLS is what makes the music sound right, so "generate it or
 *     the music is broken" is the actual situation, not a nice-to-have.
 *
 * Both inputs come out of the WAD, so the setup can build it on the user's
 * machine from data they already supplied.
 *
 * The output is byte-identical to what build_soundfont.py produces from the
 * same inputs; tools/test_dls_build.c checks that against a reference.
 */
#pragma once
#ifndef ROBOX_DLS_H
#define ROBOX_DLS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build `out_path` from `wt_path` + `pcm_path`. Returns 0 on success, non-zero
 * with a reason in `err`. Overwrites whatever is at out_path. */
int robox_dls_build(const char *wt_path, const char *pcm_path,
                    const char *out_path, char *err, size_t err_size);

/* Make sure <dir>/mods/robox.dls exists, building it from <assets_dir>/music/
 * if it does not. Returns 0 if the file is there afterwards. Cheap no-op when
 * it already exists, so it is safe to call on every launch.
 *
 * `assets_dir` must be the real Assets root, which is NOT always <dir>/Assets:
 * the setup's "locate files manually" path lets the DOL and the Assets tree
 * live anywhere. Pass NULL to fall back to RECOMP_ASSETS, then to
 * <dir>/Assets. Getting this wrong is quiet -- the build just fails, the mod
 * loader forces the music mod off, and the game falls back to its own broken
 * wavetable decode. */
int robox_dls_ensure(const char *dir, const char *assets_dir,
                     char *err, size_t err_size);

#ifdef __cplusplus
}
#endif
#endif /* ROBOX_DLS_H */
