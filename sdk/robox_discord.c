#if !defined(__3DS__)  /* PICA200 has no OpenGL; sdk/gx_c3d.c + sdk/platform_3ds.c stand in */
// sdk/robox_discord.c -- Discord Rich Presence, no dependencies.
//
// Why not discord-rpc-master/: its CMake downloads rapidjson at *configure*
// time (so a build without network access fails), it is an archived C++14
// subproject, and it starts its own IO thread with a condvar to pace itself.
// The protocol it speaks is small enough to not be worth any of that, and the
// port already has a once-per-frame host tick to drive it from.
//
// The wire protocol, in full:
//   transport  Windows: named pipe \\.\pipe\discord-ipc-N   (N = 0..9)
//              Unix:    AF_UNIX stream socket $XDG_RUNTIME_DIR/discord-ipc-N
//   framing    <u32 opcode LE> <u32 payload length LE> <payload JSON>
//   opcodes    0 HANDSHAKE  1 FRAME  2 CLOSE  3 PING  4 PONG
//   sequence   -> op 0 {"v":1,"client_id":"..."}
//              <- op 1 {... "evt":"READY" ...}          (must wait for this)
//              -> op 1 {"cmd":"SET_ACTIVITY","nonce":..,"args":{"pid":..,
//                       "activity":{...}}}
//
// Neither the title nor the image is sent from here. Discord resolves both
// from the client_id below: the header ("Playing ROBOX Recompiled") is the
// application's *name* in the developer portal, and with no Rich Presence art
// assets uploaded the client falls back to the application *icon* for the
// large image. So the default activity carries no text of its own -- just a
// start timestamp, which renders as the elapsed-time line.

#include "robox_discord.h"

// ---------------------------------------------------------------------------
// Configuration.
// ---------------------------------------------------------------------------

// https://discord.com/developers -- application named "ROBOX Recompiled".
#define DISCORD_APP_ID "1529226348499960101"

// The two optional lines under the title. Empty on purpose: the title alone is
// what should show. Anything empty is left out of the payload entirely rather
// than sent as "", which Discord would render as a blank line.
// robox_discord_set_status() can fill either in at runtime.
#define DISCORD_DETAILS ""                    /* line 2, <= 128 bytes */
#define DISCORD_STATE   ""                    /* line 3, <= 128 bytes */

// Overrides the large image with a named asset from the portal's Rich Presence
// -> Art Assets page. Empty means "use the application icon", which is what the
// app is already set up for -- and naming an asset that has not been uploaded
// gets the whole activity rejected, so this stays empty unless art is added.
#define DISCORD_LARGE_IMAGE ""
#define DISCORD_LARGE_TEXT  "ROBOX Recompiled"

// ---------------------------------------------------------------------------

#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
#  define ROBOX_DISCORD_ENABLED 0      /* no IPC endpoint on either */
#elif defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
#  define ROBOX_DISCORD_ENABLED 1
#else
#  define ROBOX_DISCORD_ENABLED 0
#endif

#if !ROBOX_DISCORD_ENABLED

void robox_discord_init(void) {}
void robox_discord_tick(void) {}
void robox_discord_shutdown(void) {}
void robox_discord_set_status(const char *d, const char *s) { (void)d; (void)s; }

#else

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
typedef HANDLE conn_t;
#  define CONN_NONE INVALID_HANDLE_VALUE
#else
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
typedef int conn_t;
#  define CONN_NONE (-1)
#  ifndef MSG_NOSIGNAL
#    define MSG_NOSIGNAL 0             /* macOS: SO_NOSIGPIPE is set instead */
#  endif
#endif

#define OP_HANDSHAKE 0
#define OP_FRAME     1
#define OP_CLOSE     2
#define OP_PING      3
#define OP_PONG      4

#define TICK_MS     250u      /* how often the state machine does anything   */
#define RETRY_MS   5000u      /* backoff between connection attempts         */
#define REFRESH_MS 60000u     /* keepalive re-send; Discord allows 5 per 20s */

static int      g_on;                  /* mod enabled                        */
static conn_t   g_conn = CONN_NONE;
static int      g_ready;               /* READY dispatch seen                */
static uint64_t g_last_tick, g_last_try, g_last_send;
static unsigned g_nonce;
static int64_t  g_start_epoch;

