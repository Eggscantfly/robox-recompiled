/* tools/test_wad_extract.c -- prove sdk/robox_wad.c against real WADs.
 *
 * Links nothing but robox_wad.c and libc, which is the point: the extraction
 * path can be checked end to end without a window, a GL context or the guest.
 *
 * Build (MSYS2 mingw64):
 *     gcc -std=c11 -O1 -Wall -Wextra -I sdk \
 *         tools/test_wad_extract.c sdk/robox_wad.c -o build/test_wad_extract.exe
 *
 * Run:
 *     test_wad_extract <key.bin> <out-dir> <wad> [expected-dol-sha1] ...
 *
 * Each WAD is extracted into its own subdirectory of <out-dir>. If an expected
 * SHA-1 follows a WAD path, the DOL it produces must match it.
 */
#include "robox_wad.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int quiet;

static int on_progress(const char *stage, int pct, void *user)
{
    (void)user;
    if (!quiet) printf("    [%3d%%] %s\n", pct, stage);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr,
                "usage: %s <key.bin> <out-dir> <wad> [expected-sha1] ...\n",
                argv[0]);
        return 2;
    }
    quiet = getenv("TEST_QUIET") != NULL;

    char err[512] = {0};
    unsigned char key[16];
    if (robox_wad_load_key(argv[1], key, err, sizeof err) != 0) {
        fprintf(stderr, "key: %s\n", err);
        return 1;
    }
    printf("key loaded: %s\n\n", argv[1]);

    const char *out_root = argv[2];
    int failures = 0;
    int case_no = 0;

    for (int i = 3; i < argc; ++i) {
        const char *wad = argv[i];
        const char *want = NULL;
        if (i + 1 < argc && strlen(argv[i + 1]) == 40 &&
            strspn(argv[i + 1], "0123456789abcdef") == 40) {
            want = argv[++i];
        }

        char dest[1024];
        snprintf(dest, sizeof dest, "%s/case%d", out_root, ++case_no);
        printf("== %s\n   -> %s\n", wad, dest);

        if (robox_wad_extract(wad, dest, key, on_progress, NULL,
                              err, sizeof err) != 0) {
            printf("   FAIL extract: %s\n\n", err);
            ++failures;
            continue;
        }

        char dol[1024];
        snprintf(dol, sizeof dol, "%s/%s", dest, ROBOX_DOL_NAME);
        char got[41];
        if (robox_wad_file_sha1(dol, got) != 0) {
            printf("   FAIL: no DOL written\n\n");
            ++failures;
            continue;
        }
        printf("   DOL sha1 %s\n", got);
        if (want) {
            if (strcmp(got, want) == 0) {
                printf("   OK      matches expected\n");
            } else {
                printf("   FAIL    expected %s\n", want);
                ++failures;
            }
        }

        if (robox_wad_have_install(dest, err, sizeof err) == 0) {
            printf("   OK      install check passed\n");
        } else {
            printf("   FAIL    install check: %s\n", err);
            ++failures;
        }
        printf("\n");
    }

    printf("%s (%d case%s, %d failure%s)\n",
           failures ? "FAILED" : "PASSED",
           case_no, case_no == 1 ? "" : "s",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
