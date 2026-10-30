#!/usr/bin/env python3
"""Build an audited full-hardware TEST candidate from an explicit timing run.

Recreates the normal Vitis workspace (FSBL/BSPs, CPU1 and CPU0). Does not run
Vivado, rebuild/package Golden, modify published images, flash or publish.
Dirty source state is recorded truthfully; timing gates are never changed.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import uuid
import zipfile

from audit_firmware_release import check_timing
from frontend_memory import audit_frontend_elf
from package_timing_firmware import BUILD_ID_RE, read_manifest
from vitis_app_build import verify_workspace_receipt

ROOT = Path(__file__).resolve().parents[1]
BOOT_SHA = "6382353ddd83d3f4fa3f4d447acac741b7d76a06501cb35042b69d190914222c"
PL_FILES = ("appletini_yarz_top.bit", "appletini_yarz_top.xsa", "candidate.dcp")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8", newline="\n")


def arm_sources():
    return {p.relative_to(ROOT).as_posix(): sha(p) for tree in ("ps_sources", "third_party")
            for p in sorted((ROOT / tree).rglob("*")) if p.is_file() and p.suffix.lower() in (".c", ".h", ".s")}


def check_hashes(values):
    for name, expected in values.items():
        path = (ROOT / name).resolve()
        if not path.is_relative_to(ROOT) or not path.is_file() or sha(path) != expected:
            raise RuntimeError(f"Source/preserved input changed: {name}")


def run(command, log, env):
    with log.open("w", encoding="utf-8") as stream:
        result = subprocess.run(command, cwd=ROOT, env=env, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}); see {log}")


def audit_command(out, version, boot, output=None):
    i = out / "inputs"
    command = [sys.executable, str(ROOT / "scripts/audit_firmware_release.py"),
               "--firmware", str(out / "FIRMWARE.BIN"), "--fsbl", str(i / "fsbl.elf"),
               "--frontend", str(i / "frontend.elf"), "--bitstream", str(i / PL_FILES[0]),
               "--xsa", str(i / PL_FILES[1]), "--candidate-dcp", str(i / PL_FILES[2]),
               "--run-manifest", str(out / "TIMING_MANIFEST.txt"),
               "--core1-elf", str(i / "frontend_core1.elf"), "--core1-blob", str(i / "core1_blob.bin"),
               "--version", version, "--boot-version", boot, "--output", str(output or out / "FIRMWARE_AUDIT.json")]
    for path in sorted(i.glob("platform-*.xsa")):
        command += ["--platform-xsa", str(path)]
    return command


def validate_run(run_dir):
    if not run_dir.is_relative_to(ROOT / ".timing_runs") or not BUILD_ID_RE.fullmatch(run_dir.name):
        raise RuntimeError("Select an explicit project-owned full timing run")
    manifest = read_manifest(run_dir / "manifest.txt")
    if manifest.get("build_id") != run_dir.name or manifest.get("rescue_used") != "0":
        raise RuntimeError("Timing run identity/rescue policy mismatch")
    if manifest.get("git_dirty") not in ("0", "1"):
        raise RuntimeError("Timing manifest must record its actual Git dirty state")
    if float(manifest["minimum_wns_ns"]) < 0.050:
        raise RuntimeError("This full-build helper requires the standard >=0.050 ns setup gate")
    # This validates timing and all new PL artifact hashes without altering the
    # manifest or pretending that the source tree was clean.
    check_timing(run_dir / "manifest.txt", (run_dir / PL_FILES[0]).read_bytes(),
                 (run_dir / PL_FILES[1]).read_bytes(), (run_dir / PL_FILES[2]).read_bytes())
    with zipfile.ZipFile(run_dir / PL_FILES[1]) as archive:
        bits = [n for n in archive.namelist() if n.endswith(".bit")]
        if len(bits) != 1 or archive.read(bits[0]) != (run_dir / PL_FILES[0]).read_bytes():
            raise RuntimeError("The selected XSA embeds a different bitstream")
    return manifest


def capture_generated(out):
    workspace = ROOT / "vitis_workspace"
    compiled = {}
    config = out / "build-config"
    config.mkdir(exist_ok=True)
    for name in ("bootloader", "frontend_core1", "frontend"):
        app = workspace / name
        for relative in ("src/UserConfig.cmake", "src/lscript.ld", "build/compile_commands.json",
                         "build/CMakeCache.txt", "build/build.ninja", "build/CMakeFiles/rules.ninja",
                         f"build/CMakeFiles/{name}.elf.rsp", "build/VITIS_API_BUILD.json",
                         "build/CMAKE_VERIFY.log", "build/DIRECT_LINK_COMMAND.json", "build/DIRECT_LINK.log", f"build/{name}.elf.size"):
            path = app / relative
            if path.is_file():
                target = config / name / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, target)
    for commands in workspace.rglob("compile_commands.json"):
        for entry in json.loads(commands.read_text()):
            path = Path(entry["file"])
            if not path.is_absolute():
                path = Path(entry["directory"]) / path
            path = path.resolve()
            if path.is_file():
                label = path.relative_to(ROOT).as_posix() if path.is_relative_to(ROOT) else str(path)
                compiled[label] = sha(path)
    generated = {p.relative_to(ROOT).as_posix(): sha(p) for p in workspace.rglob("*")
                 if p.is_file() and p.suffix.lower() in (".c", ".h", ".s", ".ld", ".cmake", ".yaml", ".spec")}
    with zipfile.ZipFile(out / "GENERATED-INPUTS.zip", "w", zipfile.ZIP_DEFLATED) as archive:
        for name in sorted(generated):
            archive.write(ROOT / name, name)
    write_json(out / "GENERATED_INPUT_HASHES.json", generated)
    return compiled


def source_members(report):
    # Ordinary project files only; exclude Git/build internals and SD seeds.
    members = set(report["sources"]) | set(report["fpga_source_hashes"])
    for tree in ("ps_sources", "third_party", "scripts", "docs", "assets", "software/https_setup"):
        folder = ROOT / tree
        if folder.is_dir():
            members.update(p.relative_to(ROOT).as_posix() for p in folder.rglob("*")
                           if p.is_file() and not any(part in (".git", "__pycache__", "build", "node_modules") for part in p.relative_to(folder).parts))
    members.update(p.name for pattern in ("README*.md", "LICENSE*") for p in ROOT.glob(pattern) if p.is_file())
    return {n for n in members if Path(n).suffix.lower() not in (".o", ".obj", ".elf", ".pyc", ".zip", ".sed")
            and Path(n).name.upper() not in ("ATNSEEDA.BIN", "ATNSEEDB.BIN", "ATNETA.SED", "ATNETB.SED")}


def finish(out, args, report):
    if arm_sources() != report["sources"] or sha(out / "FIRMWARE.BIN") != report["package"]["sha256"]:
        raise RuntimeError("Compiled source/image snapshot changed")
    check_hashes(report["fpga_source_hashes"])
    check_hashes(report["build_tool_hashes"])
    check_hashes(report["preserved_inputs"])
    audit = json.loads((out / "FIRMWARE_AUDIT.json").read_text())
    if audit["status"] != "PASS" or audit["version"] != args.version:
        raise RuntimeError("Passing exact-version image audit required")
    notes = ROOT / "docs/releases" / (args.version + ".md")
    shutil.copy2(notes, out / "RELEASE_NOTES.md")
    validation = out / "validation"
    validation.mkdir(exist_ok=True)
    for supplied in args.report:
        path = Path(supplied).resolve()
        if not path.is_file():
            raise RuntimeError(f"Missing evidence: {path}")
        target = validation / path.parent.name / path.name
        target.parent.mkdir(exist_ok=True)
        if target.exists() and target.read_bytes() != path.read_bytes():
            raise RuntimeError(f"Evidence filename collision: {path}")
        if path != target:
            shutil.copy2(path, target)
    members = source_members(report)
    inventory = {name: {"bytes": (ROOT / name).stat().st_size, "sha256": sha(ROOT / name)} for name in sorted(members)}
    with zipfile.ZipFile(out / "ARM-SOURCES.zip", "w", zipfile.ZIP_DEFLATED) as archive:
        for name in inventory:
            archive.write(ROOT / name, name)
    with zipfile.ZipFile(out / "ARM-SOURCES.zip") as archive:
        if archive.testzip() or any(hashlib.sha256(archive.read(n)).hexdigest() != item["sha256"] for n, item in inventory.items()):
            raise RuntimeError("Source archive verification failed")
    write_json(out / "SOURCE_TEST_HASHES.json", inventory)
    (out / "README.md").write_text(
        f"# {args.version} full-hardware TEST candidate\n\n"
        "The FPGA, XSA-derived FSBL/BSPs, CPU1 and CPU0 are newly built. Golden Boot remains " + report["boot_version"] + ".\n"
        "This candidate has one full Vivado run and an image audit; it is not a two-run promoted timing release or a board-validated image.\n"
        "The timing manifest preserves the actual Git dirty flag. SOURCE_FREEZE.json and the source archives identify the exact inputs.\n"
        "No flash, publishing, Golden image rebuild or replacement of prior/published firmware was performed.\n\n"
        "FIRMWARE.BIN is the installable candidate. Read RELEASE_NOTES.md and use the normal explicit firmware update confirmation.\n"
        "inputs/ contains copied new PL/ARM artifacts and both generated platform XSAs. ARM-SOURCES.zip holds project sources/tests/docs;\n"
        "GENERATED-INPUTS.zip holds generated BSP/app sources and configuration. ARM_BUILD.json and FIRMWARE_AUDIT.json record build and byte-level checks.\n",
        encoding="utf-8", newline="\n")
    report["source_archive_sha256"] = sha(out / "ARM-SOURCES.zip")
    write_json(out / "ARM_BUILD.json", report)
    runtime = out / ("Appletini-" + args.version + "-full-candidate.zip")
    with zipfile.ZipFile(runtime, "w", zipfile.ZIP_DEFLATED) as archive:
        for name in ("FIRMWARE.BIN", "README.md", "RELEASE_NOTES.md", "FIRMWARE_AUDIT.json", "ARM_BUILD.json"):
            archive.write(out / name, name)
    with zipfile.ZipFile(runtime) as archive:
        if archive.testzip() or archive.read("FIRMWARE.BIN") != (out / "FIRMWARE.BIN").read_bytes():
            raise RuntimeError("Runtime archive verification failed")
    files = {p.relative_to(out).as_posix(): {"bytes": p.stat().st_size, "sha256": sha(p)} for p in sorted(out.rglob("*"))
             if p.is_file() and "scratch" not in p.relative_to(out).parts and p.name not in ("BUILD_PROVENANCE.json", "SHA256SUMS.txt", "PACKAGE_CHECK.json")}
    write_json(out / "BUILD_PROVENANCE.json", dict(report, files=files))
    (out / "SHA256SUMS.txt").write_text("".join(v["sha256"] + "  " + n + "\n" for n, v in files.items()), newline="\n")
    write_json(out / "PACKAGE_CHECK.json", {"status": "PASS", "runtime_zip_sha256": sha(runtime),
               "firmware_sha256": sha(out / "FIRMWARE.BIN"), "source_members_verified": len(inventory),
               "source_zip_sha256": sha(out / "ARM-SOURCES.zip"), "flashed": False, "published": False})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--timing-run", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--vitis-root", type=Path, default=Path(os.environ.get("XILINX_VITIS", r"C:\Xilinx\2025.2\Vitis")))
    parser.add_argument("--report", action="append", default=[])
    parser.add_argument("--resume", action="store_true", help="regenerate after an unfinished build; never replace packaged firmware")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--build-only", action="store_true")
    mode.add_argument("--finalize-only", action="store_true")
    mode.add_argument("--audit-only", action="store_true")
    args = parser.parse_args()
    out, run_dir = args.output.resolve(), args.timing_run.resolve()
    if not re.fullmatch(r"F\d+\.\d+\.\d+", args.version) or not out.is_relative_to(ROOT / "build") or out == ROOT / "build":
        raise RuntimeError("Use a stable F version and an explicit project build/ candidate directory")
    if args.finalize_only or args.audit_only:
        report = json.loads((out / "ARM_BUILD.json").read_text())
        if report["version"] != args.version:
            raise RuntimeError("Version differs from compiled candidate")
        if args.finalize_only:
            finish(out, args, report)
        else:
            if sha(out / "FIRMWARE.BIN") != report["package"]["sha256"]:
                raise RuntimeError("Candidate firmware differs from the original build record")
            rerun = out / ("FIRMWARE_AUDIT_RERUN_" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ") + ".json")
            run(audit_command(out, args.version, report["boot_version"], rerun), rerun.with_suffix(".log"), dict(os.environ))
        return
    if (out / "FIRMWARE.BIN").exists() or ((out / "inputs").exists() and not args.resume):
        raise RuntimeError("Preserve the existing candidate; select a new output or resume an unfinished build")
    manifest = validate_run(run_dir)  # No workspace mutation before PL passes.
    before = arm_sources()
    build_tools = {"scripts/" + n: sha(ROOT / "scripts" / n) for n in (
        "build_full_firmware.py", "vitis_app_build.py", "create_vitis_workspace.py", "http_build_config.py",
        "frontend_memory.py", "make_firmware_bin.bat", "audit_firmware_release.py", "image_manifest.py", "package_timing_firmware.py")}
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    if manifest["git_sha"] != head:
        raise RuntimeError("Git HEAD differs from the selected timing build")
    out.mkdir(parents=True, exist_ok=True)
    freeze_path = out / "SOURCE_FREEZE.json"
    if freeze_path.is_file():
        freeze = json.loads(freeze_path.read_text())
        if freeze["git_head"] != head or freeze["firmware_version"] != args.version or freeze["arm_source_hashes"] != before:
            raise RuntimeError("Source freeze differs from requested compiled inputs")
    else:
        raise RuntimeError("A pre-Vivado SOURCE_FREEZE.json in the output directory is required")
    check_hashes(freeze["fpga_source_hashes"])
    header = (ROOT / "ps_sources/image_versions.h").read_text()
    boot = freeze["boot_version"]
    if f'"{args.version}"' not in header or f'"{boot}"' not in header:
        raise RuntimeError("Version header differs from selected firmware/Golden")
    if boot != "B2.0.6" or not (ROOT / "BOOT.BIN").is_file() or sha(ROOT / "BOOT.BIN") != BOOT_SHA:
        raise RuntimeError("Expected unchanged published Golden B2.0.6 image")
    preserve_path = out / "PRESERVED_INPUT_HASHES.json"
    preserved = json.loads(preserve_path.read_text()) if preserve_path.is_file() else {
        p.relative_to(ROOT).as_posix(): sha(p) for p in [ROOT / "FIRMWARE.BIN", ROOT / "BOOT.BIN", *(ROOT / "firmwares").rglob("FIRMWARE.BIN")] if p.is_file()}
    check_hashes(preserved)
    write_json(preserve_path, preserved)
    inputs = out / "inputs"
    inputs.mkdir(exist_ok=True)
    for name in PL_FILES:
        shutil.copy2(run_dir / name, inputs / name)
    shutil.copy2(run_dir / "manifest.txt", out / "TIMING_MANIFEST.txt")
    pl = out / "pl-evidence"
    pl.mkdir(exist_ok=True)
    for path in run_dir.iterdir():
        if path.is_file() and path.suffix.lower() in (".rpt", ".log", ".txt", ".json"):
            shutil.copy2(path, pl / path.name)
    shutil.copy2(inputs / PL_FILES[1], ROOT / "project" / PL_FILES[1])
    env = {k: v for k, v in os.environ.items() if k.lower() != "path"}
    env.update(XILINX_VITIS=str(args.vitis_root.resolve()), PATH=str(Path(sys.executable).parent) + os.pathsep + str(args.vitis_root / "bin") + os.pathsep + os.environ.get("PATH", ""))
    scratch = out / "scratch"
    scratch.mkdir(exist_ok=True)
    env.update(TEMP=str(scratch), TMP=str(scratch))
    build_token = uuid.uuid4().hex
    env["APPLETINI_VITIS_BUILD_TOKEN"] = build_token
    build_command = [str(args.vitis_root / "bin/vitis.bat"), "-s", str(ROOT / "scripts/create_vitis_workspace.py")]
    run(build_command, out / "VITIS_BUILD.log", env)
    workspace = ROOT / "vitis_workspace"
    completion = verify_workspace_receipt(workspace, sha(inputs / PL_FILES[1]), build_token)
    write_json(out / "VITIS_COMPLETE.json", completion)
    if arm_sources() != before:
        raise RuntimeError("Production sources changed during full Vitis build")
    check_hashes(build_tools)
    fresh = {"fsbl.elf": workspace / "appletini_platform/export/appletini_platform/sw/boot/fsbl.elf",
             "frontend.elf": workspace / "frontend/build/frontend.elf",
             "frontend_core1.elf": workspace / "frontend_core1/build/frontend_core1.elf",
             "core1_blob.bin": workspace / "frontend/src/core1_blob.bin",
             "platform-source.xsa": workspace / "appletini_platform/hw/appletini_yarz_top.xsa",
             "platform-export.xsa": workspace / "appletini_platform/export/appletini_platform/hw/appletini_yarz_top.xsa"}
    for name, path in fresh.items():
        if name.startswith("platform-") and sha(path) != sha(inputs / PL_FILES[1]):
            raise RuntimeError("Generated platform XSA differs from new hardware input")
        shutil.copy2(path, inputs / name)
    memory = audit_frontend_elf(inputs / "frontend.elf")
    compiled = capture_generated(out)
    package = [str(ROOT / "scripts/make_firmware_bin.bat"), str(out / "FIRMWARE.BIN"),
               str(inputs / "fsbl.elf"), str(inputs / PL_FILES[0]), str(inputs / "frontend.elf")]
    run(package, out / "PACKAGE.log", env)
    check_hashes(freeze["fpga_source_hashes"])
    check_hashes(preserved)
    if arm_sources() != before:
        raise RuntimeError("Production source snapshot changed while packaging")
    report = dict(kind="full-hardware TEST candidate", version=args.version, boot_version=boot,
                  built_utc=datetime.now(timezone.utc).isoformat(), timing_build_id=run_dir.name,
                  git_head=head, timing_git_dirty=manifest["git_dirty"], sources=before,
                  build_tool_hashes=build_tools,
                  fpga_source_hashes=freeze["fpga_source_hashes"], compiled_inputs=compiled,
                  fpga_rebuilt=True, cpu1_rebuilt=True, fsbl_rebuilt=True, golden_rebuilt=False,
                  flashed=False, published=False, build_command=build_command, package_command=package,
                  preserved_inputs=preserved, frontend_memory=memory,
                  package={"sha256": sha(out / "FIRMWARE.BIN"), "bytes": (out / "FIRMWARE.BIN").stat().st_size})
    report["audit_command"] = audit_command(out, args.version, boot)
    write_json(out / "ARM_BUILD.json", report)
    run(report["audit_command"], out / "AUDIT.log", env)
    if not args.build_only:
        finish(out, args, report)
    print(json.dumps(report["package"], indent=2))


if __name__ == "__main__":
    main()
