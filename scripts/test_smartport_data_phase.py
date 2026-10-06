#!/usr/bin/env python3
"""Compare frozen/current top routing with real SmartPort and overlay consumers."""
from pathlib import Path
import json
import re
import shutil
from test_w65c02_core import run, vivado_tool

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/smartport_data_phase"


def extract_adapter(text: str, name: str) -> str:
    gate = re.search(r"    function automatic globals::AppleBus_read gate_ab\(.*?    endfunction", text, re.S)
    data = re.search(r"    always_comb begin\s+data_phase_ab_read = ab_read;.*?    end", text, re.S)
    start = text.index("    globals::AppleBus_read smartport_ab_read;")
    stop = text.index("    smartport_card smartport_card_i (", start)
    body = text[start:stop].replace("    globals::AppleBus_read smartport_ab_read;", "", 1)
    if not gate or not data:
        raise RuntimeError("Top-level routing extraction failed")
    return f"""module {name} (
    input globals::AppleBus_read ab_read, physical_ab_read, slot7_devsel_ab_read,
    input logic onee_enable_effective, vtw_smartport_visible, slot7_overlay_devsel_visible,
    input logic [7:0] virtual_data_phase_data,
    output globals::AppleBus_read smartport_ab_read
);
    globals::AppleBus_read data_phase_ab_read;
{gate[0]}
{data[0]}
{body}
endmodule
"""


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    baseline = ROOT / "scripts/fixtures/smartport_data_phase/baseline_routing.sv"
    adapters = "".join((
        baseline.read_text(),
        extract_adapter((ROOT / "hdl/apple/apple_top.sv").read_text(), "smartport_routing_candidate"),
    ))
    (OUT / "routing_extracted.sv").write_text(adapters)
    for name in ("smartport_a2retronet_style_c700.mem", "smartport_a2retronet_style_c800.mem"):
        shutil.copyfile(ROOT / "hdl/apple" / name, OUT / name)
    sources = [ROOT / f for f in (
        "hdl/globals.sv", "hdl/apple/apple_virtual_bus.sv", "hdl/apple/apple_slot7_devsel_guard.sv",
        "hdl/apple/linear_text_overlay_card.sv", "hdl/apple/smartport_card.sv",
    )] + [OUT / "routing_extracted.sv", ROOT / "hdl/sim/tb_smartport_data_phase.sv"]
    run([vivado_tool("xvlog"), "--sv", *map(str, sources)], OUT, OUT / "xvlog.log")
    run([vivado_tool("xelab"), "tb_smartport_data_phase", "-s", "smartport_data_phase",
         "--timescale", "1ns/1ps", "-L", "unisims_ver"], OUT, OUT / "xelab.log")
    output = run([vivado_tool("xsim"), "smartport_data_phase", "--runall"], OUT, OUT / "xsim.log")
    result = re.search(r"SMARTPORT DATA PHASE PASS[^\r\n]*", output)
    if result is None or "FAIL:" in output:
        print(output)
        raise RuntimeError("SmartPort data-phase equivalence failed")
    (OUT / "result.json").write_text(json.dumps({"status": "PASS", "result": result[0]}, indent=2)+"\n")
    print(result[0])


if __name__ == "__main__":
    main()
