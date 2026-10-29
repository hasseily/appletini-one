#!/usr/bin/env python3
"""Build and package F1.3.4 using the retained qualified FPGA/CPU1 inputs."""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import zipfile
from http_build_config import configure_http_service
from package_firmware_f134 import finish_release

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "firmwares/firmware-F1.3.3"
OUT = ROOT / "firmwares/firmware-F1.3.4"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args, log, env):
    with (OUT / log).open("w", encoding="utf-8") as stream:
        result = subprocess.run(args, cwd=ROOT, env=env, stdout=stream,
                                stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}); see {OUT / log}")


def main():
    if (OUT / "FIRMWARE.BIN").exists() or (OUT / "inputs").exists():
        raise RuntimeError(f"Preserve existing release: {OUT}")
    baseline = json.loads((BASE / "ARM_BUILD.json").read_text())
    added = ["ps_sources/frontend/" + name for name in
             ("http_entropy.c", "http_entropy.h", "http_roots.c", "http_roots.h",
              "http_download.c", "http_download.h", "download_filename.c", "download_filename.h")]
    before = {name: sha(ROOT / name) for name in [*baseline["sources"], *added]}
    changes = [name for name in baseline["sources"] if before[name] != baseline["sources"][name]]
    allowed = {"ps_sources/frontend/" + name for name in
               ("http_core.c", "http_core.h", "http_service.c", "http_service.h", "main.c",
                "net_service.c", "net_service.h", "config_menu.c", "printer_service.c",
                "http_service_mbedtls_config.h", "https_probe_mbedtls_config.h")}
    allowed.add("ps_sources/image_versions.h")
    if not set(changes) <= allowed or not {"ps_sources/frontend/http_core.h", "ps_sources/frontend/http_service.c", "ps_sources/frontend/main.c"} <= set(changes):
        raise RuntimeError(f"Unexpected compiled source changes: {changes}")
    candidate = json.loads((ROOT / "scripts/fixtures/f134_candidate_baseline.json").read_text())
    candidate_sha = candidate["firmware_sha256"]
    for name, digest in candidate["verification_sources"].items():
        if sha(ROOT / name) != digest:
            raise RuntimeError(f"TLS verification source differs from frozen baseline: {name}")
    if set(before) != set(candidate["sources"]):
        raise RuntimeError("Production source inventory differs from working archive candidate")
    since_candidate = [name for name in before if before[name] != candidate["sources"][name]]
    expected = {"ps_sources/image_versions.h"}
    if set(since_candidate) != expected:
        raise RuntimeError(f"Only the version header may change from the working candidate: {since_candidate}")
    roots = json.loads((ROOT / "software/https_setup/roots.json").read_text())
    fingerprint = "45140b3247eb9cc8c5b4f0d7b53091f73292089e6e5a63e2749dd3aca9198eda"
    if len(roots["roots"]) != 29 or not any(row["der_sha256"] == fingerprint for row in roots["roots"]):
        raise RuntimeError("Expected approved 29-root policy including direct Go Daddy G2")
    header = (ROOT / "ps_sources/frontend/http_core.h").read_text()
    if not re.search(r"HTTP_LINE_MAX\s+8192U", header):
        raise RuntimeError("Expected the retained 8192-byte header-line limit")
    keep = [BASE / "FIRMWARE.BIN", OUT / "validation/archive-candidate/FIRMWARE.BIN",
            ROOT / "hdl/apple/apple_top.sv", ROOT / "hdl/apple/onee_mode_safety_guard.sv"]
    previous_top_level = sha(ROOT / "FIRMWARE.BIN") if (ROOT / "FIRMWARE.BIN").exists() else None
    preserved = {str(path.relative_to(ROOT)): sha(path) for path in keep if path.exists()}
    platform = ROOT / "vitis_workspace/appletini_platform/export/appletini_platform/hw/appletini_yarz_top.xsa"
    if not platform.exists():
        matches = list((ROOT / "vitis_workspace/appletini_platform").rglob("*.xsa"))
        if not matches or any(sha(p) != sha(BASE / "inputs/appletini_yarz_top.xsa") for p in matches):
            raise RuntimeError("Workspace platform XSA differs from released FPGA")
        platform = matches[0]
    blob = ROOT / "vitis_workspace/frontend/src/core1_blob.bin"
    if not blob.exists():
        blob.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(BASE / "inputs/core1_blob.bin", blob)
    for name, path in {
        "core1_blob.bin": blob,
        "appletini_yarz_top.xsa": platform,
    }.items():
        if sha(path) != sha(BASE / "inputs" / name):
            raise RuntimeError(f"Workspace input differs from retained release: {path}")
    configure_http_service(ROOT)
    config = (ROOT / "vitis_workspace/frontend/src/UserConfig.cmake").read_text()
    if "APPLETINI_HTTP_SERVICE=1" not in config or any(x in config for x in
            ("APPLETINI_HTTP_BENCH=1", "APPLETINI_HTTPS_PROBE=1", "APPLETINI_HTTP_DIAGNOSTICS=1")):
        raise RuntimeError("Workspace is not the normal production HTTP service configuration")
    (OUT / "inputs").mkdir(parents=True)
    for name in ("fsbl.elf", "appletini_yarz_top.bit", "appletini_yarz_top.xsa",
                 "frontend_core1.elf", "core1_blob.bin"):
        shutil.copy2(BASE / "inputs" / name, OUT / "inputs" / name)
    for name in ("ARM_BUILD.json", "FIRMWARE_AUDIT.json", "TIMING_MANIFEST.txt"):
        shutil.copy2(BASE / name, OUT / ("BASELINE_" + name))
    shutil.copy2(ROOT / "third_party/mozilla-ca/LICENSE", OUT / "LICENSE-roots.txt")
    shutil.copy2(ROOT / "scripts/fixtures/f134_candidate_baseline.json", OUT / "CANDIDATE_SOURCE_BASELINE.json")
    with zipfile.ZipFile(OUT / "ARM-SOURCES.zip", "w", zipfile.ZIP_DEFLATED) as archive:
        for name in before:
            archive.write(ROOT / name, name)
        archive.write(Path(__file__), "scripts/build_firmware_f134.py")
        for name in ("scripts/http_build_config.py", "scripts/create_vitis_workspace.py",
                     "scripts/package_firmware_f134.py", "scripts/frontend_memory.py",
                     "scripts/package_arm_release.py", "scripts/check_release_board.py",
                     "scripts/board_http_acceptance.py", "scripts/smartport_timing_board.py",
                     "scripts/audit_firmware_release.py", "scripts/make_firmware_bin.bat",
                     "scripts/image_manifest.py",
                     "scripts/test_onee_input_service.py", "scripts/setup_https.py",
                     "docs/releases/F1.3.4.md", "third_party/mbedtls/LICENSE",
                     "third_party/mbedtls/APPLETINI_PATCHES.md",
                     "scripts/fixtures/f134_candidate_baseline.json",
                     "scripts/build_https_roots.py", "third_party/mozilla-ca/LICENSE",
                     "third_party/mozilla-ca/cacert.pem", "software/https_setup/roots.json",
                     "software/https_setup/ATNETCA.PEM", "software/https_setup/README.md",
                     "scripts/test_http_tls.py", "scripts/fixtures/http_tls_host.c",
                     "scripts/test_http_tls_handshake.py", "scripts/fixtures/http_tls_handshake_host.c",
                     "scripts/fixtures/http_tls_controlled_ca.pem",
                     "scripts/fixtures/http_tls_controlled_ca.README.md",
                     "scripts/test_http_setup.py", "scripts/fixtures/http_setup_host.c",
                     "scripts/fixtures/http_network_host.c", "scripts/fixtures/http_service_host/ff.h",
                     "scripts/fixtures/http_service_host/xiltimer.h", "scripts/test_http_entropy.py",
                     "scripts/fixtures/http_entropy_host.c", "scripts/test_http_trust.py",
                     "scripts/fixtures/http_entropy_mmio_host.c",
                     "scripts/fixtures/http_trust_host.c", "scripts/test_https_roots.py",
                     "scripts/test_http_mobile.py", "scripts/fixtures/http_mobile_host.c",
                     "scripts/test_http_service.py", "scripts/fixtures/http_core_host.c",
                     "scripts/test_http_download.py", "scripts/fixtures/http_download_host.c",
                     "scripts/test_net_service.py", "scripts/fixtures/net_service_host.c",
                     "scripts/test_download_filename.py", "scripts/test_http_download_media.py",
                     "scripts/test_config_download_ownership.py", "scripts/test_http_attachments.py",
                     "scripts/test_printer_sd_ownership.py",
                     "scripts/fixtures/http_attachment_parser_host.c",
                     "scripts/fixtures/http_attachment_service_host.c"):
            archive.write(ROOT / name, name)
        for path in sorted((ROOT / "scripts/fixtures/http_entropy_host").rglob("*")):
            if path.is_file():
                archive.write(path, path.relative_to(ROOT).as_posix())
    env = dict(os.environ)
    path_value = next(value for key, value in env.items() if key.lower() == "path")
    env = {key: value for key, value in env.items() if key.lower() != "path"}
    cache = (ROOT / "vitis_workspace/frontend/build/CMakeCache.txt").read_text()
    cmake = re.search(r"^CMAKE_COMMAND:INTERNAL=(.+)$", cache, re.M).group(1)
    commands = json.loads((ROOT / "vitis_workspace/frontend/build/compile_commands.json").read_text())
    match = re.match(r'^(?:"([^"]+)"|(\S+))', commands[0]["command"])
    compiler = Path(match.group(1) or match.group(2))
    env["PATH"] = str(compiler.parent) + os.pathsep + path_value
    build_command = [cmake, "--build", str(ROOT / "vitis_workspace/frontend/build"), "--parallel", "8"]
    print("Building CPU0 only: " + " ".join(build_command), flush=True)
    direct_link = None
    try:
        run(build_command, "CPU0_BUILD.log", env)
    except RuntimeError:
        # Some Windows installations reject CMake's nested cmd.exe link wrapper.
        # Keep every generated link flag and response-file byte, avoiding only
        # that shell wrapper. Compilation must already have succeeded.
        log = (OUT / "CPU0_BUILD.log").read_text()
        if "FAILED: frontend.elf" not in log or "The system cannot execute the specified program." not in log:
            raise
        build_dir = ROOT / "vitis_workspace/frontend/build"
        ninja = (build_dir / "build.ninja").read_text()
        link_block = re.search(r"build frontend\.elf:.*?(?=\n\n)", ninja, re.S).group(0)
        flags = re.search(r"\n  FLAGS = ([^\n]+)", link_block).group(1)
        extra_link = re.search(r"\n  LINK_FLAGS = ([^\n]+)", link_block)
        rules = (build_dir / "CMakeFiles/rules.ninja").read_text()
        fixed_flags = re.search(r"rule C_EXECUTABLE_LINKER[^\n]*\n.*?\$FLAGS(.*?)\$LINK_FLAGS", rules, re.S).group(1)
        direct_link = [str(compiler), *shlex.split(flags), *shlex.split(fixed_flags),
                       *shlex.split(extra_link.group(1) if extra_link else ""),
                       "@CMakeFiles/frontend.elf.rsp", "-o", "frontend.elf"]
        with (OUT / "DIRECT_LINK.log").open("w") as stream:
            subprocess.run(direct_link, cwd=build_dir, env=env, stdout=stream,
                           stderr=subprocess.STDOUT, check=True)
    after = {name: sha(ROOT / name) for name in before}
    if after != before:
        raise RuntimeError("Sources changed during build")
    frontend = OUT / "inputs/frontend.elf"
    shutil.copy2(ROOT / "vitis_workspace/frontend/build/frontend.elf", frontend)
    (OUT / "build-config").mkdir()
    for name, path in {
        "UserConfig.cmake": ROOT / "vitis_workspace/frontend/src/UserConfig.cmake",
        "lscript.ld": ROOT / "vitis_workspace/frontend/src/lscript.ld",
        "build.ninja": ROOT / "vitis_workspace/frontend/build/build.ninja",
        "rules.ninja": ROOT / "vitis_workspace/frontend/build/CMakeFiles/rules.ninja",
        "frontend.elf.rsp": ROOT / "vitis_workspace/frontend/build/CMakeFiles/frontend.elf.rsp",
    }.items():
        shutil.copy2(path, OUT / "build-config" / name)
    env["XILINX_VITIS"] = "C:\\Xilinx\\2025.2\\Vitis"
    # Bootgen's batch wrapper cannot parse sandbox TEMP paths containing braces.
    # These are throwaway package files, not project inputs or linked paths.
    (OUT / "scratch").mkdir()
    env["TEMP"] = env["TMP"] = str(OUT / "scratch")
    package_command = [str(ROOT / "scripts/make_firmware_bin.bat"), str(OUT / "FIRMWARE.BIN"),
                       str(OUT / "inputs/fsbl.elf"), str(OUT / "inputs/appletini_yarz_top.bit"), str(frontend)]
    run(package_command, "PACKAGE.log", env)
    run([sys.executable, str(ROOT / "scripts/audit_firmware_release.py"),
         "--firmware", str(OUT / "FIRMWARE.BIN"), "--fsbl", str(OUT / "inputs/fsbl.elf"),
         "--frontend", str(frontend), "--bitstream", str(OUT / "inputs/appletini_yarz_top.bit"),
         "--xsa", str(OUT / "inputs/appletini_yarz_top.xsa"), "--platform-xsa", str(platform),
         "--run-manifest", str(OUT / "BASELINE_TIMING_MANIFEST.txt"),
         "--core1-elf", str(OUT / "inputs/frontend_core1.elf"),
         "--core1-blob", str(OUT / "inputs/core1_blob.bin"),
         "--version", "F1.3.4", "--boot-version", "B2.0.6", "--output", str(OUT / "FIRMWARE_AUDIT.json")],
        "AUDIT.log", env)
    if any(sha(ROOT / name) != value for name, value in preserved.items()):
        raise RuntimeError("A preserved baseline or unrelated HDL file changed")
    report = {"kind": "F1.3.4 normal HTTP-service release, version-only change from working archive candidate",
              "compiled_source_changes": changes, "compiled_sources_added": added,
              "changes_since_candidate": since_candidate,
              "candidate_baseline_sha256": candidate_sha,
              "candidate_source_inventory": "scripts/fixtures/f134_candidate_baseline.json",
              "root_addition_approved_by_user": True,
              "previous_top_level_firmware_sha256": previous_top_level,
              "top_level_firmware_updated": True,
              "tls_verification_profile_changed": False,
              "sources": before, "build_command": build_command, "direct_link_command": direct_link,
              "package_command": package_command,
              "fpga_rebuilt": False, "cpu1_rebuilt": False, "flashed": False,
              "preserved_inputs": preserved,
              "files": {str(path.relative_to(OUT)): {"bytes": path.stat().st_size, "sha256": sha(path)}
                        for path in sorted(OUT.rglob("*")) if path.is_file()},
              "limitations": ["No physical board boot or flash performed.",
                              "The user reported the working candidate succeeded; this version-only F1.3.4 image has no separate board run."]}
    finish_release(ROOT, OUT, report)
    print(json.dumps({"firmware": str(OUT / "FIRMWARE.BIN"), "sha256": sha(OUT / "FIRMWARE.BIN")}, indent=2))


if __name__ == "__main__":
    main()
