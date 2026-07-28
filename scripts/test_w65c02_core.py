#!/usr/bin/env python3
"""Build and run W65C02 RTL against SingleStepTests/65x02 vectors.

The upstream JSON corpus is intentionally not vendored: the WDC set is about
1 GB. Point --vectors at SingleStepTests/65x02/wdc65c02/v1. Selected opcodes
are converted to compact fixed-size records consumed by one XSim process, so
JSON parsing and simulator startup stay outside the per-instruction test loop.
"""

from __future__ import annotations

import argparse
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_VECTOR_DIR = Path(r"C:\tmp\65x02-tests\wdc65c02\v1")
OUT_DIR = ROOT / "build" / "w65c02_vectors"
PAYLOAD_BYTES = 256
RECORD_BYTES = 260
SNAPSHOT = "tb_w65c02_vectors_snapshot"
DIRECTED_SNAPSHOT = "tb_w65c02_directed_snapshot"


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


def parse_opcodes(spec: str) -> list[int]:
    if spec.strip().lower() == "all":
        return list(range(256))

    result: set[int] = set()
    for item in spec.replace(" ", "").split(","):
        if not item:
            continue
        if "-" in item:
            first, last = item.split("-", 1)
            result.update(range(int(first, 16), int(last, 16) + 1))
        else:
            result.add(int(item, 16))
    if not result or any(opcode < 0 or opcode > 255 for opcode in result):
        raise ValueError(f"invalid opcode selection: {spec}")
    return sorted(result)


def put_u16(buffer: bytearray, value: int) -> None:
    buffer.extend(struct.pack("<H", value & 0xFFFF))


def put_state(buffer: bytearray, state: dict[str, object]) -> None:
    put_u16(buffer, int(state["pc"]))
    for name in ("s", "a", "x", "y", "p"):
        buffer.append(int(state[name]) & 0xFF)


def put_ram(buffer: bytearray, entries: list[list[object]]) -> None:
    if len(entries) > 255:
        raise ValueError(f"RAM entry count {len(entries)} exceeds record format")
    buffer.append(len(entries))
    for address, value in entries:
        put_u16(buffer, int(address))
        buffer.append(int(value) & 0xFF)


def normalize_known_cycle_quirks(
    test: dict[str, object], opcode: int
) -> tuple[list[list[object]], list[list[object]]]:
    """Correct known non-hardware bus cycles in the upstream WDC vectors."""
    initial = test["initial"]
    cycles = test["cycles"]
    assert isinstance(initial, dict)
    assert isinstance(cycles, list)

    initial_ram = [list(entry) for entry in initial["ram"]]
    normalized_cycles = [list(cycle) for cycle in cycles]

    # SingleStepTests currently reads the final instruction byte on the extra
    # cycle of every STA abs,X/abs,Y.  Apple-compatible 65C02 hardware instead
    # false-reads the effective address when indexing stays on the same page;
    # the instruction-byte read is the page-crossing behavior.
    if opcode in (0x99, 0x9D) and len(normalized_cycles) == 5:
        index = int(initial["y"] if opcode == 0x99 else initial["x"])
        base = (
            int(normalized_cycles[1][1])
            | (int(normalized_cycles[2][1]) << 8)
        )
        if ((base & 0xFF) + index) <= 0xFF:
            target = int(normalized_cycles[-1][0])
            ram_by_address = {
                int(address): int(value) for address, value in initial_ram
            }
            read_value = ram_by_address.get(
                target, int(normalized_cycles[-2][1])
            )
            if target not in ram_by_address:
                initial_ram.append([target, read_value])
            normalized_cycles[-2] = [target, read_value, "read"]

    return initial_ram, normalized_cycles


def pack_test(test: dict[str, object], opcode: int) -> bytes:
    initial = test["initial"]
    final = test["final"]
    assert isinstance(initial, dict)
    assert isinstance(final, dict)
    initial_ram, cycles = normalize_known_cycle_quirks(test, opcode)

    if len(cycles) > 255:
        raise ValueError(f"cycle count {len(cycles)} exceeds record format")

    record = bytearray()
    put_state(record, initial)
    put_ram(record, initial_ram)
    put_state(record, final)
    put_ram(record, final["ram"])
    record.append(len(cycles))
    for address, value, cycle_type in cycles:
        put_u16(record, int(address))
        record.append(int(value) & 0xFF)
        record.append(1 if cycle_type == "write" else 0)

    if len(record) > PAYLOAD_BYTES:
        raise ValueError(
            f"packed test needs {len(record)} bytes, limit is {PAYLOAD_BYTES}"
        )
    record.extend(bytes(PAYLOAD_BYTES - len(record)))
    record.extend(bytes((opcode & 0xFF, 0, 0, 0)))
    return bytes(record)


