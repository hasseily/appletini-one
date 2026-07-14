#!/usr/bin/env python3
"""Generate the SSI-263/SC-01 formant ROM SystemVerilog package.

The source data is the Votrax SC-01-A internal ROM dump used by MAME's
votrax_sc01a device.  MAME loads the ROM as "sc01a.bin" with CRC32
fc416227 and decodes the bitfields in src/devices/sound/votrax.cpp.
"""

from __future__ import annotations

import argparse
import hashlib
import math
from pathlib import Path
import sys
import zipfile
import zlib


REPO_ROOT = Path(__file__).resolve().parents[1]
OUT_SV = REPO_ROOT / "hdl" / "apple" / "ssi263_formant_pkg.sv"
EXPECTED_SIZE = 512
EXPECTED_CRC32 = 0xFC416227
EXPECTED_SHA1 = "1d6da90b1807a01b5e186ef08476119a862b5e6d"
COEFF_FRAC_BITS = 15
COEFF_SCALE = 1 << COEFF_FRAC_BITS
FORMANT_SAMPLE_CLOCK = 48_000.0
FORMANT_CAP_CLOCK = 20_000.0


VOTRAX_TO_SSI263 = [
    0x02, 0x0A, 0x0B, 0x00, 0x28, 0x08, 0x08, 0x2F,
    0x0E, 0x07, 0x07, 0x07, 0x37, 0x38, 0x24, 0x33,
    0x32, 0x32, 0x2F, 0x10, 0x39, 0x0F, 0x13, 0x13,
    0x20, 0x29, 0x25, 0x2C, 0x26, 0x34, 0x25, 0x30,
    0x08, 0x09, 0x03, 0x1B, 0x0E, 0x27, 0x11, 0x07,
    0x16, 0x05, 0x28, 0x1D, 0x01, 0x23, 0x0C, 0x0D,
    0x10, 0x1A, 0x19, 0x18, 0x11, 0x11, 0x14, 0x14,
    0x35, 0x36, 0x1C, 0x0A, 0x01, 0x10, 0x00, 0x00,
]

def read_rom(path: Path) -> bytes:
    if path.suffix.lower() == ".zip":
        with zipfile.ZipFile(path) as archive:
            return archive.read("sc01a.bin")
    return path.read_bytes()


def validate_rom(data: bytes) -> None:
    crc32 = zlib.crc32(data) & 0xFFFFFFFF
    sha1 = hashlib.sha1(data).hexdigest()
    if len(data) != EXPECTED_SIZE:
        raise SystemExit(f"SC-01-A ROM must be {EXPECTED_SIZE} bytes, got {len(data)}")
    if crc32 != EXPECTED_CRC32:
        raise SystemExit(f"SC-01-A ROM CRC32 must be {EXPECTED_CRC32:08x}, got {crc32:08x}")
    if sha1 != EXPECTED_SHA1:
        raise SystemExit(f"SC-01-A ROM SHA1 must be {EXPECTED_SHA1}, got {sha1}")


def invert_votrax_map() -> list[int]:
    inverse = [0x3F] * 64
    for sc01, ssi263 in enumerate(VOTRAX_TO_SSI263):
        if inverse[ssi263] == 0x3F:
            inverse[ssi263] = sc01
    return inverse


def bits_to_caps(value: int, caps_values: list[float]) -> float:
    total = 0.0
    for cap in caps_values:
        if value & 1:
            total += cap
        value >>= 1
    return total


def quantize_coeff(value: float) -> int:
    quantized = int(round(value * COEFF_SCALE))
    return max(-(1 << 15), min((1 << 15) - 1, quantized))


