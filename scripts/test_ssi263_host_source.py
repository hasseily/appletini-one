#!/usr/bin/env python3
"""Test optional prototype host excitation without Vivado or physical hardware."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from test_ssi263_host_control import compiler
from ssi263_host_data import load_tables, make_demo

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "scripts/ssi263_host"


def noise_vectors() -> str:
    # Independent explicit stages in wire order Q1..Qn, rather than packed
    # C++ shifts. Rising U41C clocks U75; falling U41C shifts the CD4006.
    d1, d2, d3, d4 = [1, 0, 0, 0], [0] * 5, [0] * 4, [0] * 5
    count = 15
    rows = []
    for _ in range(32768):
        count = 1 if count == 15 else count + 1
        force = int(count < 4)
        feedback = force ^ d1[-1] ^ d2[-1] ^ d4[-2] ^ d4[-1]
        d1, d2, d3, d4 = ([d3[-1]] + d1[:-1], [d4[-1]] + d2[:-1],
                          [d2[-1]] + d3[:-1], [feedback] + d4[:-1])
        values = [sum(bit << shift for shift, bit in enumerate(bits)) for bits in (d1, d2, d3, d4)]
        rows.append(" ".join(map(str, [count] + values)))
    return "\n".join(rows) + "\n"


HARNESS = r'''
#include "native_source.h"
#include "native_control.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace ssi_host;
int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void charge_envelope(NativeSource& source, NativeSourceInputs& inputs) {
    inputs.powered_down = false;
    inputs.pw3 = 0;
    inputs.phase = false;
    inputs.phase_edge = false;
    inputs.codes[5] = 15;
    for (int n = 0; n < 15; ++n) {
        inputs.selector = 0;
        source.tick(inputs);
        inputs.selector = 4;
        source.tick(inputs);
    }
}
int main(int argc, char** argv) {
    try {
        check(argc == 3, "noise and startup vector arguments");
        NativeNoise noise;
        std::ifstream vectors(argv[1]);
        int count, d1, d2, d3, d4, rows = 0;
        while (vectors >> count >> d1 >> d2 >> d3 >> d4) {
            const int old_d1 = noise.d1, old_d2 = noise.d2, old_d3 = noise.d3, old_d4 = noise.d4;
            noise.rising_edge();
            check(noise.count == count, "U75 preset/wrap");
            check(noise.d1 == old_d1 && noise.d2 == old_d2 && noise.d3 == old_d3 && noise.d4 == old_d4,
                  "rising edge must not shift CD4006");
            noise.falling_edge();
            check(noise.d1 == d1 && noise.d2 == d2 && noise.d3 == d3 && noise.d4 == d4, "independent CD4006 stages");
            check(noise.count == count, "falling edge must not count U75");
            ++rows;
        }
        check(rows == 32768, "all noise vectors consumed");

        NativeSource source;
        NativeSourceInputs inputs;
        check(source.ampct() == 0 && source.glottal_count() == 15, "labeled deterministic cold seeds");
        inputs.powered_down = false;
        inputs.pw3 = 0;
        inputs.selector = 4;
        source.tick(inputs);
        check(source.ampct() == 0, "no source amplitude keeps U68 stopped");
        inputs.codes[5] = 15;
        source.tick(inputs);
        check(source.ampct() == 1, "gate opening during SEL2 high clocks U68");
        for (int n = 0; n < 20; ++n) source.tick(inputs);
        check(source.ampct() == 1, "held high gate must not count repeatedly");
        for (int n = 0; n < 20; ++n) {
            inputs.selector = 0;
            source.tick(inputs);
            inputs.selector = 4;
            source.tick(inputs);
        }
        check(source.ampct() == 15, "U68 stops at binary upper terminal");
        check(source.ampct_zero() == 0, "upper envelope nonzero");
        inputs.codes[4] = 15;
        inputs.phase_edge = true;
        source.tick(inputs);
        check(source.output().filter_amp == 15, "U206 samples masked amplitude at Phi0");
        inputs.phase_edge = false;
        inputs.codes[4] = 0;
        source.tick(inputs);
        check(source.output().filter_amp == 15, "U206 holds outside positive Phi0");
        inputs.pw3 = 1;
        source.tick(inputs);
        check(source.ampct() == 14, "PW3 closes source and opens down-count gate");
        for (int n = 0; n < 20; ++n) {
            inputs.selector = 0;
            source.tick(inputs);
            inputs.selector = 4;
            source.tick(inputs);
        }
        check(source.ampct() == 1 && source.ampct_zero() == 1, "AMPCT_ZERO uses Q2..Q4, not Q1");

        NativeSource latches;
        NativeSourceInputs l;
        l.codes = {1, 2, 3, 4, 5, 6, 7, 0};
        latches.tick(l);
        check(latches.output().f1 == 1 && latches.output().f3 == 4 && latches.output().voice_amp == 6,
              "Phi0 transparent parameter bank");
        check(latches.output().f2 == 0 && latches.output().f4 == 0 && latches.output().fric_amp == 0,
              "Phi1 bank holds while closed");
        l.phase = true;
        latches.tick(l);
        check(latches.output().f2 == 2 && latches.output().f2q == 3 && latches.output().f4 == 4 && latches.output().fric_amp == 7,
              "Phi1 transparent parameter bank");
        l.codes[3] = 12;
        latches.tick(l);
        check(latches.output().f3 == 4 && latches.output().f4 == 12, "F3/F4 share target but have separate phase holds");
        l.phase_edge = true;
        latches.tick(l);
        check(!latches.output().output_open, "Phi1 does not sample reconstruction");
        l.phase = false;
        latches.tick(l);
        check(latches.output().output_open, "positive Phi0 samples reconstruction");

        NativeSource pitch;
        NativeSourceInputs p;
        charge_envelope(pitch, p);
        check(pitch.ampct() == 15, "pitch test envelope precondition");
        p.inflection = 0;
        p.selector = 0;
        while (pitch.metrics().xck_ticks < 16383) pitch.tick(p);
        check(pitch.metrics().voice_clock_edges == 0, "initial divider full period");
        pitch.tick(p);
        check(pitch.metrics().voice_clock_edges == 1 && pitch.voice_toggle(), "U62 toggles at first raw voice clock");
        p.phase = false;
        p.phase_edge = true;
        pitch.tick(p);
        check(pitch.glottal_load_pending(), "U61 records rising U62 at Phi0");
        p.phase = true;
        pitch.tick(p);
        check(pitch.glottal_count() == 11 && pitch.output().voice_target_q16 == -16384, "U60 loads 1011 at Phi1 with accepted voice balance");
        for (int expected = 12; expected <= 15; ++expected) {
            p.phase = false;
            pitch.tick(p);
            p.phase = true;
            pitch.tick(p);
            check(pitch.glottal_count() == expected, "U60 saturating four-Phi1 pulse");
            check(pitch.output().voice_target_q16 == (expected == 15 ? 0 : -16384), "U116 voice trim pulse");
        }
        p.phase_edge = false;
        p.inflection = 4095;
        const auto previous_edges = pitch.metrics().voice_clock_edges;
        while (pitch.voice_ticks_left() > 1) pitch.tick(p);
        check(pitch.metrics().voice_clock_edges == previous_edges, "pitch write does not reload running divider");
        pitch.tick(p);
        check(pitch.voice_ticks_left() == 4, "new pitch sampled on divider reload");
        const auto before = pitch.metrics().voice_clock_edges;
        for (int n = 0; n < 3; ++n) pitch.tick(p);
        check(pitch.metrics().voice_clock_edges == before, "I=4095 raw divider holds three edges");
        pitch.tick(p);
        check(pitch.metrics().voice_clock_edges == before + 1, "I=4095 raw divider fourth edge");

        NativeSource gated;
        NativeSourceInputs g;
        g.powered_down = false;
        g.pw3 = 0;
        g.phase = true;
        g.codes[6] = 8;
        gated.tick(g);
        check(gated.metrics().noise_clock_edges == 1 && gated.noise_state().count == 1, "U41C rise clocks U75");
        for (int n = 0; n < 10; ++n) gated.tick(g);
        check(gated.metrics().noise_clock_edges == 1, "held noise clock not repeated");
        g.selector = 2;
        gated.tick(g);
        check(gated.metrics().noise_shift_edges == 1, "U41C fall shifts CD4006");
        g.selector = 0;
        g.pw3 = 1;
        gated.tick(g);
        check(gated.metrics().noise_clock_edges == 1, "PW3/U62 closure inhibits noise clock");
        check(!gated.noise_bit() && gated.output().fric_drive_q16 == -301, "native noise source Boolean equation");
        g.powered_down = true;
        gated.tick(g);
        check(gated.output().voice_target_q16 == 0 && gated.output().fric_drive_q16 == 0, "powerdown suppresses excitation");

        Tables tables{};
        tables.native_rom[0] = 0xf1;
        tables.native_rom[1] = 0xa1;
        tables.native_rom[2] = 0x7e;
        tables.native_rom[3] = 0x50;
        tables.native_rom[5] = 0xf0;
        NativeControl control(tables);
        NativeSource integrated;
        control.write(0, 0x80);
        control.write(2, 0xa0);
        control.write(3, 0x5f);
        control.write(4, 0xe9);
        for (int n = 0; n < 500000; ++n) {
            control.set_ampct_zero(integrated.ampct_zero());
            control.advance_xck(1);
            integrated.tick(control, 3072);
        }
        check(integrated.metrics().xck_ticks == 500000, "integrated source advances exact XCK count");
        check(integrated.metrics().glottal_loads > 1, "integrated native scanner/envelope produces voiced pulses");
        check(integrated.output().voice_amp == 15 && integrated.ampct() == 15, "native amplitude settles");
        check(integrated.output().fric1 == true && integrated.output().fric2 == false, "native route request reaches held switches");

        // Replay the real four-hello demo from a cold instance. The first H
        // must use the same noise injection as later H sounds even though
        // the prototype gate cannot yet establish U20's held value.
        std::ifstream startup(argv[2]);
        int first_tick, end_tick, event_count;
        check(bool(startup >> first_tick >> end_tick >> event_count), "startup header");
        Tables hello_tables{};
        for (int& byte : hello_tables.native_rom)
            check(bool(startup >> byte), "startup native ROM byte");
        NativeControl hello_control(hello_tables);
        NativeSource hello_source;
        int next_tick, next_reg, next_value, h_index = -1;
        std::array<int, 4> audible_h_ticks{};
        check(bool(startup >> next_tick >> next_reg >> next_value), "first startup event");
        for (int tick = first_tick; tick < end_tick; ++tick) {
            if (event_count && tick == next_tick) {
                hello_control.write(next_reg, next_value);
                if (next_reg == 0 && (next_value & 63) == 0x2c) ++h_index;
                if (--event_count)
                    check(bool(startup >> next_tick >> next_reg >> next_value), "next startup event");
            }
            hello_control.set_ampct_zero(hello_source.ampct_zero());
            hello_control.advance_xck(1);
            hello_source.tick(hello_control, 2942);
            const auto& h = hello_source.output();
            if (hello_control.phone() == 0x2c && h.filter_amp && h.fric_amp) {
                check(h_index >= 0 && h_index < 4, "four H interval bounds");
                ++audible_h_ticks[h_index];
                check(h.fric1 && !h.fric2, "cold and later H must use the same noise route");
                check(hello_control.u20() == (h_index == 0 ? -1 : 1),
                      "startup fallback must not invent known U20 state or alter later gating");
            }
        }
        check(event_count == 0 && h_index == 3, "complete four-hello trace");
        for (int audible : audible_h_ticks)
            check(audible > 1000, "each H must produce an audible noise interval");

        // Known held controls always supersede the provisional cold route.
        NativeSource route_override;
        NativeSourceInputs route_inputs;
        route_inputs.fric1 = 0;
        route_inputs.fric2 = 1;
        route_override.tick(route_inputs);
        check(!route_override.output().fric1 && route_override.output().fric2,
              "known FRIC2 route overrides cold fallback");
        route_inputs.fric1 = route_inputs.fric2 = -1;
        route_override.tick(route_inputs);
        check(!route_override.output().fric1 && route_override.output().fric2,
              "unknown inputs retain a previously established route");
        std::cout << "PASS " << checks << " native host source checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
'''


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/test_ssi263_host_source")
    args = parser.parse_args()
    args.build_dir.mkdir(parents=True, exist_ok=True)
    vectors = args.build_dir / "noise.txt"
    vectors.write_text(noise_vectors(), encoding="ascii")
    demo = make_demo("hello_four", filter_frequency=231)
    events = sorted((e for e in demo.events if e.socket == 0), key=lambda e: e.tick)
    startup = args.build_dir / "startup.txt"
    startup.write_text(
        f"{events[0].tick} {demo.duration_ticks} {len(events)}\n"
        + " ".join(map(str, load_tables()["native_rom"])) + "\n"
        + "".join(f"{e.tick} {e.register} {e.value}\n" for e in events), encoding="ascii")
    harness = args.build_dir / "test.cpp"
    harness.write_text(HARNESS, encoding="utf-8")
    executable = args.build_dir / ("test.exe" if os.name == "nt" else "test")
    cxx = compiler()
    command = [cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-pedantic", "-I", str(SOURCE),
               str(harness), str(SOURCE / "native_source.cpp"), str(SOURCE / "native_control.cpp"),
               "-o", str(executable)]
    if os.name == "nt":
        command += ["-static-libgcc", "-static-libstdc++"]
    subprocess.run(command, check=True)
    result = subprocess.run([str(executable), str(vectors), str(startup)], text=True, capture_output=True)
    print(result.stdout.strip() or result.stderr.strip())
    result.check_returncode()
    (args.build_dir / "validation.json").write_text(json.dumps({
        "result": result.stdout.strip(), "compiler": cxx,
        "source_sha256": hashlib.sha256((SOURCE / "native_source.cpp").read_bytes()).hexdigest(),
        "noise_vectors_sha256": hashlib.sha256(vectors.read_bytes()).hexdigest(),
        "startup_vectors_sha256": hashlib.sha256(startup.read_bytes()).hexdigest(),
        "model": "optional SC-02 prototype source with deterministic cold seeds and provisional trim",
        "vivado_used": False,
    }, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
