#!/usr/bin/env python3
"""Check host-model tables and input fidelity without Vivado or hardware."""
from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import ssi263_host_data as host

DEFAULT_PACKAGE = host.ROOT / "build/ssi263_calibration/SSI263-CAL-PAL-01.zip"
TABLE_SHA256 = "9daa3a5179027af095ba62835e70d6822957cb3d40681415062e268fd50c6503"


class TableTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # A renderer input loader must never launch a generator, compiler, or
        # HDL simulator merely to obtain the existing tables.
        with patch("subprocess.run", side_effect=AssertionError("unexpected child process")):
            cls.tables = host.load_tables()

    def test_all_tables_and_taps_are_present(self):
        expected = {"phones": (64, 12), "mappings": (6, 16), "f1": (16, 7),
                    "f2": (512, 7), "f3": (16, 7)}
        for key, (rows, columns) in expected.items():
            self.assertEqual(len(self.tables[key]), rows)
            self.assertTrue(all(len(row) == columns for row in self.tables[key]))
        for key, count in (("native_rom", 512), ("sc01_map", 64), ("f4", 7), ("fn", 5), ("fx", 2)):
            self.assertEqual(len(self.tables[key]), count)
        self.assertEqual(sum(len(list(host._flatten(self.tables[key]))) for key in host.TABLE_ORDER), 5262)

    def test_active_and_padded_rom_hashes(self):
        native = bytes(self.tables["native_rom"])
        self.assertEqual(hashlib.sha256(native).hexdigest(), host.ACTIVE_ROM_SHA256)
        self.assertEqual(hashlib.sha256(native + bytes(1536)).hexdigest(), host.FULL_ROM_SHA256)
        self.assertEqual(native[0x2C * 8:0x2C * 8 + 8], bytes.fromhex("71 90 0A C0 00 00 80 00"))
        self.assertEqual(native[0x2D * 8 + 2], 0x08)

    def test_export_pins_all_constants_and_order(self):
        payload = host.table_text(self.tables)
        self.assertEqual(hashlib.sha256(payload.encode("ascii")).hexdigest(), TABLE_SHA256)
        self.assertEqual(len(payload.splitlines()), len(host.TABLE_ORDER))
        self.assertEqual(len(payload.split()), 5262)
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "tables.txt"
            result = host.write_tables(target, self.tables)
            # Universal newline decoding makes the text portable to C++.
            self.assertEqual(target.read_text(encoding="ascii"), payload)
            self.assertEqual(result["table_sha256"], TABLE_SHA256)
            self.assertEqual(hashlib.sha256(target.read_bytes()).hexdigest(), result["table_sha256"])
        restored = json.loads(host.tables_json(self.tables))
        self.assertEqual(restored, self.tables)

    def test_phone_bits_agree_with_independent_existing_reader(self):
        from compare_ssi263_formant import parse_generated_data

        previous = parse_generated_data(host.FORMANT_PKG)
        for phone in range(64):
            wanted = [int(getattr(previous.phones[phone], field)) for field in host.PHONE_FIELDS]
            self.assertEqual(self.tables["phones"][phone], wanted, f"phone {phone:02X}")
            self.assertEqual(self.tables["sc01_map"][phone], previous.ssi_to_sc01[phone])

    def test_signed_coefficients_and_f2_address_order(self):
        self.assertEqual(self.tables["f1"][0], [25, 33, -7, -16, 32503, 32750, -32520])
        self.assertEqual(self.tables["f2"][0], [377, 525, -81, -229, 32177, 32471, -32473])
        self.assertEqual(self.tables["f2"][16][:2], [510, 711])
        self.assertEqual(self.tables["f2"][-1], [3810, 5312, -805, -2307, 22774, 29764, -25779])
        self.assertEqual(self.tables["mappings"][1], list(range(13)) + [14, 15, 15])
        self.assertEqual(self.tables["fn"], [1, 0, -1, 20, 32748])

    def test_parser_rejects_missing_duplicate_or_computed_entries(self):
        source = "function automatic logic [3:0] test(input logic x); case(x) "
        tail = "default: test = 4'hF; endcase endfunction"
        for entries in ("", "1'h0: test = 4'h1; 1'h0: test = 4'h2;",
                        "1'h0: test = 4'h1 + 4'h1;", "1'h1: test = 4'h1;"):
            with self.assertRaises(ValueError):
                host._case(source + entries + tail, "test", [0])
        self.assertEqual(host._case(source + "1'h0: test = 4'h1; " + tail, "test", [0]), [1])

    def test_native_rom_corruption_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            changed = Path(tmp) / "rom.mem"
            values = list(self.tables["native_rom"])
            values[-1] ^= 1
            changed.write_text("\n".join(f"{value:02x}" for value in values), encoding="ascii")
            with self.assertRaisesRegex(ValueError, "active die-target table"):
                host.load_tables(rom=changed)


