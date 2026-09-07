#!/usr/bin/env python3
"""Build and run the focused Apple IIgs PL safety regressions.

Requires the Xilinx simulation tools (xvlog/xelab/xsim) on PATH.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "iigs_bus_safety"

SOURCES = [
    "hdl/globals.sv",
    "hdl/cdc_bus_sampled.sv",
    "hdl/apple/apple_bus_write_arbiter.sv",
    "hdl/apple/apple_bus_wrapper.sv",
    "hdl/apple/apple_bus_client_policy_gate.sv",
    "hdl/apple/apple_machine_safety_policy.sv",
    "hdl/apple/apple_slot7_devsel_guard.sv",
    "hdl/apple/boot_menu_card.sv",
    "hdl/sim/tb_apple_bus_addr_enable.sv",
    "hdl/sim/tb_apple_bus_isolation.sv",
    "hdl/sim/tb_apple_bus_client_policy_gate.sv",
    "hdl/sim/tb_apple_machine_safety_policy.sv",
    "hdl/sim/tb_apple_machine_policy_latch.sv",
    "hdl/sim/tb_apple_slot7_devsel_guard.sv",
    "hdl/sim/tb_iigs_bootstrap_path.sv",
    "hdl/sim/tb_iie_bootstrap_path.sv",
    "hdl/sim/tb_boot_menu_iigs_policy.sv",
]

MEM_FILES = [
    "hdl/apple/boot_menu_slot7.mem",
    "hdl/apple/boot_menu_slot7_reloc.mem",
    "hdl/apple/boot_menu_slot7_c8.mem",
]

BENCHES = [
    ("tb_apple_bus_addr_enable", "APPLE BUS ADDR ENABLE PASS"),
    ("tb_apple_bus_isolation", "APPLE BUS ISOLATION PASS"),
    ("tb_apple_bus_client_policy_gate", "APPLE BUS CLIENT POLICY GATE PASS"),
    ("tb_apple_machine_safety_policy", "APPLE MACHINE SAFETY POLICY PASS"),
    ("tb_apple_machine_policy_latch", "APPLE MACHINE POLICY LATCH PASS"),
    ("tb_apple_slot7_devsel_guard", "APPLE SLOT7 DEVSEL GUARD PASS"),
    ("tb_iigs_bootstrap_path", "IIGS BOOTSTRAP PATH PASS"),
    ("tb_iie_bootstrap_path", "IIE BOOTSTRAP PATH PASS"),
    ("tb_boot_menu_iigs_policy", "BOOT MENU IIGS POLICY PASS"),
]


def vivado_tool(name: str) -> str:
    tool = shutil.which(f"{name}.bat") or shutil.which(name)
    if tool is None:
        raise FileNotFoundError(f"unable to locate Vivado tool {name}")
    return tool


def run(cmd: list[str], log: Path) -> str:
    completed = subprocess.run(
        cmd,
        cwd=OUT_DIR,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    log.write_text(completed.stdout, encoding="utf-8")
    if completed.returncode != 0:
        print(completed.stdout)
        raise RuntimeError(
            f"{Path(cmd[0]).name} failed with exit code {completed.returncode}"
        )
    return completed.stdout


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def static_checks() -> None:
    def source(path: str) -> str:
        return (ROOT / path).read_text(encoding="utf-8")

    top = source("hdl/apple/apple_top.sv")
    wrapper = source("hdl/apple/apple_bus_wrapper.sv")
    arbiter = source("hdl/apple/apple_bus_write_arbiter.sv")
    policy = source("hdl/apple/apple_machine_safety_policy.sv")
    guard = source("hdl/apple/apple_slot7_devsel_guard.sv")
    rom = source("software/boot_menu_slot7.a65")

    require(
        "apple_bootstrap_guard" not in top
        and "bootstrap_identity" not in policy
        and "legacy_boot_ready" not in top,
        "physical identity must come from the boot ROM, without a banner classifier",
    )
    require(
        "client_writes[i].assert_dma ||" in arbiter
        and "client_writes[i].wr_addr_rw_en ||" in arbiter
        and "client_writes[i].wr_dma_data_en" in arbiter
        and "gated_writes[i].wr_data_en     = 1'b0;" in arbiter,
        "denied ownership must discard the whole response tuple",
    )
    require(
        "physical_ownership_dependent_q <= ab_write.assert_inh ||" in wrapper
        and "(!physical_ownership_dependent_q || inh_allowed)" in wrapper
        and "physical_select_allowed_q" in wrapper
        and "assign apple_rdy_pin = 1'bz;" in wrapper
        and "assign apple_nmi_pin = 1'bz;" in wrapper,
        "physical pad gate must cover INH replacement bytes and bus-master data",
    )
    require(
        "wire physical_irq_allowed = machine_identity_legacy;" in top
        and ".inh_allowed(machine_inh_allowed_wrapper_q && machine_inh_allowed)" in top
        and ".physical_bus_isolate(physical_bus_output_isolate)" in top
        and "boot_menu_machine_id_fault || boot_menu_iigs_policy_fault;" in top
        and "machine_inh_allowed = machine_identity_legacy;" in policy,
        "physical controls must use the direct ROM policy and immediate fault isolation",
    )
    require(
        top.count("apple_bus_write_arbiter #(") == 2
        and "apple_virtual_bus_write_arbiter_i" in top
        and ".ab_write(virtual_ab_write_arb)" in top
        and ".client_writes(policy_gated_client_writes)" in top,
        "physical safety restrictions must not feed into the ONEe private bus",
    )
    require(
        "minimal_io_only" not in guard
        and "if (devsel_required && ab_read_in.m2sel)" in guard
        and "supersprite_write_out.assert_irq = 1'b0;" in guard
        and "physical_ab_read.devsel_n" in guard,
        "identified GS device I/O and stale responses require live selection",
    )
    require(
        "slot7_overlay_devsel_visible" in top
        and "physical_slot7_io_read" in top
        and "!physical_ab_read.m2sel" in top,
        "pre-ID overlay access must retain its separate selected-I/O restriction",
    )
    require(
        rom.index("jsr machine_id_report") < rom.index("jsr show_prompt")
        and "sec\n          jsr IDGS\n          bcc mid_gs" in rom
        and "lda #CMD_MID_IIGS\n          jsr bm_cmd\n"
        "          lda #CMD_IIGS_SLOT_MASK\n          jsr bm_cmd\n"
        "          lda IIGS_SLOT_REG" in rom,
        "boot ROM must identify before prompting and report GS before its raw slot mask",
    )


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for mem in MEM_FILES:
        shutil.copyfile(ROOT / mem, OUT_DIR / Path(mem).name)
    try:
        static_checks()
        print("PASS static IIgs integration checks")
        run(
            [vivado_tool("xvlog"), "--sv"]
            + [str(ROOT / source) for source in SOURCES],
            OUT_DIR / "xvlog.log",
        )
        for bench, _ in BENCHES:
            run(
                [
                    vivado_tool("xelab"),
                    bench,
                    "-s",
                    f"{bench}_snap",
                    "--timescale",
                    "1ns/1ps",
                    "-L",
                    "unisims_ver",
                ],
                OUT_DIR / f"xelab_{bench}.log",
            )
        for bench, pass_line in BENCHES:
            output = run(
                [vivado_tool("xsim"), f"{bench}_snap", "--runall"],
                OUT_DIR / f"xsim_{bench}.log",
            )
            require(pass_line in output, f"{bench} did not report {pass_line!r}")
            print(f"PASS {bench}")
        print(f"{len(BENCHES)} IIgs PL benches passed")
        return 0
    except (OSError, RuntimeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
