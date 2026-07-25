/* tools/test_dls_build.c -- prove sdk/robox_dls.c against build_soundfont.py.
 *
 * The C port has to be byte-identical to the Python, because the Python's
 * output is the DLS the music mod has always loaded. Anything else is a
 * regression in how the game sounds, and it would be a quiet one.
 *
 * Build (MSYS2 mingw64):
 *     gcc -std=c11 -O1 -Wall -Wextra -I sdk \
 *         tools/test_dls_build.c sdk/robox_dls.c -o build/test_dls_build.exe
 *
 * Run:
 *     test_dls_build <robox.wt> <robox.pcm> <out.dls> <reference.dls>
 */
#include "robox_dls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    unsigned char *p = (unsigned char *)malloc((size_t)len);
    if (p && fread(p, 1, (size_t)len, f) != (size_t)len) { free(p); p = NULL; }
    fclose(f);
    if (p && n) *n = (size_t)len;
    return p;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s <robox.wt> <robox.pcm> <out.dls> <reference.dls>\n",
                argv[0]);
        return 2;
    }

    char err[512] = {0};
    if (robox_dls_build(argv[1], argv[2], argv[3], err, sizeof err) != 0) {
        printf("FAIL build: %s\n", err);
        return 1;
    }

    size_t an = 0, bn = 0;
    unsigned char *a = slurp(argv[3], &an);
    unsigned char *b = slurp(argv[4], &bn);
    if (!a || !b) { printf("FAIL: cannot read output or reference\n"); return 1; }

    printf("ours %zu bytes, reference %zu bytes\n", an, bn);
    if (an != bn) {
        printf("FAIL: size differs by %+lld\n", (long long)an - (long long)bn);
        return 1;
    }
    for (size_t i = 0; i < an; ++i) {
        if (a[i] == b[i]) continue;
        printf("FAIL: first difference at 0x%zx: ours %02x, reference %02x\n",
               i, a[i], b[i]);
        size_t s = i > 8 ? i - 8 : 0;
        printf("  ours     ");
        for (size_t j = s; j < s + 16 && j < an; ++j) printf("%02x ", a[j]);
        printf("\n  reference");
        for (size_t j = s; j < s + 16 && j < bn; ++j) printf("%02x ", b[j]);
        printf("\n");
        return 1;
    }

    printf("PASSED: byte-identical to build_soundfont.py\n");
    return 0;
}
