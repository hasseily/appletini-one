#!/usr/bin/env python3
"""Read existing SSI tables and register traces for the host listening model.

This module parses checked-in constants; it does not run a ROM generator or
recalculate coefficients. Its text export is an ordered stream of decimal
integers for the C++ renderer. Timing uses effective XCK ticks throughout.
"""
from __future__ import annotations

from dataclasses import dataclass
from functools import lru_cache
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
FORMANT_PKG = ROOT / "scripts/fixtures/ssi263_host/ssi263_formant_pkg.sv"
NATIVE_ROM = ROOT / "hdl/apple/ssi263_sc02_rom.mem"
ACTIVE_ROM_SHA256 = "101d129a5f104e6190f2eca518bbf9ef65bf4ff92684d29eba56d9641aa02b0a"
FULL_ROM_SHA256 = "9c3bba73319e1ed3652c85dac19874df04cbb72e62fdd63d6cbd7b34ff81f941"
PHONE_FIELDS = ("f1", "va", "f2", "fc", "f2q", "f3", "fa", "cld", "vd",
                "closure", "duration", "pause")
MAPPING_FIELDS = ("f1", "f2", "f2q", "f3", "va", "fa")
TABLE_ORDER = ("phones", "native_rom", "mappings", "sc01_map",
               "f1", "f2", "f3", "f4", "fn", "fx")
PAL_EFFECTIVE_XCK_HZ = 1_015_625
DEMO_NAMES = ("hello", "hello_four", "transitions", "phone_survey")


def _without_comments(source: str) -> str:
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)


@lru_cache(maxsize=64)
def _function(source: str, name: str) -> str:
    pattern = (r"\bfunction\s+automatic\b[^;]*\b" + re.escape(name)
               + r"\s*\([^;]*;(.*?)\bendfunction\b")
    matches = re.findall(pattern, source, flags=re.S)
    if len(matches) != 1:
        raise ValueError(f"expected one constant function {name}")
    return matches[0]


_LITERAL = r"-?\d+'[sS]?[hHdDbB][0-9a-fA-F_]+"


def _literal(text: str) -> int:
    match = re.fullmatch(r"(-?)(\d+)'([sS]?)([hHdDbB])([0-9a-fA-F_]+)", text)
    if not match:
        raise ValueError(f"unsupported HDL literal: {text}")
    sign, width, signed, base, value = match.groups()
    result = int(value.replace("_", ""), {"h": 16, "d": 10, "b": 2}[base.lower()])
    if result >= 1 << int(width):
        raise ValueError(f"out-of-range HDL literal: {text}")
    if sign:
        result = -result
    elif signed and result >= 1 << (int(width) - 1):
        result -= 1 << int(width)
    return result


def _case(source: str, name: str, indexes: list[int], *, default_count: int = 0) -> list[int]:
    body = _function(source, name)
    match = re.fullmatch(r"\s*case\s*\([^)]*\)(.*?)endcase\s*", body, flags=re.S)
    if not match:
        raise ValueError(f"{name} is not a direct constant case table")
    entries = match[1]
    pattern = (r"(" + _LITERAL + r"|default)\s*:\s*" + re.escape(name)
               + r"\s*=\s*(" + _LITERAL + r")\s*;")
    values: dict[int, int] = {}
    default = None
    for row in re.finditer(pattern, entries):
        key, value = row.groups()
        if key == "default":
            if default is not None:
                raise ValueError(f"duplicate default in {name}")
            default = _literal(value)
        else:
            index = _literal(key)
            if index in values:
                raise ValueError(f"duplicate index in {name}: {index}")
            values[index] = _literal(value)
    if re.sub(pattern, "", entries).strip():
        raise ValueError(f"unparsed expression in {name}")
    wanted = set(indexes)
    missing = wanted - values.keys()
    if values.keys() - wanted or len(missing) != default_count or default is None:
        raise ValueError(f"incomplete or unexpected indexes in {name}")
    return [values.get(index, default) for index in indexes]


def _phone_field(source: str, field: str, word: int, phone: int) -> int:
    name = "sc01a_" + field
    body = _function(source, name)
    match = re.search(r"\b" + name + r"\s*=\s*(.*?);", body, flags=re.S)
    if not match:
        raise ValueError(f"missing expression for {name}")
    expression = match[1].strip()
    if field == "pause":
        phones = re.findall(r"\(\s*phone\s*==\s*(" + _LITERAL + r")\s*\)", expression)
        remainder = re.sub(r"\(\s*phone\s*==\s*" + _LITERAL + r"\s*\)", "", expression)
        if len(phones) != 2 or remainder.strip() != "||":
            raise ValueError("unsupported pause expression")
        return int(phone in [_literal(item) for item in phones])
    terms = expression.strip("{}").split(",")
    result = 0
    for term in terms:
        match = re.fullmatch(r"\s*(~?)word\[(\d+)\]\s*", term)
        if not match or int(match[2]) >= 64:
            raise ValueError(f"unsupported bit expression for {name}")
        result = (result << 1) | (((word >> int(match[2])) & 1) ^ bool(match[1]))
    return result


