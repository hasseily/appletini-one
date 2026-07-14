#!/usr/bin/env python3
"""Render and compare the SSI-263 formant backend against a MAME-like SC-01A model.

This tool is only for audio validation: it feeds SC-01A phones through a
floating-point MAME-style reference model and a current-HDL-style fixed-point
model, then writes WAV/CSV artifacts for tuning.
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
FORMANT_PKG = ROOT / "hdl" / "apple" / "ssi263_formant_pkg.sv"
OUT_DIR = ROOT / "build" / "ssi263_formant_compare"
MB_AUDIT_CAPTURE_DIR = ROOT / "Assets" / "Sounds" / "Mockingboard mb-audit samples"
MB_AUDIT_SC01_SOURCE = ROOT.parent / "play-sc01-using-ssi263" / "chip-sc01.a"

MAME_SAMPLE_RATE = 40_000
HDL_SAMPLE_RATE = 48_000
HDL_CONTROL_RATE = 20_000
CAP_CLOCK = 20_000.0
Q15 = 1 << 15
DEFAULT_NOISE_SHAPER_INPUT_SHIFT = 6
VOTRAX_OUTPUT_SCALE = 4
SSI263_OUTPUT_SCALE = 10
FORMANT_SLEW_STEP = 3000
F2N_INPUT_GAIN_SHIFT = 3
FORMANT_IDLE_DECAY_SAMPLES = 512

GLOTTAL_FLOAT = [
    0.0,
    -4.0 / 7.0,
    7.0 / 7.0,
    6.0 / 7.0,
    5.0 / 7.0,
    4.0 / 7.0,
    3.0 / 7.0,
    2.0 / 7.0,
    1.0 / 7.0,
]

GLOTTAL_INT = [0, -4681, 8192, 7022, 5851, 4681, 3511, 2340, 1170]

BANDS_HZ = (250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0)

MB_AUDIT_TRANSCRIPTS = {
    "A": "the spy strikes back",
    "B": "ouch",
    "C": "crime wave one",
    "D": "game over",
    "E": "please consult the hint sheet",
    "F": "you can't go that way",
    "G": "you were a pitiful opponent",
}

MB_AUDIT_SC01_LABELS = {
    "A": "Spy_TheSpyStrikesBack",
    "B": "Spy_Ouch",
    "C": "CW_CrimeWave1",
    "D": "CW_GameOver",
    "E": "COM_Help",
    "F": "COM_CantGoThatWay",
    "G": "BZ_PitifulOpponent",
}

MB_AUDIT_SC01_FALLBACK_PHRASES = {
    # ../play-sc01-using-ssi263/chip-sc01.a
    "A": [0x03, 0x38, 0x32, 0x03, 0x1F, 0x25, 0x15, 0x22, 0x03,
          0x1F, 0x2A, 0x2B, 0x15, 0x22, 0x19, 0x1F, 0x03, 0x0E,
          0x2E, 0x19, 0x03, 0x3F],
    "B": [0x03, 0x13, 0x37, 0x2A, 0x10, 0x03, 0x3F],
    "C": [0x03, 0x19, 0x2B, 0x15, 0x22, 0x0C, 0x03, 0x2D,
          0x20, 0x0F, 0x03, 0x2D, 0x32, 0x0D, 0x3F],
    "D": [0x03, 0x1C, 0x20, 0x0C, 0x03, 0x35, 0x37, 0x0F,
          0x3A, 0x3F],
    "E": [0x3F, 0x25, 0x18, 0x2C, 0x12, 0x03, 0x19, 0x3D,
          0x0D, 0x1F, 0x33, 0x18, 0x2A, 0x3E, 0x38, 0x33,
          0x3E, 0x1B, 0x27, 0x0D, 0x2A, 0x3E, 0x11, 0x2C,
          0x2A, 0x3E, 0x3E, 0x3F],
    "F": [0x3F, 0x29, 0x28, 0x3E, 0x19, 0x2E, 0x0D, 0x2A,
          0x3E, 0x1C, 0x26, 0x3E, 0x38, 0x2E, 0x2A, 0x3E,
          0x2D, 0x01, 0x21, 0x3E, 0x3E, 0x3F],
    "G": [0x3F, 0x22, 0x28, 0x03, 0x03, 0x2D, 0x2D, 0x2B,
          0x03, 0x3F, 0x05, 0x3E, 0x25, 0x09, 0x2A, 0x0A,
          0x1D, 0x32, 0x18, 0x03, 0x3F, 0x03, 0x08, 0x25,
          0x26, 0x0D, 0x01, 0x0D, 0x2A, 0x03, 0x3F],
}


@dataclass(frozen=True)
class PhoneParams:
    phone: int
    word: int
    f1: int
    va: int
    f2: int
    fc: int
    f2q: int
    f3: int
    fa: int
    cld: int
    vd: int
    closure: int
    duration: int
    pause: bool


@dataclass(frozen=True)
class GeneratedData:
    phones: dict[int, PhoneParams]
    ssi_to_sc01: dict[int, int]
    ssi_audio_overrides: dict[int, int]
    dur_audio_overrides: dict[int, int]


def bit(word: int, index: int) -> int:
    return (word >> index) & 1


def bits(word: int, *indexes: int) -> int:
    value = 0
    for index in indexes:
        value = (value << 1) | bit(word, index)
    return value


def decode_phone_word(phone: int, word: int) -> PhoneParams:
    return PhoneParams(
        phone=phone,
        word=word,
        f1=bits(word, 0, 7, 14, 21),
        va=bits(word, 1, 8, 15, 22),
        f2=bits(word, 2, 9, 16, 23),
        fc=bits(word, 3, 10, 17, 24),
        f2q=bits(word, 4, 11, 18, 25),
        f3=bits(word, 5, 12, 19, 26),
        fa=bits(word, 6, 13, 20, 27),
        cld=bits(word, 34, 32, 30, 28),
        vd=bits(word, 35, 33, 31, 29),
        closure=bit(word, 36),
        duration=bits(~word, 37, 38, 39, 40, 41, 42, 43),
        pause=(phone == 0x03) or (phone == 0x3E),
    )


def parse_generated_data(formant_pkg: Path) -> GeneratedData:
    words_by_phone: dict[int, int] = {}
    ssi_to_sc01: dict[int, int] = {}
    ssi_audio_overrides: dict[int, int] = {}
    dur_audio_overrides: dict[int, int] = {}

    word_re = re.compile(r"6'h([0-9A-Fa-f]{2}): sc01a_word_by_phone = 64'h([0-9A-Fa-f]{16});")
    map_re = re.compile(r"6'h([0-9A-Fa-f]{2}): ssi263_to_sc01_phone = 6'h([0-9A-Fa-f]{2});")
    audio_override_re = re.compile(r"6'h([0-9A-Fa-f]{2}): ssi263_to_sc01_audio_phone = 6'h([0-9A-Fa-f]{2});")
    dur_override_re = re.compile(r"8'h([0-9A-Fa-f]{2}): ssi263_to_sc01_audio_phone = 6'h([0-9A-Fa-f]{2});")

    for line in formant_pkg.read_text().splitlines():
        if match := word_re.search(line):
            words_by_phone[int(match.group(1), 16)] = int(match.group(2), 16)
        if match := map_re.search(line):
            ssi_to_sc01[int(match.group(1), 16)] = int(match.group(2), 16)
        if match := audio_override_re.search(line):
            ssi_audio_overrides[int(match.group(1), 16)] = int(match.group(2), 16)
        if match := dur_override_re.search(line):
            dur_audio_overrides[int(match.group(1), 16)] = int(match.group(2), 16)

    missing = sorted(set(range(64)) - set(words_by_phone))
    if missing:
        raise SystemExit(f"{formant_pkg} is missing SC-01A phone rows: {missing}")

    return GeneratedData(
        phones={phone: decode_phone_word(phone, word) for phone, word in words_by_phone.items()},
        ssi_to_sc01=ssi_to_sc01,
        ssi_audio_overrides=ssi_audio_overrides,
        dur_audio_overrides=dur_audio_overrides,
    )


def bits_to_caps(value: int, caps_values: list[float]) -> float:
    total = 0.0
    for cap in caps_values:
        if value & 1:
            total += cap
        value >>= 1
    return total


def standard_filter_coeffs(sample_rate: float,
                           c1t: float,
                           c1b: float,
                           c2t: float,
                           c2b: float,
                           c3: float,
                           c4: float) -> tuple[list[float], list[float]]:
    k0 = c1t / (CAP_CLOCK * c1b) if c1t else 0.0
    k1 = c4 * c2t / (CAP_CLOCK * c1b * c3) if c2t else 0.0
    k2 = c4 * c2b / (CAP_CLOCK * CAP_CLOCK * c1b * c3)
    fpeak = math.sqrt(abs(k0 * k1 - k2)) / (2.0 * math.pi * k2)
    zc = 2.0 * math.pi * fpeak / math.tan(math.pi * fpeak / sample_rate)
    m0 = zc * k0
    m1 = zc * k1
    m2 = zc * zc * k2
    return (
        [1.0 + m0, 3.0 + m0, 3.0 - m0, 1.0 - m0],
        [1.0 + m1 + m2, 3.0 + m1 - m2, 3.0 - m1 - m2, 1.0 - m1 + m2],
    )


def lowpass_filter_coeffs(sample_rate: float, c1t: float, c1b: float) -> tuple[list[float], list[float]]:
    k = c1b / (CAP_CLOCK * c1t) * (150.0 / 4000.0)
    fpeak = 1.0 / (2.0 * math.pi * k)
    zc = 2.0 * math.pi * fpeak / math.tan(math.pi * fpeak / sample_rate)
    m = zc * k
    return [1.0], [1.0 + m, 1.0 - m]


def noise_shaper_filter_coeffs(sample_rate: float,
                               c1: float,
                               c2t: float,
                               c2b: float,
                               c3: float,
                               c4: float) -> tuple[list[float], list[float]]:
    k0 = c2t * c3 * c2b / c4
    k1 = c2t * (CAP_CLOCK * c2b)
    k2 = c1 * c2t * c3 / (CAP_CLOCK * c4)
    fpeak = math.sqrt(1.0 / k2) / (2.0 * math.pi)
    zc = 2.0 * math.pi * fpeak / math.tan(math.pi * fpeak / sample_rate)
    m0 = zc * k0
    m1 = zc * k1
    m2 = zc * zc * k2
    return [m0, 0.0, -m0], [1.0 + m1 + m2, 2.0 - 2.0 * m2, 1.0 - m1 + m2]


def normalize_float_coeffs(a: list[float], b: list[float]) -> list[float]:
    return [term / b[0] for term in a] + [-term / b[0] for term in b[1:]]


def quantize_q15(value: float) -> int:
    quantized = int(round(value * Q15))
    return max(-(1 << 15), min((1 << 15) - 1, quantized))


def quantized_coeffs(a: list[float], b: list[float]) -> list[int]:
    return [quantize_q15(value) for value in normalize_float_coeffs(a, b)]


class FloatFilter:
    def __init__(self, x_len: int, y_len: int) -> None:
        self.x = [0.0] * x_len
        self.y = [0.0] * y_len

    def apply(self, sample: float, a: list[float], b: list[float]) -> float:
        self.x = [sample] + self.x[:-1]
        total = 0.0
        for index, coeff in enumerate(a):
            total += self.x[index] * coeff
        for index in range(1, len(b)):
            total -= self.y[index - 1] * b[index]
        out = total / b[0]
        self.y = [out] + self.y[:-1]
        return out


class FixedFilter:
    def __init__(self, x_len: int, y_len: int) -> None:
        self.x = [0] * x_len
        self.y = [0] * y_len

    def apply(self, sample: int, coeffs: list[int]) -> int:
        taps = [sample] + self.x + self.y
        total = 0
        for value, coeff in zip(taps, coeffs):
            total += value * coeff
        out = sat24(total >> 15)
        self.x = [sample] + self.x[:-1]
        self.y = [out] + self.y[:-1]
        return out


class MameLikeVotrax:
    def __init__(self, phones: dict[int, PhoneParams], inflection: int = 0) -> None:
        self.phones = phones
        self.inflection = inflection & 0x03
        self.voice_1 = FloatFilter(4, 0)
        self.voice_2 = FloatFilter(4, 3)
        self.voice_3 = FloatFilter(4, 3)
        self.noise_1 = FloatFilter(3, 0)
        self.noise_2 = FloatFilter(3, 2)
        self.vn_1 = FloatFilter(4, 0)
        self.vn_2 = FloatFilter(4, 3)
        self.vn_3 = FloatFilter(4, 0)
        self.vn_4 = FloatFilter(4, 3)
        self.vn_5 = FloatFilter(2, 0)
        self.vn_6 = FloatFilter(2, 1)
        self.phone = 0x3F
        self.params = phones[self.phone]
        self.cur_fa = 0
        self.cur_fc = 0
        self.cur_va = 0
        self.cur_f1 = 0
        self.cur_f2 = 0
        self.cur_f2q = 0
        self.cur_f3 = 0
        self.filt_fa = 0
        self.filt_fc = 0
        self.filt_va = 0
        self.filt_f1 = 0
        self.filt_f2 = 0
        self.filt_f2q = 0
        self.filt_f3 = 0
        self.phonetick = 0
        self.ticks = 0
        self.pitch = 0
        self.closure = 0
        self.update_counter = 0
        self.cur_closure = True
        self.noise = 0
        self.cur_noise = False
        self.f1 = ([0.0] * 4, [1.0, 0.0, 0.0, 0.0])
        self.f2v = ([0.0] * 4, [1.0, 0.0, 0.0, 0.0])
        self.f3 = ([0.0] * 4, [1.0, 0.0, 0.0, 0.0])
        self.f4 = ([0.0] * 4, [1.0, 0.0, 0.0, 0.0])
        self.fn = ([0.0] * 3, [1.0, 0.0, 0.0])
        self.fx = ([0.0], [1.0, 0.0])
        self.filters_commit(force=True)

    def start_phone(self, phone: int) -> None:
        self.phone = phone & 0x3F
        self.params = self.phones[self.phone]
        self.phonetick = 0
        self.ticks = 0
        if self.params.cld == 0:
            self.cur_closure = bool(self.params.closure)

    @staticmethod
    def interpolate(reg_value: int, target: int) -> int:
        return (reg_value - (reg_value >> 3) + (target << 1)) & 0xFF

    def chip_update(self) -> None:
        if self.ticks != 0x10:
            self.phonetick += 1
            if self.phonetick == ((self.params.duration << 2) | 1):
                self.phonetick = 0
                self.ticks += 1
                if self.ticks == self.params.cld:
                    self.cur_closure = bool(self.params.closure)

        self.update_counter += 1
        if self.update_counter == 0x30:
            self.update_counter = 0

        tick_625 = (self.update_counter & 0xF) == 0
        tick_208 = self.update_counter == 0x28

        if tick_208 and (not self.params.pause or not (self.filt_fa or self.filt_va)):
            self.cur_fc = self.interpolate(self.cur_fc, self.params.fc)
            self.cur_f1 = self.interpolate(self.cur_f1, self.params.f1)
            self.cur_f2 = self.interpolate(self.cur_f2, self.params.f2)
            self.cur_f2q = self.interpolate(self.cur_f2q, self.params.f2q)
            self.cur_f3 = self.interpolate(self.cur_f3, self.params.f3)

        if tick_625:
            if self.ticks >= self.params.vd:
                self.cur_fa = self.interpolate(self.cur_fa, self.params.fa)
            if self.ticks >= self.params.cld:
                self.cur_va = self.interpolate(self.cur_va, self.params.va)

        if not self.cur_closure and (self.filt_fa or self.filt_va):
            self.closure = 0
        elif self.closure != (7 << 2):
            self.closure += 1

        self.pitch = (self.pitch + 1) & 0xFF
        pitch_limit = (0xE0 ^ (self.inflection << 5) ^ (self.filt_f1 << 1)) + 2
        if self.pitch == pitch_limit:
            self.pitch = 0

        if (self.pitch & 0xF9) == 0x08:
            self.filters_commit(force=False)

        noise_in = self.cur_noise and self.noise != 0x7FFF
        self.noise = ((self.noise << 1) & 0x7FFE) | int(noise_in)
        self.cur_noise = not (((self.noise >> 14) ^ (self.noise >> 13)) & 1)

    def filters_commit(self, force: bool) -> None:
        next_fa = self.cur_fa >> 4
        next_fc = self.cur_fc >> 4
        next_va = self.cur_va >> 4
        next_f1 = self.cur_f1 >> 4
        next_f2 = self.cur_f2 >> 3
        next_f2q = self.cur_f2q >> 4
        next_f3 = self.cur_f3 >> 4

        self.filt_fa = next_fa
        self.filt_fc = next_fc
        self.filt_va = next_va

        if force or self.filt_f1 != next_f1:
            self.filt_f1 = next_f1
            self.f1 = standard_filter_coeffs(
                MAME_SAMPLE_RATE, 11247, 11797, 949, 52067,
                2280 + bits_to_caps(self.filt_f1, [2546, 4973, 9861, 19724]),
                166272,
            )

        if force or self.filt_f2 != next_f2 or self.filt_f2q != next_f2q:
            self.filt_f2 = next_f2
            self.filt_f2q = next_f2q
            self.f2v = standard_filter_coeffs(
                MAME_SAMPLE_RATE, 24840, 29154,
                829 + bits_to_caps(self.filt_f2q, [1390, 2965, 5875, 11297]),
                38180,
                2352 + bits_to_caps(self.filt_f2, [833, 1663, 3164, 6327, 12654]),
                34270,
            )

        if force or self.filt_f3 != next_f3:
            self.filt_f3 = next_f3
            self.f3 = standard_filter_coeffs(
                MAME_SAMPLE_RATE, 0, 17594, 868, 18828,
                8480 + bits_to_caps(self.filt_f3, [2226, 4485, 9056, 18111]),
                50019,
            )

        if force:
            self.f4 = standard_filter_coeffs(MAME_SAMPLE_RATE, 0, 28810, 1165, 21457, 8558, 7289)
            self.fx = lowpass_filter_coeffs(MAME_SAMPLE_RATE, 1122, 23131)
            self.fn = noise_shaper_filter_coeffs(MAME_SAMPLE_RATE, 15500, 14854, 8450, 9523, 14083)

    def analog_calc(self) -> float:
        if self.pitch >= (9 << 3):
            v = 0.0
        else:
            v = GLOTTAL_FLOAT[self.pitch >> 3]

        v = v * self.filt_va / 15.0
        v = self.voice_1.apply(v, [1.0], [1.0])
        v = self.voice_2.apply(v, *self.f1)
        v = self.voice_3.apply(v, *self.f2v)

        n = 10000.0 * (1.0 if ((self.pitch & 0x40) and self.cur_noise) else -1.0)
        n = n * self.filt_fa / 15.0
        n = self.noise_1.apply(n, [1.0], [1.0])
        n = self.noise_2.apply(n, *self.fn)

        vn = v
        vn = self.vn_1.apply(vn, [1.0], [1.0])
        vn = self.vn_2.apply(vn, *self.f3)
        vn += n * (5.0 + (15 ^ self.filt_fc)) / 20.0
        vn = self.vn_3.apply(vn, [1.0], [1.0])
        vn = self.vn_4.apply(vn, *self.f4)
        vn *= (7 ^ (self.closure >> 2)) / 7.0
        vn = self.vn_5.apply(vn, [1.0], [1.0])
        vn = self.vn_6.apply(vn, *self.fx)
        return vn * 0.35

    def render(self, phone: int, samples: int) -> list[float]:
        self.start_phone(phone)
        out: list[float] = []
        for index in range(samples):
            if index & 1:
                self.chip_update()
            out.append(self.analog_calc())
        return out


def sat24(value: int) -> int:
    return max(-(1 << 23), min((1 << 23) - 1, value))


def sat16(value: int) -> int:
    return max(-(1 << 15), min((1 << 15) - 1, value))


def soft_limit16(value: int) -> int:
    if value > 8192:
        value = 8192 + ((value - 8192) >> 2)
    elif value < -8192:
        value = -8192 + ((value + 8192) >> 2)
    return sat16(value)


def slew_limit16(previous: int, target: int, step: int = FORMANT_SLEW_STEP) -> int:
    diff = target - previous
    if diff > step:
        return sat16(previous + step)
    if diff < -step:
        return sat16(previous - step)
    return sat16(target)


def formant_output_gain(sample: int) -> int:
    return sat24(sample << 1)


def scale4(sample: int, gain: int) -> int:
    gain &= 0xF
    if gain == 0:
        return 0
    if gain == 1:
        return sample >> 4
    if gain == 2:
        return sample >> 3
    if gain == 3:
        return (sample >> 3) + (sample >> 4)
    if gain == 4:
        return sample >> 2
    if gain == 5:
        return (sample >> 2) + (sample >> 4)
    if gain == 6:
        return (sample >> 2) + (sample >> 3)
    if gain == 7:
        return (sample >> 2) + (sample >> 3) + (sample >> 4)
    if gain == 8:
        return sample >> 1
    if gain == 9:
        return (sample >> 1) + (sample >> 4)
    if gain == 10:
        return (sample >> 1) + (sample >> 3)
    if gain == 11:
        return (sample >> 1) + (sample >> 3) + (sample >> 4)
    if gain == 12:
        return (sample >> 1) + (sample >> 2)
    if gain == 13:
        return (sample >> 1) + (sample >> 2) + (sample >> 4)
    if gain == 14:
        return (sample >> 1) + (sample >> 2) + (sample >> 3)
    return sample


def scale7(sample: int, gain: int) -> int:
    gain &= 0x7
    if gain == 0:
        return 0
    if gain == 1:
        return (sample >> 3) + (sample >> 6) + (sample >> 9)
    if gain == 2:
        return (sample >> 2) + (sample >> 5) + (sample >> 8)
    if gain == 3:
        return (sample >> 2) + (sample >> 3) + (sample >> 5) + (sample >> 6)
    if gain == 4:
        return (sample >> 1) + (sample >> 4)
    if gain == 5:
        return (sample >> 1) + (sample >> 3) + (sample >> 4) + (sample >> 6)
    if gain == 6:
        return sample - (sample >> 3) - (sample >> 6)
    return sample


def scale20(sample: int, gain: int) -> int:
    gain &= 0x1F
    if gain == 0:
        return 0
    if gain == 1:
        return sample >> 4
    if gain == 2:
        return sample >> 3
    if gain == 3:
        return (sample >> 3) + (sample >> 5)
    if gain == 4:
        return sample >> 2
    if gain == 5:
        return sample >> 2
    if gain == 6:
        return (sample >> 2) + (sample >> 4) - (sample >> 6)
    if gain == 7:
        return (sample >> 2) + (sample >> 3) - (sample >> 5)
    if gain == 8:
        return (sample >> 1) - (sample >> 3) + (sample >> 5)
    if gain == 9:
        return (sample >> 1) - (sample >> 4)
    if gain == 10:
        return sample >> 1
    if gain == 11:
        return (sample >> 1) + (sample >> 4)
    if gain == 12:
        return (sample >> 1) + (sample >> 3) - (sample >> 5)
    if gain == 13:
        return (sample >> 1) + (sample >> 3) + (sample >> 5)
    if gain == 14:
        return (sample >> 1) + (sample >> 2) - (sample >> 4)
    if gain == 15:
        return (sample >> 1) + (sample >> 2)
    if gain == 16:
        return sample - (sample >> 3) - (sample >> 4)
    if gain == 17:
        return sample - (sample >> 3) - (sample >> 5)
    if gain == 18:
        return sample - (sample >> 3) + (sample >> 5)
    if gain == 19:
        return sample - (sample >> 4)
    return sample


def presence_bypass(lowpassed: int, pre_lowpass: int) -> int:
    high = pre_lowpass - lowpassed
    return sat24(lowpassed + (high >> 1) + (high >> 2))


def fricative_bypass(lowpassed: int, pre_lowpass: int) -> int:
    high = pre_lowpass - lowpassed
    return sat24(pre_lowpass + (high >> 2))


class HdlLikeFormant:
    def __init__(self,
                 phones: dict[int, PhoneParams],
                 inflection: int = 0,
                 rate_inflection: int = 0,
                 articulation: int = 5,
                 amplitude: int = 15,
                 noise_shift: int = DEFAULT_NOISE_SHAPER_INPUT_SHIFT,
                 votrax: bool = False) -> None:
        self.phones = phones
        self.inflection = inflection & 0xFF
        self.rate_inflection = rate_inflection & 0xFF
        self.articulation = articulation & 0x7
        self.amplitude = amplitude & 0xF
        self.noise_shift = max(0, noise_shift)
        self.default_votrax = votrax
        self.f1 = {index: quantized_coeffs(*standard_filter_coeffs(
            HDL_SAMPLE_RATE, 11247, 11797, 949, 52067,
            2280 + bits_to_caps(index, [2546, 4973, 9861, 19724]), 166272))
            for index in range(16)}
        self.f2 = {(f2, f2q): quantized_coeffs(*standard_filter_coeffs(
            HDL_SAMPLE_RATE, 24840, 29154,
            829 + bits_to_caps(f2q, [1390, 2965, 5875, 11297]), 38180,
            2352 + bits_to_caps(f2, [833, 1663, 3164, 6327, 12654]), 34270))
            for f2 in range(32) for f2q in range(16)}
        self.f3 = {index: quantized_coeffs(*standard_filter_coeffs(
            HDL_SAMPLE_RATE, 0, 17594, 868, 18828,
            8480 + bits_to_caps(index, [2226, 4485, 9056, 18111]), 50019))
            for index in range(16)}
        self.f4 = quantized_coeffs(*standard_filter_coeffs(HDL_SAMPLE_RATE, 0, 28810, 1165, 21457, 8558, 7289))
        self.fn = quantized_coeffs(*noise_shaper_filter_coeffs(HDL_SAMPLE_RATE, 15500, 14854, 8450, 9523, 14083))
        self.fx = quantized_coeffs(*lowpass_filter_coeffs(HDL_SAMPLE_RATE, 1122, 23131))
        self.reset()

    def reset(self) -> None:
        self.phone = 0x3F
        self.params = self.phones[0x3F]
        self.chip_accum = 0
        self.phonetick = 0
        self.ticks = 0
        self.pitch = 0
        self.pitch_gate = False
        self.closure = 0
        self.update_counter = 0
        self.noise = 0
        self.duration_mod4 = 0
        self.rate_accum = 0
        self.is_votrax = self.default_votrax
        self.target_inflection = self.ssi263_inflection12()
        self.active_inflection = self.target_inflection
        self.duration_phoneme = 0
        self.current_function = 0
        self.cur_noise = False
        self.cur_closure = True
        self.cur_fa = 0
        self.cur_fc = 0
        self.cur_va = 0
        self.cur_f1 = 0
        self.cur_f2 = 0
        self.cur_f2q = 0
        self.cur_f3 = 0
        self.filt_fa = 0
        self.filt_fc = 0
        self.filt_va = 0
        self.filt_f1 = 0
        self.filt_f2 = 0
        self.filt_f2q = 0
        self.filt_f3 = 0
        self.idle_decay_count = 0
        self.clear_filter_history()
        self.audio = 0

    def clear_filter_history(self) -> None:
        self.ff1 = FixedFilter(3, 3)
        self.ff2 = FixedFilter(3, 3)
        self.ff2n = FixedFilter(3, 3)
        self.ff3 = FixedFilter(3, 3)
        self.ff4 = FixedFilter(3, 3)
        self.ffn = FixedFilter(2, 2)
        self.ffx = FixedFilter(0, 1)
        self.presence_low = 0

    def start_phone(self, phone: int, duration_phoneme: int | None = None,
                    current_function: int | None = None,
                    votrax: bool | None = None) -> None:
        self.phone = phone & 0x3F
        self.params = self.phones[phone & 0x3F]
        self.phonetick = 0
        self.ticks = 0
        self.duration_mod4 = 0
        self.rate_accum = 0
        self.idle_decay_count = 0
        self.is_votrax = self.default_votrax if votrax is None else votrax
        if self.is_votrax and duration_phoneme is None:
            self.duration_phoneme = 0
        else:
            self.duration_phoneme = (phone if duration_phoneme is None else duration_phoneme) & 0xFF
        if self.is_votrax and current_function is None:
            self.current_function = 0
        elif current_function is None:
            self.current_function = (self.duration_phoneme >> 6) & 0x3
        else:
            self.current_function = current_function & 0x3
        self.target_inflection = self.ssi263_inflection12()
        if self.transitioned_inflection_mode():
            self.active_inflection = (
                (self.target_inflection & 0x83F) |
                (self.active_inflection & 0x7C0)
            )
        else:
            self.active_inflection = self.target_inflection
        if self.params.cld == 0:
            self.cur_closure = bool(self.params.closure)

    def articulation_shift(self) -> int:
        if self.articulation <= 1:
            return 5
        if self.articulation <= 3:
            return 4
        if self.articulation <= 5:
            return 3
        if self.articulation == 6:
            return 2
        return 1

    def interpolate(self, reg_value: int, target: int) -> int:
        current = reg_value & 0xFF
        target_value = (target & 0xF) << 4
        shift = self.articulation_shift()
        if target_value >= current:
            delta = target_value - current
            step = delta >> shift
            if delta and not step:
                step = 1
            return min(0xFF, current + step)
        delta = current - target_value
        step = delta >> shift
        if delta and not step:
            step = 1
        return max(0, current - step)

    def duration_one_skip_mode(self) -> bool:
        return self.current_function != 1 and ((self.duration_phoneme >> 6) & 0x3) == 1

    def duration_speed_step(self) -> int:
        dur = 3 if self.current_function == 1 else ((self.duration_phoneme >> 6) & 0x3)
        if dur == 2:
            step = 2
        elif dur == 3:
            step = 4
        else:
            step = 1
        if self.duration_one_skip_mode() and self.duration_mod4 == 2:
            step = 2
        return step

    def rate_period_units(self) -> int:
        rate = (self.rate_inflection >> 4) & 0xF
        return 16 if rate == 0 else 16 - rate

    def rate_scaled_speed_step(self, base_step: int) -> int:
        numerator = self.rate_accum + base_step * 6
        period = self.rate_period_units()
        scaled = numerator // period
        self.rate_accum = numerator - scaled * period
        return scaled

    def control_speed_step(self) -> int:
        if self.is_votrax:
            return 1
        return self.rate_scaled_speed_step(self.duration_speed_step())

    def chip_update(self, speed_step: int) -> None:
        if self.ticks != 0x10:
            phonetick_next = self.phonetick + speed_step
            tick_limit = (self.params.duration << 2) | 1
            if phonetick_next >= tick_limit:
                self.phonetick = phonetick_next - tick_limit
                self.ticks = (self.ticks + 1) & 0x1F
                if self.ticks == self.params.cld:
                    self.cur_closure = bool(self.params.closure)
            else:
                self.phonetick = phonetick_next

        self.update_counter = 0 if self.update_counter == 47 else self.update_counter + 1
        tick_625 = (self.update_counter & 0xF) == 0
        tick_208 = self.update_counter == 0x28

        if tick_208 and (not self.params.pause or not (self.filt_fa or self.filt_va)):
            self.cur_fc = self.interpolate(self.cur_fc, self.params.fc)
            self.cur_f1 = self.interpolate(self.cur_f1, self.params.f1)
            self.cur_f2 = self.interpolate(self.cur_f2, self.params.f2)
            self.cur_f2q = self.interpolate(self.cur_f2q, self.params.f2q)
            self.cur_f3 = self.interpolate(self.cur_f3, self.params.f3)

        if tick_625:
            if self.ticks >= self.params.vd:
                self.cur_fa = self.interpolate(self.cur_fa, self.params.fa)
            if self.ticks >= self.params.cld:
                self.cur_va = self.interpolate(self.cur_va, self.params.va)

        if not self.cur_closure and (self.filt_fa or self.filt_va):
            self.closure = 0
        elif self.closure != (7 << 2):
            self.closure += 1

        if self.duration_one_skip_mode():
            self.duration_mod4 = (self.duration_mod4 + 1) & 0x3
        else:
            self.duration_mod4 = 0

    def advance_pitch_noise(self) -> None:
        pitch_limit = self.pitch_period_limit()
        pitch_next = self.pitch + 1
        self.pitch = pitch_next - pitch_limit if pitch_next >= pitch_limit else pitch_next
        self.pitch &= 0x3FF

        self.pitch_gate = bool(self.pitch & 0x40) if self.is_votrax else self.pitch >= (pitch_limit >> 1)

        if (self.pitch & 0x3F9) == 0x008:
            self.commit_filters()

        self.noise = self.sc01_noise_next(self.noise, self.cur_noise)
        self.cur_noise = bool(self.sc01_noise_out(self.noise))

    def ssi263_inflection12(self) -> int:
        return ((self.rate_inflection & 0x08) << 8) | ((self.inflection & 0xFF) << 3) | (self.rate_inflection & 0x07)

    def transitioned_inflection_mode(self) -> bool:
        return not self.is_votrax and self.current_function == 3

    @staticmethod
    def inflection_slope_step(slope: int) -> int:
        return (1, 2, 3, 4, 6, 8, 12, 16)[slope & 0x7]

    def advance_inflection(self) -> None:
        self.target_inflection = self.ssi263_inflection12()
        next_inflection = self.target_inflection & 0xFFF
        if self.transitioned_inflection_mode():
            active_target = (self.active_inflection >> 6) & 0x1F
            target_target = (self.target_inflection >> 6) & 0x1F
            step = self.inflection_slope_step((self.target_inflection >> 3) & 0x7)
            if active_target < target_target:
                active_target += min(step, target_target - active_target)
            elif active_target > target_target:
                active_target -= min(step, active_target - target_target)
            next_inflection = (next_inflection & ~0x7C0) | (active_target << 6)
        self.active_inflection = next_inflection & 0xFFF

    def pitch_period_limit(self) -> int:
        infl = self.ssi263_inflection12() if self.is_votrax else self.active_inflection
        if not self.is_votrax:
            span = 0x1000 - infl
            period = (span * 5) >> 5
            return max(1, period)
        base_limit = 0xE0 ^ (((infl >> 10) & 0x3) << 5)
        period = (base_limit ^ ((self.filt_f1 & 0xF) << 1)) + 2
        return min(0xFF, period)

    @staticmethod
    def sc01_noise_out(state: int) -> int:
        return int(not (((state >> 14) ^ (state >> 13)) & 1))

    @staticmethod
    def sc01_noise_next(state: int, cur_noise: bool) -> int:
        state &= 0x7FFF
        inp = bool(cur_noise) and state != 0x7FFF
        return ((state << 1) & 0x7FFE) | int(inp)

    def commit_filters(self) -> None:
        self.filt_fa = self.cur_fa >> 4
        self.filt_fc = self.cur_fc >> 4
        self.filt_va = self.cur_va >> 4
        self.filt_f1 = self.cur_f1 >> 4
        self.filt_f2 = self.cur_f2 >> 3
        self.filt_f2q = self.cur_f2q >> 4
        self.filt_f3 = self.cur_f3 >> 4

    def noise_stop_burst_gain(self) -> int:
        if self.ticks <= self.params.vd:
            return 0
        age = self.ticks - self.params.vd
        if age == 1:
            return 7
        if age == 2:
            return 7
        if age == 3:
            return 5
        if age == 4:
            return 3
        if age == 5:
            return 2
        return 0

    def current_closure_gain(self) -> int:
        if self.voiced_stop_phone():
            return self.voiced_stop_attack_gain()
        if self.filt_fa and not self.filt_va:
            return self.noise_stop_burst_gain() if self.params.closure else 7
        return 0x7 ^ (self.closure >> 2)

    def nonclosure_noise_phone(self) -> bool:
        return bool(self.filt_fa and not self.params.closure)

    def ch_fricative_phone(self) -> bool:
        return bool(self.phone == 0x10 and self.filt_fa and
                    not self.filt_va and not self.params.closure)

    def voiced_stop_phone(self) -> bool:
        return bool(self.params.closure and not self.params.fa and self.params.va)

    def voiced_stop_attack_gain(self) -> int:
        if self.ticks < self.params.cld:
            return 0
        age = self.ticks - self.params.cld
        if age == 0:
            return 7
        if age == 1:
            return 7
        if age == 2:
            return 6
        if age == 3:
            return 4
        if age == 4:
            return 2
        if age == 5:
            return 1
        return 0

    def consonant_attack_level(self) -> int:
        if self.ch_fricative_phone():
            return 0
        if not (self.params.fa or self.params.va):
            return 0
        if self.params.closure and self.params.cld <= self.params.vd:
            start_tick = self.params.cld
        elif self.params.fa:
            start_tick = self.params.vd
        else:
            start_tick = self.params.cld
        if self.ticks < start_tick:
            return 0
        age = self.ticks - start_tick
        if age == 0:
            return 3
        if age == 1:
            return 2
        if age == 2:
            return 1
        return 0

    def consonant_attack_boost(self, sample: int) -> int:
        if not (self.filt_fa or self.params.closure):
            return sample
        level = self.consonant_attack_level()
        if level == 3:
            return sat24(sample + (sample >> 1))
        if level == 2:
            return sat24(sample + (sample >> 2))
        if level == 1:
            return sat24(sample + (sample >> 3))
        return sample

    def noise_mix_gain(self) -> int:
        return 5 + (0xF ^ self.filt_fc)

    def f2n_input_gain(self, fn_out: int) -> int:
        return sat24(scale4(fn_out, self.filt_fc) << F2N_INPUT_GAIN_SHIFT)

    def synth_sample(self) -> int:
        silent_decay = self.ticks == 0x10
        if silent_decay:
            if self.idle_decay_count >= FORMANT_IDLE_DECAY_SAMPLES:
                self.clear_filter_history()
                self.audio = 0
                return self.audio
            self.idle_decay_count += 1
        else:
            self.idle_decay_count = 0

        if silent_decay or self.pitch >= 72:
            voice = 0
        else:
            voice = GLOTTAL_INT[self.pitch >> 3]
        if silent_decay:
            noise = 0
        else:
            noise = 8192 if (self.pitch_gate and self.cur_noise) else -8192

        voice_in = 0 if silent_decay else scale4(voice, self.filt_va)
        noise_in = 0 if silent_decay else sat24(scale4(noise, self.filt_fa) << self.noise_shift)
        f1 = self.ff1.apply(voice_in, self.f1[self.filt_f1])
        f2 = self.ff2.apply(f1, self.f2[(self.filt_f2, self.filt_f2q)])
        fn = self.ffn.apply(noise_in, self.fn)
        f2n = self.ff2n.apply(self.f2n_input_gain(fn), self.f2[(self.filt_f2, self.filt_f2q)])
        vn = sat24(f2 + f2n)
        f3 = self.ff3.apply(vn, self.f3[self.filt_f3])
        mixed = sat24(f3 + scale20(fn, self.noise_mix_gain()))
        f4 = self.ff4.apply(mixed, self.f4)
        closed = scale7(f4, self.current_closure_gain())
        fx = self.ffx.apply(closed, self.fx)
        if self.ch_fricative_phone():
            enhanced = presence_bypass(fx, closed)
        elif self.nonclosure_noise_phone():
            enhanced = fricative_bypass(fx, closed)
        elif self.filt_va and not self.filt_fa:
            enhanced = presence_bypass(fx, closed)
        else:
            enhanced = fx
        enhanced = self.consonant_attack_boost(enhanced)
        scaled = scale4(enhanced, self.amplitude)
        presence = self.presence_boost(scaled)
        output_scale = VOTRAX_OUTPUT_SCALE if self.is_votrax else SSI263_OUTPUT_SCALE
        target = soft_limit16(formant_output_gain(scale4(presence, output_scale)))
        self.audio = slew_limit16(self.audio, target)
        return self.audio

    def presence_boost(self, sample: int) -> int:
        delta = sample - self.presence_low
        boosted = sat24(sample + (delta >> 1))
        self.presence_low = sat24(self.presence_low + (delta >> 3))
        return boosted

    def render(self, phone: int, samples: int, duration_phoneme: int | None = None,
               current_function: int | None = None) -> list[float]:
        self.start_phone(phone, duration_phoneme, current_function)
        out: list[float] = []
        for _ in range(samples):
            self.chip_accum += HDL_CONTROL_RATE
            if self.chip_accum >= HDL_SAMPLE_RATE:
                self.chip_accum -= HDL_SAMPLE_RATE
                self.advance_inflection()
                speed_step = self.control_speed_step()
                if speed_step:
                    self.chip_update(speed_step)
                self.advance_pitch_noise()
            out.append(self.synth_sample() / 32768.0)
        return out


def render_phone_isolated(model_cls, phones: dict[int, PhoneParams], phone: int, samples: int, **kwargs) -> list[float]:
    model = model_cls(phones, **kwargs)
    return model.render(phone, samples)


def peak(samples: list[float]) -> float:
    return max((abs(sample) for sample in samples), default=0.0)


def rms(samples: list[float]) -> float:
    if not samples:
        return 0.0
    return math.sqrt(sum(sample * sample for sample in samples) / len(samples))


def zero_cross_rate(samples: list[float], sample_rate: int) -> float:
    if len(samples) < 2:
        return 0.0
    crossings = 0
    prev = samples[0]
    for sample in samples[1:]:
        if (prev < 0.0 <= sample) or (prev >= 0.0 > sample):
            crossings += 1
        prev = sample
    return crossings * sample_rate / max(1, len(samples) - 1)


def goertzel_power(samples: list[float], sample_rate: int, freq: float) -> float:
    if not samples:
        return 0.0
    omega = 2.0 * math.pi * freq / sample_rate
    coeff = 2.0 * math.cos(omega)
    q0 = 0.0
    q1 = 0.0
    q2 = 0.0
    for sample in samples:
        q0 = coeff * q1 - q2 + sample
        q2 = q1
        q1 = q0
    return q1 * q1 + q2 * q2 - coeff * q1 * q2


def spectral_centroid(samples: list[float], sample_rate: int) -> float:
    powers = [(freq, goertzel_power(samples, sample_rate, freq))
              for freq in BANDS_HZ if freq < sample_rate / 2.0]
    total = sum(power for _, power in powers)
    if total <= 0.0:
        return 0.0
    return sum(freq * power for freq, power in powers) / total


def normalize_for_metrics(samples: list[float]) -> list[float]:
    p = peak(samples)
    if p <= 0.0:
        return samples[:]
    return [sample / p for sample in samples]


def correlation(a: list[float], b: list[float]) -> float:
    count = min(len(a), len(b))
    if count < 2:
        return 0.0
    a = normalize_for_metrics(a[:count])
    b = normalize_for_metrics(b[:count])
    mean_a = sum(a) / count
    mean_b = sum(b) / count
    num = 0.0
    den_a = 0.0
    den_b = 0.0
    for av, bv in zip(a, b):
        da = av - mean_a
        db = bv - mean_b
        num += da * db
        den_a += da * da
        den_b += db * db
    if den_a <= 0.0 or den_b <= 0.0:
        return 0.0
    return num / math.sqrt(den_a * den_b)


def duration_ms_from_rom(params: PhoneParams) -> float:
    return 1000.0 * 16.0 * ((params.duration << 2) | 1) / 20_000.0


def ssi263_rom_index(phoneme: int) -> int:
    normalized = 2 if phoneme == 1 else phoneme
    return 0 if normalized == 0 else normalized - 2


def ssi263_audio_map(data: GeneratedData) -> dict[int, int]:
    result = dict(data.ssi_to_sc01)
    result.update(data.ssi_audio_overrides)
    return result


def format_hex_list(values: list[int]) -> str:
    return " ".join(f"{value:02X}" for value in values)


def ssi263_candidates_for_sc01(phone: int, mapping: dict[int, int]) -> list[int]:
    return sorted(ssi_phone for ssi_phone, sc01 in mapping.items() if sc01 == phone)


def silence_for_sc01(data: GeneratedData, phone: int, sample_rate: int) -> list[float]:
    params = data.phones.get(phone)
    duration_ms = 40.0 if params is None else duration_ms_from_rom(params)
    count = max(1, int(round(sample_rate * duration_ms / 1000.0)))
    return [0.0] * count


def phrase_refs_by_phone(phrase_map: dict[str, list[int]]) -> dict[int, list[str]]:
    refs: dict[int, list[str]] = {}
    for letter, phones in phrase_map.items():
        for index, phone in enumerate(phones):
            refs.setdefault(phone, []).append(f"{letter}:{index}")
    return refs


def write_phoneme_audit(path: Path,
                        data: GeneratedData,
                        phrase_map: dict[str, list[int]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    audio_map = ssi263_audio_map(data)
    phrase_refs = phrase_refs_by_phone(phrase_map)
    rows: list[dict[str, str]] = []

    for phone in sorted(data.phones):
        params = data.phones[phone]
        base_candidates = ssi263_candidates_for_sc01(phone, data.ssi_to_sc01)
        audio_candidates = ssi263_candidates_for_sc01(phone, audio_map)
        dur_candidates = sorted(durphon for durphon, sc01 in data.dur_audio_overrides.items() if sc01 == phone)
        rows.append({
            "sc01_phone": f"{phone:02X}",
            "raw_word": f"{params.word:016X}",
            "f1": str(params.f1),
            "va": str(params.va),
            "f2": str(params.f2),
            "fc": str(params.fc),
            "f2q": str(params.f2q),
            "f3": str(params.f3),
            "fa": str(params.fa),
            "cld": str(params.cld),
            "vd": str(params.vd),
            "closure": "1" if params.closure else "0",
            "duration": str(params.duration),
            "rom_duration_ms": f"{duration_ms_from_rom(params):.2f}",
            "pause": "1" if params.pause else "0",
            "ssi263_base_candidates": format_hex_list(base_candidates),
            "ssi263_audio_candidates": format_hex_list(audio_candidates),
            "ssi263_durphon_candidates": format_hex_list(dur_candidates),
            "mb_audit_phrase_refs": " ".join(phrase_refs.get(phone, [])),
        })

    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def write_individual_phone_wavs(out_dir: Path,
                                phone: int,
                                ref: list[float],
                                hdl: list[float]) -> None:
    base = out_dir / "phones" / f"phone_{phone:02X}"
    write_wav(base.with_name(base.name + "_mame_like.wav"), ref, MAME_SAMPLE_RATE)
    write_wav(base.with_name(base.name + "_hdl_like.wav"), hdl, HDL_SAMPLE_RATE)


def metrics_for(phone: int,
                params: PhoneParams,
                ref: list[float],
                hdl: list[float],
                data: GeneratedData) -> dict[str, str]:
    ref_n = normalize_for_metrics(ref)
    hdl_n = normalize_for_metrics(hdl)
    ref_rms = rms(ref)
    hdl_rms = rms(hdl)
    ratio_db = 0.0 if ref_rms <= 0.0 or hdl_rms <= 0.0 else 20.0 * math.log10(hdl_rms / ref_rms)
    return {
        "phone": f"{phone:02X}",
        "f1": str(params.f1),
        "f2": str(params.f2),
        "f2q": str(params.f2q),
        "f3": str(params.f3),
        "va": str(params.va),
        "fa": str(params.fa),
        "fc": str(params.fc),
        "cld": str(params.cld),
        "vd": str(params.vd),
        "duration": str(params.duration),
        "rom_duration_ms": f"{duration_ms_from_rom(params):.2f}",
        "ref_peak": f"{peak(ref):.6g}",
        "hdl_peak": f"{peak(hdl):.6g}",
        "ref_rms": f"{ref_rms:.6g}",
        "hdl_rms": f"{hdl_rms:.6g}",
        "hdl_vs_ref_rms_db": f"{ratio_db:.2f}",
        "ref_zcr_hz": f"{zero_cross_rate(ref_n, MAME_SAMPLE_RATE):.1f}",
        "hdl_zcr_hz": f"{zero_cross_rate(hdl_n, HDL_SAMPLE_RATE):.1f}",
        "ref_centroid_hz": f"{spectral_centroid(ref_n, MAME_SAMPLE_RATE):.1f}",
        "hdl_centroid_hz": f"{spectral_centroid(hdl_n, HDL_SAMPLE_RATE):.1f}",
        "corr": f"{correlation(ref_n, hdl_n):.4f}",
        "pause": "1" if params.pause else "0",
    }


def write_wav(path: Path, samples: list[float], sample_rate: int,
              normalize: bool = True) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    p = peak(samples)
    scale = 0.90 / p if normalize and p > 0.0 else 1.0
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        for sample in samples:
            value = int(max(-1.0, min(1.0, sample * scale)) * 32767.0)
            wav.writeframes(struct.pack("<h", value))


def load_mb_audit_sc01_phrases(source: Path) -> tuple[dict[str, list[int]], str]:
    labels_to_letters = {label: letter for letter, label in MB_AUDIT_SC01_LABELS.items()}
    phrases: dict[str, list[int]] = {}

    if source.is_file():
        active: str | None = None
        for raw in source.read_text().splitlines():
            text = raw.split(";", 1)[0].strip()
            if not text:
                continue

            label_match = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*:?\s*$", text)
            if label_match:
                active = labels_to_letters.get(label_match.group(1))
                if active is not None:
                    phrases[active] = []
                continue

            if active is None or "!byte" not in text:
                continue

            body = text.split("!byte", 1)[1]
            for byte_match in re.finditer(r"\$([0-9A-Fa-f]{1,2})|\b([0-9]+)\b", body):
                value = int(byte_match.group(1), 16) if byte_match.group(1) else int(byte_match.group(2), 10)
                if value == 0:
                    active = None
                    break
                phrases[active].append(value & 0xFF)

        if all(letter in phrases and phrases[letter] for letter in MB_AUDIT_SC01_LABELS):
            return phrases, str(source)

    return dict(MB_AUDIT_SC01_FALLBACK_PHRASES), "built-in fallback"


def read_wav(path: Path) -> tuple[list[float], int]:
    with wave.open(str(path), "rb") as wav:
        channels = wav.getnchannels()
        width = wav.getsampwidth()
        rate = wav.getframerate()
        frames = wav.readframes(wav.getnframes())

    if width != 2:
        raise SystemExit(f"{path} must be decoded to 16-bit PCM WAV, got sample width {width}")
    values = struct.unpack("<" + "h" * (len(frames) // 2), frames)
    if channels == 1:
        return [value / 32768.0 for value in values], rate

    mono: list[float] = []
    for index in range(0, len(values), channels):
        mono.append(sum(values[index:index + channels]) / (32768.0 * channels))
    return mono, rate


def find_ffmpeg(explicit: Path | None) -> Path:
    candidates: list[Path] = []
    if explicit is not None:
        candidates.append(explicit)
    if found := shutil.which("ffmpeg"):
        candidates.append(Path(found))
    candidates.extend([
        Path(r"C:\Program Files\ffmpeg\bin\ffmpeg.exe"),
        Path(r"C:\Program Files (x86)\ffmpeg\bin\ffmpeg.exe"),
    ])
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise SystemExit("ffmpeg.exe was not found; pass --ffmpeg or install it in C:\\Program Files\\ffmpeg\\bin")


def decode_audio(ffmpeg: Path, source: Path, dest: Path, sample_rate: int = HDL_SAMPLE_RATE) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([
        str(ffmpeg),
        "-y",
        "-hide_banner",
        "-loglevel", "error",
        "-i", str(source),
        "-ac", "1",
        "-ar", str(sample_rate),
        "-sample_fmt", "s16",
        str(dest),
    ], check=True)


def trim_silence(samples: list[float], sample_rate: int,
                 threshold_ratio: float = 0.025,
                 pad_ms: float = 25.0) -> list[float]:
    p = peak(samples)
    if p <= 0.0:
        return []
    threshold = max(0.0005, p * threshold_ratio)
    first = 0
    while first < len(samples) and abs(samples[first]) < threshold:
        first += 1
    if first == len(samples):
        return []
    last = len(samples) - 1
    while last > first and abs(samples[last]) < threshold:
        last -= 1
    pad = int(round(sample_rate * pad_ms / 1000.0))
    first = max(0, first - pad)
    last = min(len(samples) - 1, last + pad)
    return samples[first:last + 1]


def normalize_peak(samples: list[float], target: float = 0.90) -> list[float]:
    p = peak(samples)
    if p <= 0.0:
        return samples[:]
    scale = target / p
    return [sample * scale for sample in samples]


def resample_linear(samples: list[float], src_rate: int, dst_rate: int) -> list[float]:
    if src_rate == dst_rate or not samples:
        return samples[:]
    out_len = max(1, int(round(len(samples) * dst_rate / src_rate)))
    if out_len == 1:
        return [samples[0]]
    scale = (len(samples) - 1) / (out_len - 1)
    out: list[float] = []
    for index in range(out_len):
        pos = index * scale
        base = int(pos)
        frac = pos - base
        if base + 1 < len(samples):
            out.append(samples[base] * (1.0 - frac) + samples[base + 1] * frac)
        else:
            out.append(samples[base])
    return out


def estimate_pitch_hz(samples: list[float], sample_rate: int) -> float:
    active = trim_silence(samples, sample_rate, threshold_ratio=0.04, pad_ms=0.0)
    if len(active) < sample_rate // 20:
        return 0.0
    limit = min(len(active), sample_rate)
    start = max(0, (len(active) - limit) // 2)
    window = active[start:start + limit]
    mean = sum(window) / len(window)
    centered = [sample - mean for sample in window]
    stride = max(1, len(centered) // 24000)
    centered = centered[::stride]
    effective_rate = sample_rate / stride
    min_lag = max(1, int(effective_rate / 400.0))
    max_lag = min(len(centered) // 2, int(effective_rate / 60.0))
    best_lag = 0
    best_score = 0.0
    for lag in range(min_lag, max_lag + 1):
        score = 0.0
        energy_a = 0.0
        energy_b = 0.0
        for index in range(0, len(centered) - lag):
            a = centered[index]
            b = centered[index + lag]
            score += a * b
            energy_a += a * a
            energy_b += b * b
        if energy_a > 0.0 and energy_b > 0.0:
            score /= math.sqrt(energy_a * energy_b)
        if score > best_score:
            best_score = score
            best_lag = lag
    if best_lag == 0 or best_score < 0.18:
        return 0.0
    return effective_rate / best_lag


def render_hdl_sc01_phrase(phone_ids: list[int],
                           phones: dict[int, PhoneParams],
                           inflection: int,
                           rate_inflection: int,
                           amplitude: int,
                           noise_shift: int,
                           inter_phone_gap_ms: float = 0.0) -> list[float]:
    model = HdlLikeFormant(phones,
                           inflection=inflection,
                           rate_inflection=rate_inflection,
                           amplitude=amplitude,
                           noise_shift=noise_shift,
                           votrax=True)
    out: list[float] = []
    gap_samples = max(0, int(round(HDL_SAMPLE_RATE * inter_phone_gap_ms / 1000.0)))
    for phone in phone_ids:
        model.start_phone(phone)
        guard = 0
        while model.ticks != 0x10 and guard < HDL_SAMPLE_RATE * 3:
            model.chip_accum += HDL_CONTROL_RATE
            if model.chip_accum >= HDL_SAMPLE_RATE:
                model.chip_accum -= HDL_SAMPLE_RATE
                model.advance_inflection()
                speed_step = model.control_speed_step()
                if speed_step:
                    model.chip_update(speed_step)
                model.advance_pitch_noise()
            out.append(model.synth_sample() / 32768.0)
            guard += 1
        for _ in range(gap_samples):
            model.chip_accum += HDL_CONTROL_RATE
            if model.chip_accum >= HDL_SAMPLE_RATE:
                model.chip_accum -= HDL_SAMPLE_RATE
                model.advance_inflection()
                speed_step = model.control_speed_step()
                if speed_step:
                    model.chip_update(speed_step)
                model.advance_pitch_noise()
            out.append(model.synth_sample() / 32768.0)
    return out


def render_mame_sc01_phrase(phone_ids: list[int],
                            phones: dict[int, PhoneParams],
                            inflection: int,
                            inter_phone_gap_ms: float = 0.0) -> list[float]:
    model = MameLikeVotrax(phones, inflection=inflection)
    out: list[float] = []
    gap = [0.0] * max(0, int(round(MAME_SAMPLE_RATE * inter_phone_gap_ms / 1000.0)))
    for phone in phone_ids:
        model.start_phone(phone)
        guard = 0
        while model.ticks != 0x10 and guard < MAME_SAMPLE_RATE * 3:
            if guard & 1:
                model.chip_update()
            out.append(model.analog_calc())
            guard += 1
        out.extend(gap)
    return normalize_peak(out)


def capture_metrics(letter: str, samples: list[float], sample_rate: int) -> dict[str, str]:
    active = trim_silence(samples, sample_rate)
    active_n = normalize_for_metrics(active)
    return {
        "sample": letter,
        "transcript": MB_AUDIT_TRANSCRIPTS.get(letter, ""),
        "duration_ms": f"{1000.0 * len(samples) / sample_rate:.2f}",
        "active_ms": f"{1000.0 * len(active) / sample_rate:.2f}",
        "peak": f"{peak(samples):.6g}",
        "active_rms": f"{rms(active):.6g}",
        "active_zcr_hz": f"{zero_cross_rate(active_n, sample_rate):.1f}",
        "active_centroid_hz": f"{spectral_centroid(active_n, sample_rate):.1f}",
        "pitch_hz": f"{estimate_pitch_hz(active, sample_rate):.1f}",
    }


def compare_capture_to_generated(letter: str,
                                 model: str,
                                 capture: list[float],
                                 generated: list[float],
                                 generated_rate: int,
                                 capture_rate: int = HDL_SAMPLE_RATE) -> dict[str, str]:
    cap_active = trim_silence(capture, capture_rate)
    gen_active = trim_silence(resample_linear(generated, generated_rate, capture_rate),
                              capture_rate)
    cap_n = normalize_for_metrics(cap_active)
    gen_n = normalize_for_metrics(gen_active)
    cap_rms = rms(cap_active)
    gen_rms = rms(gen_active)
    rms_db = 0.0 if cap_rms <= 0.0 or gen_rms <= 0.0 else 20.0 * math.log10(gen_rms / cap_rms)
    return {
        "sample": letter,
        "model": model,
        "transcript": MB_AUDIT_TRANSCRIPTS.get(letter, ""),
        "capture_active_ms": f"{1000.0 * len(cap_active) / capture_rate:.2f}",
        "generated_active_ms": f"{1000.0 * len(gen_active) / capture_rate:.2f}",
        "duration_delta_ms": f"{1000.0 * (len(gen_active) - len(cap_active)) / capture_rate:.2f}",
        "capture_rms": f"{cap_rms:.6g}",
        "generated_rms": f"{gen_rms:.6g}",
        "generated_vs_capture_rms_db": f"{rms_db:.2f}",
        "capture_centroid_hz": f"{spectral_centroid(cap_n, capture_rate):.1f}",
        "generated_centroid_hz": f"{spectral_centroid(gen_n, capture_rate):.1f}",
        "capture_pitch_hz": f"{estimate_pitch_hz(cap_active, capture_rate):.1f}",
        "generated_pitch_hz": f"{estimate_pitch_hz(gen_active, capture_rate):.1f}",
        "corr": f"{correlation(cap_n, gen_n):.4f}",
    }


def run_mb_audit_capture_compare(args: argparse.Namespace,
                                 data: GeneratedData) -> int:
    ffmpeg = find_ffmpeg(args.ffmpeg)
    phrase_map, phrase_source = load_mb_audit_sc01_phrases(args.phrase_source)
    capture_dir = args.capture_dir
    if not capture_dir.is_dir():
        raise SystemExit(f"capture directory not found: {capture_dir}")

    decoded_dir = args.out_dir / "mb_audit_captures" / "decoded"
    generated_dir = args.out_dir / "mb_audit_captures" / "generated"
    decoded_dir.mkdir(parents=True, exist_ok=True)
    generated_dir.mkdir(parents=True, exist_ok=True)

    captures: dict[str, list[float]] = {}
    capture_rows: list[dict[str, str]] = []
    for source in sorted(capture_dir.glob("Mockingboard Sample *.m4a")):
        match = re.search(r"Sample ([A-G])", source.stem, re.IGNORECASE)
        if not match:
            continue
        letter = match.group(1).upper()
        wav_path = decoded_dir / f"mb_audit_sample_{letter}.wav"
        decode_audio(ffmpeg, source, wav_path, HDL_SAMPLE_RATE)
        samples, rate = read_wav(wav_path)
        if rate != HDL_SAMPLE_RATE:
            samples = resample_linear(samples, rate, HDL_SAMPLE_RATE)
            rate = HDL_SAMPLE_RATE
        captures[letter] = samples
        capture_rows.append(capture_metrics(letter, samples, rate))
        write_wav(decoded_dir / f"mb_audit_sample_{letter}_active_norm.wav",
                  trim_silence(samples, rate), rate)

    if not captures:
        raise SystemExit(f"no Mockingboard Sample A-G captures found in {capture_dir}")

    with (args.out_dir / "mb_audit_capture_metrics.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(capture_rows[0].keys()))
        writer.writeheader()
        writer.writerows(capture_rows)

    compare_rows: list[dict[str, str]] = []
    for letter, phone_ids in phrase_map.items():
        if letter not in captures:
            continue
        hdl = render_hdl_sc01_phrase(phone_ids, data.phones,
                                     inflection=args.inflection,
                                     rate_inflection=args.rate_inflection,
                                     amplitude=args.amplitude,
                                     noise_shift=args.noise_shift,
                                     inter_phone_gap_ms=args.phrase_gap_ms)
        mame = render_mame_sc01_phrase(phone_ids, data.phones,
                                       inflection=args.sc01_inflection,
                                       inter_phone_gap_ms=args.phrase_gap_ms)
        write_wav(generated_dir / f"mb_audit_sample_{letter}_hdl_like.wav", hdl, HDL_SAMPLE_RATE)
        write_wav(generated_dir / f"mb_audit_sample_{letter}_mame_like.wav", mame, MAME_SAMPLE_RATE)
        compare_rows.append(compare_capture_to_generated(letter, "hdl_like", captures[letter],
                                                         hdl, HDL_SAMPLE_RATE))
        compare_rows.append(compare_capture_to_generated(letter, "mame_like", captures[letter],
                                                         mame, MAME_SAMPLE_RATE))

    compare_path = args.out_dir / "mb_audit_capture_compare.csv"
    if compare_rows:
        with compare_path.open("w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(compare_rows[0].keys()))
            writer.writeheader()
            writer.writerows(compare_rows)

    print(f"Decoded {len(captures)} mb-audit captures with {ffmpeg}")
    print(f"Loaded {len(phrase_map)} SC-01 phrase tables from {phrase_source}")
    print(f"Wrote {args.out_dir / 'mb_audit_capture_metrics.csv'}")
    if compare_rows:
        print(f"Wrote {compare_path}")
        print(f"Wrote generated phrase WAVs under {generated_dir}")
        print()
        for row in compare_rows:
            print(f"Sample {row['sample']} {row['model']}: "
                  f"corr={row['corr']} duration_delta={row['duration_delta_ms']}ms "
                  f"centroid {row['capture_centroid_hz']}->{row['generated_centroid_hz']}Hz "
                  f"pitch {row['capture_pitch_hz']}->{row['generated_pitch_hz']}Hz")
    else:
        print("No generated phrase comparisons were available.")

    missing = sorted(set(captures) - set(phrase_map))
    if missing:
        print()
        print("Captured phrases without known local mb-audit event tables: " +
              ", ".join(missing))
    return 0


def parse_phone_list(value: str) -> list[int]:
    if value.lower() == "all":
        return list(range(64))
    phones = []
    for item in value.replace(" ", "").split(","):
        if not item:
            continue
        phones.append(int(item, 16))
    return phones


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--phones", default="00,01,02,03,04,05,07,08,0D,10,16,20,28,2F,33,38,3E,3F",
                        help="Comma-separated SC-01A phone IDs in hex, or 'all'.")
    parser.add_argument("--segment-ms", type=float, default=260.0,
                        help="Isolated render length per phone.")
    parser.add_argument("--gap-ms", type=float, default=35.0,
                        help="Silence gap between WAV corpus phones.")
    parser.add_argument("--sc01-inflection", type=int, default=0, choices=range(4),
                        help="Two-bit SC-01A inflection value for the MAME-like reference.")
    parser.add_argument("--inflection", type=lambda v: int(v, 0), default=0x20,
                        help="SSI-263 INFLECT register value for the HDL-like path.")
    parser.add_argument("--rate-inflection", type=lambda v: int(v, 0), default=0xA8,
                        help="SSI-263 RATE/INF register value for the HDL-like path.")
    parser.add_argument("--amplitude", type=int, default=12, choices=range(16),
                        help="HDL-like SSI-263 amplitude nibble.")
    parser.add_argument("--noise-shift", type=int, default=DEFAULT_NOISE_SHAPER_INPUT_SHIFT,
                        help="Left shift applied to the fixed-point HDL noise-shaper input.")
    parser.add_argument("--out-dir", type=Path, default=OUT_DIR)
    parser.add_argument("--audit", action="store_true",
                        help="Write a 64-row SC-01 phoneme audit CSV from all available local data.")
    parser.add_argument("--individual-wavs", action="store_true",
                        help="Write per-phone MAME-like, HDL-like, and AppleWin-sample WAVs.")
    parser.add_argument("--mb-audit-captures", action="store_true",
                        help="Decode and compare real Mockingboard mb-audit A-G speech captures.")
    parser.add_argument("--capture-dir", type=Path, default=MB_AUDIT_CAPTURE_DIR,
                        help="Directory containing Mockingboard Sample A-G .m4a captures.")
    parser.add_argument("--phrase-source", type=Path, default=MB_AUDIT_SC01_SOURCE,
                        help="SC-01 phrase table source from play-sc01-using-ssi263.")
    parser.add_argument("--ffmpeg", type=Path, default=None,
                        help="Path to ffmpeg.exe for decoding .m4a captures.")
    parser.add_argument("--phrase-gap-ms", type=float, default=0.0,
                        help="Optional synthetic gap inserted between rendered phrase phonemes.")
    args = parser.parse_args()

    data = parse_generated_data(FORMANT_PKG)
    phrase_map, phrase_source = load_mb_audit_sc01_phrases(args.phrase_source)

    if args.audit:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        audit_path = args.out_dir / "phoneme_audit.csv"
        write_phoneme_audit(audit_path, data, phrase_map)
        print(f"Wrote {audit_path}")
        print(f"Loaded {len(phrase_map)} SC-01 phrase tables from {phrase_source}")

    if args.mb_audit_captures:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        return run_mb_audit_capture_compare(args, data)

    phone_list = parse_phone_list(args.phones)
    for phone in phone_list:
        if phone not in data.phones:
            raise SystemExit(f"Unknown SC-01A phone {phone:02X}")

    args.out_dir.mkdir(parents=True, exist_ok=True)

    ref_segment_samples = max(1, int(round(MAME_SAMPLE_RATE * args.segment_ms / 1000.0)))
    hdl_segment_samples = max(1, int(round(HDL_SAMPLE_RATE * args.segment_ms / 1000.0)))
    ref_gap = [0.0] * max(0, int(round(MAME_SAMPLE_RATE * args.gap_ms / 1000.0)))
    hdl_gap = [0.0] * max(0, int(round(HDL_SAMPLE_RATE * args.gap_ms / 1000.0)))

    ref_corpus: list[float] = []
    hdl_corpus: list[float] = []
    rows: list[dict[str, str]] = []

    for phone in phone_list:
        ref = render_phone_isolated(MameLikeVotrax, data.phones, phone, ref_segment_samples, inflection=args.sc01_inflection)
        hdl = render_phone_isolated(HdlLikeFormant, data.phones, phone, hdl_segment_samples,
                                    inflection=args.inflection, rate_inflection=args.rate_inflection,
                                    amplitude=args.amplitude,
                                    noise_shift=args.noise_shift,
                                    votrax=True)
        rows.append(metrics_for(phone, data.phones[phone], ref, hdl, data))
        ref_corpus.extend(ref)
        ref_corpus.extend(ref_gap)
        hdl_corpus.extend(hdl)
        hdl_corpus.extend(hdl_gap)
        if args.individual_wavs:
            write_individual_phone_wavs(args.out_dir, phone, ref, hdl)

    csv_path = args.out_dir / "metrics.csv"
    with csv_path.open("w", newline="") as f:
        fieldnames = list(rows[0].keys()) if rows else []
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

    write_wav(args.out_dir / "mame_like_reference.wav", ref_corpus, MAME_SAMPLE_RATE)
    write_wav(args.out_dir / "hdl_like_current.wav", hdl_corpus, HDL_SAMPLE_RATE)

    sorted_by_corr = sorted(rows, key=lambda row: float(row["corr"]))
    sorted_by_centroid = sorted(rows, key=lambda row: abs(float(row["hdl_centroid_hz"]) - float(row["ref_centroid_hz"])), reverse=True)

    print(f"Wrote {csv_path}")
    print(f"Wrote {args.out_dir / 'mame_like_reference.wav'}")
    print(f"Wrote {args.out_dir / 'hdl_like_current.wav'}")
    if args.individual_wavs:
        print(f"Wrote per-phone WAVs under {args.out_dir / 'phones'}")
    print()
    print("Lowest waveform correlations:")
    for row in sorted_by_corr[:6]:
        print(f"  phone ${row['phone']}: corr={row['corr']} "
              f"ref_centroid={row['ref_centroid_hz']}Hz hdl_centroid={row['hdl_centroid_hz']}Hz "
              f"rms_db={row['hdl_vs_ref_rms_db']}")
    print()
    print("Largest band-centroid deltas:")
    for row in sorted_by_centroid[:6]:
        delta = float(row["hdl_centroid_hz"]) - float(row["ref_centroid_hz"])
        print(f"  phone ${row['phone']}: delta={delta:+.1f}Hz "
              f"ref={row['ref_centroid_hz']}Hz hdl={row['hdl_centroid_hz']}Hz corr={row['corr']}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
