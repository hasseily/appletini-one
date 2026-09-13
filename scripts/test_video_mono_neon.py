#!/usr/bin/env python3
"""Run the production mono/glow ARM NEON code against a scalar pixel oracle.

Requires ARM_CC (or the installed Xilinx Cortex-A9 compiler) and Unicorn:
  python -m pip install --target build/video_mono_neon_test/deps unicorn
  python scripts/test_video_mono_neon.py

Only ignored build files are generated. No board connection is used.
"""

import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "video_mono_neon_test"
WIDTH = 640
SENTINEL = 0xA55AA55A


def build_arm():
    compiler = os.environ.get("ARM_CC") or shutil.which("arm-none-eabi-gcc")
    fallback = Path("C:/Xilinx/2025.2/Vitis/gnu/aarch32/nt/"
                    "gcc-arm-none-eabi/bin/arm-none-eabi-gcc.exe")
    if not compiler and fallback.exists():
        compiler = str(fallback)
    if not compiler:
        raise RuntimeError("Set ARM_CC to an ARM GCC compiler")
    compiler = Path(compiler)
    suffix = compiler.suffix
    prefix = compiler.name.removesuffix(suffix).removesuffix("gcc")
    nm = compiler.with_name(prefix + "nm" + suffix)
    objcopy = compiler.with_name(prefix + "objcopy" + suffix)
    objdump = compiler.with_name(prefix + "objdump" + suffix)
    BUILD.mkdir(parents=True, exist_ok=True)
    source = (ROOT / "ps_sources/frontend/compositor.c").read_text()
    effects = source[source.index("#define EFFECT_HISTORY_STRIDE"):
                     source.index("/* ---------- Format badge")]
    wrapper = BUILD / "mono_neon.c"
    wrapper.write_text('''#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
#include "video_mono.h"
#include "compositor_layout.h"
#include "video_blur.h"
#include "video_glow.h"
#include "video_ghosting.h"
#include "scanlines.h"
static int smartport_service_has_pending(void) { return 0; }
static void smartport_service_poll(void) {}
static uint8_t s_video_dot_bleed;
''' + effects + '''
uint32_t test_params[6];
uint32_t test_rows[4][COMP_APPLE_SHR_WIDTH] __attribute__((aligned(32)));
/* Freestanding memory helpers; no BSP, libc, or hardware addresses. */
void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}
void *memset(void *dst, int value, size_t n) {
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)value;
    return dst;
}
void run_case(void) {
    const int w = (int)test_params[0];
    const uint8_t color = (uint8_t)test_params[3];
    s_video_dot_bleed = (uint8_t)test_params[4];
    s_mono_x = (int)test_params[5];
    s_mono_y = 0;
    s_mono_width = w - 2*s_mono_x;
    s_mono_height = 1;
    s_mono_channel_shift = video_mono_channel_shift(color);
    video_mono_build_tint(s_mono_tint, color);
    effect_emit_2x_row(test_rows[0], test_rows[1], test_rows[2],
                       test_rows[3], w, (uint8_t)test_params[1],
                       (uint8_t)test_params[2], 0);
}
''')
    elf = BUILD / "mono_neon.elf"
    subprocess.run([str(compiler), "-std=c11", "-O2", "-mcpu=cortex-a9",
                    "-mfpu=neon", "-mfloat-abi=hard", "-marm",
                    "-ffreestanding", "-fno-builtin", "-ffunction-sections",
                    "-fdata-sections", "-fno-unwind-tables",
                    "-fno-asynchronous-unwind-tables", "-nostdlib",
                    "-I" + str(ROOT / "ps_sources/frontend"),
                    "-I" + str(ROOT / "ps_sources/lib"), str(wrapper),
                    str(ROOT / "ps_sources/lib/fb16.c"),
                    "-Wl,--gc-sections,--build-id=none,-Ttext=0x10000,-e,run_case",
                    "-lgcc", "-o", str(elf)], check=True)
    binary = BUILD / "mono_neon.bin"
    subprocess.run([str(objcopy), "-O", "binary", str(elf), str(binary)],
                   check=True)
    symbols = {}
    for line in subprocess.check_output([str(nm), "-n", str(elf)], text=True).splitlines():
        fields = line.split()
        if len(fields) == 3:
            symbols[fields[2]] = int(fields[0], 16)
    assembly = subprocess.check_output([str(objdump), "-d", str(elf)], text=True)
    (BUILD / "mono_neon.asm").write_text(assembly)
    for instruction in ("vld4.8", "vshl.u8", "vqadd.u8", "vst4.8"):
        assert instruction in assembly, f"missing NEON instruction: {instruction}"
    return binary.read_bytes(), symbols


def rgb565(r, g, b):
    return ((r & 248) << 8) | ((g & 252) << 3) | (b >> 3)


