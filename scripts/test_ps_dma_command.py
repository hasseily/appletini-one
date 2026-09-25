#!/usr/bin/env python3
"""Run DMA register, GP0 completion-retention, and abort-drain RTL tests."""

from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "ps_dma_command_sim"


def run(tool: str, *args: str) -> str:
    executable = shutil.which(tool + ".bat") or shutil.which(tool)
    if not executable:
        raise RuntimeError(f"Vivado tool {tool} is required on PATH")
    result = subprocess.run([executable, *args], cwd=OUT, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (OUT / f"{tool}_{args[0].replace('/', '_')}.log").write_text(
        result.stdout, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(result.stdout)
    return result.stdout


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    sources = ["hdl/globals.sv", *[
        "hdl/axisimple/" + name for name in (
            "skidbuffer.v", "sfifo.v", "addrdecode.v", "axi_addr.v",
            "axidouble.v", "axisimple_wrapper.sv")],
        "hdl/apple/ps_dma_command.sv", "hdl/apple/apple_dma_engine.sv",
        "hdl/sim/tb_ps_dma_command.sv", "hdl/sim/tb_ps_dma_completion.sv",
        "hdl/sim/tb_apple_dma_abort.sv"]
    run("xvlog", "--sv", *[str(ROOT / source) for source in sources])
    for bench, marker in (
        ("tb_ps_dma_command", "PS DMA COMMAND PASS"),
        ("tb_ps_dma_completion", "PS DMA COMPLETION PASS"),
        ("tb_apple_dma_abort", "APPLE DMA ABORT PASS"),
    ):
        run("xelab", bench, "-s", bench + "_snap")
        output = run("xsim", bench + "_snap", "--runall")
        if marker not in output:
            raise RuntimeError(output)
        print(marker)


if __name__ == "__main__":
    main()
