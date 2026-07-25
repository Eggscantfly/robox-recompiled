// sdk/dvd_io.h -- Internal helpers exposed for quirks files that implement
// per-game asset loaders. Stable interface; the implementation lives in
// peripherals.c.

#pragma once

#include "hle.h"

#include <stdio.h>

#define DVD_MAX_HANDLES 64

struct dvd_handle_slot {
    FILE    *fp;
    uint32_t length;
    int      in_use;
};

extern struct dvd_handle_slot dvd_handles[DVD_MAX_HANDLES];

// Resolve a guest filename to a host FILE*. Tries $RECOMP_ASSETS_DIR,
// "Assets/", "../Assets/", and finally the cwd-relative literal path.
// Returns NULL on miss; on hit, *out_len is filled with the file size.
FILE *dvd_host_open(const char *guest_name, uint32_t *out_len);

// Reserve a non-zero handle index for a host FILE*. Returns 0 if the
// table is full (callers should treat 0 as failure and fclose the fp).
int   dvd_alloc_handle(FILE *fp, uint32_t len);
