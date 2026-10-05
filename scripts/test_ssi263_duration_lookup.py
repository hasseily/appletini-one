#!/usr/bin/env python3
"""Prove the production SSI duration lookup against its arithmetic definition."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/ssi263_duration_lookup_sim"
CORE = ROOT / "hdl/apple/sc01a_digital_core.sv"
PACKAGE = ROOT / "hdl/apple/ssi263_formant_pkg.sv"
BENCH = ROOT / "hdl/sim/tb_ssi263_duration_lookup.sv"
PASS = "SSI263 DURATION LOOKUP PASS checks=65536 combinations=64"
FAIL = "SSI263 DURATION LOOKUP FAIL"


def tool(name: str) -> str:
    found = shutil.which(name + ".bat") or shutil.which(name)
    if found is None:
        raise FileNotFoundError(f"Vivado tool not found: {name}")
    return found


def run(command: list[str], directory: Path, log_name: str,
        expect_failure: bool = False) -> str:
    result = subprocess.run(command, cwd=directory, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (directory / log_name).write_text(result.stdout, encoding="utf-8")
    if result.returncode and not expect_failure:
        raise RuntimeError(f"{command[0]} failed; see {directory / log_name}")
    return result.stdout


def simulate(core: Path, directory: Path, expect_failure: bool = False) -> str:
    directory.mkdir(parents=True, exist_ok=True)
    run([tool("xvlog"), "--sv", str(PACKAGE), str(core), str(BENCH)],
        directory, "compile.log")
    run([tool("xelab"), "tb_ssi263_duration_lookup", "-s", "duration_lookup",
         "--timescale", "1ns/1ps"], directory, "elaborate.log")
    return run([tool("xsim"), "duration_lookup", "--runall"],
               directory, "simulate.log", expect_failure)


def main() -> int:
    output = simulate(CORE, OUT)
    if PASS not in output or FAIL in output:
        raise AssertionError(f"Duration lookup did not pass; see {OUT / 'simulate.log'}")

    # Make one wrong entry in an isolated copy. A passing mutant would mean
    # the fixture failed to exercise the actual production function.
    mutation_dir = OUT / "mutation"
    mutation_dir.mkdir(exist_ok=True)
    mutated, count = re.subn(r"(6'h00:\s+duration_high = 6'd)63;",
                            r"\g<1>62;", CORE.read_text(), count=1)
    if count != 1:
        raise AssertionError("Could not create the single-entry negative control")
    mutation_core = mutation_dir / CORE.name
    mutation_core.write_text(mutated, encoding="utf-8")
    negative = simulate(mutation_core, mutation_dir, expect_failure=True)
    if FAIL not in negative or PASS in negative:
        raise AssertionError("Arithmetic oracle missed the wrong duration entry")

    sources = (CORE, PACKAGE, BENCH, Path(__file__).resolve())
    result = {
        "status": "pass",
        "checks": 65536,
        "duration_rate_combinations": 64,
        "ignored_bit_combinations_per_duration_rate": 1024,
        "negative_control": "single incorrect D=0/R=0 entry rejected",
        "source_sha256": {
            path.relative_to(ROOT).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sources
        },
    }
    (OUT / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(PASS + "; single-entry negative control rejected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
