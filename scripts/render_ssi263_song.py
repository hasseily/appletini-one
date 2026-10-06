#!/usr/bin/env python3
"""Render a song-to-Phasor score with native SSI vocals and the original AY RTL.

The song framework must include its physical-ssi263 compiler profile. Its score
and sources are read only. Windows uses WSL for the existing Verilator AY driver;
the native SSI renderer still runs locally, without Vivado.
"""
from __future__ import annotations

import argparse
from dataclasses import asdict
import hashlib
import html
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import wave

ROOT = Path(__file__).resolve().parents[1]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def use_framework(song: Path) -> Path:
    framework = song.parent / "song_to_phasor"
    if not (framework / "phasor/compiler.py").is_file():
        raise ValueError("song directory needs sibling song_to_phasor framework")
    sys.path.insert(0, str(framework))
    return framework


def wsl_path(path: Path) -> str:
    path = path.resolve()
    if len(path.drive) != 2 or path.drive[1] != ":":
        raise ValueError("WSL rendering needs paths on a local Windows drive")
    return "/mnt/" + path.drive[0].lower() + "/" + "/".join(path.parts[1:])


def ay_only(song: Path, output: Path, clock_hz: int, frames: int) -> None:
    """Run the framework's exact AY driver without requiring NumPy in WSL."""
    use_framework(song)
    from phasor.full_render import _binary

    cache = Path(tempfile.gettempdir()) / "ssi263-song-ay-cache"
    executable, cache_hit, fingerprint = _binary(output / "ay_firmware", cache)
    command = [str(executable), str(output / "ay-events.txt"),
               str(output / "ay-channels.pcm"), str(frames), str(clock_hz), "0"]
    result = subprocess.run(command, check=True, capture_output=True, text=True)
    report = {"renderer": "verilator-four-ym2149", "ay_clock_hz": clock_hz,
              "source_fingerprint": fingerprint, "binary_cache_hit": cache_hit,
              **json.loads(result.stdout)}
    (output / "ay-driver.json").write_text(json.dumps(report, indent=2) + "\n")


def write_wav(path: Path, samples) -> None:
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(2)
        stream.setsampwidth(2)
        stream.setframerate(48_000)
        stream.writeframes(samples.astype("<i2").tobytes())


