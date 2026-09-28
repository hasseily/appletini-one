#!/usr/bin/env python3
"""Simulate selectable slot-2 Mouse, FourPlay, and SNES MAX cards."""

from __future__ import annotations

import re
import shutil
from pathlib import Path

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
        ".ab_read(gate_ab(data_phase_ab_read,card_slot2_bus_enable))",
        ".ps_wr_en(as_client.awvalid)",
        ".ps_rdata(slot2_ps_rdata)",
        ".mouse_selected(slot2_mouse_selected)",
        ".mouse_ab_read(mouse_ab_read)",
        ".mouse_ab_write(mouse_ab_write)",
        ".ab_write(slot2_ab_write)",
    ):
        require(connection in wrapper, f"slot-2 wrapper lacks {connection}")
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
        require("slot2_ab_write" in client_list.split(","),
                "an arbiter client list omits the selected slot-2 output")
        require("mouse_ab_write" not in client_list,
                "an arbiter bypasses slot-2 selection with raw Mouse output")
    require("APPLE_BUS_CLIENT_COUNT=13;" in compact,
            "slot-2 cards must share the existing arbiter client")
    require(re.search(r"\(\(?card_slot2_bus_enable&&slot2_mouse_selected\)?"
                      r"\?8'h04:8'h00\)", compact) is not None,
            "the no-slot clock must claim slot 2 only for enabled Mouse ROM")
    print("PASS slot-2 top-level integration checks")


def main() -> int:
    integration_checks()
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(
        ROOT / "hdl" / "apple" / "mouse_card_slot2.mem",
        OUT_DIR / "mouse_card_slot2.mem",
    )
    sources = [
        "hdl/globals.sv",
        "hdl/apple/apple_bus_write_arbiter.sv",
        "hdl/apple/mouse_card.sv",
        "hdl/apple/slot2_gamepad_card.sv",
        "hdl/apple/slot2_card.sv",
        "hdl/sim/tb_slot2_card.sv",
    ]
    run(
        [vivado_tool("xvlog"), "--sv", *[str(ROOT / path) for path in sources]],
        OUT_DIR, OUT_DIR / "xvlog.log",
    )
    run(
        [vivado_tool("xelab"), "tb_slot2_card", "-s", SNAPSHOT,
         "--timescale", "1ns/1ps"],
        OUT_DIR, OUT_DIR / "xelab.log",
    )
    output = run(
        [vivado_tool("xsim"), SNAPSHOT, "--runall"],
        OUT_DIR, OUT_DIR / "xsim.log",
    )
    if "SLOT2 CARD PASS" not in output or "FAIL:" in output:
        print(output)
        raise RuntimeError("slot-2 card simulation did not pass")
    print("PASS slot-2 register, FourPlay, SNES MAX, and Mouse selection checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