class TraceTests(unittest.TestCase):
    def test_demos_have_valid_stateful_preroll_and_two_sockets(self):
        for name in host.DEMO_NAMES:
            first, second = host.make_demo(name), host.make_demo(name)
            self.assertEqual(first, second)
            first.validate()
            self.assertTrue(all(event.tick < 0 for event in first.events[:12]))
            self.assertEqual({event.socket for event in first.events}, {0, 1})
            for socket in (0, 1):
                setup = [(event.register, event.value) for event in first.events[:12] if event.socket == socket]
                self.assertEqual(setup, [(3, 128), (0, 128), (1, 111), (2, 142), (4, 128), (3, 92)])
            self.assertEqual(first.xck_hz, 1_015_625)
        survey = host.make_demo("phone_survey")
        for socket in (0, 1):
            self.assertEqual([event.value for event in survey.events if event.socket == socket
                              and event.register == 0 and event.tick >= 0], list(range(64)))

    def test_demo_controls_are_explicit_and_validated(self):
        trace = host.make_demo("hello", filter_frequency=231, articulation=7)
        self.assertEqual([(event.register, event.value) for event in trace.events[:12]
                          if event.socket == 0][-2:], [(4, 231), (3, 124)])
        self.assertEqual([event.value for event in trace.events if event.socket == 0
                          and event.register == 0 and event.tick >= 0],
                         [0, 0x2C, 0x0B, 0x20, 0x11, 0x12, 0] * 2)
        for options in ({"filter_frequency": -1}, {"filter_frequency": 256},
                        {"articulation": 8}, {"articulation": 0.5}):
            with self.assertRaises(ValueError):
                host.make_demo("hello", **options)

    def test_trace_export_keeps_preroll_and_order(self):
        trace = host.make_demo("transitions")
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "events.txt"
            host.write_trace(target, trace)
            actual = [tuple(map(int, line.split())) for line in target.read_text().splitlines()]
        self.assertEqual(actual, [(event.tick, event.socket, event.register, event.value)
                                  for event in trace.events])
        self.assertLess(actual[0][0], 0)

    def test_trace_rejects_invalid_fields_order_and_duration(self):
        valid = host.Event(0, 0, 7, 255)
        for fields in ({"tick": 1.5}, {"socket": 2}, {"register": 8}, {"value": 256}, {"value": True}):
            with self.assertRaises(ValueError):
                host.Trace([dataclasses.replace(valid, **fields)], 100, 10, {}).validate()
        for events, hz, end in (([valid], 0, 10), ([valid], 100, 0),
                                ([host.Event(11, 0, 0, 0)], 100, 10),
                                ([host.Event(1, 0, 0, 0), valid], 100, 10)):
            with self.assertRaises(ValueError):
                host.Trace(events, hz, end, {}).validate()


def check_calibration(path: Path):
    from sim_ssi263_calibration import load_package

    original = load_package(path)
    trace = host.load_calibration(path)
    assert trace.xck_hz == original.cpu_hz == 1_015_625
    assert trace.duration_ticks == 12000 * 10156
    assert len(original.events) == trace.metadata["total_checked_writes"] == 1752
    assert len(trace.events) == trace.metadata["ssi_writes"] == 1654
    ignored = trace.metadata["ignored_ay_writes"]
    assert len(ignored) == 98
    assert {event.source_index for event in trace.events} | {event["index"] for event in ignored} == set(range(1752))
    for event in trace.events:
        wanted = original.events[event.source_index]
        assert (event.tick, event.socket, event.register, event.value) == (
            wanted.cpu_cycle, wanted.target - 4, wanted.register, wanted.value)
    assert all(item["target"] < 4 for item in ignored)
    assert trace.events[0].tick < 0
    assert trace.metadata["package_sha256"] == "81e7aaaa9f0d2745cf40417089e9410da68bae53c4178aa26a78061138781438"
    labels = {segment["settings"]["phone"]: segment["settings"]["phone_label"]
              for segment in original.manifest["segments"]
              if "phone_label" in segment.get("settings", {})}
    for code, label in ((0x00, "PA"), (0x2C, "HF"), (0x0B, "EH1"), (0x20, "L"),
                        (0x11, "O"), (0x12, "OU")):
        assert labels[code] == label
    print("Calibration input passed: all 1752 writes checked; 1654 SSI writes retained, 98 AY writes recorded as omitted.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path)
    args = parser.parse_args()
    suite = unittest.defaultTestLoader.loadTestsFromModule(__import__(__name__))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        return 1
    package = args.package or (DEFAULT_PACKAGE if DEFAULT_PACKAGE.is_file() else None)
    if package is not None:
        check_calibration(package)
    else:
        print("Calibration ZIP not present; table and demo tests passed. Use --package to check its full trace.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
