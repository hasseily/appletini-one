#!/usr/bin/env python3
"""Test the native SSI-263 / SC-02 ROM and cycle reference."""

from __future__ import annotations

import unittest
from fractions import Fraction

from ssi263_sc02_reference import (
    MODE_FRAME_IMMEDIATE,
    MODE_PHONEME_TRANSITIONED,
    ROM_ACTIVE_SIZE,
    SELECTOR_SLOW_EDGES,
    SSI263Reference,
    articulation_step_ticks,
    filter_period_ticks,
    frame_ticks,
    inflection_step_ticks,
    inflection_word,
    load_active_rom,
    phoneme_ticks,
    pitch_period_ticks,
    rom_address,
    verify_rom,
    voice_clock_period_ticks,
)


class RomTests(unittest.TestCase):
    def test_hash_and_addressing(self) -> None:
        rom = load_active_rom()
        self.assertEqual(len(rom), ROM_ACTIVE_SIZE)
        verify_rom(rom)
        for phone in range(64):
            for selector in range(8):
                self.assertEqual(rom_address(phone, selector), phone * 8 + selector)

    def test_special_columns(self) -> None:
        rom = load_active_rom()
        self.assertTrue(all(rom[rom_address(phone, 4)] == 0 for phone in range(64)))
        self.assertTrue(all(rom[rom_address(phone, 7)] == 0 for phone in range(64)))
        fricative_phones = {
            phone
            for phone in range(64)
            if (rom[rom_address(phone, 6)] >> 4) != 0
        }
        self.assertEqual(
            fricative_phones,
            {0x27, 0x28, 0x29, 0x2C, 0x2D, 0x2F, 0x30,
             0x31, 0x32, 0x33, 0x34, 0x35, 0x36},
        )


class FormulaTests(unittest.TestCase):
    def test_register_and_timing_formulas(self) -> None:
        self.assertEqual(inflection_word(0x50, 0x08), 0xA80)
        self.assertEqual(voice_clock_period_ticks(0xA80), 0x1600)
        self.assertEqual(pitch_period_ticks(0xA80), 0x2C00)
        self.assertEqual(frame_ticks(0x0A), 4096 * 6)
        self.assertEqual(phoneme_ticks(0x0A, 3), 4096 * 6)
        self.assertEqual(phoneme_ticks(0x0A, 0), 4096 * 6 * 4)
        self.assertEqual(filter_period_ticks(0x00), 512)
        self.assertEqual(filter_period_ticks(0xFF), 2)
        self.assertEqual(articulation_step_ticks(0x0F, 7), 256)
        self.assertEqual(inflection_step_ticks(0x0F, 7), 128)

    def test_two_xck_edges_with_div2_make_one_effective_tick(self) -> None:
        chip = SSI263Reference(xck_edges_per_bus_cycle=2, div2=True)
        chip.feed_bus_cycles(1234)
        self.assertEqual(chip.xck_pin_edges, 2468)
        self.assertEqual(chip.effective_xck_ticks, 1234)

    def test_phasor_q3_pitch_profile(self) -> None:
        raw_xck = Fraction(14_318_180, 7)
        effective_xck = raw_xck / 2
        old_effective_xck = Fraction(3_579_545, 4)
        pitch_hz = effective_xck / pitch_period_ticks(0xA80)

        self.assertEqual(effective_xck / old_effective_xck, Fraction(8, 7))
        self.assertGreater(float(pitch_hz), 90.0)
        self.assertLess(float(pitch_hz), 91.0)


