#!/usr/bin/env python3
"""Source-level checks for the RGB565 output pipeline (fb16 + layout + PL).

The output path is 16-bit end to end: fb16 draws RGB565, the output ring
stores RGB565, fb_reader streams RGB565, and the DVI pins are 5:6:5. The
Apple frame ring stays BGRA32 and the fb16 2x blits narrow at the final
store. These tests pin the invariants that keep those two worlds glued.
"""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

FB16_H = REPO_ROOT / "ps_sources" / "lib" / "fb16.h"
FB16_C = REPO_ROOT / "ps_sources" / "lib" / "fb16.c"
LAYOUT_H = REPO_ROOT / "ps_sources" / "frontend" / "compositor_layout.h"
LAYOUT_C = REPO_ROOT / "ps_sources" / "frontend" / "compositor_layout.c"
FB_READER = REPO_ROOT / "hdl" / "video2" / "fb_reader.sv"
VIDEO_TOP = REPO_ROOT / "hdl" / "video2" / "video_top.sv"
VIDEO_PKG = REPO_ROOT / "hdl" / "video2" / "video_pkg.sv"


class TestFailure(AssertionError):
    pass


def require(cond, message):
    if not cond:
        raise TestFailure(message)


def read(path):
    return path.read_text(encoding="utf-8", errors="replace")


def test_fb16_pixel_format():
    h = read(FB16_H)
    require("#define FB16_BPP           2" in h,
            "fb16 surface must be 2 bytes per pixel")
    require("(((uint16_t)(r) & 0xF8u) << 8)" in h and
            "(((uint16_t)(g) & 0xFCu) << 3)" in h and
            "(((uint16_t)(b)) >> 3)" in h,
            "FB16_RGB must pack 5:6:5 exactly as the DVI pins expect")
    require("fb16_from_bgra32" in h and "fb16_to_bgra32" in h,
            "fb16 must provide both narrowing and replicate-high widening")
    # Round-trip: widening then narrowing any 565 value must be identity.
    def widen(v):
        r5, g6, b5 = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        return ((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4),
                (b5 << 3) | (b5 >> 2))
    def narrow(r, g, b):
        return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    for v in range(0, 0x10000, 257):
        require(narrow(*widen(v)) == v,
                "565 -> 888 -> 565 must be lossless (replicate-high)")


def test_fb16_blits_narrow_at_store():
    c = read(FB16_C)
    require("void fb16_expand_2x_row_bgra32src(uint16_t *dst, const uint32_t *src," in c,
            "the 2x expansion must take BGRA32 sources and emit 565")
    require("vld4_u8" in c and "vshll_n_u8" in c and "vsriq_n_u16" in c and
            "vzipq_u16(px, px)" in c,
            "NEON path must deinterleave BGRA, pack 565 with shift-inserts, "
            "and double in-register")
    require("const uint32_t *src, int src_w, int src_h" in c,
            "2x blits keep BGRA32 source signatures (Apple ring stays 32-bit)")
    require("memcpy(drow0, s_blit_2x_row, row_bytes);" in c,
            "expanded rows must be copied contiguously to the noncached output")
    require("fb32" not in c and "FB32" not in c,
            "no fb32 remnants in fb16.c")


def test_output_layout_is_565():
    h = read(LAYOUT_H)
    c = read(LAYOUT_C)
    require("#define COMP_OUT_BPP           2u" in h,
            "output ring must be 2 bytes per pixel")
    require("#define COMP_APPLE_BPP                 4u" in h,
            "Apple frame ring must stay BGRA32 for the renderer's chroma")
    require("0x3E000000u" in c and "0x3E400000u" in c and "0x3E800000u" in c,
            "output slots must be the contiguous 4 MB trio at 0x3E000000")
    # 1920*1080*2 must fit the 4 MB slot spacing.
    require(1920 * 1080 * 2 <= 0x400000,
            "a 565 frame must fit inside the 4 MB slot spacing")


def test_pl_scanout_is_565():
    pkg = read(VIDEO_PKG)
    rdr = read(FB_READER)
    top = read(VIDEO_TOP)
    require("localparam integer FB_BYTES_PER_PIXEL     = 2;" in pkg,
            "video_pkg must define the 2-byte framebuffer pixel")
    require("BGRA_BYTES_PER_PIXEL" not in pkg and
            "BGRA_BYTES_PER_PIXEL" not in rdr,
            "no BGRA pixel-size constant may remain in the PL video path")
    require("output logic [15:0]  pixel_rgb565," in rdr and
            '.READ_DATA_WIDTH     (16),' in rdr,
            "fb_reader must emit 16-bit pixels from a 64->16 FIFO")
    require("dvi_red_r <= fb_pixel[15:11];" in top and
            "dvi_grn_r <= fb_pixel[10:5];" in top and
            "dvi_blu_r <= fb_pixel[4:0];" in top,
            "video_top must map RGB565 straight onto the 5:6:5 pins")
    require("truncat" not in top.lower(),
            "no truncation stage may remain: the framebuffer and the wire "
            "are the same 16 bits")


def test_no_fb32_remnants_in_output_consumers():
    frontend = REPO_ROOT / "ps_sources" / "frontend"
    lib = REPO_ROOT / "ps_sources" / "lib"
    offenders = []
    for base in (frontend, lib):
        for p in base.glob("*.[ch]"):
            t = read(p)
            if re.search(r"\bfb32_|\bFB32_", t):
                offenders.append(p.name)
    require(not offenders,
            f"fb32 symbols must not survive the port: {offenders}")


TESTS = [
    test_fb16_pixel_format,
    test_fb16_blits_narrow_at_store,
    test_output_layout_is_565,
    test_pl_scanout_is_565,
    test_no_fb32_remnants_in_output_consumers,
]


def main():
    failed = 0
    for t in TESTS:
        try:
            t()
            print(f"PASS {t.__name__}")
        except TestFailure as e:
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
    if failed:
        print(f"{len(TESTS) - failed} of {len(TESTS)} fb16 tests passed; "
              f"{failed} failed")
        return 1
    print(f"{len(TESTS)} fb16 tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
