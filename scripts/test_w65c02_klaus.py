#!/usr/bin/env python3
"""Run the W65C02 RTL against Klaus Dormann's program-level suites.

The GPL test sources and binaries remain outside this repository. By default
they are read from C:\\tmp\\klaus-65c02-tests. The script uses the two upstream
64 KiB images directly and assembles CMOS-configured decimal and interrupt
variants into the ignored build directory.
"""

from __future__ import annotations

import argparse
import re
import shutil
import struct
import subprocess
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_KLAUS_DIR = Path(r"C:\tmp\klaus-65c02-tests")
OUT_DIR = ROOT / "build" / "w65c02_klaus"
SNAPSHOT = "tb_w65c02_klaus_snapshot"


@dataclass(frozen=True)
class Suite:
    name: str
    suite_id: int
    image: bytes
    start_pc: int
    max_cycles: int
    termination_mode: int
    pass_pc: int = 0
    feedback: bool = False
    expected_address: int = 0
    expected_value: int = 0
    check_memory: bool = False


def vivado_tool(name: str) -> str:
    bat = shutil.which(f"{name}.bat")
    if bat:
        return bat
    tool = shutil.which(name)
    if tool:
        return tool
    raise FileNotFoundError(f"unable to locate Vivado tool {name}")


def run(cmd: list[str], cwd: Path, log: Path | None = None) -> str:
    completed = subprocess.run(
        cmd,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if log is not None:
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(completed.stdout, encoding="utf-8")
    if completed.returncode != 0:
        print(completed.stdout)
        raise RuntimeError(
            f"{Path(cmd[0]).name} failed with exit code {completed.returncode}"
        )
    return completed.stdout


def compile_snapshot() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    run(
        [
            vivado_tool("xvlog"),
            "--sv",
            str(ROOT / "hdl" / "apple" / "w65c02_core.sv"),
            str(ROOT / "hdl" / "sim" / "tb_w65c02_klaus.sv"),
        ],
        ROOT,
        OUT_DIR / "xvlog.log",
    )
    run(
        [vivado_tool("xelab"), "tb_w65c02_klaus", "-s", SNAPSHOT],
        ROOT,
        OUT_DIR / "xelab.log",
    )


def read_64k(path: Path) -> bytes:
    data = path.read_bytes()
    if len(data) != 65536:
        raise ValueError(f"{path} is {len(data)} bytes, expected 65536")
    return data


def patch_assignment(text: str, name: str, value: int) -> str:
    pattern = re.compile(rf"(?m)^{re.escape(name)}\s*=.*$")
    replacement = f"{name} = {value}"
    updated, count = pattern.subn(replacement, text, count=1)
    if count != 1:
        raise ValueError(f"could not patch {name} in Klaus source")
    return updated


def intel_hex_image(path: Path) -> bytes:
    image = bytearray(65536)
    upper = 0
    saw_eof = False

    for line_number, raw_line in enumerate(
        path.read_text(encoding="ascii").splitlines(), start=1
    ):
        line = raw_line.strip()
        if not line:
            continue
        if not line.startswith(":"):
            raise ValueError(f"{path}:{line_number}: invalid Intel HEX record")
        record = bytes.fromhex(line[1:])
        if len(record) < 5 or len(record) != record[0] + 5:
            raise ValueError(f"{path}:{line_number}: invalid record length")
        if sum(record) & 0xFF:
            raise ValueError(f"{path}:{line_number}: checksum mismatch")

        count = record[0]
        address = (record[1] << 8) | record[2]
        record_type = record[3]
        payload = record[4 : 4 + count]

        if record_type == 0:
            absolute = upper + address
            if absolute < 0 or absolute + count > len(image):
                raise ValueError(f"{path}:{line_number}: address outside 64 KiB")
            image[absolute : absolute + count] = payload
        elif record_type == 1:
            saw_eof = True
            break
        elif record_type == 2:
            if count != 2:
                raise ValueError(f"{path}:{line_number}: invalid type-02 record")
            upper = int.from_bytes(payload, "big") << 4
        elif record_type == 4:
            if count != 2:
                raise ValueError(f"{path}:{line_number}: invalid type-04 record")
            upper = int.from_bytes(payload, "big") << 16
        elif record_type not in (3, 5):
            raise ValueError(
                f"{path}:{line_number}: unsupported record type {record_type}"
            )

    if not saw_eof:
        raise ValueError(f"{path}: no Intel HEX EOF record")
    return bytes(image)


def assembler(klaus_dir: Path) -> Path:
    archive = klaus_dir / "as65_142.zip"
    if not archive.is_file():
        raise FileNotFoundError(f"missing Klaus assembler archive: {archive}")
    tool_dir = OUT_DIR / "tools"
    tool_dir.mkdir(parents=True, exist_ok=True)
    executable = tool_dir / "as65.exe"
    if not executable.is_file():
        with zipfile.ZipFile(archive) as package:
            executable.write_bytes(package.read("as65.exe"))
    return executable


def assemble_variant(
    klaus_dir: Path,
    name: str,
    source_name: str,
    assignments: dict[str, int],
    stop_on_success: bool = False,
) -> bytes:
    source = klaus_dir / source_name
    text = source.read_text(encoding="latin-1")
    for assignment, value in assignments.items():
        text = patch_assignment(text, assignment, value)

    if stop_on_success:
        pattern = re.compile(
            r"(?m)^(success\s+macro\s*)$\n(\s*)jmp\s+\*[^\n]*$"
        )
        text, count = pattern.subn(
            r"\1\n\2db  $db         ; automated success stop", text, count=1
        )
        if count != 1:
            raise ValueError("could not replace Klaus success trap")

    source_dir = OUT_DIR / "generated" / name
    source_dir.mkdir(parents=True, exist_ok=True)
    generated_source = source_dir / source_name
    generated_source.write_text(text, encoding="latin-1", newline="\n")
    output_hex = source_dir / f"{name}.hex"

    run(
        [
            str(assembler(klaus_dir)),
            "-q",
            "-l",
            "-m",
            "-x",
            "-s2",
            "-w",
            "-h0",
            f"-o{output_hex}",
            str(generated_source),
        ],
        klaus_dir,
        source_dir / "as65.log",
    )
    if not output_hex.is_file():
        raise FileNotFoundError(f"assembler did not create {output_hex}")
    return intel_hex_image(output_hex)


def prepare_suites(klaus_dir: Path) -> dict[str, Suite]:
    bin_dir = klaus_dir / "bin_files"
    functional = read_64k(bin_dir / "6502_functional_test.bin")
    extended = read_64k(bin_dir / "65C02_extended_opcodes_test.bin")
    decimal = assemble_variant(
        klaus_dir,
        "decimal",
        "6502_decimal_test.a65",
        {
            # Check every W65C02 decimal result bit for all accumulator,
            # operand, and carry-in values, including invalid BCD digits.
            # patch_assignment() also makes each setting a required source
            # guard: a missing or renamed upstream switch fails the run.
            "cputype": 1,
            "vld_bcd": 0,
            "chk_a": 1,
            "chk_n": 1,
            "chk_v": 1,
            "chk_z": 1,
            "chk_c": 1,
        },
    )
    interrupt = assemble_variant(
        klaus_dir,
        "interrupt",
        "6502_interrupt_test.a65",
        {"D_clear": 1},
        stop_on_success=True,
    )

    return {
        "functional": Suite(
            "functional", 1, functional, 0x0400, 200_000_000, 0, 0x3469
        ),
        "extended": Suite(
            "extended", 2, extended, 0x0400, 200_000_000, 0, 0x24F1
        ),
        "decimal": Suite(
            "decimal",
            3,
            decimal,
            0x0200,
            250_000_000,
            1,
            expected_address=0x000B,
            expected_value=0,
            check_memory=True,
        ),
        "interrupt": Suite(
            "interrupt",
            4,
            interrupt,
            0x0400,
            20_000_000,
            1,
            feedback=True,
            expected_address=0x0203,
            expected_value=0,
            check_memory=True,
        ),
    }


def write_control(suite: Suite) -> None:
    (OUT_DIR / "current_program.bin").write_bytes(suite.image)
    control = struct.pack(
        "<HHIBBHBBBB",
        suite.start_pc,
        suite.pass_pc,
        suite.max_cycles,
        suite.termination_mode,
        1 if suite.feedback else 0,
        suite.expected_address,
        suite.expected_value,
        suite.suite_id,
        1 if suite.check_memory else 0,
        0,
    )
    if len(control) != 16:
        raise AssertionError(f"control block is {len(control)} bytes")
    (OUT_DIR / "current_config.bin").write_bytes(control)


def run_suite(suite: Suite) -> str:
    write_control(suite)
    suite_dir = OUT_DIR / suite.name
    suite_dir.mkdir(parents=True, exist_ok=True)
    command = [vivado_tool("xsim"), SNAPSHOT, "--runall", "--nolog"]
    completed_lines: list[str] = []
    with (suite_dir / "console.log").open("w", encoding="utf-8") as log:
        process = subprocess.Popen(
            command,
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        assert process.stdout is not None
        for line in process.stdout:
            completed_lines.append(line)
            log.write(line)
            log.flush()
            if "KLAUS PROGRESS" in line or "KLAUS FAIL" in line:
                print(line, end="", flush=True)
        return_code = process.wait()
    output = "".join(completed_lines)
    if return_code != 0:
        raise RuntimeError(
            f"{Path(command[0]).name} failed with exit code {return_code}"
        )
    pass_lines = [line for line in output.splitlines() if "KLAUS PASS" in line]
    fail_lines = [line for line in output.splitlines() if "KLAUS FAIL" in line]
    if fail_lines or not pass_lines:
        raise RuntimeError(
            f"Klaus {suite.name}: simulator did not report PASS; "
            f"see {suite_dir / 'console.log'}"
        )
    return pass_lines[-1].strip()


def git_revision(klaus_dir: Path) -> str:
    completed = subprocess.run(
        ["git", "-C", str(klaus_dir), "rev-parse", "HEAD"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    return completed.stdout.strip() if completed.returncode == 0 else "unknown"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--klaus-dir", type=Path, default=DEFAULT_KLAUS_DIR)
    parser.add_argument(
        "--suites",
        default="functional,extended,decimal,interrupt",
        help="comma-separated selection: functional,extended,decimal,interrupt",
    )
    parser.add_argument(
        "--no-compile", action="store_true", help="reuse the XSim snapshot"
    )
    args = parser.parse_args()

    klaus_dir = args.klaus_dir.resolve()
    requested = [item.strip().lower() for item in args.suites.split(",") if item]
    valid = {"functional", "extended", "decimal", "interrupt"}
    if not requested or any(item not in valid for item in requested):
        parser.error(f"--suites must contain only {', '.join(sorted(valid))}")
    if not klaus_dir.is_dir():
        parser.error(f"Klaus suite directory does not exist: {klaus_dir}")

    try:
        OUT_DIR.mkdir(parents=True, exist_ok=True)
        suites = prepare_suites(klaus_dir)
        if not args.no_compile:
            compile_snapshot()
        for name in requested:
            print(run_suite(suites[name]))
        print(
            f"Klaus W65C02 suites passed: {', '.join(requested)}; "
            f"revision={git_revision(klaus_dir)}"
        )
        return 0
    except (OSError, ValueError, RuntimeError, zipfile.BadZipFile) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
