#!/usr/bin/env python3
"""Cross-language checks for the strict timing-manifest format."""

from __future__ import annotations

import tempfile
import tkinter
from pathlib import Path

import package_timing_firmware as pack


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "scripts" / "testdata" / "timing_manifest"
TCL_HELPERS = ROOT / "scripts" / "timing_run_helpers.tcl"
BUILD_SCRIPT = ROOT / "scripts" / "build_and_export_xsa.tcl"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def expect_python_failure(path: Path) -> None:
    try:
        pack.read_manifest(path)
    except ValueError:
        return
    raise AssertionError(f"Python accepted invalid manifest {path.name}")


def expect_tcl_failure(interp: tkinter.Tcl, path: Path) -> None:
    try:
        interp.call("timing_run::read_manifest", str(path))
    except tkinter.TclError:
        return
    raise AssertionError(f"Tcl accepted invalid manifest {path.name}")


def tcl_dict(interp: tkinter.Tcl, value: object) -> dict[str, str]:
    fields = interp.splitlist(value)
    require(len(fields) % 2 == 0, "Tcl returned a malformed dictionary")
    return dict(zip(fields[0::2], fields[1::2]))


def flat_tcl_dict(values: dict[str, str]) -> tuple[str, ...]:
    return tuple(field for item in values.items() for field in item)


def main() -> int:
    interp = tkinter.Tcl()
    interp.call("source", str(TCL_HELPERS))
    build_script = BUILD_SCRIPT.read_text(encoding="utf-8")
    require("dict set build_info vivado_version [version -short]" in
            build_script,
            "Vivado manifest producer must store a one-line version")

    valid_path = FIXTURES / "valid.manifest"
    canonical = (FIXTURES / "canonical.manifest").read_bytes()
    expected = {
        "alpha": "1",
        "empty": "",
        "path": "value with spaces",
        "token": "a=b",
        "zeta": "last",
    }
    require(pack.read_manifest(valid_path) == expected,
            "Python did not parse the shared valid fixture")
    require(tcl_dict(
        interp, interp.call("timing_run::read_manifest", str(valid_path))
    ) == expected, "Tcl did not parse the shared valid fixture")

    for invalid_path in sorted(FIXTURES.glob("invalid_*.manifest")):
        expect_python_failure(invalid_path)
        expect_tcl_failure(interp, invalid_path)

    with tempfile.TemporaryDirectory() as temp_name:
        temp_dir = Path(temp_name)
        python_path = temp_dir / "python.manifest"
        tcl_path = temp_dir / "tcl.manifest"
        crlf_path = temp_dir / "crlf.manifest"

        pack.write_manifest(python_path, expected)
        require(python_path.read_bytes() == canonical,
                "Python writer did not emit canonical bytes")
        require(tcl_dict(
            interp,
            interp.call("timing_run::read_manifest", str(python_path)),
        ) == expected, "Tcl could not read the Python round trip")

        interp.call(
            "timing_run::write_manifest",
            str(tcl_path),
            flat_tcl_dict(expected),
        )
        require(tcl_path.read_bytes() == canonical,
                "Tcl writer did not emit canonical bytes")
        require(pack.read_manifest(tcl_path) == expected,
                "Python could not read the Tcl round trip")

        crlf_path.write_bytes(b"alpha=1\r\n")
        expect_python_failure(crlf_path)
        expect_tcl_failure(interp, crlf_path)

        try:
            pack.write_manifest(python_path, {"alpha": "line\nbreak"})
        except ValueError:
            pass
        else:
            raise AssertionError("Python writer accepted an embedded newline")
        try:
            interp.call(
                "timing_run::write_manifest",
                str(tcl_path),
                ("alpha", "line\nbreak"),
            )
        except tkinter.TclError:
            pass
        else:
            raise AssertionError("Tcl writer accepted an embedded newline")

    print("PASS: Python and Tcl timing manifests share one strict format")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
