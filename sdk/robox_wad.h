/* sdk/robox_wad.h -- turn a retail Robox WAD into an install the port can run.
 *
 * The setup screen (sdk/robox_setup.c) is the only caller. Everything here is
 * host-side file work: no guest memory, no GL, no SDL, so it can be built and
 * tested on its own (see tools/test_wad_extract.c).
 *
 * What a Robox WAD actually holds, verified against a retail dump:
 *
 *   content 0  0x0000000B     244,096  save banner
 *   content 1  0x0000000C   1,042,304  the DOL, LZ11-compressed -> 2,069,056
 *   content 2  0x0000000D  32,318,672  U8 archive, 1,467 files (the game)
 *   content 3  0x00000003   1,015,434  U8, 7x strapImage_*_LZ.bin (LZ11 inside)
 *   content 4  0x00000004     112,448  U8, HomeButton3 (LZ10 inside)
 *   content 5  0x00000005   2,156,800  U8, HomeButton2+3 (LZ10/Huf8 inside)
 *   content 6  0x00000006     370,880  U8, HomeButtonSe (Huf8 inside)
 *   content 7  0x0000000E     328,960  second DOL, unused by the port
 *
 * Only content 1 is decompressed here. The LZ11/LZ10/Huf8 payloads inside
 * contents 3..6 are left exactly as they are on disc -- the game decompresses
 * those itself at runtime, and rewriting them would break the loaders that
 * expect a compressed blob.
 *
 * Contents 1..6 are AES-128-CBC encrypted under a per-title key, which is
 * itself encrypted under the Wii common key. This project ships no key: the
 * caller passes one the user supplied. See robox_setup.c for how it is asked
 * for and cached.
 */
#pragma once
#ifndef ROBOX_WAD_H
#define ROBOX_WAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SHA-1 of the USA DOL this recomp was generated from, so the setup can tell
 * the user they are running something other than what was translated. A
 * mismatch is deliberately a warning and not an error -- people mod this game
 * constantly, and a patched DOL must still boot. */
#define ROBOX_DOL_SHA1_USA "f015538de8373ee67742c102a68379717ae3779c"

/* The file the port loads at boot, relative to the install directory. */
#define ROBOX_DOL_NAME "Robox USA.dol"

/* Progress for the setup screen's bar. `stage` is a short label ("checking",
 * "assets"), `pct` is 0..100 across the whole job. Return non-zero to cancel;
 * the extract then fails with "cancelled" and leaves no partial DOL behind. */
typedef int (*robox_wad_progress_fn)(const char *stage, int pct, void *user);

/* Extract `wad_path` into `dest_dir`, producing dest_dir/"Robox USA.dol" and
 * dest_dir/Assets/... . `common_key` is the 16-byte Wii common key.
 *
 * Every content is SHA-1 checked against the TMD before it is written, so a
 * truncated download or a bad key is reported as such instead of surfacing
 * later as a mystery crash in the recompiled code.
 *
 * Returns 0 on success. On failure returns non-zero and writes a reason the
 * setup screen can show verbatim into `err`. */
int robox_wad_extract(const char *wad_path, const char *dest_dir,
                      const uint8_t common_key[16],
                      robox_wad_progress_fn progress, void *user,
                      char *err, size_t err_size);

/* Is `dest_dir` already a usable install? Checks the DOL is present and is a
 * plausible DOL, and that Assets/ has the directories the game opens by name.
 * Returns 0 if yes. This is what decides whether setup appears at all, and
 * what the "Files already extracted" button validates a chosen folder with. */
int robox_wad_have_install(const char *dest_dir, char *err, size_t err_size);

/* The two halves of that check, for the setup's "locate files manually" path
 * where the DOL and the Assets tree are chosen separately and may not live in
 * the same place. Both return 0 when the thing is usable. */
int robox_wad_check_dol(const char *dol_path, char *err, size_t err_size);
int robox_wad_check_assets(const char *assets_dir, char *err, size_t err_size);

/* Hex SHA-1 of a file into out[41]. Returns 0 on success. Exposed because the
 * setup reports whether the DOL is retail or modded. */
int robox_wad_file_sha1(const char *path, char out[41]);

/* Read a 16-byte key file (the user's key.bin). Returns 0 on success. Verifies
 * only the length -- whether the key is *correct* is proven by the title key
 * it produces decrypting content that matches its TMD hash, which is a far
 * better check than comparing the key against a constant. */
int robox_wad_load_key(const char *path, uint8_t out[16],
                       char *err, size_t err_size);

/* Standalone LZ11 decode, exposed for tools and tests. `out_size` receives the
 * decompressed length. Caller frees the returned buffer. NULL on failure. */
uint8_t *robox_lz11_decompress(const uint8_t *src, size_t src_size,
                               size_t *out_size, char *err, size_t err_size);

#ifdef __cplusplus
}
#endif
#endif /* ROBOX_WAD_H */
