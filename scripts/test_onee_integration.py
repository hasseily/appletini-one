#!/usr/bin/env python3
"""Check and simulate the HDL-only ONE//e integration checkpoint."""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "onee_integration_sim"
APPLE_TOP = ROOT / "hdl" / "apple" / "apple_top.sv"
WRAPPER = ROOT / "hdl" / "apple" / "apple_bus_wrapper.sv"
TOP = ROOT / "hdl" / "appletini_yarz_top.sv"
XDC = ROOT / "hdl" / "constraints" / "appletini_yarz.xdc"
BENCH = ROOT / "hdl" / "sim" / "tb_apple_bus_isolation.sv"


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
    apple_top = APPLE_TOP.read_text(encoding="utf-8")
    wrapper = WRAPPER.read_text(encoding="utf-8")
    top = TOP.read_text(encoding="utf-8")
    xdc = XDC.read_text(encoding="utf-8")

    require(
        re.search(r"CARD_CTRL_REG_ONEE\s*=\s*8'h5B", apple_top) is not None,
        "CARD_CTRL 0x5B must be the ONE//e request/status register",
    )
    require(
        "onee_request_q                  <= 1'b0;" in apple_top,
        "ONE//e request must reset off",
    )
    require(
        "CARD_CTRL_REG_ONEE: begin" in apple_top and
        "{31'b0, onee_request_q}" in apple_top and
        "onee_request_q <= tmp[0];" in apple_top,
        "CARD_CTRL 0x5B write bit 0 must set the manual request",
    )
    write_pos = apple_top.index("CARD_CTRL_REG_ONEE: begin")
    activity_clear_pos = apple_top.index(
        "if (onee_activity_lockout) begin"
    )
    require(
        activity_clear_pos > write_pos and
        "onee_request_q <= 1'b0;" in apple_top[activity_clear_pos:],
        "activity clear must follow and override a same-cycle request write",
    )
    require(
        "CARD_CTRL_REG_ONEE: as_client_rdata_q <= {" in apple_top and
        "8'hE1, 8'h00, 1'b1, 1'b0, ONEE_POWER_SENSE_PRESENT" in apple_top and
        "localparam logic ONEE_POWER_SENSE_PRESENT = 1'b0;" in apple_top,
        "0x5B status must expose build presence and absent power sensing",
    )

    require(
        "onee_mode_safety_guard onee_mode_safety_guard_i" in apple_top,
        "apple_top must instantiate the safety guard",
    )
    for port, signal in (
        ("apple_phi0_raw", "apple_phi0_pin"),
        ("apple_7m_raw", "apple_7m_pin"),
        ("apple_q3_raw", "apple_q3_pin"),
        ("apple_m2sel_raw", "apple_m2sel_pin"),
        ("apple_m2b0_raw", "apple_m2b0_pin"),
        ("apple_devsel_n_raw", "apple_devsel_n_pin"),
        ("apple_reset_n_raw", "apple_res_pin"),
        ("apple_inh_n_raw", "apple_inh_pin"),
        ("apple_irq_n_raw", "apple_irq_pin"),
        ("apple_nmi_n_raw", "apple_nmi_pin"),
        ("apple_rdy_n_raw", "apple_rdy_pin"),
        ("apple_dma_n_raw", "apple_dma_pin"),
    ):
        require(
            re.search(rf"\.{port}\s*\({signal}\)", apple_top) is not None,
            f"guard input {port} must use raw {signal}",
        )
    require(
        ".apple_power_present_raw(1'b0)" in apple_top,
        "this board must explicitly tie absent slot-power sensing low",
    )
    require(
        "assign tini_aux_oe_pin = !physical_bus_isolate;" in apple_top and
        ".tini_aux_oe_pin(a2fpga_oe_n_aux)" in top and
        "assign a2fpga_oe_n_aux = 1'b1;" not in top,
        "ONE//e must disable the bidirectional auxiliary translator",
    )
    for port, signal in (
        ("apple_7m_pin", "a2fpga_7m"),
        ("apple_q3_pin", "a2fpga_q3"),
        ("apple_devsel_n_pin", "a2fpga_devsel_n"),
    ):
        require(
            f".{port}({signal})" in top,
            f"top must route {signal} into apple_top",
        )
    for signal, pull in (
        ("a2fpga_clk", "PULLDOWN"),
        ("a2fpga_7m", "PULLDOWN"),
        ("a2fpga_q3", "PULLDOWN"),
        ("a2fpga_m2b0", "PULLDOWN"),
        ("a2fpga_m2sel", "PULLDOWN"),
        ("a2fpga_devsel_n", "PULLUP"),
    ):
        require(
            re.search(
                rf"PULLTYPE\s+{pull}}}.*\n\s*\[get_ports\s+{signal}\]",
                xdc,
            ) is not None,
            f"open U533 output {signal} must have a safe {pull}",
        )

    require(
        "apple_virtual_bus apple_virtual_bus_i" in apple_top and
        ".req_valid        (1'b0)" in apple_top and
        ".ab_write         (ab_write_arb)" in apple_top,
        "virtual bus must use merged card writes with its CPU request tied off",
    )
    require(
        "assign ab_read  = onee_enable_effective ? virtual_ab_read" in apple_top and
        ": physical_ab_read;" in apple_top,
        "effective ONE//e mode must select the virtual bus record",
    )
    require(
        "assign ab_write = physical_bus_isolate ? '0 : ab_write_arb;" in apple_top,
        "physical wrapper requests must be zero while isolated",
    )
    require(
        ".ab_read(physical_ab_read)" in apple_top and
        ".physical_bus_isolate(physical_bus_isolate_wrapper)" in apple_top and
        "assign physical_bus_isolate_wrapper =" in apple_top and
        "onee_request_q || onee_selected || onee_physical_isolation_hold;" in apple_top,
        "physical wrapper must remain separate and receive an exact private copy of the direct kill",
    )
    require(
        "assign apple_reset_n_out = physical_bus_isolate ? 1'b1 :" in apple_top,
        "dedicated physical RESET must release under isolation",
    )
    require(
        "onee_enable_effective ||" in apple_top and
        "(!physical_bus_isolate && vtw_machine_ok_q)" in apple_top and
        ".host_is_iiplus(vtw_host_is_iiplus_eff)" in apple_top,
        "vTW must run as a virtual //e without bypassing the host safety gate",
    )

    for contract in (
        "ab_write.wr_addr_rw_en &&\n                                !physical_bus_isolate",
        "apple_data_enable_unisolated &&\n                             !physical_bus_isolate",
        "wire apple_irq_drive_low = !physical_bus_isolate",
        "assign apple_inh_pin = (!physical_bus_isolate",
        "wire apple_dma_requested = !physical_bus_isolate",
        "assign tini_oe_pin       = tini_5v_pin;",
    ):
        require(contract in wrapper, f"missing physical isolation gate: {contract}")


def main() -> int:
    static_checks()

    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    OUT_DIR.mkdir(parents=True)
    run(
        [
            vivado_tool("xvlog"),
            "--sv",
            str(ROOT / "hdl" / "globals.sv"),
            str(ROOT / "hdl" / "cdc_bus_sampled.sv"),
            str(WRAPPER),
            str(BENCH),
        ],
        "xvlog.log",
    )
    run(
        [
            vivado_tool("xelab"),
            "tb_apple_bus_isolation",
            "-s",
            "tb_apple_bus_isolation_snap",
            "--timescale",
            "1ns/1ps",
            "-L",
            "unisims_ver",
        ],
        "xelab.log",
    )
    output = run(
        [
            vivado_tool("xsim"),
            "tb_apple_bus_isolation_snap",
            "--runall",
        ],
        "xsim.log",
    )
    require(
        "APPLE BUS ISOLATION PASS" in output,
        "physical isolation simulation did not report PASS",
    )
    print("ONE//e HDL integration checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
