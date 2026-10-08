#!/usr/bin/env python3
"""Build and test the host SSI scanner/transition model without Vivado.

Arithmetic expectations use a closed-form linear ramp, not a second copy of
the C++ accumulator. The native cadence is an experiment, not a silicon oracle.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "scripts/ssi263_host"
ROM = ROOT / "hdl/apple/ssi263_sc02_rom.mem"

HARNESS = r'''
#include "native_control.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace ssi_host;

int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
Tables read_tables(const char* path) {
    Tables tables;
    std::ifstream stream(path);
    std::string line;
    int index = 0;
    while (std::getline(stream, line)) {
        line = line.substr(0, line.find("//"));
        std::istringstream fields(line);
        int value;
        while (fields >> std::hex >> value) {
            check(index < 512, "ROM overflow");
            tables.native_rom[index++] = value;
        }
    }
    check(index == 512, "ROM missing data");
    return tables;
}
void start(NativeControl& control, int phone = 0x8e, int rate = 10, int art = 5) {
    control.write(2, rate << 4);
    control.write(0, phone);
    control.write(3, (art << 4) | 15);
}
int main(int argc, char** argv) {
    try {
        check(argc == 2, "ROM argument");
        const Tables real = read_tables(argv[1]);
        for (int from = 0; from < 16; ++from) {
            for (int to = 0; to < 16; ++to) {
                NativeDda state;
                state.a = from;
                state.retarget(to);
                check(state.a == from, "retarget must retain A");
                const int delta = std::abs(to - from);
                for (int n = 1; n <= 32; ++n) {
                    state.step();
                    const int amount = std::min(delta, (n * delta + (to >= from ? 8 : 7)) / 16);
                    const int expected = from + (to >= from ? amount : -amount);
                    check(state.a == expected, "DDA must follow uniform linear ramp");
                    check(state.a >= std::min(from, to) && state.a <= std::max(from, to), "DDA overshoot");
                }
            }
        }
        // Every target pair can be interrupted and re-targeted from held A.
        for (int to = 0; to < 16; ++to) {
            NativeDda state;
            state.retarget(15);
            for (int n = 0; n < 7; ++n) state.step();
            const int before = state.a;
            state.retarget(to);
            check(state.a == before, "retarget continuity");
            for (int n = 0; n < 16; ++n) state.step();
            check(state.a == to, "interrupted DDA reaches new target");
        }

        NativeControl scan(real);
        scan.advance_xck(1);
        check(scan.metrics().write_phases == 0, "no early WRITE");
        scan.advance_xck(1);
        check(scan.metrics().write_phases == 1, "WRITE at r2");
        scan.advance_xck(7);
        check(scan.metrics().latch_phases == 0, "no early LATCH");
        scan.advance_xck(1);
        check(scan.metrics().latch_phases == 1, "LATCH at r10");
        scan.advance_xck(118);
        check(scan.selector() == 0 && scan.selector_phase() == 0, "128 XCK scan");
        check(scan.metrics().write_phases == 8 && scan.metrics().latch_phases == 8, "eight independent slots");
        check(scan.metrics().scans == 1, "one complete scan");
        check(scan.pw3() == -1 && scan.u20() == -1, "unknown source state not invented");

        for (int phone = 0; phone < 64; ++phone) {
            NativeControl control(real);
            start(control, phone | 0x80, 15, 7);
            control.advance_xck(192);
            for (int selector = 0; selector < 7; ++selector) {
                const int expected = selector == 4 ? 15 : real.native_rom[8 * phone + selector] >> 4;
                check(control.parameter_state(selector).target == expected, "all native ROM targets address correctly");
            }
            check(control.parameter_state(7).a == 0 && control.parameter_state(7).target == 0, "slot7 must not write");
            control.write(3, 0x70);
            control.advance_xck(256);
            check(control.parameter_state(4).target == 15, "AMP zero retains stored filter-amplitude target");
            for (int selector : {5, 6})
                check(control.parameter_state(selector).target == 0, "host amplitude zero clears source targets");
        }

        // A synthetic row isolates scan/transition latency from source gates.
        Tables isolated{};
        isolated.native_rom[0] = 0xF1;
        isolated.native_rom[1] = 0xA1;
        isolated.native_rom[2] = 0x74;
        isolated.native_rom[3] = 0x50;
        NativeControl latch(isolated, 1000000, NativeTiming{15});
        start(latch, 0x80, 15, 7);
        latch.advance_xck(385);
        check(latch.parameter_state(0).a == 0, "A holds until qualified WRITE");
        latch.advance_xck(1);
        check(latch.parameter_state(0).a == 1, "first linear WRITE");
        check(latch.parameter_codes()[0] == 0, "output must not follow WRITE early");
        latch.advance_xck(7);
        check(latch.parameter_codes()[0] == 0, "output holds before LATCH");
        latch.advance_xck(1);
        check(latch.parameter_codes()[0] == 1, "output follows LATCH separately");

        // The drawn first latches and PW0/PW1/PW3 stay transparent for r11.
        // Position the phone write so duration phase 2 begins on that tick.
        for (int selector : {0, 1}) {
            NativeControl transparent(isolated);
            transparent.advance_xck(16 * selector + 11);
            start(transparent, 0xc0, 15, 7);
            transparent.advance_xck(511);
            check(transparent.selector_phase() == 10, "transparent regression r10 alignment");
            check((selector == 0 ? transparent.pw0() : transparent.pw1()) == 0,
                  "PW start comparison still false at LATCH rising edge");
            const auto latch_edges = transparent.metrics().latch_phases;
            transparent.advance_xck(1);
            check(transparent.duration_phase() == 2 && transparent.selector_phase() == 11,
                  "duration transition occurs inside open LATCH window");
            check((selector == 0 ? transparent.pw0() : transparent.pw1()) == 1,
                  "PW latch follows comparison while r11 remains open");
            check(transparent.metrics().latch_phases == latch_edges, "r11 is level transparency, not another latch edge");
        }
        Tables phase_tables = isolated;
        phase_tables.native_rom[2] = 0x76;
        NativeControl pw3_window(phase_tables);
        start(pw3_window, 0xc0, 15, 7);
        pw3_window.advance_xck(554);
        check(pw3_window.selector() == 2 && pw3_window.selector_phase() == 10 && pw3_window.pw3() == 0,
              "PW3 transparent test precondition");
        pw3_window.write(3, 0xff);
        pw3_window.advance_xck(1);
        check(pw3_window.pw3() == 1, "PW3 sees CTRL while r11 latch remains open");

        // FF=255 makes r10 coincide with positive Phi0. U166 captures old
        // U20 on that edge, then the routes settle through distinct phases.
        for (int column = 0; column < 8; ++column)
            phase_tables.native_rom[8 + column] = phase_tables.native_rom[column];
        phase_tables.native_rom[2] = 0x74;
        phase_tables.native_rom[10] = 0x7c;
        NativeControl routes(phase_tables);
        start(routes, 0xc0, 15, 7);
        routes.set_ampct_zero(1);
        routes.advance_xck(600);
        check(routes.u20() == 0 && routes.fric1_sw() == 0 && routes.fric2_sw() == 1,
              "route coincidence precondition");
        routes.write(0, 0xc1);
        int wait = 0;
        while (routes.u20() == 0 && wait++ < 2048) routes.advance_xck(1);
        check(routes.u20() == 1 && routes.filter_phase_edge() && !routes.filter_phase(),
              "U20 transition coincides with Phi0");
        check(routes.fric1_sw() == 0 && routes.fric2_sw() == 1,
              "coincident Phi0 must capture old U20");
        routes.advance_xck(1);
        check(routes.fric1_sw() == 1 && routes.fric2_sw() == 1,
              "Phi1 opens FRIC1 while FRIC2 holds prior sample");
        routes.advance_xck(1);
        check(routes.fric1_sw() == 1 && routes.fric2_sw() == 0,
              "next Phi0 completes independent route transition");

        NativeControl slow(isolated), fast(isolated);
        start(slow, 0x80, 0, 5);
        start(fast, 0x80, 15, 5);
        check(slow.articulation_period_ticks() == 6144, "explicit fixed reference cadence");
        check(slow.articulation_period_ticks() == fast.articulation_period_ticks(), "articulation independent RATE");
        for (int n = 0; n < 1000; ++n) {
            slow.advance_xck(128);
            fast.advance_xck(128);
            for (int selector : {0, 1, 2, 3})
                check(slow.parameter_codes()[selector] == fast.parameter_codes()[selector], "RATE must not alter formant ramp");
        }
        check(slow.metrics().duration_edges != fast.metrics().duration_edges, "RATE still changes duration");
        const auto held = fast.parameter_codes();
        fast.write(0, 0x80);
        check(fast.parameter_codes() == held, "phone write retains native parameters");
        check(fast.pw0() == 0 && fast.pw1() == 0, "phone write clears only PW0/PW1");
        check(fast.pw2() == 1 && fast.pw5() == 0, "other PW state retained");

        for (int phone : {0x2c, 0x2d}) {
            NativeControl control(real);
            start(control, phone | 0xc0, 15, 7);
            control.advance_xck(10000);
            check(control.pw1() == 1, "native PW1 qualification");
            check(control.pw3() == (phone == 0x2c ? 0 : 1), "HF/HFC held control distinction");
        }

        NativeControl duration(real);
        start(duration, 0x8e, 10, 5);
        check(duration.duration_period_ticks() == 3072, "duration formula");
        duration.advance_xck(3071);
        check(duration.duration_phase() == 0, "duration first interval full");
        duration.advance_xck(1);
        check(duration.duration_phase() == 1, "duration exact boundary");
        duration.write(3, 0xdf);
        duration.advance_xck(10000);
        check(duration.duration_phase() == 1, "CTL stop preserves phase");
        duration.write(3, 0x5f);
        check(duration.duration_phase() == 0, "CTL restart compatibility policy");
        duration.advance_xck(3071);
        check(duration.duration_phase() == 0, "CTL restart full first interval");
        duration.advance_xck(1);
        check(duration.duration_phase() == 1, "CTL restart boundary");

        NativeControl samples(real, 1015625), edges(real, 1015625);
        start(samples);
        start(edges);
        for (int n = 0; n < sample_rate; ++n) samples.advance_sample();
        edges.advance_xck(1015625);
        check(samples.metrics().xck_ticks == 1015625, "rational 48k scheduler no drift");
        check(samples.parameter_codes() == edges.parameter_codes(), "sample and edge schedules agree");
        check(samples.selector() == edges.selector() && samples.selector_phase() == edges.selector_phase(), "scanner phase independent of batching");
        NativeControl independent(real);
        check(independent.parameter_codes() != edges.parameter_codes(), "sockets have independent state");
        std::cout << "PASS " << checks << " native host control checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
'''


def compiler() -> str:
    result = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
    if result:
        return result
    candidates = list(Path("E:/AMDDesignTools").glob("*/tps/mingw/*/win64.o/nt/bin/g++.exe"))
    if candidates:
        return str(sorted(candidates)[-1])
    raise RuntimeError("Set CXX to a C++17 compiler (g++ or clang++)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/test_ssi263_host_control")
    args = parser.parse_args()
    data = bytes(int(field, 16) for line in ROM.read_text().splitlines()
                 for field in line.split("//")[0].split())
    digest = hashlib.sha256(data).hexdigest()
    if digest != "ea494f047de11c533cb36a51d8686949206cedb91ab4855bdf9bde9500f828d9":
        raise ValueError("Native ROM identity changed")
    args.build_dir.mkdir(parents=True, exist_ok=True)
    harness = args.build_dir / "test.cpp"
    harness.write_text(HARNESS, encoding="utf-8")
    executable = args.build_dir / ("test.exe" if os.name == "nt" else "test")
    cxx = compiler()
    cmd = [cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-pedantic", "-I", str(SOURCE),
           str(harness), str(SOURCE / "native_control.cpp"), "-o", str(executable)]
    if os.name == "nt":
        cmd += ["-static-libgcc", "-static-libstdc++"]
    subprocess.run(cmd, check=True)
    result = subprocess.run([str(executable), str(ROM)], check=True, text=True, capture_output=True)
    print(result.stdout.strip())
    (args.build_dir / "validation.json").write_text(json.dumps({
        "result": result.stdout.strip(), "compiler": cxx, "rom_sha256": digest,
        "native_control_sha256": hashlib.sha256((SOURCE / "native_control.cpp").read_bytes()).hexdigest(),
        "timing": "experimental fixed articulation reference R=8; live speech RATE excluded",
        "vivado_used": False,
    }, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
