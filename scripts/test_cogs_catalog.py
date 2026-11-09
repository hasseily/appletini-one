#!/usr/bin/env python3
"""Run the production CoGS catalog parser against bounded native fixtures."""

from pathlib import Path
import json
import subprocess

from test_onee_input_service import find_native_c_compiler

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/cogs-catalog-test"
FRONT = ROOT / "ps_sources/frontend"

HARNESS = r'''
#include "cogs_catalog.h"
#include <stdio.h>

static uint8_t json[32769];
static cogs_catalog_page_t page;

int main(int argc, char **argv)
{
    if (argc != 2) return 1;
    FILE *input = fopen(argv[1], "rb");
    if (!input) return 2;
    size_t length = fread(json, 1, sizeof json, input);
    fclose(input);
    page.pages = 1;
    page.count = 1;
    int result = cogs_catalog_parse(json, length, &page);
    printf("%d\t%u\t%u\n", result, (unsigned)page.pages, (unsigned)page.count);
    if (result != 0) return 0;
    for (unsigned i = 0; i < page.count; ++i) {
        const cogs_catalog_entry_t *entry = &page.entries[i];
        printf("%s\t%s\t%lu\n", entry->title, entry->url, (unsigned long)entry->bytes);
    }
    return 0;
}
'''


def main():
    compiler = find_native_c_compiler()
    if not compiler:
        raise RuntimeError("Native C compiler required")
    OUT.mkdir(parents=True, exist_ok=True)
    source = OUT / "catalog_host.c"
    source.write_text(HARNESS, encoding="utf-8")
    exe = OUT / "catalog_host.exe"
    subprocess.run(
        [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(FRONT),
         str(source), *(str(FRONT / name) for name in
                        ("cogs_catalog.c", "net_json.c", "http_core.c")), "-o", str(exe)],
        cwd=ROOT, check=True,
    )

    def parse(document):
        raw = document if isinstance(document, bytes) else json.dumps(document).encode("utf-8")
        path = OUT / "catalog.json"
        path.write_bytes(raw)
        completed = subprocess.run([str(exe), str(path)], check=True, capture_output=True, text=True)
        return [line.split("\t") for line in completed.stdout.splitlines()]

    po = "https://disks.a2cogs.com/disks/fujinet-5card.po"
    hdv = "https://github.com/a2-4am/4cade/releases/download/v6.1/Total.Replay.hdv"
    wrapped = "https://disks.a2cogs.com/disks/Knights%20of%20Legend%203_5%20Installer.2mg"
    first = dict(name="5 Card Stud (FujiNet)", blocks=280, readonly=True, boot=True,
                 cat="G", host="any", url=po)
    bundle = dict(name="Airheart", blocks=65535, cat="G", host="any", url=hdv,
                  **{"in": "Total Replay v6.1"})
    installer = dict(name="Knights of Legend 3_5 Installer", blocks=1600, cat="G", host="any", url=wrapped)
    assert parse(dict(pages=36, count=640, volumes=[first, bundle, installer])) == [
        ["0", "36", "2"],
        [first["name"], po, "143360"],
        [installer["name"], wrapped, "819264"],
    ]
    assert parse(dict(pages=36, count=640, volumes=[bundle] * 18)) == [["0", "36", "0"]]
    assert parse(dict(pages=1, count=0, volumes=[])) == [["0", "1", "0"]]

    replay_images = (
        ("Total Replay v6.1", hdv),
        ("Total Replay II: Instant Replay",
         "https://github.com/a2-4am/4sports/releases/download/v1.0-alpha.4/Total.Replay.II.hdv"),
        ("Wizard Replay v2.3",
         "https://github.com/a2-4am/wizard-replay/releases/download/v2.3/wizard-replay.hdv"),
        ("Total Replay v7.0",
         "https://github.com/a2-4am/4cade/releases/download/v7.0/Total.Replay.hdv?raw=1"),
    )
    replays = [dict(first, name=name, blocks=65535, url=url) for name, url in replay_images]
    replay_members = [dict(replay, name="Individual Game", **{"in": replay["name"]})
                      for replay in replays]
    assert parse(dict(pages=1, volumes=replays + replay_members)) == [
        ["0", "1", "4"], *[[name, url, "33553920"] for name, url in replay_images],
    ]
    filtered = [bundle, dict(first, **{"in": "Collection"}), dict(first, **{"in": None}),
                dict(first, **{"in": ["Collection"]}), dict(first, **{"in": "\u0000"}),
                dict(first, cat="S"), dict(first, host="gs"), dict(first, cat="GG"),
                dict(first, host=None), dict(first, name=""), dict(first, url="https://example.com/game.woz"),
                dict(first, url="http://example.com/game.po"), dict(first, url="https://u@h/game.po"),
                dict(first, url="https://example.com/a#game.po"), dict(first, url="https://example.com/.po"),
                dict(first, url="https://example.com/a\u0000.po"),
                dict(first, url="https://example.com/" + "x" * 1024 + ".po"),
                dict(first, blocks=0), dict(first, blocks=65536), dict(first, blocks=1.5),
                dict(first, bytes=33554497), dict(first, bytes=143359), dict(first, bytes=143872),
                dict(installer, bytes=819200),
                {key: value for key, value in first.items() if key not in ("blocks", "host")},
                {key: value for key, value in first.items() if key != "cat"}]
    filtered += [dict(replays[0], host="gs"), dict(replays[1], cat="S")]
    for entry in filtered:
        assert parse(dict(pages=1, volumes=[entry, first])) == [
            ["0", "1", "1"], [first["name"], po, "143360"]
        ], entry
    raw = "https://disks.a2cogs.com/disks/standalone.hdv"
    explicit_standalone = dict(first, blocks=65535, url=raw, **{"in": ""})
    assert parse(dict(pages=1, volumes=[explicit_standalone])) == [
        ["0", "1", "1"], [first["name"], raw, "33553920"]
    ]
    same_publisher = "https://github.com/a2-4am/4cade/releases/download/v6.1/Standalone.hdv"
    other_publisher = "https://github.com/other/4cade/releases/download/v6.1/Total.Replay.hdv"
    assert parse(dict(pages=1, volumes=[dict(first, url=url) for url in
                                      (same_publisher, other_publisher)])) == [
        ["0", "1", "2"], [first["name"], same_publisher, "143360"],
        [first["name"], other_publisher, "143360"]
    ]

    explicit = dict(name="é😀" + "A" * 48, bytes=512, cat="G", host="any",
                    url="https://example.com/A.PO?file=a.zip")
    assert parse(dict(pages=1, volumes=[explicit])) == [
        ["0", "1", "1"], ["??" + "A" * 45, explicit["url"], "512"]
    ]
    assert parse(b'{"volumes":[{"url":"https:\\/\\/example.com\\/x.po","host":"any",'
                 b'"name":"Game","blocks":1,"\\u0063at":"\\u0047"}],"pages":2}') == [
        ["0", "2", "1"], ["Game", "https://example.com/x.po", "512"]
    ]
    assert parse(dict(pages=1, volumes=[first] * 18))[0] == ["0", "1", "18"]
    invalid = [b"", b'{"pages":1,"volumes":[]', b'{"pages":1,"volumes":[]} null',
               b'{"pages":1,"volumes":[],"ignored":[0,]}',
               b'{"pages":1,"pages":2,"volumes":[]}',
               b'{"pages":1,"volumes":[],"\\u0076olumes":[]}',
               b'{"pages":1,"volumes":[{"cat":"G","\\u0063at":"S"}]}',
               b'{"pages":1,"volumes":[{"name":"\\ud800"}]}',
               b'{"pages":1,"volumes":[{"name":"\xf0\x80\x80\x80"}]}',
               b'{"pages":1,"volumes":[null]}',
               dict(pages=65536, volumes=[]), dict(pages=0, volumes=[]),
               dict(pages=1.0, volumes=[]), dict(pages=1, volumes=[first] * 19),
               dict(pages=1, volumes={}), dict(volumes=[]), dict(pages=1)]
    for document in invalid:
        assert parse(document) == [["-1", "0", "0"]], document
    assert parse(b'{"pages":1,"volumes":[]}' + b" " * 32769) == [["-1", "0", "0"]]
    print("PASS CoGS pages, Replay disks, 8-bit game filtering, image sizes, and malformed catalog rejection")


if __name__ == "__main__":
    main()