def standard_filter_coeffs(
    c1t: float,
    c1b: float,
    c2t: float,
    c2b: float,
    c3: float,
    c4: float,
) -> list[int]:
    k0 = c1t / (FORMANT_CAP_CLOCK * c1b) if c1t else 0.0
    k1 = c4 * c2t / (FORMANT_CAP_CLOCK * c1b * c3) if c2t else 0.0
    k2 = c4 * c2b / (FORMANT_CAP_CLOCK * FORMANT_CAP_CLOCK * c1b * c3)
    fpeak = math.sqrt(abs(k0 * k1 - k2)) / (2 * math.pi * k2)
    zc = 2 * math.pi * fpeak / math.tan(math.pi * fpeak / FORMANT_SAMPLE_CLOCK)
    m0 = zc * k0
    m1 = zc * k1
    m2 = zc * zc * k2
    a = [1 + m0, 3 + m0, 3 - m0, 1 - m0]
    b = [1 + m1 + m2, 3 + m1 - m2, 3 - m1 - m2, 1 - m1 + m2]
    return [quantize_coeff(term / b[0]) for term in a] + [
        quantize_coeff(-term / b[0]) for term in b[1:]
    ]


def lowpass_filter_coeffs(c1t: float, c1b: float) -> list[int]:
    k = c1b / (FORMANT_CAP_CLOCK * c1t) * (150.0 / 4000.0)
    fpeak = 1 / (2 * math.pi * k)
    zc = 2 * math.pi * fpeak / math.tan(math.pi * fpeak / FORMANT_SAMPLE_CLOCK)
    m = zc * k
    b0 = 1 + m
    return [
        quantize_coeff(1 / b0),
        quantize_coeff(-(1 - m) / b0),
    ]


def noise_shaper_filter_coeffs(
    c1: float,
    c2t: float,
    c2b: float,
    c3: float,
    c4: float,
) -> list[int]:
    k0 = c2t * c3 * c2b / c4
    k1 = c2t * (FORMANT_CAP_CLOCK * c2b)
    k2 = c1 * c2t * c3 / (FORMANT_CAP_CLOCK * c4)
    fpeak = math.sqrt(1 / k2) / (2 * math.pi)
    zc = 2 * math.pi * fpeak / math.tan(math.pi * fpeak / FORMANT_SAMPLE_CLOCK)
    m0 = zc * k0
    m1 = zc * k1
    m2 = zc * zc * k2
    a = [m0, 0, -m0]
    b = [1 + m1 + m2, 2 - 2 * m2, 1 - m1 + m2]
    return [quantize_coeff(term / b[0]) for term in a] + [
        quantize_coeff(-term / b[0]) for term in b[1:]
    ]


def emit_coeff_function(
    lines: list[str],
    name: str,
    index_decl: str,
    index_bits: int,
    packed_entries: list[tuple[int, list[int]]],
    index_expr: str,
) -> None:
    if index_decl:
        args = f"{index_decl}, input logic [2:0] tap"
    else:
        args = "input logic [2:0] tap"
    lines.extend([
        f"    function automatic logic signed [15:0] {name}({args});",
        f"        case ({{{index_expr}, tap}})",
    ])
    for index, coeffs in packed_entries:
        for tap, coeff in enumerate(coeffs):
            if coeff < 0:
                value = f"-16'sd{-coeff}"
            else:
                value = f"16'sd{coeff}"
            width = index_bits + 3
            key = (index << 3) | tap
            lines.append(f"            {width}'h{key:X}: {name} = {value};")
    lines.extend([
        f"            default: {name} = 16'sd0;",
        "        endcase",
        "    endfunction",
        "",
    ])


