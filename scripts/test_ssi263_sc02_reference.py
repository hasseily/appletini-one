#!/usr/bin/env python3
"""Test the native SSI-263 / SC-02 ROM and cycle reference."""

from __future__ import annotations

import unittest

from ssi263_sc02_reference import (
    MODE_FRAME_IMMEDIATE,
    MODE_PHONEME_TRANSITIONED,
    ROM_ACTIVE_SIZE,
    SELECTOR_SLOW_EDGES,
    SSI263Reference,
    filter_period_ticks,
    frame_ticks,
    inflection_word,
    load_active_rom,
    phoneme_ticks,
    pitch_period_ticks,
    rom_address,
    verify_rom,
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
        self.assertEqual(pitch_period_ticks(0xA80), 0x2C00)
        self.assertEqual(frame_ticks(0x0A), 4096 * 6)
        self.assertEqual(phoneme_ticks(0x0A, 3), 4096 * 6)
        self.assertEqual(phoneme_ticks(0x0A, 0), 4096 * 6 * 4)
        self.assertEqual(filter_period_ticks(0x00), 512)
        self.assertEqual(filter_period_ticks(0xFF), 2)

    def test_two_xck_edges_with_div2_make_one_effective_tick(self) -> None:
        chip = SSI263Reference(xck_edges_per_bus_cycle=2, div2=True)
        chip.feed_bus_cycles(1234)
        self.assertEqual(chip.xck_pin_edges, 2468)
        self.assertEqual(chip.effective_xck_ticks, 1234)


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

    def test_filter_write_preserves_current_divider_count(self) -> None:
        chip = SSI263Reference()
        chip.write(4, 0x00)
        chip.filter_ticks_to_toggle = 123
        chip.write(4, 0xFF)
        self.assertEqual(chip.filter_ticks_to_toggle, 123)
        chip.advance_effective_ticks(123)
        self.assertEqual(chip.filter_ticks_to_toggle, 1)


class SelectorTests(unittest.TestCase):
    def test_selector_three_updates_f3_and_f4(self) -> None:
        chip = SSI263Reference()
        chip.duration_phoneme = 0x00
        chip.selector = 3
        target = chip.target_for_selector()
        chip.transition_step()
        self.assertEqual(chip.parameter_values["f3"], min(target, 1))
        self.assertEqual(chip.parameter_values["f4"], min(target, 1))
        self.assertEqual(chip.selector, 4)

    def test_selector_holds_for_four_slow_edges(self) -> None:
        chip = SSI263Reference()
        start = chip.selector
        for edge in range(SELECTOR_SLOW_EDGES - 1):
            chip.advance_selector_slow_edge(pulse=edge == 0)
            self.assertEqual(chip.selector, start)
        chip.advance_selector_slow_edge()
        self.assertEqual(chip.selector, start + 1)

    def test_selector_four_uses_host_amplitude(self) -> None:
        chip = SSI263Reference()
        chip.control_articulation_amplitude = 0x0C
        chip.selector = 4
        self.assertEqual(chip.rom_byte(), 0)
        self.assertEqual(chip.target_for_selector(), 0x0C)
        chip.transition_step()
        self.assertEqual(chip.parameter_values["filter_amp"], 1)

    def test_selector_seven_is_idle(self) -> None:
        chip = SSI263Reference()
        chip.selector = 7
        chip.parameter_values["f4"] = 9
        chip.transition_step()
        self.assertEqual(chip.parameter_values["f4"], 9)
        self.assertEqual(chip.selector, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
