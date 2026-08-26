#!/usr/bin/env python3
"""Convert each SC-02 PDF sheet to a deterministic, text-based SVG view.

The SVG files preserve the source drawing and searchable text.  Electrical
connectivity still comes from ``sc02_schematic_extract.py``; SVG geometry must
not be used to infer whether crossing lines connect.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
from typing import Any

try:
    import pymupdf as fitz
except ImportError:  # PyMuPDF releases before 1.24 expose only ``fitz``.
    try:
        import fitz  # type: ignore[no-redef]
    except ImportError as exc:  # pragma: no cover - depends on host setup.
        raise SystemExit(
            "PyMuPDF is required; install it with: "
            "python -m pip install -r scripts/requirements-sc02-schematic.txt"
        ) from exc


SCHEMA_NAME = "appletini.sc02_schematic.v1"
EXPECTED_PYMUPDF_VERSION = "1.27.2.3"


class ConversionError(RuntimeError):
    """Raised when the PDF and extracted model do not describe the same source."""


def _require_pymupdf_version() -> None:
    actual = str(getattr(fitz, "VersionBind", getattr(fitz, "__version__", "unknown")))
    if actual != EXPECTED_PYMUPDF_VERSION:
        raise ConversionError(
            f"PyMuPDF {EXPECTED_PYMUPDF_VERSION} is required for byte-stable output; found {actual}. "
            "Install scripts/requirements-sc02-schematic.txt."
        )


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _slug(sheet_name: str) -> str:
    stem = sheet_name.rsplit(".", 1)[0]
    slug = re.sub(r"[^A-Za-z0-9._-]+", "_", stem).strip("._")
    if not slug:
        raise ConversionError(f"Sheet name has no safe filename: {sheet_name!r}")
    return slug.lower()


def _load_model(path: Path) -> dict[str, Any]:
    try:
        model = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ConversionError(f"Could not read extracted model {path}: {exc}") from exc
    if model.get("schema") != SCHEMA_NAME:
        raise ConversionError(
            f"Unsupported model schema {model.get('schema')!r}; expected {SCHEMA_NAME!r}"
        )
    return model


def convert(pdf_path: Path, model_path: Path, output_dir: Path) -> list[Path]:
    _require_pymupdf_version()
    pdf_path = pdf_path.resolve()
    model = _load_model(model_path.resolve())
    source = model.get("source", {})
    actual_hash = _sha256(pdf_path)
    if source.get("sha256") != actual_hash:
        raise ConversionError(
            "PDF SHA-256 does not match the extracted model: "
            f"{actual_hash} != {source.get('sha256')}"
        )

    sheets = model.get("sheets", [])
    output_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    with fitz.open(pdf_path) as document:
        if not document.is_pdf:
            raise ConversionError(f"Input is not a PDF: {pdf_path}")
        if len(document) != len(sheets):
            raise ConversionError(
                f"PDF has {len(document)} pages but model has {len(sheets)} sheets"
            )

        for page_index, sheet in enumerate(sheets):
            expected_page = page_index + 1
            if sheet.get("page_number") != expected_page:
                raise ConversionError(
                    f"Model sheet {sheet.get('name')!r} has unexpected page number "
                    f"{sheet.get('page_number')!r}; expected {expected_page}"
                )
            svg = document[page_index].get_svg_image(text_as_path=False)
            svg = svg.replace("\r\n", "\n").replace("\r", "\n")
            if not svg.lstrip().startswith("<svg"):
                raise ConversionError(f"Page {expected_page} did not produce SVG")
            provenance = (
                f"<!-- SC-02 source page {expected_page}: {sheet['name']}; "
                f"PDF SHA-256 {actual_hash} -->\n"
            )
            payload = provenance + svg.rstrip() + "\n"
            output_path = output_dir / f"{expected_page:02d}_{_slug(sheet['name'])}.svg"
            temporary_path = output_path.with_name(f".{output_path.name}.tmp")
            try:
                temporary_path.write_text(payload, encoding="utf-8", newline="\n")
                os.replace(temporary_path, output_path)
            finally:
                if temporary_path.exists():
                    temporary_path.unlink()
            written.append(output_path)

    return written


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Convert all SC-02 PDF sheets to searchable SVG files."
    )
    parser.add_argument("input_pdf", type=Path, help="SC-02 source PDF")
    parser.add_argument("--model", required=True, type=Path, help="extracted JSON")
    parser.add_argument("--output-dir", required=True, type=Path, help="SVG directory")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        written = convert(args.input_pdf, args.model, args.output_dir)
    except (ConversionError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"wrote {len(written)} source SVG sheets to {args.output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
