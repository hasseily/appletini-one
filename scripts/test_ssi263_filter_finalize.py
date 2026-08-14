#!/usr/bin/env python3
"""Build and run focused SSI263 filter-finalize pipeline checks."""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "ssi263_filter_finalize_sim"
BACKEND = ROOT / "hdl" / "apple" / "ssi263_formant_backend.sv"
CONSTRAINTS_DIR = ROOT / "hdl" / "constraints"
PASS_MARKER = "SSI263 FILTER FINALIZE PASS"


def tcl_commands(source: str) -> list[str]:
    """Return uncommented Tcl commands with backslash continuations joined."""
    commands: list[str] = []
    pending: list[str] = []
    for raw_line in source.splitlines():
        line = raw_line.split("#", 1)[0].rstrip()
        if not line and not pending:
            continue
        continued = line.endswith("\\")
        if continued:
            line = line[:-1].rstrip()
        if line:
            pending.append(line)
        if not continued and pending:
            commands.append(" ".join(pending))
            pending = []
    if pending:
        commands.append(" ".join(pending))
    return commands


def check_local_hard_reset_decode(source: str) -> None:
    reset_name = "formant_hard_reset_local"
    instance_name = "formant_hard_reset_local_i"

    if source.count(reset_name) != 4:
        raise RuntimeError(
            f"{reset_name} must have only its declaration, LUT instance, "
            "LUT output, and outer reset-condition references"
        )
    if not re.search(rf"\bwire\s+{reset_name}\s*;", source):
        raise RuntimeError(f"{reset_name} must remain a combinational wire")

    lut_contract = re.compile(
        r'\(\*\s*KEEP\s*=\s*"TRUE"\s*,\s*'
        r'DONT_TOUCH\s*=\s*"TRUE"\s*\*\)\s*'
        r"LUT1\s*#\s*\(\s*\.INIT\s*\(\s*2'h1\s*\)\s*\)\s*"
        rf"{instance_name}\s*\(\s*"
        r"\.I0\s*\(\s*rstn\s*\)\s*,\s*"
        rf"\.O\s*\(\s*{reset_name}\s*\)\s*"
        r"\)\s*;",
        re.DOTALL,
    )
    if not lut_contract.search(source):
        raise RuntimeError(
            "local formant hard reset must be the preserved stable-name "
            "LUT1 INIT=2'h1 decode of raw rstn"
        )

    reset_priority = re.compile(
        rf"if\s*\(\s*{reset_name}\s*\|\|\s*!\s*card_enabled\s*\)\s*"
        r"begin\s*reset_power_state\s*\(\s*\)\s*;\s*end\s*"
        r"else\s+if\s*\(\s*warm_reset\s*\)",
        re.DOTALL,
    )
    if not reset_priority.search(source):
        raise RuntimeError(
            "outer backend hard/card reset must use the local decode at "
            "first priority ahead of warm reset"
        )
    if re.search(r"if\s*\(\s*!\s*rstn\s*\|\|\s*!\s*card_enabled", source):
        raise RuntimeError("outer backend reset still bypasses the local decode")

    core_match = re.search(
        r"sc01a_digital_core\s+digital_core_i\s*\((.*?)\n\s*\);",
        source,
        re.DOTALL,
    )
    if not core_match:
        raise RuntimeError("missing digital_core_i instance")
    core_ports = core_match.group(1)
    if len(re.findall(r"\.rstn\s*\(\s*rstn\s*\)", core_ports)) != 1:
        raise RuntimeError("digital_core_i.rstn must remain wired to raw rstn")
    if len(
        re.findall(
            r"\.reset\s*\(\s*!\s*card_enabled\s*\|\|\s*warm_reset\s*\)",
            core_ports,
        )
    ) != 1:
        raise RuntimeError(
            "digital_core_i.reset must remain card-disable or warm-reset"
        )
    if reset_name in core_ports:
        raise RuntimeError("local outer-backend reset must not feed digital_core_i")

    forbidden_source = (
        r"\bformant_hard_reset\w*_q\b",
        rf"\b{reset_name}\s*<=",
        r"\bMAX_FANOUT\b",
        rf"\bLOC\b[^\n]*\b{reset_name}\b",
        rf"\b{reset_name}\b[^\n]*\bLOC\b",
    )
    for pattern in forbidden_source:
        if re.search(pattern, source, re.IGNORECASE):
            raise RuntimeError(
                "local formant hard reset must not use a copy register, "
                "MAX_FANOUT, or RTL placement attribute"
            )

    timing_commands = re.compile(
        r"^set_(?:false_path|max_delay|min_delay|multicycle_path)\b",
        re.IGNORECASE,
    )
    reset_targets = re.compile(
        rf"{reset_name}|{instance_name}|ssi263|formant_backend",
        re.IGNORECASE,
    )
    for xdc in CONSTRAINTS_DIR.rglob("*.xdc"):
        for command in tcl_commands(xdc.read_text(encoding="utf-8")):
            if reset_name.lower() in command.lower():
                raise RuntimeError(
                    f"local formant reset must have no XDC property or "
                    f"timing exception: {xdc}: {command}"
                )
            if timing_commands.search(command) and reset_targets.search(command):
                raise RuntimeError(
                    f"SSI263/formant reset timing exception is forbidden: "
                    f"{xdc}: {command}"
                )


