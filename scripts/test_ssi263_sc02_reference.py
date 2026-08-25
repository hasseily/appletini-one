#!/usr/bin/env python3
"""Test the native SSI-263 / SC-02 ROM and cycle reference."""

from __future__ import annotations

import unittest
from fractions import Fraction
from hashlib import sha256

from ssi263_sc02_reference import (
    MODE_FRAME_IMMEDIATE,
    MODE_PHONEME_IMMEDIATE,
    MODE_PHONEME_TRANSITIONED,
    ROM_ACTIVE_SHA256,
    ROM_ACTIVE_SIZE,
    SELECTOR_SLOW_EDGES,
    SSI263Reference,
    articulation_step_ticks,
    duration_clock_period_ticks,
    filter_period_ticks,
    frame_ticks,
    inflection_step_ticks,
    inflection_word,
    load_active_rom,
    phoneme_ticks,
    pitch_period_ticks,
    reconstructed_source_image,
    rom_address,
    verify_rom,
    voice_clock_period_ticks,
)


class RomTests(unittest.TestCase):
    def test_hash_and_addressing(self) -> None:
        rom = load_active_rom()
        self.assertEqual(len(rom), ROM_ACTIVE_SIZE)
        verify_rom(rom)
        self.assertEqual(
            sha256(bytes(rom)).hexdigest(),
            ROM_ACTIVE_SHA256,
        )
        image = reconstructed_source_image(rom)
        self.assertEqual(image[:ROM_ACTIVE_SIZE], bytes(rom))
        self.assertEqual(set(image[ROM_ACTIVE_SIZE:]), {0})
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
        self.assertEqual(duration_clock_period_ticks(0x0A, 0), 256 * 6 * 4)
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


