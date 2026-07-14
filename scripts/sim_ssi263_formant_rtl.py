#!/usr/bin/env python3
"""Run an RTL simulation of the SSI263 formant backend and capture its audio.

This is intentionally narrower than the normal firmware build: it instantiates
ssi263_formant_backend directly, drives deterministic phoneme register values,
and writes the exact signed 16-bit audio output sampled at 48 kHz.
"""

from __future__ import annotations

import argparse
import csv
import math
import re
import shutil
import struct
import subprocess
import wave
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / "build" / "ssi263_formant_rtl"
SAMPLE_RATE = 48_000


@dataclass(frozen=True)
class PhoneEvent:
    durphon: int
    inflect: int
    rateinf: int
    ctrlamp: int
    filfreq: int
    samples: int
    label: str


CLASSIC_ADV_REGS = [
    (0x29, 0x52, 0xB8, 0x7B, 0xE6),
    (0x2D, 0x52, 0xB8, 0x7B, 0xE6),
    (0x60, 0x52, 0xB8, 0x7B, 0xE6),
    (0x0C, 0x52, 0xB8, 0x7B, 0xE6),
    (0x30, 0x52, 0xB8, 0x7B, 0xE6),
    (0x47, 0x52, 0xB8, 0x7B, 0xE6),
    (0x29, 0x52, 0xB8, 0x7B, 0xE6),
    (0x4C, 0x52, 0xB8, 0x7B, 0xE6),
    (0x0C, 0x52, 0xB8, 0x7B, 0xE6),
    (0x25, 0x52, 0xB8, 0x7B, 0xE6),
    (0x33, 0x52, 0xB8, 0x7B, 0xE6),
    (0xEC, 0x52, 0xB8, 0x7B, 0xE6),
    (0x47, 0x52, 0xB8, 0x7B, 0xE6),
    (0x47, 0x52, 0xB8, 0x7B, 0xE6),
    (0x78, 0x52, 0xB8, 0x7B, 0xE6),
    (0x68, 0x52, 0xB8, 0x7B, 0xE6),
    (0x72, 0x52, 0xB8, 0x7B, 0xE6),
    (0x5C, 0x52, 0xB8, 0x7B, 0xE6),
]


def parse_phone_map() -> tuple[dict[int, int], dict[int, int]]:
    pkg = (ROOT / "hdl" / "apple" / "ssi263_formant_pkg.sv").read_text()
    mapping: dict[int, int] = {}
    duration_mapping: dict[int, int] = {}
    pattern = re.compile(r"6'h([0-9A-Fa-f]{2}): ssi263_to_sc01_phone = 6'h([0-9A-Fa-f]{2});")
    for match in pattern.finditer(pkg):
        mapping[int(match.group(1), 16)] = int(match.group(2), 16)
    audio_pattern = re.compile(r"6'h([0-9A-Fa-f]{2}): ssi263_to_sc01_audio_phone = 6'h([0-9A-Fa-f]{2});")
    for match in audio_pattern.finditer(pkg):
        mapping[int(match.group(1), 16)] = int(match.group(2), 16)
    dur_pattern = re.compile(r"8'h([0-9A-Fa-f]{2}): ssi263_to_sc01_audio_phone = 6'h([0-9A-Fa-f]{2});")
    for match in dur_pattern.finditer(pkg):
        duration_mapping[int(match.group(1), 16)] = int(match.group(2), 16)
    return mapping, duration_mapping


def audio_sc01_for_event(event: PhoneEvent, phone_map: dict[int, int],
                         duration_phone_map: dict[int, int]) -> int:
    phone = event.durphon & 0x3F
    current_function = (event.durphon >> 6) & 0x3
    dur = 3 if current_function == 1 else current_function
    key = (dur << 6) | phone
    return duration_phone_map.get(key, phone_map.get(phone, 0x3F))


