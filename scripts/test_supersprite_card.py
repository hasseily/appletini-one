#!/usr/bin/env python3
"""Source-level regression tests for the SuperSprite (TMS9918 VDP) card.

Runs without Vivado or hardware -- it greps the RTL, the PS renderer, and the
integration points to confirm the SuperSprite pieces are wired together:

    python scripts/test_supersprite_card.py

Architecture under test: the PL (supersprite_card.sv) implements only the
real-time TMS9918 register/VRAM interface at slot 7 ($C0Fx, mutually exclusive
with SmartPort); the PS
(supersprite_vdp.c) renders the picture in software; the compositor black-key
overlays it. See docs / memory `supersprite-card`.
"""

from __future__ import annotations

import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SUPERSPRITE_SV = REPO_ROOT / "hdl" / "apple" / "supersprite_card.sv"
APPLE_TOP_SV = REPO_ROOT / "hdl" / "apple" / "apple_top.sv"
TOP_SHELL_SV = REPO_ROOT / "hdl" / "appletini_yarz_top.sv"
HDL_SOURCES = REPO_ROOT / "hdl" / "hdl_sources.txt"
CARD_REGS_H = REPO_ROOT / "ps_sources" / "frontend" / "card_control_regs.h"
VDP_C = REPO_ROOT / "ps_sources" / "frontend" / "supersprite_vdp.c"
VDP_H = REPO_ROOT / "ps_sources" / "frontend" / "supersprite_vdp.h"
COMPOSITOR_C = REPO_ROOT / "ps_sources" / "frontend" / "compositor.c"
CONFIG_MENU_C = REPO_ROOT / "ps_sources" / "frontend" / "config_menu.c"
CONFIG_MENU_HELP_C = REPO_ROOT / "ps_sources" / "frontend" / "config_menu_help.c"
CONFIG_MENU_TABS_C = (
    REPO_ROOT / "ps_sources" / "frontend" / "config_menu_device_tabs.c"
)
FRONTEND_USERCONFIG = (
    REPO_ROOT / "vitis_workspace" / "frontend" / "src" / "UserConfig.cmake"
)


class TestFailure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def test_pl_slot_decode_and_reset() -> None:
    s = read(SUPERSPRITE_SV)
    require("module supersprite_card" in s, "supersprite_card module must exist")
    require("(ab_read.addr[15:8] == 8'hC0)" in s and
            "(ab_read.addr[7:4] == (4'h8 + slot_assign))" in s,
            "must decode the $C0(8+slot)x device window (slot 7 -> $C0Fx)")
    require("wire card_enabled = (slot_assign != 3'd0);" in s,
            "card is enabled only when a non-zero slot is assigned")
    require("!ab_read.res" in s and "soft_reset" in s,
            "Apple reset and the $C0n7 soft reset must reset the VDP")


def test_pl_tms9918_protocol() -> None:
    s = read(SUPERSPRITE_SV)
    # command/data ports and the address flip-flop
    require("vdp_data_hit = (off == OFF_VDP_DATA)" in s and
            "vdp_ctrl_hit = (off == OFF_VDP_CTRL)" in s,
            "VDP data port at +0, control/register port at +1")
    require("addr_ff_q" in s and "addr_temp_q" in s,
            "two-byte address/register flip-flop")
    require("2'b00: begin // set VRAM read address" in s and
            "2'b01: begin // set VRAM write address" in s and
            "2'b10: begin // write register" in s,
            "control port must decode read-setup / write-setup / register-write")
    require("read_buffer_q" in s and "prefetch_dly_q" in s,
            "TMS9918 read-ahead buffer with a settle delay after read-setup")
    require("logic [13:0] addr_q" in s and "addr_q + 14'd1" in s,
            "14-bit auto-increment VRAM address")
    require("logic [7:0] regs [0:7]" in s or "regs [0:7]" in s,
            "eight write-only registers R0..R7")
    require('ram_style = "block"' in s and "vram [0:16383]" in s,
            "16 KB VRAM in a BRAM")


def test_pl_status_irq_and_switches() -> None:
    s = read(SUPERSPRITE_SV)
    require("frame_flag_q" in s and "vblank_tick" in s,
            "frame flag (status bit 7) is set at vblank_tick")
    require("vdp_ctrl_rd" in s and "frame_flag_q <= 1'b0" in s,
            "reading status clears the frame flag")
    require("regs[1][5]" in s and "assert_irq" in s,
            "interrupt gated on R1 bit5 (IE), driven via ab_write.assert_irq")
    require("apple_video_q" in s and "vdp_overlay_q" in s and
            "OFF_SW_APPLE_ON" in s and "OFF_SW_VDP_MIX" in s,
            "video soft switches ($C0n3..$C0n6)")
    require("ps_regs" in s and "ps_status" in s and "ps_vram_addr" in s and
            "ps_vram_data" in s and "ps_frame" in s,
            "PS-facing readback ports")


