#!/usr/bin/env python3
"""Write a deterministic SHA-256 manifest for the SC-02 archive."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import sys


class ManifestError(RuntimeError):
    """Raised when an archive cannot be hashed safely."""


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_manifest(archive_dir: Path, output_path: Path) -> int:
    archive_dir = archive_dir.resolve()
    output_path = output_path.resolve()
    if not archive_dir.is_dir():
        raise ManifestError(f"Archive directory does not exist: {archive_dir}")
    try:
        output_path.relative_to(archive_dir)
    except ValueError as exc:
        raise ManifestError("Manifest output must be inside the archive") from exc

    files: list[Path] = []
    for path in archive_dir.rglob("*"):
        if path == output_path:
            continue
        if path.is_symlink():
            raise ManifestError(f"Archive contains a symbolic link: {path}")
        if path.is_file():
            files.append(path)
    files.sort(key=lambda path: path.relative_to(archive_dir).as_posix())
    if not files:
        raise ManifestError("Archive contains no files")

    lines = [
        f"{_sha256(path)}  {path.relative_to(archive_dir).as_posix()}"
        for path in files
    ]
    payload = "\n".join(lines) + "\n"
    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = output_path.with_name(f".{output_path.name}.tmp")
    try:
        temporary_path.write_text(payload, encoding="utf-8", newline="\n")
        os.replace(temporary_path, output_path)
    finally:
        if temporary_path.exists():
            temporary_path.unlink()
    return len(files)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive_dir", type=Path, help="archive root")
    parser.add_argument(
        "--output",
        required=True,
        type=Path,
        help="manifest path inside the archive",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        count = write_manifest(args.archive_dir, args.output)
    except (ManifestError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"wrote {args.output} with {count} file hashes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
