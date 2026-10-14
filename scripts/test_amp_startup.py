#!/usr/bin/env python3
"""Compile production CPU1 startup with fake MMIO and ARM barrier/wake hooks."""

import os
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/amp_startup_test"


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "xil_io.h").write_text(
        "#include <stdint.h>\nuint32_t Xil_In32(uint32_t);\n"
        "void Xil_Out32(uint32_t, uint32_t);\n"
    )
    (OUT / "xil_cache.h").write_text(
        "#include <stdint.h>\ntypedef intptr_t INTPTR;\n"
        "void Xil_DCacheFlushRange(INTPTR, uint32_t);\n"
        "void Xil_DCacheFlush(void);\n"
    )
    source = OUT / "startup.c"
    source.write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amp.h"

#define RESET_CTRL 0xF8000244U
#define RESET_BIT 2U
#define CLOCK_BIT 32U
#define VECTOR 0xFFFFFFF0U
#define UNLOCK 0xF8000008U
#define LOCK 0xF8000004U

const unsigned char core1_blob_start[] = {0x12, 0x34, 0x56, 0x78};
#ifdef EMPTY_BLOB
__asm__(".global core1_blob_end\n.set core1_blob_end, core1_blob_start + 0");
#else
__asm__(".global core1_blob_end\n.set core1_blob_end, core1_blob_start + 4");
#endif
const uint32_t core1_blob_size = sizeof(core1_blob_start);
static uint32_t control = 0x100U, addresses[16], values[16];
static unsigned writes, control_writes, drop_control_write, copies, wakes;
static char events[128];
static unsigned event_count;
static void event(char value) {
    assert(event_count + 1U < sizeof(events));
    events[event_count++] = value; events[event_count] = 0;
}
uint32_t Xil_In32(uint32_t address) {
    assert(address == RESET_CTRL); event('R'); return control;
}
void Xil_Out32(uint32_t address, uint32_t value) {
    assert(writes < 16U); addresses[writes] = address; values[writes++] = value;
    if (address == RESET_CTRL) {
        event('T'); ++control_writes;
        if (control_writes != drop_control_write) control = value;
    } else if (address == VECTOR) {
        event('V'); assert(value == AMP_CORE1_ENTRY_DEFAULT);
    } else if (address == UNLOCK) {
        event('U'); assert(value == 0xDF0DU);
    } else {
        assert(address == LOCK && value == 0x767BU); event('L');
    }
}
void *amp_test_memcpy(void *destination, const void *source, size_t length) {
    assert((uintptr_t)destination == AMP_CORE1_ENTRY_DEFAULT);
    assert(length == sizeof(core1_blob_start));
    assert(memcmp(source, core1_blob_start, length) == 0);
    ++copies; event('C'); return destination;
}
void Xil_DCacheFlushRange(intptr_t address, uint32_t length) {
    assert((uintptr_t)address == AMP_CORE1_ENTRY_DEFAULT);
    assert(length == sizeof(core1_blob_start)); event('F');
}
void Xil_DCacheFlush(void) { event('D'); }
void amp_test_memory_barrier(void) { event('B'); }
void amp_test_wake_core1(void) { ++wakes; event('W'); }
static void refused_without_activity(void) {
    const unsigned prior_events = event_count, prior_writes = writes;
    const unsigned prior_copies = copies, prior_wakes = wakes;
    assert(amp_release_core1() == 0U);
    assert(event_count == prior_events && writes == prior_writes);
    assert(copies == prior_copies && wakes == prior_wakes);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const unsigned scenario = (unsigned)strtoul(argv[1], NULL, 10);
    if (scenario == 0U) {
        assert(amp_release_core1() == sizeof(core1_blob_start));
        assert(strcmp(events, "RCFVDBW") == 0);
        assert(writes == 1U && addresses[0] == VECTOR);
        assert(!control_writes && control == 0x100U && copies == 1U && wakes == 1U);
        refused_without_activity();
    } else if (scenario <= 3U) {
        control |= scenario == 1U ? RESET_BIT : scenario == 2U ? CLOCK_BIT : RESET_BIT | CLOCK_BIT;
        assert(amp_release_core1() == 0U);
        assert(strcmp(events, "R") == 0 && !writes && !copies && !wakes);
        control = 0x100U; /* Clearing external state cannot turn this into a retry. */
        refused_without_activity();
    } else if (scenario <= 6U) {
        drop_control_write = scenario == 5U ? 1U : scenario == 6U ? 2U : 0U;
        const int rc = amp_hold_core1();
        assert((rc == 0) == (scenario == 4U));
        assert(addresses[0] == UNLOCK && addresses[writes - 1U] == LOCK);
        assert(values[1] == 0x102U);
        assert(control == (scenario == 5U ? 0x100U : scenario == 6U ? 0x102U : 0x122U));
        assert(control_writes == (scenario == 5U ? 1U : 2U));
        if (scenario != 5U) assert(values[2] == 0x122U);
        assert(!copies && !wakes);
        refused_without_activity(); /* Includes a dropped reset with no held bits. */
    } else if (scenario == 7U || scenario == 8U) {
        assert(amp_release_core1() == sizeof(core1_blob_start));
        drop_control_write = scenario == 8U ? 2U : 0U;
        assert((amp_hold_core1() == 0) == (scenario == 7U));
        assert(control == (scenario == 7U ? 0x122U : 0x102U));
        refused_without_activity();
        assert(copies == 1U && wakes == 1U); /* No warm restart or DDR overwrite. */
    } else {
        assert(scenario == 9U);
        assert(amp_release_core1() == 0U && !writes && !copies && !wakes);
        refused_without_activity();
    }
    return 0;
}
''')
    compiler = shutil.which("gcc") or shutil.which("clang")
    if not compiler:
        compiler = "C:/msys64/ucrt64/bin/gcc.exe"
    env = dict(os.environ, PATH=str(Path(compiler).parent) + os.pathsep + os.environ.get("PATH", ""))
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O0",
             "-I", str(OUT), "-I", str(ROOT / "ps_sources/lib")]
    obj = OUT / "amp.o"
    subprocess.run([compiler, *flags, "-DAMP_HOST_TEST", "-Dmemcpy=amp_test_memcpy",
                    "-c", str(ROOT / "ps_sources/lib/amp.c"), "-o", str(obj)], check=True, env=env)
    exe = OUT / "startup.exe"
    subprocess.run([compiler, *flags, str(source), str(obj), "-o", str(exe)], check=True, env=env)
    for scenario in range(9):
        subprocess.run([str(exe), str(scenario)], check=True, env=env)
    empty_exe = OUT / "empty.exe"
    subprocess.run([compiler, *flags, "-DEMPTY_BLOB", str(source), str(obj),
                    "-o", str(empty_exe)], check=True, env=env)
    subprocess.run([str(empty_exe), "9"], check=True, env=env)
    print("PASS: production CPU1 cold SEV startup, single launch, held/empty refusal, checked hold failures and no warm restart")


if __name__ == "__main__":
    main()
