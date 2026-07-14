#!/usr/bin/env python3
"""Verify Idealized/RGB crisp colors against AppleWin's generated palette.

This mirrors AppleWin NTSC.cpp GenerateBaseColors(), then checks that the
firmware table and phase-index map produce the same visible repeated-nibble
outputs:

    python scripts/test_applewin_video_palette.py
"""

from __future__ import annotations

import math
import re
import struct
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
NTSC_C = REPO_ROOT / "ps_sources" / "frontend" / "appletini_ntsc.c"

NTSC_NUM_PHASES = 4
NTSC_NUM_SEQUENCES = 4096

RAD_45 = math.pi * 0.25
RAD_90 = math.pi * 0.5
CYCLESTART = math.radians(45)

CHROMA_GAIN = 7.438011255
CHROMA_0 = -0.7318893645
CHROMA_1 = 1.2336442711

LUMA_GAIN = 13.71331570
LUMA_0 = -0.3961075449
LUMA_1 = 1.1044202472

SIGNAL_GAIN = 7.614490548
SIGNAL_0 = -0.2718798058
SIGNAL_1 = 0.7465656072

I_TO_R = 0.956
I_TO_G = -0.272
I_TO_B = -1.105

Q_TO_R = 0.621
Q_TO_G = -0.647
Q_TO_B = 1.702


class TestFailure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def f32(x: float) -> float:
    return struct.unpack("f", struct.pack("f", float(x)))[0]


def clamp_zero_one(x: float) -> float:
    x = f32(x)
    if x < 0.0:
        return 0.0
    if x > 1.0:
        return 1.0
    return x


class Filter:
    def __init__(self) -> None:
        self.x = [0.0, 0.0, 0.0]
        self.y = [0.0, 0.0, 0.0]

    def chroma(self, z: float) -> float:
        self.x[0], self.x[1] = self.x[1], self.x[2]
        self.x[2] = z / CHROMA_GAIN
        self.y[0], self.y[1] = self.y[1], self.y[2]
        self.y[2] = (
            -self.x[0]
            + self.x[2]
            + CHROMA_0 * self.y[0]
            + CHROMA_1 * self.y[1]
        )
        return self.y[2]

    def luma(self, z: float) -> float:
        self.x[0], self.x[1] = self.x[1], self.x[2]
        self.x[2] = z / LUMA_GAIN
        self.y[0], self.y[1] = self.y[1], self.y[2]
        self.y[2] = (
            self.x[0]
            + self.x[2]
            + 2.0 * self.x[1]
            + LUMA_0 * self.y[0]
            + LUMA_1 * self.y[1]
        )
        return self.y[2]

    def signal(self, z: float) -> float:
        self.x[0], self.x[1] = self.x[1], self.x[2]
        self.x[2] = z / SIGNAL_GAIN
        self.y[0], self.y[1] = self.y[1], self.y[2]
        self.y[2] = (
            self.x[0]
            + self.x[2]
            + 2.0 * self.x[1]
            + SIGNAL_0 * self.y[0]
            + SIGNAL_1 * self.y[1]
        )
        return self.y[2]


def pack_bgra(bgra: tuple[int, int, int, int]) -> int:
    b, g, r, a = bgra
    return b | (g << 8) | (r << 16) | (a << 24)


def generate_color_tv_table() -> list[list[tuple[int, int, int, int]]]:
    signal_filter = Filter()
    chroma_filter = Filter()
    luma0_filter = Filter()
    luma1_filter = Filter()
    table = [
        [(0, 0, 0, 255) for _ in range(NTSC_NUM_SEQUENCES)]
        for _ in range(NTSC_NUM_PHASES)
    ]

    for phase in range(NTSC_NUM_PHASES):
        phi = phase * RAD_90 + CYCLESTART
        for sequence in range(NTSC_NUM_SEQUENCES):
            t = sequence
            y1 = c = i_value = q_value = 0.0

            for _ in range(12):
                z = 1.0 if (t & 0x800) else 0.0
                t <<= 1

                for _ in range(2):
                    zz = signal_filter.signal(z)
                    c = chroma_filter.chroma(zz)
                    luma0_filter.luma(zz)
                    y1 = luma1_filter.luma(zz - c)

                    c *= 2.0
                    i_value += (c * math.cos(phi) - i_value) / 8.0
                    q_value += (c * math.sin(phi) - q_value) / 8.0

                    phi += RAD_45

            color = sequence & 15
            r64 = y1 + I_TO_R * i_value + Q_TO_R * q_value
            g64 = y1 + I_TO_G * i_value + Q_TO_G * q_value
            b64 = y1 + I_TO_B * i_value + Q_TO_B * q_value

            r32 = clamp_zero_one(r64)
            g32 = clamp_zero_one(g64)
            b32 = clamp_zero_one(b64)
            if color == 15:
                r32 = g32 = b32 = 1.0
            if color == 0:
                r32 = g32 = b32 = 0.0

            table[phase][sequence] = (
                int(b32 * 255),
                int(g32 * 255),
                int(r32 * 255),
                255,
            )

    return table


