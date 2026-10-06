#!/usr/bin/env python3
"""Build native FPGA audio, compare stereo PCM with the listening checkpoint.

Verilator runs locally on Linux or in Ubuntu WSL on Windows. Optional Vivado
out-of-context implementation checks this engine only, never a board image.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import shlex
import shutil
import subprocess
import wave

from render_ssi263 import build_host, render, load_json_trace
from ssi263_host_data import Event, Trace, make_demo, write_tables

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/test_ssi263_native_engine"
RTL = [ROOT / "hdl/apple" / name for name in (
    "ssi263_parameter_rom.sv", "ssi263_native_controller.sv", "ssi263_native_pitch.sv",
    "ssi263_native_source.sv", "ssi263_native_tract.sv", "ssi263_native_engine.sv")]
HOST = ROOT / "scripts/ssi263_host"
CPP = ROOT / "scripts/fixtures/ssi263_native/engine_replay.cpp"
ROM = ROOT / "hdl/apple/ssi263_sc02_rom.mem"


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def linux_path(path: Path) -> str:
    if os.name != "nt":
        return str(path.resolve())
    p = path.resolve().as_posix()
    return "/mnt/" + p[0].lower() + p[2:]


def linux_run(args: list[str], log: Path, cwd: Path = BUILD) -> str:
    if os.name == "nt":
        command = ["wsl", "-d", "Ubuntu", "--", "sh", "-lc",
                   "cd " + shlex.quote(linux_path(cwd)) + " && " + shlex.join(args)]
    else:
        command = args
    result = subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    log.write_text(result.stdout, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"{log}:\n{result.stdout[-10000:]}")
    return result.stdout


def build() -> Path:
    BUILD.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROM, BUILD / ROM.name)
    sources = [*RTL, CPP, Path(__file__), *sorted(HOST.glob("*.h")),
               HOST / "native_control.cpp", HOST / "native_source.cpp", HOST / "prototype_tract.cpp"]
    hashes = {p.relative_to(ROOT).as_posix(): digest(p) for p in sources}
    stamp = BUILD / "build.json"
    exe = BUILD / "obj/Vssi263_native_engine"
    if exe.exists() and stamp.exists() and json.loads(stamp.read_text()).get("sha256") == hashes:
        return exe
    command = ["verilator", "--cc", "--exe", "--build", "-j", "4",
               "--top-module", "ssi263_native_engine", "--Mdir", linux_path(BUILD / "obj"),
               "-CFLAGS", "-O2 -std=c++17 -I" + linux_path(HOST),
               *[linux_path(p) for p in RTL], linux_path(CPP),
               *[linux_path(HOST / f) for f in ("native_control.cpp", "native_source.cpp", "prototype_tract.cpp")]]
    linux_run(command, BUILD / "compile.log")
    if any(digest(ROOT / p) != h for p, h in hashes.items()):
        raise RuntimeError("Sources changed during compilation; rerun after edits finish")
    stamp.write_text(json.dumps({"sha256": hashes, "command": command}, indent=2) + "\n")
    return exe


def stress_trace() -> Trace:
    # All phones, register aliases, stereo independence, all clock endpoints,
    # zero/nonzero AMP, CTL, and both immediate/transitioned inflection modes.
    rng = random.Random(263)
    events = []
    for s in (0, 1):
        for reg, val in ((0, 0xc0 if s else 0x80), (1, 255), (2, 255),
                         (4, 255), (3, 0x7f)):
            events.append(Event(0, s, reg, val))
    for phone in range(64):
        at = 256 + phone * 1536
        for s in (0, 1):
            events += [Event(at+s, s, 0, (rng.randrange(4)<<6)|phone),
                       Event(at+41+s, s, 2, rng.randrange(256)),
                       Event(at+82+s, s, 1, rng.randrange(256)),
                       Event(at+123+s, s, 3, rng.choice((0, 0x0f, 0x7f, 0x80, 0x5c))),
                       Event(at+164+s, s, 4+phone%4, (0, 128, 231, 254, 255)[phone%5])]
    # A final dense fastest-clock interval must finish each update on time.
    for s in (0,1):
        events += [Event(99000+s,s,3,0x7f), Event(99002+s,s,4,255),
                   Event(99004+s,s,2,255), Event(99006+s,s,1,255)]
    events.sort(key=lambda e:e.tick)
    return Trace(events, 1020484, 105000, {"name":"native_stress"})


def replay(name: str, trace: Trace, exe: Path) -> dict:
    out = BUILD / name
    out.mkdir(parents=True, exist_ok=True)
    trace.validate()
    tables = out / "tables.txt"
    write_tables(tables)
    events = out / "events.txt"
    events.write_text(f"SSIHOST1 {trace.xck_hz} 0 {trace.duration_ticks} {len(trace.events)}\n" +
                      "".join(f"{e.tick} {e.socket} {e.register} {e.value}\n" for e in trace.events))
    pcm = out / "rtl.pcm"
    result = linux_run([linux_path(exe), linux_path(tables), linux_path(events), linux_path(pcm)],
                       out / "simulation.log")
    metrics = json.loads(result.strip().splitlines()[-1])
    # Compare the public renderer as well as the per-sample in-process oracle.
    render(trace, out / "host", "prototype", executable=build_host())
    host_wav = out / "host/prototype.wav"
    with wave.open(str(host_wav), "rb") as reader:
        expected = reader.readframes(reader.getnframes())
    actual = pcm.read_bytes()
    if actual != expected:
        raise AssertionError(f"{name}: standalone renderer and RTL PCM differ")
    with wave.open(str(out / "rtl.wav"), "wb") as writer:
        writer.setnchannels(2); writer.setsampwidth(2); writer.setframerate(48000)
        writer.writeframes(actual)
    metrics.update(name=name, pcm_sha256=hashlib.sha256(actual).hexdigest(),
                   stereo_frames=len(actual)//4, identical_to_frozen_renderer=True)
    (out / "validation.json").write_text(json.dumps(metrics, indent=2) + "\n")
    print(json.dumps(metrics), flush=True)
    return metrics


def synthesize() -> dict:
    source_hashes = {p.relative_to(ROOT).as_posix(): digest(p) for p in (*RTL, ROM)}
    tool = shutil.which("vivado.bat") or shutil.which("vivado")
    if not tool:
        raise RuntimeError("Vivado is required for --synth")
    tcl = BUILD / "implement_engine.tcl"
    tcl.write_text("".join(f"read_verilog -sv {{{p.as_posix()}}}\n" for p in RTL) +
        "synth_design -top ssi263_native_engine -part xc7z020clg484-2 -mode out_of_context\n"
        "create_clock -name clk -period 7.5 [get_ports clk]\n"
        "report_utilization -file synthesis_utilization.rpt\n"
        "opt_design\nplace_design\nphys_opt_design\nroute_design\n"
        "report_utilization -file routed_utilization.rpt\n"
        "report_timing_summary -file routed_timing.rpt\n"
        "report_drc -file routed_drc.rpt\n"
        "write_checkpoint -force native_engine_routed.dcp\n"
        "set p [get_timing_paths -max_paths 1 -setup]\n"
        "if {[llength $p] == 0} {error {No internal timing path found}}\n"
        "set slack [get_property SLACK $p]\n"
        "puts \"NATIVE ENGINE ROUTED SLACK=$slack\"\n"
        "if {$slack < 0} {error {Native engine missed fabric timing}}\n"
        "set h [get_timing_paths -max_paths 1 -hold]\n"
        "if {[llength $h] == 0} {error {No internal hold path found}}\n"
        "set hold_slack [get_property SLACK $h]\n"
        "puts \"NATIVE ENGINE ROUTED HOLD=$hold_slack\"\n"
        "if {$hold_slack < 0} {error {Native engine missed hold timing}}\n")
    result = subprocess.run([tool,"-mode","batch","-source",str(tcl)], cwd=BUILD,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (BUILD / "implementation.log").write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(f"Isolated implementation failed; see {BUILD / 'implementation.log'}")
    if any(digest(ROOT / p) != h for p, h in source_hashes.items()):
        raise RuntimeError("Sources changed during implementation; routed result is stale")
    import re
    setup = re.search(r"NATIVE ENGINE ROUTED SLACK=(-?[0-9.]+)", result.stdout)
    hold = re.search(r"NATIVE ENGINE ROUTED HOLD=(-?[0-9.]+)", result.stdout)
    if not setup or not hold:
        raise RuntimeError("Missing routed timing results")
    return {"part":"xc7z020clg484-2", "period_ns":7.5,
            "setup_slack_ns":float(setup[1]), "hold_slack_ns":float(hold[1]),
            "source_sha256":source_hashes,
            "scope":"standalone internal paths; no board IO delays or physical clock source"}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--quick", action="store_true", help="use a short prefix while developing")
    parser.add_argument("--trace", type=Path, help="replay an existing host trace JSON")
    parser.add_argument("--synth", action="store_true", help="also route the standalone engine in Vivado")
    args = parser.parse_args()
    BUILD.mkdir(parents=True, exist_ok=True)
    result = {"status":"running", "cases":[], "isolated_routed":False,
              "firmware_installed":False, "production_audio_connected":False}
    report = BUILD / "validation.json"
    report.write_text(json.dumps(result, indent=2) + "\n")
    try:
        exe = build()
        compiled_hashes = json.loads((BUILD / "build.json").read_text())["sha256"]
        demo = load_json_trace(args.trace) if args.trace else make_demo("hello_four", filter_frequency=231)
        if args.quick:
            end = min(demo.duration_ticks, demo.xck_hz//4)
            demo = Trace([e for e in demo.events if e.tick<end], demo.xck_hz, end, demo.metadata)
        result["cases"] = [replay("listening", demo, exe), replay("stress", stress_trace(), exe)]
        if any(digest(ROOT / p) != h for p, h in compiled_hashes.items()):
            raise RuntimeError("Sources changed during replay; rerun after edits finish")
        if args.synth:
            result["implementation"] = synthesize()
            result["isolated_routed"] = True
        if any(digest(ROOT / p) != h for p, h in compiled_hashes.items()):
            raise RuntimeError("Sources changed during validation; rerun after edits finish")
        result["status"] = "passed"
        result["source_sha256"] = {p.relative_to(ROOT).as_posix():digest(p) for p in (*RTL, CPP, ROM)}
    except Exception as error:
        result.update(status="failed", error=str(error))
        raise
    finally:
        report.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
