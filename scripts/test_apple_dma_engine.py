#!/usr/bin/env python3
"""Source-level regression tests for the Apple DMA engine handshake."""

from __future__ import annotations

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
APPLE_DMA_ENGINE_SV = REPO_ROOT / "hdl" / "apple" / "apple_dma_engine.sv"


class TestFailure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def state_block(source: str, state_name: str) -> str:
    fsm_start = source.find("always_ff @(posedge clk)")
    require(fsm_start >= 0, "clocked FSM process must exist")
    # Abort dispatch also names several normal states. The last occurrence
    # is the state's own block in the main FSM case.
    start = source.rfind(f"{state_name}: begin", fsm_start)
    require(start >= 0, f"{state_name} state must exist")
    end = source.find("\n                S_", start + 1)
    if end < 0:
        end = source.find("\n                default:", start + 1)
    require(end > start, f"{state_name} state block must terminate")
    return source[start:end]


def test_ddr_to_apple_writes_wait_for_memory_completion() -> None:
    source = read(APPLE_DMA_ENGINE_SV)

    require("S_W_MC_WRITE, S_W_MC_WRITE_WAIT," in source,
            "DDR-to-Apple writes must have a post-acceptance wait state")

    write_block = state_block(source, "S_W_MC_WRITE")
    require("if (dma_ready) begin" in write_block and
            "state_q <= S_W_MC_WRITE_WAIT;" in write_block,
            "DDR-to-Apple write command acceptance must only enter the wait state")
    require("mc_lines_left_q <= mc_lines_left_q - 1;" not in write_block and
            "state_q <= S_DONE;" not in write_block,
            "DDR-to-Apple writes must not complete on dma_ready")

    wait_block = state_block(source, "S_W_MC_WRITE_WAIT")
    require("if (dma_rvalid) begin" in wait_block,
            "DDR-to-Apple writes must wait for the memory-controller completion pulse")
    require("mc_lines_left_q <= mc_lines_left_q - 1;" in wait_block and
            "state_q <= S_DONE;" in wait_block,
            "DDR-to-Apple writes may only advance or finish after dma_rvalid")


def test_abort_drains_accepted_work_before_acknowledging() -> None:
    source = read(APPLE_DMA_ENGINE_SV)

    require("input  logic                 req_abort" in source and
            "output logic                 req_abort_done" in source,
            "DMA client interface must expose a separate abort handshake")
    require("S_ABORT_DDR_R, S_ABORT_MC_WAIT, S_ABORT_DONE" in source,
            "abort must have states for draining AXI reads and PSRAM work")
    output_drive = source[source.find("// ---------------- Output drives"):
                          source.find("// ---------------- FSM update")]
    require("S_ABORT_DDR_R: begin" in output_drive and
            "axi_hp1_read.rready  = 1'b1;" in output_drive,
            "an accepted AXI read burst must stay ready until all beats drain")
    abort_read = state_block(source, "S_ABORT_DDR_R")
    require("burst_remaining_q <= burst_remaining_q - 5'd1;" in abort_read and
            "if (burst_remaining_q == 5'd1)" in abort_read and
            "state_q <= S_ABORT_DONE;" in abort_read,
            "abort completion must follow the final accepted AXI beat")
    abort_mc = state_block(source, "S_ABORT_MC_WAIT")
    require("if (dma_rvalid)" in abort_mc and
            "state_q <= S_ABORT_DONE;" in abort_mc,
            "abort completion must follow an accepted PSRAM operation")
    require("req_abort_done        = (state_q == S_ABORT_DONE);" in source,
            "the client may see abort done only after the drain states")
    require("dma_valid             = 1'b0;" in output_drive and
            "S_ABORT_MC_WAIT" not in output_drive,
            "abort drain states must never launch a new PSRAM request")


TESTS = [
    test_ddr_to_apple_writes_wait_for_memory_completion,
    test_abort_drains_accepted_work_before_acknowledging,
]


def main() -> int:
    for test in TESTS:
        test()
        print(f"PASS {test.__name__}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
