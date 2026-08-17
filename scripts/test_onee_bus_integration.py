#!/usr/bin/env python3
"""Build and run the joined ONE//e synthetic-motherboard bus regression."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "onee_joined_bus_sim"
TOP = ROOT / "hdl" / "apple" / "apple_top.sv"
CORE = ROOT / "hdl" / "apple" / "vtw_core_top.sv"
BOOT_CARD = ROOT / "hdl" / "apple" / "boot_menu_card.sv"
COLD_SCAN = ROOT / "hdl" / "apple" / "onee_cold_slot_scan.sv"
SOURCES_LIST = ROOT / "hdl" / "hdl_sources.txt"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


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


def static_checks() -> None:
    top = TOP.read_text(encoding="utf-8")
    core = CORE.read_text(encoding="utf-8")
    boot_card = BOOT_CARD.read_text(encoding="utf-8")
    cold_scan = COLD_SCAN.read_text(encoding="utf-8")
    sources = SOURCES_LIST.read_text(encoding="utf-8")

    for source in (
        "apple/onee_motherboard_io.sv",
        "apple/onee_cold_slot_scan.sv",
    ):
        require(
            sources.count(source) == 1,
            f"{source} must appear once in hdl_sources.txt",
        )

    require(
        "onee_motherboard_io onee_motherboard_io_i" in top and
        ".enabled                 (onee_enable_effective)" in top and
        ".softswitch_ab_read      (onee_softswitch_ab_read)" in top and
        ".ab_read(onee_softswitch_ab_read)" in top,
        "ONE//e motherboard I/O must be effective-only and feed the manager",
    )
    require(
        ".video_vblank            (onee_video_vblank)" in top and
        "assign onee_video_vblank = (line_in_frame >= 9'd192);" in top and
        "wire       onee_speaker;" in top,
        "motherboard I/O must expose its speaker and use native VBL",
    )
    require(
        "onee_input_bridge onee_input_bridge_i" in top and
        ".keyboard_event_valid   (onee_usb_keyboard_event_valid)" in top and
        ".keyboard_event_code    (onee_usb_keyboard_event_code)" in top and
        ".keyboard_any_down      (onee_usb_keyboard_any_down)" in top and
        ".keyboard_modifiers     (onee_usb_keyboard_modifiers)" in top and
        ".pushbuttons             (onee_usb_pushbuttons)" in top and
        ".paddle_values          (onee_usb_paddle_values)" in top,
        "motherboard inputs must come from the effective-only USB bridge",
    )

    require(
        ".NUM_CLIENTS(13)" in top and
        ".FAST_DATA_CLIENT(2)" in top and
        ".FAST_ADDR_CLIENT(11)" in top and
        ".client_writes({\n            onee_motherboard_ab_write,\n"
        "            vtw_ab_write," in top,
        "motherboard client must append at index 12 without moving old clients",
    )
    require(
        ".inh_allowed(machine_inh_allowed || onee_enable_effective)" in top and
        "assign ab_write = physical_bus_isolate ? '0 : ab_write_arb;" in top,
        "ONE//e must allow internal INH while preserving physical masking",
    )
    require(
        ".vtw_enabled(vtw_enable_eff || onee_enable_effective)" in top,
        "ONE//e must explicitly block the AppliCard/AD8088 bus master",
    )

    require(
        "onee_cold_slot_scan onee_cold_slot_scan_i" in top and
        ".manual_enable_request(onee_request_q)" in top and
        ".boot_target_disk2(configured_boot_target_disk2)" in top and
        ".session_boot_target_disk2(onee_boot_target_disk2)" in top and
        ".configured_boot_target_disk2(configured_boot_target_disk2)" in top and
        "if (manual_enable_request && !request_q)\n"
        "                session_boot_target_disk2 <= boot_target_disk2;" in cold_scan and
        "assign configured_boot_target_disk2 =\n"
        "        handoff_mode_q == SLOT7_HANDOFF_DISK2;" in boot_card and
        "assign boot_target_disk2 = handoff_disk2;" in boot_card and
        "else if (!ab_read.res)\n"
        "                slot7_hidden <= session_boot_target_disk2;" in cold_scan and
        ".ab_read(gate_ab(physical_ab_read, !onee_enable_effective))" in top,
        "ONE//e must use and re-arm its target without changing host fallback",
    )
    require(
        "wire onee_smartport_boot_owner =\n"
        "        onee_enable_effective && !onee_boot_target_disk2;" in top and
        "card_supersprite_enable && !onee_smartport_boot_owner &&\n"
        "            onee_slot7_cards_visible))" in top and
        "(!card_supersprite_enable || onee_smartport_boot_owner) &&\n"
        "        (onee_enable_effective || smartport_active)" in top,
        "SmartPort-selected ONE//e must own slot 7 over saved SuperSprite",
    )
    require(
        "wire disk2_bus_visible =\n        onee_enable_effective ||" in top and
        ".ab_read(gate_ab(ab_read, disk2_bus_visible))" in top and
        "assign vtw_disk2_active = !onee_enable_effective" in top,
        "ONE//e must force slot 6 visible and disable private Disk II",
    )
    require(
        "(onee_enable_effective || smartport_active)" in top and
        "onee_slot7_cards_visible" in top and
        "!onee_enable_effective && vtw_smartport_visible" in top and
        "!onee_enable_effective && vtw_disk2_boot_scan_q" in top and
        ".sp_boot_suppress(vtw_sp_boot_suppress)" in top,
        "ONE//e SmartPort must use selected slot visibility on the synthetic bus",
    )

    require(
        "input  logic                    virtual_motherboard" in core and
        "wire xl_c01x_rd = !virtual_motherboard" in core and
        "wire xl_btn_rd = !virtual_motherboard" in core,
        "standalone status/buttons must bypass vTW private read shortcuts",
    )
    require(
        "wire scanner_bus_read = cycle_rw_q" in core and
        "full_floating_read || virtual_motherboard" in core and
        "onee_bus_data_claimed_q <= data_drive_in;" in core and
        "!onee_bus_data_claimed_q" in core and
        "eng_resp_rdata[7], shadow_a_rdata[6:0]" in core,
        "standalone reads must select claimed data or scanner fallback/low bits",
    )
    require(
        ".virtual_motherboard(onee_enable_effective)" in top,
        "apple_top must identify its virtual motherboard to vTW",
    )
    require(
        "onee_warm_reset_ctrl #(" in top and
        ") onee_warm_reset_ctrl_i (" in top and
        ".virtual_res_n          (onee_virtual_res_n)" in top and
        ".res_n_in         (onee_virtual_res_n)" in top,
        "Ctrl-Alt-Delete must reach only the virtual motherboard RESET path",
    )
    require(
        ".speed_mode(vtw_ctrl_q[3:2])" in top and
        ".pace_divider(vtw_ctrl_q[31:16])" in top and
        top.count("vtw_ctrl_q                      <= 32'h0000_0000;") == 1 and
        top.count("vtw_ctrl_q <= globals::apply_wstrb(") == 1 and
        top.count("vtw_ctrl_q <=") == 1,
        "VTW_CTRL speed must remain an AXI/global-reset register, outside "
        "the virtual Apple reset path",
    )


def main() -> int:
    static_checks()

    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    OUT_DIR.mkdir(parents=True)
    shutil.copy2(ROOT / "hdl" / "apple" / "disk2_slot6.mem", OUT_DIR)

    sources = [
        "hdl/globals.sv",
        "hdl/reset_sync.sv",
        "hdl/apple/soft_switch_manager.sv",
        "hdl/apple/apple_virtual_bus.sv",
        "hdl/apple/onee_input_bridge.sv",
        "hdl/apple/onee_warm_reset_ctrl.sv",
        "hdl/apple/onee_motherboard_io.sv",
        "hdl/apple/onee_cold_slot_scan.sv",
        "hdl/apple/apple_bus_write_arbiter.sv",
        "hdl/apple/vtw_shadow.sv",
        "hdl/apple/vtw_bus_engine.sv",
        "hdl/apple/w65c02_core.sv",
        "hdl/apple/vtw_core_top.sv",
        "hdl/apple/disk2_card.sv",
        "hdl/sim/tb_onee_joined_bus.sv",
    ]
    run(
        [vivado_tool("xvlog"), "--sv", *[str(ROOT / path) for path in sources]],
        "xvlog.log",
    )
    run(
        [
            vivado_tool("xelab"),
            "tb_onee_joined_bus",
            "-s",
            "tb_onee_joined_bus_snap",
            "--timescale",
            "1ns/1ps",
            "-L",
            "unisims_ver",
        ],
        "xelab.log",
    )
    output = run(
        [vivado_tool("xsim"), "tb_onee_joined_bus_snap", "--runall"],
        "xsim.log",
    )
    require(
        "ONEE JOINED BUS PASS" in output,
        "joined simulation did not report PASS",
    )
    print("ONE//e joined bus integration passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
