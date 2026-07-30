// sdk/robox_net.c -- HTTP(S) fetch-and-cache for mods. See robox_net.h.

#include "robox_net.h"
#include "hle.h"        /* robox_mkdir */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO_FLOAT
#include "dr_mp3.h"

#define NET_MAX 4

typedef struct {
    int      used;
    int      state;                 /* ROBOX_NET_*                      */
    int      abandoned;             /* released while still working     */
    uint64_t got, total;
    char     url [1024];
    char     dest[512];
    char     err [256];
} net_job_t;

static net_job_t g_jobs[NET_MAX];

/* --- MP3 -> WAV ---------------------------------------------------------- */

static int looks_like_mp3(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t b[3] = { 0, 0, 0 };
    size_t n = fread(b, 1, 3, f);
    fclose(f);
    if (n < 3) return 0;
    if (!memcmp(b, "ID3", 3)) return 1;                    /* tagged      */
    if (b[0] == 0xFF && (b[1] & 0xE0) == 0xE0) return 1;   /* frame sync  */
    return 0;
}

static void wr32(FILE *f, uint32_t v) {
    fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f);
    fputc((v >> 16) & 0xff, f); fputc((v >> 24) & 0xff, f);
}
static void wr16(FILE *f, uint16_t v) {
    fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f);
}

/* Decode the whole file and write a plain 16-bit RIFF next to it.
 *
 * Whole-file rather than streaming on purpose: it happens once, at download,
 * and it means sdk/robox_wav.c never learns a third codec -- it just sees a
 * WAV, which is the path with the most mileage on it. A three-minute stereo
 * track lands around 30 MB, which is a fine trade for not threading a second
 * decoder through the resampler's state machine. */
static int mp3_to_wav(const char *src, const char *dst, char *err, size_t errcap) {
    drmp3_config cfg;
    drmp3_uint64 frames = 0;
    drmp3_int16 *pcm = drmp3_open_file_and_read_pcm_frames_s16(src, &cfg, &frames, NULL);
    if (!pcm || frames == 0) {
        if (pcm) drmp3_free(pcm, NULL);
        snprintf(err, errcap, "MP3 decode failed");
        return 0;
    }
    const uint32_t ch    = cfg.channels ? cfg.channels : 2;
    const uint32_t rate  = cfg.sampleRate ? cfg.sampleRate : 44100;
    const uint32_t bytes = (uint32_t)(frames * ch * 2);

    FILE *f = fopen(dst, "wb");
    if (!f) {
        drmp3_free(pcm, NULL);
        snprintf(err, errcap, "cannot write %s", dst);
        return 0;
    }
    fwrite("RIFF", 1, 4, f);  wr32(f, 36 + bytes);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);  wr32(f, 16);
    wr16(f, 1);               wr16(f, (uint16_t)ch);
    wr32(f, rate);            wr32(f, rate * ch * 2);
    wr16(f, (uint16_t)(ch * 2)); wr16(f, 16);
    fwrite("data", 1, 4, f);  wr32(f, bytes);
    fwrite(pcm, 1, bytes, f);
    fclose(f);
    drmp3_free(pcm, NULL);

    fprintf(stderr, "[NET] transcoded MP3 -> WAV: %u Hz, %u ch, %llu frames\n",
            rate, ch, (unsigned long long)frames);
    fflush(stderr);
    return 1;
}

/* --- the download -------------------------------------------------------- */

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

/* Split "https://host/path?q" into its parts. WinHttpCrackUrl needs wide
 * strings and a URL_COMPONENTS anyway, so do it by hand and keep it obvious. */
static int split_url(const char *url, int *https, char *host, size_t hostcap,
                     char *path, size_t pathcap) {
    const char *p = url;
    *https = 1;
    if      (!strncmp(p, "https://", 8)) { p += 8; *https = 1; }
    else if (!strncmp(p, "http://",  7)) { p += 7; *https = 0; }
    else return 0;

    const char *slash = strchr(p, '/');
    size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
    if (hlen == 0 || hlen >= hostcap) return 0;
    memcpy(host, p, hlen);
    host[hlen] = 0;
    snprintf(path, pathcap, "%s", slash ? slash : "/");
    return 1;
}

static void widen(const char *s, wchar_t *w, size_t cap) {
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, (int)cap);
}

