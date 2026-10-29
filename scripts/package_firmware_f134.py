"""Finish the audited F1.3.4 release bundle; never publish or flash."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

from frontend_memory import audit_frontend_elf


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def finish_release(root, out, report):
    audit = json.loads((out / "FIRMWARE_AUDIT.json").read_text())
    if audit["status"] != "PASS" or audit["version"] != "F1.3.4":
        raise RuntimeError("A passing F1.3.4 image audit is required")
    public = {
        "setup_https.py": root / "scripts/setup_https.py",
        "ATNETCA.PEM": root / "software/https_setup/ATNETCA.PEM",
        "roots.json": root / "software/https_setup/roots.json",
        "README.md": root / "software/https_setup/README.md",
        "LICENSE-roots.txt": root / "third_party/mozilla-ca/LICENSE",
        "LICENSE-mbedtls.txt": root / "third_party/mbedtls/LICENSE",
        "MBEDTLS-CHANGES.md": root / "third_party/mbedtls/APPLETINI_PATCHES.md",
    }
    setup = out / "HTTPS-SETUP"
    setup.mkdir()
    for name, source in public.items():
        shutil.copy2(source, setup / name)
    (setup / "INSTALL.txt").write_text(
        "F1.3.4 HTTPS setup\n\n"
        "A new writable FAT SD card needs no Python tool, PEM or ATNET.CFG.\n"
        "Enable Ethernet, configure menu/DHCP DNS and set the RTC to UTC.\n"
        "Keep existing private seed files with their board. Do not clone or roll them back.\n"
        "First-use XADC seeding remains experimental; read README.md.\n\n"
        "An SD-root ATNETCA.PEM replaces the built-in 29 roots. Remove an old\n"
        "override to use firmware roots, or replace it with this bundle, then reboot.\n"
        "An old 28-root override still fails for the tested archive.org chain.\n\n"
        "The included setup_https.py remains optional for older firmware or PC seeding:\n"
        "python setup_https.py E:\\ --roots ATNETCA.PEM --utc-offset 0\n"
        "Use the actual SD root and RTC local-minus-UTC offset in minutes.\n"
        "This package contains no private seeds.\n\n"
        "Install FIRMWARE.BIN with the normal SD updater. Golden B2.0.6 stays in place.\n",
        newline="\n")
    shutil.copy2(root / "docs/releases/F1.3.4.md", out / "RELEASE_NOTES.md")
    shutil.copy2(out / "BASELINE_TIMING_MANIFEST.txt", out / "TIMING_MANIFEST.txt")
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    dirty = subprocess.check_output(["git", "status", "--porcelain", "--ignore-submodules=all"], cwd=root, text=True)
    (out / "SOURCE_COMMIT.txt").write_text(
        "Uncommitted F1.3.4 release preparation\nBase HEAD: " + head +
        "\nExact source identity: ARM_BUILD.json and ARM-SOURCES.zip\n", newline="\n")
    (out / "SOURCE_STATE.json").write_text(json.dumps({
        "base_head": head, "committed_release": False, "working_tree_status": dirty.splitlines(),
        "submodule_status_polled": False,
        "unrelated_hdl_excluded": True, "published": False, "tagged": False, "flashed": False,
    }, indent=2) + "\n", newline="\n")
    report.update({
        "variant": "http-service", "application": True, "diagnostics": False,
        "frontend_sha256": sha(out / "inputs/frontend.elf"),
        "core1_sha256": sha(out / "inputs/frontend_core1.elf"),
        "embedded_core1_sha256": sha(out / "inputs/core1_blob.bin"),
        "frontend_memory": audit_frontend_elf(out / "inputs/frontend.elf"),
        "mbedtls": "3.6.7", "flash_programmed": False,
        "package": {"sha256": sha(out / "FIRMWARE.BIN"),
                    "fsbl_sha256": sha(out / "inputs/fsbl.elf"),
                    "bitstream_sha256": sha(out / "inputs/appletini_yarz_top.bit"),
                    "baseline": "firmwares/firmware-F1.3.3"},
    })
    (out / "ARM_BUILD.json").write_text(json.dumps(report, indent=2) + "\n", newline="\n")
    with zipfile.ZipFile(out / "ARM-SOURCES.zip") as source:
        inventory = {name: {"bytes": len(source.read(name)),
                           "sha256": hashlib.sha256(source.read(name)).hexdigest()}
                     for name in source.namelist()}
        if any(source.read(name) != (root / name).read_bytes() for name in source.namelist()):
            raise RuntimeError("Source snapshot differs from the release working tree")
    (out / "SOURCE_TEST_HASHES.json").write_text(json.dumps(inventory, indent=2) + "\n", newline="\n")
    for name, firmware in (("HTTPS-SETUP.zip", False), ("Appletini-F1.3.4.zip", True)):
        with zipfile.ZipFile(out / name, "w", zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(setup.iterdir()):
                archive.write(path, path.name)
            if firmware:
                for member in ("FIRMWARE.BIN", "RELEASE_NOTES.md", "SOURCE_COMMIT.txt"):
                    archive.write(out / member, member)
        with zipfile.ZipFile(out / name) as archive:
            if archive.testzip() is not None:
                raise RuntimeError("Release ZIP integrity failed")
            if firmware and archive.read("FIRMWARE.BIN") != (out / "FIRMWARE.BIN").read_bytes():
                raise RuntimeError("Release ZIP firmware differs")
    members = ("FIRMWARE.BIN", "HTTPS-SETUP.zip", "Appletini-F1.3.4.zip", "ARM-SOURCES.zip",
               "ARM_BUILD.json", "FIRMWARE_AUDIT.json", "SOURCE_COMMIT.txt", "SOURCE_STATE.json",
               "SOURCE_TEST_HASHES.json", "RELEASE_NOTES.md", "TIMING_MANIFEST.txt")
    (out / "SHA256SUMS.txt").write_text(
        "".join(sha(out / name) + "  " + name + "\n" for name in members), newline="\n")
    report["files"] = {p.relative_to(out).as_posix(): {"bytes": p.stat().st_size, "sha256": sha(p)}
                       for p in sorted(out.rglob("*")) if p.is_file() and p.name != "BUILD_PROVENANCE.json"}
    (out / "BUILD_PROVENANCE.json").write_text(json.dumps(report, indent=2) + "\n", newline="\n")
    shutil.copy2(out / "FIRMWARE.BIN", root / "FIRMWARE.BIN")
