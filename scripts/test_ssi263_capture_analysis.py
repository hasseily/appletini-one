#!/usr/bin/env python3
"""Check capture analysis against known signals, not a generated SSI waveform."""
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

import numpy as np

from analyze_ssi263_capture import (BUILD_ID, align_markers, analyze, load_manifest,
                                   read_wav)


def manifest():
    segments = []
    for name, kind, start, end in (("isolate-4", "isolation", 280, 340),
                                   ("isolate-5", "isolation", 480, 540),
                                   ("reference-start", "reference", 680, 740),
                                   ("reference-end", "reference", 11530, 11590),
                                   ("sweep-FF255", "ff-sweep", 8700, 8704)):
        segments.append({"segment_id": name, "kind": kind, "settings": {},
                         "candidate_windows": [{"start_tick": start, "end_tick": end,
                                                "purpose": "known test window"}]})
    return {"build_id": BUILD_ID, "duration_seconds": 120,
            "clock": {"tick_seconds": 0.01}, "segments": segments}


def recording(rate=8000, offset=0.8, scale=1.0007, swapped=True):
    samples = np.zeros((round((offset + scale * 120 + 0.3) * rate), 2))
    def tone(start, length, hz, amplitudes):
        first = round((offset + scale * start) * rate)
        last = round((offset + scale * (start + length)) * rate)
        t = np.arange(last - first) / rate
        signal = np.sin(2 * np.pi * hz * t)
        samples[first:last] += signal[:, None] * np.asarray(amplitudes)
    # The different channel polarities catch accidental stereo summation.
    for start, length, hz in ((1, .2, 1000), (1.4, .1, 1500),
                              (116.5, .1, 1500), (116.9, .2, 1000), (117.3, .1, 1500)):
        tone(start, length, hz, (.12, -.12))
    left, right = ((.01, .12), (.12, .01)) if swapped else ((.12, .01), (.01, .12))
    tone(2, 1.5, 110, left)
    tone(4, 1.5, 110, right)
    tone(6, 1.5, 110, (.12, .12))
    tone(114.5, 1.5, 110, (.12, .12))
    return samples


def wav_bytes(data, bits=16, encoding=1, extensible=False, channels=2, rate=48000):
    align = channels * bits // 8
    fmt = struct.pack("<HHIIHH", 0xFFFE if extensible else encoding,
                      channels, rate, rate * align, align, bits)
    if extensible:
        fmt += struct.pack("<HHI", 22, bits, 3)
        fmt += struct.pack("<I", encoding) + bytes.fromhex("00001000800000aa00389b71")
    def chunk(tag, body):
        return tag + struct.pack("<I", len(body)) + body + (b"\0" if len(body) & 1 else b"")
    body = b"WAVE" + chunk(b"JUNK", b"odd") + chunk(b"fmt ", fmt) + chunk(b"data", data)
    return b"RIFF" + struct.pack("<I", len(body)) + body


class CaptureAnalysisTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.samples = recording()
        cls.info = {"clip_level": 32767 / 32768}
        cls.alignment = align_markers(cls.samples, 8000, .01)

    def test_global_alignment_and_channel_identity(self):
        self.assertLess(abs(self.alignment["offset_seconds"] - .8), .025)
        self.assertLess(abs(self.alignment["time_scale"] - 1.0007), .00015)
        result = analyze(self.samples, 8000, self.info, manifest(), self.alignment)
        self.assertEqual(result["socket_mapping"]["status"], "distinct")
        self.assertEqual(result["socket_mapping"]["isolations"]["4"]["candidate_channel"], 2)
        self.assertEqual(result["socket_mapping"]["isolations"]["5"]["candidate_channel"], 1)
        self.assertEqual(len(result["windows"]), 5)
        for delta in result["reference_end_minus_start_db"]:
            self.assertLess(abs(delta), .02)
        self.assertTrue(all("unverified" in w["settled"] for w in result["windows"]))
        # Silence must remain silence, not become an arbitrary fitting target.
        self.assertIsNone(result["windows"][-1]["channels"][0]["rms_dbfs"])
        json.dumps(result, allow_nan=False)

    def test_missing_marker_and_multiple_runs_are_not_guessed(self):
        with self.assertRaises(ValueError):
            align_markers(self.samples[:8000 * 30], 8000, .01)
        with self.assertRaises(ValueError):
            align_markers(np.concatenate((self.samples, self.samples)), 8000, .01)
        result = analyze(np.zeros((8000, 2)), 8000, self.info, manifest())
        self.assertEqual(result["status"], "alignment_required")
        self.assertEqual(result["windows"], [])

    def test_manual_truncated_capture_reports_missing_windows(self):
        result = analyze(self.samples[:8000 * 10], 8000, self.info, manifest(), self.alignment)
        self.assertTrue(any("complete scheduled run" in flag for flag in result["flags"]))
        self.assertTrue(any(item["status"] == "outside_capture" for item in result["windows"]))

    def test_duplicate_channels_and_clipping_remain_visible(self):
        samples = self.samples.copy()
        samples[:, 1] = samples[:, 0]
        samples[0, 0] = 1
        result = analyze(samples, 8000, self.info, manifest(), self.alignment)
        self.assertEqual(result["socket_mapping"]["status"], "unresolved")
        self.assertEqual(result["channels"][0]["rail_samples"], 1)
        self.assertTrue(any("digital rail" in flag for flag in result["flags"]))

    def test_full_rate_alignment(self):
        samples = recording(rate=48000, offset=.35, scale=.9996, swapped=False)
        timing = align_markers(samples, 48000, .01)
        self.assertLess(abs(timing["offset_seconds"] - .35), .025)
        self.assertLess(abs(timing["time_scale"] - .9996), .00015)

    def test_dc_and_noise_floor_do_not_identify_chips(self):
        samples = np.zeros_like(self.samples)
        # Alternating DC levels have no speech energy despite huge raw ratios.
        samples[round(3.0 * 8000):round(4.3 * 8000), 0] = .2
        samples[round(5.0 * 8000):round(6.3 * 8000), 1] = .2
        result = analyze(samples, 8000, self.info, manifest(), self.alignment)
        self.assertEqual(result["socket_mapping"]["status"], "unresolved")
        # Strong constant background must not qualify as an isolated chip.
        rng = np.random.default_rng(1)
        samples = rng.normal(0, .03, self.samples.shape)
        samples += self.samples * .1
        result = analyze(samples, 8000, self.info, manifest(), self.alignment)
        self.assertEqual(result["socket_mapping"]["status"], "unresolved")

    def test_pcm_and_float_formats(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "test.wav"
            for bits in (8, 16, 24, 32):
                limit = 2 ** (bits - 1)
                values = np.array([-limit, -limit // 2, 0, limit // 2, limit - 1, 0], dtype=np.int64)
                if bits == 8:
                    encoded = (values + 128).astype(np.uint8).tobytes()
                elif bits == 24:
                    encoded = b"".join((int(value) & 0xFFFFFF).to_bytes(3, "little") for value in values)
                else:
                    encoded = values.astype(f"<i{bits // 8}").tobytes()
                for extensible in (False, True):
                    path.write_bytes(wav_bytes(encoded, bits, extensible=extensible))
                    rate, samples, info = read_wav(path)
                    self.assertEqual(rate, 48000)
                    np.testing.assert_array_equal(samples.ravel(), values / limit)
                    self.assertEqual(info["container_bits"], bits)
            for bits in (32, 64):
                values = np.array([-.5, .25, 1.1, -.2], dtype=f"<f{bits // 8}")
                path.write_bytes(wav_bytes(values.tobytes(), bits, encoding=3))
                _, samples, _ = read_wav(path)
                np.testing.assert_array_equal(samples.ravel(), values.astype(np.float64))

    def test_invalid_wav_and_manifest_fail(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "test.wav"
            good = wav_bytes(b"\0" * 16)
            cases = (good[:-1], wav_bytes(b"\0" * 16, channels=1),
                     wav_bytes(struct.pack("<ff", float("nan"), 0), 32, encoding=3),
                     wav_bytes(b"\0" * 3))
            for case in cases:
                path.write_bytes(case)
                with self.assertRaises(ValueError):
                    read_wav(path)
            path = Path(temporary) / "manifest.json"
            path.write_text(json.dumps(manifest()))
            self.assertEqual(load_manifest(path)["build_id"], BUILD_ID)
            archive_path = Path(temporary) / "package.zip"
            with zipfile.ZipFile(archive_path, "w") as archive:
                archive.writestr("capture/manifest.json", json.dumps(manifest()))
            self.assertEqual(load_manifest(archive_path), manifest())
            bad = manifest()
            bad["build_id"] = "OTHER"
            path.write_text(json.dumps(bad))
            with self.assertRaises(ValueError):
                load_manifest(path)


if __name__ == "__main__":
    unittest.main(verbosity=2)