class InterfaceTests(unittest.TestCase):
    def wake(self, chip: SSI263Reference, duration_phoneme: int = 0xC0) -> None:
        chip.write(0, duration_phoneme)
        chip.write(3, 0x00)

    def test_write_latches_on_end(self) -> None:
        chip = SSI263Reference()
        chip.begin_write(4, 0xE9)
        self.assertEqual(chip.filter_frequency, 0xFF)
        chip.end_write()
        self.assertEqual(chip.filter_frequency, 0xE9)

    def test_acknowledgment_rules(self) -> None:
        for register, data, clears in (
            (0, 0xC0, True),
            (1, 0x00, True),
            (2, 0xA0, True),
            (3, 0x00, False),
            (3, 0x80, True),
            (4, 0xE9, False),
            (7, 0xE9, False),
        ):
            with self.subTest(register=register, data=data):
                chip = SSI263Reference()
                chip.d7_pending = True
                chip.write(register, data)
                self.assertEqual(chip.d7_pending, not clears)

    def test_acknowledgment_wins_on_completion_edge(self) -> None:
        chip = SSI263Reference()
        chip.write(2, 0xF0)
        self.wake(chip, 0x40)
        chip.advance_effective_ticks(frame_ticks(0x0F) - 1)
        chip.step_effective_tick(write_end=(1, 0x00))
        self.assertEqual(chip.completed_boundaries, 1)
        self.assertEqual(chip.read_d7(), 0)

    def test_host_write_consumes_colliding_parameter_slot(self) -> None:
        chip = SSI263Reference(div2=False)
        chip.parameter_sweep_mask = 0x01
        chip.selector_subphase = SELECTOR_SLOW_EDGES - 1
        chip.selector_fast_phase = 3
        chip.step_effective_tick(write_end=(0, 0x01))
        self.assertEqual(chip.selector, 1)
        self.assertEqual(chip.parameter_sweep_mask & 0x01, 0)
        self.assertEqual(chip.parameter_values["f1"], 0)
        self.assertEqual(chip.duration_phoneme, 0x01)

    def test_phoneme_timing_and_continuous_repeat(self) -> None:
        chip = SSI263Reference()
        chip.write(2, 0xA0)
        self.wake(chip, 0xC1)
        period = phoneme_ticks(0x0A, 3)
        chip.advance_effective_ticks(period - 1)
        self.assertEqual(chip.read_d7(), 0)
        chip.advance_effective_ticks(1)
        self.assertEqual(chip.read_d7(), 1)
        self.assertTrue(chip.ar_drive_low)
        self.assertEqual(chip.completed_boundaries, 1)

        chip.advance_effective_ticks(period)
        self.assertEqual(chip.completed_boundaries, 2)
        self.assertEqual(chip.read_d7(), 1)

        chip.write(1, 0x00)
        self.assertEqual(chip.read_d7(), 0)
        self.assertTrue(chip.phone_active)
        chip.advance_effective_ticks(period)
        self.assertEqual(chip.read_d7(), 1)
        self.assertEqual(chip.completed_boundaries, 3)

    def test_dr_zero_sets_d7_but_blocks_ar(self) -> None:
        chip = SSI263Reference()
        chip.write(2, 0xF0)
        self.wake(chip, 0x40)
        self.assertEqual(chip.response_mode, MODE_FRAME_IMMEDIATE)
        chip.write(3, 0x80)
        chip.write(0, 0x00)
        chip.write(3, 0x00)
        self.assertEqual(chip.response_mode, MODE_FRAME_IMMEDIATE)
        self.assertFalse(chip.ar_enabled)
        chip.advance_effective_ticks(frame_ticks(0x0F))
        self.assertEqual(chip.read_d7(), 1)
        self.assertFalse(chip.ar_drive_low)

    def test_pd_rst_retains_non_control_registers(self) -> None:
        chip = SSI263Reference()
        chip.write(0, 0xC5)
        chip.write(1, 0x50)
        chip.write(2, 0xA8)
        chip.write(4, 0xE9)
        chip.assert_pd_rst()
        self.assertEqual(chip.duration_phoneme, 0xC5)
        self.assertEqual(chip.inflection_high, 0x50)
        self.assertEqual(chip.rate_inflection, 0xA8)
        self.assertEqual(chip.filter_frequency, 0xE9)
        self.assertTrue(chip.powered_down)
        self.assertFalse(chip.ar_drive_low)

    def test_pd_rst_owns_a_colliding_or_held_ap_write(self) -> None:
        chip = SSI263Reference(revision_ap=True, div2=False)
        chip.inflection_high = 0x25
        chip.step_effective_tick(
            write_end=(1, 0xAA), assert_pd_rst=True
        )
        self.assertEqual(chip.inflection_high, 0x25)
        self.assertTrue(chip.powered_down)

        chip.write(1, 0x55)
        self.assertEqual(chip.inflection_high, 0x25)

        faulty_p = SSI263Reference(revision_ap=False, div2=False)
        faulty_p.inflection_high = 0x25
        faulty_p.control_articulation_amplitude = 0x00
        faulty_p.step_effective_tick(
            write_end=(1, 0xAA), assert_pd_rst=True
        )
        self.assertEqual(faulty_p.inflection_high, 0xAA)
        self.assertFalse(faulty_p.powered_down)

    def test_voice_and_glottal_cadence(self) -> None:
        chip = SSI263Reference(div2=False)
        chip.write(0, 0x80)
        chip.write(1, 0xFF)
        chip.write(2, 0x0F)
        chip.write(3, 0x00)
        chip.voice_ticks_to_edge = voice_clock_period_ticks(
            chip.pitch_inflection
        )
        chip.advance_effective_ticks(20)
        self.assertEqual(chip.voice_clock_edges, 5)
        self.assertEqual(chip.pitch_events, 3)

        first = chip.last_pitch_event_tick
        chip.advance_effective_ticks(8)
        self.assertEqual(chip.last_pitch_event_tick - first, 8)

    def test_held_pw3_produces_sustained_u41c_edges(self) -> None:
        chip = SSI263Reference(div2=False)
        chip.write(0, 0x80)
        chip.write(1, 0xFF)
        chip.write(2, 0x0F)
        chip.write(3, 0x00)
        chip.pw_3 = True
        chip.parameter_values["fric_amp"] = 1
        chip.voice_ticks_to_edge = voice_clock_period_ticks(
            chip.pitch_inflection
        )
        chip.advance_effective_ticks(24)
        self.assertTrue(chip.pw_3)
        self.assertGreaterEqual(chip.noise_clock_edges, 2)

    def test_closure_is_u49_terminal_gated_by_filter_phase(self) -> None:
        chip = SSI263Reference(div2=False)
        chip.write(4, 0xFF)
        chip.advance_effective_ticks(6)
        self.assertEqual(chip.filter_phase_edges, 6)
        self.assertEqual(chip.closure_events, 3)
        self.assertEqual(chip.last_closure_tick, 6)

    def test_filter_write_preserves_current_divider_count(self) -> None:
        chip = SSI263Reference()
        chip.write(4, 0x00)
        chip.filter_ticks_to_toggle = 123
        chip.write(4, 0xFF)
        self.assertEqual(chip.filter_ticks_to_toggle, 123)
        chip.advance_effective_ticks(123)
        self.assertEqual(chip.filter_ticks_to_toggle, 1)

    def test_power_down_keeps_free_digital_state_running(self) -> None:
        chip = SSI263Reference(div2=False)
        chip.write(0, 0x00)
        chip.write(1, 0xFF)
        chip.write(2, 0xF0)
        chip.write(4, 0xFF)
        self.assertTrue(chip.powered_down)

        chip.advance_effective_ticks(4097)
        self.assertGreater(chip.selector_steps, 0)
        self.assertGreater(chip.filter_phase_edges, 0)
        self.assertGreater(chip.transitioned_inflection, 0)
        self.assertGreater(chip.articulation_steps, 0)
        self.assertEqual(chip.duration_phoneme, 0x00)
        self.assertEqual(chip.inflection_high, 0xFF)
        self.assertEqual(chip.rate_inflection, 0xF0)

        state = chip.transitioned_inflection
        steps = chip.selector_steps
        chip.write(3, 0x00)
        self.assertFalse(chip.powered_down)
        self.assertEqual(chip.transitioned_inflection, state)
        self.assertEqual(chip.selector_steps, steps)
        chip.advance_effective_ticks(128)
        self.assertGreater(chip.selector_steps, steps)


