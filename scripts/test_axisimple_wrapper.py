#!/usr/bin/env python3
"""Build and run focused AxiSimple wrapper regressions."""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "axisimple_wrapper_sim"
WRAPPER = ROOT / "hdl" / "axisimple" / "axisimple_wrapper.sv"
MMIO_H = ROOT / "ps_sources" / "lib" / "axisimple_mmio.h"
FRONTEND_MAIN = ROOT / "ps_sources" / "frontend" / "main.c"
CORE1_MAIN = ROOT / "ps_sources" / "frontend_core1" / "main.c"


def vivado_tool(name: str) -> str:
    tool = shutil.which(f"{name}.bat") or shutil.which(name)
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
            f"{Path(command[0]).name} failed with {completed.returncode}"
        )
    return completed.stdout


def require(pattern: str, source: str, message: str) -> None:
    if not re.search(pattern, source, re.MULTILINE | re.DOTALL):
        raise AssertionError(message)


def check_registered_wskid() -> None:
    source = WRAPPER.read_text(encoding="utf-8")
    require(
        r"wire\s+\[36:0\]\s+wskid_payload\s*;",
        source,
        "axisimple_wrapper must retain one 37-bit W payload",
    )
    require(
        r"assign\s+S_AXI_WREADY\s*=\s*"
        r"S_AXI_ARESETN\s*&&\s*wskid_input_ready\s*;",
        source,
        "wrapper WREADY must use the skid input-ready signal and reset gate",
    )
    require(
        r"skidbuffer\s*#\s*\("
        r"(?=[^;]*\.OPT_LOWPOWER\s*\(\s*1'b1\s*\))"
        r"(?=[^;]*\.OPT_OUTREG\s*\(\s*1'b1\s*\))"
        r"(?=[^;]*\.DW\s*\(\s*37\s*\))"
        r"[^;]*\)\s*wchannel_skid_i\s*\(",
        source,
        "wrapper must instantiate a registered 37-bit low-power skid",
    )
    require(
        r"wchannel_skid_i\s*\("
        r"(?=[^;]*\.i_clk\s*\(\s*S_AXI_ACLK\s*\))"
        r"(?=[^;]*\.i_reset\s*\(\s*!S_AXI_ARESETN\s*\))"
        r"(?=[^;]*\.i_valid\s*\(\s*S_AXI_WVALID\s*\))"
        r"(?=[^;]*\.o_ready\s*\(\s*wskid_input_ready\s*\))"
        r"(?=[^;]*\.i_data\s*\(\s*\{\s*S_AXI_WLAST\s*,\s*"
        r"S_AXI_WSTRB\s*,\s*S_AXI_WDATA\s*\}\s*\))"
        r"(?=[^;]*\.o_valid\s*\(\s*wskid_valid\s*\))"
        r"(?=[^;]*\.i_ready\s*\(\s*axid_wready\s*\))"
        r"(?=[^;]*\.o_data\s*\(\s*wskid_payload\s*\))",
        source,
        "registered skid ports or W payload packing changed",
    )
    require(
        r"\.S_AXI_WVALID\s*\(\s*wskid_valid\s*\)\s*,\s*"
        r"\.S_AXI_WREADY\s*\(\s*axid_wready\s*\)\s*,\s*"
        r"\.S_AXI_WDATA\s*\(\s*wskid_payload\[31:0\]\s*\)\s*,\s*"
        r"\.S_AXI_WSTRB\s*\(\s*wskid_payload\[35:32\]\s*\)\s*,\s*"
        r"\.S_AXI_WLAST\s*\(\s*wskid_payload\[36\]\s*\)",
        source,
        "axidouble must consume valid and all payload bits from the W skid",
    )


