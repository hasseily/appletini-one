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
    start = source.find(f"{state_name}: begin", fsm_start)
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
    require("if (dma_ready) begin\n"
            "                        state_q <= S_W_MC_WRITE_WAIT;" in write_block,
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


TESTS = [
    test_ddr_to_apple_writes_wait_for_memory_completion,
]


def main() -> int:
    for test in TESTS:
        test()
        print(f"PASS {test.__name__}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
