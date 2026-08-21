#!/usr/bin/env python3
"""Unit checks for timing-run firmware manifest validation."""

from __future__ import annotations

import copy
import tempfile
from pathlib import Path

import package_timing_firmware as pack


def expect_failure(fn, label: str) -> None:
    try:
        fn()
    except (FileNotFoundError, ValueError):
        return
    raise AssertionError(f"accepted {label}")


def main() -> int:
    build_id = "20260812T180800Z-a3d71d3f-full"
    with tempfile.TemporaryDirectory() as temp_name:
        run_dir = Path(temp_name)
        spaced_dir = run_dir / "batch path with spaces"
        spaced_dir.mkdir()
        batch = spaced_dir / "check argument.cmd"
        batch.write_text(
            '@echo off\nif not "%~1"=="argument with spaces" exit /b 9\n'
            'exit /b 0\n',
            encoding="utf-8",
        )
        pack.run_batch([str(batch), "argument with spaces"])

        artifacts = {
            "candidate_dcp_sha256": run_dir / "candidate.dcp",
            "bitstream_sha256": run_dir / "appletini_yarz_top.bit",
            "xsa_sha256": run_dir / "appletini_yarz_top.xsa",
        }
        for index, path in enumerate(artifacts.values()):
            path.write_bytes(bytes([index]) * 32)

        manifest = {
            "build_id": build_id,
            "status": "exported",
            "build_mode": "full",
            "git_dirty": "0",
            "route_status": "PASS",
            "bus_skew_status": "PASS",
            "wns_ns": "0.300",
            "whs_ns": "0.000",
            "wpws_ns": "0.000",
        }
        for key in (
            "tns_ns", "ths_ns", "tpws_ns",
            "setup_failing_endpoints", "hold_failing_endpoints",
            "pulse_width_failing_endpoints", "unconstrained_internal_endpoints",
            "route_errors", "missing_constraint_objects",
        ):
            manifest[key] = "0.000"
        for key, path in artifacts.items():
            manifest[key] = pack.sha256_file(path)

        pack.validate_build_manifest(build_id, run_dir, manifest)

        low_slack = copy.deepcopy(manifest)
        low_slack["wns_ns"] = "0.299"
        expect_failure(
            lambda: pack.validate_build_manifest(build_id, run_dir, low_slack),
            "low setup margin",
        )

        bad_hash = copy.deepcopy(manifest)
        bad_hash["bitstream_sha256"] = "0" * 64
        expect_failure(
            lambda: pack.validate_build_manifest(build_id, run_dir, bad_hash),
            "bad bitstream hash",
        )

    print("PASS: timing firmware packaging checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
