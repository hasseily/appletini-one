#!/usr/bin/env python3
"""Check mb-audit SSI bus stimuli against the pinned upstream phrase bytes."""
from __future__ import annotations

from copy import deepcopy
import json
import unittest

from render_ssi263_mb_audit import FIXTURE, make_trace
from ssi263_host_data import PAL_EFFECTIVE_XCK_HZ


# Independently decoded from Tom Charlesworth's source tables and the SSI
# driver's lookup at 11025ed20d9f05afe899c854db43a85ba8a850c4. These are SSI
# register-zero bytes, including the final STOP -> PA, not SC-01 sound data.
EXPECTED_PHONES = {
    "A": "00 35 19 00 30 27 0F 03 00 30 28 1D 0F 03 29 30 00 24 0C 29 00 00",
    "B": "00 10 14 28 32 00 00",
    "C": "00 29 1D 0F 03 37 00 23 08 33 00 23 19 38 00",
    "D": "00 26 08 37 00 11 14 33 1C 00",
    "E": "00 27 20 01 2F 00 29 10 38 30 18 20 28 00 35 18 00 2C 07 38 28 00 32 01 28 00 00 00",
    "F": "00 05 16 00 29 0C 38 28 00 26 11 00 35 0C 28 00 23 0A 09 00 00 00",
    "G": "00 03 16 00 00 23 23 1D 00 00 08 00 27 07 28 07 34 19 20 00 00 00 0E 27 11 38 0A 38 28 00 00",
}
CLASSIC_DURPHON = bytes.fromhex("29 2D 60 0C 30 47 29 4C 0C 25 33 EC 47 47 78 68 72 5C")


class MbAuditTraceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
        cls.phrases = cls.fixture["translated_player"]["phrases"]
        cls.classic = cls.fixture["classic_adventure"]

    @staticmethod
    def one_socket(trace):
        return [event for event in trace.events if event.socket == 0]

    def test_fixture_contains_exact_native_ssi_stimuli(self):
        self.assertEqual(self.fixture["schema"], 1)
        self.assertEqual([phrase["id"] for phrase in self.phrases], list("ABCDEFG"))
        for phrase in self.phrases:
            self.assertEqual(bytes(phrase["native_ssi_durphon"]),
                             bytes.fromhex(EXPECTED_PHONES[phrase["id"]]))
        self.assertEqual(self.fixture["translated_player"]["attributes"],
                         [0, 0x20, 0xA8, 0x5C, 0xE9])
        self.assertEqual(self.classic["register_rows"],
                         [[phone, 0x52, 0xB8, 0x7B, 0xE6] for phone in CLASSIC_DURPHON])

    def test_a_g_replay_original_attribute_phone_and_shutdown_writes(self):
        for rate in range(16):
            for phrase in self.phrases:
                with self.subTest(rate=rate, phrase=phrase["id"]):
                    trace = make_trace(self.fixture, phrase, rate)
                    events = self.one_socket(trace)
                    wanted = [(4, 0xE9), (3, 0x5C), (2, rate * 16 + 8),
                              (1, 0x20), (3, 0xDC), (0, 0xC0), (3, 0x5C)]
                    wanted += [(0, value) for value in bytes.fromhex(EXPECTED_PHONES[phrase["id"]])]
                    wanted += [(3, 0x80), (0, 0), (3, 0)]
                    self.assertEqual([(event.register, event.value) for event in events], wanted)
                    # There is one final mapped PA; the data sentinel must not
                    # add five spurious PA phonemes to the physical stream.
                    self.assertEqual(len(trace.metadata["segments"]), len(wanted) - 10)

    def test_a_g_nominal_requests_cover_full_duration_and_initial_pause(self):
        for rate in (0, 10, 11, 12, 15):
            for phrase in self.phrases:
                with self.subTest(rate=rate, phrase=phrase["id"]):
                    trace = make_trace(self.fixture, phrase, rate)
                    events = self.one_socket(trace)
                    # Initial C0 has DUR=3; all translated phrase bytes have
                    # DUR=0. Both count from the current renderer's CTL release.
                    quarter_phone_ticks = 4096 * (16 - rate)
                    self.assertEqual(events[7].tick - events[6].tick, quarter_phone_ticks)
                    segments = trace.metadata["segments"]
                    for index, segment in enumerate(segments):
                        self.assertEqual(segment["start_tick"], events[index + 7].tick)
                        self.assertEqual(segment["end_tick"] - segment["start_tick"],
                                         4 * quarter_phone_ticks)
                        if index:
                            self.assertEqual(segment["start_tick"], segments[index - 1]["end_tick"])
                    self.assertEqual(events[-3].tick, segments[-1]["end_tick"])
                    self.assertEqual([event.tick - events[-3].tick for event in events[-3:]],
                                     [0, 10, 20])

    def test_classic_preserves_reverse_register_order_and_original_total(self):
        trace = make_trace(self.fixture, self.classic)
        events = self.one_socket(trace)
        self.assertEqual([(event.register, event.value) for event in events[:3]],
                         [(3, 0x80), (0, 0xC0), (3, 0)])
        self.assertEqual(events[3].tick - events[2].tick, 65536)
        self.assertIn("RATE=0", trace.metadata["timing"])
        segments = trace.metadata["segments"]
        self.assertEqual(len(segments), 18)
        for index, phone in enumerate(CLASSIC_DURPHON):
            writes = events[3 + 5 * index:8 + 5 * index]
            self.assertEqual([(event.register, event.value) for event in writes],
                             [(4, 0xE6), (3, 0x7B), (2, 0xB8), (1, 0x52), (0, phone)])
            self.assertEqual([event.tick - writes[0].tick for event in writes], [0, 16, 32, 48, 64])
            self.assertEqual(segments[index]["start_tick"], writes[-1].tick)
            self.assertEqual(segments[index]["end_tick"] - writes[-1].tick,
                             (4 - (phone >> 6)) * 20480)
            if index:
                self.assertEqual(writes[0].tick, segments[index - 1]["end_tick"])
        # Native table contains 60 duration units at RATE=B: 60 * 5 * 4096.
        self.assertEqual(sum(segment["end_tick"] - segment["start_tick"] for segment in segments),
                         1_228_800)
        self.assertEqual([(event.register, event.value) for event in events[-3:]],
                         [(3, 0x80), (0, 0), (3, 0)])
        self.assertEqual(events[-3].tick, segments[-1]["end_tick"])

    def test_stereo_is_the_same_stimulus_and_tail_is_silent_shutdown_time(self):
        cases = [(phrase, rate) for phrase in self.phrases for rate in (10, 11, 12)]
        cases.append((self.classic, None))
        for phrase, rate in cases:
            with self.subTest(rate=rate, phrase=phrase["id"]):
                trace = make_trace(self.fixture, phrase, rate)
                left = [(e.tick, e.register, e.value, e.source_index) for e in trace.events if e.socket == 0]
                right = [(e.tick, e.register, e.value, e.source_index) for e in trace.events if e.socket == 1]
                self.assertEqual(left, right)
                self.assertEqual(trace.xck_hz, PAL_EFFECTIVE_XCK_HZ)
                self.assertEqual(trace.duration_ticks - trace.events[-1].tick, PAL_EFFECTIVE_XCK_HZ // 4)
                self.assertEqual(trace.metadata["ssi_writes"], len(trace.events))
                trace.validate()

    def test_invalid_rate_and_non_native_translated_byte_are_rejected(self):
        for rate in (-1, 16, True, 10.0, "B"):
            with self.subTest(rate=rate), self.assertRaises(ValueError):
                make_trace(self.fixture, self.phrases[0], rate)
        for value in (-1, 64, 255, True, 1.0):
            phrase = deepcopy(self.phrases[0])
            phrase["native_ssi_durphon"][0] = value
            with self.subTest(value=value), self.assertRaises(ValueError):
                make_trace(self.fixture, phrase, 11)


if __name__ == "__main__":
    unittest.main()
