#!/usr/bin/env python3
"""Append and verify the Appletini boot-image manifest."""

from __future__ import annotations

import argparse
import os
import struct
import sys
import tempfile
import zlib
from dataclasses import dataclass
from pathlib import Path


MAGIC = b"ATNIMG1\0"
FORMAT_VERSION = 1
ROLE_GOLDEN = 1
ROLE_FIRMWARE = 2
FLAG_RECOVERY_CAPABLE = 1 << 0
KNOWN_FLAGS = FLAG_RECOVERY_CAPABLE
MAX_U32 = 0xFFFFFFFF
COPY_CHUNK_SIZE = 1024 * 1024

_MANIFEST_PREFIX = struct.Struct("<8s5I")
_MANIFEST = struct.Struct("<8s6I")
MANIFEST_SIZE = _MANIFEST.size

ROLE_BY_NAME = {
    "golden": ROLE_GOLDEN,
    "firmware": ROLE_FIRMWARE,
}
ROLE_NAME = {value: name for name, value in ROLE_BY_NAME.items()}


class ManifestError(ValueError):
    """The image or manifest does not match the required format."""


@dataclass(frozen=True)
class ImageManifest:
    version: int
    role: int
    flags: int
    payload_size: int
    payload_crc32: int
    manifest_crc32: int


def _crc32(data: bytes, value: int = 0) -> int:
    return zlib.crc32(data, value) & MAX_U32


def _validate_role_and_flags(role: int, flags: int) -> None:
    if role not in ROLE_NAME:
        raise ManifestError(f"unknown image role {role}")
    if flags & ~KNOWN_FLAGS:
        raise ManifestError(f"unknown manifest flags 0x{flags:08X}")


def encode_manifest(role: int, flags: int, payload_size: int,
                    payload_crc32: int) -> bytes:
    """Return one exact 32-byte little-endian manifest."""
    _validate_role_and_flags(role, flags)
    if not 0 <= payload_size <= MAX_U32:
        raise ManifestError("payload size does not fit in u32")
    if not 0 <= payload_crc32 <= MAX_U32:
        raise ManifestError("payload CRC32 does not fit in u32")

    prefix = _MANIFEST_PREFIX.pack(
        MAGIC,
        FORMAT_VERSION,
        role,
        flags,
        payload_size,
        payload_crc32,
    )
    return prefix + struct.pack("<I", _crc32(prefix))


def decode_manifest(data: bytes) -> ImageManifest:
    """Parse and validate one exact manifest."""
    if len(data) != MANIFEST_SIZE:
        raise ManifestError(
            f"manifest is {len(data)} bytes, expected {MANIFEST_SIZE}"
        )

    (magic, version, role, flags, payload_size, payload_crc32,
     manifest_crc32) = _MANIFEST.unpack(data)
    if magic != MAGIC:
        raise ManifestError("bad image manifest magic")
    if version != FORMAT_VERSION:
        raise ManifestError(f"unsupported manifest version {version}")
    _validate_role_and_flags(role, flags)

    expected_crc32 = _crc32(data[:_MANIFEST_PREFIX.size])
    if manifest_crc32 != expected_crc32:
        raise ManifestError(
            "manifest CRC32 mismatch: "
            f"stored=0x{manifest_crc32:08X} expected=0x{expected_crc32:08X}"
        )

    return ImageManifest(
        version=version,
        role=role,
        flags=flags,
        payload_size=payload_size,
        payload_crc32=payload_crc32,
        manifest_crc32=manifest_crc32,
    )


def _payload_crc32(stream, payload_size: int) -> int:
    remaining = payload_size
    crc32 = 0
    while remaining:
        chunk = stream.read(min(remaining, COPY_CHUNK_SIZE))
        if not chunk:
            raise ManifestError("image ended before its declared payload size")
        crc32 = _crc32(chunk, crc32)
        remaining -= len(chunk)
    return crc32


def verify_image(image_path: Path | str, expected_role: int | None = None,
                 expected_recovery: bool | None = None) -> ImageManifest:
    """Verify the trailing manifest and payload CRC32 of an image."""
    path = Path(image_path)
    with path.open("rb") as stream:
        stream.seek(0, os.SEEK_END)
        image_size = stream.tell()
        if image_size < MANIFEST_SIZE:
            raise ManifestError("image is too small to contain a manifest")

        stream.seek(image_size - MANIFEST_SIZE)
        manifest = decode_manifest(stream.read(MANIFEST_SIZE))
        actual_payload_size = image_size - MANIFEST_SIZE
        if manifest.payload_size != actual_payload_size:
            raise ManifestError(
                "payload size mismatch: "
                f"stored={manifest.payload_size} actual={actual_payload_size}"
            )

        stream.seek(0)
        actual_payload_crc32 = _payload_crc32(stream, manifest.payload_size)
        if manifest.payload_crc32 != actual_payload_crc32:
            raise ManifestError(
                "payload CRC32 mismatch: "
                f"stored=0x{manifest.payload_crc32:08X} "
                f"actual=0x{actual_payload_crc32:08X}"
            )

    if expected_role is not None and manifest.role != expected_role:
        raise ManifestError(
            f"image role is {ROLE_NAME[manifest.role]}, expected "
            f"{ROLE_NAME.get(expected_role, expected_role)}"
        )
    if expected_recovery is not None:
        recovery = bool(manifest.flags & FLAG_RECOVERY_CAPABLE)
        if recovery != expected_recovery:
            state = "set" if expected_recovery else "clear"
            raise ManifestError(f"recovery-capable flag is not {state}")

    return manifest