class DurationClockTests(unittest.TestCase):
    @staticmethod
    def configured_chip(rate: int, duration: int) -> SSI263Reference:
        return SSI263Reference(
            div2=False,
            duration_phoneme=(duration << 6),
            rate_inflection=(rate << 4),
            control_articulation_amplitude=0,
        )

    @staticmethod
    def wait_for_duration_rise(chip: SSI263Reference) -> int:
        target = chip.duration_clock_rises + 1
        while chip.duration_clock_rises < target:
            chip.advance_effective_ticks(1)
        assert chip.last_duration_clock_tick is not None
        return chip.last_duration_clock_tick

    @staticmethod
    def prime_duration_rise_next_tick(chip: SSI263Reference) -> None:
        # Complete selector slot 1 on the next effective tick. Its SEL1 rise
        # clocks RATECLK high, U21B Q high, and exposes terminal U28 as DURCLK.
        chip.selector = 1
        chip.selector_subphase = 3
        chip.selector_fast_phase = 3
        chip.rate_edges_left = 1
        chip.rate_clock = 0
        chip.rate_clock_div2 = 0
        chip.u28_count = 15
        chip.duration_clock = False
        chip.u29a_level = not (
            chip._write_active and chip._write_register == 0
        )

    def test_all_rate_duration_pairs_have_exact_u28_cadence(self) -> None:
        for rate in range(16):
            for duration in range(4):
                with self.subTest(rate=rate, duration=duration):
                    chip = self.configured_chip(rate, duration)
                    first = self.wait_for_duration_rise(chip)
                    second = self.wait_for_duration_rise(chip)
                    expected = duration_clock_period_ticks(rate, duration)
                    self.assertEqual(second - first, expected)
                    self.assertEqual(phoneme_ticks(rate, duration), 16 * expected)

    def test_every_full_phoneme_contains_sixteen_durclk_rises(self) -> None:
        for rate in range(16):
            for duration in range(4):
                with self.subTest(rate=rate, duration=duration):
                    chip = self.configured_chip(rate, duration)
                    self.wait_for_duration_rise(chip)
                    rise_base = chip.duration_clock_rises
                    chip.response_mode = MODE_PHONEME_TRANSITIONED
                    chip.u37_count = 0
                    chip.d7_pending = False
                    chip.completed_boundaries = 0

                    chip.advance_effective_ticks(
                        phoneme_ticks(rate, duration) - 1
                    )
                    self.assertEqual(chip.duration_clock_rises - rise_base, 15)
                    self.assertEqual(chip.u37_count, 15)
                    self.assertEqual(chip.completed_boundaries, 0)
                    self.assertFalse(chip.d7_pending)

                    chip.advance_effective_ticks(1)
                    self.assertEqual(chip.duration_clock_rises - rise_base, 16)
                    self.assertEqual(chip.u37_count, 0)
                    self.assertEqual(chip.completed_boundaries, 1)
                    self.assertTrue(chip.d7_pending)

    def test_frame_response_is_separate_from_u28_phoneme_cadence(self) -> None:
        rate = 0x0F
        for duration in range(4):
            with self.subTest(duration=duration):
                chip = self.configured_chip(rate, duration)
                self.wait_for_duration_rise(chip)
                rise_base = chip.duration_clock_rises
                chip.response_mode = MODE_FRAME_IMMEDIATE
                chip.u36_count = 0
                chip.d7_pending = False
                chip.completed_boundaries = 0
                chip.u36_eq15_entries = 0

                chip.advance_effective_ticks(phoneme_ticks(rate, duration))

                self.assertEqual(chip.duration_clock_rises - rise_base, 16)
                self.assertEqual(chip.u36_eq15_entries, 4 - duration)
                self.assertEqual(chip.completed_boundaries, 4 - duration)
                self.assertTrue(chip.d7_pending)

    def test_response_acknowledgment_wins_with_same_tick_durclk(self) -> None:
        chip = self.configured_chip(0x0F, 3)
        chip.response_mode = MODE_FRAME_IMMEDIATE
        chip.u36_count = 14
        self.prime_duration_rise_next_tick(chip)

        chip.step_effective_tick(write_end=(1, 0x00))

        self.assertTrue(chip.duration_clock_rise_this_tick)
        self.assertEqual(chip.duration_clock_rises, 1)
        self.assertTrue(chip.u36_eq15_this_tick)
        self.assertEqual(chip.u36_count, 15)
        self.assertEqual(chip.completed_boundaries, 1)
        self.assertFalse(chip.d7_pending)

    def test_done_zero_hold_uses_pre_edge_response_state(self) -> None:
        chip = self.configured_chip(0x0F, 3)
        chip.u37_count = 15
        self.prime_duration_rise_next_tick(chip)
        chip.advance_effective_ticks(1)

        self.assertEqual(chip.u37_count, 0)
        self.assertTrue(chip.d7_pending)

        self.prime_duration_rise_next_tick(chip)
        chip.advance_effective_ticks(1)
        self.assertTrue(chip.duration_clock_rise_this_tick)
        self.assertEqual(chip.u37_count, 0)

    def test_phone_write_reset_wins_its_u37_wrap_collision(self) -> None:
        chip = self.configured_chip(0x0F, 3)
        chip.u37_count = 15

        chip.begin_write(0, 0xC0)

        self.assertEqual(chip.u37_count, 0)
        self.assertEqual(chip.completed_boundaries, 1)
        self.assertFalse(chip.d7_pending)
        chip.end_write()

    def test_u36_resets_for_full_phone_write_and_release_edge(self) -> None:
        chip = self.configured_chip(0x0F, 3)
        chip.u36_count = 0x123
        chip.begin_write(0, 0xC0)
        self.assertEqual(chip.u36_count, 0)

        chip.u36_count = 14
        self.prime_duration_rise_next_tick(chip)
        chip.step_effective_tick(write_end=(0, 0xC0))

        self.assertTrue(chip.duration_clock_rise_this_tick)
        self.assertEqual(chip.u36_count, 0)
        self.assertEqual(chip.u36_eq15_entries, 0)

        chip._clock_u36()
        self.assertEqual(chip.u36_count, 1)

    def test_u36_eq15_is_an_entry_pulse_on_u21b_q_rises(self) -> None:
        chip = self.configured_chip(0x0F, 3)
        chip.response_mode = MODE_FRAME_IMMEDIATE
        chip.u36_count = 13

        chip._clock_u36()
        self.assertEqual(chip.u36_count, 14)
        self.assertEqual(chip.u36_eq15_entries, 0)
        chip._clock_u36()
        self.assertEqual(chip.u36_count, 15)
        self.assertEqual(chip.u36_eq15_entries, 1)
        self.assertEqual(chip.completed_boundaries, 1)
        chip._clock_u36()
        self.assertEqual(chip.u36_count, 16)
        self.assertEqual(chip.u36_eq15_entries, 1)


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

    def test_first_transitioned_wake_seeds_programmed_pitch_once(self) -> None:
        chip = SSI263Reference(div2=False)
        chip.write(1, 0x50)
        chip.write(2, 0xA8)
        self.assertFalse(chip.transitioned_inflection_seeded)
        self.assertEqual(chip.transitioned_inflection, 0)

        self.wake(chip, 0xC0)

        self.assertTrue(chip.transitioned_inflection_seeded)
        self.assertEqual(chip.transitioned_inflection, 0x50)
        self.assertEqual(chip.pitch_inflection, 0xA80)
        for _ in range(5):
            chip.write(1, 0x50)
            chip.write(2, 0xA8)
            chip.write(0, 0x0E)
            chip.advance_effective_ticks(frame_ticks(0x0A))
            self.assertEqual(chip.transitioned_inflection, 0x50)
            self.assertEqual(chip.pitch_inflection, 0xA80)

        chip.write(3, 0x80)
        chip.write(1, 0x20)
        self.wake(chip, 0xC0)
        self.assertEqual(chip.transitioned_inflection, 0x50)
        chip.advance_effective_ticks(inflection_step_ticks(0x0A, 0))
        self.assertEqual(chip.transitioned_inflection, 0x4F)

    def test_transition_wake_before_pitch_write_consumes_zero_seed(self) -> None:
        chip = SSI263Reference(div2=False)

        self.wake(chip, 0xC0)

        self.assertTrue(chip.transitioned_inflection_seeded)
        self.assertEqual(chip.transitioned_inflection, 0)
        chip.write(1, 0x50)
        chip.write(2, 0xA8)
        self.assertEqual(chip.transitioned_inflection, 0)

    def test_immediate_and_frame_wakes_defer_transition_seed(self) -> None:
        for mode in (MODE_PHONEME_IMMEDIATE, MODE_FRAME_IMMEDIATE):
            with self.subTest(mode=mode):
                chip = SSI263Reference(div2=False)
                chip.write(1, 0x50)
                chip.write(2, 0xA8)
                self.wake(chip, mode << 6)
                self.assertEqual(chip.response_mode, mode)
                self.assertFalse(chip.transitioned_inflection_seeded)
                self.assertEqual(chip.pitch_inflection, 0xA80)

                chip.write(3, 0x80)
                self.wake(chip, 0xC0)
                self.assertEqual(
                    chip.response_mode, MODE_PHONEME_TRANSITIONED
                )
                self.assertTrue(chip.transitioned_inflection_seeded)
                self.assertEqual(chip.transitioned_inflection, 0x50)

    def test_dr_zero_wake_seeds_retained_transition_mode(self) -> None:
        chip = SSI263Reference(div2=False)
        chip.write(1, 0x50)
        chip.write(2, 0xA8)

        self.wake(chip, 0x00)

        self.assertEqual(chip.response_mode, MODE_PHONEME_TRANSITIONED)
        self.assertFalse(chip.ar_enabled)
        self.assertTrue(chip.transitioned_inflection_seeded)
        self.assertEqual(chip.transitioned_inflection, 0x50)
        self.assertEqual(chip.pitch_inflection, 0xA80)

    def test_dr_zero_wake_retains_immediate_mode_without_seed(self) -> None:
        chip = SSI263Reference(div2=False)
        chip.write(1, 0x50)
        chip.write(2, 0xA8)
        self.wake(chip, MODE_PHONEME_IMMEDIATE << 6)
        chip.write(3, 0x80)

        self.wake(chip, 0x00)

        self.assertEqual(chip.response_mode, MODE_PHONEME_IMMEDIATE)
        self.assertFalse(chip.transitioned_inflection_seeded)
        self.assertEqual(chip.transitioned_inflection, 0)
        self.assertEqual(chip.pitch_inflection, 0xA80)

    def test_pd_rst_retains_consumed_transition_seed(self) -> None:
        chip = SSI263Reference(div2=False)
        chip.write(1, 0x50)
        chip.write(2, 0xA8)
        self.wake(chip, 0xC0)
        self.assertTrue(chip.transitioned_inflection_seeded)

        chip.assert_pd_rst()
        chip.release_pd_rst()

        self.assertTrue(chip.transitioned_inflection_seeded)
        self.assertEqual(chip.transitioned_inflection, 0x50)

    def test_acknowledgment_rules(self) -> None:
        for response_mode, register, data, clears in (
            (MODE_PHONEME_TRANSITIONED, 0, 0xC0, True),
            (MODE_PHONEME_TRANSITIONED, 1, 0x00, False),
            (MODE_PHONEME_TRANSITIONED, 2, 0xA0, False),
            (MODE_PHONEME_TRANSITIONED, 3, 0x00, False),
            (MODE_PHONEME_TRANSITIONED, 3, 0x80, True),
            (MODE_PHONEME_TRANSITIONED, 4, 0xE9, False),
            (MODE_PHONEME_TRANSITIONED, 7, 0xE9, False),
            (MODE_FRAME_IMMEDIATE, 0, 0x40, True),
            (MODE_FRAME_IMMEDIATE, 1, 0x00, True),
            (MODE_FRAME_IMMEDIATE, 2, 0xA0, True),
            (MODE_FRAME_IMMEDIATE, 3, 0x00, False),
            (MODE_FRAME_IMMEDIATE, 3, 0x80, True),
            (MODE_FRAME_IMMEDIATE, 4, 0xE9, False),
        ):
            with self.subTest(
                response_mode=response_mode,
                register=register,
                data=data,
            ):
                chip = SSI263Reference(
                    control_articulation_amplitude=0,
                    response_mode=response_mode,
                )
                chip.d7_pending = True
                chip.write(register, data)
                self.assertEqual(chip.d7_pending, not clears)

    def test_acknowledgment_wins_on_completion_edge(self) -> None:
        chip = SSI263Reference(
            div2=False,
            control_articulation_amplitude=0,
            response_mode=MODE_FRAME_IMMEDIATE,
        )
        chip.u36_count = 14
        DurationClockTests.prime_duration_rise_next_tick(chip)
        chip.step_effective_tick(assert_pd_rst=True)
        self.assertTrue(chip.u36_eq15_this_tick)
        self.assertEqual(chip.completed_boundaries, 1)
        self.assertEqual(chip.read_d7(), 0)

    def test_phone_write_blocks_dda_until_sel2_samples_setup(self) -> None:
        chip = SSI263Reference(div2=False)
        chip.write(0, 0x01)
        chip.selector = 0
        chip.selector_subphase = 0
        chip.selector_fast_phase = 1
        chip.tc_edge_window = True
        chip.pw_5 = False
        chip.parameter_resa[0] = 0
        chip.parameter_resb[0] = 2
        chip.parameter_resc[0] = 14
        chip.parameter_rescy[0] = True

        chip.advance_effective_ticks(1)

        self.assertTrue(chip.phone_setup_pending)
        self.assertEqual(chip.parameter_resa[0], 0)
        self.assertEqual(chip.parameter_resc[0], 14)
        self.assertEqual(chip.parameter_write_log, [])

    def test_phoneme_timing_and_continuous_repeat(self) -> None:
        chip = SSI263Reference(
            div2=False,
            duration_phoneme=0xC1,
            rate_inflection=0xF0,
            control_articulation_amplitude=0,
            response_mode=MODE_PHONEME_TRANSITIONED,
            ar_enabled=True,
            phone_active=True,
        )
        chip.u37_count = 0
        for _ in range(15):
            DurationClockTests.wait_for_duration_rise(chip)
        self.assertEqual(chip.read_d7(), 0)
        self.assertEqual(chip.u37_count, 15)
        DurationClockTests.wait_for_duration_rise(chip)
        self.assertEqual(chip.read_d7(), 1)
        self.assertTrue(chip.ar_drive_low)
        self.assertEqual(chip.completed_boundaries, 1)

        DurationClockTests.wait_for_duration_rise(chip)
        self.assertEqual(chip.u37_count, 0)
        self.assertEqual(chip.completed_boundaries, 1)
        self.assertEqual(chip.read_d7(), 1)

        chip.write(1, 0x00)
        self.assertEqual(chip.read_d7(), 1)
        chip.write(0, 0xC1)
        self.assertEqual(chip.read_d7(), 0)
        self.assertTrue(chip.phone_active)
        for _ in range(16):
            DurationClockTests.wait_for_duration_rise(chip)
        self.assertEqual(chip.read_d7(), 1)
        self.assertEqual(chip.completed_boundaries, 2)

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
        chip.u36_count = 14
        DurationClockTests.prime_duration_rise_next_tick(chip)
        chip.advance_effective_ticks(1)
        self.assertTrue(chip.u36_eq15_this_tick)
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

        cold_chip = SSI263Reference(revision_ap=True, div2=False)
        cold_chip.write(0, 0xC0)
        cold_chip.write(1, 0x50)
        cold_chip.write(2, 0xA8)
        cold_chip.step_effective_tick(
            write_end=(3, 0x5C), assert_pd_rst=True
        )
        self.assertFalse(cold_chip.transitioned_inflection_seeded)
        self.assertEqual(cold_chip.transitioned_inflection, 0)
        cold_chip.release_pd_rst()
        cold_chip.write(3, 0x5C)
        self.assertTrue(cold_chip.transitioned_inflection_seeded)
        self.assertEqual(cold_chip.transitioned_inflection, 0x50)

        faulty_p = SSI263Reference(revision_ap=False, div2=False)
        faulty_p.inflection_high = 0x25
        faulty_p.control_articulation_amplitude = 0x00
        faulty_p.d7_pending = True
        faulty_p.step_effective_tick(
            write_end=(1, 0xAA), assert_pd_rst=True
        )
        self.assertEqual(faulty_p.inflection_high, 0xAA)
        self.assertTrue(faulty_p.d7_pending)
        self.assertFalse(faulty_p.powered_down)

    def test_voice_and_glottal_cadence(self) -> None:
        chip = SSI263Reference(div2=False)
        chip.write(0, 0x80)
        chip.write(1, 0xFF)
        chip.write(2, 0x0F)
        chip.write(3, 0x00)
        chip.parameter_values["voice_amp"] = 1
        chip.ampct_count = 15
        chip.pw_3 = False
        chip.selector = 3
        chip.voice_ticks_to_edge = voice_clock_period_ticks(
            chip.pitch_inflection
        )
        chip.advance_effective_ticks(20)
        self.assertEqual(chip.voice_clock_edges, 5)
        self.assertEqual(chip.pitch_events, 3)

        first = chip.last_pitch_event_tick
        chip.advance_effective_ticks(8)
        self.assertEqual(chip.last_pitch_event_tick - first, 8)

    def test_u104c_gates_u41c_without_phone_class_decode(self) -> None:
        chip = SSI263Reference(div2=False)
        chip.pw_3 = True
        chip.parameter_values["fric_amp"] = 1
        chip.selector = 0
        chip.u62_q = False
        chip._update_u41c_edge()
        self.assertEqual(chip.noise_clock_edges, 0)
        chip.pw_3 = False
        chip._update_u41c_edge()
        self.assertEqual(chip.noise_clock_edges, 1)

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

        steps = chip.selector_steps
        chip.write(3, 0x00)
        self.assertFalse(chip.powered_down)
        self.assertTrue(chip.transitioned_inflection_seeded)
        self.assertEqual(
            chip.transitioned_inflection,
            chip.transitioned_inflection_target,
        )
        self.assertEqual(chip.selector_steps, steps)
        chip.advance_effective_ticks(128)
        self.assertGreater(chip.selector_steps, steps)