def generate_base_colors() -> list[tuple[int, int, int]]:
    table = generate_color_tv_table()
    base = []
    for index in range(16):
        phase = 0
        signal_bits = 0
        bits = (index << 12) | (index << 8) | (index << 4) | index
        colors = [0, 0, 0, 0]

        for j in range(16):
            signal_bits = ((signal_bits << 1) | (bits & 1)) & 0xFFF
            colors[j & 3] = pack_bgra(table[phase][signal_bits])
            bits >>= 1
            phase = (phase + 1) & 3

        r = sum((color >> 16) & 0xFF for color in colors) // 4
        g = sum((color >> 8) & 0xFF for color in colors) // 4
        b = sum(color & 0xFF for color in colors) // 4
        base.append((r, g, b))

    return base


def extract_applewin_base_colors(source: str) -> list[tuple[int, int, int]]:
    match = re.search(
        r"g_aAppleWinBaseColors\[16\]\s*=\s*\{(?P<body>.*?)\n\};",
        source,
        re.S,
    )
    require(match is not None, "missing g_aAppleWinBaseColors[16]")
    triples = re.findall(
        r"ATN_BGR\(0x([0-9A-Fa-f]{2}),\s*0x([0-9A-Fa-f]{2}),\s*0x([0-9A-Fa-f]{2})\)",
        match.group("body"),
    )
    require(len(triples) == 16, "g_aAppleWinBaseColors must contain 16 ATN_BGR entries")
    return [tuple(int(channel, 16) for channel in triple) for triple in triples]


def extract_phase_index_map(source: str) -> list[list[int]]:
    match = re.search(
        r"g_aAppleWinPhaseColorIndex\[ATN_NUM_PHASES\]\[16\]\s*=\s*\{(?P<body>.*?)\n\};",
        source,
        re.S,
    )
    require(match is not None, "missing g_aAppleWinPhaseColorIndex[ATN_NUM_PHASES][16]")
    rows = re.findall(r"\{([^{}]+)\}", match.group("body"))
    require(len(rows) == 4, "phase color index map must contain four phase rows")
    parsed = []
    for row in rows:
        values = [int(token.rstrip("U"), 0) for token in re.findall(r"0x[0-9A-Fa-f]+|\d+U?", row)]
        require(len(values) == 16, "each phase color index row must contain 16 entries")
        require(sorted(values) == list(range(16)), "each phase row must be a 0..15 permutation")
        parsed.append(values)
    return parsed


def repeated_nibble_outputs(nibble: int, phase_map: list[list[int]]) -> list[int]:
    bits = (nibble << 12) | (nibble << 8) | (nibble << 4) | nibble
    phase = 0
    signal_bits = 0
    outputs = []

    for dot in range(16):
        signal_bits = ((signal_bits << 1) | (bits & 1)) & 0xFFF
        if dot >= 4:
            outputs.append(phase_map[phase][signal_bits & 0x0F])
        bits >>= 1
        phase = (phase + 1) & 3

    return outputs


def main() -> int:
    source = NTSC_C.read_text(encoding="utf-8")
    expected = generate_base_colors()
    actual = extract_applewin_base_colors(source)
    phase_map = extract_phase_index_map(source)

    require(actual == expected, "firmware AppleWin base palette does not match GenerateBaseColors()")

    for nibble in range(16):
        outputs = repeated_nibble_outputs(nibble, phase_map)
        require(
            all(output == nibble for output in outputs),
            f"repeated nibble {nibble:X} renders as {outputs}, expected all {nibble:X}",
        )

    rendered = " ".join(f"{index:X}:#{r:02X}{g:02X}{b:02X}" for index, (r, g, b) in enumerate(actual))
    print(f"AppleWin Idealized/RGB base palette: {rendered}")
    print("PASS AppleWin palette and repeated-nibble output mapping")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