def load_tables(package: Path = FORMANT_PKG, rom: Path = NATIVE_ROM) -> dict:
    """Read every coefficient and mapping from the current committed-style HDL.

    The phone rows describe the retained baseline model, not native SSI ROM
    records. The native ROM is separate and must match its known dump hash.
    """
    source = _without_comments(package.read_text(encoding="utf-8"))
    for name, expected in (("SC01_COEFF_FRAC_BITS", 15), ("SC01_FORMANT_SAMPLE_CLOCK_HZ", 48_000)):
        match = re.search(r"\blocalparam\s+int\s+" + name + r"\s*=\s*(\d+)\s*;", source)
        if not match or int(match[1]) != expected:
            raise ValueError(f"host model requires {name} = {expected}")
    rom_text = _without_comments(rom.read_text(encoding="ascii"))
    tokens = rom_text.split()
    if len(tokens) != 512 or any(not re.fullmatch(r"[0-9A-Fa-f]{2}", token) for token in tokens):
        raise ValueError("native ROM must contain exactly 512 byte literals")
    native = bytes(int(token, 16) for token in tokens)
    if hashlib.sha256(native).hexdigest() != ACTIVE_ROM_SHA256:
        raise ValueError("native ROM does not match the canonical SSI dump")
    rows = _case(source, "ssi263_sc02_rom_row", list(range(64)), default_count=1)
    if b"".join(row.to_bytes(8, "little") for row in rows) != native:
        raise ValueError("HDL native ROM rows differ from canonical ROM")
    words = _case(source, "sc01a_word_by_phone", list(range(64)))
    result = {
        "phones": [[_phone_field(source, field, word, phone) for field in PHONE_FIELDS]
                   for phone, word in enumerate(words)],
        "native_rom": list(native),
        "mappings": [_case(source, "ssi263_native_" + field + "_to_sc01", list(range(16)),
                           default_count=1) for field in MAPPING_FIELDS],
        "sc01_map": _case(source, "ssi263_to_sc01_phone", list(range(64))),
    }
    for field, count in (("f1", 16), ("f2", 32 * 16), ("f3", 16)):
        indexes = [index * 8 + tap for index in range(count) for tap in range(7)]
        flat = _case(source, "sc01a_" + field + "_coeff", indexes)
        result[field] = [flat[index:index + 7] for index in range(0, len(flat), 7)]
    for field, count in (("f4", 7), ("fn", 5), ("fx", 2)):
        result[field] = _case(source, "sc01a_" + field + "_coeff", list(range(count)))
    result["metadata"] = {
        "format_version": 1, "phone_fields": list(PHONE_FIELDS),
        "mapping_fields": list(MAPPING_FIELDS), "f2_index": "f2 * 16 + f2q",
        "coefficient_fraction_bits": 15, "coefficient_sample_rate": 48_000,
        "package_sha256": hashlib.sha256(package.read_bytes()).hexdigest(),
        "active_rom_sha256": hashlib.sha256(native).hexdigest(),
        "full_rom_sha256": hashlib.sha256(native + bytes(1536)).hexdigest(),
    }
    return result


def _flatten(values):
    for value in values:
        if isinstance(value, list):
            yield from _flatten(value)
        else:
            yield int(value)


def table_text(tables: dict) -> str:
    """Numeric stream: phones, ROM, six mappings, phone map, F1/F2/F3/F4/FN/FX.

    F2 contains 512 rows, ordered f2 first, then f2q. Every variable formant
    row contains seven taps. No dimensions or labels precede the numbers.
    """
    return "\n".join(" ".join(map(str, _flatten(tables[key]))) for key in TABLE_ORDER) + "\n"


def write_tables(path: Path, tables: dict | None = None) -> dict:
    tables = load_tables() if tables is None else tables
    payload = table_text(tables)
    path.write_bytes(payload.encode("ascii"))
    return {**tables["metadata"], "table_sha256": hashlib.sha256(payload.encode("ascii")).hexdigest()}


@dataclass(frozen=True)
class Event:
    tick: int
    socket: int
    register: int
    value: int
    source_index: int = -1


@dataclass
class Trace:
    events: list[Event]
    xck_hz: int
    duration_ticks: int
    metadata: dict

    def validate(self) -> None:
        if (type(self.xck_hz) is not int or type(self.duration_ticks) is not int
                or self.xck_hz <= 0 or self.duration_ticks <= 0):
            raise ValueError("trace clock and duration must be positive")
        prior = None
        for event in self.events:
            if any(type(value) is not int for value in
                   (event.tick, event.socket, event.register, event.value, event.source_index)):
                raise ValueError("event fields must be integers")
            if event.socket not in (0, 1) or event.register not in range(8) or event.value not in range(256):
                raise ValueError("invalid socket, register, or data byte")
            if prior is not None and event.tick < prior:
                raise ValueError("trace events must be ordered")
            if event.tick > self.duration_ticks:
                raise ValueError("event exceeds trace duration")
            prior = event.tick


