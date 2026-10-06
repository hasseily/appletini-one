#!/usr/bin/env python3
"""Check attack instrumentation against normal PCM and prototype gate wiring."""
from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from audit_ssi263_attack import audit_trace, build_diagnostic, load_states
from render_ssi263 import build_host
from render_ssi263_mb_audit import FIXTURE, make_trace
from ssi263_host_data import make_demo


class AttackAuditTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="ssi263_attack_audit_")
        cls.root = Path(cls.temp.name)
        diagnostic = build_diagnostic(cls.root)
        renderer = build_host()
        fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
        phrase = next(p for p in fixture["translated_player"]["phrases"] if p["id"] == "E")
        cls.traces = [make_demo("hello_four", filter_frequency=231), make_trace(fixture, phrase, 12)]
        cls.reports = []
        cls.rows = []
        for index, trace in enumerate(cls.traces):
            out = cls.root / str(index)
            cls.reports.append(audit_trace(trace, out, diagnostic, renderer))
            cls.rows.append(load_states(out / "states.csv"))

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_instrumentation_preserves_pcm_and_bus_writes(self):
        for trace, report, rows in zip(self.traces, self.reports, self.rows):
            self.assertTrue(report["pcm_matches_normal_render"])
            self.assertGreater(report["metrics"]["frames"], 48000)
            self.assertEqual(report["metrics"]["output_clips"], 0)
            self.assertEqual(report["metrics"]["state_saturations"], 0)
            expected = [(e.tick, e.register, e.value) for e in trace.events if e.socket == 0]
            actual = [(r["tick"], r["write_reg"], r["write_value"]) for r in rows if r["reason"] == "write"]
            self.assertEqual(actual, expected)
            self.assertEqual([r["tick"] for r in rows], sorted(r["tick"] for r in rows))
        self.assertLess(self.rows[0][0]["tick"], 0, "pre-roll must remain part of state replay")

    def test_u206_samples_the_four_separate_and_gates_only_on_phi0(self):
        # Sheet 7 U70A-D feed U206 D1-D4. AMPCT0 comes from /U104C;
        # AMPCT1..3 are U68 Q2..Q4. This is bit gating, not multiplication.
        changes = 0
        for rows in self.rows:
            for previous, row in zip(rows, rows[1:]):
                if row["filter_amp"] == previous["filter_amp"]:
                    continue
                changes += 1
                self.assertEqual((row["filter_phase"], row["filter_edge"]), (0, 1))
                pins = [int(not (row["pw3"] == 1 and not row["u62"]))]
                pins += [(row["ampct"] >> bit) & 1 for bit in (1, 2, 3)]
                expected = sum(1 << bit for bit in range(4)
                               if pins[bit] and row["amp_code"] & (1 << bit))
                self.assertEqual(row["filter_amp"], expected)
        self.assertGreater(changes, 100, "exercise rising and falling phrase masks")

    def test_u68_counts_one_binary_step_in_its_gated_direction(self):
        # Sheet 6 ties B/D high and connects U/D to AMPCT0 AND /VA&FAZERO.
        # Check every observed count change, including source changes during
        # an already-high SEL2 interval; do not assume a fixed soft fade.
        rising = falling = 0
        for rows in self.rows:
            for previous, row in zip(rows, rows[1:]):
                difference = row["ampct"] - previous["ampct"]
                if not difference:
                    continue
                up = not (row["pw3"] == 1 and not row["u62"]) and bool(row["voice_amp"] or row["fric_amp"])
                self.assertEqual(difference, 1 if up else -1)
                self.assertEqual(row["ampct_zero"], int(row["ampct"] < 2))
                rising += difference > 0
                falling += difference < 0
        self.assertGreater(rising, 100)
        self.assertGreater(falling, 100)
        for report in self.reports:
            for ramp in report["u68_rises_to_15"]:
                self.assertEqual(ramp["count_edges"][-1][1], 15)
                self.assertEqual(ramp["count_edges"][0][1], ramp["start_count"] + 1)
                self.assertGreaterEqual(ramp["enable_to_full_count_ms"], ramp["first_to_last_increment_ms"])


if __name__ == "__main__":
    unittest.main()
