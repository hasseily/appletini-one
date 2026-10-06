#!/usr/bin/env python3
"""Compare synthesizable SSI source RTL with the frozen host candidate.

This proves implementation agreement, not agreement with production silicon.
Windows uses WSL Ubuntu's Verilator; Linux uses the current PATH.
"""
from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import shlex
import subprocess

from ssi263_host_data import load_tables, make_demo

ROOT = Path(__file__).resolve().parents[1]


def settle_bound() -> dict[str, int]:
    """Exhaust every Boolean gate state, including unreachable cold states."""
    cases = max_passes = 0
    for initial in itertools.product(range(16), range(2), range(2), range(2),
                                     range(2), range(2), range(8)):
        amp, u62, clock, pw3, va, fa, selector = initial
        for passes in range(1, 17):
            changed = False
            blocked = pw3 and not u62
            up = not blocked and (va or fa)
            not_carry = amp != (15 if up else 0)
            if (blocked or not_carry) and u62:
                u62 = False
                changed = True
            blocked = pw3 and not u62
            up = not blocked and (va or fa)
            not_carry = amp != (15 if up else 0)
            next_clock = bool(selector & 4) and not (
                (not not_carry and up) or (not up and amp & 14 == 0))
            if next_clock and not clock:
                amp = (amp + (1 if up else 15)) & 15
                changed = True
            changed |= next_clock != clock
            clock = next_clock
            if not changed:
                break
        assert passes <= 3, (initial, passes)
        max_passes = max(max_passes, passes)
        cases += 1
    return {"states": cases, "max_passes": max_passes}


def linux_path(path: Path) -> str:
    path = path.resolve()
    if os.name != "nt":
        return str(path)
    return "/mnt/" + path.drive[0].lower() + str(path)[2:].replace("\\", "/")


def run(command: list[str]) -> str:
    if os.name == "nt":
        command = ["wsl", "-d", "Ubuntu", "--", "bash", "-lc", shlex.join(command)]
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode:
        print(result.stdout)
        print(result.stderr)
        result.check_returncode()
    return result.stdout.strip()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/test_ssi263_native_source")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    bounds = settle_bound()
    trace = make_demo("hello_four", filter_frequency=231)
    events = sorted((e for e in trace.events if e.socket == 0), key=lambda e: e.tick)
    startup = build / "startup.txt"
    startup.write_text(
        f"{events[0].tick} {trace.duration_ticks} {len(events)}\n"
        + " ".join(map(str, load_tables()["native_rom"])) + "\n"
        + "".join(f"{e.tick} {e.register} {e.value}\n" for e in events), encoding="ascii")
    host = ROOT / "scripts/ssi263_host"
    fixture = ROOT / "scripts/fixtures/ssi263_native"
    rtl = ROOT / "hdl/apple/ssi263_native_source.sv"
    command = ["verilator", "--cc", "--exe", "--build", "-j", "4", "--top-module", "source_tb",
               "--Mdir", linux_path(build / "obj"), "-Wall", "-Wno-UNUSEDSIGNAL",
               "-CFLAGS", f"-std=c++17 -O2 -I{linux_path(host)}",
               linux_path(rtl), linux_path(fixture / "source_tb.sv"),
               linux_path(fixture / "source_compare.cpp"),
               linux_path(host / "native_source.cpp"), linux_path(host / "native_control.cpp")]
    output = run(command)
    (build / "build.log").write_text(output + "\n", encoding="utf-8")
    result = run([linux_path(build / "obj/Vsource_tb"), linux_path(startup)])
    print(result)
    print(f"PASS gate settling bound: {bounds['states']} states, at most {bounds['max_passes']} passes")
    sources = [rtl, host / "native_source.cpp", host / "native_source.h", fixture / "source_compare.cpp"]
    (build / "validation.json").write_text(json.dumps({
        "result": result, "settle_bound": bounds,
        "source_sha256": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in sources},
        "scope": "Frozen host candidate equivalence; physical SSI not yet verified",
        "vivado_used": False,
    }, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