def reference(rows, width, blur, glow, color, bleed, border):
    shift = {1: 3, 2: 2, 3: 1}[glow]
    pixels = []
    for x in range(width):
        channels = []
        for bit in (0, 8, 16):
            values = [(row[x] >> bit) & 255 for row in rows]
            halo = (values[2] >> 1) + (values[1] >> 2) + (values[3] >> 2)
            base = values[0] if blur == 0 else values[2] if blur == 1 else halo
            channels.append(min(255, base + (halo >> shift)))
        pixels.append(channels[0] | (channels[1] << 8) | (channels[2] << 16))
    output = []
    taps = {1: ((1, 3), (3, 1), (-1, 0), (0, 1), 4),
            2: ((12, 20), (20, 12), (-1, 0), (0, 1), 32),
            3: ((14, 17, 1), (1, 17, 14), (-1, 0, 1), (-1, 0, 1), 32)}
    for x, pixel in enumerate(pixels):
        if x < border or x >= width - border:
            packed = rgb565((pixel >> 16) & 255, (pixel >> 8) & 255, pixel & 255)
            output.extend((packed, packed))
            continue
        left, right, loff, roff, divisor = taps[bleed]
        for weights, offsets in ((left, loff), (right, roff)):
            total = sum(weight * ((pixels[max(border, min(width-border-1, x+offset))]
                                   >> (8 if color == 3 else 16)) & 255)
                        for weight, offset in zip(weights, offsets))
            y = (total + divisor // 2) // divisor
            r, g, b = y, y, y
            if color == 0:
                r = g = b = 0
            elif color == 2:
                g, b = y * 128 // 255, y // 255
            elif color == 3:
                r, b = y * 8 // 181, y * 82 // 181
            output.append(rgb565(r, g, b))
    return pixels, output


def main():
    sys.path.insert(0, str(BUILD / "deps"))
    try:
        from unicorn import (Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_MEM_READ,
                             UC_HOOK_MEM_WRITE, UC_MEM_WRITE)
        from unicorn.arm_const import (UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC,
                                       UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC,
                                       UC_CPU_ARM_CORTEX_A9)
    except ImportError as exc:
        raise RuntimeError("Install Unicorn using the command in this script's docstring") from exc
    binary, symbols = build_arm()
    cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
    cpu.mem_map(0x10000, 0x400000)
    cpu.mem_write(0x10000, binary)
    cpu.reg_write(UC_ARM_REG_C1_C0_2, 0xF << 20)
    cpu.reg_write(UC_ARM_REG_FPEXC, 1 << 30)
    cpu.reg_write(UC_ARM_REG_SP, 0x400000)
    stop = 0x3F0000
    cpu.reg_write(UC_ARM_REG_LR, stop)
    row_addr = symbols["test_rows"]
    active_width = 0
    failures = []

    def bounds_hook(uc, access, address, size, value, user_data):
        if row_addr <= address < row_addr + 4 * WIDTH * 4:
            offset = (address - row_addr) % (WIDTH * 4)
            if offset + size > active_width * 4:
                failures.append(("source read", address, size))
                uc.emu_stop()
        for name, item_size in (("s_effect_row", 4), ("s_effect_2x_row", 4)):
            start = symbols[name]
            if access == UC_MEM_WRITE and start <= address < start + WIDTH * 4:
                if address + size > start + active_width * item_size:
                    failures.append(("output write", name, address, size))
                    uc.emu_stop()

    cpu.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, bounds_hook)
    randomizer = random.Random(109)
    count = 0
    for width in (1, 2, 3, 7, 8, 9, 14, 15, 16, 17, 31, 559, 560, 616, 640):
        active_width = width
        rows = [[randomizer.getrandbits(32) for _ in range(width)] for _ in range(4)]
        for row in rows:
            # Exercise saturated channels, black/white, and ignored alpha.
            row[:min(4, width)] = [0xFF000000, 0xFFFFFFFF, 0xFF00FF80, 0x7FFFFF00][:width]
        for index, row in enumerate(rows):
            cpu.mem_write(row_addr + index*WIDTH*4, struct.pack("<" + "I"*width, *row))
        for blur in range(4):
            for glow in (1, 2, 3):
                for color in range(4):
                    for bleed in (1, 2, 3):
                        border = 2 if width > 4 and (color + bleed) % 2 else 0
                        params = (width, blur, glow, color, bleed, border)
                        cpu.mem_write(symbols["test_params"], struct.pack("<6I", *params))
                        for name in ("s_effect_row", "s_effect_2x_row"):
                            cpu.mem_write(symbols[name], struct.pack("<I", SENTINEL)*WIDTH)
                        cpu.reg_write(UC_ARM_REG_LR, stop)
                        cpu.emu_start(symbols["run_case"], stop, count=2000000)
                        assert not failures, (params, failures)
                        assert cpu.reg_read(UC_ARM_REG_PC) == stop, (params, "instruction limit")
                        expected_row, expected_output = reference(rows, *params)
                        for name, expected, fmt in (("s_effect_row", expected_row, "I"),
                                                    ("s_effect_2x_row", expected_output, "H")):
                            size = struct.calcsize(fmt) * len(expected)
                            actual = struct.unpack("<" + fmt*len(expected), cpu.mem_read(symbols[name], size))
                            assert list(actual) == expected, (params, name, next(
                                (i, a, b) for i, (a, b) in enumerate(zip(actual, expected)) if a != b))
                            guard = cpu.mem_read(symbols[name] + size, WIDTH*4 - size)
                            assert guard == struct.pack("<I", SENTINEL) * (WIDTH-width), (params, name, "guard")
                        count += 1
    print(f"PASS {count} Cortex-A9 ARM executions: all blur/glow/tint/bleed levels, "
          "vector boundaries, scalar tails, borders, saturation, alpha, and row guards")


if __name__ == "__main__":
    main()