def static_checks() -> bool:
    source = BACKEND.read_text(encoding="utf-8")
    check_local_hard_reset_decode(source)
    has_finalize = "SYNTH_FILTER_FINALIZE" in source

    if has_finalize:
        if not re.search(
            r"SYNTH_OUT\s*,\s*SYNTH_FILTER_FINALIZE\s*\n\s*}",
            source,
        ):
            raise RuntimeError(
                "SYNTH_FILTER_FINALIZE must be appended after SYNTH_OUT so "
                "all old state codes stay fixed"
            )
        required = (
            "mac_accum_q <= mac_next;",
            "synth_state_q <= SYNTH_FILTER_FINALIZE;",
            "SYNTH_FILTER_FINALIZE: begin",
            "mac_accum_q <= 56'sd0;",
            "case (filter_stage_q)",
        )
        for text in required:
            if text not in source:
                raise RuntimeError(f"missing filter-finalize contract: {text}")
        if not re.search(
            r"filter_out\s*=\s*sat24_from56\(\s*"
            r"mac_accum_q\s*>>>\s*SC01_COEFF_FRAC_BITS\s*\)\s*;",
            source,
        ):
            raise RuntimeError(
                "SYNTH_FILTER_FINALIZE must saturate the saved accumulator"
            )

        accum_at = source.index("SYNTH_FILTER_ACCUM: begin")
        finalize_at = source.index("SYNTH_FILTER_FINALIZE: begin", accum_at)
        accum_block = source[accum_at:finalize_at]
        if "filter_out = sat24_from56(mac_next" in accum_block:
            raise RuntimeError("last ACCUM still saturates before finalization")
    else:
        if "filter_out = sat24_from56(mac_next >>> SC01_COEFF_FRAC_BITS);" not in source:
            raise RuntimeError("unmodified filter ACCUM reference behavior is missing")

    return has_finalize


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
    output = completed.stdout or ""
    (OUT_DIR / log_name).write_text(output, encoding="utf-8")
    if completed.returncode != 0:
        print(output)
        raise RuntimeError(
            f"{Path(command[0]).name} failed with {completed.returncode}; "
            f"see {OUT_DIR / log_name}"
        )
    return output


def main() -> int:
    has_finalize = static_checks()
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    xvlog_command = [vivado_tool("xvlog"), "--sv"]
    if has_finalize:
        xvlog_command.extend(["--define", "SSI263_HAS_FILTER_FINALIZE"])
    xvlog_command.extend(
        [
            str(ROOT / "hdl" / "apple" / "ssi263_formant_pkg.sv"),
            str(ROOT / "hdl" / "apple" / "sc01a_digital_core.sv"),
            str(BACKEND),
            str(ROOT / "hdl" / "sim" / "tb_ssi263_filter_finalize.sv"),
        ]
    )
    run(xvlog_command, "xvlog.log")
    run(
        [
            vivado_tool("xelab"),
            "tb_ssi263_filter_finalize",
            "-s",
            "tb_ssi263_filter_finalize_snap",
            "--timescale",
            "1ns/1ps",
            "-L",
            "unisims_ver",
        ],
        "xelab.log",
    )
    output = run(
        [vivado_tool("xsim"), "tb_ssi263_filter_finalize_snap", "--runall"],
        "xsim.log",
    )
    if PASS_MARKER not in output or "SSI263 FILTER FINALIZE FAIL" in output:
        print(output)
        raise RuntimeError("SSI263 filter-finalize test did not pass")

    print(next(line for line in output.splitlines() if PASS_MARKER in line))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
