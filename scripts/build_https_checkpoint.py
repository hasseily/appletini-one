#!/usr/bin/env python3
"""Build opt-in CPU0 HTTPS instrumentation and matching CPU1 health telemetry.

Run with python scripts/build_https_checkpoint.py after preparing the test CA.
Uses the existing platform/XSA; does not rebuild PL or program flash.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/https-cpu0-checkpoint"
sys.path.insert(0, str(ROOT / "scripts"))
from frontend_memory import audit_frontend_elf
from http_build_config import configure_http_service


def append_config(component, block, entries):
    path = ROOT / "vitis_workspace" / component / "src/UserConfig.cmake"
    text = path.read_text()
    match = re.search(r"set\(\s*" + block + r"\b(?P<body>[^)]*)\)", text, re.S)
    if not match:
        raise RuntimeError(f"Missing {block}: {path}")
    body = match.group("body").rstrip()
    for entry in entries:
        if entry not in body:
            body += "\n" + entry
    replacement = f"set({block}{body}\n)"
    path.write_text(text[:match.start()] + replacement + text[match.end():])


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def restore_default_config():
    for component in ("frontend", "frontend_core1"):
        path = ROOT / "vitis_workspace" / component / "src/UserConfig.cmake"
        lines = path.read_text().splitlines(keepends=True)
        probe_entries = ("APPLETINI_HTTPS_PROBE=", "MBEDTLS_CONFIG_FILE=",
                         "APPLETINI_HTTP_SERVICE=", "APPLETINI_HTTP_BENCH=", "APPLETINI_HTTP_DIAGNOSTICS=",
                         "APPLETINI_NET_TRANSPORT_TEST=",
                         "third_party/mbedtls/include", "third_party/mbedtls/library/")
        path.write_text("".join(line for line in lines
                                if not any(entry in line for entry in probe_entries)))


def main():
    global OUT
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--package", action="store_true")
    mode.add_argument("--restore-default-build", action="store_true",
                      help="remove probe settings and rebuild normal CPU0/CPU1 artifacts")
    parser.add_argument("--baseline", type=Path, default=ROOT / "firmwares/firmware-F1.3.3b3")
    parser.add_argument("--service", action="store_true", help="build the reusable HTTP/S service instead of the old probe")
    parser.add_argument("--application", action="store_true", help="production SmartPort API, without bench or CPU1 probe telemetry (requires --service)")
    parser.add_argument("--diagnostics",action="store_true",help="include opt-in setup/register test commands")
    args = parser.parse_args()
    if args.application and not args.service:parser.error('--application requires --service')
    if args.service:
        OUT = ROOT / "build/http-service-checkpoint"
    OUT.mkdir(parents=True, exist_ok=True)
    required = ROOT / "ps_sources/frontend/https_probe_test_ca.h"
    if not args.service and not args.restore_default_build and not required.exists():
        raise RuntimeError("Run python scripts/https_probe_host.py prepare first")
    if args.restore_default_build:
        restore_default_config()
        configure_http_service(ROOT)
    else:
        restore_default_config()
        if not args.application:
            append_config("frontend_core1", "USER_COMPILE_DEFINITIONS", ["APPLETINI_HTTPS_PROBE=1"])
        append_config("frontend", "USER_COMPILE_DEFINITIONS",
                      ["APPLETINI_HTTP_SERVICE=1" if args.service else "APPLETINI_HTTPS_PROBE=1"])
        if args.service and not args.application:
            append_config("frontend", "USER_COMPILE_DEFINITIONS", ["APPLETINI_HTTP_BENCH=1"])
        # Angle includes avoid GCC's extra Windows quote parsing for -D strings.
        config_path = ROOT / "vitis_workspace/frontend/src/UserConfig.cmake"
        config_text = config_path.read_text()
        config_text = "\n".join(line for line in config_text.split("\n")
                                if "MBEDTLS_CONFIG_FILE=" not in line)
        config_path.write_text(config_text)
        append_config("frontend", "USER_COMPILE_DEFINITIONS",
                      ['"MBEDTLS_CONFIG_FILE=<' + ("http_service_mbedtls_config.h" if args.service else "https_probe_mbedtls_config.h") + '>"'])
        append_config("frontend", "USER_INCLUDE_DIRECTORIES", ['"../../../third_party/mbedtls/include"'])
        sources = ['"../../../ps_sources/frontend/https_probe.c"']
        sources += ['"../../../ps_sources/frontend/' + name + '"' for name in
                    ("http_core.c", "http_seed.c", "http_service.c", "http_bench.c", "net_service.c", "net_json.c")]
        sources += ['"../../../' + path.relative_to(ROOT).as_posix() + '"'
                    for path in sorted((ROOT / "third_party/mbedtls/library").glob("*.c"))]
        append_config("frontend", "USER_COMPILE_SOURCES", sources)
        if args.service:
            configure_http_service(ROOT,diagnostics=args.diagnostics or not args.application)
    source_files = sorted((ROOT / "ps_sources").rglob("*.c")) + sorted((ROOT / "ps_sources").rglob("*.h"))
    source_files += sorted((ROOT / "third_party/mbedtls").rglob("*.c"))
    source_files += sorted((ROOT / "third_party/mbedtls").rglob("*.h"))
    before = {p.relative_to(ROOT).as_posix(): digest(p) for p in source_files}
    # Windows environments sometimes expose both PATH and Path. Preserve one.
    for key in list(os.environ):
        if key.lower() == "path" and key != "PATH":
            value = os.environ.pop(key)
            if "PATH" not in os.environ:
                os.environ["PATH"] = value
    # Use the existing generated CMake projects directly. This also gives a
    # reliable failing exit code; the Vitis component API can return success
    # after reporting compiler failures, and retains workspace locks on exit.
    cache = (ROOT / "vitis_workspace/frontend/build/CMakeCache.txt").read_text()
    cmake = re.search(r"^CMAKE_COMMAND:INTERNAL=(.+)$", cache, re.M).group(1)
    compile_commands = json.loads((ROOT / "vitis_workspace/frontend/build/compile_commands.json").read_text())
    # Normal builds can put -I before -D; only the executable is the path.
    match = re.match(r'^(?:"([^"]+)"|(\S+))', compile_commands[0]["command"])
    compiler = match.group(1) or match.group(2)
    tools = Path(compiler).parent
    if not (tools / "arm-none-eabi-size.exe").is_file():
        raise RuntimeError(f"Missing ARM toolchain utilities: {tools}")
    os.environ["PATH"] = str(tools) + os.pathsep + os.environ.get("PATH", "")
    print("Building CPU1" + (" defaults" if args.restore_default_build else " health telemetry"), flush=True)
    subprocess.run([cmake, "--build", str(ROOT / "vitis_workspace/frontend_core1/build"),
                    "--parallel", "8"], check=True)
    core = ROOT / "vitis_workspace/frontend_core1/build/frontend_core1.elf"
    blob = ROOT / "vitis_workspace/frontend/src/core1_blob.bin"
    subprocess.run([str(tools / "arm-none-eabi-objcopy.exe"), "-O", "binary", str(core), str(blob)], check=True)
    print("Building CPU0" + (" defaults" if args.restore_default_build else " HTTP service" if args.service else " HTTPS probe"), flush=True)
    subprocess.run([cmake, "--build", str(ROOT / "vitis_workspace/frontend/build"),
                    "--parallel", "8"], check=True)
    after = {p.relative_to(ROOT).as_posix(): digest(p) for p in source_files}
    if before != after:
        raise RuntimeError("Sources changed during the build")
    elf = ROOT / "vitis_workspace/frontend/build/frontend.elf"
    symbols = subprocess.check_output([str(tools / "arm-none-eabi-nm.exe"), str(elf)], text=True)
    has_probe = bool(re.search(r"\bT https_probe_command$", symbols, re.M))
    has_service = bool(re.search(r"\bT http_service_start$", symbols, re.M))
    if args.restore_default_build:
        if has_probe or not has_service or re.search(r'\bT http_bench_start$',symbols,re.M):
            raise RuntimeError("Normal CPU0 must contain HTTP/S and omit probes/bench")
        report = {"frontend_sha256": digest(elf), "core1_sha256": digest(core),
                  "embedded_core1_sha256": digest(blob),
                  "frontend_memory": audit_frontend_elf(elf), "probe_absent": True}
        (OUT / "default-build-restored.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print("PASS: normal CPU0/CPU1 builds restored; saved checkpoint artifacts retained", flush=True)
        return
    if not (has_service if args.service else has_probe):
        raise RuntimeError("Requested service/probe is absent from CPU0 ELF")
    if args.application and not args.diagnostics:
        image=elf.read_bytes()
        if any(s in image for s in (b'erase-test-provisioning',b'[http-net-reg]',b'[https]')):
            raise RuntimeError('Diagnostic command remains in normal application')
    audit = audit_frontend_elf(elf)
    for path in (elf, core, blob):
        shutil.copy2(path, OUT / path.name)
    report = {
        "frontend_sha256": digest(elf), "core1_sha256": digest(core),
        "embedded_core1_sha256": digest(blob), "frontend_memory": audit,
        "mbedtls": "3.6.7", "sources": before,
        "variant": "http-service" if args.service else "https-probe",
        "application":args.application,"diagnostics":args.diagnostics or (args.service and not args.application),
        "fpga_rebuilt": False, "flash_programmed": False,
    }
    if args.package:
        os.environ["XILINX_VITIS"] = str(tools.parents[4])
        fsbl = args.baseline / "inputs/fsbl.elf"
        bit = args.baseline / "inputs/appletini_yarz_top.bit"
        package = OUT / "FIRMWARE.BIN"
        subprocess.run([str(ROOT / "scripts/make_firmware_bin.bat"), str(package),
                        str(fsbl), str(bit), str(OUT / "frontend.elf")], check=True)
        report["package"] = {"sha256": digest(package), "fsbl_sha256": digest(fsbl),
                             "bitstream_sha256": digest(bit), "baseline": str(args.baseline)}
    (OUT / "build-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("PASS: matching CPU0/CPU1 probe build and frontend memory audit", flush=True)


if __name__ == "__main__":
    main()