def parse_phone_list(value: str) -> list[int]:
    if value.lower() == "classic":
        return []
    phones: list[int] = []
    for item in value.replace(" ", "").split(","):
        if item:
            phones.append(int(item, 16))
    return phones


def build_events(args: argparse.Namespace) -> list[PhoneEvent]:
    segment_samples = max(1, int(round(SAMPLE_RATE * args.segment_ms / 1000.0)))
    if args.preset == "classic":
        return [
            PhoneEvent(dur, inf, rate, ctrl, filt, segment_samples, f"classic_{idx:02d}_ssi{dur & 0x3F:02X}")
            for idx, (dur, inf, rate, ctrl, filt) in enumerate(CLASSIC_ADV_REGS)
        ]

    phones = parse_phone_list(args.phones)
    return [
        PhoneEvent(phone & 0x3F, args.inflect, args.rateinf, args.ctrlamp,
                   args.filfreq, segment_samples, f"ssi{phone & 0x3F:02X}")
        for phone in phones
    ]


def sv_array(values: list[int], width: int = 8) -> str:
    return "'{" + ", ".join(f"{width}'h{value:0{max(1, width // 4)}X}" for value in values) + "}"


def int_array(values: list[int]) -> str:
    return "'{" + ", ".join(str(value) for value in values) + "}"


def write_testbench(path: Path, events: list[PhoneEvent], sample_txt: Path,
                    tick_cycles: int, sc01_map: str) -> None:
    dur = [event.durphon for event in events]
    inf = [event.inflect for event in events]
    rate = [event.rateinf for event in events]
    ctrl = [event.ctrlamp for event in events]
    filt = [event.filfreq for event in events]
    samples = [event.samples for event in events]
    sample_path = sample_txt.as_posix()
    sc01_expr = "dur[5:0]" if sc01_map == "identity" else "ssi263_to_sc01_phone(dur[5:0])"

    path.write_text(f"""`timescale 1ns / 1ps

import ssi263_formant_pkg::*;

module ssi263_formant_audio_tb;
    localparam int TICK_CYCLES = {tick_cycles};
    localparam int EVENT_COUNT = {len(events)};

    logic clk = 1'b0;
    logic rstn = 1'b0;
    logic card_enabled = 1'b1;
    logic warm_reset = 1'b0;
    logic audio_tick = 1'b0;
    logic start = 1'b0;
    logic [5:0] start_phoneme = 6'd0;
    logic [5:0] start_sc01_phone = 6'd0;
    logic start_votrax = 1'b0;
    logic [1:0] current_function = 2'd0;
    logic [7:0] duration_phoneme = 8'd0;
    logic [7:0] inflection = 8'd0;
    logic [7:0] rate_inflection = 8'd0;
    logic [7:0] ctrl_art_amp = 8'h0F;
    logic [7:0] filter_freq = 8'd0;
    logic phoneme_done;
    logic signed [15:0] audio;

    logic [7:0] dur_values [0:EVENT_COUNT-1] = {sv_array(dur)};
    logic [7:0] inf_values [0:EVENT_COUNT-1] = {sv_array(inf)};
    logic [7:0] rate_values [0:EVENT_COUNT-1] = {sv_array(rate)};
    logic [7:0] ctrl_values [0:EVENT_COUNT-1] = {sv_array(ctrl)};
    logic [7:0] filt_values [0:EVENT_COUNT-1] = {sv_array(filt)};
    int unsigned sample_counts [0:EVENT_COUNT-1] = {int_array(samples)};

    integer fd;
    integer event_index;
    integer sample_index;

    always #5 clk = ~clk;

    ssi263_formant_backend dut (
        .clk(clk),
        .rstn(rstn),
        .card_enabled(card_enabled),
        .warm_reset(warm_reset),
        .audio_tick(audio_tick),
        .start(start),
        .start_phoneme(start_phoneme),
        .start_sc01_phone(start_sc01_phone),
        .start_votrax(start_votrax),
        .current_function(current_function),
        .duration_phoneme(duration_phoneme),
        .inflection(inflection),
        .rate_inflection(rate_inflection),
        .ctrl_art_amp(ctrl_art_amp),
        .filter_freq(filter_freq),
        .phoneme_done(phoneme_done),
        .audio(audio)
    );

    task automatic emit_sample_tick;
        begin
            repeat (TICK_CYCLES - 1) begin
                audio_tick <= 1'b0;
                @(posedge clk);
            end
            audio_tick <= 1'b1;
            @(posedge clk);
            #1;
            $fwrite(fd, "%0d\\n", $signed(audio));
            audio_tick <= 1'b0;
        end
    endtask

    task automatic start_event(input logic [7:0] dur,
                               input logic [7:0] inf,
                               input logic [7:0] rate,
                               input logic [7:0] ctrl,
                               input logic [7:0] filt);
        begin
            @(posedge clk);
            duration_phoneme <= dur;
            current_function <= dur[7:6];
            inflection <= inf;
            rate_inflection <= rate;
            ctrl_art_amp <= ctrl;
            filter_freq <= filt;
            start_phoneme <= dur[5:0];
            start_sc01_phone <= {sc01_expr};
            start_votrax <= 1'b0;
            start <= 1'b1;
            @(posedge clk);
            start <= 1'b0;
        end
    endtask

    initial begin
        fd = $fopen("{sample_path}", "w");
        if (fd == 0) begin
            $fatal(1, "failed to open sample output");
        end

        repeat (20) @(posedge clk);
        rstn <= 1'b1;
        repeat (20) @(posedge clk);

        for (event_index = 0; event_index < EVENT_COUNT; event_index = event_index + 1) begin
            start_event(dur_values[event_index],
                        inf_values[event_index],
                        rate_values[event_index],
                        ctrl_values[event_index],
                        filt_values[event_index]);
            for (sample_index = 0; sample_index < sample_counts[event_index]; sample_index = sample_index + 1) begin
                emit_sample_tick();
            end
        end

        $fclose(fd);
        $finish;
    end
endmodule
""")


