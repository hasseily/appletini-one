#!/usr/bin/env python3
"""Independent signal tests for capture comparison; no host renderer needed."""
import base64
import json
import math
from pathlib import Path
import tempfile
import unittest

import numpy as np

import analyze_ssi263_capture as analyzer
import compare_ssi263_capture as comparison
from test_ssi263_capture_analysis import manifest, recording


class ComparisonSignalTests(unittest.TestCase):
    def test_known_tone_level_spectrum_and_no_normalization(self):
        rate = 48000
        time = np.arange(rate) / rate
        signal = .2 * np.sin(2 * np.pi * 1000 * time)
        samples = np.column_stack((signal, signal * .5))
        one = comparison.measure_series(samples, rate, 32767 / 32768, 0, (0, 1), (0, 1), 0)
        two = comparison.measure_series(samples, rate, 32767 / 32768, 1, (0, 1), (0, 1), 0)
        self.assertAlmostEqual(one["metrics"]["rms_dbfs"], 20 * math.log10(.2 / math.sqrt(2)), places=8)
        self.assertAlmostEqual(one["metrics"]["peak_dbfs"], 20 * math.log10(.2), places=8)
        self.assertAlmostEqual(one["metrics"]["rms_dbfs"] - two["metrics"]["rms_dbfs"], 6.020599913, places=7)
        self.assertAlmostEqual(one["spectrum"]["peak_hz"], 1000, places=6)
        self.assertAlmostEqual(one["spectrum"]["peak_amplitude_dbfs"], 20 * math.log10(.2), places=6)
        self.assertAlmostEqual(one["metrics"]["spectral_centroid_hz"], 1000, places=4)

    def test_affine_map_changes_time_labels_not_recorded_pitch(self):
        rate, offset, scale = 8000, .15, 1.1
        time = np.arange(rate * 2) / rate
        samples = np.zeros((len(time), 2))
        active = (time >= offset + scale * .4) & (time < offset + scale * .8)
        samples[active, 0] = .2 * np.sin(2 * np.pi * 400 * time[active])
        result = comparison.measure_series(samples, rate, 1, 0, (.2, 1), (.45, .75), .2, offset, scale)
        self.assertAlmostEqual(result["window_clip"][1] - result["window_clip"][0], .3 * scale)
        self.assertLess(abs(result["spectrum"]["peak_hz"] - 400), result["spectrum"]["resolution_hz"])
        onset = next(t for t, db in result["envelope"] if db > -40)
        self.assertLess(abs(onset - .2), .003)  # .4 nominal onset minus .2 origin
        self.assertEqual(result["sample_rate"], rate)

    def test_overrange_and_dc_remain_in_metrics(self):
        rate = 8000
        signal = np.full((rate, 2), 1.25)
        result = comparison.measure_series(signal, rate, 1, 0, (0, 1), (.2, .8), 0)
        self.assertAlmostEqual(result["metrics"]["peak_dbfs"], 20 * math.log10(1.25))
        self.assertEqual(result["metrics"]["dc"], 1.25)
        self.assertEqual(result["metrics"]["rail_samples"], 4800)
        self.assertIsNone(result["spectrum"]["peak_hz"])
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "refusing to clip"):
                comparison.write_pcm24(Path(directory) / "bad.wav", rate, signal)

    def test_partial_candidate_is_not_replaced_with_available_subset(self):
        samples = np.ones((800, 2)) * .1
        result = comparison.measure_series(samples, 8000, 1, 0, (0, .2), (.05, .15), 0)
        self.assertEqual(result["context_status"], "partial")
        self.assertEqual(result["window_status"], "partial")
        self.assertIsNone(result["metrics"])
        self.assertEqual(result["spectrum"]["points"], [])
        absent = comparison.measure_series(samples, 8000, 1, 0, (2, 3), (2, 3), 2)
        self.assertEqual(absent["window_status"], "outside_capture")
        self.assertEqual(absent["envelope"], [])

    def test_plot_reduction_retains_known_peak(self):
        rate = 48000
        signal = .1 * np.sin(2 * np.pi * 4321 * np.arange(rate * 2) / rate)
        result = comparison.spectrum(signal, rate, max_points=60)
        self.assertEqual(len(result["points"]), 60)
        peak = max(result["points"], key=lambda row: row[1])
        self.assertEqual(peak[0], 4321)

    def test_audio_asset_keeps_original_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "original.wav"
            payload = b"arbitrary original bytes including float overload\x00\xff"
            source.write_bytes(payload)
            asset = Path(directory) / "capture.js"
            comparison.write_audio_payload(asset, source, "capture")
            encoded = json.loads(asset.read_text().split("=", 1)[1].rstrip(";\n"))
            self.assertEqual(base64.b64decode(encoded), payload)

    def test_initial_page_loads_plot_payload_only_on_selection(self):
        item = {"id": "known:0", "segment_id": "known", "kind": "tone", "settings": {},
                "targets": {"4": {"large_marker": "PAYLOAD" * 100000}}}
        report = {"synthetic": None, "recording_name": "real.wav", "recording_sha256": "a",
                  "package_sha256": "b", "model_settings": {},
                  "analysis": {"status": "alignment_required", "flags": [], "windows": [item]},
                  "windows": [item]}
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            comparison.write_page_assets(out, report)
            self.assertLess((out / "index.html").stat().st_size, 100000)
            self.assertNotIn("large_marker", (out / "index.html").read_text(encoding="utf-8"))
            self.assertIn("large_marker", (out / "windows/0000.js").read_text(encoding="utf-8"))


class ComparisonMappingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rate = 8000
        cls.samples = recording(rate=cls.rate, swapped=True)
        cls.info = {"clip_level": 32767 / 32768}
        cls.manifest = manifest()
        cls.alignment = analyzer.align_markers(cls.samples, cls.rate, .01)
        time = np.arange(cls.rate * 120) / cls.rate
        model = np.column_stack((.08 * np.sin(2 * np.pi * 330 * time),
                                 .04 * np.sin(2 * np.pi * 660 * time)))
        cls.models = {"baseline": (cls.rate, model, cls.info),
                      "prototype": (cls.rate, model * .5, cls.info)}

    def windows(self, samples=None, alignment=True):
        samples = self.samples if samples is None else samples
        report = analyzer.analyze(samples, self.rate, self.info, self.manifest,
                                  self.alignment if alignment else None)
        return report, comparison.compare_windows(self.manifest, report,
            (self.rate, samples, self.info), self.models)

    def test_verified_swapped_mapping_is_used_for_every_window(self):
        report, windows = self.windows()
        self.assertEqual(report["socket_mapping"]["status"], "distinct")
        for window in windows:
            self.assertEqual(window["targets"]["4"]["capture"]["channel"], 2)
            self.assertEqual(window["targets"]["5"]["capture"]["channel"], 1)
            self.assertEqual(window["targets"]["4"]["baseline"]["channel"], 1)
            self.assertEqual(window["targets"]["5"]["baseline"]["channel"], 2)
        self.assertTrue(any("Short FF window" in flag for flag in windows[-1]["flags"]))
        json.dumps(windows, allow_nan=False)

    def test_duplicate_channels_stay_unresolved(self):
        duplicate = np.column_stack((self.samples[:, 0], self.samples[:, 0]))
        report, windows = self.windows(duplicate)
        self.assertEqual(report["socket_mapping"]["status"], "unresolved")
        for window in windows:
            self.assertIsNone(window["targets"]["4"]["capture"])
            self.assertIsNone(window["targets"]["5"]["capture"])

    def test_missing_markers_prevent_matching_instead_of_guessing(self):
        report, windows = self.windows(np.zeros((8000, 2)), alignment=False)
        self.assertEqual(report["status"], "alignment_required")
        self.assertEqual(len(windows), 5)
        self.assertTrue(all(window["targets"]["4"]["capture"] is None for window in windows))
        self.assertTrue(all(window["targets"]["4"]["baseline"] is not None for window in windows))

    def test_incomplete_capture_stays_explicit(self):
        report, windows = self.windows(self.samples[:10 * self.rate])
        self.assertTrue(any("complete scheduled run" in flag for flag in report["flags"]))
        late = next(window for window in windows if window["segment_id"] == "reference-end")
        self.assertEqual(late["targets"]["4"]["capture"]["window_status"], "outside_capture")
        self.assertIsNone(late["targets"]["4"]["capture"]["metrics"])

    def test_inline_metadata_cannot_close_script(self):
        text = '</script><img src=x onerror="bad()">\u2028&'
        encoded = comparison.safe_json({"title": text})
        self.assertNotIn("<", encoded)
        self.assertNotIn("&", encoded)
        self.assertEqual(json.loads(encoded)["title"], text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
