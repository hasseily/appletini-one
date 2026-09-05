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
    "hdl/apple/apple_bootstrap_guard.sv",
    "hdl/apple/apple_slot7_devsel_guard.sv",
    "hdl/apple/boot_menu_card.sv",
    "hdl/sim/tb_apple_bus_addr_enable.sv",
    "hdl/sim/tb_apple_bus_isolation.sv",
    "hdl/sim/tb_apple_bus_client_policy_gate.sv",
    "hdl/sim/tb_apple_machine_safety_policy.sv",
    "hdl/sim/tb_apple_bootstrap_guard.sv",
    "hdl/sim/tb_apple_slot7_devsel_guard.sv",
    "hdl/sim/tb_iigs_bootstrap_path.sv",
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
    ("tb_apple_bootstrap_guard", "APPLE BOOTSTRAP GUARD PASS"),
    ("tb_apple_slot7_devsel_guard", "APPLE SLOT7 DEVSEL GUARD PASS"),
    ("tb_iigs_bootstrap_path", "IIGS BOOTSTRAP PATH PASS"),
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
    top = (ROOT / "hdl/apple/apple_top.sv").read_text(encoding="utf-8")
    wrapper = (ROOT / "hdl/apple/apple_bus_wrapper.sv").read_text(
        encoding="utf-8"
    )
    arbiter = (ROOT / "hdl/apple/apple_bus_write_arbiter.sv").read_text(
        encoding="utf-8"
    )
    client_gate = (
        ROOT / "hdl/apple/apple_bus_client_policy_gate.sv"
    ).read_text(encoding="utf-8")
    policy = (ROOT / "hdl/apple/apple_machine_safety_policy.sv").read_text(
        encoding="utf-8"
    )
    capture = (ROOT / "hdl/apple/apple_cycle_capture.sv").read_text(
        encoding="utf-8"
    )
    boot = (ROOT / "hdl/apple/boot_menu_card.sv").read_text(encoding="utf-8")
    bootstrap = (ROOT / "hdl/apple/apple_bootstrap_guard.sv").read_text(
        encoding="utf-8"
    )
    slot7_guard = (
        ROOT / "hdl/apple/apple_slot7_devsel_guard.sv"
    ).read_text(encoding="utf-8")
    smartport = (ROOT / "hdl/apple/smartport_card.sv").read_text(
        encoding="utf-8"
    )
    psram = (ROOT / "hdl/apple/psram_simple.sv").read_text(encoding="utf-8")
    constraints = (ROOT / "hdl/constraints/appletini_yarz.xdc").read_text(
        encoding="utf-8"
    )
    rom = (ROOT / "software/boot_menu_slot7.a65").read_text(encoding="utf-8")

    require(
        "client_writes[i].assert_dma ||" in arbiter
        and "client_writes[i].wr_addr_rw_en ||" in arbiter
        and "client_writes[i].wr_dma_data_en" in arbiter
        and "gated_writes[i].wr_data_en     = 1'b0;" in arbiter,
        "arbiter must drop every field of an unsafe ownership tuple",
    )
    require(
        "ab_write.wr_addr_rw_en && inh_allowed" in wrapper
        and "physical_bus_master_q ?" in wrapper
        and "physical_slave_read_q <= ab_read_r.cycle_valid && ab_read_r.rw;"
        in wrapper
        and "physical_slave_select_ok_q <= physical_slave_select_ok;"
        in wrapper
        and "physical_irq_allowed &&" in wrapper
        and "gs_m2_qualify ? ~m2sel_clean" in wrapper
        and "assign apple_rdy_pin = 1'bz;" in wrapper
        and "assign apple_nmi_pin = 1'bz;" in wrapper,
        "wrapper must enforce the final GS pad policy",
    )
    require(
        "apple_machine_safety_policy apple_machine_safety_policy_i" in top
        and "physical_slot_allowed_mask" in top
        and "apple_bootstrap_guard apple_bootstrap_guard_i" in top
        and "apple_slot7_devsel_guard apple_slot7_devsel_guard_i" in top
        and "boot_menu_physical_visible" in top
        and "wire physical_slave_select_ok = machine_identity_legacy ||"
        in top
        and "wire physical_irq_allowed = machine_identity_legacy;" in top
        and ".ab_write         (virtual_ab_write_arb)" in top
        and "apple_virtual_bus_write_arbiter_i" in top
        and ".inh_allowed(1'b1)" in top
        and ".inh_allowed(machine_inh_allowed_wrapper_q && machine_inh_allowed)"
        in top
        and ".m2sel_active_high(machine_m2sel_active_high_safe)" in top,
        "top-level GS identity, bootstrap, and slot gates must stay intact",
    )
    require(
        ".ab_read(boot_menu_ab_read)" in top
        and "boot_menu_ab_read.data_en = 1'b0;" in top
        and "signature_read_match ||\n                    low_c700_classifier"
        not in bootstrap
        and "assign boot_menu_physical_visible = accepted_read_q &&"
        in bootstrap
        and "wire classified_cycle_allowed = selected_cycle &&" in bootstrap
        and "configured && apple_bus_visible && ab_read.res" in smartport,
        "registered bootstrap permission must stay out of card response timing",
    )
    require(
        top.count("apple_bus_write_arbiter #(") == 2
        and ".ab_write(virtual_ab_write_arb)" in top
        and "apple_bus_private_client_gate" not in top
        and ".client_writes({\n            onee_motherboard_ab_write," in top
        and ".client_writes(policy_gated_client_writes)" in top
        and ".ab_write(ab_write_arb)" in top
        and ".irq_assert_in(virtual_ab_write_arb.assert_irq)" in top
        and ".data_drive_in(virtual_ab_write_arb.wr_data_en)" in top,
        "private and physical write paths must stay split",
    )
    require(
        "if (ab_read.addr_en || ab_read.data_en) begin" in psram
        and "ab_write.assert_inh <= 1'b0;" in psram
        and "ab_write.wr_data_en <= 1'b0;" in psram,
        "PSRAM must release held bus output before private-gate admission",
    )
    require(
        "physical_slot_allowed_mask = 8'h80;" in policy
        and "bootstrap_identity_legacy &&" in policy
        and "machine_identity_iigs =\n            bootstrap_identity_iigs &&"
        in policy
        and "machine_inh_allowed = machine_identity_legacy;" in policy
        and "bootstrap_identity_iigs;" in policy
        and "effective_m2sel_active_high = 1'b0;" in policy,
        "machine policy must directly gate GS slots, ownership, and M2SEL",
    )
    require(
        "client_enable[i] ? client_writes[i] : '0" in client_gate
        and "apple_bus_client_policy_gate #(" in top
        and ") apple_bus_client_policy_gate_i (" in top,
        "disabled cards must release all write and interrupt outputs",
    )
    require(
        "16'hC707" in bootstrap
        and "16'hC705" in bootstrap
        and "16'hC703" in bootstrap
        and "16'hC701" in bootstrap
        and "low_c700_classifier" in bootstrap
        and "!physical_ab_read.m2sel" in bootstrap
        and "accepted_read_q" in bootstrap
        and "onee_enable_effective" in bootstrap
        and "high_c700" not in bootstrap,
        "bootstrap must be exact, low-only, response-held, and ONEe-safe",
    )
    require(
        "devsel_n_early" in wrapper
        and "devsel_n_clean" in wrapper
        and "minimal_io_only" in slot7_guard
        and "if (devsel_required && ab_read_in.m2sel)" in slot7_guard
        and "supersprite_write_out.assert_irq = 1'b0;" in slot7_guard
        and "physical_ab_read.devsel_n" in slot7_guard
        and "ab_read.addr_en || ab_read.data_en" in smartport
        and "ab_read.data_en || ab_read.addr_en" in boot,
        "slot7 C0F state, stale data, and GS IRQ must use live DEVSEL",
    )
    require(
        "sss.io_select[\n"
        "                                 slot_resolved ? resolved_slot : 3'h7]"
        in boot,
        "boot scratch reads and writes must require this card's C8 claim",
    )
    m2sel_anchor = constraints.index(
        "set_property PACKAGE_PIN AA17 [get_ports a2fpga_m2sel]"
    )
    m2sel_window = constraints[m2sel_anchor : m2sel_anchor + 500]
    require(
        "PULLTYPE PULLUP" in m2sel_window
        and "cannot bias" in m2sel_window,
        "active-low /M2SEL must fail closed without claiming bus-side bias",
    )
    require(
        ".fake_shr_allowed(machine_identity_legacy || onee_enable_effective)"
        in top
        and "fake_shr_allowed &&" in capture
        and "((cap_addr != 16'hC029) || fake_shr_allowed)" in capture
        and "shr_capture_active_next = !fake_shr_allowed ? 1'b0" in capture
        and "else if (!fake_shr_allowed)" in capture,
        "physical UNKNOWN/IIgs C029 writes must not enter fake-SHR",
    )
    require(
        "iigs_external_slot_mask_valid_q" in boot
        and "iigs_slot_mask_escape_q" in boot
        and "machine_id_fault_q" in boot
        and "BM_REG_C8_PATCH:      as_client.rdata" in boot,
        "boot-menu PL must lock and expose the direct IIgs policy facts",
    )
    require(
        "lda #CMD_MID_IIGS\n          jsr bm_cmd\n"
        "          lda #CMD_IIGS_SLOT_MASK\n          jsr bm_cmd\n"
        "          lda IIGS_SLOT_REG" in rom,
        "boot ROM must report ID4 then escaped raw $C02D",
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
