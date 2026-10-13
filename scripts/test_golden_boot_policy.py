#!/usr/bin/env python3
"""Exercise production Golden eligibility, SmartPort pending and DMA ownership.

The native fixture uses the firmware's functions, register definitions and
PS-DMA header. Register readbacks are simulated; this is not a board test.
"""

import os
from pathlib import Path
import re
import shutil
import subprocess

from test_joystick_config_menu import function

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/golden-boot-policy-test"


def definitions(source: str, prefix: str) -> str:
    return "\n".join(
        line for line in source.splitlines()
        if re.match(r"^#define\s+" + re.escape(prefix), line)
    ) + "\n"


PREAMBLE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "framebuffer.h"
#include "psdma.h"
static uint32_t boot_status,boot_handoff,sp_status,dma_status;
static unsigned failed_boot_status,failed_boot_handoff,dma_reads;
static volatile uint32_t g_cmd_pending_count;
static psdma_owner_t g_psdma_owner=PSDMA_OWNER_NONE;
'''

READBACKS = r'''
static uint32_t read_register(uint32_t address)
{
    if(address==BOOT_MENU_REG_STATUS)
        return failed_boot_status ? UINT32_MAX:boot_status;
    if(address==BOOT_MENU_REG_HANDOFF_MODE)
        return failed_boot_handoff ? UINT32_MAX:boot_handoff;
    assert(address==PSDMA_STATUS_REG);
    ++dma_reads;
    return dma_status;
}
#define REG_READ(address) read_register(address)
static uint32_t sp_hw_status(void) { return sp_status; }
'''

SCENARIOS = r'''
static void initial_menu_policy(void)
{
    unsigned flags,mode,pending,cases=0;
    /* Real menu acknowledgements and machine ID occupy other STATUS bits.
     * Eligibility comes from the boot ROM's live ownership, never PS ACK. */
    boot_handoff=1U;
    boot_status=0x03002000U|1U|4U;
    assert(boot_menu_service_initial_menu_active());
    boot_status=0x03002000U|1U|2U;
    assert(boot_menu_service_initial_menu_active());
    boot_status=0x03002000U|1U|32U;
    assert(!boot_menu_service_initial_menu_active());
    boot_status=0x03002000U|32U;
    assert(!boot_menu_service_initial_menu_active());
    /* Bootgen target choice is separate from current execution mode.
     * Pending is bit5: 24 zeros + pending + reserved + mode2 + target2. */
    for(flags=0;flags<128U;++flags)
        for(mode=0;mode<4U;++mode)
            for(pending=0;pending<2U;++pending) {
                unsigned expected=(flags&1U) && (flags&(2U|4U)) &&
                    !(flags&(8U|16U)) && !mode && !pending;
                boot_status=0x03002000U|flags;
                boot_handoff=2U|(mode<<2U)|(pending<<5U);
                assert(!!boot_menu_service_initial_menu_active()==!!expected);
                ++cases;
            }
    boot_status=1U|4U;
    boot_handoff=1U|(1U<<5U);
    assert(!boot_menu_service_initial_menu_active());
    boot_handoff=1U;
    failed_boot_status=1;
    assert(!boot_menu_service_initial_menu_active());
    failed_boot_status=0;
    failed_boot_handoff=1;
    assert(!boot_menu_service_initial_menu_active());
    failed_boot_handoff=0;
    boot_status=boot_handoff=0;
    assert(!boot_menu_service_initial_menu_active());
    /* Apple reset clears boot eligibility and transport ownership. */
    boot_status=4U|32U;
    boot_handoff=1U;
    assert(!boot_menu_service_initial_menu_active());
    printf("PASS: %u live initial-ROM policy combinations, ACK-only/zero/unreadable/reset denial\n",cases);
}
static void smartport_pending_policy(void)
{
    g_cmd_pending_count=0;sp_status=0;
    assert(!smartport_service_has_pending());
    g_cmd_pending_count=1;
    assert(smartport_service_has_pending());
    g_cmd_pending_count=0;sp_status=SP_ST_EXEC_PENDING;
    assert(smartport_service_has_pending());
    g_cmd_pending_count=2;
    assert(smartport_service_has_pending());
    g_cmd_pending_count=0;sp_status=SP_ST_READY|SP_ST_EXEC_VTW|7U;
    assert(!smartport_service_has_pending());
    puts("PASS: SmartPort IRQ-only, hardware-only, both and neither pending");
}
static void dma_ownership_policy(void)
{
    unsigned before;
    dma_status=0;dma_reads=0;
    assert(psdma_current_owner()==PSDMA_OWNER_NONE);
    assert(psdma_acquire(PSDMA_OWNER_NONE)==PSDMA_ERR_ARG && !dma_reads);
    assert(psdma_current_owner()==PSDMA_OWNER_NONE);
    assert(psdma_acquire(PSDMA_OWNER_FIRMWARE_UPDATE)==PSDMA_OK);
    assert(psdma_current_owner()==PSDMA_OWNER_FIRMWARE_UPDATE && dma_reads==1);
    before=dma_reads;
    assert(psdma_acquire(PSDMA_OWNER_SMARTPORT)==PSDMA_ERR_OWNED);
    assert(psdma_acquire(PSDMA_OWNER_FIRMWARE_UPDATE)==PSDMA_ERR_OWNED);
    assert(dma_reads==before);
    psdma_release(PSDMA_OWNER_SMARTPORT);
    assert(psdma_current_owner()==PSDMA_OWNER_FIRMWARE_UPDATE);
    psdma_release(PSDMA_OWNER_FIRMWARE_UPDATE);
    assert(psdma_current_owner()==PSDMA_OWNER_NONE);
    dma_status=PSDMA_BUSY_BIT;
    assert(psdma_acquire(PSDMA_OWNER_FIRMWARE_UPDATE)==PSDMA_ERR_BUSY);
    assert(psdma_current_owner()==PSDMA_OWNER_NONE);
    /* DONE or ABORTED do not establish safety while BUSY remains set. */
    dma_status=PSDMA_BUSY_BIT|PSDMA_DONE_BIT|PSDMA_ABORTED_BIT;
    assert(psdma_acquire(PSDMA_OWNER_FIRMWARE_UPDATE)==PSDMA_ERR_BUSY);
    assert(psdma_current_owner()==PSDMA_OWNER_NONE);
    dma_status=PSDMA_DONE_BIT|PSDMA_ABORTED_BIT;
    assert(psdma_acquire(PSDMA_OWNER_SMARTPORT)==PSDMA_OK);
    assert(psdma_acquire(PSDMA_OWNER_FIRMWARE_UPDATE)==PSDMA_ERR_OWNED);
    psdma_release(PSDMA_OWNER_FIRMWARE_UPDATE);
    assert(psdma_current_owner()==PSDMA_OWNER_SMARTPORT);
    psdma_release(PSDMA_OWNER_SMARTPORT);
    assert(psdma_acquire(PSDMA_OWNER_FIRMWARE_UPDATE)==PSDMA_OK);
    psdma_release(PSDMA_OWNER_FIRMWARE_UPDATE);
    assert(psdma_current_owner()==PSDMA_OWNER_NONE);
    puts("PASS: production PS-DMA owner conflict, real BUSY, correct release and drained completion");
}
int main(void)
{
    initial_menu_policy();smartport_pending_policy();dma_ownership_policy();
    return 0;
}
'''


def main() -> None:
    boot = (ROOT / "ps_sources/frontend/boot_menu_service.c").read_text()
    smartport = (ROOT / "ps_sources/frontend/smartport_service.c").read_text()
    dma = (ROOT / "ps_sources/lib/psdma.c").read_text()
    code = (
        PREAMBLE + definitions(boot, "BOOT_MENU_") +
        definitions(smartport, "SP_ST_") + definitions(dma, "PSDMA_") +
        READBACKS + function(boot, "boot_menu_service_initial_menu_active") +
        function(smartport, "smartport_service_has_pending") +
        "\n".join(function(dma, name) for name in (
            "psdma_claim", "psdma_release", "psdma_acquire", "psdma_current_owner"
        )) + SCENARIOS
    )
    OUT.mkdir(parents=True, exist_ok=True)
    harness = OUT / "policy.c"
    harness.write_text(code)
    compiler = shutil.which("gcc") or "C:/msys64/ucrt64/bin/gcc.exe"
    env = dict(os.environ, PATH=str(Path(compiler).parent) + os.pathsep + os.environ.get("PATH", ""))
    executable = OUT / "policy.exe"
    subprocess.run([
        compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O0",
        "-I", str(ROOT / "ps_sources/lib"), str(harness), "-o", str(executable),
    ], check=True, env=env)
    subprocess.run([str(executable)], check=True, env=env)


if __name__ == "__main__":
    main()
