#!/usr/bin/env python3
"""Build and run the virtual TransWarp simulation benches.

Two XSim benches cover the phase-1 vTW island (README_VIRTUAL_TRANSWARP.md):

- tb_vtw_engine_unit: vtw_bus_engine alone against synthesized wrapper
  strobes -- /DMA sequencing, posted/sync ordering (flush-before-video-window,
  I/O overtake), queue backpressure and drops, session release.
- tb_vtw_system: the real W65C02 core executing a program from vtw_shadow,
  mastering the pin-level apple_bus_wrapper against a motherboard model --
  posted write-through order and integrity, sync cycle data, early-PHI1
  address discipline, PHI1-only /DMA transitions, $C074 and the cycle-locked
  1 MHz pace.

Requires the Xilinx simulation tools (xvlog/xelab/xsim) on PATH.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "vtw_sim"

SOURCES = [
    "hdl/globals.sv",
    "hdl/cdc_bus_sampled.sv",
    "hdl/reset_sync.sv",
    "hdl/apple/soft_switch_manager.sv",
    "hdl/apple/apple_bus_wrapper.sv",
    "hdl/apple/apple_bus_write_arbiter.sv",
    "hdl/apple/vtw_shadow.sv",
    "hdl/apple/vtw_bus_engine.sv",
    "hdl/apple/w65c02_core.sv",
    "hdl/apple/vtw_core_top.sv",
    "hdl/apple/smartport_card.sv",
    "hdl/sim/tb_vtw_engine_unit.sv",
    "hdl/sim/tb_vtw_system.sv",
    "hdl/sim/tb_smartport_reset.sv",
    "hdl/sim/tb_smartport_shortcut.sv",
    "hdl/sim/tb_vtw_slowdown.sv",
    "hdl/sim/tb_vtw_video.sv",
]

# smartport_card $readmemh resolves against the simulation cwd.
MEM_FILES = [
    "hdl/apple/smartport_a2retronet_style_c700.mem",
    "hdl/apple/smartport_a2retronet_style_c800.mem",
]

BENCHES = [
    ("tb_vtw_engine_unit", "VTW ENGINE UNIT PASS"),
    ("tb_vtw_system", "VTW SYSTEM PASS"),
    ("tb_smartport_reset", "SP RESET PASS"),
    ("tb_smartport_shortcut", "SP SHORTCUT PASS"),
    ("tb_vtw_slowdown", "VTW SLOWDOWN PASS"),
    ("tb_vtw_video", "VTW VIDEO PASS"),
]


def vivado_tool(name: str) -> str:
    bat = shutil.which(f"{name}.bat")
    if bat:
        return bat
    tool = shutil.which(name)
    if tool:
        return tool
    raise FileNotFoundError(f"unable to locate Vivado tool {name}")


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


def read(rel_path: str) -> str:
    return (ROOT / rel_path).read_text(encoding="utf-8")


def static_checks() -> None:
    top = read("hdl/apple/apple_top.sv")
    sources = read("hdl/hdl_sources.txt")
    regs = read("ps_sources/frontend/card_control_regs.h")
    service = read("ps_sources/frontend/vtw_service.c")
    uart = read("ps_sources/frontend/uart_control.c")
    menu_c = read("ps_sources/frontend/config_menu.c")
    menu_h = read("ps_sources/frontend/config_menu.h")
    main_c = read("ps_sources/frontend/main.c")

    # PL integration
    require("vtw_core_top vtw_core_top_i" in top and
            "apple_bus_write_arbiter #(.NUM_CLIENTS(11))" in top and
            "vtw_ab_write," in top,
            "apple_top must instantiate the vTW as the 11th arbiter client")
    require("vtw_machine_ok_q  <= 1'b1;" in top and
            "machine_mode_q == 2'd2 || machine_mode_q == 2'd1" in top and
            "vtw_ctrl_q[0] && vtw_machine_ok_q" in top,
            "vTW enable must latch the machine verdict so sessions survive CTRL-RESET")

    # SmartPort short-circuit: vtw_core_top fast port wired to the card.
    core_top = read("hdl/apple/vtw_core_top.sv")
    card = read("hdl/apple/smartport_card.sv")
    boot_card = read("hdl/apple/boot_menu_card.sv")
    require("vtw_valid," in card and "vtw_resp_valid" in card and
            "data_write_ev" in card and "vtw_data_write" in card and
            "vtw_pop_write" in card,
            "smartport_card must expose the vTW short-circuit port sharing bus-path state")
    require("sp_active," in core_top and
            "X_SP_ISSUE" in core_top and "wire sp_hit" in core_top and
            "cycle_sp_iosel7_q" in core_top,
            "vtw_core_top must classify and short-circuit SmartPort accesses")
    require("wire         vtw_sp_active = vtw_smartport_visible;" in top and
            ".sp_active(vtw_sp_active)" in top and
            ".vtw_valid(vtw_sp_req_valid)" in top,
            "apple_top must gate the short-circuit on SmartPort owning slot 7 and wire the port")
    require(".boot_target_disk2(boot_target_disk2)" in top and
            "vtw_disk2_boot_scan_q <= boot_target_disk2;" in top and
            "vtw_slot6_boot_probe" in top and
            "!vtw_disk2_boot_scan_q" in top and
            ".sp_boot_suppress(vtw_disk2_boot_scan_q)" in top,
            "vTW Disk II cold boots must hide slot 7 only until the accelerated scan probes slot 6")
    require("assign boot_target_disk2 = handoff_disk2;" in boot_card,
            "boot_menu_card must export the resolved Disk II handoff target to vTW")
    require("sp_boot_suppress_hit" in core_top and
            "core_data_in_q <= 8'hFF;" in core_top,
            "vTW must return deterministic no-card data while slot 7 is hidden")

    # Per-region slowdown (TW DIP block 2).
    require("slow_region_en" in core_top and "slow_duration" in core_top and
            "slow_cnt_q" in core_top and "sd_hit" in core_top and
            "slow_active" in core_top and "sd_iosel" in core_top and
            "sd_floating_io" in core_top and "full_floating_read" in core_top,
            "vtw_core_top must implement the per-region slowdown one-shot "
            "including floating motherboard I/O and the $Cn00 I/O-select "
            "space (Mockingboard 6522)")
    require("(cycle_addr_q[7:4] >= 4'h3)" in core_top and
            "(cycle_addr_q[7:4] <= 4'h5)" in core_top and
            "if (full_floating_read)" in core_top,
            "all reads in $C030-$C05F must return the synthesized scanner byte")
    require("disk2_timing_active" in core_top and "sd_disk2" in core_top and
            "sd_disk2 || disk2_timing_active" in core_top and
            ".disk2_timing_active(disk2_sound_spinning)" in top,
            "active Disk II operation must unconditionally hold vTW at 1 MHz")
    require("CARD_CTRL_REG_VTW_SLOWDOWN" in top and
            ".slow_region_en(vtw_slowdown_q[9:0])" in top and
            ".slow_duration(vtw_slowdown_q[31:16])" in top,
            "apple_top must decode the slowdown register and wire it to the core")
    service_c = read("ps_sources/frontend/vtw_service.c")
    require("vtw_service_set_slowdown" in service_c and
            "CARD_CTRL_VTW_SLOWDOWN_REG" in service_c,
            "vtw_service must push the slowdown config to the PL")

    # Takeover machine reset (RES#-at-takeover)
    wrapper = read("hdl/apple/apple_bus_wrapper.sv")
    service = read("ps_sources/frontend/vtw_service.c")
    require("assign apple_res_pin = ab_write.assert_res ? 1'b0 : 1'bz;" in wrapper,
            "wrapper must drive RES# open-collector from assert_res")
    require(".assert_apple_res(vtw_ctrl_q[4])" in top,
            "apple_top must wire VTW_CTRL bit4 to the takeover reset")
    require("VTW_ST_RES_HOLD" in service and
            "vtw_ctrl_value(1U, 0U, 1U)" in service and
            "VTW_RES_HOLD_MS" in service,
            "vtw_service must sequence the takeover machine reset before the copy")
    for src in ("apple/w65c02_core.sv", "apple/vtw_shadow.sv",
                "apple/vtw_bus_engine.sv", "apple/vtw_core_top.sv"):
        require(src in sources, f"hdl_sources.txt must list {src}")

    # Register window agreement (PL 0x70-0x7A <-> PS defines)
    require("CARD_CTRL_REG_VTW_CTRL        = 8'h70" in top and
            "CARD_CTRL_VTW_CTRL_REG             CARD_CTRL_REG_ADDR(0x70U)" in regs and
            "CARD_CTRL_VTW_CNT_INVALID_REG      CARD_CTRL_REG_ADDR(0x7AU)" in regs,
            "vTW register window must agree between apple_top and card_control_regs.h")

    # PS service + wiring
    require("boot_menu_service_machine_mode() != CARD_MACHINE_MODE_IIE" in service,
            "vtw_service must wait for the //e machine report")
    require("boot_menu_service_slot7_handed_off()" in service,
            "vtw_service must wait for the boot menu handoff before taking the "
            "bus, so the boot menu never re-runs on the vTW core")
    require("VTW_ST_LOAD_ROM" in service and
            "apple2e_cpu_rom[i]" in service and
            "#define VTW_SHADOW_ROM_PHYS    0x20000UL" in service,
            "vtw_service must load the embedded Enhanced //e ROM into the shadow ROM region")

    # Accelerated personality is always an Enhanced //e (no II+ fork).
    require("translate_state_from_sss(vsss)" in core_top and
            "iiplus_translate_state" not in core_top and
            "video_vbl" in core_top,
            "accelerated core must always use the //e translate model")
    # Synthesized //e status reads ($C011-$C01F incl. $C019 VBL).
    require("xl_c01x_rd" in core_top and "X_STATUS_DONE" in core_top and
            "status_byte" in core_top and "RDVBLBAR" in core_top,
            "vtw_core_top must synthesize the //e $C01x status reads")
    require("status_vbl_data_phase_q" in core_top and
            "status_vbl_sampled_q" in core_top and
            "status_vbl_data_phase_q && ab_read.data_en" in core_top and
            "vtw_video_position_rewind" in core_top and
            "video_cycle, 2'd1" in core_top and
            "core_data_in_q       <= {~status_video_vbl, 7'b0};" in core_top,
            "exact-1 MHz $C019 reads must use the one-cycle-lagged data phase")
    engine = read("hdl/apple/vtw_bus_engine.sv")
    require("wire vtw_video_vbl = (line_in_frame >= 9'd192);" in top and
            "function automatic logic [15:0] vtw_video_position_rewind" in engine,
            "vTW $C019 must derive its lag from the native video position")
    # Accelerated floating bus: scanner address from native timing + shadow
    # main RAM. The expansion-slot data pins do not expose PHI1 video fetches.
    require("vtw_scanner_address" in engine and
            "floating_scan_addr_q" in core_top and
            "floating_scan_pos" in core_top and
            "video_cycle, 2'd2" in core_top and
            "floating_scan_issue" in core_top and
            "full_floating_read" in core_top and
            "(cycle_addr_q[7:4] >= 4'h3)" in core_top and
            "(cycle_addr_q[7:4] <= 4'h5)" in core_top and
            "core_data_in_q <= shadow_a_rdata;" in core_top and
            ".video_line(line_in_frame)" in top and
            ".video_cycle(cycle_in_line)" in top and
            "vid_phi1_data" not in wrapper,
            "vTW $C030-$C05F reads must use the two-cycle-lagged scanner "
            "address and main shadow byte")
    # Super Hi-Res: aux posted-write window extended to $9FFF.
    require("vtw_is_video_window(cycle_addr_q, xl_is_aux, post_main_wide)"
            in core_top and
            "input logic is_aux" in engine and
            "input logic wide_main" in engine,
            "vTW must extend the aux posted-write window for Super Hi-Res "
            "and support the widened main window for SHR interlace")
    require("vtw_service_init(UART0_BASE);" in main_c and
            "vtw_service_poll();" in main_c and
            "menu_platform.set_vtw_config = control_set_vtw_config;" in main_c,
            "main.c must init, poll, and bind the vTW service")
    require('str_ieq(argv[0], "vtw")' in uart and
            "config_menu_set_vtw_enabled(g_sdd_config_menu, enable);" in uart,
            "uart vtw command must route enable through the config menu")
    require('"vtw.enabled=%s\\n"' in menu_c and
            'strcmp(key, "vtw.enabled") == 0' in menu_c and
            "menu->platform.set_vtw_config(menu->platform.ctx," in menu_c,
            "config menu must persist and apply the vTW settings")
    require("uint8_t vtw_enabled;" in menu_h and
            "void config_menu_set_vtw_enabled(config_menu_t *menu, uint8_t enable);" in menu_h,
            "config_menu.h must expose the vTW fields and setter")

    # Config-menu tab
    internal_h = read("ps_sources/frontend/config_menu_internal.h")
    tabs_c = read("ps_sources/frontend/config_menu_device_tabs.c")
    help_c = read("ps_sources/frontend/config_menu_help.c")
    require("CONFIG_TAB_TRANSWARP," in internal_h and
            '"TransWarp",' in menu_c and
            "case CONFIG_TAB_TRANSWARP:" in menu_c and
            "config_menu_draw_transwarp(fb, menu, x, y, w);" in menu_c,
            "config menu must have the TransWarp tab wired (enum, label, items, draw)")
    require("k_vtw_speed_presets" in menu_c and
            '"3.6 MHz (TransWarp)"' in menu_c and
            '"1 MHz default"' in menu_c and
            '"26 MHz"' in menu_c and
            '"MAX Speed"' in menu_c,
            "TransWarp tab must offer the speed presets")
    require("menu->vtw_speed_mode == CARD_CTRL_VTW_SPEED_FULL" in menu_c and
            "return VTW_SPEED_PRESET_COUNT - 1U;" in menu_c and
            "menu->vtw_speed_mode == CARD_CTRL_VTW_SPEED_1MHZ" in menu_c and
            "return 0U;" in menu_c,
            "TransWarp full speed and 1 MHz modes must resolve to their correct endpoint labels")
    ladder_index = service[service.index(
        "static int vtw_eff_ladder_index(void)"):
        service.index("void vtw_service_set_slug_enabled", service.index(
            "static int vtw_eff_ladder_index(void)"))]
    require(service.index('"26 MHz"') < service.index('"MAX Speed"') and
            "fastest_divided" in ladder_index and
            "div >= k_vtw_ladder[i].divider" in ladder_index and
            "if (div >= 51U) return 1;" not in ladder_index,
            "runtime speed stepping must derive every divided rung from the ladder table")
    require("void config_menu_draw_transwarp" in tabs_c and
            '"TransWarp on: 128K + 8MB RamWorks, accelerated"' in tabs_c,
            "RAM tab must surface the accelerated-RamWorks status")
    require("TAB_WITH_OVERRIDES(CONFIG_TAB_TRANSWARP, transwarp, transwarp_overrides)," in help_c,
            "TransWarp tab must have help text")


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for mem in MEM_FILES:
        shutil.copyfile(ROOT / mem, OUT_DIR / Path(mem).name)
    try:
        static_checks()
        print("PASS static integration checks")
        run(
            [vivado_tool("xvlog"), "--sv"]
            + [str(ROOT / src) for src in SOURCES],
            OUT_DIR / "xvlog.log",
        )
        for bench, _pass in BENCHES:
            run(
                [vivado_tool("xelab"), bench, "-s", f"{bench}_snap",
                 "--timescale", "1ns/1ps"],
                OUT_DIR / f"xelab_{bench}.log",
            )
        for bench, pass_line in BENCHES:
            output = run(
                [vivado_tool("xsim"), f"{bench}_snap", "--runall"],
                OUT_DIR / f"xsim_{bench}.log",
            )
            fail_lines = [
                line for line in output.splitlines()
                if "FAIL" in line and "FAILED" not in line
            ]
            for line in fail_lines:
                print(line)
            if pass_line not in output:
                raise RuntimeError(f"{bench} did not report '{pass_line}'")
            print(f"PASS {bench}")
        print(f"{len(BENCHES)} vTW benches passed")
        return 0
    except (OSError, RuntimeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