def check_no_exclusive_mmio_contract() -> None:
    wrapper_source = WRAPPER.read_text(encoding="utf-8")
    mmio_source = MMIO_H.read_text(encoding="utf-8")
    frontend_source = FRONTEND_MAIN.read_text(encoding="utf-8")
    core1_source = CORE1_MAIN.read_text(encoding="utf-8")
    exclusive_settings = re.findall(
        r"\.OPT_EXCLUSIVE_ACCESS\s*\(\s*([^)]*?)\s*\)",
        wrapper_source,
    )
    if exclusive_settings != ["1'b0"]:
        raise AssertionError(
            "axisimple_wrapper must disable its one exclusive monitor instance"
        )
    require(
        r"axisimple_mmio_mmu_init\s*\([^)]*\)\s*\{.*?"
        r"Xil_SetTlbAttributes\s*\(\s*0x40000000U\s*,\s*"
        r"DEVICE_MEMORY\s*\)\s*;",
        mmio_source,
        "both cores must share the Device-memory AxiSimple mapping helper",
    )
    if mmio_source.count("Xil_SetTlbAttributes") != 1:
        raise AssertionError("AxiSimple MMIO helper must map exactly one section")
    if re.search(r"NORM_NONCACHE|STRONG_ORDERED", mmio_source):
        raise AssertionError("AxiSimple MMIO mapping must fail closed to Device")
    for source, first_pl_use, name in (
        (frontend_source, "apple_fb_handoff_init();", "CPU0"),
        (core1_source, "apple_cycle_egress_amp_secondary_init();", "CPU1"),
    ):
        main_source = source[source.index("int main(void)") :]
        if main_source.count("axisimple_mmio_mmu_init();") != 1:
            raise AssertionError(f"{name} must map AxiSimple MMIO exactly once")
        call = main_source.find("axisimple_mmio_mmu_init();")
        first_use = main_source.find(first_pl_use)
        if call < 0 or first_use < 0 or call > first_use:
            raise AssertionError(
                f"{name} must map GP0 as Device memory before its first PL use"
            )

    # This is deliberately broad. Any new atomic use needs review before it
    # can coexist with a PL register plane that has no exclusive monitor.
    forbidden = re.compile(
        r"(?:__atomic|__sync|__ldrex|__strex|\bldrex(?:b|h|d)?\b|"
        r"\bstrex(?:b|h|d)?\b|\bclrex\b|\bswpb?\b|"
        r"Xil_InitializeSpinLock|\bstdatomic\.h\b)",
        re.IGNORECASE,
    )
    violations: list[str] = []
    for path in sorted((ROOT / "ps_sources").rglob("*")):
        if path.suffix.lower() not in {".c", ".h", ".s"}:
            continue
        source = path.read_text(encoding="utf-8", errors="ignore")
        if forbidden.search(source):
            violations.append(str(path.relative_to(ROOT)))
    if violations:
        raise AssertionError(
            "GP0 MMIO firmware must not introduce atomic/exclusive operations: "
            + ", ".join(violations)
        )


def main() -> int:
    check_registered_wskid()
    check_no_exclusive_mmio_contract()
    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    OUT_DIR.mkdir(parents=True)

    axisimple = ROOT / "hdl" / "axisimple"
    run(
        [
            vivado_tool("xvlog"),
            "--sv",
            str(ROOT / "hdl" / "globals.sv"),
            str(axisimple / "skidbuffer.v"),
            str(axisimple / "sfifo.v"),
            str(axisimple / "addrdecode.v"),
            str(axisimple / "axi_addr.v"),
            str(axisimple / "axidouble.v"),
            str(WRAPPER),
            str(ROOT / "hdl" / "apple" / "disk2_card.sv"),
            str(ROOT / "hdl" / "sim" / "tb_axisimple_wrapper_wskid.sv"),
            str(ROOT / "hdl" / "sim" / "tb_axisimple_disk2_base.sv"),
        ],
        "xvlog.log",
    )
    shutil.copy2(
        ROOT / "hdl" / "apple" / "disk2_slot6.mem",
        OUT_DIR / "disk2_slot6.mem",
    )
    run(
        [
            vivado_tool("xelab"),
            "tb_axisimple_wrapper_wskid",
            "-s",
            "tb_axisimple_wrapper_wskid_snap",
        ],
        "xelab.log",
    )
    output = run(
        [
            vivado_tool("xsim"),
            "tb_axisimple_wrapper_wskid_snap",
            "--runall",
        ],
        "xsim.log",
    )
    if "AXISIMPLE WRAPPER WSKID PASS" not in output:
        print(output)
        raise RuntimeError("AXI write-data skid bench did not report success")
    run(
        [
            vivado_tool("xelab"),
            "tb_axisimple_disk2_base",
            "-s",
            "tb_axisimple_disk2_base_snap",
        ],
        "xelab_disk2.log",
    )
    output = run(
        [
            vivado_tool("xsim"),
            "tb_axisimple_disk2_base_snap",
            "--runall",
        ],
        "xsim_disk2.log",
    )
    if "AXISIMPLE DISK2 BASE PASS" not in output:
        print(output)
        raise RuntimeError("AXI Disk II BASE bench did not report success")
    print("AxiSimple wrapper regressions passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