class SelectorTests(unittest.TestCase):
    def test_selector_three_updates_f3_and_f4(self) -> None:
        chip = SSI263Reference()
        chip.duration_phoneme = 0x00
        chip.selector = 3
        target = chip.target_for_selector()
        chip.parameter_sweep_mask = 1 << 3
        chip.transition_step()
        self.assertEqual(chip.parameter_values["f3"], min(target, 1))
        self.assertEqual(chip.parameter_values["f4"], min(target, 1))
        self.assertEqual(chip.selector, 4)

    def test_selector_holds_for_four_slow_edges(self) -> None:
        chip = SSI263Reference()
        start = chip.selector
        for edge in range(SELECTOR_SLOW_EDGES - 1):
            chip.advance_selector_slow_edge()
            self.assertEqual(chip.selector, start)
        chip.advance_selector_slow_edge()
        self.assertEqual(chip.selector, start + 1)

    def test_selector_four_uses_host_amplitude(self) -> None:
        chip = SSI263Reference()
        chip.control_articulation_amplitude = 0x0C
        chip.selector = 4
        self.assertEqual(chip.rom_byte(), 0)
        self.assertEqual(chip.target_for_selector(), 0x0C)
        chip.parameter_sweep_mask = 1 << 4
        chip.transition_step()
        self.assertEqual(chip.parameter_values["filter_amp"], 1)

    def test_selector_seven_is_idle(self) -> None:
        chip = SSI263Reference()
        chip.selector = 7
        chip.parameter_values["f4"] = 9
        chip.transition_step()
        self.assertEqual(chip.parameter_values["f4"], 9)
        self.assertEqual(chip.selector, 0)

    def test_slots_do_not_move_without_an_articulation_sweep(self) -> None:
        chip = SSI263Reference()
        chip.duration_phoneme = 0x00
        target = chip.target_for_selector(0)
        self.assertGreater(target, 0)
        chip.transition_step()
        self.assertEqual(chip.parameter_values["f1"], 0)

    def test_all_observed_lower_rom_codes(self) -> None:
        observed = {
            selector: {
                self.rom_nibble(phone, selector)
                for phone in range(64)
            }
            for selector in range(8)
        }
        self.assertEqual(observed[0], {0x0, 0x1})
        self.assertEqual(observed[1], {0x0, 0x1})
        self.assertEqual(observed[2], {0x4, 0x6, 0x8, 0xA, 0xC, 0xE})
        self.assertTrue(all(observed[index] == {0} for index in range(3, 8)))

        for phone, code in (
            (0x28, 0x4),
            (0x2F, 0x6),
            (0x2B, 0x8),
            (0x00, 0xA),
            (0x24, 0xC),
            (0x01, 0xE),
        ):
            with self.subTest(phone=phone, code=code):
                chip = SSI263Reference(div2=False)
                chip.duration_phoneme = phone
                chip.advance_effective_ticks(48)
                self.assertEqual(chip.pw_2, bool(code & 0x4))
                self.assertEqual(chip.pw_3, bool(code & 0x2))
                self.assertEqual(chip.pw_5, not bool(code & 0x4))
                self.assertEqual(chip.fric1_sw, bool(code & 0x8))
                self.assertEqual(chip.fric2_sw, not bool(code & 0x8))

        chip = SSI263Reference(div2=False)
        chip.duration_phoneme = 0x27
        chip.advance_effective_ticks(32)
        self.assertTrue(chip.phone_fricative)
        self.assertFalse(chip.phone_voiced)
        chip = SSI263Reference(div2=False)
        chip.duration_phoneme = 0x01
        chip.advance_effective_ticks(32)
        self.assertFalse(chip.phone_fricative)
        self.assertTrue(chip.phone_voiced)

        chip = SSI263Reference(div2=False)
        chip.duration_phoneme = 0x2F
        chip.advance_effective_ticks(32)
        self.assertFalse(chip.pw_0)
        self.assertFalse(chip.pw_1)
        self.assertTrue(chip.phone_fricative)
        self.assertTrue(chip.phone_voiced)

        for phone in range(64):
            with self.subTest(phone=phone):
                chip = SSI263Reference(div2=False)
                chip.duration_phoneme = phone
                chip.advance_effective_ticks(32)
                self.assertEqual(
                    chip.phone_voiced,
                    not bool(chip.rom[rom_address(phone, 0)] & 0x01),
                )
                self.assertEqual(
                    chip.phone_fricative,
                    not bool(chip.rom[rom_address(phone, 1)] & 0x01),
                )

    @staticmethod
    def rom_nibble(phone: int, selector: int) -> int:
        chip = SSI263Reference()
        return chip.rom[rom_address(phone, selector)] & 0x0F


