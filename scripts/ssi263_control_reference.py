#!/usr/bin/env python3
"""Event-level reference for the documented SC-02 TPARM and route latches.

This is a prototype circuit reference, not a complete production SSI-263 model.
The equations come from docs/SSI263_SC02_ROM_FORMAT.md. Callers supply selector,
duration, source and phase events: this module does not invent their clocks,
the U68 amplitude counter, or the CD4006 noise recurrence. Unknown startup bits
are None and remain unknown until the supplied events establish their values.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
from typing import Iterable


Bit = int | None
ROOT = Path(__file__).resolve().parents[1]
ROM_PATH = ROOT / "hdl/apple/ssi263_sc02_rom.mem"
ROM_SHA256 = "849baa20baae3d756f26813cf4e4f47392573e735cb4c66afdc434f9932147e0"
STATE_NAMES = ("pw0", "pw1", "pw2", "pw3", "pw5", "u20", "fric1_sw", "fric2_sw")
GATE_NAMES = ("u104c", "ampct0", "fricative", "u32b")
STIMULUS_FIELDS = {
    "phone_write": (0, 1), "wr_sel0": (1, 1), "wr_sel1": (2, 1),
    "wr_sel2": (3, 1), "duration_phase": (4, 4), "tparm": (8, 4),
    "latched_ctrl": (12, 1), "ampct_zero": (13, 1),
    "voice_amplitude_zero": (14, 1), "fricative_amplitude_zero": (15, 1),
    "tpho5": (16, 1), "u62_q_n": (17, 1), "d3_or_d4": (18, 1),
    "phi1": (19, 1), "phi0_rise": (20, 1), "cold_resetn": (21, 1),
}


def bit(value: Bit) -> Bit:
    if value not in (None, 0, 1):
        raise ValueError(f"Expected 0, 1 or None, got {value!r}")
    return None if value is None else int(value)


def invert(value: Bit) -> Bit:
    value = bit(value)
    return None if value is None else 1 - value


def conjunction(*values: Bit) -> Bit:
    values = tuple(bit(value) for value in values)
    return 0 if 0 in values else None if None in values else 1


def disjunction(*values: Bit) -> Bit:
    values = tuple(bit(value) for value in values)
    return 1 if 1 in values else None if None in values else 0


def held_load(old: Bit, enable: Bit, new: Bit) -> Bit:
    """An unknown enable preserves a fact only when both outcomes agree."""
    old, enable, new = bit(old), bit(enable), bit(new)
    if enable == 0:
        return old
    if enable == 1:
        return new
    return old if old == new else None


def load_rom(path: Path = ROM_PATH) -> bytes:
    fields = [field for line in path.read_text(encoding="ascii").splitlines()
              for field in line.split("//", 1)[0].split()]
    data = bytes(int(field, 16) for field in fields)
    if len(data) != 512 or hashlib.sha256(data + bytes(1536)).hexdigest() != ROM_SHA256:
        raise ValueError("SSI ROM length or source identity differs")
    return data


def rom_byte(phone: int, selector: int, rom: bytes | None = None) -> int:
    if not 0 <= phone < 64 or not 0 <= selector < 8:
        raise ValueError("Phone must be 0..63 and selector 0..7")
    return (load_rom() if rom is None else rom)[8 * phone + selector]


@dataclass
class SourceControlState:
    pw0: Bit = None
    pw1: Bit = None
    pw2: Bit = None
    pw3: Bit = None
    pw5: Bit = None
    u20: Bit = None
    fric1_sw: Bit = None
    fric2_sw: Bit = None
    # These are caller-supplied input levels, not extra circuit latches.
    ctrl: Bit = None
    phi1: Bit = 0

    def snapshot(self) -> dict[str, Bit]:
        return {name: getattr(self, name) for name in STATE_NAMES}

    def phone_write(self) -> None:
        self.pw0 = self.pw1 = 0

    def set_latched_ctrl(self, value: Bit) -> None:
        """Set the external CTRL level; this does not model a host CTL write."""
        self.ctrl = bit(value)

    def set_phi1(self, level: Bit) -> None:
        self.phi1 = bit(level)
        self.fric1_sw = held_load(self.fric1_sw, self.phi1, self.u20)

    def phi0_rising(self) -> None:
        self.fric2_sw = invert(self.u20)

    def selector_edge(self, selector: int, rom_byte: int, duration_phase: int,
                      *, ctrl: Bit = None, ampct_zero: Bit = None,
                      fa_zero: Bit = None) -> None:
        """Apply one qualified WR_SEL edge; all conditions use pre-edge state.

        Pass ctrl explicitly, including None if unknown. The high ROM nibble
        is deliberately ignored here; it is a target, not a control extension.
        Selector slots 3..7 have no documented TPARM latch action.
        """
        if not 0 <= selector < 8 or not 0 <= rom_byte <= 255 or not 0 <= duration_phase < 16:
            raise ValueError("Invalid selector, ROM byte, or duration phase")
        self.ctrl = bit(ctrl)
        tparm0, tparm1, tparm2, tparm3 = ((rom_byte >> n) & 1 for n in range(4))
        if selector in (0, 1):
            if duration_phase == (2 if tparm0 else 6):
                setattr(self, "pw0" if selector == 0 else "pw1", 1)
        elif selector == 2:
            route_enable = conjunction(
                self.pw1,
                disjunction(self.pw2, tparm2),
                disjunction(conjunction(self.pw0, self.pw1, ampct_zero), fa_zero),
            )
            self.u20 = held_load(self.u20, route_enable, tparm3)
            self.pw3 = held_load(self.pw3, self.pw1, disjunction(self.ctrl, invert(tparm1)))
            self.pw2, self.pw5 = tparm2, invert(tparm2)
        # U112 is transparent throughout Phi1, including a change at U20.
        self.set_phi1(self.phi1)

    def transition_permit(self, tpho5: Bit, va_nonzero: Bit, fa_nonzero: Bit) -> Bit:
        return invert(conjunction(self.pw5, disjunction(tpho5, va_nonzero, fa_nonzero)))

    def source_terms(self, u62_nq: Bit, d3_or4: Bit, va_zero: Bit) -> dict[str, Bit]:
        u104c = conjunction(self.pw3, u62_nq)
        return {"u104c": u104c, "ampct0": invert(u104c),
                "fricative": conjunction(invert(disjunction(d3_or4, u104c)),
                                          disjunction(u62_nq, va_zero))}


def apply_inputs(state: SourceControlState, inputs: dict[str, int]) -> dict:
    """Apply one supplied fabric observation and return a vector expectation.

    The caller must resolve simultaneous phone/selector and selector2/Phi0
    events before using this event model. Their physical ordering is not
    specified by the published equations; this API refuses to choose one.
    """
    if set(inputs) != set(STIMULUS_FIELDS):
        raise ValueError("Supply every documented stimulus field exactly once")
    for name, (_, width) in STIMULUS_FIELDS.items():
        if not isinstance(inputs[name], int) or not 0 <= inputs[name] < 1 << width:
            raise ValueError(f"Invalid stimulus {name}")
    selectors = [n for n in range(3) if inputs[f"wr_sel{n}"]]
    if len(selectors) > 1 or inputs["phone_write"] and selectors:
        raise ValueError("Resolve overlapping phone/selector edges before replay")
    if 2 in selectors and inputs["phi0_rise"]:
        raise ValueError("Resolve selector2/Phi0 edge order before replay")
    if not inputs["cold_resetn"]:
        # This represents an observation before any known latch history, not
        # a claim that the prototype's physical reset clears these latches.
        for name in STATE_NAMES:
            setattr(state, name, None)
        state.ctrl = inputs["latched_ctrl"]
        state.phi1 = inputs["phi1"]
    else:
        state.set_latched_ctrl(inputs["latched_ctrl"])
        # Close/open the transparent phase first. While high, selector_edge
        # also propagates a new U20 value through U112.
        state.set_phi1(inputs["phi1"])
        if inputs["phone_write"]:
            state.phone_write()
        if selectors:
            state.selector_edge(selectors[0], inputs["tparm"], inputs["duration_phase"],
                                ctrl=inputs["latched_ctrl"], ampct_zero=inputs["ampct_zero"],
                                fa_zero=inputs["fricative_amplitude_zero"])
        if inputs["phi0_rise"]:
            state.phi0_rising()
    gates = state.source_terms(inputs["u62_q_n"], inputs["d3_or_d4"],
                               inputs["voice_amplitude_zero"])
    gates["u32b"] = state.transition_permit(inputs["tpho5"],
                                           invert(inputs["voice_amplitude_zero"]),
                                           invert(inputs["fricative_amplitude_zero"]))
    return {"inputs": dict(inputs), "state": state.snapshot(), "gates": gates}


def trace(events: Iterable[dict[str, int]]) -> list[dict]:
    """Return JSON-ready expected state/gates for complete input vectors."""
    state = SourceControlState()
    return [apply_inputs(state, event) for event in events]


def pack_bits(values: dict[str, Bit], names: tuple[str, ...]) -> tuple[int, int]:
    value = known = 0
    for shift, name in enumerate(names):
        candidate = bit(values[name])
        if candidate is not None:
            known |= 1 << shift
            value |= candidate << shift
    return value, known


def pack_vector(item: dict) -> int:
    stimulus = sum(item["inputs"][name] << shift
                   for name, (shift, _) in STIMULUS_FIELDS.items())
    state, state_known = pack_bits(item["state"], STATE_NAMES)
    gates, gates_known = pack_bits(item["gates"], GATE_NAMES)
    return stimulus << 24 | state << 16 | state_known << 8 | gates << 4 | gates_known


def qualification_events() -> list[dict[str, int]]:
    """Exercise native ROM controls and gate truth tables, without a scan clock."""
    events = []
    inputs = {name: 0 for name in STIMULUS_FIELDS}
    inputs.update(cold_resetn=1, ampct_zero=1, voice_amplitude_zero=1,
                  fricative_amplitude_zero=1)

    def emit(**changes: int) -> None:
        inputs.update(phone_write=0, wr_sel0=0, wr_sel1=0, wr_sel2=0, phi0_rise=0)
        inputs.update(changes)
        events.append(dict(inputs))

    rom = load_rom()
    emit(cold_resetn=0)
    emit(cold_resetn=1)
    for phone in range(64):
        emit(phone_write=1, phi1=0, tpho5=phone >> 5)
        # Probe all phases: PW0/1 set once at their own comparison point.
        for phase in range(16):
            emit(wr_sel0=1, duration_phase=phase, tparm=rom_byte(phone, 0, rom) & 15)
            emit(wr_sel1=1, tparm=rom_byte(phone, 1, rom) & 15)
            emit(wr_sel2=1, tparm=rom_byte(phone, 2, rom) & 15,
                 latched_ctrl=(phase >> 3), phi1=0)
            emit(phi1=1)
            emit(phi1=0, phi0_rise=1)
    # Exhaustive source combinational inputs, with both qualified PW3 values.
    for pw3 in (0, 1):
        emit(wr_sel2=1, tparm=0xE if pw3 == 0 else 0xC, latched_ctrl=0, phi1=0)
        for conditions in range(64):
            emit(u62_q_n=conditions & 1, d3_or_d4=(conditions >> 1) & 1,
                 voice_amplitude_zero=(conditions >> 2) & 1,
                 fricative_amplitude_zero=(conditions >> 3) & 1,
                 tpho5=(conditions >> 4) & 1, latched_ctrl=(conditions >> 5) & 1)
    # Gate each condition and each old/new PW2 pair independently.
    for conditions in range(128):
        # Establish route zero through valid inputs before requesting one.
        # Otherwise a previously held one could hide an incorrect load gate.
        emit(wr_sel0=1, duration_phase=2, tparm=1, phi1=0)
        emit(wr_sel1=1, duration_phase=2, tparm=1)
        emit(wr_sel2=1, tparm=4, ampct_zero=1,
             fricative_amplitude_zero=1)
        emit(phi1=1)
        emit(phi1=0, phi0_rise=1)
        emit(phone_write=1, phi1=0, latched_ctrl=0)
        if conditions & 1:
            emit(wr_sel0=1, duration_phase=2, tparm=1)
        if conditions & 2:
            emit(wr_sel1=1, duration_phase=2, tparm=1)
        old_pw2 = (conditions >> 2) & 1
        emit(wr_sel2=1, tparm=old_pw2 << 2, ampct_zero=0,
             fricative_amplitude_zero=0)
        emit(wr_sel2=1, tparm=8 | ((conditions >> 3) & 1) << 2,
             ampct_zero=(conditions >> 4) & 1,
             fricative_amplitude_zero=(conditions >> 5) & 1,
             phi1=(conditions >> 6) & 1)
        emit(phi1=0, phi0_rise=1)
        emit(phi1=1)
    return events


def write_vectors(path: Path) -> int:
    """Write 56-bit readmemh vectors.

    [55:24] stimulus, [23:16] state value, [15:8] state known,
    [7:4] gate value, [3:0] gate known. Unknown value bits encode as zero;
    a consumer must mask them with the separate known word. State/gate and
    stimulus bit orders are the named constants at the start of this module.
    """
    vectors = trace(qualification_events())
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(f"{pack_vector(item):014x}\n" for item in vectors), encoding="ascii")
    return len(vectors)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--events", type=Path, help="JSON array of complete stimulus vectors")
    parser.add_argument("--out", type=Path, help="JSON trace; unknown bits remain null")
    parser.add_argument("--vectors", type=Path, help="56-bit qualification vectors for readmemh")
    args = parser.parse_args()
    if args.events and not args.out:
        parser.error("--events requires --out")
    if args.vectors:
        print(f"Wrote {write_vectors(args.vectors)} prototype control vectors to {args.vectors}")
    if args.out:
        events = json.loads(args.events.read_text()) if args.events else qualification_events()
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(trace(events), indent=2) + "\n", encoding="utf-8")
    if not args.vectors and not args.out:
        parser.error("Choose --vectors or --out")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