def emit_coefficients(lines: list[str]) -> None:
    lines.extend([
        f"    localparam int SC01_COEFF_FRAC_BITS = {COEFF_FRAC_BITS};",
        f"    localparam int SC01_FORMANT_SAMPLE_CLOCK_HZ = {int(FORMANT_SAMPLE_CLOCK)};",
        "",
    ])

    f1_entries = []
    for f1 in range(16):
        coeffs = standard_filter_coeffs(
            11247,
            11797,
            949,
            52067,
            2280 + bits_to_caps(f1, [2546, 4973, 9861, 19724]),
            166272,
        )
        f1_entries.append((f1, coeffs))
    emit_coeff_function(
        lines,
        "sc01a_f1_coeff",
        "input logic [3:0] index",
        4,
        f1_entries,
        "index",
    )

    f2_entries = []
    for f2 in range(32):
        for f2q in range(16):
            coeffs = standard_filter_coeffs(
                24840,
                29154,
                829 + bits_to_caps(f2q, [1390, 2965, 5875, 11297]),
                38180,
                2352 + bits_to_caps(f2, [833, 1663, 3164, 6327, 12654]),
                34270,
            )
            f2_entries.append(((f2 << 4) | f2q, coeffs))
    emit_coeff_function(
        lines,
        "sc01a_f2_coeff",
        "input logic [4:0] f2, input logic [3:0] f2q",
        9,
        f2_entries,
        "f2, f2q",
    )

    f3_entries = []
    for f3 in range(16):
        coeffs = standard_filter_coeffs(
            0,
            17594,
            868,
            18828,
            8480 + bits_to_caps(f3, [2226, 4485, 9056, 18111]),
            50019,
        )
        f3_entries.append((f3, coeffs))
    emit_coeff_function(
        lines,
        "sc01a_f3_coeff",
        "input logic [3:0] index",
        4,
        f3_entries,
        "index",
    )

    emit_coeff_function(
        lines,
        "sc01a_f4_coeff",
        "",
        4,
        [(0, standard_filter_coeffs(0, 28810, 1165, 21457, 8558, 7289))],
        "4'd0",
    )
    emit_coeff_function(
        lines,
        "sc01a_fn_coeff",
        "",
        4,
        [(0, noise_shaper_filter_coeffs(15500, 14854, 8450, 9523, 14083))],
        "4'd0",
    )
    emit_coeff_function(
        lines,
        "sc01a_fx_coeff",
        "",
        4,
        [(0, lowpass_filter_coeffs(1122, 23131))],
        "4'd0",
    )


