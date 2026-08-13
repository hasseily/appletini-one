#!/usr/bin/env python3
"""Build and run the focused virtual TransWarp simulation benches.

The suite covers the full vTW path plus its SmartPort shortcut, RamWorks
flush/hold protocol, PS-DMA abort path, video timing, II+ bus rules, Disk II
physical/WOZ paths at every vTW speed, and reset handling. It does not run the
large standalone 65C02 instruction suites.

Requires the Xilinx simulation tools (xvlog/xelab/xsim) on PATH.
"""

from __future__ import annotations

import re
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
    "hdl/apple/vtw_shadow_host_port.sv",
    "hdl/apple/vtw_bus_engine.sv",
    "hdl/apple/w65c02_core.sv",
    "hdl/apple/vtw_core_top.sv",
    "hdl/apple/apple_dma_engine.sv",
    "hdl/apple/ps_dma_command.sv",
    "hdl/apple/disk2_card.sv",
    "hdl/apple/linear_text_overlay_card.sv",
    "hdl/apple/smartport_card.sv",
    "hdl/sim/tb_vtw_engine_unit.sv",
    "hdl/sim/tb_vtw_system.sv",
    "hdl/sim/tb_smartport_reset.sv",
    "hdl/sim/tb_smartport_shortcut.sv",
    "hdl/sim/tb_linear_text_overlay.sv",
    "hdl/sim/tb_vtw_slowdown.sv",
    "hdl/sim/tb_vtw_video.sv",
    "hdl/sim/tb_vtw_drivehold.sv",
    "hdl/sim/tb_iiplus_irq_chirp.sv",
    "hdl/sim/tb_iiplus_dma_refresh.sv",
    "hdl/sim/tb_apple_dma_abort.sv",
    "hdl/sim/tb_ps_dma_command.sv",
    "hdl/sim/tb_vtw_shadow_host_port.sv",
    "hdl/sim/tb_disk2_vtw_read.sv",
    "hdl/sim/tb_disk2_physical_bus.sv",
    "hdl/sim/tb_disk2_woz_rw.sv",
    "hdl/sim/tb_vtw_disk2_speed_matrix.sv",
    "hdl/sim/tb_vtw_disk2_woz_e2e.sv",
]

# Card-ROM $readmemh calls resolve against the simulation cwd.
MEM_FILES = [
    "hdl/apple/smartport_a2retronet_style_c700.mem",
    "hdl/apple/smartport_a2retronet_style_c800.mem",
    "hdl/apple/disk2_slot6.mem",
]

