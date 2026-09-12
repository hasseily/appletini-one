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
    if not compiler:
        raise RuntimeError("Set CC to a native GCC compiler")
    BUILD.mkdir(parents=True, exist_ok=True)
    # Exercise the production bit packing, without the ARM OCM/barrier code.
    handoff = (ROOT / "ps_sources/frontend/apple_fb_handoff.c").read_text()
    packing = handoff[handoff.index("#define HANDOFF_PUBLISHED_SLOT_ADDR"):
                      handoff.index("static void handoff_map_shared_ocm")]
    compositor = (ROOT / "ps_sources/frontend/compositor.c").read_text()
    effects = compositor[compositor.index("#define EFFECT_HISTORY_STRIDE"):
                         compositor.index("/* ---------- Format badge")]
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
''' + packing + effects + '''
void shape(uint16_t *dst, const uint32_t *src, int width, uint8_t color,
           uint8_t bleed) {
    uint16_t tint[256];
    video_mono_build_tint(tint, color);
    video_mono_expand_row(dst, src, width, video_mono_channel_shift(color),
                          tint, bleed);
}
uint32_t packed(uint32_t detail, uint32_t mode, uint8_t border) {
    return handoff_pack_published(2, mode, detail, border);
}
uint32_t unpacked(uint32_t word) { return handoff_published_format_detail(word); }
/* bleed 0 models a color frame or Dot bleed Off: draw_apple_subwindow then
 * claims no mono span and the plain RGB path runs. */