def test_pl_psg_audio() -> None:
    s = read(SUPERSPRITE_SV)
    require("YM2149 ssp_psg (" in s,
            "SuperSprite must instantiate the AY-3-8910 (YM2149) PSG")
    require("off[3:2] == 2'b11" in s and "addr_psg" in s,
            "PSG decodes the $C0nC..$C0nF sound registers")
    require("ssp_audio" in s and "psg_ch_a" in s,
            "PSG mono audio output from the three channels")
    require("logic signed [15:0] ssp_audio_q;" in s and
            "assign ssp_audio = ssp_audio_q;" in s,
            "PSG mono output is registered before top-level audio mixing")
    require("PSG data read" in s and "psg_dout" in s,
            "PSG data read is presented back on the Apple bus")
    top = read(APPLE_TOP_SV)
    require(".ssp_audio(ss_psg_audio)" in top, "PSG audio is wired in apple_top")
    require("ss_psg_audio >>> 1" in top and "mockingboard_audio_l" in top,
            "PSG is summed into the card-audio bus with saturation")
    shell = read(TOP_SHELL_SV)
    require(".sample_l  (mockingboard_audio_24_sampled_fclk[47:24])" in shell and
            ".sample_r  (mockingboard_audio_24_sampled_fclk[23:0])" in shell,
            "SPDIF must use the sampled audio register, not the live mix path")


def test_apple_top_integration() -> None:
    s = read(APPLE_TOP_SV)
    require("supersprite_card supersprite_card_i (" in s,
            "SuperSprite must be instantiated in apple_top")
    require(".ab_read(gate_ab(ab_read, card_supersprite_enable))" in s and
            ".slot_assign(3'h7)" in s,
            "SuperSprite occupies hard slot 7 when its feature gate is enabled")
    require("card_feature_enable_mask_q[CARD_CTRL_FEATURE_SS_ENABLE_BIT]" in s,
            "enable comes from the feature-enable mask")
    require(".ab_read(gate_ab(ab_read,\n"
            "                         smartport_active && !card_supersprite_enable))" in s,
            "SmartPort (also slot 7) must be gated off when SuperSprite is enabled")
    require(".vblank_tick(apple_vblank_start_pulse)" in s,
            "frame tick reuses the Apple vblank pulse")
    require("apple_bus_write_arbiter #(.NUM_CLIENTS(11))" in s and
            "supersprite_ab_write" in s,
            "SuperSprite must be in the 11-client write arbiter")
    # PS export window
    require("CARD_CTRL_REG_SS_REGS_LO" in s and "CARD_CTRL_REG_SS_VRAM_ADDR" in s and
            "CARD_CTRL_REG_SS_SPR_FLAGS" in s,
            "SuperSprite card-control registers declared")
    require("ss_regs[31:0]" in s and "ss_regs[63:32]" in s and
            "ss_vram_data" in s,
            "read mux exposes VDP regs + VRAM data to the PS")
    require("ss_vram_addr_q <= a[13:0]" in s and "ss_status_flags_q <= f[6:0]" in s,
            "write decode accepts the VRAM address and PS sprite flags")


def test_ps_header_and_build() -> None:
    require("supersprite_card.sv" in read(HDL_SOURCES),
            "RTL must be in the HDL manifest")
    require("supersprite_vdp.c" in read(FRONTEND_USERCONFIG),
            "PS renderer must be in the frontend build")
    regs = read(CARD_REGS_H)
    for name in ("CARD_CTRL_SS_REGS_LO_REG", "CARD_CTRL_SS_REGS_HI_REG",
                 "CARD_CTRL_SS_STATUS_REG", "CARD_CTRL_SS_VRAM_DATA_REG",
                 "CARD_CTRL_SS_VRAM_ADDR_REG", "CARD_CTRL_SS_SPR_FLAGS_REG",
                 "CARD_CTRL_FEATURE_ENABLE_REG",
                 "CARD_CTRL_FEATURE_SUPERSPRITE_ENABLE_BIT"):
        require(name in regs, f"card_control_regs.h must define {name}")


