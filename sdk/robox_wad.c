/* sdk/robox_wad.c -- WAD -> playable install. See robox_wad.h for the layout
 * of a Robox WAD and why only content 1 gets decompressed.
 *
 * Self-contained on purpose: AES, SHA-1, LZ11 and the U8 walker are all here
 * rather than pulled from a crypto library. The whole job is one AES-128-CBC
 * mode, one hash, one LZ variant and one archive format, and the port already
 * refuses outside dependencies elsewhere (stb_vorbis is vendored the same way).
 * A library would be more code to ship, not less.
 */
#include "robox_wad.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* hle.h has the same macro, but including it would drag in the whole recomp
 * runtime header and cost this file the property that makes it easy to trust:
 * it builds and runs on its own against a real WAD (tools/test_wad_extract.c),
 * with no guest state, no SDL and no GL anywhere in the link. */
#if defined(_WIN32)
#  include <direct.h>
#  define wad_mkdir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  define wad_mkdir(p) mkdir((p), 0755)
#endif

/* ------------------------------------------------------------------------ */
/* small helpers                                                             */
/* ------------------------------------------------------------------------ */

static void set_err(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

/* WAD sections are padded to 64 bytes; encrypted content is a whole number of
 * AES blocks. Both roundings are needed and they are not the same one. */
static uint64_t align_up(uint64_t v, uint64_t a)
{
    uint64_t r = v % a;
    return r ? v + (a - r) : v;
}

/* mkdir -p. The U8 archives nest (HomeButton2/, HomeButtonSe/), and the parent
 * may or may not already exist depending on which content wrote it first. */
static int mkdir_p(const char *path)
{
    char tmp[1024];
    size_t n = strlen(path);
    if (n >= sizeof tmp) return -1;
    memcpy(tmp, path, n + 1);

    for (char *p = tmp + 1; *p; ++p) {
        if (*p != '/' && *p != '\\') continue;
        char sep = *p;
        *p = 0;
        wad_mkdir(tmp);          /* already-exists is fine, and unreported */
        *p = sep;
    }
    wad_mkdir(tmp);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* AES-128, decryption only                                                  */
/* ------------------------------------------------------------------------ */

static const uint8_t AES_RSBOX[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

/* Forward S-box is needed too: key expansion uses it even when decrypting. */
static const uint8_t AES_SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t AES_RCON[11] = {
    0x8d,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36
};

typedef struct { uint8_t rk[176]; } aes128;

static void aes128_init(aes128 *a, const uint8_t key[16])
{
    memcpy(a->rk, key, 16);
    for (int i = 4; i < 44; ++i) {
        uint8_t t[4];
        memcpy(t, a->rk + (i - 1) * 4, 4);
        if (i % 4 == 0) {
            uint8_t s = t[0];
            t[0] = (uint8_t)(AES_SBOX[t[1]] ^ AES_RCON[i / 4]);
            t[1] = AES_SBOX[t[2]];
            t[2] = AES_SBOX[t[3]];
            t[3] = AES_SBOX[s];
        }
        for (int j = 0; j < 4; ++j)
            a->rk[i * 4 + j] = (uint8_t)(a->rk[(i - 4) * 4 + j] ^ t[j]);
    }
}

static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1b : 0));
        b >>= 1;
    }
    return r;
}

/* State bytes are column-major: s[c*4 + r]. InvShiftRows rotates row r right
 * by r; InvMixColumns works a column at a time. */
