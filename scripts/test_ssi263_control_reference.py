#!/usr/bin/env python3
"""Check the independent SC-02 control reference against its circuit contract."""
from __future__ import annotations

from itertools import product
from pathlib import Path
import tempfile
import unittest

import ssi263_control_reference as ref


class SourceControlReferenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rom = ref.load_rom()

    def test_rom_low_fields_and_all_64_addresses(self):
        self.assertEqual(len(self.rom), 512)
        fields = [[ref.rom_byte(phone, selector, self.rom) for selector in range(8)]
                  for phone in range(64)]
        self.assertEqual(bytes(value for row in fields for value in row), self.rom)
        self.assertEqual({row[0] & 15 for row in fields}, {0, 1})
        self.assertEqual({row[1] & 15 for row in fields}, {0, 1})
        self.assertEqual({row[2] & 15 for row in fields}, {4, 6, 8, 10, 12, 14})
        self.assertEqual(sum(row[0] & 1 for row in fields), 9)
        self.assertEqual(sum(row[1] & 1 for row in fields), 50)
        self.assertEqual([sum(bool(row[2] & (1 << n)) for row in fields) for n in (1, 2, 3)],
                         [57, 58, 59])
        self.assertTrue(all(value & 15 == 0 for row in fields for value in row[3:]))

    def test_startup_latches_are_unknown(self):
        state = ref.SourceControlState()
        self.assertEqual(state.snapshot(), dict.fromkeys(ref.STATE_NAMES))
        self.assertEqual(ref.pack_bits(state.snapshot(), ref.STATE_NAMES), (0, 0))
        state.phone_write()
        self.assertEqual(state.pw0, 0)
        self.assertEqual(state.pw1, 0)
        self.assertTrue(all(getattr(state, name) is None for name in ref.STATE_NAMES[2:]))

    def test_phone_writes_clear_only_pw0_pw1(self):
        state = ref.SourceControlState(**dict.fromkeys(ref.STATE_NAMES, 1))
        state.phone_write()
        self.assertEqual(state.snapshot(), {name: int(name not in ("pw0", "pw1"))
                                           for name in ref.STATE_NAMES})
        state.set_latched_ctrl(0)
        before = state.snapshot()
        # The externally supplied CTRL level is not a host CTL transaction.
        state.set_latched_ctrl(1)
        state.set_latched_ctrl(0)
        self.assertEqual(state.snapshot(), before)

    def test_every_phone_pw_start_and_hold(self):
        for phone, selector in product(range(64), (0, 1)):
            byte = ref.rom_byte(phone, selector, self.rom)
            name = f"pw{selector}"
            comparison_phase = 2 if byte & 1 else 6
            state = ref.SourceControlState()
            state.phone_write()
            for phase in range(16):
                state.selector_edge(selector, byte, phase, ctrl=0)
                self.assertEqual(getattr(state, name), int(phase >= comparison_phase),
                                 (phone, selector, phase))
            state.selector_edge(selector, byte ^ 1, 0, ctrl=0)
            self.assertEqual(getattr(state, name), 1)
            self.assertEqual(getattr(state, f"pw{1-selector}"), 0)

    def test_selector_controls_ignore_target_nibble(self):
        for selector, low, phase in product(range(8), range(16), (2, 6)):
            states = []
            for high in (0, 15):
                state = ref.SourceControlState(pw0=1, pw1=1, pw2=1, u20=0)
                state.selector_edge(selector, high << 4 | low, phase,
                                    ctrl=0, ampct_zero=1, fa_zero=0)
                states.append(state.snapshot())
            self.assertEqual(*states)

    def test_other_selectors_do_not_load_tparm_controls(self):
        for selector in range(3, 8):
            state = ref.SourceControlState(pw0=0, pw1=1, pw2=0, pw3=1, pw5=1,
                                           u20=0, fric1_sw=0, fric2_sw=1)
            before = state.snapshot()
            state.selector_edge(selector, 255, 6, ctrl=0, ampct_zero=1, fa_zero=1)
            self.assertEqual(state.snapshot(), before)

    def test_hf_hfc_differ_in_qualified_pw3_not_targets(self):
        hf = self.rom[0x2C * 8:0x2D * 8]
        hfc = self.rom[0x2D * 8:0x2E * 8]
        self.assertEqual(bytes(value >> 4 for value in hf), bytes(value >> 4 for value in hfc))
        self.assertEqual([(i, a ^ b) for i, (a, b) in enumerate(zip(hf, hfc)) if a != b], [(2, 2)])
        states = []
        for phone in (0x2C, 0x2D):
            state = ref.SourceControlState(pw3=0)
            state.phone_write()
            state.selector_edge(2, ref.rom_byte(phone, 2, self.rom), 0, ctrl=0)
            self.assertEqual(state.pw3, 0)  # PW1 has not opened the load gate.
            state.selector_edge(1, ref.rom_byte(phone, 1, self.rom), 6, ctrl=0)
            state.selector_edge(2, ref.rom_byte(phone, 2, self.rom), 6, ctrl=0)
            states.append(state)
        self.assertEqual([s.pw3 for s in states], [0, 1])
        self.assertEqual([s.source_terms(1, 0, 0)["fricative"] for s in states], [1, 0])
        # A direct TPARM1 mute would wrongly keep these two results different.
        self.assertEqual([s.source_terms(0, 0, 1)["fricative"] for s in states], [1, 1])
        for state, phone in zip(states, (0x2C, 0x2D)):
            state.set_latched_ctrl(1)
            # Changing the external CTRL level does not cause a selector-2 load.
            self.assertEqual(state.pw3, int(phone == 0x2D))
            state.selector_edge(2, ref.rom_byte(phone, 2, self.rom), 6, ctrl=1)
            self.assertEqual(state.pw3, 1)

    def test_old_pw2_or_current_tparm2_both_qualify_route(self):
        for old_pw2, current_tparm2 in ((1, 0), (0, 1)):
            state = ref.SourceControlState(pw0=1, pw1=1, pw2=old_pw2, u20=0)
            state.selector_edge(2, 8 | current_tparm2 << 2, 6,
                                ctrl=0, ampct_zero=1, fa_zero=0)
            self.assertEqual(state.u20, 1)
            self.assertEqual(state.pw2, current_tparm2)
            self.assertEqual(state.pw5, 1 - current_tparm2)
        state = ref.SourceControlState(pw0=1, pw1=1, pw2=0, u20=1)
        state.selector_edge(2, 0, 6, ctrl=0, ampct_zero=1, fa_zero=1)
        self.assertEqual(state.u20, 1)  # Both route-permit sources are now low.

    def test_route_load_gate_exhaustively(self):
        for pw0, pw1, old_pw2, tparm2, amp_zero, fa_zero, old_route, request in product((0, 1), repeat=8):
            state = ref.SourceControlState(pw0=pw0, pw1=pw1, pw2=old_pw2, u20=old_route)
            # Contract expressed as independently blocking conditions.
            blocked = not pw1 or not (old_pw2 or tparm2) or not (fa_zero or (pw0 and amp_zero))
            state.selector_edge(2, request << 3 | tparm2 << 2, 0,
                                ctrl=0, ampct_zero=amp_zero, fa_zero=fa_zero)
            self.assertEqual(state.u20, old_route if blocked else request)

    def test_routes_hold_independently_and_phi1_is_transparent(self):
        state = ref.SourceControlState(pw0=1, pw1=1, pw2=1, u20=0)
        state.set_phi1(1)
        state.set_phi1(0)
        state.phi0_rising()
        self.assertEqual((state.fric1_sw, state.fric2_sw), (0, 1))
        state.selector_edge(2, 12, 6, ctrl=0, ampct_zero=1, fa_zero=1)
        self.assertEqual((state.u20, state.fric1_sw, state.fric2_sw), (1, 0, 1))
        state.set_phi1(1)
        self.assertEqual((state.fric1_sw, state.fric2_sw), (1, 1))
        state.set_phi1(0)
        state.phi0_rising()
        self.assertEqual((state.fric1_sw, state.fric2_sw), (1, 0))
        state.set_phi1(1)
        # No new Phi1 edge: U112 must still pass the new U20 value.
        state.selector_edge(2, 4, 6, ctrl=0, ampct_zero=1, fa_zero=1)
        self.assertEqual((state.fric1_sw, state.fric2_sw), (0, 0))
        state.set_phi1(0)
        state.phi0_rising()
        self.assertEqual((state.fric1_sw, state.fric2_sw), (0, 1))

    def test_source_gate_truth_table(self):
        for pw3, u62_nq, d3_or4, va_zero in product((0, 1), repeat=4):
            state = ref.SourceControlState(pw3=pw3)
            terms = state.source_terms(u62_nq, d3_or4, va_zero)
            if d3_or4 or pw3 and u62_nq:
                expected_fricative = 0
            else:
                expected_fricative = int(bool(u62_nq or va_zero))
            self.assertEqual(terms["fricative"], expected_fricative)
            self.assertEqual(terms["u104c"], int(bool(pw3 and u62_nq)))
            self.assertEqual(terms["ampct0"], 1 - terms["u104c"])

    def test_transition_permission_is_not_an_audio_mute(self):
        for pw5, phone_high, voice, noise in product((0, 1), repeat=4):
            state = ref.SourceControlState(pw5=pw5, pw3=0)
            self.assertEqual(state.transition_permit(phone_high, voice, noise),
                             int(not pw5 or not any((phone_high, voice, noise))))
            self.assertEqual(state.source_terms(1, 0, 0)["fricative"], 1)

    def test_unknowns_propagate_and_forcing_values_resolve_them(self):
        self.assertIsNone(ref.invert(None))
        self.assertEqual(ref.conjunction(None, 0), 0)
        self.assertEqual(ref.disjunction(None, 1), 1)
        self.assertEqual(ref.held_load(1, None, 1), 1)
        self.assertIsNone(ref.held_load(0, None, 1))
        state = ref.SourceControlState()
        self.assertEqual(state.source_terms(0, 0, 1), {"u104c": 0, "ampct0": 1, "fricative": 1})
        state.selector_edge(2, 12, 0, ctrl=0, ampct_zero=1, fa_zero=1)
        self.assertEqual((state.pw2, state.pw5), (1, 0))
        self.assertIsNone(state.pw3)
        self.assertIsNone(state.u20)
        self.assertEqual(state.transition_permit(1, 1, 1), 1)

    def test_event_api_refuses_unknown_edge_order(self):
        inputs = ref.qualification_events()[1]
        for changes in ({"phone_write": 1, "wr_sel0": 1},
                        {"wr_sel0": 1, "wr_sel1": 1},
                        {"wr_sel2": 1, "phi0_rise": 1}):
            with self.assertRaises(ValueError):
                ref.apply_inputs(ref.SourceControlState(), {**inputs, **changes})

    def test_vector_format_and_determinism(self):
        events = ref.qualification_events()
        vectors = ref.trace(events)
        self.assertGreater(len(vectors), 5000)
        self.assertEqual(vectors, ref.trace(events))
        self.assertEqual(vectors[0]["state"], dict.fromkeys(ref.STATE_NAMES))
        for item in vectors:
            packed = ref.pack_vector(item)
            self.assertLess(packed, 1 << 56)
            stimulus = packed >> 24
            for name, (shift, width) in ref.STIMULUS_FIELDS.items():
                self.assertEqual((stimulus >> shift) & ((1 << width) - 1), item["inputs"][name])
            state_value, state_known = ref.pack_bits(item["state"], ref.STATE_NAMES)
            self.assertEqual((packed >> 16) & 255, state_value)
            self.assertEqual((packed >> 8) & 255, state_known)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "vectors.mem"
            self.assertEqual(ref.write_vectors(path), len(vectors))
            self.assertEqual(path.read_text().splitlines(), [f"{ref.pack_vector(v):014x}" for v in vectors])


if __name__ == "__main__":
    unittest.main(verbosity=2)