static char g_details[160] = DISCORD_DETAILS;
static char g_state[160]   = DISCORD_STATE;
static char g_sent[336];               /* last pushed details\x01state       */

static unsigned char g_rx[4096];
static unsigned      g_rx_have;

// ---------------------------------------------------------------------------
// Platform glue.
// ---------------------------------------------------------------------------

static uint64_t now_ms(void) {
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

static int cur_pid(void) {
#if defined(_WIN32)
    return (int)GetCurrentProcessId();
#else
    return (int)getpid();
#endif
}

static void conn_close(void) {
    if (g_conn == CONN_NONE) return;
#if defined(_WIN32)
    CloseHandle(g_conn);
#else
    close(g_conn);
#endif
    g_conn   = CONN_NONE;
    g_ready  = 0;
    g_rx_have = 0;
    g_sent[0] = '\0';
}

#if !defined(_WIN32)
// Discord may be a native install, a Flatpak or a Snap, and each puts its
// socket somewhere different under the runtime dir.
static const char *const k_ipc_subdirs[] = {
    "",
    "app/com.discordapp.Discord/",
    "app/com.discordapp.DiscordCanary/",
    "snap.discord/",
    "snap.discord-canary/",
};
static const char *ipc_base_dir(void) {
    const char *v;
    if ((v = getenv("XDG_RUNTIME_DIR")) && *v) return v;
    if ((v = getenv("TMPDIR"))          && *v) return v;
    if ((v = getenv("TMP"))             && *v) return v;
    if ((v = getenv("TEMP"))            && *v) return v;
    return "/tmp";
}
#endif

// Try every endpoint Discord might be listening on. Fails fast and silently:
// "Discord is not running" is the common case, not an error.
static int conn_open(void) {
#if defined(_WIN32)
    for (int i = 0; i < 10; ++i) {
        char path[64];
        snprintf(path, sizeof path, "\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) { g_conn = h; return 1; }
    }
    return 0;
#else
    const char *base = ipc_base_dir();
    const unsigned nsub = (unsigned)(sizeof k_ipc_subdirs / sizeof k_ipc_subdirs[0]);
    for (unsigned s = 0; s < nsub; ++s) {
        for (int i = 0; i < 10; ++i) {
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof addr);
            addr.sun_family = AF_UNIX;
            if ((size_t)snprintf(addr.sun_path, sizeof addr.sun_path,
                                 "%s/%sdiscord-ipc-%d", base, k_ipc_subdirs[s], i)
                >= sizeof addr.sun_path)
                continue;                        /* path would be truncated */

            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) return 0;
#if defined(__APPLE__)
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
            if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
                g_conn = fd;
                return 1;
            }
            close(fd);
        }
    }
    return 0;
#endif
}

static int conn_write(const void *buf, unsigned len) {
    const char *p = (const char *)buf;
#if defined(_WIN32)
    while (len) {
        DWORD put = 0;
        if (!WriteFile(g_conn, p, len, &put, NULL) || put == 0) return 0;
        p += put; len -= put;
    }
    return 1;
#else
    // The fd is non-blocking so a full socket buffer surfaces as EAGAIN.
    // Payloads here are well under a kilobyte and the buffer is 64 KB+, so a
    // short bounded retry is enough -- and beats blocking the render thread.
    for (int spin = 0; len && spin < 1000; ) {
        ssize_t put = send(g_conn, p, len, MSG_NOSIGNAL);
        if (put > 0) { p += put; len -= (unsigned)put; spin = 0; continue; }
        if (put < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            ++spin; continue;
        }
        return 0;
    }
    return len == 0;
#endif
}

// Non-blocking. Returns bytes read, 0 for "nothing pending", -1 for a dead
// connection.
static int conn_read(void *buf, unsigned cap) {
#if defined(_WIN32)
    DWORD avail = 0;
    if (!PeekNamedPipe(g_conn, NULL, 0, NULL, &avail, NULL)) return -1;
    if (avail == 0) return 0;
    if (avail > cap) avail = cap;
    DWORD got = 0;
    if (!ReadFile(g_conn, buf, avail, &got, NULL)) return -1;
    return (int)got;
#else
    ssize_t got = recv(g_conn, buf, cap, 0);
    if (got > 0) return (int)got;
    if (got == 0) return -1;                       /* peer closed */
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
#endif
}