void compose(uint16_t *dst, const uint32_t *src, int w, int h, int sy,
             int scan, int bleed, int color, int border, int blur, int glow,
             int ghost, int reset) {
    s_video_dot_bleed = (uint8_t)bleed;
    s_mono_x = border ? 2 : 0;
    s_mono_y = border ? 1 : 0;
    s_mono_width = bleed ? w - 2*s_mono_x : 0;
    s_mono_height = h - 2*s_mono_y;
    s_mono_channel_shift = video_mono_channel_shift(color);
    video_mono_build_tint(s_mono_tint, color);
    if (reset) effect_clear_history();
    if (bleed || blur || glow || ghost) {
        blit_apple_ghosting_2x(dst, 0, 0, src, w, h, w, sy,
                              scan, ghost, blur, glow);
    } else if (sy == 4) {
        blit_apple_2x4_serviced(dst, 0, 0, src, w, h, w, scan);
    } else {
        blit_apple_2x2_serviced(dst, 0, 0, src, w, h, w, scan);
    }
}
uint32_t history(int x) { return s_effect_history[x]; }
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
    lib.packed.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint8]
    lib.packed.restype = lib.unpacked.restype = ctypes.c_uint32
    lib.unpacked.argtypes = [ctypes.c_uint32]
    lib.compose.argtypes = [ctypes.POINTER(ctypes.c_uint16),
                           ctypes.POINTER(ctypes.c_uint32)] + [ctypes.c_int] * 11
    lib.history.argtypes = [ctypes.c_int]
    lib.history.restype = ctypes.c_uint32

    def shape(pixels, color=1, bleed=LIGHT):
        src = (ctypes.c_uint32 * len(pixels))(*pixels)
        guarded = (ctypes.c_uint16 * (2 * len(pixels) + 2))()
        guarded[0] = guarded[-1] = 0xA55A
        dst = ctypes.cast(ctypes.byref(guarded, 2), ctypes.POINTER(ctypes.c_uint16))
        lib.shape(dst, src, len(pixels), color, bleed)
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
    # once made four black output columns. Light keeps its middle two black
    # with dim edges; Medium and Strong widen the spot until no column is
    # fully black. Each profile is fixed and symmetric.
    pattern = [0xFFFFFFFF] * 2 + [0xFF000000] * 2 + [0xFFFFFFFF] * 2
    profiles = {
        LIGHT:  [255, 255, 255, 191, 64, 0, 0, 64, 191, 255, 255, 255],
        MEDIUM: [255, 255, 239, 175, 80, 16, 16, 80, 175, 239, 255, 255],
        STRONG: [255, 239, 215, 159, 96, 56, 56, 96, 159, 215, 239, 255],
    }
    for bleed, expected in profiles.items():
        result = shape(pattern, bleed=bleed)
        assert result == [rgb565(v, v, v) for v in expected], (bleed, result)
        assert result == result[::-1]
    assert shape(pattern, bleed=OFF) == plain(pattern)
    assert [profiles[b].count(0) for b in LEVELS] == [2, 0, 0]
    assert profiles[LIGHT][5] < profiles[MEDIUM][5] < profiles[STRONG][5]
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

    def compose(rows, sy=4, scan=0, bleed=LIGHT, color=1, border=0,
                blur=0, glow=0, ghost=0, reset=1):
        w, h = len(rows[0]), len(rows)
        src = (ctypes.c_uint32 * (w*h))(*(v for row in rows for v in row))
        count = 1920*h*sy
        guarded = (ctypes.c_uint16 * (count + 2))()
        guarded[0] = guarded[-1] = 0xA55A
        dst = ctypes.cast(ctypes.byref(guarded, 2), ctypes.POINTER(ctypes.c_uint16))
        lib.compose(dst, src, w, h, sy, scan, bleed, color, border,
                    blur, glow, ghost, reset)
        assert guarded[0] == guarded[-1] == 0xA55A
        for y in range(h*sy):
            assert not any(dst[y*1920+2*w:y*1920+1920]), "write crossed row width"
        return [list(dst[y*1920:y*1920+2*w]) for y in range(h*sy)]

    for bleed in LEVELS:
        for sy in (2, 4):
            for scan in range(4):
                rows = compose([pattern] * 3, sy=sy, scan=scan, bleed=bleed)
                for y, row in enumerate(rows):
                    phase = y % sy
                    blank = (phase >= 4-scan if sy == 4 else phase == 1 and scan >= 2)
                    assert row == ([0] * 12 if blank else shape(pattern, bleed=bleed))
    red = 0xFFFF0000
    bordered = [[red]*10] + [[red]*2 + pattern + [red]*2]*3 + [[red]*10]
    for bleed in LEVELS:
        rows = compose(bordered, border=1, bleed=bleed)
        for y, row in enumerate(rows):
            if y < 4 or y >= 16:
                assert row == [rgb565(255, 0, 0)] * 20
            else:
                assert row[:4] == row[-4:] == [rgb565(255, 0, 0)] * 4
                assert row[4:-4] == shape(pattern, bleed=bleed)
    print("PASS production compositor: scanline phases, 2x/4x row reuse and colored borders")

    for blur in range(4):
        for glow in range(4):
            for ghost in range(4):
                compose([pattern] * 3, bleed=OFF, blur=blur, glow=glow, ghost=ghost)
                color_history = [lib.history(x) for x in range(len(pattern))]
                for bleed in (LIGHT, STRONG):
                    compose([pattern] * 3, bleed=bleed, blur=blur, glow=glow, ghost=ghost)
                    assert [lib.history(x) for x in range(len(pattern))] == color_history
    for sy in (2, 4):
        rows = compose(bordered, sy=sy, bleed=OFF)
        for y, row in enumerate(rows):
            assert row == plain(bordered[y//sy])
    print("PASS Off/color bypass and all blur/glow/ghosting combinations; shaping never enters history")

    for detail in range(0x2000):
        for mode in range(3):
            border = detail & 15
            word = lib.packed(detail, mode, border)
            assert lib.unpacked(word) == detail
            assert (word & 255) == 2
            assert ((word >> 8) & 3) == mode
            assert ((word >> 10) & 15) == border
    print("PASS all mono/format bits survive frame handoff without corrupting geometry or border")


if __name__ == "__main__":
    main()