class SelectorTests(unittest.TestCase):
    def test_selector_three_latches_one_resa_into_f3_and_f4(self) -> None:
        chip = SSI263Reference()
        chip.selector = 3
        chip.parameter_resa[3] = 9
        chip.transition_step()
        self.assertEqual(chip.parameter_values["f3"], 9)
        self.assertEqual(chip.parameter_values["f4"], 9)
        self.assertEqual(chip.selector, 4)

    def test_parameter_latch_occurs_at_r10_not_slot_end(self) -> None:
        chip = SSI263Reference(div2=False)
        chip.selector = 3
        chip.parameter_resa[3] = 9
        chip.advance_effective_ticks(9)
        self.assertEqual(chip.parameter_values["f3"], 0)
        chip.advance_effective_ticks(1)
        self.assertEqual(chip.parameter_values["f3"], 9)
        self.assertEqual(chip.parameter_values["f4"], 9)

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
        chip.parameter_resa[4] = 2
        chip.control_setup_window = True
        chip.transition_step()
        self.assertEqual(chip.parameter_resa[4], 2)
        self.assertEqual(chip.parameter_resb[4], 0x0A)
        self.assertEqual(chip.parameter_resc[4], 8)
        self.assertTrue(chip.parameter_rescy[4])
        self.assertEqual(chip.parameter_values["filter_amp"], 2)

    def test_selector_seven_is_idle(self) -> None:
        chip = SSI263Reference()
        chip.selector = 7
        chip.parameter_values["f4"] = 9
        chip.transition_step()
        self.assertEqual(chip.parameter_values["f4"], 9)
        self.assertEqual(chip.selector, 0)

    def test_slots_do_not_move_without_u96_permit(self) -> None:
        chip = SSI263Reference()
        chip.duration_phoneme = 0x00
        target = chip.target_for_selector(0)
        self.assertGreater(target, 0)
        chip.parameter_resb[0] = target
        chip.parameter_resc[0] = 16 - target
        chip.parameter_rescy[0] = True
        chip.transition_step()
        self.assertEqual(chip.parameter_resa[0], 0)
        self.assertEqual(chip.parameter_write_log, [])

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
                chip.control_articulation_amplitude = 0
                chip.pw_1 = True
                chip.u37_count = 6
                chip.advance_effective_ticks(48)
                self.assertEqual(chip.pw_2, bool(code & 0x4))
                self.assertEqual(chip.pw_3, not bool(code & 0x2))
                self.assertEqual(chip.pw_5, not bool(code & 0x4))

        for phone in range(64):
            with self.subTest(phone=phone):
                chip = SSI263Reference(div2=False)
                chip.duration_phoneme = phone
                chip.u37_count = (
                    2
                    if chip.rom[rom_address(phone, 0)] & 0x01
                    else 6
                )
                chip.advance_effective_ticks(32)
                self.assertTrue(chip.pw_0)

                chip = SSI263Reference(div2=False)
                chip.duration_phoneme = phone
                chip.u37_count = (
                    2
                    if chip.rom[rom_address(phone, 1)] & 0x01
                    else 6
                )
                chip.advance_effective_ticks(32)
                self.assertTrue(chip.pw_1)

    def test_u68_u85_and_route_latches_follow_drawn_gates(self) -> None:
        chip = SSI263Reference(div2=False)
        chip.parameter_values["voice_amp"] = 1
        chip.pw_3 = False
        chip.selector = 3
        chip._update_amplitude_counter()
        chip.selector = 4
        chip._update_amplitude_counter()
        self.assertEqual(chip.ampct_count, 1)
        self.assertTrue(chip.ampct_zero)
        self.assertTrue(chip.u62_reset)

        chip.ampct_count = 14
        chip.u68_clock_level = False
        chip.selector = 3
        chip._update_amplitude_counter()
        chip.selector = 4
        chip._update_amplitude_counter()
        self.assertEqual(chip.ampct_count, 15)
        self.assertFalse(chip.ampct_nco)
        self.assertFalse(chip.u62_reset)

        chip.duration_phoneme = 0x01
        chip.selector = 2
        chip.pw_0 = True
        chip.pw_1 = True
        chip.pw_2 = True
        chip.ampct_count = 0
        chip.parameter_values["fric_amp"] = 0
        expected_tparm3 = bool(chip.flags_for_selector(2) & 0x08)
        chip.transition_step()
        self.assertEqual(chip.u20b_q, expected_tparm3)
        chip.filter_phase = 1
        chip._advance_filter_clock(0)
        self.assertEqual(chip.fric1_sw, chip.u20b_q)
        chip.filter_ticks_to_toggle = 1
        chip.filter_phase = 1
        chip._advance_filter_clock(1)
        self.assertEqual(chip.fric2_sw, not chip.u20b_q)

    def test_u20_first_selector_two_scan_uses_settling_pw2_gate(self) -> None:
        chip = SSI263Reference(div2=False)
        chip.duration_phoneme = 0x01
        chip.selector = 2
        chip.pw_0 = True
        chip.pw_1 = True
        chip.pw_2 = False
        chip.ampct_count = 0
        chip.parameter_values["fric_amp"] = 0
        flags = chip.flags_for_selector(2)
        self.assertTrue(flags & 0x04)
        expected_tparm3 = bool(flags & 0x08)
        chip.transition_step()
        self.assertTrue(chip.pw_2)
        self.assertEqual(chip.u20b_q, expected_tparm3)

    @staticmethod
    def rom_nibble(phone: int, selector: int) -> int:
        chip = SSI263Reference()
        return chip.rom[rom_address(phone, selector)] & 0x0F