def pack_suite(
    vector_dir: Path,
    opcodes: list[int],
    limit: int,
    check_cycles: bool,
    output: Path,
) -> tuple[int, int, list[int]]:
    output.parent.mkdir(parents=True, exist_ok=True)
    total = 0
    opcode_count = 0
    skipped: list[int] = []

    with output.open("wb") as handle:
        handle.write(bytes(16))
        for opcode in opcodes:
            source = vector_dir / f"{opcode:02x}.json"
            if not source.is_file() or source.stat().st_size == 0:
                skipped.append(opcode)
                continue

            with source.open("r", encoding="utf-8") as source_handle:
                tests = json.load(source_handle)
            if limit > 0:
                tests = tests[:limit]
            if not tests:
                skipped.append(opcode)
                continue

            for test in tests:
                handle.write(pack_test(test, opcode))
            total += len(tests)
            opcode_count += 1

        handle.seek(0)
        handle.write(
            struct.pack(
                "<4sHHIBB2x",
                b"W65V",
                1,
                RECORD_BYTES,
                total,
                0,
                1 if check_cycles else 0,
            )
        )

    return total, opcode_count, skipped


def compile_snapshot() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    run(
        [
            vivado_tool("xvlog"),
            "--sv",
            str(ROOT / "hdl" / "apple" / "w65c02_core.sv"),
            str(ROOT / "hdl" / "sim" / "tb_w65c02_vectors.sv"),
            str(ROOT / "hdl" / "sim" / "tb_w65c02_directed.sv"),
        ],
        ROOT,
        OUT_DIR / "xvlog.log",
    )
    run(
        [vivado_tool("xelab"), "tb_w65c02_vectors", "-s", SNAPSHOT],
        ROOT,
        OUT_DIR / "xelab.log",
    )
    run(
        [vivado_tool("xelab"), "tb_w65c02_directed", "-s", DIRECTED_SNAPSHOT],
        ROOT,
        OUT_DIR / "xelab_directed.log",
    )


def run_directed() -> str:
    output = run(
        [vivado_tool("xsim"), DIRECTED_SNAPSHOT, "--runall"],
        ROOT,
        OUT_DIR / "directed.log",
    )
    pass_lines = [
        line for line in output.splitlines() if "W65C02 DIRECTED PASS" in line
    ]
    if not pass_lines:
        raise RuntimeError("directed simulator did not report PASS")
    return pass_lines[-1].strip()


def run_suite(packed: Path) -> str:
    suite_dir = OUT_DIR / "suite"
    suite_dir.mkdir(parents=True, exist_ok=True)
    # Keep the simulation snapshot reusable without XSim runtime plusargs.
    # XSim 2025.2 on Windows splits NAME=VALUE plusargs at the equals sign.
    shutil.copyfile(packed, OUT_DIR / "current_vectors.bin")
    command = [
        vivado_tool("xsim"),
        SNAPSHOT,
        "--runall",
        "--log",
        str(suite_dir / "xsim.log"),
        "--wdb",
        str(suite_dir / "xsim.wdb"),
    ]
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
            if "W65C02 PROGRESS" in line or "W65C02 FAIL" in line:
                print(line, end="", flush=True)
        return_code = process.wait()
    output = "".join(completed_lines)
    if return_code != 0:
        raise RuntimeError(
            f"{Path(command[0]).name} failed with exit code {return_code}"
        )
    pass_lines = [line for line in output.splitlines() if "W65C02 PASS" in line]
    if not pass_lines:
        raise RuntimeError("simulator did not report PASS")
    return pass_lines[-1].strip()


def git_revision(vector_dir: Path) -> str:
    current = vector_dir.resolve()
    for candidate in (current, *current.parents):
        if (candidate / ".git").exists():
            completed = subprocess.run(
                ["git", "-C", str(candidate), "rev-parse", "HEAD"],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
            )
            if completed.returncode == 0:
                return completed.stdout.strip()
            break
    return "unknown"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--vectors",
        type=Path,
        default=DEFAULT_VECTOR_DIR,
        help="SingleStepTests/65x02 wdc65c02/v1 directory",
    )
    parser.add_argument(
        "--opcodes",
        default="00,03,09,20,28,4c,60,69,6c,7c,80,8d,a9,bd,d0,e9,ff",
        help="comma-separated hex opcodes/ranges, or 'all'",
    )
    parser.add_argument(
        "--limit",
        type=int,
        default=100,
        help="vectors per opcode; zero means all 10,000",
    )
    parser.add_argument(
        "--state-only",
        action="store_true",
        help="check architectural and memory state but not individual bus cycles",
    )
    parser.add_argument(
        "--no-compile",
        action="store_true",
        help="reuse the existing XSim snapshot",
    )
    args = parser.parse_args()

    vector_dir = args.vectors.resolve()
    if not vector_dir.is_dir():
        parser.error(f"vector directory does not exist: {vector_dir}")
    if args.limit < 0:
        parser.error("--limit must be zero or positive")

    try:
        opcodes = parse_opcodes(args.opcodes)
        if not args.no_compile:
            compile_snapshot()
        print(run_directed())

        packed = OUT_DIR / "suite" / "vectors.bin"
        total, passed_opcodes, skipped = pack_suite(
            vector_dir,
            opcodes,
            args.limit,
            not args.state_only,
            packed,
        )
        for opcode in skipped:
            print(f"SKIP opcode={opcode:02x}: no single-step vectors")

        if passed_opcodes == 0:
            raise RuntimeError("no opcode vector files were executed")

        print(run_suite(packed))
        print(
            f"W65C02 vector suite passed: {total} tests across "
            f"{passed_opcodes} opcodes; vectors={git_revision(vector_dir)}; "
            f"cycle_check={0 if args.state_only else 1}"
        )
        return 0
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
