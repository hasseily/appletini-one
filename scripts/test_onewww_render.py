"""Host check of the ONEWWW renderer.

Compiles the production parser and renderer, lays a page out, composes
views and decodes the SHR fields to PNG files in build/onewww-host/.

  python scripts/test_onewww_render.py
      The built-in home page, with basic checks.
  python scripts/test_onewww_render.py page.html https://site/ [tops...]
      A saved web page, for a look at the result. No checks.
Needs gcc on PATH or in the GCC variable.
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "ps_sources/frontend"
OUT = ROOT / "build/onewww-host"
GCC_DEFAULT = Path("C:/Xilinx/2025.2/Model_Composer/tps/mingw/10.0.0/"
                   "win64.o/nt/bin/gcc.exe")

MAIN = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "onewww_render.h"
static unsigned char fields[ONEWWW_FIELDS_BYTES];
int main(int argc, char **argv)
{
    /* argv: html file or "-", base url, then pairs: top, output file */
    const unsigned char *html = (const unsigned char *)onewww_home_page;
    size_t length = strlen(onewww_home_page);
    int internal = 1;
    if (strcmp(argv[1], "-") != 0) {
        FILE *f = fopen(argv[1], "rb");
        unsigned char *data = malloc(8u << 20);
        length = fread(data, 1, 8u << 20, f);
        fclose(f);
        html = data; internal = 0;
    }
    if (onewww_page_build(html, length, NULL, argv[2], internal) != 0) return 2;
    printf("%u %u %s\n", (unsigned)onewww_page_rows(),
           (unsigned)onewww_link_count(),
           onewww_link_count() ? onewww_link_href(0) : "-");
    for (int i = 3; i + 1 < argc; i += 2) {
        unsigned top; FILE *f;
        sscanf(argv[i], "%u", &top);
        onewww_compose(top, onewww_page_title(), "12%", 0, 300, 120, fields);
        f = fopen(argv[i + 1], "wb");
        fwrite(fields, 1, sizeof(fields), f);
        fclose(f);
    }
    return 0;
}
'''


def decode(fields):
    """Decodes two standard SHR fields as the card renderer does."""
    img = Image.new("RGB", (640, 400))
    px = img.load()
    for row in range(400):
        base = (row & 1) * 0x8000
        line = row >> 1
        scb = fields[base + 0x7D00 + line]
        pal = base + 0x7E00 + (scb & 15) * 32
        colours = []
        for i in range(16):
            v = fields[pal + 2 * i] | (fields[pal + 2 * i + 1] << 8)
            colours.append((((v >> 8) & 15) * 16, ((v >> 4) & 15) * 16,
                            (v & 15) * 16))
        data = fields[base + line * 160: base + line * 160 + 160]
        for x in range(640):
            byte = data[x >> 2]
            if scb & 0x80:
                value = (byte >> (6 - 2 * (x & 3))) & 3
                group = (8, 12, 0, 4)[x & 3]
                px[x, row] = colours[group + value]
            else:
                px[x, row] = colours[(byte >> 4) if (x & 2) == 0 else byte & 15]
    return img


def main():
    gcc = os.environ.get("GCC") or shutil.which("gcc") or str(GCC_DEFAULT)
    env = dict(os.environ)
    env["PATH"] = str(Path(gcc).parent) + os.pathsep + env["PATH"]
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "main.c").write_text(MAIN)
    exe = OUT / "onewww_host.exe"
    sources = ["onewww_render.c", "onewww_dom.c", "onewww_entities.c",
               "onewww_font_data.c", "onewww_home_page.c", "http_core.c"]
    subprocess.run([gcc, "-std=gnu99", "-O1", "-Wall", "-Wextra", "-I",
                    str(SRC), str(OUT / "main.c")] +
                   [str(SRC / s) for s in sources] + ["-o", str(exe)],
                   check=True, env=env)
    page = sys.argv[1] if len(sys.argv) > 1 else "-"
    base = sys.argv[2] if len(sys.argv) > 2 else "http://home.invalid/"
    tops = [int(t) for t in sys.argv[3:]] or [0, 384]
    name = "home" if page == "-" else Path(page).stem
    args = [page, base]
    for top in tops:
        args += [str(top), str(OUT / f"{name}{top}.bin")]
    result = subprocess.run([str(exe)] + args, check=True, env=env,
                            capture_output=True, text=True).stdout.split()
    rows, links = int(result[0]), int(result[1])
    for top in tops:
        fields = (OUT / f"{name}{top}.bin").read_bytes()
        decode(fields).save(OUT / f"{name}{top}.png")
        if page == "-":
            assert len(fields) == 0x10000 and fields[0x7DF8] == 1
            assert fields[0x7DFC:0x7E00] == bytes([0xD3, 0xC8, 0xD2, 0xB4])
    if page == "-":
        assert 600 < rows < 60000 and links == 6, (rows, links)
        assert result[2] == "https://appletini.org/", result[2]
    print(f"PASS: {rows} rows, {links} links; pictures in {OUT}")


if __name__ == "__main__":
    main()