def load_calibration(path: Path) -> Trace:
    """Retain exact nominal write cycles, including every pre-roll SSI write.

    PAL Phasor CPU cycles equal effective XCK ticks (raw XCK/2). All package
    writes are checked before AY writes are removed from the raw SSI stream.
    AY writes remain in metadata so the omission is explicit and countable.
    """
    from sim_ssi263_calibration import load_package

    source = load_package(path)
    events = [Event(event.cpu_cycle, event.target - 4, event.register, event.value, event.index)
              for event in source.events if event.target in (4, 5)]
    ignored = [dict(event.__dict__) for event in source.events if event.target < 4]
    trace = Trace(events, source.cpu_hz,
                  int(source.manifest["duration_ticks"]) * source.period,
                  {"name": source.manifest["build_id"],
                   "package_sha256": source.package_sha256,
                   "trace_sha256": source.trace_sha256,
                   "trace_model": source.trace_model,
                   "total_checked_writes": len(source.events),
                   "ssi_writes": len(events), "ignored_ay_writes": ignored,
                   "segments": source.manifest.get("segments", []),
                   "clock_assumption": "CPU cycles equal effective XCK ticks; raw XCK/2",
                   "channel_order": ["target4: secondary SSI", "target5: primary SSI"]})
    trace.validate()
    return trace


def make_demo(name: str, xck_hz: int = PAL_EFFECTIVE_XCK_HZ,
              filter_frequency: int = 128, articulation: int = 5) -> Trace:
    """Hand-authored SSI register demos; these are not measured chip captures."""
    if name not in DEMO_NAMES:
        raise ValueError(f"unknown demo {name!r}; choose from {', '.join(DEMO_NAMES)}")
    if type(filter_frequency) is not int or filter_frequency not in range(256):
        raise ValueError("filter_frequency must be an integer from 0 through 255")
    if type(articulation) is not int or articulation not in range(8):
        raise ValueError("articulation must be an integer from 0 through 7")
    events = []
    # Stop, select immediate inflection function, set pitch/rate/filter, run.
    # I=2942 gives about 110 Hz at PAL XCK. CTL release latches function 2.
    setup = ((3, 0x80), (0, 0x80), (1, 0x6F), (2, 0x8E),
             (4, filter_frequency), (3, (articulation << 4) | 12))
    for index, (register, value) in enumerate(setup):
        for socket in (0, 1):
            events.append(Event(-120 + 20 * index + socket, socket, register, value))
    if name in ("hello", "hello_four"):
        repeats = 4 if name == "hello_four" else 2
        sequence = [(0x00, 100), (0x2C, 100), (0x0B, 160), (0x20, 120),
                    (0x11, 200), (0x12, 90), (0x00, 250)] * repeats
        labels = f"PA HF EH1 L O OU PA, repeated {repeats} times in one run; approximate hand-authored hello"
    elif name == "transitions":
        sequence = [(phone, 350) for phone in (0x0E, 0x2C, 0x0E, 0x2D, 0x0E,
                                               0x30, 0x0E, 0x01, 0x16, 0x01)]
        labels = "AH HF AH HFC AH S AH E U E"
    else:
        sequence = [(phone, 250) for phone in range(64)]
        labels = "All 64 ROM addresses in ascending order, 250 ms each"
    elapsed_ms = 0
    segments = []
    for phone, duration_ms in sequence:
        tick = elapsed_ms * xck_hz // 1000
        for socket in (0, 1):
            events.append(Event(tick + socket, socket, 0, phone))
        segments.append({"phone": phone, "start_tick": tick,
                         "end_tick": (elapsed_ms + duration_ms) * xck_hz // 1000})
        elapsed_ms += duration_ms
    stop = elapsed_ms * xck_hz // 1000
    for socket in (0, 1):
        events.append(Event(stop + socket, socket, 3, 0x80))
    trace = Trace(events, xck_hz, (elapsed_ms + 200) * xck_hz // 1000,
                  {"name": name, "description": labels, "segments": segments,
                   "filter_frequency": filter_frequency, "articulation": articulation,
                   "inflection_word": 2942,
                   "source": "hand-authored host demo", "ssi_writes": len(events),
                   "channel_order": ["socket0", "socket1"]})
    trace.validate()
    return trace


def write_trace(path: Path, trace: Trace) -> None:
    """Export untrimmed tick/socket/register/value rows for the host renderer."""
    trace.validate()
    path.write_text("".join(f"{event.tick} {event.socket} {event.register} {event.value}\n"
                            for event in trace.events), encoding="ascii")


def tables_json(tables: dict | None = None) -> str:
    return json.dumps(load_tables() if tables is None else tables, indent=2) + "\n"
