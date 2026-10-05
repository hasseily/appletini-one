#!/usr/bin/env python3
"""Compile the dot-bleed shaper; check gaps, tints, edges, levels and frame tags."""

import ctypes
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "video_mono_test"

OFF, LIGHT, MEDIUM, STRONG = 0, 1, 2, 3
LEVELS = (LIGHT, MEDIUM, STRONG)


def main():
    compiler = os.environ.get("CC") or shutil.which("gcc")
    if not compiler and Path("C:/msys64/ucrt64/bin/gcc.exe").exists():
        compiler = "C:/msys64/ucrt64/bin/gcc.exe"
    if not compiler and Path("E:/AMDDesignTools/2025.2/tps/mingw/10.0.0/win64.o/nt/bin/gcc.exe").exists():
        compiler = "E:/AMDDesignTools/2025.2/tps/mingw/10.0.0/win64.o/nt/bin/gcc.exe"
    if not compiler:
        raise RuntimeError("Set CC to a native GCC compiler")
    BUILD.mkdir(parents=True, exist_ok=True)
    # Exercise the production bit packing, without the ARM OCM/barrier code.
    handoff = (ROOT / "ps_sources/frontend/apple_fb_handoff.c").read_text()
    packing = handoff[handoff.index("#define HANDOFF_PUBLISHED_SLOT_ADDR"):
                      handoff.index("static void handoff_map_shared_ocm")]
    wrapper = BUILD / "mono_test.c"
    wrapper.write_text('''#include <string.h>
#include "video_mono.h"
#include "apple_fb_handoff.h"
#include "compositor_layout.h"
#include "video_blur.h"
#include "video_glow.h"
#include "video_ghosting.h"
#include "scanlines.h"
static int smartport_service_has_pending(void) { return 0; }
static void smartport_service_poll(void) {}
static uint8_t s_video_dot_bleed = APPLETINI_VIDEO_DOT_BLEED_LIGHT;
''' + packing + '''
void shape(uint16_t *dst, const uint32_t *src, int width, uint8_t color,
           uint8_t bleed) {
    uint16_t tint[256];
    video_mono_build_tint(tint, color);
    video_mono_expand_row(dst, src, width, video_mono_channel_shift(color),
                          tint, bleed);
}
/* Keep the exact brightness samples visible before RGB565 quantization. */
void shape_levels(uint16_t *dst, const uint32_t *src, int width, uint8_t color,
                  uint8_t bleed) {
    uint16_t tint[256];
    for (unsigned y = 0; y < 256; ++y) tint[y] = (uint16_t)y;
    video_mono_expand_row(dst, src, width, video_mono_channel_shift(color),
                          tint, bleed);
}
uint32_t packed(uint32_t detail, uint32_t mode, uint8_t border) {
    return handoff_pack_published(2, mode, detail, border);
}
uint32_t unpacked(uint32_t word) { return handoff_published_format_detail(word); }
''')
    libpath = BUILD / ("mono_test.dll" if os.name == "nt" else "mono_test.so")
    env = os.environ.copy()
    env["PATH"] = str(Path(compiler).resolve().parent) + os.pathsep + env["PATH"]
    subprocess.run([compiler, "-std=c11", "-O2", "-shared", "-funsigned-char",
                    "-Wall", "-Wextra",
                    "-Werror", "-Wno-unused-variable", "-Wno-unused-function", "-fPIC",
                    "-I" + str(ROOT / "ps_sources/frontend"),
                    "-I" + str(ROOT / "ps_sources/lib"), str(wrapper),
                    str(ROOT / "ps_sources/lib/fb16.c"),
                    "-o", str(libpath)], env=env, check=True)
    lib = ctypes.CDLL(str(libpath))
    lib.shape.argtypes = [ctypes.POINTER(ctypes.c_uint16),
                         ctypes.POINTER(ctypes.c_uint32), ctypes.c_int,
                         ctypes.c_uint8, ctypes.c_uint8]
    lib.shape_levels.argtypes = lib.shape.argtypes
    lib.packed.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint8]
    lib.packed.restype = lib.unpacked.restype = ctypes.c_uint32
    lib.unpacked.argtypes = [ctypes.c_uint32]

    def shape(pixels, color=1, bleed=LIGHT, levels=False):
        src = (ctypes.c_uint32 * len(pixels))(*pixels)
        guarded = (ctypes.c_uint16 * (2 * len(pixels) + 2))()
        guarded[0] = guarded[-1] = 0xA55A
        dst = ctypes.cast(ctypes.byref(guarded, 2), ctypes.POINTER(ctypes.c_uint16))
        (lib.shape_levels if levels else lib.shape)(dst, src, len(pixels), color, bleed)
        assert guarded[0] == guarded[-1] == 0xA55A, "row write crossed an edge"
        return list(guarded)[1:-1]

    def rgb565(r, g, b):
        return ((r & 248) << 8) | ((g & 252) << 3) | (b >> 3)

    def plain(pixels):
        out = []
        for p in pixels:
            v = rgb565((p >> 16) & 255, (p >> 8) & 255, p & 255)
            out.extend([v, v])
        return out

    # DC gain, no periodic byte seams, and tiny/tail/full-width rows at
    # every level. Every kernel sums to one, so solid fills never shift.
    for bleed in (OFF,) + LEVELS:
        for width in (0, 1, 2, 3, 7, 8, 14, 559, 560, 616, 640):
            for level in (0, 1, 63, 127, 181, 254, 255):
                result = shape([0xFF000000 | level * 0x010101] * width, bleed=bleed)
                assert result == [rgb565(level, level, level)] * (2 * width)
    print("PASS solid fills, edge clamps, tails and row bounds at every level")

    # Ordinary HGR doubles each bit to two source dots. An interior off bit
    # makes four output columns. Light and Medium keep its middle two black
    # as the edges brighten; Strong adds only a little light at the center.
    # Each profile is fixed and symmetric.
    pattern = [0xFFFFFFFF] * 2 + [0xFF000000] * 2 + [0xFFFFFFFF] * 2
    profiles = {
        LIGHT:  [255, 255, 255, 191, 64, 0, 0, 64, 191, 255, 255, 255],
        MEDIUM: [255, 255, 255, 159, 96, 0, 0, 96, 159, 255, 255, 255],
        STRONG: [255, 255, 247, 143, 112, 8, 8, 112, 143, 247, 255, 255],
    }
    for bleed, expected in profiles.items():
        result = shape(pattern, bleed=bleed)
        assert result == [rgb565(v, v, v) for v in expected], (bleed, result)
        assert result == result[::-1]
        assert shape(pattern, bleed=bleed, levels=True) == expected
    assert shape(pattern, bleed=OFF) == plain(pattern)
    assert [profiles[b].count(0) for b in LEVELS] == [2, 2, 0]
    assert profiles[LIGHT][4] < profiles[MEDIUM][4] < profiles[STRONG][4]
    print("PASS HGR gap profiles per level; Off is the plain expansion")

    # DHGR alternating single dots (80-column text density) keep two
    # distinct phases at every level. Contrast falls as the spot widens
    # but never reaches zero, and no column goes black.
    alternating = [0xFF000000, 0xFFFFFFFF] * 16
    contrasts = []
    for bleed in LEVELS:
        interior = shape(alternating, bleed=bleed)[4:-4]
        assert all(v != 0 for v in interior)
        values = sorted(set(interior))
        assert len(values) == 2
        contrasts.append(values[1] - values[0])
    assert contrasts[0] > contrasts[1] > contrasts[2] > 0, contrasts
    print("PASS DHGR phases stay distinct; contrast falls with the level")

    peaks = {0: (0, 0, 0), 1: (255, 255, 255),
             2: (255, 128, 1), 3: (8, 181, 82)}
    for bleed in LEVELS:
        for color, (r, g, b) in peaks.items():
            pixel = 0xFF000000 | (r << 16) | (g << 8) | b
            assert shape([pixel] * 7, color, bleed) == [rgb565(r, g, b)] * 14
            assert shape([0xFF000000] * 7, color, bleed) == [0] * 14
        # Irrelevant RGB components cannot influence brightness shaping.
        assert shape([0xFF00B500], 3, bleed) == shape([0xFFFFB5FF], 3, bleed)
        assert shape([0xFF800000], 2, bleed) == shape([0xFF80FFFF], 2, bleed)
    print("PASS mono tints, black output and single-channel filtering at every level")

    # Runtime Blur/Dot/glow/scanline coverage lives in test_video_smoothing.py;
    # this checks mono shaping and frame-tag compatibility vectors.

    for detail in range(0x8000):
        for mode in range(3):
            border = detail & 15
            word = lib.packed(detail, mode, border)
            # The thirteen original format/mono bits survive; removed TV/mix
            # flags and other reserved bits are ignored.
            assert lib.unpacked(word) == (detail & 0x1FFF)
            assert (word & 255) == 2
            assert ((word >> 8) & 3) == mode
            assert ((word >> 10) & 15) == border
    print("PASS mono/format bits survive frame handoff; reserved bit discarded; geometry/border unchanged")


if __name__ == "__main__":
    main()
