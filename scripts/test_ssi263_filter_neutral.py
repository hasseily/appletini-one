#!/usr/bin/env python3
"""Compare FF128 with the actual F1.2.2 backend, not a second new backend."""

from __future__ import annotations

import csv
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "ssi263_filter_neutral_sim"
BASELINE = "310193469104b5e07b847631129223467b043cfd"
BACKEND = "hdl/apple/ssi263_formant_backend.sv"
SOURCES = [ROOT / "hdl/apple/ssi263_formant_pkg.sv",
           ROOT / "hdl/apple/sc01a_digital_core.sv", ROOT / BACKEND,
           ROOT / "hdl/sim/tb_ssi263_filter_neutral.sv"]


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def tool(name: str) -> str:
    found = shutil.which(name + ".bat") or shutil.which(name)
    if found is None:
        raise FileNotFoundError(f"Vivado simulation tool not found: {name}")
    return found


def run(command: list[str], log: str, *, expect_failure: bool = False) -> str:
    result = subprocess.run(command, cwd=OUT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    (OUT / log).write_text(result.stdout, encoding="utf-8")
    if result.returncode and not expect_failure:
        raise RuntimeError(f"Simulation command failed; see {OUT / log}\n{result.stdout}")
    return result.stdout


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    baseline = subprocess.check_output(["git", "show", f"{BASELINE}:{BACKEND}"], cwd=ROOT)
    name = b"module ssi263_formant_backend ("
    assert baseline.count(name) == 1, "baseline declaration changed"
    # Preserve every byte of the old implementation except its module name.
    fixture_bytes = baseline.replace(name, b"module ssi263_formant_backend_f122 (", 1)
    fixture = OUT / "ssi263_formant_backend_f122.sv"
    fixture.write_bytes(fixture_bytes)
    package = subprocess.check_output(
        ["git", "show", f"{BASELINE}:hdl/apple/ssi263_formant_pkg.sv"], cwd=ROOT)
    assert package.replace(b"\r\n", b"\n") == SOURCES[0].read_bytes().replace(b"\r\n", b"\n"), \
        "shared coefficient package differs from the F1.2.2 baseline"
    captured = {str(path.relative_to(ROOT)): digest(path.read_bytes())
                for path in [*SOURCES, Path(__file__).resolve()]}
    run([tool("xvlog"), "--sv", *map(str, SOURCES[:3]), str(fixture), str(SOURCES[3])],
        "xvlog.log")
    run([tool("xelab"), "tb_ssi263_filter_neutral", "-s", "neutral",
         "--timescale", "1ns/1ps"], "xelab.log")
    output = run([tool("xsim"), "neutral", "--runall"], "neutral_xsim.log")
    match = re.search(r"SSI263 FILTER NEUTRAL PASS cycles=(\d+) state_checks=(\d+) "
                      r"exact_checks=(\d+) frames=(\d+) nonzero=(\d+) responses=(\d+) done=(\d+)",
                      output)
    if match is None or "SSI263 FILTER NEUTRAL FAIL" in output:
        raise AssertionError(f"Actual F1.2.2 comparison failed; see {OUT / 'neutral_xsim.log'}")
    waveform = OUT / "neutral_waveform.csv"
    waveform_bytes = waveform.read_bytes()
    exact_samples = 0
    with waveform.open(newline="") as handle:
        for _, ff, votrax, sample, reference in csv.reader(handle):
            if int(ff) == 128 or int(votrax):
                assert sample == reference, "captured legacy PCM differs"
                exact_samples += 1
    # Prove the oracle is sensitive to a one-code error in neutral selection.
    mutant = run([tool("xsim"), "neutral", "--runall", "-testplusarg", "wrong_neutral"],
                 "wrong_neutral_xsim.log", expect_failure=True)
    if "SSI263 FILTER NEUTRAL FAIL:" not in mutant or "SSI263 FILTER NEUTRAL PASS" in mutant:
        raise AssertionError("FF127 negative control did not fail")
    waveform.write_bytes(waveform_bytes)
    for relative, sha256 in captured.items():
        assert digest((ROOT / relative).read_bytes()) == sha256, f"Source changed: {relative}"
    summary = {
        "status": "PASS", "baseline_commit": BASELINE,
        "baseline_backend_sha256": digest(baseline),
        "renamed_fixture_sha256": digest(fixture_bytes),
        "fixture_changes": "module name only",
        "shared_core_note": "Current digital core; duration lookup separately qualified against legacy arithmetic.",
        "coefficient_package_matches_baseline": True,
        "neutral_ff": 128, "wrong_neutral_127_rejected": True,
        **dict(zip(("cycles", "state_checks", "exact_cycle_checks", "frames", "nonzero_frames",
                    "responses", "completions"), map(int, match.groups()))),
        "exact_pcm_samples": exact_samples,
        "waveform_sha256": digest(waveform_bytes), "source_sha256": captured,
    }
    (OUT / "results.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
