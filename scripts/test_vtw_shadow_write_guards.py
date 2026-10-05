#!/usr/bin/env python3
"""Check current TURBO write permission against its original retirement gate.

Requires xvlog/xelab/xsim on PATH. The production core is read only: the
equivalence oracle and a deliberate negative mutation exist only under --out.
The directed test reuses tb_vtw_turbo's real CPU, shadow, bus, and test helpers.
tb_vtw_system supplies the remaining legal completion states without rerunning
the full TURBO suite. Logs, source hashes, and coverage are saved in results.json.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import test_vtw


ROOT = Path(__file__).resolve().parents[1]
CORE_REL = "hdl/apple/vtw_core_top.sv"
ORACLE_REL = "hdl/sim/vtw_shadow_write_guard_oracle.svh"
CASES_REL = "hdl/sim/vtw_shadow_write_guard_cases.svh"
MISMATCH = "SHADOW WRITE GUARD EQUATION MISMATCH"
COVERAGE = re.compile(
    r"SHADOW WRITE GUARD COVERAGE (\S+) cycles=(\d+) writes=(\d+) "
    r"states=([0-9a-fA-F]+) speeds=([0-9a-fA-F]+) live_drop=(\d+)"
)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def run(command: list[str], directory: Path, log_name: str,
        *, allow_failure: bool = False) -> str:
    result = subprocess.run(command, cwd=directory, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (directory / log_name).write_text(result.stdout, encoding="utf-8")
    if result.returncode and not allow_failure:
        raise RuntimeError(f"{Path(command[0]).name} failed; see {directory / log_name}")
    return result.stdout


def prepare_core(source: str, oracle: str, directory: Path,
                 *, remove_pause: bool = False) -> Path:
    if remove_pause:
        assignment = re.search(r"assign\s+turbo_shadow_write\s*=([^;]+);", source)
        require(assignment is not None, "Cannot locate the write permission for negative test")
        body, count = re.subn(r"!pause\s*&&\s*", "", assignment.group(1))
        require(count == 1, "Negative test expects one direct pause guard; update mutation for new RTL")
        source = source[:assignment.start(1)] + body + source[assignment.end(1):]
    require(len(re.findall(r"\bendmodule\b", source)) == 1,
            "Expected one core module for out-of-tree instrumentation")
    instrumented = re.sub(r"\bendmodule\b", lambda _: oracle + "\nendmodule", source)
    path = directory / "vtw_core_top.instrumented.sv"
    path.write_text(instrumented, encoding="utf-8")
    return path


def compile_variant(source: str, oracle: str, directory: Path,
                    *, remove_pause: bool = False) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    core = prepare_core(source, oracle, directory, remove_pause=remove_pause)
    for memory in test_vtw.MEM_FILES:
        shutil.copyfile(ROOT / memory, directory / Path(memory).name)
    sources = [str(core if name == CORE_REL else ROOT / name)
               for name in test_vtw.SOURCES]
    run([test_vtw.vivado_tool("xvlog"), "--sv", "--define", "VTW_SHADOW_WRITE_GUARDS",
         "--include", str(ROOT / "hdl/sim"), *sources], directory, "xvlog.log")


def simulate(bench: str, directory: Path, *, allow_failure: bool = False) -> str:
    snapshot = bench + "_guard_snap"
    run([test_vtw.vivado_tool("xelab"), bench, "-s", snapshot,
         "--timescale", "1ns/1ps", "-L", "unisims_ver"],
        directory, f"xelab_{bench}.log")
    return run([test_vtw.vivado_tool("xsim"), snapshot, "--runall"],
               directory, f"xsim_{bench}.log", allow_failure=allow_failure)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "build/vtw_shadow_write_guards")
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    # Do not leave an earlier success artifact if this invocation fails.
    (out / "results.json").write_text('{"status": "RUNNING"}\n', encoding="utf-8")
    tracked = set(test_vtw.SOURCES + test_vtw.MEM_FILES +
                  [ORACLE_REL, CASES_REL, "scripts/test_vtw.py",
                   "scripts/test_vtw_shadow_write_guards.py"])
    before = {name: sha256(ROOT / name) for name in sorted(tracked)}
    source = (ROOT / CORE_REL).read_text(encoding="utf-8")
    oracle = (ROOT / ORACLE_REL).read_text(encoding="utf-8")
    try:
        test_vtw.static_checks()
        positive = out / "positive"
        compile_variant(source, oracle, positive)
        coverage = []
        for bench, marker in (("tb_vtw_turbo", "SHADOW WRITE GUARDS PASS"),
                              ("tb_vtw_system", "VTW SYSTEM PASS")):
            text = simulate(bench, positive)
            require(marker in text and not re.search(r"\b(?:FAIL|Fatal|ERROR)\b", text),
                    f"{bench} failed; see {positive / ('xsim_' + bench + '.log')}")
            matches = list(COVERAGE.finditer(text))
            require(len(matches) == 1, f"Expected one core coverage record from {bench}")
            instance, cycles, writes, states, speeds, drops = matches[0].groups()
            coverage.append(dict(bench=bench, instance=instance, cycles=int(cycles),
                                 writes=int(writes), state_mask=int(states, 16),
                                 speed_mask=int(speeds, 16), live_drop=int(drops)))
            print(f"PASS {bench}", flush=True)

        enum = re.search(r"typedef\s+enum\s+logic\s*\[4:0\]\s*\{(.*?)\}\s*xstate_t", source, re.S)
        require(enum is not None, "Cannot read the operational state enum")
        enum_text = re.sub(r"//[^\n]*|/\*.*?\*/", "", enum.group(1), flags=re.S)
        require("=" not in enum_text, "Coverage decoder expects sequential enum values")
        states = re.findall(r"\bX_[A-Z0-9_]+\b", enum_text)
        state_mask = speed_mask = 0
        for item in coverage:
            state_mask |= item["state_mask"]
            speed_mask |= item["speed_mask"]
        require(state_mask == (1 << len(states)) - 1,
                f"Legal-state coverage incomplete: observed={state_mask:08x}, states={states}")
        require(speed_mask == 0xF, "Not all four configured speed modes were observed")
        require(sum(item["writes"] for item in coverage) > 0, "No real TURBO writes observed")
        require(sum(item["live_drop"] for item in coverage) > 0,
                "No live session-loss versus registered-reset boundary observed")

        negative = out / "negative_missing_pause"
        compile_variant(source, oracle, negative, remove_pause=True)
        text = simulate("tb_vtw_turbo", negative, allow_failure=True)
        require(MISMATCH in text and "SHADOW WRITE GUARDS PASS" not in text,
                "Negative missing-pause mutation did not trip the equation oracle")
        print("PASS negative mutation: missing pause guard trips equation oracle", flush=True)
        require(before == {name: sha256(ROOT / name) for name in before},
                "Production or test input changed during this run")
        logs = {str(p.relative_to(out)): sha256(p)
                for p in sorted(out.rglob("*.log")) if "backup" not in p.name}
        result = dict(status="PASS", sources_unchanged=True, source_sha256=before,
                      coverage=coverage, legal_states=states, configured_speeds=4,
                      equation_checks=sum(item["cycles"] for item in coverage),
                      actual_turbo_writes=sum(item["writes"] for item in coverage),
                      directed_parked_write_cases=11, live_abort_cases=3,
                      mapping_and_ramworks_cases=2, status_pace_cases=4,
                      deferred_video_barrier_cases=1,
                      negative_mutation="Removed only !pause from temporary write assignment; oracle rejected",
                      log_sha256=logs,
                      limits=["State and speed coverage is aggregate, not every cross product.",
                              "No unknown-state or illegal physical multi-hot equivalence claim.",
                              "No synthesis, physical timing, or board performance claim."])
        (out / "results.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(f"PASS {result['equation_checks']:,} equation checks, {len(states)} legal states, four speeds")
        print(f"Evidence: {out / 'results.json'}")
        return 0
    except (OSError, RuntimeError) as exc:
        (out / "results.json").write_text(json.dumps(dict(status="FAIL", error=str(exc)), indent=2) + "\n")
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
