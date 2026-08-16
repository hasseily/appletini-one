#!/usr/bin/env python3
"""Check ONE//e input, virtual-reset, and speaker top-level wiring."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "onee_top_io_sim"
APPLE_TOP = ROOT / "hdl" / "apple" / "apple_top.sv"
BOARD_TOP = ROOT / "hdl" / "appletini_yarz_top.sv"
SOURCES = ROOT / "hdl" / "hdl_sources.txt"
RESET_RTL = ROOT / "hdl" / "apple" / "onee_warm_reset_ctrl.sv"
RESET_BENCH = ROOT / "hdl" / "sim" / "tb_onee_warm_reset_ctrl.sv"


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


def instance_text(source: str, start: str) -> str:
    begin = source.index(start)
    end = source.index("\n    );", begin) + len("\n    );")
    return source[begin:end]


def static_contract_checks() -> None:
    apple = APPLE_TOP.read_text(encoding="utf-8")
    board = BOARD_TOP.read_text(encoding="utf-8")
    sources = SOURCES.read_text(encoding="utf-8")

    for name in (
        "apple/onee_input_bridge.sv",
        "apple/onee_warm_reset_ctrl.sv",
        "apple/onee_speaker_audio.sv",
    ):
        require(name in sources, f"missing HDL source-list entry: {name}")

    bridge = instance_text(apple, "onee_input_bridge onee_input_bridge_i")
    for token in (
        ".enabled                (onee_enable_effective)",
        ".ps_addr                (as_common.awaddr)",
        ".ps_read_addr           (as_common.araddr)",
        ".keyboard_event_valid   (onee_usb_keyboard_event_valid)",
        ".keyboard_event_ready   (onee_usb_keyboard_event_ready)",
        ".keyboard_modifiers     (onee_usb_keyboard_modifiers)",
        ".pushbuttons             (onee_usb_pushbuttons)",
        ".paddle_values          (onee_usb_paddle_values)",
        ".warm_reset_request     (onee_warm_reset_request)",
        ".warm_reset_ack         (onee_warm_reset_ack)",
    ):
        require(token in bridge, f"input bridge is not wired: {token}")

    for offset in ("5C", "5D", "5E", "5F"):
        require(f"= 8'h{offset};" in apple,
                f"CARD_CTRL 0x{offset} has no top-level decode")
    require(apple.count("as_client_rdata_q <= onee_input_ps_rdata;") == 4,
            "all four ONE//e input registers must reach the AXI read mux")

    reset = instance_text(apple, "onee_warm_reset_ctrl #(")
    require(".MIN_NATIVE_CYCLES(8)" in reset,
            "warm reset must span eight native cycles")
    require("virtual_ab_read.data_en &&" in reset and
            "virtual_ab_read.cycle_valid" in reset,
            "warm reset must count private native bus cycles")
    virtual_bus = instance_text(apple, "apple_virtual_bus apple_virtual_bus_i")
    require(".res_n_in         (onee_virtual_res_n)" in virtual_bus,
            "warm reset must drive the virtual motherboard RESET input")
    motherboard = instance_text(
        apple, "onee_motherboard_io onee_motherboard_io_i"
    )
    require(".ab_read                 (ab_read)" in motherboard and
            ".enabled                 (onee_enable_effective)" in motherboard,
            "virtual RESET must reach enabled motherboard state through ab_read")
    vtw = instance_text(apple, "vtw_core_top vtw_core_top_i")
    require(".ab_read(ab_read)" in vtw,
            "virtual RESET must reach the soft CPU through ab_read")
    physical_bus = instance_text(apple, "apple_bus_wrapper apple_bus_wrapper_i")
    require("onee_virtual_res_n" not in physical_bus and
            ".apple_res_pin(apple_res_pin)" in physical_bus,
            "virtual warm reset must not enter the physical RESET path")

    speaker = instance_text(apple, "onee_speaker_audio onee_speaker_audio_i")
    for token in (
        ".enabled          (onee_enable_effective)",
        ".speaker_level    (onee_speaker)",
        ".audio_sample_tick(audio_sample_tick)",
        ".audio_mono       (onee_audio_mono_raw)",
    ):
        require(token in speaker, f"speaker source is not wired: {token}")
    require("assign onee_audio_mono = onee_enable_effective ?" in apple,
            "speaker output must have an immediate effective-mode mask")

    require(".onee_audio_mono(onee_audio_mono)" in board,
            "board top must receive ONE//e speaker audio")
    require(
        "assign card_audio_l = sat_add16(mockingboard_audio_l, disk2_audio_l);"
        in board and
        "assign card_audio_r = sat_add16(mockingboard_audio_r, disk2_audio_r);"
        in board and
        "assign mixed_audio_l = sat_add16(card_audio_l, onee_audio_mono);"
        in board and
        "assign mixed_audio_r = sat_add16(card_audio_r, onee_audio_mono);"
        in board,
        "speaker mono must enter both stereo channels through signed saturation",
    )


def main() -> int:
    static_contract_checks()

    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    OUT_DIR.mkdir(parents=True)

    run([
        vivado_tool("xvlog"), "--sv", str(RESET_RTL), str(RESET_BENCH),
    ], "xvlog.log")
    run([
        vivado_tool("xelab"), "tb_onee_warm_reset_ctrl",
        "-s", "tb_onee_warm_reset_ctrl_snap",
    ], "xelab.log")
    output = run([
        vivado_tool("xsim"), "tb_onee_warm_reset_ctrl_snap", "--runall",
    ], "xsim.log")
    require("PASS: ONE//e warm reset controller" in output,
            "warm-reset simulation did not report its pass marker")

    print("ONE//e top I/O integration test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