// ---------------------------------------------------------------------------
// Framing.
// ---------------------------------------------------------------------------

static int frame_send(uint32_t op, const char *json) {
    size_t len = strlen(json);
    if (len > 8192) return 0;

    unsigned char buf[8192 + 8];
    buf[0] = (unsigned char)(op      ); buf[1] = (unsigned char)(op >>  8);
    buf[2] = (unsigned char)(op >> 16); buf[3] = (unsigned char)(op >> 24);
    buf[4] = (unsigned char)(len      ); buf[5] = (unsigned char)(len >>  8);
    buf[6] = (unsigned char)(len >> 16); buf[7] = (unsigned char)(len >> 24);
    memcpy(buf + 8, json, len);
    return conn_write(buf, (unsigned)(len + 8));
}

// Drain whatever Discord has sent and act on complete frames. Returns 0 if the
// connection should be dropped.
//
// No JSON parser: the only thing this needs off the wire is "did the READY
// dispatch arrive", and a substring test answers that. Anything unrecognised
// is deliberately ignored rather than treated as an error -- Discord adds
// fields and events over time and none of them are our business.
static int pump_rx(void) {
    for (;;) {
        if (g_rx_have < sizeof g_rx) {
            int got = conn_read(g_rx + g_rx_have,
                                (unsigned)(sizeof g_rx - g_rx_have));
            if (got < 0) return 0;
            g_rx_have += (unsigned)got;
            if (got == 0 && g_rx_have < 8) return 1;
        }

        if (g_rx_have < 8) return 1;

        uint32_t op  = (uint32_t)g_rx[0] | ((uint32_t)g_rx[1] << 8) |
                       ((uint32_t)g_rx[2] << 16) | ((uint32_t)g_rx[3] << 24);
        uint32_t len = (uint32_t)g_rx[4] | ((uint32_t)g_rx[5] << 8) |
                       ((uint32_t)g_rx[6] << 16) | ((uint32_t)g_rx[7] << 24);

        // A frame that cannot fit the buffer can never be consumed, so the
        // loop would spin forever waiting for it. Treat it as a desync.
        if (len > sizeof g_rx - 8 - 1) {
            fprintf(stderr, "[discord] oversized frame (%u bytes), dropping link\n",
                    (unsigned)len);
            return 0;
        }
        if (g_rx_have < 8 + len) return 1;         /* wait for the rest */

        char payload[sizeof g_rx];
        memcpy(payload, g_rx + 8, len);
        payload[len] = '\0';

        g_rx_have -= 8 + len;
        memmove(g_rx, g_rx + 8 + len, g_rx_have);

        switch (op) {
            case OP_FRAME:
                if (!g_ready && strstr(payload, "\"READY\"")) {
                    g_ready = 1;
                    fprintf(stderr, "[discord] ready\n");
                    fflush(stderr);
                } else if (strstr(payload, "\"ERROR\"")) {
                    fprintf(stderr, "[discord] error frame: %s\n", payload);
                    fflush(stderr);
                    return 0;
                }
                break;
            case OP_PING:
                if (!frame_send(OP_PONG, payload)) return 0;
                break;
            case OP_CLOSE:
                fprintf(stderr, "[discord] closed by client: %s\n", payload);
                fflush(stderr);
                return 0;
            default:
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// Presence payload.
// ---------------------------------------------------------------------------

static void json_escape(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    for (; *src && o + 7 < cap; ++src) {
        unsigned char c = (unsigned char)*src;
        switch (c) {
            case '"':  dst[o++] = '\\'; dst[o++] = '"';  break;
            case '\\': dst[o++] = '\\'; dst[o++] = '\\'; break;
            case '\n': dst[o++] = '\\'; dst[o++] = 'n';  break;
            case '\r': dst[o++] = '\\'; dst[o++] = 'r';  break;
            case '\t': dst[o++] = '\\'; dst[o++] = 't';  break;
            default:
                if (c < 0x20) o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", c);
                else          dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

/* Append to buf, keeping `*off` a valid offset even if the write truncates
 * (snprintf reports what it *would* have written, which would run *off past
 * the end and underflow the next size argument). */
static void append(char *buf, size_t cap, size_t *off, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    *off += (size_t)n;
    if (*off >= cap) *off = cap - 1;
}

static int push_presence(void) {
    /* Every field here is optional, and an empty one is omitted rather than
     * sent as "" -- Discord renders an empty string as a blank line. */
    char fields[1024];
    size_t o = 0;
    fields[0] = '\0';

    if (g_details[0]) {
        char d[352];
        json_escape(d, sizeof d, g_details);
        append(fields, sizeof fields, &o, "\"details\":\"%s\",", d);
    }
    if (g_state[0]) {
        char s[352];
        json_escape(s, sizeof s, g_state);
        append(fields, sizeof fields, &o, "\"state\":\"%s\",", s);
    }
    if (DISCORD_LARGE_IMAGE[0]) {
        append(fields, sizeof fields, &o,
               "\"assets\":{\"large_image\":\"%s\",\"large_text\":\"%s\"},",
               DISCORD_LARGE_IMAGE, DISCORD_LARGE_TEXT);
    }

    char msg[1400];
    snprintf(msg, sizeof msg,
             "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"%u\",\"args\":{\"pid\":%d,"
             "\"activity\":{%s\"timestamps\":{\"start\":%lld},\"instance\":true}}}",
             ++g_nonce, cur_pid(), fields, (long long)g_start_epoch);

    return frame_send(OP_FRAME, msg);
}

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

void robox_discord_set_status(const char *details, const char *state) {
    // Discord caps both at 128 bytes and rejects anything longer, so clamp
    // here rather than letting the activity be silently dropped.
    if (details) { strncpy(g_details, details, 128); g_details[128] = '\0'; }
    if (state)   { strncpy(g_state,   state,   128); g_state[128]   = '\0'; }
}

void robox_discord_init(void) {
    g_on          = 1;
    g_start_epoch = (int64_t)time(NULL);
    g_last_try    = 0;
    g_last_tick   = 0;
    atexit(robox_discord_shutdown);
    fprintf(stderr, "[discord] rich presence armed (app %s)\n", DISCORD_APP_ID);
    fflush(stderr);
}

void robox_discord_tick(void) {
    if (!g_on) return;

    uint64_t now = now_ms();
    if (now - g_last_tick < TICK_MS) return;
    g_last_tick = now;

    if (g_conn == CONN_NONE) {
        if (g_last_try && now - g_last_try < RETRY_MS) return;
        g_last_try = now;
        if (!conn_open()) return;                  /* Discord not running */

        char hs[128];
        snprintf(hs, sizeof hs, "{\"v\":1,\"client_id\":\"%s\"}", DISCORD_APP_ID);
        if (!frame_send(OP_HANDSHAKE, hs)) { conn_close(); return; }
        return;
    }

    if (!pump_rx()) { conn_close(); return; }
    if (!g_ready) return;                          /* SET_ACTIVITY before the
                                                    * READY dispatch is dropped
                                                    * on the floor by Discord */

    char sig[336];
    snprintf(sig, sizeof sig, "%s\x01%s", g_details, g_state);
    if (!strcmp(sig, g_sent) && now - g_last_send < REFRESH_MS) return;

    if (!push_presence()) { conn_close(); return; }
    memcpy(g_sent, sig, sizeof g_sent);
    g_last_send = now;
}

void robox_discord_shutdown(void) {
    if (!g_on) return;
    g_on = 0;
    if (g_conn == CONN_NONE) return;
    // SET_ACTIVITY with no "activity" key is how the presence is cleared.
    // Discord drops it on process exit anyway, but not instantly.
    char msg[128];
    snprintf(msg, sizeof msg,
             "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"%u\",\"args\":{\"pid\":%d}}",
             ++g_nonce, cur_pid());
    frame_send(OP_FRAME, msg);
    conn_close();
}

#endif /* ROBOX_DISCORD_ENABLED */

#endif /* !__3DS__ */