def emit_package(data: bytes) -> str:
    words_by_phone: dict[int, int] = {}
    for row in range(64):
        word = int.from_bytes(data[row * 8:(row + 1) * 8], "little")
        phone = (word >> 56) & 0x3F
        words_by_phone[phone] = word

    missing = sorted(set(range(64)) - set(words_by_phone))
    if missing:
        raise SystemExit(f"SC-01-A ROM is missing phoneme rows: {missing}")

    inverse = invert_votrax_map()
    lines: list[str] = [
        "`timescale 1ns / 1ps",
        "",
        "// Generated by scripts/build_ssi263_formant_rom.py from sc01a.bin.",
        "// MAME src/devices/sound/votrax.cpp loads this Votrax SC-01-A ROM as",
        "// CRC32 fc416227 / SHA1 1d6da90b1807a01b5e186ef08476119a862b5e6d and",
        "// decodes the bitfields with the bitswap pattern mirrored below.",
        "package ssi263_formant_pkg;",
        "",
        "    function automatic logic [5:0] ssi263_to_sc01_phone(input logic [5:0] phoneme);",
        "        case (phoneme)",
    ]

    for phoneme, sc01 in enumerate(inverse):
        lines.append(f"            6'h{phoneme:02X}: ssi263_to_sc01_phone = 6'h{sc01:02X};")

    lines.extend([
        "            default: ssi263_to_sc01_phone = 6'h3F;",
        "        endcase",
        "    endfunction",
        "",
        "    function automatic logic [5:0] ssi263_to_sc01_audio_phone(",
        "        input logic [7:0] duration_phoneme,",
        "        input logic [1:0] current_function",
        "    );",
        "        logic [5:0] phoneme;",
        "        logic unused_current_function;",
        "        begin",
        "            phoneme = duration_phoneme[5:0];",
        "            unused_current_function = ^current_function;",
        "            ssi263_to_sc01_audio_phone = ssi263_to_sc01_phone(phoneme);",
        "        end",
        "    endfunction",
        "",
        "    function automatic logic [63:0] sc01a_word_by_phone(input logic [5:0] phone);",
        "        case (phone)",
    ])

    for phone in range(64):
        lines.append(f"            6'h{phone:02X}: sc01a_word_by_phone = 64'h{words_by_phone[phone]:016X};")

    lines.extend([
        "            default: sc01a_word_by_phone = 64'h000000000000003F;",
        "        endcase",
        "    endfunction",
        "",
        "    function automatic logic [3:0] sc01a_f1(input logic [5:0] phone);",
        "        logic [63:0] word;",
        "        begin",
        "            word = sc01a_word_by_phone(phone);",
        "            sc01a_f1 = {word[0], word[7], word[14], word[21]};",
        "        end",
        "    endfunction",
        "",
        "    function automatic logic [3:0] sc01a_va(input logic [5:0] phone);",
        "        logic [63:0] word;",
        "        begin",
        "            word = sc01a_word_by_phone(phone);",
        "            sc01a_va = {word[1], word[8], word[15], word[22]};",
        "        end",
        "    endfunction",
        "",
        "    function automatic logic [3:0] sc01a_f2(input logic [5:0] phone);",
        "        logic [63:0] word;",
        "        begin",
        "            word = sc01a_word_by_phone(phone);",
        "            sc01a_f2 = {word[2], word[9], word[16], word[23]};",
        "        end",
        "    endfunction",
        "",
        "    function automatic logic [3:0] sc01a_fc(input logic [5:0] phone);",
        "        logic [63:0] word;",
        "        begin",
        "            word = sc01a_word_by_phone(phone);",
        "            sc01a_fc = {word[3], word[10], word[17], word[24]};",
        "        end",
        "    endfunction",
        "",
        "    function automatic logic [3:0] sc01a_f2q(input logic [5:0] phone);",
        "        logic [63:0] word;",
        "        begin",
        "            word = sc01a_word_by_phone(phone);",
        "            sc01a_f2q = {word[4], word[11], word[18], word[25]};",
        "        end",
        "    endfunction",
        "",
        "    function automatic logic [3:0] sc01a_f3(input logic [5:0] phone);",
        "        logic [63:0] word;",
        "        begin",
        "            word = sc01a_word_by_phone(phone);",
        "            sc01a_f3 = {word[5], word[12], word[19], word[26]};",
        "        end",
        "    endfunction",
        "",
        "    function automatic logic [3:0] sc01a_fa(input logic [5:0] phone);",
        "        logic [63:0] word;",
        "        begin",
        "            word = sc01a_word_by_phone(phone);",
        "            sc01a_fa = {word[6], word[13], word[20], word[27]};",
        "        end",
        "    endfunction",
        "",
        "    // MAME notes that CLD/VD bit order is intentionally inverted.",
        "    function automatic logic [3:0] sc01a_cld(input logic [5:0] phone);",
        "        logic [63:0] word;",
        "        begin",
        "            word = sc01a_word_by_phone(phone);",
        "            sc01a_cld = {word[34], word[32], word[30], word[28]};",
        "        end",
        "    endfunction",
        "",
        "    function automatic logic [3:0] sc01a_vd(input logic [5:0] phone);",
        "        logic [63:0] word;",
        "        begin",
        "            word = sc01a_word_by_phone(phone);",
        "            sc01a_vd = {word[35], word[33], word[31], word[29]};",
        "        end",
        "    endfunction",
        "",
        "    function automatic logic sc01a_closure(input logic [5:0] phone);",
        "        logic [63:0] word;",
        "        begin",
        "            word = sc01a_word_by_phone(phone);",
        "            sc01a_closure = word[36];",
        "        end",
        "    endfunction",
        "",
        "    function automatic logic [6:0] sc01a_duration(input logic [5:0] phone);",
        "        logic [63:0] word;",
        "        begin",
        "            word = sc01a_word_by_phone(phone);",
        "            sc01a_duration = {~word[37], ~word[38], ~word[39], ~word[40],",
        "                              ~word[41], ~word[42], ~word[43]};",
        "        end",
        "    endfunction",
        "",
        "    function automatic logic sc01a_pause(input logic [5:0] phone);",
        "        begin",
        "            sc01a_pause = (phone == 6'h03) || (phone == 6'h3E);",
        "        end",
        "    endfunction",
        "",
    ])

    emit_coefficients(lines)

    lines.extend([
        "endpackage",
        "",
    ])

    return "\n".join(lines)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("rom", type=Path, help="Path to sc01a.bin or votrsc01a.zip")
    parser.add_argument("--out", type=Path, default=OUT_SV)
    args = parser.parse_args(argv)

    data = read_rom(args.rom)
    validate_rom(data)
    args.out.write_text(emit_package(data), newline="\n")
    print(f"Wrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
