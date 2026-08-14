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
PASS_MARKER = "SSI263 FILTER FINALIZE PASS"


def static_checks() -> bool:
    source = BACKEND.read_text(encoding="utf-8")
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
