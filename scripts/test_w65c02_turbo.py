#!/usr/bin/env python3
"""Validate relaxed W65C02 timing without changing the cycle-exact suites.

Run in an isolated XSim work directory. Reuse the independent SingleStepTests
architectural results and Klaus programs, with generated harnesses that wire
turbo explicitly. The committed differential bench also compares all writes
and Apple I/O reads, mode changes, stalls, and opcode coverage.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import test_w65c02_core as vectors
import test_w65c02_klaus as klaus


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "w65c02_turbo"


def replace_once(text: str, old: str, new: str) -> str:
    if text.count(old) != 1:
        raise ValueError(f"expected one harness fragment: {old!r}")
    return text.replace(old, new, 1)


def generated_harness(name: str, turbo: bool) -> tuple[str, Path]:
    original = f"tb_w65c02_{name}"
    top = f"{original}_{'turbo' if turbo else 'classic'}"
    text = (ROOT / "hdl" / "sim" / f"{original}.sv").read_text()
    text = replace_once(text, f"module {original};", f"module {top};")
    text = replace_once(
        text, ".ready(ready),",
        f".ready(ready),\n        .turbo(1'b{int(turbo)}),",
    )
    if name == "vectors":
        text = replace_once(
            text, "build/w65c02_vectors/current_vectors.bin", "vectors.bin"
        )
        if turbo:
            # Keep the classic cycle count as an upper bound, but retire at
            # the core's instruction boundary before comparing reference state.
            text = replace_once(text, "check_cycles = header[13];", "check_cycles = 0;")
            text = replace_once(
                text,
                "cycle_index < cycle_count; cycle_index = cycle_index + 1",
                "cycle_index < cycle_count && !instruction_done; cycle_index = cycle_index + 1",
            )
    elif name == "klaus":
        text = text.replace("build/w65c02_klaus/", "klaus/")
    path = OUT / f"{top}.sv"
    path.write_text(text, encoding="utf-8")
    return top, path


def run_snapshot(top: str, marker: str, name: str | None = None) -> str:
    output = vectors.run(
        [vectors.vivado_tool("xsim"), top, "--runall", "--nolog"],
        OUT, OUT / f"{name or top}.log",
    )
    matches = [line for line in output.splitlines() if marker in line]
    if not matches or " FAIL" in output or "Fatal:" in output:
        raise RuntimeError(f"{top} did not pass; see {OUT / (top + '.log')}")
    print(matches[-1], flush=True)
    return matches[-1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vectors", type=Path, default=vectors.DEFAULT_VECTOR_DIR)
    parser.add_argument("--limit", type=int, default=100,
                        help="vectors per opcode; zero runs the full corpus")
    parser.add_argument("--klaus-dir", type=Path, default=klaus.DEFAULT_KLAUS_DIR)
    parser.add_argument("--skip-klaus", action="store_true")
    parser.add_argument("--no-compile", action="store_true")
    args = parser.parse_args()
    if args.limit < 0:
        parser.error("--limit must be nonnegative")
    OUT.mkdir(parents=True, exist_ok=True)
    try:
        harnesses = [generated_harness("vectors", turbo) for turbo in (False, True)]
        if not args.skip_klaus:
            harnesses.extend(generated_harness("klaus", turbo) for turbo in (False, True))
        tops = ["tb_w65c02_turbo", "tb_w65c02_directed"] + [top for top, _ in harnesses]
        if not args.no_compile:
            sources = [ROOT / "hdl/apple/w65c02_core.sv",
                       ROOT / "hdl/sim/tb_w65c02_turbo.sv",
                       ROOT / "hdl/sim/tb_w65c02_directed.sv"]
            sources.extend(path for _, path in harnesses)
            vectors.run([vectors.vivado_tool("xvlog"), "--sv", *map(str, sources)],
                        OUT, OUT / "xvlog.log")
            for top in tops:
                vectors.run([vectors.vivado_tool("xelab"), top, "-s", top],
                            OUT, OUT / f"{top}_xelab.log")
        run_snapshot("tb_w65c02_directed", "W65C02 DIRECTED PASS")
        run_snapshot("tb_w65c02_turbo", "W65C02 TURBO PASS")
        count, opcode_count, skipped = vectors.pack_suite(
            args.vectors.resolve(), list(range(256)), args.limit, True, OUT / "vectors.bin"
        )
        if count == 0:
            raise RuntimeError("no instruction vectors found")
        for top in ("tb_w65c02_vectors_classic", "tb_w65c02_vectors_turbo"):
            run_snapshot(top, "W65C02 PASS")
        print(f"Independent vectors: {count} per mode across {opcode_count} opcodes; "
              f"skipped={','.join(f'{op:02x}' for op in skipped)}; "
              f"revision={vectors.git_revision(args.vectors)}", flush=True)
        if not args.skip_klaus:
            # All generated/third-party artifacts stay in this isolated run.
            klaus.ROOT = OUT
            klaus.OUT_DIR = OUT / "klaus"
            klaus.OUT_DIR.mkdir(parents=True, exist_ok=True)
            suites = klaus.prepare_suites(args.klaus_dir.resolve())
            for turbo in (False, True):
                klaus.SNAPSHOT = f"tb_w65c02_klaus_{'turbo' if turbo else 'classic'}"
                for name in ("functional", "extended", "decimal", "interrupt"):
                    result = klaus.run_suite(suites[name])
                    log = klaus.OUT_DIR / name / "console.log"
                    (klaus.OUT_DIR / name / f"{'turbo' if turbo else 'classic'}.log").write_text(
                        log.read_text(), encoding="utf-8"
                    )
                    print(f"turbo={int(turbo)} {result}", flush=True)
            print(f"Klaus revision={klaus.git_revision(args.klaus_dir)}", flush=True)
        return 0
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
