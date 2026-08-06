#!/usr/bin/env python3
"""Source checks for the one-owner CPU0 PS-DMA service."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PSDMA_C = ROOT / "ps_sources/lib/psdma.c"
PSDMA_H = ROOT / "ps_sources/lib/psdma.h"
SMARTPORT = ROOT / "ps_sources/frontend/smartport_service.c"
CAL = ROOT / "ps_sources/frontend/psram_bench.c"
UART = ROOT / "ps_sources/frontend/uart_control.c"
BUILD = ROOT / "scripts/create_vitis_workspace.py"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    service = PSDMA_C.read_text(encoding="utf-8")
    header = PSDMA_H.read_text(encoding="utf-8")
    smartport = SMARTPORT.read_text(encoding="utf-8")
    cal = CAL.read_text(encoding="utf-8")
    uart = UART.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8")

    require("static psdma_owner_t g_psdma_owner" in service and
            "psdma_claim" in service and "psdma_release" in service,
            "PS-DMA must reject nested owners")
    require('#include "xiltimer.h"' in service and
            "XTime_GetTime" in service and "psdma_timeout_ticks" in service,
            "PS-DMA bounds must use elapsed time, not loop counts")
    require("psdma_abort_owned" in service and
            "PSDMA_BUSY_BIT" in service and "PSDMA_ABORTED_BIT" in service,
            "timeout handling must drain an accepted command")
    require("PSDMA_ERR_TIMEOUT means the abort drained safely" in header and
            "PSDMA_ERR_ABORT means the engine did not prove" in header,
            "the API must state its safe-fallback rule")
    require("psdma_transfer(PSDMA_OWNER_SMARTPORT" in smartport and
            "psdma_transfer(\n        PSDMA_OWNER_PSRAM_CAL" in cal and
            "psdma_transfer(PSDMA_OWNER_UART" in uart,
            "all three CPU0 users must use the shared service")
    require('"../../../ps_sources/lib/psdma.c"' in build,
            "the frontend build must include the shared service")
    require(build.count('"../../../ps_sources/lib/psdma.c"') == 2 and
            "Remove frontend-only sources from {name}" in build,
            "bootloader and CPU1 must remove the CPU0-only PS-DMA service")

    raw = "0x400300"
    for path in ROOT.glob("ps_sources/**/*.c"):
        if path == PSDMA_C:
            continue
        require(raw not in path.read_text(encoding="utf-8"),
                f"raw PS-DMA base remains in {path.relative_to(ROOT)}")

    print("PASS shared PS-DMA ownership and deadline checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
