"""Host check of the ONEWWW renderer.

Compiles the production parser and renderer, lays a page out, composes
views and writes them as PNG files in build/onewww-host/.

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
#include "net_image.h"
#include "onewww_cookie.h"
static uint32_t frame[ONEWWW_FRAME_PIXELS];
extern int (*onewww_pause)(void);
static int calls, stop_after;
static int stop_soon(void) { return ++calls >= stop_after; }
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
    /* A stop request in the parser, then in the layout, must leave an
     * empty page and no damage; the next build must be normal. */
    for (stop_after = 1; internal && stop_after <= 41; stop_after += 40) {
        calls = 0;
        onewww_pause = stop_soon;
        if (onewww_page_build(html, length, NULL, internal ? NULL : argv[2], internal) == 0 ||
            onewww_page_rows() != 0) return 3;
        onewww_pause = NULL;
    }
    /* The home page has no base address, as on the card. */
    if (onewww_page_parse(html, length, NULL, internal ? NULL : argv[2], internal) != 0) return 2;
    {   /* ONEWWW_CSS: files for the linked sheets, in order, ';' between */
        char *list = getenv("ONEWWW_CSS");
        for (unsigned i = 0; list != NULL && i < onewww_page_sheets(); i++) {
            char *stop;
            FILE *f;
            if (onewww_page_sheet_url(i) == NULL) continue;
            fprintf(stderr, "sheet %u: %s\n", i, onewww_page_sheet_url(i));
            if (*list == 0) break;
            stop = strchr(list, ';');
            if (stop != NULL) *stop = 0;
            f = fopen(list, "rb");
            if (f != NULL) {
                char *css = malloc(4u << 20);
                size_t n = fread(css, 1, 4u << 20, f);
                fclose(f);
                onewww_page_sheet_text(i, css, n);
                free(css);
            }
            list = stop != NULL ? stop + 1 : list + strlen(list);
        }
    }
    if (onewww_page_layout() != 0) return 2;
    if (getenv("ONEWWW_LIST") != NULL) {    /* the pictures that the page wants */
        for (unsigned i = 0; i < onewww_page_images(); i++) {
            if (onewww_page_image_url(i) != NULL) printf("%u %s\n", i, onewww_page_image_url(i));
        }
        return 0;
    }
    if (getenv("ONEWWW_IMG") != NULL) {     /* files <dir>/<number> */
        unsigned char *data = malloc(8u << 20);
        for (unsigned i = 0; i < onewww_page_images(); i++) {
            char path[512];
            unsigned char *rgba = NULL;
            unsigned w = 0, h = 0;
            size_t n = 0;
            FILE *f;
            if (onewww_page_image_url(i) == NULL) continue;
            snprintf(path, sizeof(path), "%s/%u", getenv("ONEWWW_IMG"), i);
            f = fopen(path, "rb");
            if (f != NULL) { n = fread(data, 1, 8u << 20, f); fclose(f); }
            if (n != 0 && net_image_decode(data, n, &rgba, &w, &h) == 0) {
                onewww_page_image_data(i, rgba, w, h);
            } else onewww_page_image_failed(i);
            free(rgba);
        }
        /* The page before stays on the screen while the new one is made. */
        if (onewww_page_rows() == 0 || onewww_page_layout() != 0) return 4;
    }
    if (getenv("ONEWWW_FORM") != NULL) {    /* cookies: scope, secure, removal */
        static http_url_t site, plain, other;
        char out[256];
        strcpy(site.host, "www.site.test"); strcpy(site.path, "/a/b"); site.tls = 1;
        plain = site; plain.tls = 0;
        strcpy(other.host, "evil.test"); strcpy(other.path, "/a/b"); other.tls = 1;
        onewww_cookie_clear();
        onewww_cookie_store(&site, "sid=abc; Path=/; Domain=site.test; Secure; HttpOnly");
        onewww_cookie_store(&site, "t=1; Path=/a");
        onewww_cookie_store(&site, "far=1; Path=/zzz");
        onewww_cookie_store(&site, "x=9; Domain=other.com");
        if (onewww_cookie_load(&site, out, sizeof(out)) == 0 || strcmp(out, "sid=abc; t=1") != 0) return 6;
        if (onewww_cookie_load(&plain, out, sizeof(out)) == 0 || strcmp(out, "t=1") != 0) return 6;
        if (onewww_cookie_load(&other, out, sizeof(out)) != 0) return 6;
        onewww_cookie_store(&site, "t=2; Path=/a");
        onewww_cookie_store(&site, "sid=; Path=/; Domain=site.test; Max-Age=0");
        if (onewww_cookie_load(&site, out, sizeof(out)) == 0 || strcmp(out, "t=2") != 0) return 6;
    }
    if (getenv("ONEWWW_FORM") != NULL) {    /* fill the fields, send each form */
        static char url[1024], body[2048];
        int post;
        for (unsigned l = 0; l < onewww_link_count(); l++) {
            const int field = onewww_field_of_link(l);
            switch (field < 0 ? 0 : onewww_field_type(field)) {
            case ONEWWW_FIELD_TEXT: onewww_field_set(field, "apple ii & more"); break;
            case ONEWWW_FIELD_PASSWORD: onewww_field_set(field, "s3cret"); break;
            case ONEWWW_FIELD_CHECK: case ONEWWW_FIELD_RADIO: case ONEWWW_FIELD_LIST:
                onewww_field_activate(field);
                break;
            default: break;
            }
        }
        if (onewww_page_layout() != 0) return 5;
        for (unsigned l = 0; l < onewww_link_count(); l++) {
            const int field = onewww_field_of_link(l);
            if (field >= 0 && onewww_field_type(field) == ONEWWW_FIELD_SEND &&
                onewww_field_submit(field, url, sizeof(url), body, sizeof(body), &post) == 0) {
                fprintf(stderr, "FORM %d %s|%s\n", post, url, body);
            }
        }
    }
    printf("%u %u %s\n", (unsigned)onewww_page_rows(),
           (unsigned)onewww_link_count(),
           onewww_link_count() ? onewww_link_href(0) : "-");
    for (int i = 3; i + 1 < argc; i += 2) {
        unsigned top; FILE *f;
        sscanf(argv[i], "%u", &top);
        onewww_compose(top, onewww_page_title(), "12%", 0, 300, 120, frame);
        f = fopen(argv[i + 1], "wb");
        fwrite(frame, 1, sizeof(frame), f);
        fclose(f);
    }
    return 0;
}
'''


def decode(frame):
    """A view is 640 x 400 pixels of 32 bits, 0xFFrrggbb."""
    return Image.frombuffer("RGBA", (640, 400), frame, "raw", "BGRA", 0, 1).convert("RGB")


FORM_PAGE = """<form action="/find" method="get">
<input type="hidden" name="src" value="box">
<input type="text" name="q" value="old">
<select name="lang"><option value="en">English<option value="fr" selected>French
<option value="de">German</select>
<input type="checkbox" name="exact" value="1">
<input type="radio" name="size" value="s" checked><input type="radio" name="size" value="l">
<input type="submit" name="go" value="Search"></form>
<form action="login?next=1" method="POST">
<input name="user" value="henri"><input type="password" name="pass">
<input type="checkbox" name="keep" checked><textarea name="note">first</textarea>
<button name="b" value="in">Log <b>in</b></button><input type="submit" name="other" value="x">
</form>"""


def check_forms(exe, env):
    """Fills each field of a test page and checks the requests of its forms."""
    page = OUT / "form.html"
    page.write_text(FORM_PAGE)
    picture = OUT / "form0.bin"
    run = subprocess.run([str(exe), str(page), "https://site.test/a/b?x=1", "0", str(picture)],
                         check=True, env=dict(env, ONEWWW_FORM="1"),
                         capture_output=True, text=True)
    decode(picture.read_bytes()).save(OUT / "form0.png")
    forms = [line[5:] for line in run.stderr.splitlines() if line.startswith("FORM ")]
    # GET: the text is encoded; the list moved from French to German; the
    # box is now on; the second radio button is on; one button only.
    assert forms[0] == ("0 https://site.test:443/find?src=box&q=apple+ii+%26+more"
                        "&lang=de&exact=1&size=l&go=Search|"), forms[0]
    # POST with the <button>: the box was on and is now off.
    assert forms[1] == ("1 https://site.test:443/a/login?next=1|user=apple+ii+%26+more"
                        "&pass=s3cret&note=apple+ii+%26+more&b=in"), forms[1]
    assert forms[2].endswith("&other=x"), forms[2]


def main():
    gcc = os.environ.get("GCC") or shutil.which("gcc") or str(GCC_DEFAULT)
    env = dict(os.environ)
    env["PATH"] = str(Path(gcc).parent) + os.pathsep + env["PATH"]
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "main.c").write_text(MAIN)
    exe = OUT / "onewww_host.exe"
    sources = ["onewww_render.c", "onewww_dom.c", "onewww_entities.c",
               "onewww_css.c", "onewww_css_colors.c",
               "onewww_font_data.c", "onewww_home_page.c", "http_core.c",
               "net_image.c", "../lib/lodepng.c", "onewww_cookie.c"]
    subprocess.run([gcc, "-std=gnu99", "-O1", "-Wall", "-Wextra", "-I",
                    str(SRC), str(OUT / "main.c")] +
                   [str(SRC / s) for s in sources] + ["-o", str(exe)],
                   check=True, env=env)
    page = sys.argv[1] if len(sys.argv) > 1 else "-"
    base = sys.argv[2] if len(sys.argv) > 2 else "-"
    tops = [int(t) for t in sys.argv[3:]] or [0, 384]
    name = "home" if page == "-" else Path(page).stem
    args = [page, base]
    for top in tops:
        args += [str(top), str(OUT / f"{name}{top}.bin")]
    if page != "-" and os.environ.get("ONEWWW_PICTURES"):
        # Get the pictures that the page wants, then lay it out with them.
        import urllib.request
        folder = OUT / f"img_{name}"
        folder.mkdir(exist_ok=True)
        listing = subprocess.run([str(exe), page, base], check=True,
                                 env=dict(env, ONEWWW_LIST="1"),
                                 capture_output=True, text=True).stdout
        for line in listing.splitlines():
            number, url = line.split(" ", 1)
            target = folder / number
            if target.exists():
                continue
            try:
                request = urllib.request.Request(
                    url, headers={"User-Agent": "Mozilla/5.0 (compatible; test)"})
                target.write_bytes(urllib.request.urlopen(request, timeout=20).read())
            except Exception as error:      # a picture that fails stays out
                print("no picture:", url[:70], error)
        env["ONEWWW_IMG"] = str(folder)
    result = subprocess.run([str(exe)] + args, check=True, env=env,
                            capture_output=True, text=True).stdout.split()
    rows, links = int(result[0]), int(result[1])
    for top in tops:
        fields = (OUT / f"{name}{top}.bin").read_bytes()
        decode(fields).save(OUT / f"{name}{top}.png")
        assert len(fields) == 640 * 400 * 4
    if page == "-":
        assert 600 < rows < 60000 and links == 6, (rows, links)
        assert result[2] == "https://appletini.org/", result[2]
        check_forms(exe, env)
    print(f"PASS: {rows} rows, {links} links; pictures in {OUT}")


if __name__ == "__main__":
    main()