class DdaAndU96Tests(unittest.TestCase):
    def test_all_rom_rows_reach_pw_phases_from_natural_durclk(self) -> None:
        for phone in range(64):
            with self.subTest(phone=phone):
                data = 0xC0 | phone
                chip = SSI263Reference(
                    div2=False,
                    duration_phoneme=data,
                    rate_inflection=0xF0,
                    control_articulation_amplitude=0,
                )
                flags0 = chip.rom[rom_address(phone, 0)] & 0x0F
                flags1 = chip.rom[rom_address(phone, 1)] & 0x0F
                flags2 = chip.rom[rom_address(phone, 2)] & 0x0F
                expected_phase0 = 2 if flags0 & 0x01 else 6
                expected_phase1 = 2 if flags1 & 0x01 else 6
                expected_pw3 = not bool(flags2 & 0x02)

                chip.d7_pending = True
                chip.u37_count = 0
                chip.pw_0 = True
                chip.pw_1 = True
                chip.pw_3 = not expected_pw3
                chip.begin_write(0, data)
                self.assertEqual(chip.u37_count, 0)
                self.assertFalse(chip.pw_0)
                self.assertFalse(chip.pw_1)
                chip.end_write()
                self.assertFalse(chip.d7_pending)

                start_rises = chip.duration_clock_rises
                previous_pw0 = chip.pw_0
                previous_pw1 = chip.pw_1
                previous_pw3 = chip.pw_3
                pw0_phase = None
                pw1_phase = None
                pw3_phase = None
                while chip.duration_clock_rises - start_rises < 7:
                    chip.advance_effective_ticks(1)
                    if chip.pw_0 != previous_pw0:
                        pw0_phase = chip.u37_count
                    if chip.pw_1 != previous_pw1:
                        pw1_phase = chip.u37_count
                    if chip.pw_3 != previous_pw3:
                        pw3_phase = chip.u37_count
                    previous_pw0 = chip.pw_0
                    previous_pw1 = chip.pw_1
                    previous_pw3 = chip.pw_3

                self.assertEqual(pw0_phase, expected_phase0)
                self.assertEqual(pw1_phase, expected_phase1)
                self.assertEqual(pw3_phase, expected_phase1)
                self.assertTrue(chip.pw_0)
                self.assertTrue(chip.pw_1)
                self.assertEqual(chip.pw_3, expected_pw3)

    def test_u37_zero_hold_and_timed_pw_latches(self) -> None:
        chip = SSI263Reference(div2=False)
        chip.d7_pending = True
        chip.u37_count = 0
        chip.pw_0 = True
        chip.pw_1 = True
        chip.begin_write(0, 0x01)
        self.assertEqual(chip.u37_count, 0)
        self.assertFalse(chip.pw_0)
        self.assertFalse(chip.pw_1)
        chip.end_write()

        chip.begin_write(0, 0x01)
        self.assertEqual(chip.u37_count, 1)
        chip.end_write()
        chip.selector = 0
        chip.u37_count = 2 if chip.flags_for_selector() & 1 else 6
        chip._selector_latch_event(suppress_control_latch=False)
        self.assertTrue(chip.pw_0)

        chip.selector = 1
        chip.u37_count = 2 if chip.flags_for_selector() & 1 else 6
        chip._selector_latch_event(suppress_control_latch=False)
        self.assertTrue(chip.pw_1)

        chip.control_articulation_amplitude = 0
        chip.u183a_q = False
        chip.duration_phoneme = 0x01
        chip.selector = 2

        chip.pw_3 = True
        chip.pw_1 = False
        chip.u37_count = 2 if chip.flags_for_selector() & 1 else 6
        chip._selector_latch_event(suppress_control_latch=False)
        self.assertTrue(chip.pw_3)

        chip.pw_1 = True
        chip.u37_count = 5
        chip._selector_latch_event(suppress_control_latch=False)
        self.assertFalse(chip.pw_3)

    def test_u29a_overlap_set_wins_and_u183_retention(self) -> None:
        chip = SSI263Reference(div2=False)
        chip.control_articulation_amplitude = 0
        DurationClockTests.prime_duration_rise_next_tick(chip)
        chip.begin_write(0, 0x01)
        self.assertEqual(chip.u37_count, 1)
        self.assertFalse(chip.u29a_level)
        chip.step_effective_tick(write_end=(0, 0x01))
        self.assertTrue(chip.duration_clock_rise_this_tick)
        self.assertEqual(chip.u37_count, 1)
        self.assertFalse(chip.u29a_level)
        self.assertTrue(chip.duration_clock)
        chip._u21b_nq_rise()
        self.assertEqual(chip.u28_count, 12)
        self.assertFalse(chip.duration_clock)
        self.assertTrue(chip.u29a_level)

        chip = SSI263Reference(div2=False)
        chip.duration_phoneme = 0x01
        chip.selector = 0
        chip.selector_subphase = 2
        chip.selector_fast_phase = 2
        chip.u37_count = (
            2 if chip.flags_for_selector(0) & 1 else 6
        )
        chip.begin_write(0, 0x01)
        self.assertTrue(chip.pw_0)

        chip = SSI263Reference(div2=False)
        chip.control_articulation_amplitude = 0
        chip.assert_pd_rst()
        chip.release_pd_rst()
        chip.control_articulation_amplitude = 0
        chip.duration_phoneme = 0x01
        chip.selector = 2
        chip.selector_subphase = 2
        chip.selector_fast_phase = 1
        chip.pw_1 = True
        chip.u37_count = 6
        chip.advance_effective_ticks(1)
        self.assertTrue(chip.pw_3)
        chip.advance_effective_ticks(1)
        self.assertFalse(chip.pw_3)
        chip.u37_count = 5
        chip.duration_phoneme = 0x00
        chip._selector_latch_event(suppress_control_latch=False)
        self.assertFalse(chip.pw_3)

    def test_upward_dda_vector_reaches_target_on_event_fifteen(self) -> None:
        chip = SSI263Reference(div2=False)
        chip.duration_phoneme = 0x04  # ROM selector 3 target is B.
        chip.selector = 3
        chip.parameter_resa[3] = 0x3
        chip.phone_setup_window = True

        chip._selector_write_event()

        self.assertEqual(chip.parameter_resa[3], 0x3)
        self.assertEqual(chip.parameter_resb[3], 0x8)
        self.assertEqual(chip.parameter_resc[3], 0x8)
        self.assertTrue(chip.parameter_rescy[3])

        chip.phone_setup_window = False
        chip.tc_edge_window = True
        chip.pw_5 = False
        observed: list[tuple[int, int]] = []
        for _ in range(15):
            chip._selector_write_event()
            observed.append(
                (chip.parameter_resa[3], chip.parameter_resc[3])
            )

        self.assertEqual(observed[:3], [(0x4, 0x0), (0x4, 0x8), (0x5, 0x0)])
        self.assertEqual(observed[-1], (0xB, 0x0))
        self.assertEqual(len(chip.parameter_write_log), 15)

        chip._selector_write_event()
        self.assertEqual(chip.parameter_resa[3], 0xB)
        self.assertEqual(chip.parameter_resc[3], 0x0)
        self.assertEqual(len(chip.parameter_write_log), 15)

    def test_downward_dda_vector_reaches_target_on_event_sixteen(self) -> None:
        chip = SSI263Reference(div2=False)
        chip.duration_phoneme = 0x05  # ROM selector 0 target is 3.
        chip.selector = 0
        chip.parameter_resa[0] = 0xC
        chip.phone_setup_window = True

        chip._selector_write_event()

        self.assertEqual(chip.parameter_resa[0], 0xC)
        self.assertEqual(chip.parameter_resb[0], 0x7)
        self.assertEqual(chip.parameter_resc[0], 0x8)
        self.assertFalse(chip.parameter_rescy[0])

        chip.phone_setup_window = False
        chip.tc_edge_window = True
        chip.pw_5 = False
        observed: list[tuple[int, int]] = []
        for _ in range(16):
            chip._selector_write_event()
            observed.append(
                (chip.parameter_resa[0], chip.parameter_resc[0])
            )

        self.assertEqual(observed[:2], [(0xB, 0xF), (0xB, 0x6)])
        self.assertEqual(observed[-1], (0x3, 0x8))
        self.assertEqual(len(chip.parameter_write_log), 16)

    def test_host_write_suppresses_setup_and_normal_dda_write(self) -> None:
        chip = SSI263Reference(div2=False)
        chip.duration_phoneme = 0x04
        chip.selector = 3
        chip.parameter_resa[3] = 3
        chip.parameter_resb[3] = 2
        chip.parameter_resc[3] = 14
        chip.parameter_rescy[3] = True
        chip.phone_setup_window = True

        chip._selector_write_event(suppress_write=True)
        self.assertEqual(chip.parameter_resa[3], 3)
        self.assertEqual(chip.parameter_resb[3], 2)
        self.assertEqual(chip.parameter_resc[3], 14)

        chip.phone_setup_window = False
        chip.tc_edge_window = True
        chip.pw_5 = False
        chip._selector_write_event(suppress_write=True)
        self.assertEqual(chip.parameter_resa[3], 3)
        self.assertEqual(chip.parameter_resc[3], 14)
        self.assertEqual(chip.parameter_write_log, [])

    def test_setup_decode_matches_phone_and_control_windows(self) -> None:
        chip = SSI263Reference()
        chip.control_articulation_amplitude = 0x0F
        chip.phone_setup_window = True
        self.assertEqual(
            [chip.transition_a_clr(selector) for selector in range(8)],
            [True, True, True, True, False, True, True, True],
        )

        chip.phone_setup_window = False
        chip.control_setup_window = True
        self.assertEqual(
            [chip.transition_a_clr(selector) for selector in range(8)],
            [False, False, False, False, True, True, True, False],
        )

        chip.control_articulation_amplitude = 0x00
        self.assertFalse(chip.transition_a_clr(4))
        self.assertTrue(chip.transition_a_clr(5))
        self.assertTrue(chip.transition_a_clr(6))

    def test_u96_prototype_truth_table(self) -> None:
        chip = SSI263Reference()
        chip.tc_edge_window = True
        chip.pw_5 = False
        self.assertEqual(
            [chip.u96_write_permit(selector) for selector in range(8)],
            [True, True, True, True, False, False, False, False],
        )

        chip.pw_5 = True
        chip.duration_phoneme = 0x20
        self.assertFalse(chip.u96_write_permit(0))
        self.assertFalse(chip.u96_write_permit(1))
        self.assertTrue(chip.u96_write_permit(2))
        self.assertFalse(chip.u96_write_permit(3))

        chip.control_articulation_amplitude = 0x0F
        chip.duration_edge_window = True
        chip.u166b_nq = True
        self.assertTrue(chip.u96_write_permit(4))
        chip.u166b_nq = False
        self.assertFalse(chip.u96_write_permit(4))
        chip.u166b_nq = True
        chip.control_articulation_amplitude = 0x00
        self.assertFalse(chip.u96_write_permit(4))

        chip.pw_0 = True
        chip.pw_1 = True
        chip.u93_rate_q1 = True
        chip.u93_rate_q2 = False
        self.assertTrue(chip.u96_write_permit(5))
        self.assertTrue(chip.u96_write_permit(6))
        chip.u93_rate_q2 = True
        self.assertFalse(chip.u96_write_permit(5))
        self.assertFalse(chip.u96_write_permit(6))
        self.assertFalse(chip.u96_write_permit(7))

    def test_sel2_samples_pending_events_for_one_scan(self) -> None:
        chip = SSI263Reference()
        chip.selector = 3
        chip.duration_phoneme = 0x80
        chip.rate_clock = 1
        chip.phone_setup_pending = True
        chip.control_setup_pending = True
        chip.tc_edge_pending = True
        chip.duration_edge_pending = True
        chip.u166b_nq = False

        chip._complete_selector_slot(suppress_slot_write=False)

        self.assertEqual(chip.selector, 4)
        self.assertTrue(chip.phone_setup_window)
        self.assertTrue(chip.control_setup_window)
        self.assertTrue(chip.tc_edge_window)
        self.assertTrue(chip.duration_edge_window)
        self.assertFalse(chip.phone_setup_pending)
        self.assertFalse(chip.control_setup_pending)
        self.assertFalse(chip.tc_edge_pending)
        self.assertFalse(chip.duration_edge_pending)
        self.assertTrue(chip.u93_rate_q1)
        self.assertFalse(chip.u93_rate_q2)
        self.assertTrue(chip.u166b_nq)

        chip.u166b_nq = False
        chip.selector = 3
        chip.control_setup_pending = True
        chip.duration_edge_pending = False
        chip._complete_selector_slot(suppress_slot_write=False)
        self.assertTrue(chip.u166b_nq)

        chip.selector = 3
        chip.rate_clock = 0
        chip._complete_selector_slot(suppress_slot_write=False)
        self.assertFalse(chip.phone_setup_window)
        self.assertFalse(chip.control_setup_window)
        self.assertFalse(chip.tc_edge_window)
        self.assertFalse(chip.duration_edge_window)
        self.assertFalse(chip.u93_rate_q1)
        self.assertTrue(chip.u93_rate_q2)

    def test_ampzero_masks_voice_and_fricative_targets(self) -> None:
        chip = SSI263Reference()
        chip.control_articulation_amplitude = 0x00
        nonzero = [
            (phone, selector)
            for phone in range(64)
            for selector in (5, 6)
            if chip.rom[rom_address(phone, selector)] >> 4
        ]
        self.assertTrue(nonzero)
        for phone, selector in nonzero:
            chip.duration_phoneme = phone
            self.assertEqual(chip.target_for_selector(selector), 0)


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
