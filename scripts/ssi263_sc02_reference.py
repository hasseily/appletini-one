#!/usr/bin/env python3
"""Cycle reference for the native SSI-263A / SC-02 control interface.

This model contains only behavior supported by the manufacturer documents,
the supplied SC-02 reconstruction, or real-card mb-audit results.  It models
the reconstructed rate, articulation, inflection, ROM scan, and held control
state.  It does not model the analog filter and source equations.
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
ROM_ACTIVE_SHA256 = "101d129a5f104e6190f2eca518bbf9ef65bf4ff92684d29eba56d9641aa02b0a"
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
    active_digest = sha256(bytes(rom)).hexdigest()
    if active_digest != ROM_ACTIVE_SHA256:
        raise ValueError(f"SSI-263 active ROM SHA-256 mismatch: {active_digest}")
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


def voice_clock_period_ticks(inflection: int) -> int:
    """Return one raw U59 VOICECLK period in effective XCK ticks."""

    return 4 * (4096 - (inflection & 0xFFF))


def filter_period_ticks(filter_frequency: int) -> int:
    """Return one full Phi0/Phi1 filter period in effective XCK ticks."""

    return 2 * (256 - (filter_frequency & 0xFF))


def frame_ticks(rate: int) -> int:
    return 4096 * (16 - (rate & 0x0F))


def phoneme_ticks(rate: int, duration: int) -> int:
    return frame_ticks(rate) * (4 - (duration & 0x03))


def rate_clock_ticks(rate: int) -> int:
    """Return the effective ticks between RATECLK rising edges."""

    return 128 * (16 - (rate & 0x0F))


def articulation_step_ticks(rate: int, articulation: int) -> int:
    """Return the steady-state ticks between U94 terminal pulses."""

    return 2 * rate_clock_ticks(rate) * (8 - (articulation & 0x07))


def inflection_step_ticks(rate: int, slope: int) -> int:
    """Return the steady-state ticks between U66 terminal pulses."""

    return rate_clock_ticks(rate) * (8 - (slope & 0x07))


def move_one_toward_byte(value: int, target: int) -> int:
    value &= 0xFF
    target &= 0xFF
    if value < target:
        return value + 1
    if value > target:
        return value - 1
    return value


@dataclass
class SSI263Reference:
    """Pin and control reference for one SSI-263AP instance."""

    rom: tuple[int, ...] = field(default_factory=load_active_rom)
    # Phasor card profile: Apple Q3 at XCK, then DIV2 in the chip.  These are
    # card settings, not intrinsic SSI-263 defaults.
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
    closure_events: int = 0
    last_closure_tick: int | None = None

    voice_ticks_to_edge: int = 16384
    voice_clock_edges: int = 0
    pitch_events: int = 0
    last_pitch_event_tick: int | None = None
    # Deterministic FPGA power-up seeds for physical counters with no reset
    # pin. Runtime behavior follows the sheet-6 U68/U85C network.
    u62_q: bool = False
    ampct_count: int = 0
    u68_clock_level: bool = False
    u41c_level: bool = False
    noise_clock_edges: int = 0

    selector: int = 0
    selector_subphase: int = 0
    selector_fast_phase: int = 0
    selector_steps: int = 0
    parameter_values: dict[str, int] = field(
        default_factory=lambda: {name: 0 for name in PARAMETER_NAMES}
    )
    # Sheet 4 U87-U90 contain eight addressed DDA states. RESA is the value
    # later copied to the parameter latches. RESB holds target-A modulo 16,
    # RESC is the four-bit accumulator, and RESCY retains the setup carry as
    # the direction bit.
    parameter_resa: list[int] = field(default_factory=lambda: [0] * 8)
    parameter_resb: list[int] = field(default_factory=lambda: [0] * 8)
    parameter_resc: list[int] = field(default_factory=lambda: [8] * 8)
    parameter_rescy: list[bool] = field(default_factory=lambda: [True] * 8)
    parameter_write_log: list[tuple[int, int]] = field(default_factory=list)

    # U76/U93/U169/U166 retain asynchronous events until the positive SEL2
    # boundary, then expose them for one complete selector scan.
    phone_setup_pending: bool = False
    control_setup_pending: bool = False
    phone_setup_window: bool = False
    control_setup_window: bool = False
    tc_edge_pending: bool = False
    tc_edge_window: bool = False
    duration_edge_pending: bool = False
    duration_edge_window: bool = False
    u93_rate_q1: bool = False
    u93_rate_q2: bool = False
    u166b_nq: bool = True

    rate_edges_left: int = 16
    rate_clock: int = 0
    rate_clock_div2: int = 0
    articulation_edges_left: int = 8
    inflection_edges_left: int = 8
    rate_clock_rises: int = 0
    rate_clock_div2_rises: int = 0
    articulation_steps: int = 0
    inflection_steps: int = 0
    last_articulation_tick: int | None = None
    last_inflection_tick: int | None = None

    transitioned_inflection: int = 0
    u37_count: int = 0
    u29a_level: bool = True
    u183a_q: bool = True
    pw_0: bool = False
    pw_1: bool = False
    pw_2: bool = False
    pw_3: bool = False
    pw_5: bool = True
    fric1_sw: bool = False
    fric2_sw: bool = True
    u20b_q: bool = False

    _write_active: bool = False
    _write_register: int = 0
    _write_data: int = 0

    def __post_init__(self) -> None:
        verify_rom(self.rom)
        if self.xck_edges_per_bus_cycle <= 0:
            raise ValueError("XCK edges per bus cycle must be positive")
        self.filter_ticks_to_toggle = 256 - self.filter_frequency
        self.voice_ticks_to_edge = voice_clock_period_ticks(
            self.pitch_inflection
        )

    @property
    def powered_down(self) -> bool:
        return (self.revision_ap and self.pd_rst_asserted) or bool(
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
    def transitioned_inflection_target(self) -> int:
        return self.inflection_high & 0xF8

    @property
    def pitch_inflection(self) -> int:
        if self.response_mode != MODE_PHONEME_TRANSITIONED:
            return self.inflection
        return (
            ((self.rate_inflection & 0x08) << 8)
            | ((self.transitioned_inflection & 0xFF) << 3)
            | (self.rate_inflection & 0x07)
        )

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

        register &= 7
        pw0_set = (
            register == 0
            and self.selector_latch_level
            and self.selector == 0
            and self.u38_equal(0)
        )
        pw1_set = (
            register == 0
            and self.selector_latch_level
            and self.selector == 1
            and self.u38_equal(1)
        )
        self._write_active = True
        self._write_register = register
        self._write_data = data & 0xFF
        if self._write_register == 0:
            # U29A falls as the decoded phone-write condition asserts.
            self._update_u29a(duration_edge=False)
            self.pw_0 = False
            self.pw_1 = False
            if pw0_set:
                self.pw_0 = True
            if pw1_set:
                self.pw_1 = True

    def end_write(self) -> None:
        """Latch the address and data when the active write condition ends."""

        if not self._write_active:
            return
        self._write_active = False
        self._update_u29a(duration_edge=False)
        self._latch_write(self._write_register, self._write_data)

    def write(self, register: int, data: int) -> None:
        self.begin_write(register, data)
        self.end_write()

    def _acknowledge(self) -> None:
        self.d7_pending = False

    def _clock_u37(self) -> None:
        """Apply one falling U29A edge to the sheet-5 MC14040."""

        if self.d7_pending and self.u37_count == 0:
            return
        self.u37_count = (self.u37_count + 1) & 0x0F

    def _update_u29a(self, *, duration_edge: bool) -> None:
        phone_write_active = (
            self._write_active and self._write_register == 0
        )
        new_level = not (duration_edge or phone_write_active)
        if self.u29a_level and not new_level:
            self._clock_u37()
        self.u29a_level = new_level

    def u38_equal(self, selector: int | None = None) -> bool:
        flags = self.flags_for_selector(selector)
        return self.u37_count == (2 if flags & 0x01 else 6)

    def _latch_write(self, register: int, data: int) -> None:
        if self.revision_ap and self.pd_rst_asserted:
            return

        if register <= 2 or (register == 3 and (data & 0x80)):
            self._acknowledge()

        if register == 0:
            self.duration_phoneme = data
            self.phone_setup_pending = True
            if not self.powered_down:
                self._start_or_replace_phone()
        elif register == 1:
            self.inflection_high = data
        elif register == 2:
            self.rate_inflection = data
        elif register == 3:
            old_control = self.control_articulation_amplitude
            self.control_articulation_amplitude = data
            self.control_setup_pending = True
            self.u166b_nq = False
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
        self.u183a_q = True
        if self.revision_ap:
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
        for _ in range(ticks):
            self._advance_one_effective_tick()

    def _advance_one_effective_tick(
        self,
        *,
        suppress_slot_write: bool = False,
    ) -> None:
        self.effective_xck_ticks += 1

        if self.phone_active and self.ticks_to_boundary == 0:
            self.ticks_to_boundary = self._boundary_period()
        duration_edge_now = (
            self.phone_active and self.ticks_to_boundary == 1
        )
        if duration_edge_now:
            # U169 keeps DUREDGE until U93 samples it at SEL2. Setting the
            # pending bit before the selector tick also covers a same-tick
            # DUREDGE/SEL2 collision.
            self.duration_edge_pending = True

        self._advance_filter_clock(1)
        self._advance_voice_clock()
        self._advance_selector_tick(suppress_slot_write=suppress_slot_write)
        self._update_amplitude_counter()
        self._update_u41c_edge()

        # U29A, U37, U183A, and the selected LATCH clocks are parallel
        # physical edges. Keep the old U37/U183 state visible to latch work
        # above, then apply their edge updates.
        self._update_u29a(duration_edge=duration_edge_now)
        if self.pd_rst_asserted:
            self.u183a_q = True
        else:
            self.u183a_q = bool(
                self.control_articulation_amplitude & 0x80
            )

        if not self.phone_active:
            return
        self.ticks_to_boundary -= 1
        if self.ticks_to_boundary == 0:
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

        write_commit = write_end is not None and not (
            self.revision_ap and (assert_pd_rst or self.pd_rst_asserted)
        )
        self._advance_one_effective_tick(suppress_slot_write=write_commit)
        if write_commit and write_end is not None:
            self._latch_write(write_end[0] & 7, write_end[1] & 0xFF)
        if assert_pd_rst:
            self.assert_pd_rst()

    def _advance_voice_clock(self) -> None:
        if self.u62_reset:
            self.u62_q = False
        self.voice_ticks_to_edge -= 1
        if self.voice_ticks_to_edge:
            return
        self.voice_clock_edges += 1
        if not self.u62_reset:
            self.u62_q = not self.u62_q
        if not self.u62_reset and self.u62_q:
            self.pitch_events += 1
            self.last_pitch_event_tick = self.effective_xck_ticks
        self.voice_ticks_to_edge = voice_clock_period_ticks(
            self.pitch_inflection
        )

    def _update_u41c_edge(self) -> None:
        # U104C.8 is U62 /Q. U72A inverts PW3 & /Q; U42D is the
        # NOR of SEL1 and FRIC_AMP_ZERO; U41C clocks U75 and U73.
        level = (
            not (self.pw_3 and not self.u62_q)
            and not bool(self.selector & 0x02)
            and self.parameter_values["fric_amp"] != 0
        )
        if level and not self.u41c_level:
            self.noise_clock_edges += 1
        self.u41c_level = level

    @property
    def ampct_zero(self) -> bool:
        return (self.ampct_count & 0x0E) == 0

    @property
    def ampct_up(self) -> bool:
        ampct0 = not (self.pw_3 and not self.u62_q)
        any_amplitude = (
            self.parameter_values["voice_amp"] != 0
            or self.parameter_values["fric_amp"] != 0
        )
        return ampct0 and any_amplitude

    @property
    def ampct_nco(self) -> bool:
        if self.ampct_up:
            return self.ampct_count != 15
        return self.ampct_count != 0

    @property
    def u62_reset(self) -> bool:
        u104c = self.pw_3 and not self.u62_q
        return u104c or self.ampct_nco

    def _update_amplitude_counter(self) -> None:
        u71c = (not self.ampct_nco) and self.ampct_up
        u71d = (not self.ampct_up) and self.ampct_zero
        u69a = not (u71c or u71d)
        level = bool(self.selector & 0x04) and u69a
        if level and not self.u68_clock_level:
            if self.ampct_up:
                self.ampct_count = (self.ampct_count + 1) & 0x0F
            else:
                self.ampct_count = (self.ampct_count - 1) & 0x0F
        self.u68_clock_level = level

    def _advance_filter_clock(self, ticks: int) -> None:
        remaining = ticks
        while remaining >= self.filter_ticks_to_toggle:
            remaining -= self.filter_ticks_to_toggle
            self.filter_phase ^= 1
            self.filter_phase_edges += 1
            if self.filter_phase == 0:
                self.closure_events += 1
                self.last_closure_tick = self.effective_xck_ticks
                self.fric2_sw = not self.u20b_q
            self.filter_ticks_to_toggle = 256 - self.filter_frequency
        if self.filter_phase == 1:
            # U112 is transparent while Phi1_X is high.
            self.fric1_sw = self.u20b_q
        self.filter_ticks_to_toggle -= remaining

    def rom_byte(self, selector: int | None = None) -> int:
        active_selector = self.selector if selector is None else selector
        return self.rom[rom_address(self.phoneme, active_selector)]

    def target_for_selector(self, selector: int | None = None) -> int:
        active_selector = self.selector if selector is None else selector & 7
        if active_selector == 4:
            return self.amplitude
        if active_selector in (5, 6) and self.amplitude == 0:
            # U79B/U79C and U78A-D force both source-amplitude targets low
            # when AMPZERO is true.
            return 0
        return self.rom_byte(active_selector) >> 4

    def flags_for_selector(self, selector: int | None = None) -> int:
        return self.rom_byte(selector) & 0x0F

    @property
    def selector_latch_level(self) -> bool:
        return self.selector_subphase == 2 and self.selector_fast_phase >= 2

    @property
    def selected_rate_clock(self) -> bool:
        if self.duration_phoneme & 0x80:
            return bool(self.rate_clock)
        return bool(self.rate_clock_div2)

    @property
    def u32b_write_gate(self) -> bool:
        source_active = (
            bool(self.duration_phoneme & 0x20)
            or self.parameter_values["voice_amp"] != 0
            or self.parameter_values["fric_amp"] != 0
        )
        return not (self.pw_5 and source_active)

    def u96_write_permit(self, selector: int | None = None) -> bool:
        """Return the prototype U96 output for one selector address."""

        active_selector = self.selector if selector is None else selector & 7
        if active_selector in (0, 1, 3):
            return self.tc_edge_window and self.u32b_write_gate
        if active_selector == 2:
            return self.tc_edge_window
        if active_selector == 4:
            return (
                self.duration_edge_window
                and self.u166b_nq
                and self.amplitude != 0
            )
        rate_edge = self.u93_rate_q1 and not self.u93_rate_q2
        if active_selector == 5:
            return self.pw_0 and rate_edge
        if active_selector == 6:
            return self.pw_1 and rate_edge
        return False

    def transition_a_clr(self, selector: int | None = None) -> bool:
        """Return the sheet-4 A_CLR setup condition for an address."""

        active_selector = self.selector if selector is None else selector & 7
        phone_setup = (
            self.phone_setup_window and active_selector != 4
        )
        control_setup = self.control_setup_window and (
            (active_selector == 4 and self.amplitude != 0)
            or active_selector in (5, 6)
        )
        return phone_setup or control_setup

    def _selector_write_event(self, *, suppress_write: bool = False) -> None:
        """Clock the selected U87-U90 DDA state on the WRITE edge."""

        if suppress_write:
            return

        selector = self.selector & 7
        target = self.target_for_selector(selector) & 0x0F
        resa = self.parameter_resa[selector] & 0x0F

        if self.transition_a_clr(selector):
            self.parameter_resb[selector] = (target - resa) & 0x0F
            self.parameter_resc[selector] = 8
            self.parameter_rescy[selector] = target >= resa
            return

        if (
            self.phone_setup_pending
            or not self.u96_write_permit(selector)
            or resa == target
        ):
            return

        total = (
            (self.parameter_resb[selector] & 0x0F)
            + (self.parameter_resc[selector] & 0x0F)
        )
        carry = bool(total & 0x10)
        self.parameter_resc[selector] = total & 0x0F
        if self.parameter_rescy[selector] and carry:
            self.parameter_resa[selector] = (resa + 1) & 0x0F
        elif not self.parameter_rescy[selector] and not carry:
            self.parameter_resa[selector] = (resa - 1) & 0x0F
        self.parameter_write_log.append(
            (self.effective_xck_ticks, selector)
        )

    def _latch_selected_parameter(self, selector: int) -> None:
        value = self.parameter_resa[selector] & 0x0F
        if selector == 0:
            self.parameter_values["f1"] = value
        elif selector == 1:
            self.parameter_values["f2"] = value
        elif selector == 2:
            self.parameter_values["f2_res"] = value
        elif selector == 3:
            self.parameter_values["f3"] = value
            self.parameter_values["f4"] = value
        elif selector == 4:
            self.parameter_values["filter_amp"] = value
        elif selector == 5:
            self.parameter_values["voice_amp"] = value
        elif selector == 6:
            self.parameter_values["fric_amp"] = value

    def _selector_latch_event(self, *, suppress_control_latch: bool) -> None:
        selector = self.selector & 7
        self._latch_selected_parameter(selector)
        if suppress_control_latch:
            return

        flags = self.flags_for_selector(selector)
        if selector == 0:
            if self.u38_equal(selector):
                self.pw_0 = True
        elif selector == 1:
            if self.u38_equal(selector):
                self.pw_1 = True
        elif selector == 2:
            if self.u38_equal(selector):
                self.pw_3 = (
                    self.u183a_q
                    or not bool(flags & 0x02)
                )
            u20_clock_enable = (
                (
                    (self.pw_0 and self.pw_1 and self.ampct_zero)
                    or self.parameter_values["fric_amp"] == 0
                )
                and self.pw_1
                and (self.pw_2 or bool(flags & 0x04))
            )
            if u20_clock_enable:
                self.u20b_q = bool(flags & 0x08)
            self.pw_2 = bool(flags & 0x04)
            self.pw_5 = not self.pw_2

    def advance_selector_slow_edge(
        self,
        *,
        suppress_slot_write: bool = False,
    ) -> None:
        """Advance one of the four SLOWCLK edges in a selector slot."""

        # This public helper advances a complete quarter-slot without the
        # intervening FASTCLK calls. Reproduce the r=2 WRITE and r=10 LATCH
        # events before their respective SLOWCLK edges.
        if self.selector_subphase == 0:
            self._selector_write_event(suppress_write=suppress_slot_write)
        elif self.selector_subphase == 2:
            self._selector_latch_event(
                suppress_control_latch=suppress_slot_write
            )
        self.selector_subphase += 1
        if self.selector_subphase == SELECTOR_SLOW_EDGES:
            self.selector_subphase = 0
            self._complete_selector_slot(
                suppress_slot_write=suppress_slot_write
            )

    def transition_step(self) -> None:
        """Advance one full selector slot for vector generation."""

        for _ in range(SELECTOR_SLOW_EDGES):
            self.advance_selector_slow_edge()

    def _advance_selector_tick(self, *, suppress_slot_write: bool) -> None:
        self.selector_fast_phase += 1
        if self.selector_fast_phase == 2:
            if self.selector_subphase == 0:
                self._selector_write_event(
                    suppress_write=suppress_slot_write
                )
            elif self.selector_subphase == 2:
                self._selector_latch_event(
                    suppress_control_latch=suppress_slot_write
                )
        if self.selector_fast_phase == SLOWCLOCK_FAST_TICKS:
            self.selector_fast_phase = 0
            self.selector_subphase += 1
            if self.selector_subphase == SELECTOR_SLOW_EDGES:
                self.selector_subphase = 0
                self._complete_selector_slot(
                    suppress_slot_write=suppress_slot_write
                )

    def _complete_selector_slot(self, *, suppress_slot_write: bool) -> None:
        selector = self.selector & 7

        # U93/U169 sample at the positive SEL2 boundary, which is the 3->4
        # selector transition. Each sampled level remains valid for one scan.
        if selector == 3:
            self.phone_setup_window = self.phone_setup_pending
            self.control_setup_window = self.control_setup_pending
            self.tc_edge_window = self.tc_edge_pending
            self.duration_edge_window = self.duration_edge_pending
            self.phone_setup_pending = False
            self.control_setup_pending = False
            self.tc_edge_pending = False
            self.duration_edge_pending = False
            self.u93_rate_q2 = self.u93_rate_q1
            self.u93_rate_q1 = self.selected_rate_clock
            if self.control_setup_window:
                self.u166b_nq = True

        self.selector = (selector + 1) & 7
        self.selector_steps += 1

        # SEL1 rises as the three-bit selector moves 1->2 and 5->6.
        if selector in (1, 5):
            self._advance_rate_edge()

    def _advance_rate_edge(self) -> None:
        self.rate_edges_left -= 1
        if self.rate_edges_left:
            return
        self.rate_edges_left = 16 - self.rate
        old_rate_clock = self.rate_clock
        self.rate_clock ^= 1
        if old_rate_clock:
            return

        self.rate_clock_rises += 1
        self.inflection_edges_left -= 1
        if self.inflection_edges_left == 0:
            self.inflection_edges_left = 8 - (self.inflection_high & 0x07)
            target = self.transitioned_inflection_target
            next_value = move_one_toward_byte(
                self.transitioned_inflection, target
            )
            if next_value != self.transitioned_inflection:
                self.transitioned_inflection = next_value
                self.inflection_steps += 1
                self.last_inflection_tick = self.effective_xck_ticks

        old_div2 = self.rate_clock_div2
        self.rate_clock_div2 ^= 1
        if old_div2:
            return

        self.rate_clock_div2_rises += 1
        self.articulation_edges_left -= 1
        if self.articulation_edges_left == 0:
            self.articulation_edges_left = 8 - self.articulation
            self.articulation_steps += 1
            self.last_articulation_tick = self.effective_xck_ticks
            self.tc_edge_pending = True


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