def run(cmd: list[str], cwd: Path, log: Path | None = None) -> None:
    completed = subprocess.run(cmd, cwd=cwd, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
    if log is not None:
        log.write_text(completed.stdout)
    if completed.returncode != 0:
        print(completed.stdout)
        raise SystemExit(f"{cmd[0]} failed with exit code {completed.returncode}")


def vivado_tool(name: str) -> str:
    bat = shutil.which(f"{name}.bat")
    if bat:
        return bat
    tool = shutil.which(name)
    if tool:
        return tool
    raise FileNotFoundError(f"unable to locate Vivado tool {name}")


def read_samples(path: Path) -> list[int]:
    samples: list[int] = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if line:
            samples.append(int(line))
    return samples


def write_wav(path: Path, samples: list[int]) -> None:
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        for sample in samples:
            sample = max(-32768, min(32767, sample))
            wav.writeframes(struct.pack("<h", sample))


def rms(samples: list[int]) -> float:
    if not samples:
        return 0.0
    return math.sqrt(sum(sample * sample for sample in samples) / len(samples))


def peak(samples: list[int]) -> int:
    return max((abs(sample) for sample in samples), default=0)


def autocorr_pitch(samples: list[int]) -> tuple[float, float]:
    if len(samples) < 512:
        return 0.0, 0.0
    values = [float(sample) for sample in samples]
    mean = sum(values) / len(values)
    values = [sample - mean for sample in values]
    energy = sum(sample * sample for sample in values)
    if energy <= 1.0:
        return 0.0, 0.0

    min_lag = int(SAMPLE_RATE / 500.0)
    max_lag = min(int(SAMPLE_RATE / 50.0), len(values) // 2)
    best_lag = 0
    best_corr = -1.0
    for lag in range(min_lag, max_lag + 1):
        corr = sum(values[i] * values[i + lag] for i in range(len(values) - lag))
        norm = math.sqrt(sum(values[i] * values[i] for i in range(len(values) - lag)) *
                         sum(values[i + lag] * values[i + lag] for i in range(len(values) - lag)))
        if norm > 0.0:
            corr /= norm
        if corr > best_corr:
            best_corr = corr
            best_lag = lag
    if best_lag == 0 or best_corr < 0.15:
        return 0.0, max(0.0, best_corr)
    return SAMPLE_RATE / best_lag, best_corr


def goertzel_power(samples: list[int], freq: float) -> float:
    if not samples:
        return 0.0
    omega = 2.0 * math.pi * freq / SAMPLE_RATE
    coeff = 2.0 * math.cos(omega)
    q0 = q1 = q2 = 0.0
    for sample in samples:
        q0 = coeff * q1 - q2 + sample
        q2 = q1
        q1 = q0
    return q1 * q1 + q2 * q2 - coeff * q1 * q2


def spectral_centroid(samples: list[int]) -> float:
    bands = [250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 12000.0]
    powers = [(freq, goertzel_power(samples, freq)) for freq in bands]
    total = sum(power for _, power in powers)
    if total <= 0.0:
        return 0.0
    return sum(freq * power for freq, power in powers) / total


def normalize(samples: list[int]) -> list[float]:
    p = float(peak(samples))
    if p <= 0.0:
        return [float(sample) for sample in samples]
    return [sample / p for sample in samples]


def correlation(a: list[int], b: list[int]) -> float:
    count = min(len(a), len(b))
    if count < 2:
        return 0.0
    af = normalize(a[:count])
    bf = normalize(b[:count])
    mean_a = sum(af) / count
    mean_b = sum(bf) / count
    num = 0.0
    den_a = 0.0
    den_b = 0.0
    for av, bv in zip(af, bf):
        da = av - mean_a
        db = bv - mean_b
        num += da * db
        den_a += da * da
        den_b += db * db
    if den_a <= 0.0 or den_b <= 0.0:
        return 0.0
    return num / math.sqrt(den_a * den_b)


def write_metadata(path: Path, events: list[PhoneEvent], phone_map: dict[int, int],
                   duration_phone_map: dict[int, int]) -> None:
    offset = 0
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=[
            "index", "label", "ssi_phone", "sc01_phone", "durphon", "inflect",
            "rateinf", "ctrlamp", "filfreq", "start_sample", "sample_count",
        ])
        writer.writeheader()
        for index, event in enumerate(events):
            phone = event.durphon & 0x3F
            writer.writerow({
                "index": index,
                "label": event.label,
                "ssi_phone": f"{phone:02X}",
                "sc01_phone": f"{audio_sc01_for_event(event, phone_map, duration_phone_map):02X}",
                "durphon": f"{event.durphon:02X}",
                "inflect": f"{event.inflect:02X}",
                "rateinf": f"{event.rateinf:02X}",
                "ctrlamp": f"{event.ctrlamp:02X}",
                "filfreq": f"{event.filfreq:02X}",
                "start_sample": offset,
                "sample_count": event.samples,
            })
            offset += event.samples


def write_metrics(path: Path, events: list[PhoneEvent], phone_map: dict[int, int],
                  duration_phone_map: dict[int, int], samples: list[int]) -> None:
    offset = 0
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=[
            "index", "label", "ssi_phone", "sc01_phone", "rms", "peak",
            "pitch_hz", "pitch_conf", "centroid_hz",
        ])
        writer.writeheader()
        for index, event in enumerate(events):
            segment = samples[offset:offset + event.samples]
            offset += event.samples
            trim = int(round(0.020 * SAMPLE_RATE))
            analysis = segment[trim:] if len(segment) > trim * 2 else segment
            pitch_hz, confidence = autocorr_pitch(analysis)
            phone = event.durphon & 0x3F
            writer.writerow({
                "index": index,
                "label": event.label,
                "ssi_phone": f"{phone:02X}",
                "sc01_phone": f"{audio_sc01_for_event(event, phone_map, duration_phone_map):02X}",
                "rms": f"{rms(segment):.2f}",
                "peak": peak(segment),
                "pitch_hz": f"{pitch_hz:.2f}",
                "pitch_conf": f"{confidence:.3f}",
                "centroid_hz": f"{spectral_centroid(analysis):.1f}",
            })


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preset", choices=["classic", "phones"], default="classic")
    parser.add_argument("--phones", default="05,08,0A,0C,0D,10,16,18,29,33",
                        help="SSI263 phone IDs for --preset phones.")
    parser.add_argument("--segment-ms", type=float, default=90.0,
                        help="Samples captured per event.")
    parser.add_argument("--inflect", type=lambda v: int(v, 0), default=0x52)
    parser.add_argument("--rateinf", type=lambda v: int(v, 0), default=0xB8)
    parser.add_argument("--ctrlamp", type=lambda v: int(v, 0), default=0x7B)
    parser.add_argument("--filfreq", type=lambda v: int(v, 0), default=0xE6)
    parser.add_argument("--tick-cycles", type=int, default=192,
                        help="Fabric clock cycles between 48 kHz sample ticks in "
                             "simulation. Must exceed the synth pipeline length "
                             "(~140 cycles with the F2N noise stage) or the next "
                             "audio_tick restarts the pipeline before it reaches "
                             "SYNTH_OUT and the output is silent. Real hardware has "
                             "~2771 cycles/tick (133 MHz / 48 kHz), so this is a "
                             "sim-only constraint.")
    parser.add_argument("--sc01-map", choices=["pkg", "identity"], default="pkg",
                        help="How the testbench maps SSI263 phones to the SC-01 backend input.")
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    events = build_events(args)
    if not events:
        raise SystemExit("no events to simulate")

    phone_map, duration_phone_map = parse_phone_map()
    if args.sc01_map == "identity":
        phone_map = {phone: phone for phone in range(64)}
        duration_phone_map = {}
    tb = args.out_dir / "ssi263_formant_audio_tb.sv"
    sample_txt = args.out_dir / "rtl_audio_i16.txt"
    write_testbench(tb, events, sample_txt, args.tick_cycles, args.sc01_map)
    write_metadata(args.out_dir / "segments.csv", events, phone_map, duration_phone_map)

    xvlog_log = args.out_dir / "xvlog.log"
    xelab_log = args.out_dir / "xelab.log"
    xsim_log = args.out_dir / "xsim.log"
    snapshot = "ssi263_formant_audio_tb_snapshot"
    run([vivado_tool("xvlog"), "-sv",
         str(ROOT / "hdl" / "apple" / "ssi263_formant_pkg.sv"),
         str(ROOT / "hdl" / "apple" / "sc01a_digital_core.sv"),
         str(ROOT / "hdl" / "apple" / "ssi263_formant_backend.sv"),
         str(tb)], ROOT, xvlog_log)
    run([vivado_tool("xelab"), "ssi263_formant_audio_tb", "-s", snapshot], ROOT, xelab_log)
    run([vivado_tool("xsim"), snapshot, "-runall"], ROOT, xsim_log)

    samples = read_samples(sample_txt)
    wav_path = args.out_dir / "rtl_formant_audio.wav"
    write_wav(wav_path, samples)
    write_metrics(args.out_dir / "metrics.csv", events, phone_map, duration_phone_map, samples)

    print(f"Wrote {wav_path}")
    print(f"Wrote {args.out_dir / 'segments.csv'}")
    print(f"Wrote {args.out_dir / 'metrics.csv'}")
    print(f"Captured {len(samples)} samples at {SAMPLE_RATE} Hz")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