static DWORD WINAPI net_thread(LPVOID arg) {
    net_job_t *j = (net_job_t *)arg;

    int https = 1;
    char host[256], path[1024];
    if (!split_url(j->url, &https, host, sizeof host, path, sizeof path)) {
        snprintf(j->err, sizeof j->err, "not an http:// or https:// URL");
        j->state = ROBOX_NET_ERROR;
        return 0;
    }

    wchar_t whost[256], wpath[1024];
    widen(host, whost, 256);
    widen(path, wpath, 1024);

    HINTERNET ses = NULL, con = NULL, req = NULL;
    FILE *out = NULL;
    char part[560];
    snprintf(part, sizeof part, "%s.part", j->dest);

    ses = WinHttpOpen(L"ROBOX-Recompiled/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { snprintf(j->err, sizeof j->err, "WinHttpOpen failed"); goto fail; }

    con = WinHttpConnect(ses, whost,
                         https ? INTERNET_DEFAULT_HTTPS_PORT
                               : INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!con) { snprintf(j->err, sizeof j->err, "cannot reach %s", host); goto fail; }

    req = WinHttpOpenRequest(con, L"GET", wpath, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                             https ? WINHTTP_FLAG_SECURE : 0);
    if (!req) { snprintf(j->err, sizeof j->err, "WinHttpOpenRequest failed"); goto fail; }

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        snprintf(j->err, sizeof j->err, "request failed (offline? blocked?)");
        goto fail;
    }

    {   /* Anything but 200 is a failure worth naming -- a 404 written to disk
         * as if it were audio is the confusing kind of bug. */
        DWORD code = 0, len = sizeof code;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
        if (code != 200) {
            snprintf(j->err, sizeof j->err, "server returned HTTP %lu", (unsigned long)code);
            goto fail;
        }
        DWORD clen = 0; len = sizeof clen;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &clen, &len, WINHTTP_NO_HEADER_INDEX))
            j->total = clen;
    }

    out = fopen(part, "wb");
    if (!out) { snprintf(j->err, sizeof j->err, "cannot write %s", part); goto fail; }

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) {
            snprintf(j->err, sizeof j->err, "connection dropped");
            goto fail;
        }
        if (avail == 0) break;
        char buf[16384];
        while (avail > 0) {
            DWORD want = avail > sizeof buf ? (DWORD)sizeof buf : avail, read = 0;
            if (!WinHttpReadData(req, buf, want, &read) || read == 0) {
                snprintf(j->err, sizeof j->err, "read failed");
                goto fail;
            }
            fwrite(buf, 1, read, out);
            j->got += read;
            avail -= read;
        }
    }
    fclose(out); out = NULL;
    WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
    req = con = ses = NULL;

    if (looks_like_mp3(part)) {
        if (!mp3_to_wav(part, j->dest, j->err, sizeof j->err)) {
            remove(part);
            j->state = ROBOX_NET_ERROR;
            return 0;
        }
        remove(part);
    } else {
        remove(j->dest);                 /* rename fails onto an existing file */
        if (rename(part, j->dest) != 0) {
            snprintf(j->err, sizeof j->err, "cannot move into place");
            remove(part);
            j->state = ROBOX_NET_ERROR;
            return 0;
        }
    }

    fprintf(stderr, "[NET] %s -> %s (%llu bytes)\n",
            j->url, j->dest, (unsigned long long)j->got);
    fflush(stderr);
    j->state = ROBOX_NET_DONE;
    return 0;

fail:
    if (out) { fclose(out); remove(part); }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    fprintf(stderr, "[NET] %s FAILED: %s\n", j->url, j->err);
    fflush(stderr);
    j->state = ROBOX_NET_ERROR;
    return 0;
}

int robox_net_fetch(const char *url, const char *dest) {
    if (!url || !dest) return -1;
    int h = -1;
    for (int i = 0; i < NET_MAX; ++i) if (!g_jobs[i].used) { h = i; break; }
    if (h < 0) return -1;

    net_job_t *j = &g_jobs[h];
    memset(j, 0, sizeof *j);
    j->used  = 1;
    j->state = ROBOX_NET_WORKING;
    snprintf(j->url,  sizeof j->url,  "%s", url);
    snprintf(j->dest, sizeof j->dest, "%s", dest);

    HANDLE t = CreateThread(NULL, 0, net_thread, j, 0, NULL);
    if (!t) {
        snprintf(j->err, sizeof j->err, "cannot start the download thread");
        j->state = ROBOX_NET_ERROR;
        return h;
    }
    CloseHandle(t);
    return h;
}

#else   /* not Windows: fail cleanly so a mod can fall back */

int robox_net_fetch(const char *url, const char *dest) {
    (void)url; (void)dest;
    for (int i = 0; i < NET_MAX; ++i) {
        if (g_jobs[i].used) continue;
        memset(&g_jobs[i], 0, sizeof g_jobs[i]);
        g_jobs[i].used  = 1;
        g_jobs[i].state = ROBOX_NET_ERROR;
        snprintf(g_jobs[i].err, sizeof g_jobs[i].err,
                 "downloads are Windows-only in this build");
        return i;
    }
    return -1;
}

#endif

int robox_net_status(int handle, uint64_t *got, uint64_t *total) {
    if (handle < 0 || handle >= NET_MAX || !g_jobs[handle].used) return ROBOX_NET_ERROR;
    if (got)   *got   = g_jobs[handle].got;
    if (total) *total = g_jobs[handle].total;
    return g_jobs[handle].state;
}

const char *robox_net_error(int handle) {
    if (handle < 0 || handle >= NET_MAX) return "bad handle";
    return g_jobs[handle].err;
}

void robox_net_release(int handle) {
    if (handle < 0 || handle >= NET_MAX) return;
    /* Only reclaim a slot whose thread has finished with it. */
    if (g_jobs[handle].state == ROBOX_NET_WORKING) g_jobs[handle].abandoned = 1;
    else                                           g_jobs[handle].used = 0;
}