BENCHES = [
    ("tb_vtw_engine_unit", "VTW ENGINE UNIT PASS"),
    ("tb_vtw_system", "VTW SYSTEM PASS"),
    ("tb_smartport_reset", "SP RESET PASS"),
    ("tb_smartport_shortcut", "SP SHORTCUT PASS"),
    ("tb_linear_text_overlay", "LINEAR TEXT OVERLAY PASS"),
    ("tb_vtw_slowdown", "VTW SLOWDOWN PASS"),
    ("tb_vtw_video", "VTW VIDEO PASS"),
    ("tb_vtw_drivehold", "VTW DRIVEHOLD PASS"),
    ("tb_iiplus_irq_chirp", "IIPLUS IRQ CHIRP PASS"),
    ("tb_iiplus_dma_refresh", "IIPLUS DMA REFRESH PASS"),
    ("tb_apple_dma_abort", "APPLE DMA ABORT PASS"),
    ("tb_ps_dma_command", "PS DMA COMMAND PASS"),
    ("tb_vtw_shadow_host_port", "VTW SHADOW HOST PASS"),
    ("tb_disk2_vtw_read", "DISK2 VTW READ PASS"),
    ("tb_disk2_physical_bus", "DISK2 PHYSICAL BUS PASS"),
    ("tb_disk2_woz_rw", "DISK2 WOZ RW PASS"),
    ("tb_vtw_disk2_speed_matrix", "VTW DISK2 SPEED MATRIX PASS"),
    ("tb_vtw_disk2_woz_e2e", "VTW DISK2 WOZ E2E PASS"),
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
    xdc = read("hdl/constraints/appletini_yarz.xdc")
    regs = read("ps_sources/frontend/card_control_regs.h")
    service = read("ps_sources/frontend/vtw_service.c")
    uart = read("ps_sources/frontend/uart_control.c")
    menu_c = read("ps_sources/frontend/config_menu.c")
    menu_h = read("ps_sources/frontend/config_menu.h")
    main_c = read("ps_sources/frontend/main.c")

    ladder_match = re.search(
        r"} k_vtw_ladder\[\] = \{(.*?)\n\};", service, re.DOTALL
    )
    require(ladder_match is not None,
            "vtw_service must keep a readable speed-ladder table")
    ladder = re.findall(
        r"\{\s*(CARD_CTRL_VTW_SPEED_[A-Z0-9_]+),\s*(\d+)U,\s*\"[^\"]+\"\s*\}",
        ladder_match.group(1) if ladder_match is not None else "",
    )
    require(
        ladder == [
            ("CARD_CTRL_VTW_SPEED_1MHZ", "0"),
            ("CARD_CTRL_VTW_SPEED_DIVIDED", "51"),
            ("CARD_CTRL_VTW_SPEED_DIVIDED", "37"),
            ("CARD_CTRL_VTW_SPEED_DIVIDED", "19"),
            ("CARD_CTRL_VTW_SPEED_DIVIDED", "10"),
            ("CARD_CTRL_VTW_SPEED_DIVIDED", "5"),
            ("CARD_CTRL_VTW_SPEED_FULL", "0"),
        ] and "#define VTW_SLUG_DIVIDER 2667U" in service,
        "the dynamic Disk II speed matrix must match every vTW ladder preset "
        "and the slug override",
    )

    # PL integration
    require("vtw_core_top vtw_core_top_i" in top and
            ".NUM_CLIENTS(12)" in top and
            ".FAST_DATA_CLIENT(2)" in top and
            "vtw_ab_write," in top,
            "apple_top must instantiate the vTW in the 12-client arbiter")
    require("logic vtw_machine_ok_q;" in top and
            "if (machine_mode_q == 2'd1)" in top and
            "else if (machine_mode_q == 2'd2)" in top and
            "vtw_machine_ok_q       <= 1'b1;" in top and
            "vtw_ctrl_q[0] && vtw_machine_ok_q" in top,
            "vTW enable must latch the machine verdict so sessions survive CTRL-RESET")
    require("vtw_host_is_iiplus_q" in top and
            ".host_is_iiplus(vtw_host_is_iiplus_q)" in top and
            "IIPLUS_PARK_ADDR = 16'h0200" in
            read("hdl/apple/vtw_bus_engine.sv"),
            "vTW must latch II/II+ host type and park that host in ordinary "
            "main RAM rather than I/O or Language Card space")
    require("(* DONT_TOUCH = \"TRUE\" *) logic machine_inh_allowed_wrapper_q;" in top and
            ".inh_allowed(machine_inh_allowed_wrapper_q)" in top and
            "machine_inh_allowed_wrapper_q   <= 1'b0;" in top and
            "machine_inh_allowed_wrapper_q <=" in top and
            "(as_common.wdata[1:0] == 2'd1) ||" in top and
            "(as_common.wdata[1:0] == 2'd2);" in top,
            "the wrapper machine interlock needs a preserved same-edge local copy")

    # SmartPort short-circuit: vtw_core_top fast port wired to the card.
    core_top = read("hdl/apple/vtw_core_top.sv")
    card = read("hdl/apple/smartport_card.sv")
    disk2_card = read("hdl/apple/disk2_card.sv")
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
    require("xl_is_overlay_post" in core_top and
            "overlay_capture_armed" in core_top and
            "xl_decoded[16] == overlay_capture_bank_aux" in core_top and
            ".overlay_capture_armed(overlay_capture_armed)" in top,
            "vTW must post writes in the armed linear-text RAM window")
    require("sync_slot7_devsel" in read("hdl/apple/vtw_bus_engine.sv") and
            "sync_addr_q[15:4] == 12'hC0F" in
            read("hdl/apple/vtw_bus_engine.sv"),
            "vTW must drain posted text bytes before slot-7 DEVSEL access")
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
    require("logic                    ssm_apply_pulse;" in core_top and
            "core_ab.addr        = cycle_addr_q;" in core_top and
            "core_ab.rw          = cycle_rw_q;" in core_top and
            "core_ab.data        = cycle_wdata_q;" in core_top and
            "core_ab.serve_en    = ssm_apply_pulse;" in core_top and
            "core_ab.data_en     = ssm_apply_pulse;" in core_top and
            "assign ssm_pulse       = core_active && (xstate_q == X_CAPTURE);"
            in core_top and
            "assign ssm_apply_pulse = core_active && (xstate_q == X_ROUTE);"
            in core_top and
            "else if (ssm_pulse && !core_rwb && core_addr == 16'hC074 &&"
            in core_top and
            "core_ab.addr        = core_addr;" not in core_top,
            "the private soft-switch manager must apply the captured cycle at "
            "X_ROUTE while $C074 keeps the raw X_CAPTURE pulse")

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
    require("wire d2_fast_hit" in core_top and
            "cycle_rw_q &&" in core_top and
            "!cycle_addr_q[0] && !d2_write_timing_active" in core_top and
            "wire sd_disk2_native = sd_disk2 && !d2_fast_hit;" in core_top,
            "vTW may bypass the Apple bus only for even Disk II reads")
    require("wire d2_cycle_tick_accept =\n"
            "        core_en && d2_active && !private_d2_q && !sd_disk2_native;"
            in core_top and
            "logic d2_cycle_tick_q;" in core_top and
            "assign d2_cycle_tick = d2_cycle_tick_q;" in core_top and
            "d2_cycle_tick_q     <= 1'b0;" in core_top and
            "if (!core_active)\n                d2_cycle_tick_q <= 1'b0;"
            in core_top and
            "d2_cycle_tick_q <= d2_cycle_tick_accept;" in core_top and
            "assign d2_cycle_tick =\n        core_en" not in core_top,
            "vTW must stage each accepted normal Disk II tick for one fabric "
            "clock without changing private or native tick selection")
    require("d2_write_timing_active" in core_top and
            "sd_disk2_native ||" in core_top and
            ".d2_write_timing_active(vtw_d2_write_timing_active)" in top and
            "disk2_timing_active" not in core_top,
            "physical Disk II accesses and Q7 write mode must force 1 MHz")
    require("logic disk2_active_vtw_q;" in top and
            "if (!rstn[1])\n            disk2_active_vtw_q <= 1'b0;" in top and
            "disk2_active_vtw_q <= disk2_active;" in top and
            "assign vtw_disk2_active = vtw_core_run_eff && vtw_bus_owned" in top and
            "card_slot6_enable && disk2_active_vtw_q" in top and
            "!vtw_ctrl_q[7];" in top and
            ".vtw_active(vtw_disk2_active)" in top and
            ".d2_active(vtw_disk2_active)" in top,
            "apple_top must stage the boot-menu Disk II gate for vTW, then enable both "
            "private-port consumers only during bus ownership and while compatibility "
            "disable is clear")
    require("wire disk_cycle_tick" in disk2_card and
            "vtw_native_cycle_active" in disk2_card and
            "(vtw_cycle_tick || vtw_io_read);" in disk2_card and
            "assign vtw_time_ready" in disk2_card and
            "assign vtw_write_timing_active" in disk2_card and
            "logic       vtw_drive_spinning_q;" in disk2_card and
            "vtw_drive_spinning_q <= drive_spinning;" in disk2_card and
            "enabled && ab_read.res && vtw_drive_spinning_q && q7_q" in disk2_card and
            "!vtw_drive_spinning_q || !drive_has_media" in disk2_card and
            "logic        vtw_req_pending_q;" in disk2_card and
            "wire vtw_io_read = vtw_req_pending_q;" in disk2_card and
            "vtw_resp_valid <= 1'b1;" in disk2_card,
            "disk2_card must expose virtual time, safe holds, and the private read response")
    require("CARD_CTRL_REG_VTW_SLOWDOWN" in top and
            ".slow_region_en(vtw_slowdown_q[9:0])" in top and
            ".slow_duration(vtw_slowdown_q[31:16])" in top,
            "apple_top must decode the slowdown register and wire it to the core")
    service_c = read("ps_sources/frontend/vtw_service.c")
    require("vtw_service_set_slowdown" in service_c and
            "CARD_CTRL_VTW_SLOWDOWN_REG" in service_c,
            "vtw_service must push the slowdown config to the PL")
    require("CARD_CTRL_VTW_CTRL_IIPLUS_DMA_REFRESH_BIT" not in regs and
            "vtw_service_set_iiplus_dma_refresh" not in service_c and
            "g_iiplus_dma_refresh_enabled" not in service_c and
            "dma-refresh" not in uart,
            "automatic II+ /DMA refresh must have no PS control bit, "
            "runtime selector, or UART command")

    # Takeover machine reset (RES#-at-takeover)
    wrapper = read("hdl/apple/apple_bus_wrapper.sv")
    service = read("ps_sources/frontend/vtw_service.c")
    require("assign apple_res_pin = 1'bz;" in wrapper and
            "apple_reset_release && !ab_write_arb.assert_res;" in top,
            "all internal RESET requests must use the dedicated A2CTRL "
            "transistor while A2FPGA.RESET remains observation-only")
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
    require("input  logic                    ignore_c074" in core_top and
            "if (!ab_read.res || ignore_c074)" in core_top and
            ".ignore_c074(vtw_ctrl_q[6])" in top and
            "CARD_CTRL_VTW_CTRL_IGNORE_C074_BIT" in regs and
            "vtw_service_set_ignore_c074" in service,
            "the persisted $C074 override must clear and ignore every software value")
    require("CARD_CTRL_VTW_CTRL_DISABLE_D2_ACCEL_BIT" in regs and
            "vtw_service_set_disk2_accel_disabled" in service and
            "g_disable_disk2_accel" in service,
            "the persisted Disk II compatibility switch must reach VTW_CTRL bit7")

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
    require("vtw_is_video_window(cycle_addr_q, xl_is_aux," in core_top and
            "post_main_wide_eff)" in core_top and
            "wire post_main_wide_eff = post_main_wide | shr_post_main_wide_q;"
            in core_top and
            "cycle_addr_q == 16'h9DF8" in core_top and
            "input logic is_aux" in engine and
            "input logic wide_main" in engine,
            "vTW must extend the aux posted-write window for Super Hi-Res "
            "and arm the main interlace window from its private ctrl write")
    require("wire core_post_accept = core_post_req && !eng_post_full;"
            in core_top and
            "wire arm_post_accept = arm_post_we && arm_post_ready;"
            in core_top and
            "logic        post_stage_valid_q;" in core_top and
            "logic [15:0] post_stage_addr_q;" in core_top and
            "logic [7:0]  post_stage_wdata_q;" in core_top and
            "post_stage_valid_q <= core_post_accept || arm_post_accept;"
            in core_top and
            "post_stage_addr_q  <= arm_post_accept ? arm_post_addr"
            in core_top and
            "post_stage_wdata_q <= arm_post_accept ? arm_post_wdata"
            in core_top and
            "assign eng_post_we    = post_stage_valid_q && rstn && enable && ab_read.res;"
            in core_top and
            "assign eng_post_addr  = post_stage_addr_q;" in core_top and
            "assign eng_post_wdata = post_stage_wdata_q;" in core_top and
            "if (!rstn || !enable || !ab_read.res) begin\n"
            "            post_stage_valid_q <= 1'b0;" in core_top and
            "assign arm_post_ready = core_active && !eng_post_full && !core_post_req;"
            in core_top and
            "assign eng_post_we    = core_post_push" not in core_top,
            "vTW must register the final accepted core-or-ARM posted tuple "
            "for one clock, keep core priority, and cancel it with queue clear")
    require("vtw_service_init(UART0_BASE);" in main_c and
            "vtw_service_poll();" in main_c and
            "menu_platform.set_vtw_config = control_set_vtw_config;" in main_c,
            "main.c must init, poll, and bind the vTW service")
    require('str_ieq(argv[0], "vtw")' in uart and
            "config_menu_set_vtw_enabled(g_sdd_config_menu, enable);" in uart,
            "uart vtw command must route enable through the config menu")
    require('"vtw.enabled=%s\\n"' in menu_c and
            '"vtw.c074.ignore=%s\\n"' in menu_c and
            '"vtw.disk2.acceleration.disabled=%s\\n"' in menu_c and
            'strcmp(key, "vtw.enabled") == 0' in menu_c and
            'strcmp(key, "vtw.c074.ignore") == 0' in menu_c and
            'strcmp(key, "vtw.disk2.acceleration.disabled") == 0' in menu_c and
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
            '"Ignore $C074 Speed Switch"' in tabs_c and
            '"Disable DiskII Acceleration"' in tabs_c and
            "option_x2, y + (2 * row_h), option_w2" in tabs_c and
            '"TransWarp on: 128K + 8MB RamWorks, accelerated"' in tabs_c,
            "TransWarp tab must show both compatibility checkboxes on one row, "
            "and RAM must surface the accelerated-RamWorks status")
    require("TAB_WITH_OVERRIDES(CONFIG_TAB_TRANSWARP, transwarp, transwarp_overrides)," in help_c,
            "TransWarp tab must have help text")

    # II/II+ hosts: the embedded //e ROM's reset-path Apple-key checks must
    # be patched at load (floating game-connector buttons otherwise enter
    # the ROM self-test and force cold starts). Verify the patch table, the
    # machine gate, the original-byte guard, and that the embedded ROM
    # still carries the exact bytes the patch was audited against.
    require("k_iiplus_rom_patches" in service and
            "{ 0x02BBU, 0xA9U }" in service and
            "{ 0x02C3U, 0xA9U }" in service and
            "{ 0x02BEU, 0x80U }" not in service and
            "{ 0x02C6U, 0x80U }" not in service,
            "vtw_service must patch the //e ROM button checks at $C2BB/$C2C3")
    require("boot_menu_service_machine_mode() == CARD_MACHINE_MODE_IIPLUS" in service,
            "the ROM button patch must be gated on a II/II+ host")
    require("II+ button patch SKIPPED" in service,
            "the ROM button patch must guard against a changed ROM image")
    rom = (ROOT / "docs" / "Apple2e_Enhanced.rom").read_bytes()
    require(rom[0x02BB:0x02BE] == bytes((0xAD, 0x62, 0xC0)) and
            rom[0x02BE:0x02C3] == bytes((0x10, 0x03, 0x4C, 0x00, 0xC6)) and
            rom[0x02C3:0x02C6] == bytes((0xAD, 0x61, 0xC0)) and
            rom[0x02C6:0x02C8] == bytes((0x10, 0x1A)),
            "embedded //e ROM reset button checks moved -- re-audit the II+ patch offsets")

    # vTW-on-II+ fix batch (raw-PHI0 data release, IRQ merge, button
    # synthesis, OA-window machine gate): the wiring must stay intact end
    # to end. The II+-specific hold is permitted only for read responses;
    # writes must still release directly at raw PHI0.
    require("LUT6 #(.INIT(64'hFFFF_FFFF_8088_8080)) apple_data_enable_lut" in wrapper and
            ".I0(bus_emit_state)," in wrapper and
            ".I1(physical_data_en_safe)," in wrapper and
            ".I2(apple_phi0_pin)," in wrapper and
            ".I3(host_is_iiplus)," in wrapper and
            ".I4(physical_addr_rw_en_q)," in wrapper and
            ".I5(data_override_safe)," in wrapper and
            "physical_data_en_q    <= ab_write.wr_data_en;" in wrapper and
            "physical_data_q <= ab_write.wr_data;" in wrapper and
            "physical_inh_dependent_q <= ab_write.assert_inh;" in wrapper and
            "iiplus_read_inh_dependent_q <=" in wrapper and
            "physical_inh_dependent_q;" in wrapper and
            "(!physical_inh_dependent_q || inh_allowed)" in wrapper and
            "drive_live && (!physical_addr_rw_en_q || physical_rw_q)" in wrapper and
            "DMA_WRITE_START_CLKS = 18" in wrapper and
            "DMA_WRITE_END_CLKS   = 40" in wrapper and
            "ab_write.wr_dma_data_en" in wrapper and
            "else if (phi0_fall)" in wrapper and
            "else if (read_response_live && phi0_filt)" in wrapper and
            "data_override_q && data_override_saved_q" in wrapper and
            "iiplus_read_hold_active ? iiplus_read_data_q : physical_data_q" in wrapper,
            "placed data-enable LUT must absorb the phase/data-enable AND "
            "from a staged physical response while preserving the II+ saved-read hold "
            "and timed DMA write window with an INH fail-safe")
    require("{IOSTANDARD LVCMOS33 DRIVE 12 SLEW FAST}" in xdc and
            "[get_ports a2fpga_dir_d]" in xdc,
            "the raw-PHI0 data-direction release must use the timed fast pad edge")
    require("TAP_IRQ_REARM_RELEASE = TAP_DATA_SNAP - 8;" in wrapper and
            "TAP_IRQ_REARM_ASSERT  = TAP_DATA_SNAP - 4;" in wrapper and
            "always_ff @(posedge clk)" in wrapper and
            "(!host_is_iiplus || !irq_rearm_release_q);" in wrapper and
            "{NAME =~ *apple_bus_wrapper_i/irq_rearm_release_q_reg}" in xdc and
            "{IOSTANDARD LVCMOS33 DRIVE 16 SLEW FAST}" in xdc and
            "-to [get_ports a2fpga_irq_n]" in xdc,
            "II+ IRQ must use a mostly-low re-arm waveform with a bounded "
            "register-to-pad route and the strongest IRQ-pad edge")
    require("TAP_DMA_REARM_RELEASE = TAP_DATA_SNAP - 8;" in wrapper and
            "TAP_DMA_REARM_ASSERT  = TAP_DATA_SNAP - 4;" in wrapper and
            "iiplus_dma_refresh_active && dma_rearm_release_q" in wrapper and
            "assign vtw_iiplus_dma_refresh_active = vtw_host_is_iiplus_q;" in top and
            ".iiplus_dma_refresh_active(vtw_iiplus_dma_refresh_active)" in top and
            "{NAME =~ *apple_bus_wrapper_i/dma_rearm_release_q_reg}" in xdc and
            "-to [get_ports a2fpga_dma_n]" in xdc,
            "automatic /DMA refresh must be phase locked and impossible "
            "outside a session latched as II/II+")
    require("S_RESET_HOLD" in engine and
            "A II/II+ has no //e MMU/IOU" in engine and
            "assert_dma_q    <= 1'b1;" in engine and
            "session_q <= (!enable || !host_is_iiplus)" in engine and
            "wr_addr_q       <= IIPLUS_PARK_ADDR;" in engine,
            "an active II+ session must hold /DMA through RESET, release "
            "address/data, and resume at the inert park address")
    require("if (host_is_iiplus &&" in engine and
            "(cyc_q == CYC_SYNC && !sync_rw_q)" in engine and
            "(cyc_q == CYC_POST)" in engine and
            "wr_data_en_q <= 1'b0;" in engine,
            "II+ vTW writes must clear their logical data drive at data_en "
            "instead of carrying it into the next cycle")
    core = read("hdl/apple/vtw_core_top.sv")
    require("core_irq_n = ab_read.irq & ~irq_assert_in" in core and
            ".irq_n(core_irq_n)" in core,
            "vTW core must merge the internal IRQ assert with the pin sample")
    require("xl_btn_rd" in core and
            "iiplus_buttons_zero" in core,
            "vTW core must synthesize $C061-$C063 reads on II/II+ hosts")
    require(".irq_assert_in(ab_write_arb.assert_irq)" in top and
            ".iiplus_buttons_zero(vtw_ctrl_q[5])" in top,
            "apple_top must wire the vTW IRQ merge and button-synthesis controls")
    require("CARD_CTRL_VTW_CTRL_IIPLUS_BTNS_BIT (1UL << 5)" in regs,
            "ctrl bit 5 must be reserved for II+ button synthesis")
    require("CARD_CTRL_VTW_CTRL_IIPLUS_BTNS_BIT;" in service,
            "vtw_service must set the II+ button-synthesis bit for II/II+ hosts")
    require(".sync(core_sync)" in core and
            "eng_bad_c000_pulse" in core and
            "dbg_trace_selftest_event" in core,
            "vTW must preserve an instruction trail through RESET and freeze "
            "on phantom keyboard input or internal self-test entry")
    require("dbg_bad_c000_event" in engine and
            "dbg_self_data_bad" in engine and
            "dbg_addr_bad" in engine and
            "ab_read.dma" in engine and
            "dbg_io_trace_q" in engine,
            "vTW must preserve physical-cycle context and distinguish "
            "address, self-drive-data, and DMA-release faults")
    require("CARD_CTRL_VTW_TRACE_STATUS_REG" in regs and
            "CARD_CTRL_VTW_IO_TRACE_REG(n)" in regs and
            "CARD_CTRL_VTW_PC_TRACE_REG(n)" in regs and
            ".data_drive_value_in(ab_write_arb.wr_data)" in top,
            "event trace must be wired through the card-control window")
    require("CARD_CTRL_RESET_FORENSICS_INTERNAL_BIT" in regs and
            "CARD_CTRL_RESET_FORENSICS_EXTERNAL_BIT" in regs and
            "res_internal_seen_sticky" in top and
            "res_external_seen_sticky" in top and
            "REG_WRITE(CARD_CTRL_RESET_FORENSICS_REG, 1U);" in uart,
            "busdbg clear must re-arm reset-source classification")
    require("vtw: trace=" in service and "vtw: io trail" in service and
            "vtw: pc trail" in service,
            "vtw status must print an event-frozen CPU and I/O trail")
    require("vtw: slowdown mask=" in service,
            "vtw status must surface the slowdown configuration")
    bmc = read("hdl/apple/boot_menu_card.sv")
    require("end else if (machine_id_q != 4'd1) begin" in bmc,
            "boot menu OA snoop window must not arm on II/II+ hosts")


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
                 "--timescale", "1ns/1ps", "-L", "unisims_ver"],
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