static void aes128_decrypt_block(const aes128 *a, uint8_t s[16])
{
    for (int i = 0; i < 16; ++i) s[i] ^= a->rk[160 + i];

    for (int round = 9; round >= 0; --round) {
        uint8_t t;
        /* InvShiftRows */
        t = s[13]; s[13] = s[9];  s[9]  = s[5];  s[5]  = s[1];  s[1]  = t;
        t = s[2];  s[2]  = s[10]; s[10] = t;
        t = s[6];  s[6]  = s[14]; s[14] = t;
        t = s[3];  s[3]  = s[7];  s[7]  = s[11]; s[11] = s[15]; s[15] = t;

        for (int i = 0; i < 16; ++i) s[i] = AES_RSBOX[s[i]];
        for (int i = 0; i < 16; ++i) s[i] ^= a->rk[round * 16 + i];

        if (round == 0) break;      /* final round has no InvMixColumns */

        for (int c = 0; c < 4; ++c) {
            uint8_t *p = s + c * 4;
            uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
            p[0] = (uint8_t)(gmul(a0,14) ^ gmul(a1,11) ^ gmul(a2,13) ^ gmul(a3, 9));
            p[1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1,14) ^ gmul(a2,11) ^ gmul(a3,13));
            p[2] = (uint8_t)(gmul(a0,13) ^ gmul(a1, 9) ^ gmul(a2,14) ^ gmul(a3,11));
            p[3] = (uint8_t)(gmul(a0,11) ^ gmul(a1,13) ^ gmul(a2, 9) ^ gmul(a3,14));
        }
    }
}

/* In-place CBC decrypt. `n` must be a multiple of 16. */
static void aes128_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16],
                               uint8_t *buf, size_t n)
{
    aes128 a;
    aes128_init(&a, key);

    uint8_t chain[16], next[16];
    memcpy(chain, iv, 16);

    for (size_t off = 0; off + 16 <= n; off += 16) {
        memcpy(next, buf + off, 16);              /* ciphertext feeds forward */
        aes128_decrypt_block(&a, buf + off);
        for (int i = 0; i < 16; ++i) buf[off + i] ^= chain[i];
        memcpy(chain, next, 16);
    }
}

/* ------------------------------------------------------------------------ */
/* SHA-1                                                                     */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint32_t h[5];
    uint64_t total;
    uint8_t  buf[64];
    size_t   n;
} sha1_ctx;

static void sha1_init(sha1_ctx *c)
{
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0;
    c->total = 0; c->n = 0;
}

static void sha1_block(sha1_ctx *c, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = be32(p + i * 4);
    for (int i = 16; i < 80; ++i) {
        uint32_t v = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
        w[i] = (v << 1) | (v >> 31);
    }
    uint32_t a = c->h[0], b = c->h[1], d = c->h[2], e = c->h[3], f = c->h[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t k, t;
        if      (i < 20) { t = (b & d) | (~b & e);             k = 0x5A827999; }
        else if (i < 40) { t = b ^ d ^ e;                      k = 0x6ED9EBA1; }
        else if (i < 60) { t = (b & d) | (b & e) | (d & e);    k = 0x8F1BBCDC; }
        else             { t = b ^ d ^ e;                      k = 0xCA62C1D6; }
        uint32_t tmp = ((a << 5) | (a >> 27)) + t + f + k + w[i];
        f = e; e = d; d = (b << 30) | (b >> 2); b = a; a = tmp;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += d; c->h[3] += e; c->h[4] += f;
}

static void sha1_update(sha1_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    c->total += len;
    while (len) {
        size_t take = 64 - c->n;
        if (take > len) take = len;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; len -= take;
        if (c->n == 64) { sha1_block(c, c->buf); c->n = 0; }
    }
}

static void sha1_final(sha1_ctx *c, uint8_t out[20])
{
    uint64_t bits = c->total * 8;
    static const uint8_t pad0 = 0x80;
    sha1_update(c, &pad0, 1);
    /* total is now wrong by the padding, but only the saved `bits` is used. */
    static const uint8_t zero = 0;
    while (c->n != 56) sha1_update(c, &zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - i * 8));
    sha1_update(c, len, 8);
    for (int i = 0; i < 5; ++i) {
        out[i*4+0] = (uint8_t)(c->h[i] >> 24);
        out[i*4+1] = (uint8_t)(c->h[i] >> 16);
        out[i*4+2] = (uint8_t)(c->h[i] >> 8);
        out[i*4+3] = (uint8_t)(c->h[i]);
    }
}

static void sha1_hex(const uint8_t d[20], char out[41])
{
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < 20; ++i) {
        out[i*2+0] = H[d[i] >> 4];
        out[i*2+1] = H[d[i] & 15];
    }
    out[40] = 0;
}

