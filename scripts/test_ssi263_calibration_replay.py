#!/usr/bin/env python3
"""Trace integrity, range/state continuity, and optional dual-SSI RTL replay.

The default tests use a small independent fixture and need no capture ZIP.
--rtl runs the two real voices, including their bus wrappers and XCK clock path.
--package PATH also validates every event in the supplied physical-test bundle.
"""
from __future__ import annotations

import argparse
import copy
import csv
from fractions import Fraction
import io
from pathlib import Path
import tempfile
import unittest
import wave

import sim_ssi263_calibration as replay


def fixture():
    # Distinct sustained phones/inflections on the two sockets. AY data is
    # included to catch confusing stream targets with slot or socket numbers.
    writes = [(0, 4, 3, 128, -200), (0, 5, 3, 128, -120),
              (0, 0, 8, 0, -30)]
    for target, r1, r2, phone in ((4, 111, 142, 14), (5, 168, 142, 8)):
        for register, value in ((3, 128), (0, 128), (1, r1), (2, r2),
                                (4, 231), (3, 0x7F), (0, phone)):
            writes.append((1, target, register, value, 20 + (len(writes) - 3) * 40))
    # An alias write must reach the wrapper unchanged.
    writes.append((2, 4, 7, 230, 40))
    # ACK then speed the next response-slot reload so D7 rises in this short run.
    writes.append((5, 4, 2, 0xFE, 40))
    writes.append((10, 5, 3, 128, 40))
    clock = 1_015_625
    period = 10156
    events = [dict(event_id=f"E{index:05}", tick=tick, target=target,
                   register=reg, value=value, segment_id="fixture")
              for index, (tick, target, reg, value, offset) in enumerate(writes)]
    manifest = {"build_id": "TEST-FIXTURE", "duration_ticks": 12,
                "clock": {"effective_clock_hz": clock, "timer_period_cpu_cycles": period},
                "events": events,
                "segments": [{"segment_id": "fixture", "start_seconds": 0,
                              "end_seconds": 12 * period / clock}]}
    source = io.StringIO(newline="")
    writer = csv.writer(source)
    writer.writerow(("tick", "target", "register", "value", "nominal_cycle_offset", "origin"))
    for tick, target, reg, value, offset in writes:
        writer.writerow((tick, target, reg, value, offset,
                         "before_timer_start" if tick == 0 else "poll_entry_on_timer_tick"))
    validation = {"main": {"verified_register_writes": len(events),
                            "trace_model": "synthetic independent replay fixture"}}
    return manifest, source.getvalue(), validation


