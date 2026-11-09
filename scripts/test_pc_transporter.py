#!/usr/bin/env python3
"""Run the PC Transporter host and FPGA simulation tests."""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
BIOS_SHA = "b2ac846f17f5a16ca80c80b7fb608746c72e8a2c2918dcf10ffa51f890d1a886"
ROUTING_SHA = "75692784f1fe5e8aa4bf7bcf587f76fb40b5292ef9b9d8ba3504add5f9250b9a"


def run(argv, cwd, env=None):
    subprocess.run([str(arg) for arg in argv], cwd=cwd, env=env, check=True)


def host_tests(out, bios, routing):
    cc = os.environ.get("CC") or shutil.which("gcc") or shutil.which("clang")
    if cc is None and Path("C:/msys64/ucrt64/bin/gcc.exe").is_file():
        cc = "C:/msys64/ucrt64/bin/gcc.exe"
    if cc is None:
        raise RuntimeError("Set CC to a host C compiler")
    env = os.environ.copy()
    env["PATH"] = str(Path(cc).parent) + os.pathsep + env["PATH"]
    front = ROOT / "ps_sources/frontend"
    core = ROOT / "third_party/xtulator_cpu"
    flags = [cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-I", front, "-I", core, "-I", out]
    test_inputs = [bios, routing] if bios is not None else []
    machine = front / "pc_transporter_machine.c"
    ad8088 = front / "ad8088_machine.c"
    cpu = core / "cpu.c"
    service = (front / "pc_transporter_service.c").read_text(encoding="utf-8")
    for header in ("xil_cache.h", "xil_mmu.h", "xiltimer.h", "../lib/common.h", "../lib/uart.h"):
        service = service.replace(f'#include "{header}"\n', "")
    # The native fixture supplies MMIO, DDR and BSP functions. It runs on one thread.
    barrier = '__asm__ volatile ("dsb sy" ::: "memory");'
    (out / "pc_transporter_service_native.inc").write_text(
        service.replace(barrier, "(void)0;"), encoding="utf-8")
    jobs = (
        ("test_pc_transporter_machine", [machine, ad8088, cpu], test_inputs),
        ("test_pc_transporter_service", [machine], []),
        ("test_pc_transporter_video", [front / "pc_transporter_video.c",
                                       front / "linear_text_overlay_font.c"], []),
        ("test_ad8088_cpu", [machine, ad8088, cpu], []),
    )
    for name, sources, inputs in jobs:
        program = out / (name + (".exe" if os.name == "nt" else ""))
        run([*flags, ROOT / f"scripts/{name}.c", *sources, "-o", program], ROOT, env)
        run([program, *inputs], out, env)


def vivado_tool(name):
    tool = shutil.which(f"{name}.bat") or shutil.which(name)
    if tool is None:
        raise RuntimeError(f"Put Vivado {name} on PATH")
    return tool


def rtl_tests(out):
    hdl = ROOT / "hdl"
    sources = ["globals.sv", "apple/soft_switch_manager.sv", "apple/pc_transporter_pit.sv",
               "apple/pc_transporter_card.sv", "apple/apple_virtual_bus.sv", "apple/disk2_ddr_bridge.sv",
               "sim/tb_pc_transporter_card.sv", "sim/tb_disk2_ddr_bridge_shared.sv"]
    run([vivado_tool("xvlog"), "--sv", *(hdl / src for src in sources)], out)
    for name, success in (("tb_pc_transporter_card", "PC TRANSPORTER CARD PASS"),
                          ("tb_disk2_ddr_bridge_shared", "DISK II AND TRANSPORTER DDR PASS")):
        snapshot = f"{name}_snap"
        log = f"{name}.log"
        run([vivado_tool("xelab"), name, "-s", snapshot], out)
        run([vivado_tool("xsim"), snapshot, "--runall", "--log", log], out)
        if success not in (out / log).read_text(encoding="utf-8", errors="replace"):
            raise RuntimeError(f"{name} did not pass. See {out / log}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bios", type=Path)
    parser.add_argument("--routing", type=Path)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--host-only", action="store_true")
    modes.add_argument("--rtl-only", action="store_true")
    args = parser.parse_args()
    if (args.bios is None) != (args.routing is None):
        parser.error("Supply --bios and --routing together")
    for path, expected in ((args.bios, BIOS_SHA), (args.routing, ROUTING_SHA)):
        if path is not None and hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            parser.error(f"File hash does not match the 2.0.5 fixture: {path}")
    build = ROOT / "build"
    build.mkdir(exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="pc_transporter_tests_", dir=build))
    print(out, flush=True)
    if not args.rtl_only:
        host_tests(out, args.bios.resolve() if args.bios else None,
                   args.routing.resolve() if args.routing else None)
    if not args.host_only:
        rtl_tests(out)


if __name__ == "__main__":
    main()
