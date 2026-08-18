#!/usr/bin/env python3
"""Build and package firmware from one immutable timing run."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import re
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD_ID_RE = re.compile(
    r"^[0-9]{8}T[0-9]{6}Z-[0-9a-f]{8}-full(?:-[0-9]{2})?$"
)


def read_manifest(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise ValueError(f"malformed manifest line in {path}: {raw_line}")
        key, value = line.split("=", 1)
        values[key] = value
    return values


def write_manifest(path: Path, values: dict[str, str]) -> None:
    text = "".join(f"{key}={values[key]}\n" for key in sorted(values))
    path.write_text(text, encoding="utf-8", newline="\n")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def require_value(values: dict[str, str], key: str, expected: str) -> None:
    actual = values.get(key)
    if actual != expected:
        raise ValueError(f"{key} must be {expected!r}, got {actual!r}")


def require_nonnegative(values: dict[str, str], key: str,
                        minimum: float = 0.0) -> None:
    try:
        actual = float(values[key])
    except (KeyError, ValueError) as exc:
        raise ValueError(f"{key} is missing or invalid") from exc
    if actual < minimum:
        raise ValueError(f"{key} must be at least {minimum:.3f}, got {actual}")


def validate_build_manifest(build_id: str, run_dir: Path,
                            values: dict[str, str]) -> None:
    require_value(values, "build_id", build_id)
    require_value(values, "status", "exported")
    require_value(values, "build_mode", "full")
    require_value(values, "git_dirty", "0")
    require_value(values, "route_status", "PASS")
    require_value(values, "bus_skew_status", "PASS")
    require_nonnegative(values, "wns_ns", 0.300)
    require_nonnegative(values, "whs_ns")
    require_nonnegative(values, "wpws_ns")
    for key in (
        "tns_ns", "ths_ns", "tpws_ns",
        "setup_failing_endpoints", "hold_failing_endpoints",
        "pulse_width_failing_endpoints", "unconstrained_internal_endpoints",
        "route_errors", "missing_constraint_objects",
    ):
        require_nonnegative(values, key)
        if float(values[key]) != 0.0:
            raise ValueError(f"{key} must be zero, got {values[key]}")

    artifacts = {
        "candidate_dcp_sha256": run_dir / "candidate.dcp",
        "bitstream_sha256": run_dir / "appletini_yarz_top.bit",
        "xsa_sha256": run_dir / "appletini_yarz_top.xsa",
    }
    for key, path in artifacts.items():
        if not path.is_file():
            raise FileNotFoundError(path)
        actual = sha256_file(path)
        if values.get(key) != actual:
            raise ValueError(f"{path.name} does not match {key}")


def git_output(*args: str) -> str:
    return subprocess.check_output(
        ["git", *args], cwd=ROOT, text=True
    ).strip()


def run_batch(args: list[str]) -> None:
    command = "call " + subprocess.list2cmdline(args)
    subprocess.run(
        ["cmd.exe", "/d", "/c", command], cwd=ROOT, check=True
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_id", help="full timing build ID")
    args = parser.parse_args()

    if not BUILD_ID_RE.fullmatch(args.build_id):
        raise SystemExit(f"invalid full-build ID: {args.build_id}")

    run_dir = ROOT / ".timing_runs" / args.build_id
    manifest_path = run_dir / "manifest.txt"
    firmware_manifest_path = run_dir / "firmware_manifest.txt"
    firmware_path = run_dir / "FIRMWARE.BIN"
    if firmware_manifest_path.exists() or firmware_path.exists():
        raise SystemExit(
            f"firmware already exists for {args.build_id}; refusing to overwrite it"
        )

    manifest = read_manifest(manifest_path)
    validate_build_manifest(args.build_id, run_dir, manifest)
    current_sha = git_output("rev-parse", "HEAD")
    require_value(manifest, "git_sha", current_sha)
    if git_output("status", "--porcelain", "--untracked-files=normal"):
        raise SystemExit("the Git worktree must be clean")

    # Vitis reads this fixed generated path. Replace it with the named run's
    # immutable XSA, then build the whole workspace from that hardware design.
    run_xsa = run_dir / "appletini_yarz_top.xsa"
    project_xsa = ROOT / "project" / "appletini_yarz_top.xsa"
    shutil.copy2(run_xsa, project_xsa)
    if sha256_file(project_xsa) != manifest["xsa_sha256"]:
        raise RuntimeError("current Vitis input XSA does not match the timing run")

    vitis = shutil.which("vitis.bat") or shutil.which("vitis")
    if not vitis:
        raise FileNotFoundError("vitis was not found on PATH")
    run_batch([vitis, "-s", str(ROOT / "scripts" / "create_vitis_workspace.py")])

    fsbl = (ROOT / "vitis_workspace" / "appletini_platform" / "export" /
            "appletini_platform" / "sw" / "boot" / "fsbl.elf")
    frontend = ROOT / "vitis_workspace" / "frontend" / "build" / "frontend.elf"
    core1 = (ROOT / "vitis_workspace" / "frontend_core1" / "build" /
             "frontend_core1.elf")
    for path in (fsbl, frontend, core1):
        if not path.is_file():
            raise FileNotFoundError(path)

    run_batch([
        str(ROOT / "scripts" / "make_firmware_bin.bat"),
        str(firmware_path), str(fsbl),
        str(run_dir / "appletini_yarz_top.bit"), str(frontend),
    ])
    if not firmware_path.is_file():
        raise RuntimeError("firmware packaging did not create FIRMWARE.BIN")
    if git_output("rev-parse", "HEAD") != current_sha or \
            git_output("status", "--porcelain", "--untracked-files=normal"):
        raise RuntimeError("the Git state changed while firmware was built")

    firmware_manifest = {
        "status": "packaged",
        "packaged_utc": dt.datetime.now(dt.timezone.utc).strftime(
            "%Y-%m-%dT%H:%M:%SZ"
        ),
        "build_id": args.build_id,
        "git_sha": current_sha,
        "candidate_dcp_sha256": manifest["candidate_dcp_sha256"],
        "bitstream_sha256": manifest["bitstream_sha256"],
        "xsa_sha256": manifest["xsa_sha256"],
        "fsbl_sha256": sha256_file(fsbl),
        "frontend_elf_sha256": sha256_file(frontend),
        "core1_elf_sha256": sha256_file(core1),
        "firmware_file": firmware_path.name,
        "firmware_sha256": sha256_file(firmware_path),
    }
    write_manifest(firmware_manifest_path, firmware_manifest)
    print(f"Packaged timing firmware: {firmware_path}")
    print(f"Firmware SHA-256: {firmware_manifest['firmware_sha256']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
