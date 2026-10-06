#!/usr/bin/env python3
"""Compare the standalone RTL pitch adapter with frozen Baseline pitch state."""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from ssi263_host_data import write_tables
from test_ssi263_native_controller import ROOT, HOST, digest, linux_path, run

RTL = ROOT / "hdl/apple/ssi263_native_pitch.sv"
HARNESS = ROOT / "scripts/fixtures/ssi263_native/pitch_parity.cpp"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/test_ssi263_native_pitch")
    args = parser.parse_args()
    out = args.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    tables = out / "tables.txt"
    table_metadata = write_tables(tables)
    command = ["verilator", "--cc", "--exe", "--build", "-j", "4", "--Wall",
               "-Wno-DECLFILENAME", "--top-module", "ssi263_native_pitch", "--Mdir", linux_path(out),
               "-CFLAGS", f"-std=c++17 -O2 -I{linux_path(HOST)}", linux_path(RTL), linux_path(HARNESS)]
    run(command, out, out / "compile.log")
    output = run([linux_path(out / "Vssi263_native_pitch"), linux_path(tables)], out, out / "simulation.log")
    result = next(json.loads(line) for line in output.splitlines() if line.startswith('{"status"'))
    result.update({"source_sha256": {str(path.relative_to(ROOT)).replace('\\', '/'): digest(path)
                                    for path in (RTL, HARNESS, HOST / "baseline.h")},
                   "tables": table_metadata,
                   "coverage": ["all 4096 inflection words in all four functions", "all 8 glide steps",
                                "all 32 x 32 active/target field pairs in both directions",
                                "write-before-sample ordering", "no-sample holds", "CTL/function changes",
                                "cold seed and retained glide seed", "FF alias writes", "deterministic random traffic"],
                   "physical_ssi_behavior_verified": False})
    (out / "validation.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"Native pitch: {result['observations']:,} observations, {result['checks']:,} exact state checks")


if __name__ == "__main__":
    main()