class ReplayTests(unittest.TestCase):
    def setUp(self):
        self.manifest, self.source, self.validation = fixture()
        self.trace = replay.parse_trace(self.manifest, self.source, self.validation)

    def test_trace_matches_manifest_including_order(self):
        broken = copy.deepcopy(self.manifest)
        broken["events"][4]["value"] ^= 1
        with self.assertRaisesRegex(ValueError, "disagrees"):
            replay.parse_trace(broken, self.source, self.validation)
        broken = copy.deepcopy(self.manifest)
        broken["events"][0], broken["events"][1] = broken["events"][1], broken["events"][0]
        with self.assertRaises(ValueError):
            replay.parse_trace(broken, self.source, self.validation)

    def test_trace_requires_real_offsets_and_strict_order(self):
        with self.assertRaises(ValueError):
            replay.parse_trace(self.manifest, self.source.replace("-120", "-200"), self.validation)
        with self.assertRaises(ValueError):
            replay.parse_trace(self.manifest, self.source.replace("-200", "0"), self.validation)
        with self.assertRaises(ValueError):
            replay.parse_trace(self.manifest, self.source.replace("poll_entry_on_timer_tick", "guessed"), self.validation)
        with self.assertRaises(ValueError):
            replay.parse_trace(self.manifest, self.source, {"main": {"verified_register_writes": 1}})

    def test_socket_ids_and_native_alias_address(self):
        self.assertEqual(self.trace.events[0].address, 0xC423)
        self.assertEqual(self.trace.events[1].address, 0xC443)
        self.assertIsNone(self.trace.events[2].address)
        self.assertEqual(self.trace.events[-3].address, 0xC427)

    def test_ranges_preserve_all_prior_writes_and_clock_phase(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            full = replay.prepare(self.trace, root / "full")
            tail = replay.prepare(self.trace, root / "tail", Fraction("0.02"))
            self.assertEqual((root / "full/stimulus.txt").read_bytes(),
                             (root / "tail/stimulus.txt").read_bytes())
            self.assertEqual(full["timer_origin_fabric_cycle"], tail["timer_origin_fabric_cycle"])
            self.assertEqual(full["end_fabric_cycle"], tail["end_fabric_cycle"])
            first = int((root / "full/stimulus.txt").read_text().split()[0])
            self.assertGreaterEqual(first, 32)
            self.assertLess(first, full["timer_origin_fabric_cycle"])
            self.assertEqual(full["replayed_writes"], len(self.trace.events))
            self.assertEqual(full["replayed_ssi_writes"], len(self.trace.events) - 1)

    def test_fractional_cycles_never_run_a_write_early(self):
        with tempfile.TemporaryDirectory() as tmp:
            details = replay.prepare(self.trace, Path(tmp))
            with (Path(tmp) / "events.csv").open() as source:
                rows = list(csv.DictReader(source))
            for row, event in zip(rows, self.trace.events):
                actual = int(row["fabric_cycle"]) - details["timer_origin_fabric_cycle"]
                exact = Fraction(event.cpu_cycle * replay.FABRIC_HZ, self.trace.cpu_hz)
                self.assertGreaterEqual(actual, exact)
                self.assertLess(actual - exact, 1)

    def test_bad_range_or_accelerated_clock_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            for start, end in ((-1, None), (1, None), (0, 0), (0, 120)):
                with self.assertRaises(ValueError):
                    replay.prepare(self.trace, Path(tmp), Fraction(start), None if end is None else Fraction(end))
            with self.assertRaises(ValueError):
                replay.prepare(self.trace, Path(tmp), fabric_hz=1_000_000)

    def test_wav_channel_order_amplitude_and_alignment(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            details = replay.prepare(self.trace, out)
            origin = details["timer_origin_fabric_cycle"]
            (out / "samples.csv").write_text(f"fabric_cycle,target4,target5\n{origin},32767,-32768\n{origin + 2771},12,-4\n")
            result = replay.finish_audio(out, details)
            self.assertEqual(result["audio"]["peak"], [32767, 32768])
            self.assertEqual(result["audio"]["rail_samples"], [1, 1])
            self.assertEqual(result["segments"][0]["wav_start_sample"], 0)
            with wave.open(str(out / "ssi_raw.wav"), "rb") as wav:
                self.assertEqual((wav.getnchannels(), wav.getframerate(), wav.getnframes()), (2, 48000, 2))
                self.assertEqual(wav.readframes(1), b"\xff\x7f\x00\x80")


def rtl_check(out: Path):
    trace = replay.parse_trace(*fixture())
    details = replay.prepare(trace, out)
    details = replay.simulate(out, details)
    with (out / "executed_events.csv").open() as source:
        executed = list(csv.DictReader(source))
    with (out / "events.csv").open() as source:
        expected = list(csv.DictReader(source))
    assert len(executed) == len(expected)
    for actual, wanted in zip(executed, expected):
        for field in ("index", "fabric_cycle", "target", "register", "value"):
            assert actual[field] == wanted[field], (field, actual, wanted)
    with (out / "samples.csv").open() as source:
        samples = list(csv.DictReader(source))
    assert len(samples) in (5759, 5760), len(samples)
    assert any(int(row["target4"]) for row in samples), "target4 did not speak"
    assert any(int(row["target5"]) for row in samples), "target5 did not speak"
    assert any(row["target4"] != row["target5"] for row in samples), "socket outputs were tied together"
    with (out / "status.csv").open() as source:
        status = list(csv.DictReader(source))
    assert any(row["d7_target4"] == "1" for row in status), "D7 response did not reach the status log"
    assert [int(row["tick_boundary"]) for row in status if int(row["tick_boundary"]) >= 0] == list(range(12))
    # Replay a trimmed output range from the SAME reset/pre-roll, not from a
    # register snapshot. Exact sample equality checks hidden filter state too.
    tail_out = out / "tail"
    tail = replay.prepare(trace, tail_out, Fraction("0.02"))
    replay.simulate(tail_out, tail)
    with (tail_out / "samples.csv").open() as source:
        tail_samples = list(csv.DictReader(source))
    assert tail_samples == [row for row in samples if int(row["fabric_cycle"]) >= tail["capture_start_fabric_cycle"]]
    print(f"RTL replay passed: {len(executed)} exact writes, dual-chip audio, unchanged range state.")
    return details


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rtl", action="store_true")
    parser.add_argument("--package", type=Path)
    parser.add_argument("--out-dir", type=Path, default=replay.ROOT / "build/test_ssi263_calibration_replay")
    args = parser.parse_args()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ReplayTests))
    if not result.wasSuccessful():
        return 1
    if args.package:
        trace = replay.load_package(args.package)
        replay.prepare(trace, args.out_dir / "package")
        print(f"Package trace passed: {len(trace.events)} writes, {len(trace.manifest['segments'])} segments.")
    if args.rtl:
        rtl_check(args.out_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
