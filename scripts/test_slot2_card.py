#!/usr/bin/env python3
"""Simulate selectable slot-2 cards and a relocatable Phasor."""

from __future__ import annotations

import re
import shutil
from pathlib import Path

from test_vtw_ssi263_bus import ROM, SOURCES as PHASOR_SOURCES
from test_w65c02_core import run, vivado_tool


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "slot2_card"
SNAPSHOT = "tb_slot2_card_snapshot"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def integration_checks() -> None:
    sources = (ROOT / "hdl" / "hdl_sources.txt").read_text(encoding="utf-8")
    for name in ("apple/slot2_gamepad_card.sv", "apple/slot2_card.sv"):
        require(name in sources.splitlines(), f"HDL source list omits {name}")
    source = (ROOT / "hdl" / "apple" / "apple_top.sv").read_text(encoding="utf-8")
    source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.DOTALL)
    compact = re.sub(r"\s+", "", source)
    wrapper = compact.split("slot2_cardslot2_card_i(", 1)[1].split(");", 1)[0]
    for connection in (
        ".resetn(rstn[3])",
        ".card_resetn(rstn[2])",
        ".enabled(card_slot2_enable)",
        ".ab_read(gate_slot_ab(data_phase_ab_read,card_slot2_bus_enable,8'h04,iigs_bus_permissions))",
        ".ps_wr_en(as_client.awvalid)",
        ".ps_rdata(slot2_ps_rdata)",
        ".mouse_selected(slot2_mouse_selected)",
        ".phasor_selected(slot2_phasor_selected)",
        ".phasor_location2(phasor_location2)",
        ".phasor_ab_write(phasor_enabled&&phasor_location2?mb1_ab_write:'0)",
        ".mouse_ab_read(mouse_ab_read)",
        ".mouse_ab_write(mouse_ab_write)",
        ".ab_write(slot2_ab_write)",
    ):
        require(connection in wrapper, f"slot-2 wrapper lacks {connection}")
    require(source.count("mockingboard mb") == 1, "expected one Phasor core")
    phasor = compact.split("mockingboardmb1(", 1)[1].split(");", 1)[0]
    for connection in (
        ".rstn(phasor_enabled)",
        ".slot_assign(phasor_location2?3'd2:3'd4)",
        "phasor_location2?8'h04:8'h10,iigs_bus_permissions)",
        ".iigs_data_tick(native_gs_active&&physical_data_tick&&phasor_enabled)",
    ):
        require(connection in phasor, f"single Phasor lacks {connection}")
    mouse = compact.split("mouse_cardmouse_card_i(", 1)[1].split(");", 1)[0]
    for connection in (
        ".rstn(slot2_mouse_selected)",
        ".ab_read(mouse_ab_read)",
        ".ab_write(mouse_ab_write)",
        ".as_client(mouse_as_client)",
        ".slot_assign(3'd2)",
    ):
        require(connection in mouse, f"top-level Mouse lacks {connection}")
    slot2 = (ROOT / "hdl/apple/slot2_card.sv").read_text(encoding="utf-8")
    require("mouse_card mouse_card_i" not in slot2,
            "Mouse must retain its original apple_top instance hierarchy")
    for name, offset in (("CONTROL", "AD"), ("STATE_LO", "AE"), ("STATE_HI", "AF")):
        require(f"CARD_CTRL_REG_SLOT2_{name}=8'h{offset};" in compact,
                f"slot-2 {name} must decode CARD_CTRL {offset}")
    require(
        "CARD_CTRL_REG_SLOT2_CONTROL,CARD_CTRL_REG_SLOT2_STATE_LO,"
        "CARD_CTRL_REG_SLOT2_STATE_HI:as_client_rdata_q<=slot2_ps_rdata;" in compact,
        "slot-2 PS registers must use the registered CARD_CTRL read mux",
    )
    clients = re.findall(r"\.client_writes\(\{([^}]*)\}\)", compact)
    require(len(clients) == 2, "expected physical policy and virtual client lists")
    for client_list in clients:
        require(re.search(r"\bslot2_ab_write\b", client_list) is not None,
                "an arbiter client list omits the selected slot-2 output")
        require("mouse_ab_write" not in client_list,
                "an arbiter bypasses slot-2 selection with raw Mouse output")
    require("APPLE_BUS_CLIENT_COUNT=13;" in compact,
            "slot-2 cards must share the existing arbiter client")
    require(re.search(r"\(\(?card_slot2_bus_enable&&\(slot2_mouse_selected\|\|slot2_phasor_selected\)\)?"
                      r"\?8'h04:8'h00\)", compact) is not None,
            "the no-slot clock must claim slot 2 for enabled Mouse or Phasor")
    print("PASS slot-2 top-level integration checks")


