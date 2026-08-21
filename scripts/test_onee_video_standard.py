#!/usr/bin/env python3
"""Prove the ONE//e PAL/NTSC session, scanner, UI, and routing contract."""

from __future__ import annotations

import hashlib
import re
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "onee_video_standard_sim"


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def between(source: str, start_marker: str, end_marker: str) -> str:
    start = source.find(start_marker)
    end = source.find(end_marker, start + len(start_marker))
    require(start >= 0 and end > start,
            f"could not isolate {start_marker}")
    return source[start:end]


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


def static_contract_checks() -> None:
    top = read("hdl/apple/apple_top.sv")
    control = read("hdl/apple/onee_video_standard_ctrl.sv")
    bus = read("hdl/apple/apple_virtual_bus.sv")
    timing = read("hdl/apple/apple_timing_gen.sv")
    sources = read("hdl/hdl_sources.txt").splitlines()
    regs = read("ps_sources/frontend/card_control_regs.h")
    menu = read("ps_sources/frontend/config_menu.c")
    menu_header = read("ps_sources/frontend/config_menu.h")
    menu_internal = read("ps_sources/frontend/config_menu_internal.h")
    menu_draw = read("ps_sources/frontend/config_menu_main_tabs.c")
    menu_help = read("ps_sources/frontend/config_menu_help.c")
    main = read("ps_sources/frontend/main.c")
    boot = read("ps_sources/frontend/boot_menu_service.c")
    vtw = read("ps_sources/frontend/vtw_service.c")
    renderer = read("ps_sources/frontend/apple_cycle_renderer.c")
    video_top = read("hdl/video2/video_top.sv")
    dvi_timing = read("hdl/video2/video_timing_gen.sv")

    require(sources.count("apple/onee_video_standard_ctrl.sv") == 1,
            "ONE//e standard latch must appear once in hdl_sources.txt")
    require(re.search(r"CARD_CTRL_REG_ONEE_VIDEO\s*=\s*8'hA2", top) is not None,
            "card-control offset 0xA2 is not assigned to ONE//e video")
    a2_card_regs = re.findall(
        r"(CARD_CTRL_REG_[A-Z0-9_]+)\s*=\s*8'hA2", top
    )
    require(a2_card_regs == ["CARD_CTRL_REG_ONEE_VIDEO"],
            "card-control offset 0xA2 has another owner")
    require("CARD_CTRL_ONEE_VIDEO_REG" in regs and
            "CARD_CTRL_REG_ADDR(0xA2U)" in regs,
            "firmware does not share the 0xA2 register contract")

    require(".virtual_res_n  (virtual_ab_read.res)" in top,
            "standard latch must use the resolved virtual RESET line")
    require("if (!enabled || !virtual_res_n)" in control and
            "active_50hz <= write_valid ? write_50hz : desired_50hz;" in control,
            "active standard is not frozen until reset/session end")
    require("onee_video_50hz_active" in top and
            "onee_video_50hz_desired" in top and
            "CARD_CTRL_REG_ONEE_VIDEO:" in top,
            "desired/active readback is incomplete")
    require("parameter integer CYCLE_CLKS     = 130" in bus and
            "parameter integer PAL_CYCLE_CLKS = CYCLE_CLKS + 1" in bus and
            "video_mode_50hz ?" in bus,
            "virtual bus does not select 130/131-clock cadence")
    require("video_mode_50hz ? 9'd311 : 9'd261" in timing and
            "VBL_START_LINE = 9'd192" in timing,
            "scanner is not fixed at 312/262 lines with VBL line 192")

    # This digest covers the pre-existing physical period detector and reset
    # calibration block. The ONE//e override must route around it, not edit it.
    detector_start = top.index(
        "    always_ff @(posedge clk) begin\n"
        "        if (!rstn[1]) begin\n"
        "            apple_reset_prev_q"
    )
    detector_end = top.index("\n\n    apple_timing_gen", detector_start)
    detector = top[detector_start:detector_end]
    require(
        hashlib.sha256(detector.encode("utf-8")).hexdigest() ==
        "2bcd79d884a3b6fa054191ef2df05e79396b58308fd93a142fd167f09ee9408b",
        "physical Apple PAL/NTSC detector changed",
    )
    require(
        "assign video_mode_50hz = onee_enable_effective ?\n"
        "        onee_video_50hz_active :\n"
        "        (video_mode_50hz_valid_q && video_mode_50hz_detected_q);" in top,
        "ONE//e standard does not leave the physical measured path intact",
    )

    require(".apple_video_mode_valid(video_mode_50hz_valid)" in top and
            ".apple_video_mode_50hz(video_mode_50hz)" in top,
            "boot-menu timing readback does not receive the active standard")
    require("BOOT_MENU_C8_DELAY_PAL_X" in boot and
            "BOOT_MENU_C8_DELAY_NTSC_X" in boot and
            "BOOT_MENU_APPLE_TIMING_50HZ" in boot,
            "boot ROM PAL/NTSC delay patch selection is missing")
    onee_start = between(vtw,
                         "uint8_t vtw_service_onee_start(",
                         "uint8_t vtw_service_onee_set_paused")
    onee_reboot = between(vtw,
                          "uint8_t vtw_service_onee_cold_reboot(",
                          "void vtw_service_onee_suspend")
    start_reset = onee_start.find("vtw_onee_ctrl_value(0U, 1U)")
    start_patch = onee_start.find(
        "boot_menu_service_apply_video_rom_patch();")
    start_release = onee_start.find("vtw_onee_ctrl_value(0U, 0U)")
    reboot_assert = onee_reboot.find("vtw_apply_ctrl_live()")
    reboot_cold = onee_reboot.find("vtw_shadow_force_cold_start(1U)")
    reboot_patch = onee_reboot.find(
        "boot_menu_service_apply_video_rom_patch();")
    reboot_timer = onee_reboot.find("XTime_GetTime")
    require(0 <= start_reset < start_patch < start_release and
            0 <= reboot_assert < reboot_cold < reboot_patch < reboot_timer,
            "boot ROM standard patch is not inside both private RES# holds")
    require(".line_in_frame(line_in_frame)" in top and
            "if (line >= ATN_SCANNER_MAX_VERT_NTSC)" in renderer and
            "(s_prev_line >= 300u) ? ATN_SCANNER_MAX_VERT_PAL" in renderer,
            "renderer does not infer the selected scanner standard")
    require("cdc_apple_video_mode_50hz_i" in video_top and
            ".mode_1080p50 (apple_video_mode_50hz_pixel)" in video_top,
            "DVI timing does not receive the selected standard safely")
    require("if (frame_end) begin" in dvi_timing and
            "mode_1080p50_latched <= mode_1080p50;" in dvi_timing and
            "wire [11:0] h_total = mode_1080p50_latched ?" in dvi_timing,
            "DVI mode is not applied at a complete-frame boundary")

    require("#define APPLETINI_CFG_VERSION 116U" in menu and
            "cfg_version < APPLETINI_CFG_VERSION" in menu,
            "v115 global config migration to v116 is not enabled")
    require('strcmp(key, "onee.video.standard") == 0' in menu and
            '"onee.video.standard=%s\\n"' in menu,
            "global ONE//e video standard persistence is missing")
    require(
        'strcmp(key, "onee.standalone.persisted") != 0 &&\n'
        '                strcmp(key, "onee.video.standard") != 0' in menu,
        "profile load does not reject the global standard key",
    )
    require("CONFIG_DEFAULT_ONEE_VIDEO_50HZ 0U" in menu and
            "menu->onee_video_50hz = CONFIG_DEFAULT_ONEE_VIDEO_50HZ;" in menu,
            "ONE//e video standard does not default to NTSC")
    boot_draw = between(menu_draw,
                        "void config_menu_draw_boot_settings",
                        "void config_menu_draw_video")
    video_draw = between(menu_draw,
                         "void config_menu_draw_video",
                         "void config_menu_draw_clock")
    require("#define CONFIG_MENU_BOOT_ONEE_STANDARD_ITEM" in menu_header and
            "CONFIG_MENU_BOOT_USB_BIND_RESET_ITEM" in menu_header and
            re.search(
                r"#define\s+CONFIG_MENU_BOOT_ONEE_STANDARD_ITEM\s+"
                r"\\\s+CONFIG_MENU_BOOT_USB_BIND_RESET_ITEM",
                menu_header,
            ) is not None and
            "CONFIG_VIDEO_ITEM_ONEE_STANDARD" not in menu_internal and
            '"ONE//e video standard"' in boot_draw and
            "if (onee_fixed != 0U)" in boot_draw and
            '"ONE//e video standard"' not in video_draw,
            "ONE//e-only standard row must live below standalone in Boot Settings")
    require("standard_was_visible != 0U" in menu and
            "menu->item_focus = CONFIG_MENU_BOOT_ONEE_ITEM;" in menu,
            "stopping ONE//e must not turn the standard row into USB reset")
    require("PAL (pending reset)" in menu and
            "NEXT VIRTUAL RESET OR RESTART" in menu and
            "next virtual reset or restart" in menu_help,
            "UI does not distinguish desired from active cadence")
    require("set_onee_video_50hz" in menu_header and
            "get_onee_video_active_50hz" in menu_header and
            "menu_platform_set_onee_video_50hz" in main and
            "CARD_CTRL_ONEE_VIDEO_ACTIVE_50HZ_BIT" in main,
            "platform desired/active callback path is incomplete")
    toggle = between(menu,
                     "static void config_menu_toggle_onee_video_standard",
                     "static uint8_t config_menu_usb_binding_source_valid")
    require("menu->platform.set_onee_video_50hz" in toggle and
            "config_menu_save_settings_to_path(" in toggle and
            "config_menu_apply_runtime" not in toggle and
            menu.count("config_menu_toggle_onee_video_standard(menu);") == 2,
            "video-standard edit must use only its narrow write/save path")