def render_song(song: Path, output: Path, region: str, distro: str,
                ssi_mix_gain: float = 1.25, reference: str | None = None) -> dict:
    import numpy as np
    from render_ssi263 import render, reference_settings
    from ssi263_host_data import Event, Trace

    framework = use_framework(song)
    from phasor.compiler import AY_CLOCKS, compile_score
    from phasor.full_render import DEFAULT_PAN, mix_ay
    from phasor.hardware import TARGET_SOURCE_COMMIT, TARGET_SOURCE_SHA256
    from phasor.stream import decode, encode

    if not math.isfinite(ssi_mix_gain) or not 0 < ssi_mix_gain <= 4:
        raise ValueError("SSI mix gain must be finite and greater than 0, up to 4")
    output.mkdir(parents=True, exist_ok=True)
    score_path = song / "score.json"
    score = json.loads(score_path.read_text(encoding="utf-8"))
    clock = AY_CLOCKS[region] // 2
    events, compilation = compile_score(score, clock=region, profile="physical-ssi263")
    tick_hz, duration = score["tick_hz"], score["duration_ticks"]
    packed = encode(events, tick_hz, duration)
    decoded, stream_meta = decode(packed)
    if decoded != events:
        raise RuntimeError("PHS1 register stream failed its round trip")
    (output / "song.phs").write_bytes(packed)
    (output / "compile.json").write_text(json.dumps(compilation, indent=2) + "\n")
    shutil.copyfile(score_path, output / "score.json")
    sources = [score_path, *sorted(framework.glob("phasor/*.py"))]
    provenance = {p.relative_to(song.parent).as_posix(): sha256(p) for p in sources}
    source_commit = subprocess.run(["git", "-C", str(song), "rev-parse", "HEAD"],
                                   check=True, capture_output=True, text=True).stdout.strip()

    # Preserve write order within a 100 Hz score tick. Bus execution overhead is
    # intentionally not invented; each nominal timestamp rounds down by <1 XCK.
    writes = [Event(tick * clock // tick_hz, target - 4, reg, value, index)
              for index, (tick, target, reg, value) in enumerate(events)
              if target in (4, 5)]
    metadata = {"name": score["title"], "source_repository": "appletini-software",
                "source_commit": source_commit, "score_sha256": sha256(score_path),
                "source_sha256": provenance, "compiler_profile": "physical-ssi263",
                "region": region, "score_tick_hz": tick_hz,
                "timing": "Nominal score tick times; same-tick write order retained. CPU bus overhead excluded.",
                "channel_order": ["secondary SSI / left", "primary SSI / right"]}
    trace = Trace(writes, clock, duration * clock // tick_hz, metadata)
    trace.validate()
    (output / "trace.json").write_text(json.dumps({
        "effective_clock_hz": clock, "duration_ticks": trace.duration_ticks,
        "events": [asdict(e) for e in writes], "metadata": metadata}, indent=2) + "\n")
    vocals_report = render(trace, output / "vocals", "prototype", **reference_settings(reference))
    shutil.copyfile(output / "vocals/prototype.wav", output / "vocals.wav")

    # Only the original AY RTL and mixer are read from the song's pinned
    # firmware revision. No legacy speech source or ROM generator is run.
    ay_sources = ("hdl/apple/YM2149.sv", "hdl/apple/mockingboard.sv")
    for relative in ay_sources:
        data = subprocess.run(["git", "-C", str(ROOT), "show",
                               f"{TARGET_SOURCE_COMMIT}:{relative}"],
                              check=True, capture_output=True).stdout
        if hashlib.sha256(data).hexdigest() != TARGET_SOURCE_SHA256[relative]:
            raise RuntimeError(f"Pinned AY source hash mismatch: {relative}")
        destination = output / "ay_firmware" / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
    (output / "ay-events.txt").write_text("".join(
        f"{tick * 18_432_000 // tick_hz} {target} {reg} {value}\n"
        for tick, target, reg, value in events if target < 4 and tick < duration),
        encoding="ascii")
    frames = (duration * 48_000 + tick_hz - 1) // tick_hz
    if os.name == "nt":
        subprocess.run(["wsl", "-d", distro, "--", "python3", wsl_path(Path(__file__)),
                        "--song", wsl_path(song), "--output", wsl_path(output),
                        "--ay-only", "--ay-clock", str(AY_CLOCKS[region]),
                        "--frames", str(frames)], check=True)
    else:
        ay_only(song, output, AY_CLOCKS[region], frames)
    channels = np.fromfile(output / "ay-channels.pcm", dtype=np.uint8)
    if len(channels) != frames * 12:
        raise RuntimeError("Incorrect AY render length")
    backing = mix_ay(channels.reshape(-1, 12), DEFAULT_PAN)
    write_wav(output / "backing.wav", backing)
    with wave.open(str(output / "vocals.wav"), "rb") as stream:
        vocals = np.frombuffer(stream.readframes(stream.getnframes()), dtype="<i2").reshape(-1, 2)
    if vocals.shape != backing.shape:
        raise RuntimeError("Vocal and AY stem lengths differ")
    # Card-level balance only: scale the completed SSI output, including both
    # voice and frication. Keep the raw model stem for checkpoint comparisons.
    mix_vocals = np.rint(vocals.astype(np.float64) * ssi_mix_gain).astype(np.int32)
    vocal_clips = int(np.count_nonzero((mix_vocals < -32768) | (mix_vocals > 32767)))
    if vocal_clips:
        raise RuntimeError(f"SSI mix stem would clip {vocal_clips} samples")
    reference = vocals.astype(np.int32) + backing.astype(np.int32)
    mixed = mix_vocals + backing.astype(np.int32)
    clipped = int(np.count_nonzero((mixed < -32768) | (mixed > 32767)))
    reference_clips = int(np.count_nonzero((reference < -32768) | (reference > 32767)))
    if clipped or reference_clips:
        raise RuntimeError(f"Mix would clip {clipped} samples, reference {reference_clips}; keep the stems for review")
    stem = "house-of-the-rising-sun-native-ssi263"
    wav_path, mp3_path = output / (stem + ".wav"), output / (stem + ".mp3")
    reference_wav = output / (stem + "-reference.wav")
    reference_mp3 = output / (stem + "-reference.mp3")
    write_wav(wav_path, mixed)
    write_wav(reference_wav, reference)
    write_wav(output / "vocals-mixed.wav", mix_vocals)
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        raise RuntimeError("ffmpeg is required for the requested MP3")
    for source_wav, destination_mp3 in ((wav_path, mp3_path), (reference_wav, reference_mp3)):
        subprocess.run([ffmpeg, "-v", "error", "-nostdin", "-y", "-i", str(source_wav),
                        "-codec:a", "libmp3lame", "-q:a", "2", "-metadata",
                        "title=House of the Rising Sun - native SSI model", "-metadata",
                        "artist=Appletini Phasor / SSI-263", str(destination_mp3)], check=True)
    gain_db = 20 * math.log10(ssi_mix_gain)
    report = {"title": score["title"], "firmware_target": "F1.2.5-d1",
              "seconds": frames / 48_000, "frames": frames, "sample_rate": 48_000,
              "region": region, "stream": stream_meta, "source": metadata,
              "ssi_writes": len(writes), "ay_writes": sum(e[1] < 4 for e in events),
              "mix": "Original AY pan/scaling plus separate SSI output trim; no normalization or EQ",
              "ssi_mix_gain": ssi_mix_gain, "ssi_mix_gain_db": gain_db,
              "ssi_mix_peak_pcm": int(np.abs(mix_vocals).max()),
              "ssi_mix_clipped_samples": vocal_clips,
              "reference_peak_pcm": int(np.abs(reference).max()),
              "reference_clipped_samples": reference_clips,
              "pan": DEFAULT_PAN, "peak_pcm": int(np.abs(mixed).max()),
              "clipped_samples": clipped, "ssi": vocals_report,
              "ay": json.loads((output / "ay-driver.json").read_text()),
              "files": {p.name: sha256(p) for p in (wav_path, mp3_path, reference_wav, reference_mp3,
                        output / "vocals.wav", output / "vocals-mixed.wav", output / "backing.wav")},
              "limits": ["This uses the corrected physical SSI compiler profile: pitch and FF differ from the old Appletini MP3.",
                         "Composition, phonemes, musical timing and AY accompaniment are unchanged.",
                         "The SSI mix trim follows listening feedback; physical chip levels, Phasor analog mix, output frequency response and CPU bus overhead remain unmeasured.",
                         "The SSI engine retains its documented prototype conflicts and provisional attack envelope."]}
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    cards = "".join(f'<section><h2>{title}</h2><audio controls preload="metadata" '
                    f'src="{name}?v={sha256(output / name)[:12]}"></audio>'
                    f'<p><a href="{name}">Download</a></p></section>' for title, name in (
                        (f"Full song — SSI {gain_db:+.2f} dB", mp3_path.name),
                        ("Reference — previous SSI / AY balance", reference_mp3.name),
                        ("SSI vocals at revised mix level", "vocals-mixed.wav"),
                        ("Original AY backing", "backing.wav")))
    (output / "listen.html").write_text('<!doctype html><meta charset="utf-8">'
        '<title>House of the Rising Sun — native SSI</title><style>'
        'body{max-width:850px;margin:40px auto;padding:0 24px;background:#171b20;color:#e3e9ef;font:16px/1.5 system-ui}'
        'section{padding:18px 24px;margin:20px 0;background:#232a32;border-radius:12px}'
        'audio{width:100%}a{color:#9ecfff}p{color:#b8c4d0}h2{font-size:19px}</style>'
        '<h1>House of the Rising Sun</h1><p>Current native SSI model, with the original AY accompaniment. '
        f'{frames / 48_000:.2f} seconds; {region.upper()} clock; corrected physical SSI pitch and filter bytes.</p>'
        '<p>The composition and register controls come from the song score in appletini-software. '
        f'SSI output is multiplied by {ssi_mix_gain:g} ({gain_db:+.2f} dB); AY levels stay fixed. '
        'The reference uses the previous balance. This is a model preview, with no normalization. Its analog mix and attack timing '
        'have not been matched to a physical Phasor.</p>' + cards +
        f'<p><a href="{html.escape(wav_path.name)}">Full-resolution WAV</a> · '
        '<a href="report.json">Source hashes and model notes</a></p>', encoding="utf-8")
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--song", type=Path, required=True, help="house_of_the_rising_sun directory")
    parser.add_argument("--output", type=Path, default=ROOT / "build/ssi263_host/house_rising_sun")
    parser.add_argument("--region", choices=("pal", "ntsc"), default="pal")
    parser.add_argument("--wsl-distro", default="Ubuntu")
    parser.add_argument("--reference", choices=("balanced",), help="accepted SSI voice/noise balance")
    parser.add_argument("--ssi-mix-gain", type=float, default=1.25,
                        help="SSI output multiplier relative to AY (default: 1.25, about +2 dB)")
    parser.add_argument("--ay-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--ay-clock", type=int, help=argparse.SUPPRESS)
    parser.add_argument("--frames", type=int, help=argparse.SUPPRESS)
    args = parser.parse_args()
    song, output = args.song.resolve(), args.output.resolve()
    if args.ay_only:
        ay_only(song, output, args.ay_clock, args.frames)
    else:
        report = render_song(song, output, args.region, args.wsl_distro, args.ssi_mix_gain, args.reference)
        print(json.dumps({key: report[key] for key in
                          ("seconds", "frames", "ssi_writes", "ay_writes", "peak_pcm", "clipped_samples")}, indent=2))
        print(output / "listen.html")


if __name__ == "__main__":
    main()