int robox_wad_file_sha1(const char *path, char out[41])
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    sha1_ctx c; sha1_init(&c);
    static uint8_t buf[64 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) sha1_update(&c, buf, n);
    fclose(f);
    uint8_t d[20]; sha1_final(&c, d); sha1_hex(d, out);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* LZ11                                                                      */
/* ------------------------------------------------------------------------ */

/* Header is 0x11 then a 24-bit little-endian decompressed size; a zero there
 * means the size is a 32-bit field that follows (Robox never uses that form,
 * but other LyN-era titles do and it costs four lines to accept).
 *
 * Then: one flag byte, MSB first, eight symbols per flag. A clear bit is a
 * literal. A set bit is a back-reference whose width depends on the top nibble
 * of the first byte -- 0 means a 3-byte form with length+0x11, 1 means a
 * 4-byte form with length+0x111, anything else is the 2-byte form with the
 * nibble itself as length-1. All three carry a 12-bit displacement, stored
 * one less than the real distance. */
uint8_t *robox_lz11_decompress(const uint8_t *src, size_t src_size,
                               size_t *out_size, char *err, size_t err_size)
{
    if (src_size < 4 || src[0] != 0x11) {
        set_err(err, err_size, "not LZ11 data (first byte 0x%02x)",
                src_size ? src[0] : 0);
        return NULL;
    }

    size_t p = 4;
    size_t n = (size_t)src[1] | ((size_t)src[2] << 8) | ((size_t)src[3] << 16);
    if (n == 0) {
        if (src_size < 8) { set_err(err, err_size, "LZ11 header truncated"); return NULL; }
        n = (size_t)src[4] | ((size_t)src[5] << 8) |
            ((size_t)src[6] << 16) | ((size_t)src[7] << 24);
        p = 8;
    }
    if (n == 0 || n > 64u * 1024u * 1024u) {
        set_err(err, err_size, "LZ11 size %zu out of range", n);
        return NULL;
    }

    uint8_t *out = (uint8_t *)malloc(n);
    if (!out) { set_err(err, err_size, "out of memory (%zu bytes)", n); return NULL; }

    size_t w = 0;
    while (w < n) {
        if (p >= src_size) goto truncated;
        uint8_t flags = src[p++];

        for (int bit = 0; bit < 8 && w < n; ++bit) {
            if (!(flags & (0x80 >> bit))) {
                if (p >= src_size) goto truncated;
                out[w++] = src[p++];
                continue;
            }

            if (p >= src_size) goto truncated;
            uint8_t b0 = src[p];
            unsigned ind = (unsigned)(b0 >> 4);
            size_t len, disp;

            if (ind == 0) {
                if (p + 3 > src_size) goto truncated;
                uint8_t b1 = src[p+1], b2 = src[p+2];
                p += 3;
                len  = (size_t)(((b0 & 0x0F) << 4) | (b1 >> 4)) + 0x11;
                disp = (size_t)(((b1 & 0x0F) << 8) | b2);
            } else if (ind == 1) {
                if (p + 4 > src_size) goto truncated;
                uint8_t b1 = src[p+1], b2 = src[p+2], b3 = src[p+3];
                p += 4;
                len  = (size_t)(((size_t)(b0 & 0x0F) << 12) |
                                ((size_t)b1 << 4) | (b2 >> 4)) + 0x111;
                disp = (size_t)(((b2 & 0x0F) << 8) | b3);
            } else {
                if (p + 2 > src_size) goto truncated;
                uint8_t b1 = src[p+1];
                p += 2;
                len  = ind + 1;
                disp = (size_t)(((b0 & 0x0F) << 8) | b1);
            }

            if (disp + 1 > w) {
                set_err(err, err_size,
                        "LZ11 back-reference before start of output at %zu", w);
                free(out);
                return NULL;
            }
            size_t from = w - disp - 1;
            /* Overlapping copies are legal and load-bearing: a run is encoded
             * as a reference one byte back. Must stay byte-at-a-time. */
            for (size_t i = 0; i < len && w < n; ++i) out[w++] = out[from + i];
        }
    }

    if (out_size) *out_size = n;
    return out;

truncated:
    set_err(err, err_size, "LZ11 stream truncated at %zu of %zu bytes", w, n);
    free(out);
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* U8 archives                                                               */
/* ------------------------------------------------------------------------ */

#define U8_MAGIC 0x55AA382Du

/* Node table: 12 bytes each. Byte 0 is the type (1 = directory), the low 24
 * bits of the first word are the offset of the name in the string table. For a
 * file, offset/size are the payload. For a directory, `offset` is the parent
 * index and `size` is the index of the first node *after* the directory --
 * which is what makes an iterative walk with a scope stack work. */
static int u8_extract(const uint8_t *u8, size_t u8_size, const char *dest,
                      robox_wad_progress_fn progress, void *user,
                      const char *stage, int pct_lo, int pct_hi,
                      char *err, size_t err_size)
{
    if (u8_size < 32 || be32(u8) != U8_MAGIC) {
        set_err(err, err_size, "not a U8 archive");
        return -1;
    }
    uint32_t root_off = be32(u8 + 4);
    if (root_off + 12 > u8_size) {
        set_err(err, err_size, "U8 root node out of range");
        return -1;
    }
    uint32_t count = be32(u8 + root_off + 8);
    if (!count || (uint64_t)root_off + (uint64_t)count * 12 > u8_size) {
        set_err(err, err_size, "U8 node table out of range (%u nodes)", count);
        return -1;
    }
    const uint8_t *nodes = u8 + root_off;
    const uint8_t *strtab = nodes + (size_t)count * 12;
    size_t strtab_max = u8_size - (size_t)(strtab - u8);

    /* Scope stack: (path length to restore, node index this directory ends at).
     * Depth 16 is far past anything these archives use (deepest is 2). */
    struct { size_t path_len; uint32_t end; } stack[16];
    int depth = 0;

    char path[1024];
    int path_len = snprintf(path, sizeof path, "%s", dest);
    if (path_len < 0 || (size_t)path_len >= sizeof path) {
        set_err(err, err_size, "destination path too long");
        return -1;
    }
    mkdir_p(path);

    for (uint32_t i = 1; i < count; ++i) {
        while (depth > 0 && i >= stack[depth - 1].end) {
            path_len = (int)stack[depth - 1].path_len;
            path[path_len] = 0;
            --depth;
        }

        const uint8_t *nd = nodes + (size_t)i * 12;
        uint8_t  type     = nd[0];
        uint32_t name_off = be32(nd) & 0x00FFFFFFu;
        uint32_t off      = be32(nd + 4);
        uint32_t size     = be32(nd + 8);

        if (name_off >= strtab_max) {
            set_err(err, err_size, "U8 node %u name out of range", i);
            return -1;
        }
        const char *name = (const char *)strtab + name_off;
        size_t name_len = strnlen(name, strtab_max - name_off);
        if (name_len == strtab_max - name_off) {
            set_err(err, err_size, "U8 node %u name unterminated", i);
            return -1;
        }

        /* The root of some contents is named "." -- a real node, but it must
         * not become a directory called "." in the output or every path below
         * it gains a redundant component. Enter its scope, keep the path. */
        int anonymous = (name_len == 0 || strcmp(name, ".") == 0);

        int saved_len = path_len;
        if (!anonymous) {
            int n = snprintf(path + path_len, sizeof path - (size_t)path_len,
                             "/%s", name);
            if (n < 0 || (size_t)n >= sizeof path - (size_t)path_len) {
                set_err(err, err_size, "U8 path too long at node %u", i);
                return -1;
            }
            path_len += n;
        }

        if (type == 1) {
            if (depth >= (int)(sizeof stack / sizeof stack[0])) {
                set_err(err, err_size, "U8 nesting too deep at node %u", i);
                return -1;
            }
            if (!anonymous) mkdir_p(path);
            stack[depth].path_len = (size_t)saved_len;
            stack[depth].end      = size ? size : count;
            ++depth;
            continue;
        }

        if ((uint64_t)off + size > u8_size) {
            set_err(err, err_size, "U8 file '%s' payload out of range", name);
            return -1;
        }

        FILE *f = fopen(path, "wb");
        if (!f) {
            set_err(err, err_size, "cannot write '%s'", path);
            return -1;
        }
        if (size && fwrite(u8 + off, 1, size, f) != size) {
            fclose(f);
            set_err(err, err_size, "short write on '%s' (disk full?)", path);
            return -1;
        }
        fclose(f);

        path_len = saved_len;
        path[path_len] = 0;

        if (progress && (i % 64) == 0) {
            int pct = pct_lo + (int)((uint64_t)(pct_hi - pct_lo) * i / count);
            if (progress(stage, pct, user)) {
                set_err(err, err_size, "cancelled");
                return -1;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* WAD                                                                       */
/* ------------------------------------------------------------------------ */

int robox_wad_load_key(const char *path, uint8_t out[16],
                       char *err, size_t err_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        set_err(err, err_size, "cannot open key file '%s'", path);
        return -1;
    }
    size_t n = fread(out, 1, 16, f);
    /* A 17th byte means it is not a raw key -- most likely a text file with a
     * trailing newline, which would decrypt to garbage and produce a confusing
     * "content hash mismatch" three steps later. */
    int extra = fgetc(f) != EOF;
    fclose(f);

    if (n != 16 || extra) {
        set_err(err, err_size,
                "key file must be exactly 16 raw bytes (this one is %s)",
                n != 16 ? "shorter" : "longer");
        return -1;
    }
    return 0;
}

static uint8_t *read_whole_file(const char *path, size_t *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0) { fclose(f); return NULL; }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    if (size_out) *size_out = (size_t)n;
    return buf;
}

static int write_whole_file(const char *path, const uint8_t *data, size_t n,
                            char *err, size_t err_size)
{
    FILE *f = fopen(path, "wb");
    if (!f) { set_err(err, err_size, "cannot write '%s'", path); return -1; }
    if (n && fwrite(data, 1, n, f) != n) {
        fclose(f);
        set_err(err, err_size, "short write on '%s' (disk full?)", path);
        return -1;
    }
    fclose(f);
    return 0;
}

int robox_wad_extract(const char *wad_path, const char *dest_dir,
                      const uint8_t common_key[16],
                      robox_wad_progress_fn progress, void *user,
                      char *err, size_t err_size)
{
    int rc = -1;
    uint8_t *wad = NULL;
    size_t wad_size = 0;

    if (progress && progress("reading WAD", 0, user)) {
        set_err(err, err_size, "cancelled");
        return -1;
    }

    wad = read_whole_file(wad_path, &wad_size);
    if (!wad) {
        set_err(err, err_size, "cannot read '%s'", wad_path);
        return -1;
    }
    if (wad_size < 64) {
        set_err(err, err_size, "file is too small to be a WAD");
        goto done;
    }

    /* An installable WAD starts with a 0x20-byte header whose type is "Is".
     * Backup WADs ("Bk") hold a NAND save, not a title, and have no contents
     * to extract -- worth naming so the user is not left guessing. */
    if (be32(wad) != 0x20u) {
        set_err(err, err_size, "not a WAD (header size is 0x%x, expected 0x20)",
                be32(wad));
        goto done;
    }
    if (memcmp(wad + 4, "Is", 2) != 0) {
        set_err(err, err_size, "not an installable WAD (type '%c%c', expected 'Is')",
                wad[4], wad[5]);
        goto done;
    }

    uint64_t cert_size = be32(wad + 8);
    uint64_t tik_size  = be32(wad + 16);
    uint64_t tmd_size  = be32(wad + 20);

    uint64_t tik_pos = 64 + align_up(cert_size, 64);
    uint64_t tmd_pos = tik_pos + align_up(tik_size, 64);
    uint64_t dat_pos = tmd_pos + align_up(tmd_size, 64);

    if (tmd_pos + 484 > wad_size || dat_pos > wad_size) {
        set_err(err, err_size, "WAD is truncated (header points past end of file)");
        goto done;
    }

    /* Title key: 16 bytes at ticket+0x1BF, AES-CBC under the common key with
     * the title id (padded to 16) as IV. */
    uint8_t title_key[16];
    memcpy(title_key, wad + tik_pos + 447, 16);
    uint8_t tk_iv[16];
    memset(tk_iv, 0, sizeof tk_iv);
    memcpy(tk_iv, wad + tik_pos + 476, 8);
    aes128_cbc_decrypt(common_key, tk_iv, title_key, 16);

    uint32_t n_contents = be16(wad + tmd_pos + 478);
    if (!n_contents || n_contents > 512) {
        set_err(err, err_size, "TMD lists %u contents, which is not plausible",
                n_contents);
        goto done;
    }
    if (tmd_pos + 484 + (uint64_t)n_contents * 36 > wad_size) {
        set_err(err, err_size, "TMD content table runs past end of file");
        goto done;
    }

    char dol_path[1024];
    char assets_dir[1024];
    snprintf(dol_path,   sizeof dol_path,   "%s/%s", dest_dir, ROBOX_DOL_NAME);
    snprintf(assets_dir, sizeof assets_dir, "%s/Assets", dest_dir);
    mkdir_p(dest_dir);

    /* Contents 2..6 all extract into Assets/ and together make the tree the
     * game opens by name. Content 0 is the save banner and content 7 is a
     * second DOL the port never jumps to; both are skipped. */
    uint64_t pos = dat_pos;
    int wrote_dol = 0;

    for (uint32_t i = 0; i < n_contents; ++i) {
        const uint8_t *ent = wad + tmd_pos + 484 + (size_t)i * 36;
        uint32_t index    = be16(ent + 4);
        uint64_t size     = be64(ent + 8);
        const uint8_t *want_sha1 = ent + 16;

        uint64_t stored = align_up(size, 64);
        uint64_t padded = align_up(size, 16);
        if (pos + padded > wad_size) {
            set_err(err, err_size, "content %u runs past end of file", i);
            goto done;
        }

        /* Only decrypt what we are going to use. */
        int want = (i == 1) || (i >= 2 && i <= 6);
        if (!want) { pos += stored; continue; }

        int pct = 5 + (int)(70ull * i / n_contents);
        if (progress && progress(i == 1 ? "decrypting the DOL" : "decrypting assets",
                                 pct, user)) {
            set_err(err, err_size, "cancelled");
            goto done;
        }

        uint8_t *plain = (uint8_t *)malloc((size_t)padded);
        if (!plain) {
            set_err(err, err_size, "out of memory (%llu bytes)",
                    (unsigned long long)padded);
            goto done;
        }
        memcpy(plain, wad + pos, (size_t)padded);

        uint8_t iv[16];
        memset(iv, 0, sizeof iv);
        iv[0] = (uint8_t)(index >> 8);
        iv[1] = (uint8_t)(index & 0xFF);
        aes128_cbc_decrypt(title_key, iv, plain, (size_t)padded);

        /* The TMD hash covers the unpadded content. This is what catches a
         * wrong key, a truncated download and a corrupt dump, all before a
         * single byte is written. */
        sha1_ctx sc; sha1_init(&sc);
        sha1_update(&sc, plain, (size_t)size);
        uint8_t got[20]; sha1_final(&sc, got);
        if (memcmp(got, want_sha1, 20) != 0) {
            free(plain);
            if (i == 1) {
                set_err(err, err_size,
                        "content %u failed its hash check -- the key is probably "
                        "wrong, or the WAD is damaged", i);
            } else {
                set_err(err, err_size,
                        "content %u failed its hash check -- the WAD is damaged", i);
            }
            goto done;
        }

        if (i == 1) {
            /* Retail stores the DOL LZ11-compressed; a WAD repacked by modding
             * tools usually stores it raw. Sniff rather than assume, so both
             * work without the user having to know which they have. */
            uint8_t *dol = plain;
            size_t   dol_size = (size_t)size;
            uint8_t *decoded = NULL;

            if (size >= 4 && plain[0] == 0x11) {
                decoded = robox_lz11_decompress(plain, (size_t)size, &dol_size,
                                                err, err_size);
                if (!decoded) { free(plain); goto done; }
                dol = decoded;
            } else if (!(size >= 4 && be32(plain) == 0x100u)) {
                /* Format the bytes before releasing them, not after. */
                set_err(err, err_size,
                        "content 1 is neither an LZ11 blob nor a DOL "
                        "(starts %02x %02x %02x %02x)",
                        plain[0], plain[1], plain[2], plain[3]);
                free(plain);
                goto done;
            }

            int wrc = write_whole_file(dol_path, dol, dol_size, err, err_size);
            free(decoded);
            free(plain);
            if (wrc != 0) goto done;
            wrote_dol = 1;
        } else {
            int urc = u8_extract(plain, (size_t)size, assets_dir,
                                 progress, user, "extracting assets",
                                 pct, pct + 4, err, err_size);
            free(plain);
            if (urc != 0) goto done;
        }

        pos += stored;
    }

    if (!wrote_dol) {
        set_err(err, err_size, "WAD contains no DOL at content 1");
        goto done;
    }

    if (progress) progress("done", 100, user);
    rc = 0;

done:
    free(wad);
    return rc;
}

/* ------------------------------------------------------------------------ */
/* install check                                                             */
/* ------------------------------------------------------------------------ */

int robox_wad_check_dol(const char *dol_path, char *err, size_t err_size)
{
    /* Deliberately says nothing about the file's name. The extractor writes
     * ROBOX_DOL_NAME, but a DOL picked through "locate files manually" can be
     * called anything -- reporting "no Robox USA.dol here" for a file the user
     * just chose by hand would be nonsense. robox_wad_have_install() names the
     * file itself, where the name really is fixed. */
    FILE *f = fopen(dol_path, "rb");
    if (!f) {
        set_err(err, err_size, "cannot open that file");
        return -1;
    }
    /* A DOL opens with the .text[0] file offset, which is always 0x100 -- the
     * header is exactly that long. Cheap way to reject a renamed file. */
    uint8_t head[4];
    size_t n = fread(head, 1, 4, f);
    fclose(f);
    if (n != 4 || be32(head) != 0x100u) {
        set_err(err, err_size, "that file is not a DOL");
        return -1;
    }
    return 0;
}

int robox_wad_check_assets(const char *assets_dir, char *err, size_t err_size)
{
    /* The game opens these by name early in boot; if they are missing the
     * install is partial and failing here beats failing inside guest code. */
    static const char *required[] = { "anim", "lev", "script", "sound" };
    char path[1024];

    for (size_t i = 0; i < sizeof required / sizeof required[0]; ++i) {
        snprintf(path, sizeof path, "%s/%s", assets_dir, required[i]);
        /* Portable "does this directory exist": opening a directory with
         * fopen fails on Windows, so probe with a rename to itself instead --
         * succeeds for anything that exists, changes nothing. */
        if (rename(path, path) != 0) {
            set_err(err, err_size, "that folder has no %s/ in it", required[i]);
            return -1;
        }
    }
    return 0;
}

int robox_wad_have_install(const char *dest_dir, char *err, size_t err_size)
{
    char path[1024];

    snprintf(path, sizeof path, "%s/%s", dest_dir, ROBOX_DOL_NAME);
    /* Existence is checked here rather than left to check_dol so the message
     * can name the file: in an install directory the name IS fixed, and "no
     * Robox USA.dol here" is the useful thing to say. */
    FILE *f = fopen(path, "rb");
    if (!f) {
        set_err(err, err_size, "no %s here", ROBOX_DOL_NAME);
        return -1;
    }
    fclose(f);
    if (robox_wad_check_dol(path, err, err_size) != 0) return -1;

    snprintf(path, sizeof path, "%s/Assets", dest_dir);
    return robox_wad_check_assets(path, err, err_size);
}