def run_bench(top: str, snapshot: str, marker: str) -> None:
    run(
        [
            vivado_tool("xelab"),
            top,
            "-s",
            snapshot,
            "--timescale",
            "1ns/1ps",
        ],
        f"xelab_{top}.log",
    )
    output = run(
        [vivado_tool("xsim"), snapshot, "--runall"],
        f"xsim_{top}.log",
    )
    require(marker in output, f"{top} did not report PASS")


def main() -> int:
    static_contract_checks()

    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    OUT_DIR.mkdir(parents=True)
    run(
        [
            vivado_tool("xvlog"),
            "--sv",
            str(ROOT / "hdl" / "globals.sv"),
            str(ROOT / "hdl" / "apple" / "onee_video_standard_ctrl.sv"),
            str(ROOT / "hdl" / "apple" / "apple_timing_gen.sv"),
            str(ROOT / "hdl" / "apple" / "apple_virtual_bus.sv"),
            str(ROOT / "hdl" / "sim" / "tb_onee_video_standard.sv"),
            str(ROOT / "hdl" / "sim" / "tb_apple_virtual_bus.sv"),
        ],
        "xvlog.log",
    )
    run_bench(
        "tb_onee_video_standard",
        "tb_onee_video_standard_snap",
        "ONEE VIDEO STANDARD PASS",
    )
    run_bench(
        "tb_apple_virtual_bus",
        "tb_onee_video_bus_cadence_snap",
        "APPLE VIRTUAL BUS PASS",
    )
    print("ONE//e PAL/NTSC standard contract PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
