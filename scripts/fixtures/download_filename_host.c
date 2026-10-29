/* SPDX-License-Identifier: MIT */
#include "download_filename.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned cases;
static void check(const char *hint, const char *url, const char *expected)
{
    unsigned char storage[DOWNLOAD_FILENAME_MAX + 5U];
    memset(storage, 0xA5, sizeof(storage));
    char *out = (char *)storage + 1;
    assert(!download_filename(hint, url, out, DOWNLOAD_FILENAME_MAX + 1U));
    assert(!strcmp(out, expected));
    assert(strlen(out) <= DOWNLOAD_FILENAME_MAX);
    assert(!strchr(out, '/') && !strchr(out, '\\'));
    assert(storage[0] == 0xA5 && storage[DOWNLOAD_FILENAME_MAX + 2U] == 0xA5);
    ++cases;
}
int main(void)
{
    check(NULL, "https://host.test/files/Apple%20II%2B.po?token=x#frag", "Apple II+.po");
    check("", "https://host.test/a+plus.zip", "a+plus.zip");
    check("chosen.dsk", "https://host.test/other.zip", "chosen.dsk");
    check("../../escape.po", NULL, "escape.po");
    check("C:\\private\\escape.zip", NULL, "escape.zip");
    check("C:escape.zip", NULL, "C_escape.zip");
    check("..", NULL, "download.bin");
    check(". . hidden.zip. ", NULL, "hidden.zip");
    check("bad<>:\"|?*.zip", NULL, "bad_______.zip");
    check("control\001\177.zip", NULL, "control__.zip");
    check("caf\303\251.zip", NULL, "caf__.zip");
    check("Con.txt", NULL, "_Con.txt");
    check("NUL", NULL, "_NUL");
    check("PRN .zip", NULL, "_PRN .zip");
    check("AUX.tar.gz", NULL, "_AUX.tar.gz");
    check("COM1.po", NULL, "_COM1.po");
    check("lpt9.dsk", NULL, "_lpt9.dsk");
    check("COM0.po", NULL, "COM0.po");
    check("conin$.txt", NULL, "_conin$.txt");
    check("CLOCK$", NULL, "_CLOCK$");
    check(NULL, "https://host.test", "download.bin");
    check(NULL, "https://host.test/", "download.bin");
    check(NULL, "https://host.test?path=/fake.zip", "download.bin");
    check(NULL, "https://host.test/%2e%2e", "download.bin");
    check(NULL, "https://host.test/name%2fescape.zip", "name_escape.zip");
    check(NULL, "https://host.test/name%00.bin", "name_.bin");
    check(NULL, "https://host.test/bad%XX.zip", "bad%XX.zip");
    check(NULL, "https://host.test/quote%22.zip", "quote_.zip");
    check(NULL, NULL, "download.bin");
    char long_name[301], out[DOWNLOAD_FILENAME_MAX + 1U];
    memset(long_name, 'a', 280); strcpy(long_name + 280, ".zip");
    assert(!download_filename(long_name, NULL, out, sizeof(out)));
    assert(strlen(out) == DOWNLOAD_FILENAME_MAX && !strcmp(out + DOWNLOAD_FILENAME_MAX - 4U, ".zip"));
    ++cases;
    char tiny[12]; memset(tiny, 0x5A, sizeof(tiny));
    assert(download_filename("file.zip", NULL, tiny, sizeof(tiny)) < 0 && tiny[0] == 0);
    assert(download_filename("file.zip", NULL, NULL, 0) < 0);
    ++cases;
    printf("PASS %u download filename cases: paths, escaped bytes, FAT names, bounds and extension preservation\n", cases);
    return 0;
}
