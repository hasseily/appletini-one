#!/usr/bin/env python3
"""Round-trip regression for config-file path values (ps_sources/frontend/config_menu.c).

There is no runnable host C toolchain in this environment, so this test
models the exact string logic of two C functions and asserts the
save->load round-trip for tricky file names. The model MUST stay
byte-faithful to the C:

  - config_menu_parse_config_line()  (the key=value / quote / comment parser)
  - config_menu_quote_path()         (the path serializer)

If you change either C function, update the model below to match and keep
these cases green. It encodes the contract: any allowed FAT/exFAT filename
character -- including '#', spaces, '=', '%', '&', parentheses -- must
survive a config save and reload unchanged.
"""

from __future__ import annotations

import sys

BOM = "﻿"
_WS = " \t\r\n"


def _trim(text: str) -> str:
    # Mirrors config_menu_trim: strip a UTF-8 BOM, then surrounding
    # whitespace (space, tab, CR, LF).
    if text.startswith(BOM):
        text = text[len(BOM):]
    return text.strip(_WS)


def parse_config_line(line: str):
    """Model of config_menu_parse_config_line. Returns (key, value) or None."""
    eq = line.find("=")
    hash_ = line.find("#")
    if eq == -1 or (hash_ != -1 and hash_ < eq):
        return None
    key = _trim(line[:eq])
    if key == "":
        return None
    value = line[eq + 1:]
    # skip leading whitespace
    i = 0
    while i < len(value) and value[i] in _WS:
        i += 1
    value = value[i:]
    if value.startswith('"'):
        value = value[1:]
        close = value.find('"')
        if close != -1:
            value = value[:close]
    else:
        vhash = value.find("#")
        if vhash != -1:
            value = value[:vhash]
        value = _trim(value)
    return key.lower(), value


def quote_path(path: str) -> str:
    """Model of config_menu_quote_path: always quotes; empty -> ""."""
    return '"' + (path or "") + '"'


FIRMWARE = "FIRMWARE"


def is_firmware_default(value: str) -> bool:
    # Mirrors config_menu_config_path_is_firmware_default (empty / "firmware").
    return value == "" or value.lower() == "firmware"


# --- test cases --------------------------------------------------------------

# File names that a user may legitimately have on a FAT/exFAT card. FAT
# forbids  " * / : < > ? \ |  in a name, so those are excluded; everything
# else is fair game and must round-trip.
DISK_PATHS = [
    "0:/games/normal.hdv",
    "0:/games/C#/Ultima #4.2mg",          # '#' inside dir AND file
    "0:/My Games (1983)/rock & roll.woz",  # spaces, parens, ampersand
    "0:/a=b.dsk",                          # '=' in the value
    "0:/80% full.po",                      # percent
    "0:/naughty; name.2mg",                # semicolon
    "0:/[brackets]{braces}.hdv",           # brackets/braces
    "0:/plus+minus-dot..name.woz",         # plus/minus/dots
    "0:/#leading-hash.dsk",                # leading '#'
    "0:/trailing-hash#.hdv",               # trailing '#'
    "",                                    # no disk
]

BEZEL_ROM_PATHS = [
    "0:/bezels/Apple #2.png",
    "0:/ROMs/char & set.rom",
    "",                                    # empty -> built-in (FIRMWARE)
]

fails = 0


def check(cond: bool, msg: str) -> None:
    global fails
    if not cond:
        fails += 1
        print(f"FAIL: {msg}")


def roundtrip_disk(path: str) -> str:
    """Write like a smartport/disk2 line, parse it back, return the value."""
    line = f'smartport.disk.1.path={quote_path(path)}'
    parsed = parse_config_line(line)
    assert parsed is not None, line
    key, value = parsed
    check(key == "smartport.disk.1.path", f"key preserved for {line!r}")
    return value


def roundtrip_bezel(path: str) -> str:
    value_out = quote_path(path) if path else FIRMWARE
    line = f"video.bezel.path={value_out}"
    parsed = parse_config_line(line)
    assert parsed is not None, line
    _key, value = parsed
    return value


def main() -> int:
    # 1. Disk paths round-trip exactly (empty stays empty = "no disk").
    for p in DISK_PATHS:
        got = roundtrip_disk(p)
        check(got == p, f"disk path round-trip: {p!r} -> {got!r}")

    # 2. Bezel/ROM: real paths round-trip; empty maps to the FIRMWARE
    #    sentinel which the loader treats as "built-in".
    for p in BEZEL_ROM_PATHS:
        got = roundtrip_bezel(p)
        if p == "":
            check(is_firmware_default(got),
                  f"empty bezel path -> firmware default (got {got!r})")
        else:
            check(got == p and not is_firmware_default(got),
                  f"bezel path round-trip: {p!r} -> {got!r}")

    # 3. Legacy unquoted values still parse (numbers, on/off, and the
    #    firmware sentinel) so old config files keep loading.
    check(parse_config_line("vtw.speed.mode=1") == ("vtw.speed.mode", "1"),
          "legacy numeric value parses")
    check(parse_config_line("video.bezel.path=FIRMWARE") ==
          ("video.bezel.path", "FIRMWARE"),
          "legacy FIRMWARE sentinel parses")
    check(parse_config_line("boot.device = SMARTPORT ") ==
          ("boot.device", "SMARTPORT"),
          "legacy value is trimmed")

    # 4. Comments and blank lines are ignored.
    check(parse_config_line("# a comment") is None, "full-line comment ignored")
    check(parse_config_line("   # indented comment") is None,
          "indented comment ignored")
    check(parse_config_line("") is None, "blank line ignored")
    check(parse_config_line("no-equals-here") is None, "no '=' ignored")

    # 5. Unquoted trailing comment still strips (legacy behaviour).
    check(parse_config_line("boot.device=SMARTPORT # note") ==
          ("boot.device", "SMARTPORT"),
          "unquoted trailing comment stripped")

    # 6. A quoted value with a '#' before its '=' would be a commented-out
    #    line -- must be ignored, not misparsed.
    check(parse_config_line('#smartport.disk.1.path="0:/x.hdv"') is None,
          "commented-out path line ignored")

    # 7. File-manager last-directory memory: directory paths (which can
    #    themselves contain '#') round-trip through the same quoting.
    for d in ["0:/games", "0:/C#/games", "0:/My Disks (A-M)", ""]:
        line = f"browser.lastdir.smartport={quote_path(d)}"
        parsed = parse_config_line(line)
        assert parsed is not None, line
        check(parsed == ("browser.lastdir.smartport", d),
              f"lastdir round-trip: {d!r} -> {parsed[1]!r}")

    if fails == 0:
        print(f"PASS: {len(DISK_PATHS) + len(BEZEL_ROM_PATHS)} path round-trips "
              "+ legacy/comment cases")
        return 0
    print(f"FAILED: {fails} checks")
    return 1


if __name__ == "__main__":
    sys.exit(main())
