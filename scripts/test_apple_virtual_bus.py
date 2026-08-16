#!/usr/bin/env python3
"""Build and run the isolated virtual Apple bus scaffold bench."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "apple_virtual_bus_sim"
RTL = ROOT / "hdl" / "apple" / "apple_virtual_bus.sv"
BENCH = ROOT / "hdl" / "sim" / "tb_apple_virtual_bus.sv"
HDL_SOURCES = ROOT / "hdl" / "hdl_sources.txt"


def vivado_tool(name: str) -> str:
    bat = shutil.which(f"{name}.bat")
    if bat:
        return bat
    tool = shutil.which(name)
    if tool:
        return tool
    raise FileNotFoundError(f"unable to locate Vivado tool {name}")


def run(command: list[str], log_name: str) -> str:
    completed = subprocess.run(
        command,
        cwd=OUT_DIR,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    (OUT_DIR / log_name).write_text(completed.stdout, encoding="utf-8")
    if completed.returncode != 0:
        print(completed.stdout)
        raise RuntimeError(
            f"{Path(command[0]).name} failed with "
            f"exit code {completed.returncode}"
        )
    return completed.stdout


def static_contract_checks() -> None:
    sources = HDL_SOURCES.read_text(encoding="utf-8").splitlines()
    rtl = RTL.read_text(encoding="utf-8")

    if sources.count("apple/apple_virtual_bus.sv") != 1:
        raise RuntimeError(
            "apple_virtual_bus.sv must appear once in hdl_sources.txt"
        )
    for phase in ("drive_en", "addr_en", "sss_en", "serve_en", "data_en"):
        if phase not in rtl:
            raise RuntimeError(f"virtual bus is missing {phase}")
    if "output globals::AppleBus_read   ab_read" not in rtl:
        raise RuntimeError("virtual bus must provide the shared AppleBus_read contract")
    if "input  globals::AppleBus_write  ab_write" not in rtl:
        raise RuntimeError("virtual bus must consume the shared AppleBus_write contract")


def main() -> None:
    static_contract_checks()

    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    OUT_DIR.mkdir(parents=True)

    run(
        [
            vivado_tool("xvlog"),
            "--sv",
            str(ROOT / "hdl" / "globals.sv"),
            str(RTL),
            str(BENCH),
        ],
        "xvlog.log",
    )
    run(
        [
            vivado_tool("xelab"),
            "tb_apple_virtual_bus",
            "-s",
            "tb_apple_virtual_bus_snap",
            "--timescale",
            "1ns/1ps",
        ],
        "xelab.log",
    )
    output = run(
        [
            vivado_tool("xsim"),
            "tb_apple_virtual_bus_snap",
            "--runall",
        ],
        "xsim.log",
    )
    if "APPLE VIRTUAL BUS PASS" not in output:
        raise RuntimeError("virtual Apple bus bench did not report PASS")
    print("APPLE VIRTUAL BUS PASS")


if __name__ == "__main__":
    main()
