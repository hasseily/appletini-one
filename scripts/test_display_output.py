#!/usr/bin/env python3
"""Exercise the real mode-switch driver with modeled MMIO and clock failures."""
from pathlib import Path
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/display_output_test"
FRONT = ROOT / "ps_sources/frontend"


def main():
    compiler = shutil.which("gcc") or str(Path(
        "E:/AMDDesignTools/2025.2/tps/mingw/10.0.0/win64.o/nt/bin/gcc.exe"))
    if not Path(compiler).is_file():
        raise RuntimeError("A native GCC compiler is required")
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "xiltimer.h").write_text(
        "#pragma once\n#include <stdint.h>\n"
        "typedef uint64_t XTime;\n#define COUNTS_PER_SECOND 100U\n"
        "void XTime_GetTime(XTime *time);\n")
    source = (FRONT / "display_output.c").read_text().replace(
        '#include "../lib/common.h"', '')
    code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "xiltimer.h"
#include "compositor.h"
#include "compositor_layout.h"
#include "display_modes.h"
#include "../lib/framebuffer.h"
const uint32_t comp_out_slot_addr[COMP_OUT_SLOT_COUNT] = {
    0x3e000000U, 0x3e400000U, 0x3e800000U
};
static uint32_t status, staged, writes, polls, ticks, services, clears;
static uint8_t current_mode, requested, paused, fault, in_flight;
static uint32_t read_reg(uintptr_t addr)
{
    if (addr == FB_LAST_LATCHED_REG) return comp_out_slot_addr[0];
    if (addr == FB_BASE_ADDR_REG) return comp_out_slot_addr[1];
    assert(addr == FB_MODE_STATUS_REG);
    if (in_flight && ++polls > 2 && fault != 2) {
        in_flight = 0;
        status = FB_MODE_SIGNATURE | FB_MODE_LOCKED |
            (fault ? FB_MODE_ERROR | DISPLAY_MODE_DEFAULT : requested) |
            (fault == 3 ? FB_MODE_HELD : 0);
    }
    return status;
}
static void write_reg(uintptr_t addr, uint32_t value)
{
    ++writes;
    if (addr == FB_MODE_BASE_REG) { staged = value; return; }
    assert(addr == FB_MODE_REQUEST_REG);
    assert(staged == comp_out_slot_addr[2] && clears == 1 && paused);
    requested = (uint8_t)value;
    in_flight = 1;
    status = FB_MODE_SIGNATURE | FB_MODE_BUSY | DISPLAY_MODE_DEFAULT;
}
static void *clear_frame(void *ptr, int value, size_t size)
{
    assert((uintptr_t)ptr == comp_out_slot_addr[2]);
    assert(value == 0 && size == COMP_OUT_MAX_BYTES && paused);
    ++clears;
    return ptr;
}
void XTime_GetTime(XTime *time) { *time = ticks++; }
void compositor_set_paused(uint8_t value) { paused = value; }
void compositor_set_output_mode(uint8_t mode) { current_mode = mode; }
static void service(void) { ++services; }
#define REG_READ(addr) read_reg(addr)
#define REG_WRITE(addr, value) write_reg(addr, value)
#define memset clear_frame
'''
    code += source
    code += r'''
#undef memset
static void reset(void)
{
    staged = writes = polls = ticks = services = clears = 0;
    paused = fault = in_flight = 0;
    current_mode = DISPLAY_MODE_DEFAULT;
    status = FB_MODE_SIGNATURE | FB_MODE_LOCKED | DISPLAY_MODE_DEFAULT;
}
int main(void)
{
    for (uint8_t mode = 0; mode < DISPLAY_MODE_COUNT; ++mode) {
        reset();
        assert(display_output_apply(mode, service) == 0);
        assert(current_mode == mode && !paused);
        if (mode != DISPLAY_MODE_DEFAULT) {
            assert(writes == 2 && clears == 1 && services > 0);
        } else assert(writes == 0 && clears == 0);
    }
    reset(); status = 0;
    assert(display_output_apply(DISPLAY_MODE_DEFAULT, service) == 0);
    assert(display_output_apply(0, service) == -2 && writes == 0);
    reset();
    assert(display_output_apply(DISPLAY_MODE_COUNT, service) == -1 && writes == 0);
    reset(); status |= FB_MODE_BUSY;
    assert(display_output_apply(0, service) == -3 && writes == 0);
    reset(); fault = 1;
    assert(display_output_apply(0, service) == -5);
    assert(current_mode == DISPLAY_MODE_DEFAULT && !paused);
    reset(); fault = 2;
    assert(display_output_apply(0, service) == -5);
    assert(paused && ticks >= 200 && ticks < 210 && services > 0);
    reset(); fault = 3;
    assert(display_output_apply(0, service) == -5 && paused);
    puts("PASS: mode switch, safe slot, old PL, failure recovery, bounded timeout");
    return 0;
}
'''
    harness = OUT / "test.c"
    harness.write_text(code)
    exe = OUT / "test.exe"
    env = dict(os.environ)
    env["PATH"] = str(Path(compiler).parent) + os.pathsep + env.get("PATH", "")
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(OUT), "-I", str(FRONT), str(harness), "-o", str(exe)],
                   check=True, env=env)
    subprocess.run([str(exe)], check=True, env=env)


if __name__ == "__main__":
    main()