def extract_phasor_routing() -> Path:
    source = (ROOT / "hdl/apple/apple_top.sv").read_text(encoding="utf-8")
    start = source.index("    logic phasor_location2_q")
    end = source.index("    wire [7:0]", start)
    gates = source[start:end].replace("wire phasor_enabled =", "assign phasor_enabled =")
    reply_start = source.index("    wire globals::AppleBus_write phasor_slot4_reply")
    reply_end = source.index(";", reply_start) + 1
    mixer_start = source.index("    wire signed [16:0] ss_mix_l")
    end_marker = "    assign mockingboard_audio_r = mockingboard_audio_r_q;"
    mixer_end = source.index(end_marker, mixer_start) + len(end_marker)
    routing = OUT_DIR / "single_phasor_routing.sv"
    routing.write_text(
        """module single_phasor_routing(
    input logic clk,
    input logic [2:0] rstn,
    input logic phasor_location2, slot2_phasor_selected,
    input logic card_slot4_bus_enable, card_slot2_bus_enable,
    input globals::AppleBus_write mb1_ab_write,
    input logic signed [15:0] mb1_audio_l, mb1_audio_r, ss_psg_audio,
    output logic phasor_enabled,
    output globals::AppleBus_write slot4_reply,
    output wire signed [15:0] mockingboard_audio_l, mockingboard_audio_r
);
""" + gates + source[reply_start:reply_end] + "\n    assign slot4_reply = phasor_slot4_reply;\n" +
        source[mixer_start:mixer_end] + "\nendmodule\n", encoding="utf-8")
    return routing


def main() -> int:
    integration_checks()
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(
        ROOT / "hdl" / "apple" / "mouse_card_slot2.mem",
        OUT_DIR / "mouse_card_slot2.mem",
    )
    shutil.copyfile(ROOT / ROM, OUT_DIR / Path(ROM).name)
    sources = [
        *(path for path in PHASOR_SOURCES if path not in {
            "hdl/globals.sv", "hdl/apple/apple_bus_write_arbiter.sv",
            "hdl/sim/tb_vtw_ssi263_bus.sv",
        }),
        "hdl/globals.sv",
        "hdl/apple/apple_bus_write_arbiter.sv",
        "hdl/apple/mouse_card.sv",
        "hdl/apple/slot2_gamepad_card.sv",
        "hdl/apple/slot2_card.sv",
        "hdl/sim/tb_slot2_card.sv",
    ]
    run(
        [vivado_tool("xvlog"), "--sv", *[str(ROOT / path) for path in sources],
         str(extract_phasor_routing())],
        OUT_DIR, OUT_DIR / "xvlog.log",
    )
    run(
        [vivado_tool("xelab"), "tb_slot2_card", "-s", SNAPSHOT,
         "--timescale", "1ns/1ps", "-L", "unisims_ver"],
        OUT_DIR, OUT_DIR / "xelab.log",
    )
    output = run(
        [vivado_tool("xsim"), SNAPSHOT, "--runall"],
        OUT_DIR, OUT_DIR / "xsim.log",
    )
    if "SLOT2 CARD PASS" not in output or "FAIL:" in output:
        print(output)
        raise RuntimeError("slot-2 card simulation did not pass")
    print("PASS slot-2 register, gamepad, Mouse, and relocatable Phasor checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