def test_ps_renderer() -> None:
    h = read(VDP_H)
    require("#define SS_VDP_WIDTH   256" in h and "#define SS_VDP_HEIGHT  192" in h,
            "VDP output is 256x192")
    require("supersprite_vdp_render" in h, "renderer entry point")
    c = read(VDP_C)
    require("k_tms_palette[16]" in c, "16-colour TMS9918 palette")
    require("render_graphics1" in c and "render_graphics2" in c and
            "render_text" in c and "render_multicolor" in c and
            "render_sprites" in c,
            "Graphics I/II, Text, Multicolor and sprites are rendered")
    require("CARD_CTRL_SS_VRAM_ADDR_REG" in c and "CARD_CTRL_SS_VRAM_DATA_REG" in c,
            "VRAM is read through the card-control window")
    require("CARD_CTRL_SS_SPR_FLAGS_REG" in c,
            "sprite coincidence / fifth-sprite flags are written back")
    require("s_last_render_time" in c and "COUNTS_PER_SECOND" in c,
            "VDP render is wall-clock rate-limited (not gated on the PL frame counter)")
    require("r[1] & 0x40" in c, "honour the R1 blanking bit")


def test_compositor_overlay() -> None:
    c = read(COMPOSITOR_C)
    require("draw_supersprite_overlay" in c, "compositor must have the overlay")
    require("supersprite_vdp_render()" in c, "overlay calls the renderer")
    require("0x00FFFFFFu) == 0u" in c,
            "black VDP pixels are transparent (Apple shows through)")
    require("COMP_SUBWIN_X_OFF" in c and "COMP_SUBWIN_WIDTH" in c,
            "overlay is scaled into the Apple subwindow rect")
    require("draw_supersprite_overlay(fb);" in c,
            "overlay is invoked from compositor_tick")


def test_config_menu_smartport_tab() -> None:
    c = read(CONFIG_MENU_C)
    help_source = read(CONFIG_MENU_HELP_C)
    tabs = read(CONFIG_MENU_TABS_C)
    require("CONFIG_TAB_SMARTPORT" in c and '"SmartPort"' in c,
            "SmartPort tab must exist")
    require("HELP(smartport_supersprite," in help_source and
            "disables the SmartPort disk rows" in help_source and
            "OVERRIDE(SMARTPORT_DEVICE_COUNT + 2U, smartport_supersprite)" in help_source and
            "TAB_WITH_OVERRIDES(CONFIG_TAB_SMARTPORT, smartport, smartport_overrides)" in help_source,
            "SmartPort help must explain that enabling SuperSprite disables SmartPort")
    require("config_menu_draw_smartport" in c and "config_menu_draw_smartport" in tabs,
            "SmartPort draw function must be defined and dispatched")
    require("menu->supersprite_enabled" in tabs and
            '"SuperSprite VDP + PSG (Slot 7, disables SmartPort)"' in tabs and
            '"RAM32: 32MB volatile ram disk"' in tabs and
            "SMARTPORT_DEVICE_COUNT + 3) * row_h" in tabs and
            "hgr_draw_item_dimmed" in tabs and
            "hgr_draw_check_item_dimmed" in tabs,
            "SuperSprite checkbox must be drawn at the SmartPort bottom and dim the rest when enabled")
    require("case CONFIG_TAB_SMARTPORT:" in c and "set_supersprite_enabled" in c and
            "SUPERSPRITE ON - SMARTPORT DISABLED" in c and
            "SUPERSPRITE DISABLES SMARTPORT" in c,
            "SmartPort activation must toggle SuperSprite and block the rest of SmartPort")


def main() -> int:
    tests = [
        test_pl_slot_decode_and_reset,
        test_pl_tms9918_protocol,
        test_pl_status_irq_and_switches,
        test_pl_psg_audio,
        test_apple_top_integration,
        test_ps_header_and_build,
        test_ps_renderer,
        test_compositor_overlay,
        test_config_menu_smartport_tab,
    ]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"PASS {t.__name__}")
        except TestFailure as exc:
            failed += 1
            print(f"FAIL {t.__name__}: {exc}")
        except Exception as exc:  # noqa: BLE001
            failed += 1
            print(f"ERROR {t.__name__}: {exc}")
    if failed:
        print(f"{len(tests) - failed} of {len(tests)} SuperSprite tests passed; "
              f"{failed} failed")
        return 1
    print(f"{len(tests)} SuperSprite tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
