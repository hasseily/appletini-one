#!/usr/bin/env python3
"""Check an untrimmed PAL-01 stereo WAV before fitting any SSI sound model.

Requires NumPy. Uses one affine timing map for the entire recording, inferred
from both AY bookends. Never aligns, normalizes or equalizes individual phones.
The output contains measurements and review flags, not a calibrated chip model.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import zipfile

import numpy as np


BUILD_ID = "SSI263-CAL-PAL-01"


def load_manifest(path: Path) -> dict:
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            names = [name for name in archive.namelist()
                     if Path(name).name == "manifest.json"]
            if len(names) != 1:
                raise ValueError("Package must contain exactly one manifest.json")
            manifest = json.loads(archive.read(names[0]))
    else:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    if manifest.get("build_id") != BUILD_ID:
        raise ValueError("This analyzer needs the SSI263-CAL-PAL-01 manifest")
    tick = manifest["clock"]["tick_seconds"]
    if not math.isfinite(tick) or tick <= 0:
        raise ValueError("Invalid manifest tick duration")
    if not math.isclose(manifest["duration_seconds"], tick * 12000, abs_tol=1e-6):
        raise ValueError("Unexpected PAL-01 duration")
    return manifest


def read_wav(path: Path) -> tuple[int, np.ndarray, dict]:
    """Read integer/float RIFF WAV, including PCM WAVE_FORMAT_EXTENSIBLE.

    Keep input scale and channel order. Float samples outside [-1,1] remain
    outside that range so overload cannot disappear through conversion.
    """
    raw = path.read_bytes()
    if len(raw) < 12 or raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError("Expected a little-endian RIFF WAV")
    end = struct.unpack_from("<I", raw, 4)[0] + 8
    if end > len(raw):
        raise ValueError("Truncated RIFF WAV")
    fmt = data = None
    position = 12
    while position + 8 <= end:
        tag = raw[position:position + 4]
        size = struct.unpack_from("<I", raw, position + 4)[0]
        position += 8
        if position + size > end:
            raise ValueError("Truncated WAV chunk")
        if tag == b"fmt ":
            if fmt is not None:
                raise ValueError("Multiple WAV format chunks")
            fmt = raw[position:position + size]
        if tag == b"data":
            if data is not None:
                raise ValueError("Multiple WAV audio chunks")
            data = raw[position:position + size]
        position += size + (size & 1)
    if fmt is None or len(fmt) < 16 or data is None:
        raise ValueError("Missing WAV format or audio")
    encoding, channels, rate, byte_rate, align, bits = struct.unpack_from("<HHIIHH", fmt)
    valid_bits = bits
    if encoding == 0xFFFE:
        if len(fmt) < 40 or struct.unpack_from("<H", fmt, 16)[0] < 22:
            raise ValueError("Incomplete extensible WAV format")
        valid_bits = struct.unpack_from("<H", fmt, 18)[0]
        if fmt[28:40] != bytes.fromhex("00001000800000aa00389b71"):
            raise ValueError("Unsupported WAV subformat")
        encoding = struct.unpack_from("<I", fmt, 24)[0]
    if channels != 2:
        raise ValueError("Expected two separate recorded channels; do not sum them")
    if rate < 8000 or rate > 384000 or bits % 8 or not 0 < valid_bits <= bits:
        raise ValueError("Unsupported WAV sample format")
    if align != channels * bits // 8 or byte_rate != rate * align or len(data) % align:
        raise ValueError("Invalid WAV sample/block size")
    if encoding == 1 and bits in (8, 16, 24, 32):
        if bits == 8:
            samples = np.frombuffer(data, dtype=np.uint8).astype(np.float64) - 128
        elif bits == 24:
            octets = np.frombuffer(data, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
            values = octets[:, 0] | (octets[:, 1] << 8) | (octets[:, 2] << 16)
            samples = ((values ^ 0x800000) - 0x800000).astype(np.float64)
        else:
            samples = np.frombuffer(data, dtype=f"<i{bits // 8}").astype(np.float64)
        samples /= float(2 ** (bits - 1))
        clip_level = 1.0 - 2.0 ** (1 - valid_bits)
    elif encoding == 3 and bits in (32, 64):
        samples = np.frombuffer(data, dtype=f"<f{bits // 8}").astype(np.float64)
        clip_level = 1.0
    else:
        raise ValueError("Only integer PCM or IEEE float WAV is supported")
    samples = samples.reshape(-1, 2)
    if len(samples) == 0 or not np.isfinite(samples).all():
        raise ValueError("WAV contains no audio or non-finite samples")
    return rate, samples, {"sample_rate": rate, "channels": channels,
                          "container_bits": bits, "valid_bits": valid_bits,
                          "encoding": "PCM" if encoding == 1 else "float",
                          "duration_seconds": len(samples) / rate,
                          "clip_level": clip_level}


def db(value: float) -> float | None:
    return float(20 * math.log10(value)) if value > 0 else None


def levels(samples: np.ndarray, clip_level: float) -> list[dict]:
    return [{"rms_dbfs": db(float(np.sqrt(np.mean(channel * channel)))),
             "ac_rms_dbfs": db(float(np.std(channel))),
             "peak_dbfs": db(float(np.max(np.abs(channel)))),
             "dc": float(np.mean(channel)),
             "rail_samples": int(np.count_nonzero(np.abs(channel) >= clip_level))}
            for channel in samples.T]


def tone_runs(samples: np.ndarray, rate: int) -> list[dict]:
    """Find narrow-band 1/1.5 kHz markers, with 5 ms hops and 40 ms windows.

    Average channel powers (never waveforms) so reversed polarity is harmless.
    Unrelated speech may trigger individual runs; the bookend pattern and
    whole-recording timing fit below must also agree.
    """
    width = max(64, round(rate * 0.040))
    hop = max(1, round(rate * 0.005))
    taper = np.hanning(width)
    bins = np.fft.rfftfreq(width, 1 / rate)
    bands = [np.abs(bins - hz) <= 80 for hz in (1000, 1500)]
    starts = np.arange(0, len(samples) - width + 1, hop)
    labels = np.zeros(len(starts), dtype=np.uint8)
    for begin in range(0, len(starts), 256):
        positions = starts[begin:begin + 256, None] + np.arange(width)
        block = samples[positions]
        block = block - block.mean(axis=1, keepdims=True)
        energy = np.mean(block * block, axis=(1, 2))
        spectrum = np.abs(np.fft.rfft(block * taper[None, :, None], axis=1)) ** 2
        total = spectrum.sum(axis=(1, 2))
        powers = np.stack([spectrum[:, band, :].sum(axis=(1, 2)) for band in bands], axis=1)
        dominant = np.argmax(powers, axis=1)
        ratio = powers.max(axis=1) / np.maximum(total, 1e-30)
        valid = (ratio >= 0.65) & (energy >= 10 ** (-75 / 10))
        labels[begin:begin + len(block)] = np.where(valid, dominant + 1, 0)
    runs = []
    i = 0
    while i < len(labels):
        j = i + 1
        while j < len(labels) and labels[j] == labels[i]:
            j += 1
        if labels[i] and j - i >= 8:
            # Threshold crossings are approximated by frame centers. Keep the
            # resulting uncertainty in the report, not a false sample accuracy.
            start = (starts[i] + width / 2 - hop / 2) / rate
            stop = (starts[j - 1] + width / 2 + hop / 2) / rate
            runs.append({"tone": int(labels[i]), "start": start, "end": stop})
        i = j
    return runs


def align_markers(samples: np.ndarray, rate: int, tick_seconds: float) -> dict:
    runs = tone_runs(samples, rate)
    openings, closings = [], []
    def length_ok(run, seconds):
        return abs((run["end"] - run["start"]) - seconds) <= 0.055
    for a, b in zip(runs, runs[1:]):
        if (a["tone"], b["tone"]) == (1, 2) and length_ok(a, 0.2) and length_ok(b, 0.1):
            if abs(b["start"] - a["start"] - 40 * tick_seconds) < 0.04:
                openings.append((a, b))
    for a, b, c in zip(runs, runs[1:], runs[2:]):
        if (a["tone"], b["tone"], c["tone"]) == (2, 1, 2):
            if all(length_ok(run, duration) for run, duration in zip((a, b, c), (0.1, 0.2, 0.1))):
                if all(abs(delta - 40 * tick_seconds) < 0.04
                       for delta in (b["start"] - a["start"], c["start"] - b["start"])):
                    closings.append((a, b, c))
    expected = np.array([100, 140, 11650, 11690, 11730]) * tick_seconds
    matrix = np.column_stack([np.ones(5), expected])
    matches = []
    for opening in openings:
        for closing in closings:
            observed = np.array([r["start"] for r in (*opening, *closing)])
            offset, scale = np.linalg.lstsq(matrix, observed, rcond=None)[0]
            residual = float(np.max(np.abs(observed - (offset + scale * expected))))
            if 0.98 <= scale <= 1.02 and residual <= 0.025:
                matches.append({"method": "five AY bookend markers; one affine fit",
                                "offset_seconds": float(offset), "time_scale": float(scale),
                                "drift_ppm": float((scale - 1) * 1e6),
                                "max_marker_residual_seconds": residual,
                                "edge_uncertainty_seconds": 0.025,
                                "time_scale_uncertainty_ppm": float(0.05 / (expected[-1] - expected[0]) * 1e6),
                                "timing_note": "Combined host/recorder timing; not a direct chip-clock measurement. "
                                               "Marker edge uncertainty can cross adjacent 40 ms FF sweep steps.",
                                "observed_marker_seconds": observed.tolist()})
    if len(matches) != 1:
        raise ValueError(f"Expected one complete marker pair, found {len(matches)}; "
                         "inspect the capture or supply a measured offset/time scale")
    return matches[0]


def window_metrics(samples: np.ndarray, rate: int, clip_level: float) -> list[dict]:
    result = levels(samples, clip_level)
    taper = np.hanning(len(samples))
    bins = np.fft.rfftfreq(len(samples), 1 / rate)
    for channel, item in zip(samples.T, result):
        centered = channel - channel.mean()
        power = np.abs(np.fft.rfft(centered * taper)) ** 2
        total = float(power.sum())
        item["spectral_centroid_hz"] = float(np.dot(bins, power) / total) if total else None
        item["power_fractions"] = {
            f"{lo}-{hi}Hz": float(power[(bins >= lo) & (bins < hi)].sum() / total) if total else None
            for lo, hi in ((0, 70), (70, 500), (500, 4000), (4000, rate / 2 + 1))}
        thirds = np.array_split(channel, 3)
        third_rms = [float(np.sqrt(np.mean(part * part))) for part in thirds]
        item["third_rms_dbfs"] = [db(value) for value in third_rms]
        item["last_vs_first_third_db"] = db(third_rms[-1] / third_rms[0]) if third_rms[0] else None
    return result


def analyze(samples: np.ndarray, rate: int, info: dict, manifest: dict,
            alignment: dict | None = None) -> dict:
    flags = []
    report = {"build_id": BUILD_ID, "wav": info, "channels": levels(samples, info["clip_level"]),
              "flags": flags, "windows": [],
              "scope": "Capture quality and measurements only. All fitting windows remain unverified. "
                       "No gain, EQ, per-phone alignment, or chip-model fitting is applied."}
    if any(channel["rail_samples"] for channel in report["channels"]):
        flags.append("Samples reach a digital rail; inspect clipping before fitting.")
    if rate < 48000:
        flags.append("Sample rate is below the requested 48 kHz; usable bandwidth is reduced.")
    if alignment is None:
        try:
            alignment = align_markers(samples, rate, manifest["clock"]["tick_seconds"])
        except ValueError as error:
            flags.append(str(error))
            report["status"] = "alignment_required"
            return report
    offset, scale = alignment["offset_seconds"], alignment["time_scale"]
    if not math.isfinite(offset) or not math.isfinite(scale) or scale <= 0:
        raise ValueError("Offset must be finite and time scale must be finite and positive")
    report["alignment"] = alignment
    if abs(scale - 1) > 0.002:
        flags.append("Global timing differs by more than 0.2%; check clock/recording speed.")
    mapped_end = offset + scale * manifest["duration_seconds"]
    if offset < -0.025 or mapped_end > len(samples) / rate + 0.025:
        flags.append("The WAV does not cover the complete scheduled run.")
    tick_seconds = manifest["clock"]["tick_seconds"]
    for segment in manifest["segments"]:
        for window in segment["candidate_windows"]:
            begin = offset + scale * window["start_tick"] * tick_seconds
            end = offset + scale * window["end_tick"] * tick_seconds
            start_index, stop_index = round(begin * rate), round(end * rate)
            item = {"segment_id": segment["segment_id"], "kind": segment["kind"],
                    "purpose": window["purpose"], "settings": segment["settings"],
                    "start_seconds": begin, "end_seconds": end,
                    "settled": "unverified; inspect waveform and spectrum"}
            if start_index < 0 or stop_index > len(samples) or stop_index - start_index < 3:
                item["status"] = "outside_capture"
            else:
                item["status"] = "measured"
                item["channels"] = window_metrics(samples[start_index:stop_index], rate, info["clip_level"])
            report["windows"].append(item)
    by_name = {item["segment_id"]: item for item in report["windows"] if item["status"] == "measured"}
    # Initial silence follows both SSI CTL stops and AY mutes, before markers.
    # Compare AC levels to this baseline; DC or an almost silent channel must
    # not turn into a confident chip assignment through a large level ratio.
    quiet_start = round((offset + scale * .2) * rate)
    quiet_end = round((offset + scale * .7) * rate)
    quiet = (levels(samples[quiet_start:quiet_end], info["clip_level"])
             if 0 <= quiet_start < quiet_end <= len(samples) else None)
    report["initial_quiet_channels"] = quiet
    mapping = {}
    for target in (4, 5):
        item = by_name.get(f"isolate-{target}")
        if item is None:
            continue
        powers = [channel["ac_rms_dbfs"] for channel in item["channels"]]
        if all(value is None for value in powers):
            continue
        # Below -100 dBFS retain only a lower bound on channel separation.
        # Do not display an invented hundreds-of-dB ratio for exact zero.
        values = [max(value if value is not None else -100.0, -100.0) for value in powers]
        louder = int(values[1] > values[0])
        separation = abs(values[1] - values[0])
        background = quiet[louder]["ac_rms_dbfs"] if quiet else None
        signal_above_quiet = values[louder] - max(background if background is not None else -100.0, -100.0)
        clear = quiet is not None and separation >= 6 and signal_above_quiet >= 12
        mapping[str(target)] = {"candidate_channel": louder + 1,
                                "isolation_level_difference_db": separation,
                                "difference_is_lower_bound": any(value is None or value < -100 for value in powers),
                                "signal_above_quiet_db": signal_above_quiet,
                                "confidence": "clear" if clear else "ambiguous"}
    clear = len(mapping) == 2 and all(item["confidence"] == "clear" for item in mapping.values())
    clear = clear and mapping["4"]["candidate_channel"] != mapping["5"]["candidate_channel"]
    report["socket_mapping"] = {"status": "distinct" if clear else "unresolved", "isolations": mapping}
    if not clear:
        flags.append("Socket/channel assignment is unresolved; keep both recorded channels separate.")
    if "reference-start" in by_name and "reference-end" in by_name:
        deltas = []
        for start, end in zip(by_name["reference-start"]["channels"], by_name["reference-end"]["channels"]):
            a, b = start["ac_rms_dbfs"], end["ac_rms_dbfs"]
            deltas.append(b - a if a is not None and b is not None else None)
        report["reference_end_minus_start_db"] = deltas
        if any(delta is None or abs(delta) > 1 for delta in deltas):
            flags.append("Repeated reference levels differ by over 1 dB or are silent; inspect gain/state drift.")
    report["status"] = "review_required" if flags else "measured_windows_need_review"
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wav", type=Path)
    parser.add_argument("--manifest", required=True, type=Path, help="PAL-01 manifest.json or tester ZIP")
    parser.add_argument("--out", type=Path, required=True, help="JSON measurements and review flags")
    parser.add_argument("--offset-seconds", type=float, help="Manual recording time of scheduled tick zero")
    parser.add_argument("--time-scale", type=float, default=1.0, help="Manual recorded seconds / scheduled second")
    args = parser.parse_args()
    if args.offset_seconds is None and args.time_scale != 1.0:
        parser.error("--time-scale requires --offset-seconds")
    manifest = load_manifest(args.manifest)
    rate, samples, info = read_wav(args.wav)
    alignment = None if args.offset_seconds is None else {
        "method": "manual; supplied by analyst", "offset_seconds": args.offset_seconds,
        "time_scale": args.time_scale, "drift_ppm": (args.time_scale - 1) * 1e6}
    report = analyze(samples, rate, info, manifest, alignment)
    report["input_sha256"] = hashlib.sha256(args.wav.read_bytes()).hexdigest()
    report["manifest_sha256"] = hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(f"{report['status']}: {len(report['windows'])} candidate windows -> {args.out}")
    for flag in report["flags"]:
        print(f"REVIEW: {flag}")
    return 2 if report["status"] == "alignment_required" else 0


if __name__ == "__main__":
    raise SystemExit(main())