def append_manifest(payload_path: Path | str, output_path: Path | str,
                    role: int, flags: int = 0,
                    max_size: int | None = None) -> ImageManifest:
    """Copy a payload, append its manifest, verify it, then replace output."""
    _validate_role_and_flags(role, flags)
    if max_size is not None and not (
        MANIFEST_SIZE <= max_size <= MAX_U32 + MANIFEST_SIZE
    ):
        raise ManifestError(
            f"max image size must be {MANIFEST_SIZE}.."
            f"{MAX_U32 + MANIFEST_SIZE} bytes"
        )
    source = Path(payload_path)
    destination = Path(output_path)
    destination_parent = destination.parent
    if not destination_parent.is_dir():
        raise FileNotFoundError(
            f"output directory does not exist: {destination_parent}"
        )

    temp_path: Path | None = None
    try:
        with source.open("rb") as source_stream:
            fd, temp_name = tempfile.mkstemp(
                prefix=f".{destination.name}.",
                suffix=".tmp",
                dir=destination_parent,
            )
            temp_path = Path(temp_name)
            with os.fdopen(fd, "wb") as output_stream:
                payload_size = 0
                payload_crc32 = 0
                while True:
                    chunk = source_stream.read(COPY_CHUNK_SIZE)
                    if not chunk:
                        break
                    payload_size += len(chunk)
                    if payload_size > MAX_U32:
                        raise ManifestError("payload size does not fit in u32")
                    final_size = payload_size + MANIFEST_SIZE
                    if max_size is not None and final_size > max_size:
                        raise ManifestError(
                            f"final image size {final_size} exceeds limit "
                            f"{max_size}"
                        )
                    payload_crc32 = _crc32(chunk, payload_crc32)
                    output_stream.write(chunk)

                output_stream.write(encode_manifest(
                    role,
                    flags,
                    payload_size,
                    payload_crc32,
                ))
                output_stream.flush()
                os.fsync(output_stream.fileno())

        manifest = verify_image(
            temp_path,
            expected_role=role,
            expected_recovery=bool(flags & FLAG_RECOVERY_CAPABLE),
        )
        os.replace(temp_path, destination)
        temp_path = None
        return manifest
    finally:
        if temp_path is not None:
            try:
                temp_path.unlink()
            except FileNotFoundError:
                pass


def _print_manifest(path: Path, manifest: ImageManifest, verb: str) -> None:
    recovery = "yes" if manifest.flags & FLAG_RECOVERY_CAPABLE else "no"
    print(
        f"{verb}: {path} role={ROLE_NAME[manifest.role]} "
        f"recovery={recovery} payload={manifest.payload_size} "
        f"crc32=0x{manifest.payload_crc32:08X}"
    )


def _parse_size(value: str) -> int:
    try:
        size = int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            f"invalid size {value!r}; use decimal or 0xHEX"
        ) from exc
    if size < MANIFEST_SIZE or size > MAX_U32 + MANIFEST_SIZE:
        raise argparse.ArgumentTypeError(
            f"size must be {MANIFEST_SIZE}..{MAX_U32 + MANIFEST_SIZE}"
        )
    return size


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    append_parser = subparsers.add_parser(
        "append", help="append a manifest and atomically write the output"
    )
    append_parser.add_argument("input", type=Path, help="raw Bootgen image")
    append_parser.add_argument("output", type=Path,
                               help="manifest-appended output image")
    append_parser.add_argument("--role", choices=ROLE_BY_NAME,
                               required=True)
    append_parser.add_argument("--recovery-capable", action="store_true")
    append_parser.add_argument(
        "--max-size",
        type=_parse_size,
        help="maximum final image size, in decimal or 0xHEX",
    )

    verify_parser = subparsers.add_parser(
        "verify", help="verify an image manifest and payload"
    )
    verify_parser.add_argument("image", type=Path)
    verify_parser.add_argument("--role", choices=ROLE_BY_NAME)
    verify_parser.add_argument("--require-recovery-capable",
                               action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        if args.command == "append":
            flags = (FLAG_RECOVERY_CAPABLE
                     if args.recovery_capable else 0)
            manifest = append_manifest(
                args.input,
                args.output,
                role=ROLE_BY_NAME[args.role],
                flags=flags,
                max_size=args.max_size,
            )
            _print_manifest(args.output, manifest, "WROTE")
        else:
            expected_role = (ROLE_BY_NAME[args.role]
                             if args.role is not None else None)
            expected_recovery = (True
                                 if args.require_recovery_capable else None)
            manifest = verify_image(
                args.image,
                expected_role=expected_role,
                expected_recovery=expected_recovery,
            )
            _print_manifest(args.image, manifest, "VERIFIED")
    except (ManifestError, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