class TransitionTimingTests(unittest.TestCase):
    @staticmethod
    def wait_for(chip: SSI263Reference, field: str, count: int) -> int:
        while getattr(chip, field) < count:
            chip.advance_effective_ticks(1)
        return chip.effective_xck_ticks

    def test_every_articulation_setting_has_exact_steady_pace(self) -> None:
        for setting in range(8):
            with self.subTest(articulation=setting):
                chip = SSI263Reference(div2=False)
                chip.write(2, 0xF0)
                chip.write(3, 0x80 | (setting << 4))
                self.wait_for(chip, "articulation_steps", 1)
                first = chip.last_articulation_tick
                self.wait_for(chip, "articulation_steps", 2)
                self.assertEqual(
                    chip.last_articulation_tick - first,
                    articulation_step_ticks(0x0F, setting),
                )

    def test_every_inflection_slope_has_exact_steady_pace(self) -> None:
        for slope in range(8):
            with self.subTest(slope=slope):
                chip = SSI263Reference(div2=False)
                chip.write(1, 0xF8 | slope)
                chip.write(2, 0xF0)
                self.wait_for(chip, "inflection_steps", 1)
                first = chip.last_inflection_tick
                first_value = chip.transitioned_inflection
                self.wait_for(chip, "inflection_steps", 2)
                self.assertEqual(
                    chip.last_inflection_tick - first,
                    inflection_step_ticks(0x0F, slope),
                )
                self.assertEqual(
                    chip.transitioned_inflection,
                    first_value + 1,
                )

    def test_transitioned_pitch_uses_transition_state(self) -> None:
        chip = SSI263Reference(div2=False)
        chip.write(1, 0xF8 | 7)
        chip.write(2, 0xF5)
        self.wait_for(chip, "inflection_steps", 1)
        chip.write(0, 0xC0)
        chip.write(3, 0x00)
        self.assertEqual(chip.response_mode, MODE_PHONEME_TRANSITIONED)
        self.assertEqual(
            (chip.pitch_inflection >> 3) & 0xFF,
            chip.transitioned_inflection,
        )
        self.assertEqual(chip.pitch_inflection & 0x807, 0x005)


if __name__ == "__main__":
    unittest.main(verbosity=2)
