#!/usr/bin/env python3
"""Cycle reference for the native SSI-263A / SC-02 control interface.

This model contains only behavior supported by the manufacturer documents,
the supplied SC-02 reconstruction, or real-card mb-audit results.  The ROM
lower-nibble pulse decoder and the analog state equations are added in later
checkpoints; callers supply an explicit transition pulse in this version.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from hashlib import sha256
from pathlib import Path
import zlib


ROOT = Path(__file__).resolve().parents[1]
ROM_PATH = ROOT / "hdl" / "apple" / "ssi263_sc02_rom.mem"
ROM_SOURCE_SIZE = 2048
ROM_ACTIVE_SIZE = 512
ROM_SHA256 = "9c3bba73319e1ed3652c85dac19874df04cbb72e62fdd63d6cbd7b34ff81f941"
ROM_CRC32 = 0xCC0A72EE

MODE_REQUEST_DISABLED = 0
MODE_FRAME_IMMEDIATE = 1
MODE_PHONEME_IMMEDIATE = 2
MODE_PHONEME_TRANSITIONED = 3

PARAMETER_NAMES = (
    "f1",
    "f2",
    "f2_res",
    "f3",
    "f4",
    "filter_amp",
    "voice_amp",
    "fric_amp",
)

SLOWCLOCK_FAST_TICKS = 4
SELECTOR_SLOW_EDGES = 4
SELECTOR_FAST_TICKS = SLOWCLOCK_FAST_TICKS * SELECTOR_SLOW_EDGES


def load_active_rom(path: Path = ROM_PATH) -> tuple[int, ...]:
    values: list[int] = []
    for raw_line in path.read_text(encoding="ascii").splitlines():
        line = raw_line.partition("//")[0].strip()
        if not line:
            continue
        value = int(line, 16)
        if value > 0xFF:
            raise ValueError(f"ROM value is wider than eight bits: {line}")
        values.append(value)
    if len(values) != ROM_ACTIVE_SIZE:
        raise ValueError(
            f"expected {ROM_ACTIVE_SIZE} active ROM bytes, got {len(values)}"
        )
    return tuple(values)


def reconstructed_source_image(rom: tuple[int, ...]) -> bytes:
    if len(rom) != ROM_ACTIVE_SIZE:
        raise ValueError("active ROM must contain 512 bytes")
    return bytes(rom) + bytes(ROM_SOURCE_SIZE - ROM_ACTIVE_SIZE)


def verify_rom(rom: tuple[int, ...]) -> None:
    image = reconstructed_source_image(rom)
    digest = sha256(image).hexdigest()
    crc = zlib.crc32(image) & 0xFFFFFFFF
    if digest != ROM_SHA256:
        raise ValueError(f"SSI-263 ROM SHA-256 mismatch: {digest}")
    if crc != ROM_CRC32:
        raise ValueError(f"SSI-263 ROM CRC32 mismatch: {crc:08x}")


def rom_address(phoneme: int, selector: int) -> int:
    return ((phoneme & 0x3F) << 3) | (selector & 7)


def inflection_word(inflection_high: int, rate_inflection: int) -> int:
    return (
        ((rate_inflection & 0x08) << 8)
        | ((inflection_high & 0xFF) << 3)
        | (rate_inflection & 0x07)
    )


def pitch_period_ticks(inflection: int) -> int:
    """Return one voice period in effective XCK ticks."""

    return 8 * (4096 - (inflection & 0xFFF))


def filter_period_ticks(filter_frequency: int) -> int:
    """Return one full Phi0/Phi1 filter period in effective XCK ticks."""

    return 2 * (256 - (filter_frequency & 0xFF))


def frame_ticks(rate: int) -> int:
    return 4096 * (16 - (rate & 0x0F))


def phoneme_ticks(rate: int, duration: int) -> int:
    return frame_ticks(rate) * (4 - (duration & 0x03))


def move_one_toward(value: int, target: int) -> int:
    value &= 0x0F
    target &= 0x0F
    if value < target:
        return value + 1
    if value > target:
        return value - 1
    return value


@dataclass
class SSI263Reference:
    """Pin and control reference for one SSI-263AP instance."""

    rom: tuple[int, ...] = field(default_factory=load_active_rom)
    # Provisional Phasor card wiring: about 2x bus at XCK, then DIV2 in the
    # chip.  These are card assumptions, not intrinsic SSI-263 defaults.
    xck_edges_per_bus_cycle: int = 2
    div2: bool = True
    revision_ap: bool = True

    duration_phoneme: int = 0xC0
    inflection_high: int = 0x00
    rate_inflection: int = 0x00
    control_articulation_amplitude: int = 0x80
    filter_frequency: int = 0xFF

    response_mode: int = MODE_PHONEME_TRANSITIONED
    ar_enabled: bool = False
    d7_pending: bool = False
    phone_active: bool = False
    pd_rst_asserted: bool = False

    xck_pin_edges: int = 0
    effective_xck_ticks: int = 0
    div2_phase: int = 0
    ticks_to_boundary: int = 0
    completed_boundaries: int = 0

    filter_ticks_to_toggle: int = 1
    filter_phase: int = 0
    filter_phase_edges: int = 0

    selector: int = 0
    selector_subphase: int = 0
    selector_pulse_seen: bool = False
    parameter_values: dict[str, int] = field(
        default_factory=lambda: {name: 0 for name in PARAMETER_NAMES}
    )

    _write_active: bool = False
    _write_register: int = 0
    _write_data: int = 0

    def __post_init__(self) -> None:
        verify_rom(self.rom)
        if self.xck_edges_per_bus_cycle <= 0:
            raise ValueError("XCK edges per bus cycle must be positive")
        self.filter_ticks_to_toggle = 256 - self.filter_frequency

    @property
    def powered_down(self) -> bool:
        return self.pd_rst_asserted or bool(
            self.control_articulation_amplitude & 0x80
        )

    @property
    def ar_drive_low(self) -> bool:
        return self.d7_pending and self.ar_enabled and not self.powered_down

    @property
    def phoneme(self) -> int:
        return self.duration_phoneme & 0x3F

    @property
    def duration(self) -> int:
        return (self.duration_phoneme >> 6) & 0x03

    @property
    def rate(self) -> int:
        return (self.rate_inflection >> 4) & 0x0F

    @property
    def inflection(self) -> int:
        return inflection_word(self.inflection_high, self.rate_inflection)

    @property
    def articulation(self) -> int:
        return (self.control_articulation_amplitude >> 4) & 0x07

    @property
    def amplitude(self) -> int:
        return self.control_articulation_amplitude & 0x0F

    def read_d7(self) -> int:
        return int(self.d7_pending)

    def begin_write(self, register: int, data: int) -> None:
        """Present a selected write without latching it yet."""

        self._write_active = True
        self._write_register = register & 7
        self._write_data = data & 0xFF

    def end_write(self) -> None:
        """Latch the address and data when the active write condition ends."""

        if not self._write_active:
            return
        self._write_active = False
        self._latch_write(self._write_register, self._write_data)

    def write(self, register: int, data: int) -> None:
        self.begin_write(register, data)
        self.end_write()

    def _acknowledge(self) -> None:
        self.d7_pending = False

    def _latch_write(self, register: int, data: int) -> None:
        if register <= 2 or (register == 3 and (data & 0x80)):
            self._acknowledge()

        if register == 0:
            self.duration_phoneme = data
            if not self.powered_down:
                self._start_or_replace_phone()
        elif register == 1:
            self.inflection_high = data
        elif register == 2:
            self.rate_inflection = data
        elif register == 3:
            old_control = self.control_articulation_amplitude
            self.control_articulation_amplitude = data
            old_power_down = bool(old_control & 0x80)
            new_power_down = bool(data & 0x80)
            if new_power_down:
                self.phone_active = False
                self.ar_enabled = False
            elif old_power_down:
                requested_mode = self.duration
                if requested_mode != MODE_REQUEST_DISABLED:
                    self.response_mode = requested_mode
                    self.ar_enabled = True
                else:
                    self.ar_enabled = False
                self._start_or_replace_phone()
        else:
            self.filter_frequency = data

    def assert_pd_rst(self) -> None:
        """Assert active-low PD/RST while retaining non-control registers."""

        self.pd_rst_asserted = True
        self.control_articulation_amplitude |= 0x80
        self.d7_pending = False
        self.ar_enabled = False
        self.phone_active = False

    def release_pd_rst(self) -> None:
        self.pd_rst_asserted = False

    def _boundary_period(self) -> int:
        if self.response_mode == MODE_FRAME_IMMEDIATE:
            return frame_ticks(self.rate)
        return phoneme_ticks(self.rate, self.duration)

    def _start_or_replace_phone(self) -> None:
        self.phone_active = True
        self.ticks_to_boundary = self._boundary_period()

    def feed_bus_cycles(self, cycles: int) -> None:
        if cycles < 0:
            raise ValueError("cycle count must not be negative")
        self.feed_xck_edges(cycles * self.xck_edges_per_bus_cycle)

    def feed_xck_edges(self, edges: int) -> None:
        if edges < 0:
            raise ValueError("edge count must not be negative")
        self.xck_pin_edges += edges
        if not self.div2:
            self.advance_effective_ticks(edges)
            return

        total = self.div2_phase + edges
        ticks = total // 2
        self.div2_phase = total & 1
        self.advance_effective_ticks(ticks)

    def advance_effective_ticks(self, ticks: int) -> None:
        if ticks < 0:
            raise ValueError("tick count must not be negative")
        self.effective_xck_ticks += ticks
        self._advance_filter_clock(ticks)

        remaining = ticks
        while self.phone_active and remaining:
            if self.ticks_to_boundary == 0:
                self.ticks_to_boundary = self._boundary_period()
            if remaining < self.ticks_to_boundary:
                self.ticks_to_boundary -= remaining
                break
            remaining -= self.ticks_to_boundary
            self.completed_boundaries += 1
            self.d7_pending = True
            self.ticks_to_boundary = self._boundary_period()

    def step_effective_tick(
        self,
        *,
        write_end: tuple[int, int] | None = None,
        assert_pd_rst: bool = False,
    ) -> None:
        """Advance one effective tick with explicit same-edge priority.

        Timer and filter work occur first in the software model, then the
        higher-priority write or PD/RST event overrides their visible result.
        This makes an acknowledgment on a completion edge win, as it does in
        the planned RTL event table.
        """

        self.advance_effective_ticks(1)
        if write_end is not None:
            self._latch_write(write_end[0] & 7, write_end[1] & 0xFF)
        if assert_pd_rst:
            self.assert_pd_rst()

    def _advance_filter_clock(self, ticks: int) -> None:
        remaining = ticks
        while remaining >= self.filter_ticks_to_toggle:
            remaining -= self.filter_ticks_to_toggle
            self.filter_phase ^= 1
            self.filter_phase_edges += 1
            self.filter_ticks_to_toggle = 256 - self.filter_frequency
        self.filter_ticks_to_toggle -= remaining

    def rom_byte(self, selector: int | None = None) -> int:
        active_selector = self.selector if selector is None else selector
        return self.rom[rom_address(self.phoneme, active_selector)]

    def target_for_selector(self, selector: int | None = None) -> int:
        active_selector = self.selector if selector is None else selector & 7
        if active_selector == 4:
            return self.amplitude
        return self.rom_byte(active_selector) >> 4

    def flags_for_selector(self, selector: int | None = None) -> int:
        return self.rom_byte(selector) & 0x0F

    def _apply_selected_transition(self) -> None:
        """Apply the provisional one-count target move for the active slot."""

        selector = self.selector & 7
        target = self.target_for_selector(selector)
        destinations: tuple[str, ...]
        if selector == 0:
            destinations = ("f1",)
        elif selector == 1:
            destinations = ("f2",)
        elif selector == 2:
            destinations = ("f2_res",)
        elif selector == 3:
            destinations = ("f3", "f4")
        elif selector == 4:
            destinations = ("filter_amp",)
        elif selector == 5:
            destinations = ("voice_amp",)
        elif selector == 6:
            destinations = ("fric_amp",)
        else:
            destinations = ()
        for name in destinations:
            self.parameter_values[name] = move_one_toward(
                self.parameter_values[name], target
            )

    def advance_selector_slow_edge(self, pulse: bool = False) -> None:
        """Advance one SLOWCLK edge without guessing the pulse decoder.

        Sheet 3 holds each selector value for four SLOWCLK edges, or sixteen
        FASTCLK ticks.  ``pulse`` records that the lower-nibble decoder selected
        a write during the slot.  The exact pulse subphase remains subject to
        the sheets 4-6 gate transcription.
        """

        self.selector_pulse_seen |= pulse
        self.selector_subphase += 1
        if self.selector_subphase == SELECTOR_SLOW_EDGES:
            if self.selector_pulse_seen:
                self._apply_selected_transition()
            self.selector = (self.selector + 1) & 7
            self.selector_subphase = 0
            self.selector_pulse_seen = False

    def transition_step(self, pulse: bool = True) -> None:
        """Advance one full selector slot for vector-generation convenience."""

        for subphase in range(SELECTOR_SLOW_EDGES):
            self.advance_selector_slow_edge(pulse and subphase == 0)


def main() -> int:
    rom = load_active_rom()
    verify_rom(rom)
    print(
        "SSI263 SC02 ROM PASS "
        f"bytes={len(rom)} crc32={ROM_CRC32:08X} sha256={ROM_SHA256}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
