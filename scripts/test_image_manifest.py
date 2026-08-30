#!/usr/bin/env python3
"""Focused checks for Appletini image-manifest packaging."""

from __future__ import annotations

import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

import image_manifest as image


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def expect_failure(fn, message: str) -> None:
    try:
        fn()
    except (image.ManifestError, OSError):
        return
    raise AssertionError(message)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="appletini-manifest-") as name:
        temp_dir = Path(name)
        payload_path = temp_dir / "raw.bin"
        output_path = temp_dir / "BOOT.BIN"
        payload = bytes(range(256)) * 17 + b"Appletini"
        payload_path.write_bytes(payload)

        manifest = image.append_manifest(
            payload_path,
            output_path,
            role=image.ROLE_GOLDEN,
            flags=image.FLAG_RECOVERY_CAPABLE,
        )
        packaged = output_path.read_bytes()
        require(packaged[:-image.MANIFEST_SIZE] == payload,
                "packager changed the Bootgen payload")
        require(len(packaged[-image.MANIFEST_SIZE:]) == 32,
                "manifest is not exactly 32 bytes")

        payload_crc32 = zlib.crc32(payload) & 0xFFFFFFFF
        prefix = struct.pack(
            "<8s5I",
            b"ATNIMG1\0",
            1,
            1,
            1,
            len(payload),
            payload_crc32,
        )
        expected_manifest = prefix + struct.pack(
            "<I", zlib.crc32(prefix) & 0xFFFFFFFF
        )
        require(packaged[-32:] == expected_manifest,
                "manifest bytes do not match the fixed LE format")
        require(manifest.payload_crc32 == payload_crc32,
                "append result has the wrong payload CRC32")

        verified = image.verify_image(
            output_path,
            expected_role=image.ROLE_GOLDEN,
            expected_recovery=True,
        )
        require(verified == manifest, "verify did not return the manifest")
        expect_failure(
            lambda: image.verify_image(
                output_path, expected_role=image.ROLE_FIRMWARE
            ),
            "verify accepted the wrong image role",
        )

        damaged_payload = temp_dir / "damaged-payload.bin"
        damaged = bytearray(packaged)
        damaged[0] ^= 0x01
        damaged_payload.write_bytes(damaged)
        expect_failure(
            lambda: image.verify_image(damaged_payload),
            "verify accepted a damaged payload",
        )

        damaged_manifest = temp_dir / "damaged-manifest.bin"
        damaged = bytearray(packaged)
        damaged[-1] ^= 0x01
        damaged_manifest.write_bytes(damaged)
        expect_failure(
            lambda: image.verify_image(damaged_manifest),
            "verify accepted a damaged manifest",
        )

        old_output = temp_dir / "existing.bin"
        old_output.write_bytes(b"keep this output")
        original_verify = image.verify_image

        def reject_temp(*_args, **_kwargs):
            raise image.ManifestError("injected pre-replace failure")

        image.verify_image = reject_temp
        try:
            expect_failure(
                lambda: image.append_manifest(
                    payload_path,
                    old_output,
                    role=image.ROLE_FIRMWARE,
                    flags=image.FLAG_RECOVERY_CAPABLE,
                ),
                "append accepted an injected verification failure",
            )
        finally:
            image.verify_image = original_verify
        require(old_output.read_bytes() == b"keep this output",
                "failed append replaced the prior output")
        require(not list(temp_dir.glob(".existing.bin.*.tmp")),
                "failed append left a temporary file")

        slot_size = 0x100000
        boundary_payload = temp_dir / "boundary-raw.bin"
        boundary_output = temp_dir / "boundary-BOOT.BIN"
        boundary_payload.write_bytes(
            b"\xA5" * (slot_size - image.MANIFEST_SIZE)
        )
        image.append_manifest(
            boundary_payload,
            boundary_output,
            role=image.ROLE_GOLDEN,
            flags=image.FLAG_RECOVERY_CAPABLE,
            max_size=slot_size,
        )
        require(boundary_output.stat().st_size == slot_size,
                "packager rejected or changed an exact-slot-size image")

        oversized_payload = temp_dir / "oversized-raw.bin"
        oversized_output = temp_dir / "oversized-BOOT.BIN"
        oversized_output.write_bytes(b"keep prior image")
        oversized_payload.write_bytes(
            b"\x5A" * (slot_size - image.MANIFEST_SIZE + 1)
        )
        expect_failure(
            lambda: image.append_manifest(
                oversized_payload,
                oversized_output,
                role=image.ROLE_GOLDEN,
                flags=image.FLAG_RECOVERY_CAPABLE,
                max_size=slot_size,
            ),
            "packager accepted a final image larger than one slot",
        )
        require(oversized_output.read_bytes() == b"keep prior image",
                "oversize failure replaced the prior output")
        require(not list(temp_dir.glob(".oversized-BOOT.BIN.*.tmp")),
                "oversize failure left a temporary file")

        cli_payload = temp_dir / "cli-raw.bin"
        cli_output = temp_dir / "FIRMWARE.BIN"
        cli_payload.write_bytes(b"firmware payload")
        tool = ROOT / "scripts" / "image_manifest.py"
        appended = subprocess.run(
            [
                sys.executable,
                str(tool),
                "append",
                str(cli_payload),
                str(cli_output),
                "--role",
                "firmware",
                "--recovery-capable",
                "--max-size",
                "0x100000",
            ],
            capture_output=True,
            text=True,
        )
        require(appended.returncode == 0,
                f"append CLI failed: {appended.stdout}{appended.stderr}")
        checked = subprocess.run(
            [
                sys.executable,
                str(tool),
                "verify",
                str(cli_output),
                "--role",
                "firmware",
                "--require-recovery-capable",
            ],
            capture_output=True,
            text=True,
        )
        require(checked.returncode == 0,
                f"verify CLI failed: {checked.stdout}{checked.stderr}")

    boot_script = (ROOT / "scripts" / "make_boot_bin.bat").read_text(
        encoding="utf-8"
    )
    firmware_script = (
        ROOT / "scripts" / "make_firmware_bin.bat"
    ).read_text(encoding="utf-8")
    require('image_manifest.py" append' in boot_script and
            "--role golden --recovery-capable --max-size 0x100000" in
            boot_script,
            "golden build does not enforce its one-megabyte slot")
    require('image_manifest.py" append' in firmware_script and
            "--role firmware --recovery-capable --max-size 0x00DF0000" in
            firmware_script,
            "firmware build does not enforce its slot or append its recovery manifest")
    require('-o "%TMP_RAW%"' in boot_script and
            '-o "%TMP_RAW%"' in firmware_script,
            "Bootgen must write a temporary raw image")

    print("PASS: image manifests use the fixed format and atomic packaging")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
