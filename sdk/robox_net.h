// sdk/robox_net.h -- fetch a file over HTTP(S), for mods.
//
// Exists so a Lua mod can use audio (or any data) it does not ship: point it
// at a URL, get a file on disk, and hand that to whatever wanted it. The
// download runs on its own thread and the caller polls, because the frame
// pump is inside guest execution and must never block on a socket.
//
// It fetches and caches -- it does not stream. A song is a few megabytes, the
// disk copy is reused on every later run, and every alternative (a streaming
// decoder, a ring buffer fed by a socket, stall handling when the network
// hiccups mid-bar) is a great deal more machinery for something that finishes
// in a second on the first launch and never runs again.
//
// MP3 in, WAV out: the payload is sniffed after download and transcoded with
// dr_mp3 if it is MPEG audio, so callers only ever deal with formats
// sdk/robox_wav.c already plays. WAV, Ogg and RBXS are written through
// untouched.
//
// WINDOWS ONLY for now (WinHTTP, which ships with the OS -- no new
// dependency, and TLS and redirects are handled for us). Everywhere else the
// fetch fails cleanly with "not supported on this platform" so a mod can fall
// back rather than break.
#ifndef ROBOX_NET_H
#define ROBOX_NET_H

#include <stdint.h>

enum {
    ROBOX_NET_WORKING = 0,
    ROBOX_NET_DONE    = 1,
    ROBOX_NET_ERROR   = 2
};

/* Start a download. Returns a handle, or -1 if no slot was free. `dest` is
 * created atomically: the bytes land in "<dest>.part" and are only moved into
 * place once the transfer (and any transcode) has finished, so a half-written
 * file is never mistaken for a cached one. */
int robox_net_fetch(const char *url, const char *dest);

/* ROBOX_NET_*. `got`/`total` may be NULL; `total` is 0 when the server does
 * not send a length. */
int robox_net_status(int handle, uint64_t *got, uint64_t *total);

/* Human-readable reason for ROBOX_NET_ERROR, else "". */
const char *robox_net_error(int handle);

/* Release a finished handle. Safe on a handle still working -- it is marked
 * and freed when the thread exits. */
void robox_net_release(int handle);

#endif /* ROBOX_NET_H */
