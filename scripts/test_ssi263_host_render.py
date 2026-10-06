#!/usr/bin/env python3
"""Exercise the standalone SSI renderer without HDL tools or hardware.

These tests check model consistency and trace replay, not physical-chip
fidelity or universal bit equality with RTL. The host sample origin differs
from the saved fabric-clock simulation by one sample in some fixtures.
"""
from __future__ import annotations

import argparse
import dataclasses
from fractions import Fraction
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import wave

import numpy as np

import render_ssi263 as render
from ssi263_host_data import Event, Trace, load_calibration, make_demo, write_tables


def frame_at(tick: int, hz: int) -> int:
    return -(-tick * 48000 // hz)


def fixture() -> Trace:
    trace = make_demo("transitions")
    # Include a sustained vowel, then a source change. This also keeps every
    # setup write before tick zero, where range slicing must preserve it.
    trace.duration_ticks = trace.xck_hz // 2
    trace.events = [event for event in trace.events if event.tick < trace.duration_ticks]
    return trace


def read_pcm(directory: Path, profile: str) -> np.ndarray:
    with wave.open(str(directory / (profile + ".wav")), "rb") as wav:
        assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) == (2, 2, 48000)
        return np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2").reshape(-1, 2)


class RenderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.executable = render.build_host()
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        cls.trace = fixture()
        cls.reports = {}
        cls.pcm = {}
        for profile in render.PROFILES:
            directory = cls.root / ("full_" + profile)
            cls.reports[profile] = render.render(cls.trace, directory, profile, cls.executable)
            cls.pcm[profile] = read_pcm(directory, profile)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_repeat_renders_are_identical_and_audible(self):
        for profile in render.PROFILES:
            with self.subTest(profile=profile):
                directory = self.root / ("repeat_" + profile)
                report = render.render(self.trace, directory, profile, self.executable)
                self.assertEqual(report["pcm_sha256"], self.reports[profile]["pcm_sha256"])
                self.assertEqual(report["wav_sha256"], self.reports[profile]["wav_sha256"])
                self.assertEqual(report["frames"], frame_at(self.trace.duration_ticks, self.trace.xck_hz))
                self.assertEqual(report["applied_writes"], len(self.trace.events))
                for channel in range(2):
                    self.assertTrue(np.any(self.pcm[profile][:, channel]))
                    self.assertGreater(report["statistics"][channel]["rms"], 0)

    def test_trimmed_range_replays_prefix_and_preserves_exact_samples(self):
        start, end = 113579, 454321
        first, last = frame_at(start, self.trace.xck_hz), frame_at(end, self.trace.xck_hz)
        for profile in render.PROFILES:
            with self.subTest(profile=profile):
                directory = self.root / ("tail_" + profile)
                report = render.render(self.trace, directory, profile, self.executable, start, end)
                np.testing.assert_array_equal(read_pcm(directory, profile), self.pcm[profile][first:last])
                self.assertEqual(report["frames"], last - first)
                self.assertGreater(report["processed_frames"], report["frames"])
                # The stimulus still begins with the original CTL write,
                # rather than a guessed register snapshot at range start.
                self.assertEqual((directory / "events.txt").read_text().splitlines()[1], "-120 0 3 128")

    def test_socket_changes_do_not_modify_the_other_voice(self):
        changed = dataclasses.replace(self.trace, events=list(self.trace.events))
        changed.events.append(Event(self.trace.xck_hz // 10, 0, 1, 160))
        changed.events.sort(key=lambda event: event.tick)
        for profile in render.PROFILES:
            with self.subTest(profile=profile):
                directory = self.root / ("independent_" + profile)
                render.render(changed, directory, profile, self.executable)
                actual = read_pcm(directory, profile)
                np.testing.assert_array_equal(actual[:, 1], self.pcm[profile][:, 1])
                self.assertTrue(np.any(actual[:, 0] != self.pcm[profile][:, 0]))

    def test_register_aliases_reach_filter_register_unchanged(self):
        for profile in render.PROFILES:
            with self.subTest(profile=profile):
                outputs = []
                for register in (4, 5, 6, 7):
                    changed = dataclasses.replace(self.trace, events=list(self.trace.events))
                    changed.events.append(Event(self.trace.xck_hz // 10, 0, register, 231))
                    changed.events.sort(key=lambda event: event.tick)
                    directory = self.root / f"alias_{profile}_{register}"
                    report = render.render(changed, directory, profile, self.executable)
                    outputs.append(report["pcm_sha256"])
                self.assertEqual(len(set(outputs)), 1)
                self.assertNotEqual(outputs[0], self.reports[profile]["pcm_sha256"])

    def test_empty_register_stream_produces_silence(self):
        trace = Trace([], 1_015_625, 10_000, {"name": "empty"})
        for profile in render.PROFILES:
            directory = self.root / ("empty_" + profile)
            report = render.render(trace, directory, profile, self.executable)
            self.assertEqual(report["applied_writes"], 0)
            self.assertFalse(np.any(read_pcm(directory, profile)))
            self.assertEqual([item["rms"] for item in report["statistics"]], [0, 0])

    def test_invalid_ranges_and_profiles_fail_before_process_launch(self):
        for kwargs in ({"start_tick": -1}, {"start_tick": 100, "end_tick": 100},
                       {"end_tick": self.trace.duration_ticks + 1}, {"profile": "unknown"},
                       {"articulation_reference": -1}, {"articulation_reference": 16},
                       {"articulation_reference": 8.5}, {"articulation_reference": True},
                       {"start_tick": 0.5}, {"end_tick": 100.5},
                       {"start_tick": 1, "end_tick": 2},
                       {"prototype_gain": 0}, {"prototype_gain": 1025},
                       {"prototype_gain": 1.5}, {"prototype_gain": True},
                       {"voice_trim": -1}, {"voice_trim": 131072},
                       {"voice_trim": 0.5}, {"voice_trim": False}):
            options = {"profile": "baseline", **kwargs}
            with self.subTest(options=options):
                with patch("subprocess.run", side_effect=AssertionError("invalid input launched executable")):
                    with self.assertRaises(ValueError):
                        render.render(self.trace, self.root / "invalid", executable=self.executable, **options)

    def test_executable_rejects_bad_headers_and_events(self):
        tables = self.root / "checked_tables.txt"
        write_tables(tables)
        traces = ("wrong 1015625 0 100 0\n", "SSIHOST1 1015625 0 100 1\n",
                  "SSIHOST1 1015625 0 100 1\n0 2 0 0\n",
                  "SSIHOST1 1015625 0 100 1\n0 0 8 0\n",
                  "SSIHOST1 1015625 0 100 1\n0 0 0 256\n",
                  "SSIHOST1 1015625 0 100 2\n10 0 0 0\n9 0 0 0\n",
                  "SSIHOST1 1015625 0 100 0\n0 0 0 0\n",
                  "SSIHOST1 1015625 1 2 0\n")
        for index, text in enumerate(traces):
            stimulus = self.root / f"bad_{index}.txt"
            stimulus.write_text(text)
            result = subprocess.run([str(self.executable), str(tables), str(stimulus),
                                     str(self.root / "bad.pcm"), "baseline", "8"],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue(result.stderr.strip())

    def test_executable_rejects_fractional_or_suffixed_articulation(self):
        tables = self.root / "checked_tables.txt"
        write_tables(tables)
        stimulus = self.root / "integer_articulation.txt"
        stimulus.write_text("SSIHOST1 1015625 0 100 0\n")
        for value in ("8.5", "8junk", "True"):
            result = subprocess.run([str(self.executable), str(tables), str(stimulus),
                                     str(self.root / "bad_articulation.pcm"), "baseline", value],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue(result.stderr.strip())

    def test_writes_after_last_sample_are_still_applied(self):
        # The last PCM sample lands at tick85, while the final write is at99.
        # No additional sample is due, but the full input trace must complete.
        trace = Trace([Event(99, 0, 3, 128)], 1_015_625, 100, {"name": "final_fraction"})
        for profile in render.PROFILES:
            report = render.render(trace, self.root / ("fraction_" + profile), profile, self.executable)
            self.assertEqual(report["frames"], 5)
            self.assertEqual(report["applied_writes"], 1)

    def test_direct_executable_wav_matches_python_wrapper(self):
        original = self.root / "full_baseline"
        destination = self.root / "standalone.wav"
        result = subprocess.run([str(self.executable), str(original / "tables.txt"),
                                 str(original / "events.txt"), str(destination), "baseline", "8"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        details = json.loads(result.stdout)
        with wave.open(str(destination), "rb") as wav:
            self.assertEqual((wav.getnchannels(), wav.getsampwidth(), wav.getframerate()), (2, 2, 48000))
            self.assertEqual(wav.getnframes(), details["frames"])
            actual = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2").reshape(-1, 2)
        np.testing.assert_array_equal(actual, self.pcm["baseline"])
        self.assertEqual(destination.stat().st_size, 44 + 4 * details["frames"])

    def test_prototype_setting_limits_and_strict_executable_parsing(self):
        trace = Trace([], 1_015_625, 100, {"name": "settings"})
        for gain, trim in ((1, 0), (1024, 131071)):
            report = render.render(trace, self.root / f"settings_{gain}", "prototype", self.executable,
                                   prototype_gain=gain, voice_trim=trim)
            self.assertEqual(report["prototype_gain"], gain)
            self.assertEqual(report["voice_trim_q16"], trim)
        original = self.root / "full_baseline"
        for gain, trim in (("0", "0"), ("1025", "0"), ("1", "-1"), ("1", "131072"),
                           ("1.5", "0"), ("1", "2048junk")):
            result = subprocess.run([str(self.executable), str(original / "tables.txt"),
                                     str(original / "events.txt"), str(self.root / "bad_setting.pcm"),
                                     "prototype", "8", gain, trim], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue(result.stderr.strip())


class JsonTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def load(self, content):
        path = self.root / "trace.json"
        path.write_text(json.dumps(content), encoding="utf-8")
        with patch("subprocess.run", side_effect=AssertionError("JSON loader launched process")):
            return render.load_json_trace(path)

    def test_fractional_seconds_round_up_and_keep_preroll(self):
        trace = self.load({"effective_clock_hz": 1_015_625, "duration_seconds": "0.125",
                           "events": [{"seconds": "-0.00002", "socket": 1, "register": 7, "value": 231},
                                      {"seconds": "0.00002", "socket": 1, "register": 0, "value": 14}]})
        self.assertEqual(trace.events[0].tick, -20)
        self.assertEqual(trace.events[1].tick, 21)
        self.assertEqual(trace.duration_ticks, 126954)
        self.assertLess(Fraction(trace.events[0].tick, trace.xck_hz) - Fraction("-0.00002"), Fraction(1, trace.xck_hz))

    def test_bad_json_event_values_are_rejected(self):
        event = {"tick": 0, "socket": 0, "register": 0, "value": 14}
        for change in ({"tick": 1.5}, {"socket": 2}, {"register": 8}, {"value": 256}, {"value": True}):
            with self.assertRaises(ValueError):
                self.load({"duration_ticks": 100, "events": [{**event, **change}]})
        for clock in (0, -1, 1.5, True):
            with self.assertRaises(ValueError):
                self.load({"effective_clock_hz": clock, "duration_ticks": 100, "events": []})

    def test_bad_json_container_shapes_fail_clearly(self):
        for source in ([], None, 12, "trace", {}, {"events": {}},
                       {"duration_ticks": 100, "events": [None]},
                       {"duration_ticks": 100, "events": [[]]},
                       {"duration_ticks": 1.5, "events": []},
                       {"duration_ticks": True, "events": []}):
            with self.subTest(source=source):
                with self.assertRaises(ValueError):
                    self.load(source)


class ControlInterruptionTests(unittest.TestCase):
    def test_phone_and_amplitude_writes_at_every_scanner_phase(self):
        """Changing targets must retain A, then finish from its held value."""
        harness = r'''
#include "native_control.h"
#include <iostream>
#include <stdexcept>
using namespace ssi_host;
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
Tables source() {
    Tables tables{};
    // Synthetic rows hold U32B open, so the test isolates scanning from
    // source-route gates. Both amplitude latches enable at duration phase2.
    const int row0[8] = {0xf1, 0x11, 0x74, 0x50, 0, 0xa0, 0x80, 0};
    const int row1[8] = {0x01, 0xf1, 0x24, 0xb0, 0, 0x40, 0xc0, 0};
    for (int n = 0; n < 8; ++n) {
        tables.native_rom[n] = row0[n];
        tables.native_rom[8 + n] = row1[n];
    }
    return tables;
}
void start(NativeControl& control) {
    control.write(2, 0xf0);
    control.write(0, 0xc0);
    control.write(3, 0x7f);
}
int main() {
    try {
        const Tables tables = source();
        for (int phase = 0; phase < 128; ++phase) {
            NativeControl control(tables, 1015625, NativeTiming{15});
            start(control);
            control.advance_xck(2048 + phase);
            const auto held = control.parameter_codes();
            const int selector = control.selector(), scan = control.selector_phase();
            control.write(0, 0xc1);
            check(control.parameter_codes() == held, "phone write changed held parameter codes");
            check(control.selector() == selector && control.selector_phase() == scan,
                  "phone write reset scanner phase");
            control.advance_xck(256);
            for (int slot : {0, 1, 2, 3, 5, 6})
                check(control.parameter_state(slot).target == (tables.native_rom[8 + slot] >> 4),
                      "phone interrupt left stale target after scan");
            check(control.parameter_state(4).target == 15, "phone write changed host amplitude target");
            control.advance_xck(8192);
            for (int slot : {0, 1, 2, 3, 5, 6})
                check(control.parameter_codes()[slot] == (tables.native_rom[8 + slot] >> 4),
                      "interrupted transition did not reach final target");

            // Both rapid phone writes occur before a complete scan. The
            // last write must replace every target without jumping A.
            const auto before_rapid = control.parameter_codes();
            control.write(0, 0xc0);
            check(control.parameter_codes() == before_rapid, "first rapid write jumped A");
            control.advance_xck(phase);
            const auto before_second = control.parameter_codes();
            control.write(0, 0xc1);
            check(control.parameter_codes() == before_second, "second rapid write jumped A");
            control.advance_xck(256);
            for (int slot : {0, 1, 2, 3, 5, 6})
                check(control.parameter_state(slot).target == (tables.native_rom[8 + slot] >> 4),
                      "rapid phone writes left mixed targets");
            NativeControl amplitude(tables, 1015625, NativeTiming{15});
            start(amplitude);
            amplitude.advance_xck(8192 + phase);
            const auto before_mute = amplitude.parameter_codes();
            amplitude.write(3, 0x70);
            check(amplitude.parameter_codes() == before_mute, "amplitude write jumped native parameter A");
            amplitude.advance_xck(256);
            for (int slot : {4, 5, 6})
                check(amplitude.parameter_state(slot).target == 0, "live AMPZERO left stale source target");
            amplitude.advance_xck(8192);
            for (int slot : {4, 5, 6})
                check(amplitude.parameter_codes()[slot] == 0, "native amplitude fade did not finish");
            amplitude.write(3, 0x77);
            amplitude.advance_xck(256);
            check(amplitude.parameter_state(4).target == 7, "live amplitude target missing");
            check(amplitude.parameter_state(5).target == 10 && amplitude.parameter_state(6).target == 8,
                  "live amplitude restore did not restore ROM source targets");
            amplitude.advance_xck(8192);
            check(amplitude.parameter_codes()[4] == 7 && amplitude.parameter_codes()[5] == 10
                  && amplitude.parameter_codes()[6] == 8, "restored amplitude transition did not finish");
        }
        std::cout << "128 scan phases: phone interruption, rapid writes, amplitude zero and restore passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            source = directory / "review.cpp"
            source.write_text(harness)
            executable = directory / ("review.exe" if os.name == "nt" else "review")
            compiler = render.find_compiler()
            environment = os.environ.copy()
            environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
            command = [compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                       "-I", str(render.SOURCE), str(source), str(render.SOURCE / "native_control.cpp"),
                       "-o", str(executable)]
            if os.name == "nt":
                command.append("-static")
            compiled = subprocess.run(command, env=environment, text=True, capture_output=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(executable)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


def check_calibration(path: Path) -> None:
    """Render the entire real tester trace, keeping raw SSI/AY scope explicit."""
    trace = load_calibration(path)
    with tempfile.TemporaryDirectory() as tmp:
        report = render.render(trace, Path(tmp), "baseline")
        assert report["trace"]["total_checked_writes"] == 1752
        assert len(report["trace"]["ignored_ay_writes"]) == 98
        assert report["checked_ssi_writes"] == report["applied_writes"] == 1654
        assert report["frames"] == frame_at(trace.duration_ticks, trace.xck_hz)
        assert report["audio_seconds"] > 119.99
        assert all(channel["nonzero_samples"] > 1000 for channel in report["statistics"])
        assert report["rendered_audio_per_wall_second"] > 1, "two-minute baseline should render faster than real time"
        print(f"Full calibration passed: {report['applied_writes']} SSI writes, "
              f"{report['audio_seconds']:.3f}s audio in {report['render_seconds']:.3f}s.")
        reference = json.loads(render.BALANCED_REFERENCE.read_text(encoding="utf-8"))
        balanced = render.render(trace, Path(tmp), "prototype",
                                 **render.reference_settings("balanced"))
        assert balanced["wav_sha256"] == reference["calibration"]["balanced_wav_sha256"], \
            "The approved balanced calibration audio changed"
        assert all(channel["state_saturations"] == channel["output_clips"] == 0
                   for channel in balanced["native_metrics"])
        print("Approved balanced reference: complete calibration WAV is byte-identical.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, help="also render the complete checked calibration ZIP")
    args = parser.parse_args()
    suite = unittest.defaultTestLoader.loadTestsFromModule(__import__(__name__))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        return 1
    if args.package:
        check_calibration(args.package)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
