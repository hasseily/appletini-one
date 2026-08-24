#!/usr/bin/env python3
"""Source-level regression tests for the Phasor-compatible sound card.

These tests run without Vivado or hardware:

    python scripts/test_phasor_card.py
"""

from __future__ import annotations

import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
MOCKINGBOARD_SV = REPO_ROOT / "hdl" / "apple" / "mockingboard.sv"
YM2149_SV = REPO_ROOT / "hdl" / "apple" / "YM2149.sv"
VIA6522_V = REPO_ROOT / "hdl" / "apple" / "via6522.v"
APPLE_TOP_SV = REPO_ROOT / "hdl" / "apple" / "apple_top.sv"
APPLE_BUS_WRAPPER_SV = REPO_ROOT / "hdl" / "apple" / "apple_bus_wrapper.sv"
APPLETINI_YARZ_TOP_SV = REPO_ROOT / "hdl" / "appletini_yarz_top.sv"
CONFIG_MENU_C = REPO_ROOT / "ps_sources" / "frontend" / "config_menu.c"
CONFIG_MENU_H = REPO_ROOT / "ps_sources" / "frontend" / "config_menu.h"
CONFIG_MENU_INTERNAL_H = REPO_ROOT / "ps_sources" / "frontend" / "config_menu_internal.h"
CONFIG_MENU_HELP_C = REPO_ROOT / "ps_sources" / "frontend" / "config_menu_help.c"
CONFIG_MENU_PHASOR_C = REPO_ROOT / "ps_sources" / "frontend" / "config_menu_phasor.c"
FRONTEND_MAIN_C = REPO_ROOT / "ps_sources" / "frontend" / "main.c"
CARD_CONTROL_REGS_H = REPO_ROOT / "ps_sources" / "frontend" / "card_control_regs.h"
IMAGE_VERSIONS_H = REPO_ROOT / "ps_sources" / "image_versions.h"
CREATE_VITIS_WORKSPACE_PY = REPO_ROOT / "scripts" / "create_vitis_workspace.py"
CREATE_PROJECT_TCL = REPO_ROOT / "scripts" / "create_project.tcl"
HDL_SOURCES_TXT = REPO_ROOT / "hdl" / "hdl_sources.txt"

LEGACY_SPEECH_RE = re.compile(
    r"(?i)(?<![a-z0-9])(?:sc(?:[-_ ]?0?1)a?|votrax)(?![a-z0-9])"
)
LEGACY_IMPLEMENTATION_NAMES = (
    "ssi263_type",
    "ssi263_formant_pkg",
    "ssi263_formant_backend",
    "ssi263_bus_wrapper",
)


class TestFailure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def implementation_text(path: Path) -> str:
    source = read(path)
    if path.suffix.lower() in {".sv", ".svh", ".v", ".vh"}:
        source = re.sub(r"/\*.*?\*/", "", source, flags=re.DOTALL)
        source = re.sub(r"//[^\r\n]*", "", source)
    elif path.suffix.lower() == ".xdc":
        source = re.sub(r"(?m)#.*$", "", source)
    return source


def hdl_source_closure() -> list[Path]:
    entries = [
        line.strip()
        for line in read(HDL_SOURCES_TXT).splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    paths = [REPO_ROOT / "hdl" / entry for entry in entries]
    missing = [str(path) for path in paths if not path.is_file()]
    require(not missing, f"Vivado HDL source closure has missing files: {missing}")
    return paths


def sv_instance_block(source: str, instance_header: str) -> str:
    require(instance_header in source, f"missing SystemVerilog instance: {instance_header}")
    return source.split(instance_header, 1)[1].split(");", 1)[0]


def test_native_ssi263_hdl_source_closure() -> None:
    paths = hdl_source_closure()
    closure_names = "\n".join(
        path.relative_to(REPO_ROOT / "hdl").as_posix() for path in paths
    )
    closure_text = "\n".join(implementation_text(path) for path in paths)
    closure = f"{closure_names}\n{closure_text}"

    legacy_match = LEGACY_SPEECH_RE.search(closure)
    if legacy_match:
        raise TestFailure(
            "Vivado HDL source closure retains legacy speech name "
            f"{legacy_match.group(0)!r}"
        )
    closure_lower = closure.lower()
    require(
        all(name not in closure_lower for name in LEGACY_IMPLEMENTATION_NAMES),
        "Vivado HDL source closure retains a removed speech implementation",
    )


def test_phasor_mode_switch_and_reset_contract() -> None:
    source = read(MOCKINGBOARD_SV)

    require("PH_MOCKINGBOARD = 3'd0" in source and
            "PH_PHASOR       = 3'd5" in source and
            "PH_ECHOPLUS     = 3'd7" in source,
            "Phasor mode constants must match AppleWin's 0/5/7 modes")
    require("wire phasor_mode_hit =" in source and
            "(ab_read.addr[15:8] == 8'hC0)" in source and
            "wire [3:0] phasor_mode_nibble = {1'b1, slot_assign};" in source,
            "Phasor mode switch must decode C0nX for the assigned slot")
    require("if (!rstn || !ab_read.res || !card_enabled || mockingboard_only) begin\n"
            "        phasor_mode_q <= PH_MOCKINGBOARD;" in source,
            "Apple reset must return the card to Mockingboard-compatible mode")
    require("if (ab_read.addr[3]) begin\n"
            "            next_mode = PH_MOCKINGBOARD;" in source and
            "phasor_mode_q <= next_mode | ab_read.addr[2:0];" in source,
            "C0n8-C0nF accesses must clear then OR in Phasor mode bits")


def test_four_ay_chips_and_phasor_chip_selects() -> None:
    source = read(MOCKINGBOARD_SV)
    ym2149 = read(YM2149_SV)
    via = read(VIA6522_V)

    require(source.count("YM2149 psg") == 4,
            "Phasor card must instantiate four AY/YM PSG cores")
    require("wire psg_volume_mode_ay8913 = audio_control[25];" in source and
            source.count(".MODE(psg_volume_mode_ay8913)") == 4 and
            "// AY8910" in ym2149 and
            "volTable[32] = 8'h00;" in ym2149 and
            "volTable[62] = 8'hff;" in ym2149 and
            "volTable[63] = 8'hff;" in ym2149,
            "Mockingboard/Phasor PSGs must support runtime YM2149 vs AY8913 output tables")
    require("noise_reset <= (addr == 8'd6);" in ym2149 and
            "wire [4:0] noise_period = ymreg[6][4:0] ? ymreg[6][4:0] : 5'd1;" in ym2149 and
            "poly17 <= 17'h00001;" in ym2149 and
            "if (poly17[0] ^ poly17[1])" in ym2149 and
            "poly17 <= {(poly17[0] ^ poly17[2]), poly17[16:1]};" in ym2149 and
            "noise_gen_op <= {3{~noise_toggle}};" in ym2149 and
            "if (RESET || env_reset) begin" in ym2149,
            "AY noise/envelope timing must keep AppleWin-style reset and noise-toggle behavior")
    require("output wire [7:0] portb_bus" in via and
            "assign portb_bus = (portb_out & ddrb) | (portb_in & ~ddrb);" in via,
            "6522 must expose the AppleWin-style DDR-masked ORB bus view")
    require("(phasor_native && ab_read.addr[4])" in source and
            "(phasor_native && ab_read.addr[7])" in source and
            "via0_data_out | via1_data_out" in source and
            "ab_read.serve_en && ab_read.rw && (via0_hit || via1_hit)" in source,
            "Phasor native I/O must use AppleWin's bit4/bit7 VIA select and OR-read semantics")
    require("wire slot_rom_visible =" in source and
            "!sss.sw_intcxrom" in source and
            "((slot_assign != 3'd3) || sss.sw_slotc3rom)" in source and
            "sss.slot_access &&" not in source,
            "Phasor slot visibility must keep the motherboard ROM policy without the duplicate live decode")
    require("logic via0_cycle_hit_q;" in source and
            "logic via1_cycle_hit_q;" in source and
            "logic ssi_primary_cycle_hit_q;" in source and
            "logic ssi_secondary_cycle_hit_q;" in source and
            "else if (ab_read.serve_en) begin" in source and
            "via0_cycle_hit_q <= via0_hit;" in source and
            "via1_cycle_hit_q <= via1_hit;" in source and
            "ssi_primary_cycle_hit_q <= ssi_write_hit && ab_read.addr[6];" in source and
            "ssi_secondary_cycle_hit_q <= ssi_write_hit && ab_read.addr[5];" in source and
            "wire via0_strobe = ab_read.data_en && via0_cycle_hit_q;" in source and
            "wire via1_strobe = ab_read.data_en && via1_cycle_hit_q;" in source and
            "wire ssi_primary_write = ab_read.data_en && ssi_primary_cycle_hit_q;" in source and
            "wire ssi_secondary_write = ab_read.data_en && ssi_secondary_cycle_hit_q;" in source,
            "VIA and SSI writes must retain the authoritative SERVE decode through DATA")
    require("logic via0_ay0_selected_q = 1'b0;" in source and
            "logic via0_ay1_selected_q = 1'b0;" in source and
            "logic via1_ay0_selected_q = 1'b0;" in source and
            "logic via1_ay1_selected_q = 1'b0;" in source and
            "wire via0_psg_reset_func = !via0_portb_bus[2];" in source and
            "via0_psg_latch_func ? via0_ay1_cs" in source and
            "via1_psg_latch_func ? via1_ay1_cs" in source and
            "via0_psg_read_write_func && via0_ay0_cs && via0_ay0_selected_q" in source and
            "via0_psg_read_write_func && (via0_ay0_cs || via0_ay1_cs) && via0_ay1_selected_q" in source and
            "via1_psg_read_write_func && via1_ay0_cs && via1_ay0_selected_q" in source and
            "via1_psg_read_write_func && (via1_ay0_cs || via1_ay1_cs) && via1_ay1_selected_q" in source,
            "Phasor GAL model must preserve AppleWin's persistent AY chip-select side effects")
    require("wire via0_ay0_cs = !via0_portb_bus[4];" in source and
            "wire via0_ay1_cs = !via0_portb_bus[3];" in source and
            "wire via1_ay0_cs = !via1_portb_bus[4];" in source and
            "wire via1_ay1_cs = !via1_portb_bus[3];" in source,
            "Phasor native mode must use active-low bus-view bits 4 and 3 as AY chip selects")
    require(".BDIR(via0_ay1_drive ? via0_portb_bus[1] : 1'b0)" in source and
            ".BDIR(via1_ay1_drive ? via1_portb_bus[1] : 1'b0)" in source,
            "secondary AY bus control must keep the AppleWin Phasor GAL drive model")
    require("assign via0_porta_in = selected_psg_data(via0_ay0_drive, psg0_data_out,\n"
            "                                         via0_ay1_drive, psg2_data_out);" in source and
            "assign via1_porta_in = selected_psg_data(via1_ay0_drive, psg1_data_out,\n"
            "                                         via1_ay1_drive, psg3_data_out);" in source,
            "VIA Port A readback must select the addressed AY data bus")
    require("wire via_bus_clock = card_enabled && ab_read.data_en;" in source and
            "wire via_timer_clock = card_enabled && ab_read.sss_en;" in source and
            "wire phasor_timer_read_extra_clock = !mockingboard_only && phasor_native;" in source and
            "psg_ce_extra_q <= phasor_native && via_bus_clock;" in source and
            "wire psg_clock = via_bus_clock || psg_ce_extra_q;" in source and
            source.count(".slow_clock(via_timer_clock)") == 2 and
            source.count(".timer_read_extra_clock(phasor_timer_read_extra_clock)") == 2,
            "Phasor must keep PSG/register strobes on data_en while ticking VIA timers at the read-data setup phase")
    require("if (value[11]) begin\n"
            "        mix4_to_pcm = 16'sh7FFF;" in source and
            "mix4_to_pcm = $signed({1'b0, value[10:0], 4'b0000});" in source,
            "native Phasor gain must use Mockingboard scale with positive saturation")
    require("if (phasor_native) begin\n"
            "                base_l_next = mix_speech(" in source and
            "                    ssi0_audio);" in source and
            "                    ssi1_audio);" in source and
            "speech_audio_q" not in source and
            "sat_add16(ssi0_audio, ssi1_audio)" not in source and
            "psg_phasor_l_mix_q <= sum4_10(psg0_l_sum_q," in source and
            "mix4_to_pcm(psg_phasor_l_mix_q)" in source and
            "end else if (echo_plus) begin\n"
            "                base_l_next = mix_speech(" in source and
            "psg_echo_l_mix_q <= sum2_10(psg1_l_sum_q, psg3_l_sum_q);" in source and
            "mix2_to_pcm(psg_echo_l_mix_q)" in source and
            "psg_mockingboard_l_mix_q <= sum2_10(psg0_l_sum_q, psg1_l_sum_q);" in source and
            "mix2_to_pcm(psg_mockingboard_l_mix_q)" in source and
            "tone_bass_adjust_l_q <= audio_control_adjust(tone_bass_l_q, tone_bass_control_q);" in source and
            "tone_mid_adjust_l_q <= audio_control_adjust(tone_mid_l_q, tone_mid_control_q);" in source and
            "tone_volume_adjust_l_q <= audio_control_adjust(tone_apply_base_l_q, tone_volume_control_q);" in source and
            "tone_warm_adjust_l_q <= audio_control_adjust(tone_warm_l_q, tone_warm_control_q);" in source and
            "tone_warm_treble_adjust_l_q <= warmth_treble_adjust(tone_treble_l_q, tone_warm_control_q);" in source and
            "4'd1:    adjusted = widened >>> 3;" in source and
            "default: adjusted = widened;" in source and
            "bass_audio_control_adjust" not in source and
            "tone_shaped_l_q <= tone_base_ext_l_q +" in source and
            "tone_warm_shaped_l_q <= warm_shape_from21(tone_shaped_l_q, tone_warm_control_q);" in source and
            "audio_l <= sat16_from21(tone_warm_shaped_l_q);" in source,
            "native Phasor must mix four AYs, keep A5/A6 speech stereo, and prevent Echo+ VIA0 leakage")


def test_dual_native_ssi263_contract() -> None:
    source = read(MOCKINGBOARD_SV)
    top = read(APPLE_TOP_SV)
    voice = read(REPO_ROOT / "hdl" / "apple" / "ssi263_voice.sv")
    core = read(REPO_ROOT / "hdl" / "apple" / "ssi263_sc02_core.sv")
    audio = read(REPO_ROOT / "hdl" / "apple" / "ssi263_sc02_audio.sv")
    xck = read(REPO_ROOT / "hdl" / "apple" / "ssi263_xck_ce.sv")
    sources = read(REPO_ROOT / "hdl" / "hdl_sources.txt")

    required_sources = (
        "apple/ssi263_sc02_rom.mem",
        "apple/ssi263_sc02_core.sv",
        "apple/ssi263_sc02_audio.sv",
        "apple/ssi263_xck_ce.sv",
        "apple/ssi263_voice.sv",
    )
    require(all(path in sources for path in required_sources),
            "Vivado must include the native SC-02 ROM, core, audio, clock, and voice sources")
    removed_sources = (
        "apple/ssi263_formant_pkg.sv",
        "apple/sc01a_digital_core.sv",
        "apple/ssi263_formant_backend.sv",
        "apple/ssi263_bus_wrapper.sv",
    )
    require(all(path not in sources for path in removed_sources),
            "Vivado must not retain the former speech implementation sources")

    active_implementation = "\n".join((source, voice, core, audio)).lower()
    require("start_votrax" not in active_implementation and
            "has_sc01" not in active_implementation and
            "sc01a_digital_core" not in active_implementation and
            "ssi263_bus_wrapper" not in active_implementation and
            "ssi263_formant_backend" not in active_implementation,
            "the active card and voice implementation must contain only native SSI-263/SC-02 logic")

    require(source.count("ssi263_voice ssi263_") == 2 and
            "ssi263_voice ssi263_secondary_i" in source and
            "ssi263_voice ssi263_primary_i" in source and
            "SSI263_TYPE" not in source and
            "HAS_SC01" not in source,
            "A5 and A6 must each have one fixed SSI-263AP voice")
    require("ssi263_sc02_core #(" in voice and
            ".REVISION_AP(1'b1)" in voice and
            "ssi263_sc02_audio audio_i" in voice and
            ".div2(1'b1)" in voice,
            "the voice wrapper must bind one AP-revision native core and its native audio block")
    require(".write_active(ssi_write_active)" in voice and
            "assign write_end = write_active_q && !write_active;" in core,
            "the selected write level must latch in the native core on its falling edge")
    require(source.count(".apple_res(ab_read.res)") == 2 and
            ".rstn(rstn && card_enabled)" in voice and
            ".pd_rst_n(apple_res)" in voice,
            "card removal must hard-reset each core while Apple RESET uses the AP PD/RST pin")
    require(".d7_pending(ssi_d7)" in voice and
            ".ar_drive_low(ar_drive_low)" in voice and
            "assign d7_pending = pending_q;" in core and
            "assign ar_drive_low = pending_q && ar_enabled_q && !powered_down;" in core,
            "D7 pending state and the enabled active-low A/R pin must remain distinct")

    xck_instance = sv_instance_block(source, "ssi263_xck_ce ssi_xck_ce_i")
    card_instance = sv_instance_block(top, "mockingboard mb1(")
    require("input logic apple_q3_raw" in source and
            ".apple_q3_raw(apple_q3_pin)" in card_instance and
            ".q3_raw(apple_q3_raw)" in xck_instance and
            "(* ASYNC_REG = \"TRUE\" *) logic q3_sync1_q;" in xck and
            "(* ASYNC_REG = \"TRUE\" *) logic q3_sync2_q;" in xck and
            "assign xck_ce = q3_sync2_q && !q3_sync2_d_q;" in xck and
            "XCK_NUMERATOR_HZ" not in xck and "accumulator_q" not in xck and
            source.count(".xck_ce(ssi_xck_ce)") == 3,
            "physical Apple Q3 must cross two synchronizer flops and feed both voices")
    require("phasor_native" not in xck_instance and
            "card_mode" not in voice,
            "XCK and chip state must run independently of the Phasor mode switch")

    require("wire ssi_primary_write = ab_read.data_en && ssi_primary_cycle_hit_q;" in source and
            "wire ssi_secondary_write = ab_read.data_en && ssi_secondary_cycle_hit_q;" in source and
            ".ssi_write_active(ssi_secondary_write)" in source and
            ".ssi_write_active(ssi_primary_write)" in source,
            "A5 and A6 must select the secondary and primary write sockets")
    require("wire ssi_read_drive = ssi_secondary_read || ssi_primary_read;" in source and
            "ssi_secondary_read ? ssi0_d7 : ssi1_d7" in source and
            "!ab_read.addr[4] && !ab_read.addr[7] && (ab_read.addr[6] || ab_read.addr[5])" in source,
            "native A5/A6 reads must drive D7, with A5 priority on a dual select")
    require("wire ssi_visible_mode = mockingboard_mode || phasor_native;" in source and
            "&& !ab_read.rw && ssi_visible_mode;" in source and
            "ab_read.rw && phasor_native &&" in source,
            "Echo+ must hide speech bus reads and writes without stopping the voices")

    require("wire ssi0_direct_irq = phasor_native && ssi0_ar_drive_low;" in source and
            "wire ssi1_direct_irq = phasor_native && ssi1_ar_drive_low;" in source and
            ".ca1_in(mockingboard_mode ? !ssi0_ar_drive_low : 1'b1)" in source and
            ".ca1_in(mockingboard_mode ? !ssi1_ar_drive_low : 1'b1)" in source,
            "A/R must feed matching VIA CA1 pins only in Mockingboard mode and direct IRQ only in native mode")
    require(source.count(".ifr_set_ext(7'd0)") == 2 and
            source.count(".ifr_clr_ext(7'd0)") == 2,
            "speech must not inject synthetic external VIA IFR pulses")
    require("via0_votrax_mode" not in source and
            source.count(".RESET(via_reset || !via0_portb_bus[2])") == 2,
            "VIA PCR state must not alter VIA0 AY drive or reset behavior")

    require(source.count("                    ssi0_audio);") == 3 and
            source.count("                    ssi1_audio);") == 3 and
            "speech_audio_q" not in source and
            "sat_add16(ssi0_audio, ssi1_audio)" not in source,
            "A5 speech must remain left and A6 speech right in all three card modes")
    require("assign dbg_backend_done = response_boundary_ce;" in voice and
            "assign dbg_enable_ints = ar_enabled;" in voice,
            "speech debug taps must expose response boundaries and the A/R enable state")
    require(".fricative(fricative)" in voice and
            ".voiced(voiced)" in voice and
            ".noise_clock_ce(noise_clock_ce)" in voice and
            ".voice_toggle(voice_toggle)" in voice and
            ".fric1_sw(fric1_sw)" in voice and
            ".fric2_sw(fric2_sw)" in voice and
            ".closure(closure)" in voice,
            "the wrapper must carry the proved native source, switch, and closure controls into audio")


def test_ssi263_exhaustive_address_decode_model() -> None:
    ph_mockingboard = 0
    ph_phasor = 5
    ph_echo_plus = 7

    write_counts = {
        ph_mockingboard: [0, 0, 0],
        ph_phasor: [0, 0, 0],
        ph_echo_plus: [0, 0, 0],
    }
    read_counts = {
        ph_mockingboard: [0, 0, 0],
        ph_phasor: [0, 0, 0],
        ph_echo_plus: [0, 0, 0],
    }

    for mode in (ph_mockingboard, ph_phasor, ph_echo_plus):
        visible = mode in (ph_mockingboard, ph_phasor)
        for offset in range(256):
            a4 = bool(offset & 0x10)
            a5 = bool(offset & 0x20)
            a6 = bool(offset & 0x40)
            a7 = bool(offset & 0x80)

            secondary_write = visible and a5
            primary_write = visible and a6
            expected_secondary_write = mode != ph_echo_plus and a5
            expected_primary_write = mode != ph_echo_plus and a6
            require(secondary_write == expected_secondary_write and
                    primary_write == expected_primary_write,
                    f"write decode mismatch mode={mode} offset=${offset:02X}")
            write_counts[mode][0] += int(secondary_write)
            write_counts[mode][1] += int(primary_write)
            write_counts[mode][2] += int(secondary_write and primary_write)

            native_read_window = (mode == ph_phasor and not a4 and not a7 and
                                  (a5 or a6))
            secondary_read = native_read_window and a5
            primary_read = native_read_window and a6
            read_drive = secondary_read or primary_read
            expected_window = (mode == ph_phasor and
                               (offset & 0x90) == 0 and
                               (offset & 0x60) != 0)
            require(read_drive == expected_window,
                    f"read decode mismatch mode={mode} offset=${offset:02X}")

            if secondary_read and primary_read:
                read_counts[mode][2] += 1
            elif secondary_read:
                read_counts[mode][0] += 1
            elif primary_read:
                read_counts[mode][1] += 1

            for secondary_d7, primary_d7 in ((False, False), (False, True),
                                              (True, False), (True, True)):
                if read_drive:
                    observed_d7 = secondary_d7 if secondary_read else primary_d7
                    expected_d7 = secondary_d7 if a5 else primary_d7
                    require(observed_d7 == expected_d7,
                            f"D7 priority mismatch mode={mode} offset=${offset:02X}")

    require(write_counts[ph_mockingboard] == [128, 128, 64] and
            write_counts[ph_phasor] == [128, 128, 64] and
            write_counts[ph_echo_plus] == [0, 0, 0],
            "A5/A6 writes must cover both visible modes and no Echo+ offsets")
    require(read_counts[ph_mockingboard] == [0, 0, 0] and
            read_counts[ph_phasor] == [16, 16, 16] and
            read_counts[ph_echo_plus] == [0, 0, 0],
            "only the 48 native A5/A6 read offsets may expose D7")


def test_ssi263_dual_request_state_model() -> None:
    chips = [
        {"pending": True, "ar_enabled": True},
        {"pending": True, "ar_enabled": True},
    ]

    def ar_low(index: int) -> bool:
        chip = chips[index]
        return chip["pending"] and chip["ar_enabled"]

    def native_irq() -> bool:
        return ar_low(0) or ar_low(1)

    def mockingboard_ca1() -> tuple[bool, bool]:
        return (not ar_low(0), not ar_low(1))

    require(native_irq() and mockingboard_ca1() == (False, False),
            "two pending requests must assert both MB CA1 pins and native IRQ")

    chips[0]["pending"] = False
    require(chips[1]["pending"] and native_irq(),
            "acknowledging A5 must not clear A6 or release native IRQ")
    require(mockingboard_ca1() == (True, False),
            "acknowledging A5 must release only VIA-A CA1")

    chips[0]["pending"] = True
    chips[1]["pending"] = False
    require(native_irq() and mockingboard_ca1() == (False, True),
            "acknowledging A6 must leave A5 and VIA-A CA1 independent")

    chips[0]["pending"] = False
    require(not native_irq() and mockingboard_ca1() == (True, True),
            "native IRQ may release only after both independent requests clear")


def test_via_ifr_read_uses_committed_timer_flags() -> None:
    via = read(VIA6522_V)

    require("ADDR_IFR:                   data_out = {irq_p, ifr};" in via and
            "wire [6:0]  ifr_read" not in via and
            "addr == ADDR_IFR && timer1_undf" not in via and
            "addr == ADDR_IFR && timer2_undf" not in via,
            "late VIA IFR reads must expose only committed interrupt flags, not the pending-underflow phase")


def test_phasor_timer_low_reads_can_add_one_tick() -> None:
    via = read(VIA6522_V)

    require("input wire           timer_read_extra_clock" in via and
            "wire        timer_read_extra_tick =\n"
            "        timer_read_extra_clock &&\n"
            "        rd_strobe &&\n"
            "        ((addr == ADDR_TIMER1_LO) || (addr == ADDR_TIMER2_LO));" in via and
            "wire        timer_clock = slow_clock || timer_read_extra_tick;" in via,
            "VIA must expose an optional extra timer tick on T1L/T2L reads")
    require("else if (timer1_undf && timer_clock) begin" in via and
            "else if (timer_clock) begin" in via and
            "else if (timer1_undf && timer_clock && !acr[6])" in via and
            "wire        irq_t1_set = (timer1_undf && timer_clock &&" in via and
            "else if (timer2l_reload && timer_clock) begin" in via and
            "else if ((!acr[5] || pb6_trans) && timer_clock) begin" in via and
            "else if (timer2_undf && timer_clock)" in via and
            "wire        irq_t2_set = (timer2_undf && timer_clock &&" in via,
            "T1/T2 state and IRQ behavior must use the combined timer clock")


def test_via_timer_reads_preserve_pre_tick_value() -> None:
    source = read(MOCKINGBOARD_SV)
    via = read(VIA6522_V)

    require("wire via_timer_clock = card_enabled && ab_read.sss_en;" in source and
            "ab_read.serve_en && ab_read.rw && (via0_hit || via1_hit)" in source,
            "Mockingboard timer cadence must remain early while reads use late authoritative decode")
    require("reg [15:0] timer1_bus_value;" in via and
            "reg [15:0] timer2_bus_value;" in via and
            "else if (slow_clock) begin\n"
            "            timer1_bus_value <= timer1;\n"
            "            timer2_bus_value <= timer2;\n"
            "        end" in via and
            "ADDR_TIMER1_LO:             data_out = timer1_bus_value[7:0];" in via and
            "ADDR_TIMER1_HI:             data_out = timer1_bus_value[15:8];" in via and
            "ADDR_TIMER2_LO:             data_out = timer2_bus_value[7:0];" in via and
            "ADDR_TIMER2_HI:             data_out = timer2_bus_value[15:8];" in via,
            "late VIA timer reads must expose the value from before the current Apple-cycle tick")


def test_via_apple_reset_preserves_timer_latches() -> None:
    source = read(MOCKINGBOARD_SV)
    via = read(VIA6522_V)

    require("wire card_reset = !rstn || !card_enabled;" in source and
            "wire apple_reset = !ab_read.res;" in source and
            "wire via_reset = card_reset || apple_reset;" in source and
            source.count(".power_reset(card_reset)") == 2,
            "Mockingboard must distinguish power/card reset from Apple RESET for VIA state")
    require("input wire           power_reset" in via and
            "if (power_reset)\n            timer1_latch_lo <= 8'hff;" in via and
            "else if (!reset && wr_strobe && (addr == ADDR_TIMER1_LO ||" in via and
            "if (power_reset)\n            timer1_latch_hi <= 8'hff;" in via and
            "else if (!reset && wr_strobe && (addr == ADDR_TIMER1_HI ||" in via and
            "if (power_reset)\n            timer2_latch_lo <= 8'hff;" in via,
            "Apple RESET must clear VIA control/IRQ state but preserve programmed timer latch bytes")


def test_phasor_irq_is_suppressed_during_apple_reset() -> None:
    source = read(MOCKINGBOARD_SV)

    require("ab_write_q.assert_irq <= card_enabled && ab_read.res &&\n"
            "                                  (via0_irq | via1_irq | ssi0_direct_irq | ssi1_direct_irq);" in source,
            "Mockingboard must not drive IRQ while Apple RESET is asserted")


def test_phasor_slot4_disable_removes_the_card() -> None:
    source = read(MOCKINGBOARD_SV)
    top = read(APPLE_TOP_SV)
    instance = sv_instance_block(top, "mockingboard mb1(")

    require("input logic card_enable" in source and
            "wire card_enabled = card_enable && (slot_assign != 3'd0);" in source,
            "Phasor must combine the slot-4 enable bit with its assigned slot")
    require(".card_enable(card_slot4_enable)" in instance and
            ".ab_read(gate_ab(ab_read, card_slot4_enable))" in instance,
            "apple_top must remove both the Phasor bus and card state when slot 4 is disabled")
    require("wire card_reset = !rstn || !card_enabled;" in source and
            "if (!rstn || !card_enabled) begin" in source and
            "audio_l <= '0;" in source and
            "audio_r <= '0;" in source,
            "slot disable must reset the card and mute both audio channels")
    require("ab_write_q.assert_irq <= card_enabled && ab_read.res &&" in source and
            "if (!card_enabled || ab_read.data_en || ab_read.addr_en) begin" in source and
            "ab_write_q.wr_data_en <= 1'b0;" in source,
            "slot disable must release IRQ and any registered Apple-bus read drive")


def test_phasor_pan_registers_and_menu_schema() -> None:
    top = read(APPLE_TOP_SV)
    header = read(CONFIG_MENU_H)
    config = read(CONFIG_MENU_C)
    internal = read(CONFIG_MENU_INTERNAL_H)
    help_c = read(CONFIG_MENU_HELP_C)
    phasor_help = help_c[help_c.index("HELP(phasor,"):
                         help_c.index("/*  ETHERNET")]
    phasor_config = read(CONFIG_MENU_PHASOR_C)
    frontend_main = read(FRONTEND_MAIN_C)
    card_regs = read(CARD_CONTROL_REGS_H)
    vitis_script = read(CREATE_VITIS_WORKSPACE_PY)
    mockingboard = read(MOCKINGBOARD_SV)

    require("input logic [47:0] pan" in mockingboard and
            "input logic [31:0] audio_control" in mockingboard,
            "sound card inputs must cover twelve pan channels and packed audio controls")
    require("CARD_CTRL_REG_PHASOR_PAN_LO       = 8'h08" in top and
            "CARD_CTRL_REG_PHASOR_PAN_HI       = 8'h0A" in top and
            "CARD_CTRL_REG_PHASOR_AUDIO        = 8'h0C" in top and
            "PHASOR_PAN_RESET                 = 48'h5B5B5B5B5B5B" in top,
            "PL card-control registers must expose Phasor pan and audio-control words")
    require("void (*set_phasor_pan)(void *ctx, uint32_t pan_lo, uint32_t pan_hi);" in header and
            "void (*set_phasor_audio)(void *ctx," in header and
            "uint8_t mockingboard_pan[12];" in header and
            "int8_t phasor_warmth;" in header and
            "int8_t phasor_volume;" in header,
            "config menu platform must carry Phasor pan and audio-control values")
    version_match = re.search(r"#define APPLETINI_CFG_VERSION\s+(\d+)U", config)
    require(version_match is not None and int(version_match.group(1)) >= 100 and
            "#define MOCKINGBOARD_CHANNEL_COUNT 12U" in internal and
            "#define PHASOR_AUDIO_CONTROL_COUNT 4U" in internal and
            "#define PHASOR_WARMTH_DEFAULT 8" in internal and
            "#define PHASOR_PSG_MODE_YM2149 0U" in internal and
            "#define PHASOR_PSG_MODE_AY8913 1U" in internal and
            '"Phasor"' in config and
            '"phasor.slot4.enabled=%s\\n"' in phasor_config and
            '"phasor.pan.%u=%u\\n"' in phasor_config and
            "11U, 5U, 11U," in phasor_config and
            "5U, 11U, 5U" in phasor_config and
            '"phasor.eq.bass=%d\\n"' in phasor_config and
            '"phasor.eq.mid=%d\\n"' in phasor_config and
            '"phasor.eq.treble=%d\\n"' in phasor_config and
            '"phasor.warmth=%d\\n"' in phasor_config and
            '"phasor.volume=%d\\n"' in phasor_config and
            '"phasor.psg.mode=%s\\n"' in phasor_config and
            "for (uint32_t channel = 0U; channel < MOCKINGBOARD_CHANNEL_COUNT; ++channel)" in phasor_config,
            "saved config and visible menu must describe the Phasor card")
    require('strcmp(key, "phasor.slot4.enabled") == 0' in phasor_config and
            'strncmp(key, "phasor.pan.", 11U) == 0' in phasor_config and
            'strcmp(key, "phasor.warmth") == 0' in phasor_config and
            "menu->phasor_warmth = PHASOR_WARMTH_DEFAULT;" in phasor_config and
            'strcmp(key, "phasor.volume") == 0' in phasor_config and
            'strcmp(key, "phasor.psg.mode") == 0' in phasor_config and
            "phasor_psg_mode_text(value)" in phasor_config,
            "loader must accept the documented Phasor dot keys")
    require('"Warmth"' not in phasor_config and
            "PHASOR_AUDIO_CONTROL_WARMTH" not in phasor_config and
            "cmui_slider(fb," in phasor_config and
            'hgr_put_text(fb, bar_x + (8 * step) - 2, y, "0"' not in phasor_config and
            '"L"' in phasor_config and
            '"R"' in phasor_config and
            '"-"' in phasor_config and
            '"+"' in phasor_config,
            "Phasor tab must draw the documented AY/audio controls with shared sliders")
    require('"Volume Envelope"' in phasor_config and
            "const int psg_item_x = x + column_w + column_gap;" in phasor_config and
            "hgr_draw_value_item(fb,\n"
            "                        psg_item_x,\n"
            "                        audio_y,\n"
            "                        column_w,\n" in phasor_config and
            "phasor_psg_mode_label(menu->phasor_psg_ay_mode)" in phasor_config and
            "menu->item_focus == PHASOR_PSG_MODE_FOCUS" in phasor_config and
            "menu->phasor_psg_ay_mode = PHASOR_PSG_MODE_AY8913;" in phasor_config,
            "Phasor tab must expose the persisted PSG volume toggle next to the Bass slider")
    require("wire mockingboard_only = audio_control[26];" in mockingboard and
            "|| mockingboard_only" in mockingboard,
            "Phasor PL must lock to Mockingboard mode when audio_control bit 26 is set")
    require("#define CARD_CTRL_PHASOR_AUDIO_MOCKINGBOARD_ONLY_BIT (1UL << 26)" in card_regs and
            "packed |= CARD_CTRL_PHASOR_AUDIO_MOCKINGBOARD_ONLY_BIT;" in frontend_main and
            "uint8_t mockingboard_only)" in frontend_main,
            "PS must pack the Mockingboard-only lock into Phasor audio register bit 26")
    require("uint8_t phasor_mockingboard_only;" in header and
            "uint8_t mockingboard_only);" in header and
            'strcmp(key, "phasor.mockingboard.only") == 0' in phasor_config and
            '"phasor.mockingboard.only=%s\\n"' in phasor_config and
            "menu->item_focus == PHASOR_MOCKINGBOARD_ONLY_FOCUS" in phasor_config and
            '"Mockingboard Only"' in phasor_config,
            "config menu must persist and expose the Phasor Mockingboard-only toggle")
    require("#define PHASOR_MOCKINGBOARD_ONLY_FOCUS 1U" in internal and
            "#define PHASOR_PAN_FOCUS_BASE 2U" in internal and
            "y + row_h,\n"
            "                        w,\n"
            "                        (uint8_t)(menu->item_focus == PHASOR_MOCKINGBOARD_ONLY_FOCUS),\n"
            "                        menu->phasor_mockingboard_only,\n"
            "                        \"Mockingboard Only\")" in phasor_config,
            "Mockingboard-only toggle must sit directly under Enable in Slot 4")
    require("menu->phasor_mockingboard_only != 0U &&\n"
            "                     channel >= 6U" in phasor_config and
            "if (phasor_pan_channel_disabled(menu, channel) != 0U) {\n"
            "            return 1U;\n"
            "        }" in phasor_config and
            "if (phasor_pan_channel_disabled(menu, channel) != 0U) {\n"
            "            return;\n"
            "        }" in phasor_config and
            "const uint8_t dimmed = phasor_pan_channel_disabled(menu, i);" in phasor_config,
            "Mockingboard-only mode must disable AY2/AY3 pan editing and dim those rows")
    require('"SSI-263 Speech"' not in phasor_config and
            "PHASOR_SPEECH_BACKEND_FOCUS" not in phasor_config and
            "phasor_speech_backend_label" not in phasor_config and
            "phasor_speech_formant" not in phasor_config and
            "phasor_speech_formant" not in config and
            "phasor_speech_formant" not in header,
            "Phasor tab must use the fixed SSI263 backend without a selector")
    require("#define CARD_CTRL_PHASOR_PAN_LO_REG        CARD_CTRL_REG_ADDR(0x08U)" in card_regs and
            "#define CARD_CTRL_PHASOR_PAN_HI_REG        CARD_CTRL_REG_ADDR(0x0AU)" in card_regs and
            "#define CARD_CTRL_PHASOR_AUDIO_REG         CARD_CTRL_REG_ADDR(0x0CU)" in card_regs and
            "REG_WRITE(CARD_CTRL_PHASOR_PAN_HI_REG, pan_hi & 0x00FFFFFFUL);" in frontend_main and
            "phasor_audio_pack5(warmth) << 15" in frontend_main and
            "phasor_audio_pack5(volume) << 20" in frontend_main and
            "((uint32_t)(psg_ay_mode != 0U)) << 25" in frontend_main and
            "speech_formant" not in frontend_main and
            "REG_WRITE(CARD_CTRL_PHASOR_AUDIO_REG, packed);" in frontend_main,
            "frontend must write Phasor pan and audio-control registers")
    require("tone_bass_control_q <= clamp_audio_control(audio_control[4:0]);" in mockingboard and
            "tone_mid_control_q <= clamp_audio_control(audio_control[9:5]);" in mockingboard and
            "tone_treble_control_q <= clamp_audio_control(audio_control[14:10]);" in mockingboard and
            "tone_warm_control_q <= clamp_audio_control(audio_control[19:15]);" in mockingboard and
            "tone_volume_control_q <= clamp_audio_control(audio_control[24:20]);" in mockingboard,
            "PL must decode five packed signed 5-bit Phasor audio controls")
    require("PHASOR_AUDIO_RESET               = 32'h0204_0000" in top and
            "bit 25 selects PSG volume table (0=YM, 1=AY)." in top and
            "bit 26 selects SSI263 backend" not in top,
            "PL reset/default register value must keep AY PSG mode and the current audio-control layout")
    require('"../../../ps_sources/frontend/config_menu_phasor.c"' in vitis_script and
            '"../../../ps_sources/frontend/config_menu_main_tabs.c"' in vitis_script and
            '"../../../ps_sources/frontend/config_menu_device_tabs.c"' in vitis_script,
            "Vitis source registration must include the split menu modules")
    phasor_help_items = re.findall(r"OVERRIDE\(([^,]+),", phasor_help)
    require(phasor_help_items == [
                "PHASOR_MOCKINGBOARD_ONLY_FOCUS",
                "PHASOR_AUDIO_FOCUS_BASE + PHASOR_AUDIO_CONTROL_BASS",
                "PHASOR_AUDIO_FOCUS_BASE + PHASOR_AUDIO_CONTROL_MID",
                "PHASOR_AUDIO_FOCUS_BASE + PHASOR_AUDIO_CONTROL_TREBLE",
                "PHASOR_AUDIO_FOCUS_BASE + PHASOR_AUDIO_CONTROL_VOLUME",
                "PHASOR_PSG_MODE_FOCUS",
            ] and
            "Phasor sound card:" in phasor_help and
            "Two SSI-263AP (SC-02) speech chips." in phasor_help and
            "TAB_WITH_OVERRIDES(CONFIG_TAB_MOCKINGBOARD, phasor, phasor_overrides)" in help_c and
            re.search(r'"\s*\n\s*"', phasor_help) is None and
            all(len(line) <= 100 for line in
                re.findall(r'^\s*"([^"]*)', phasor_help, re.MULTILINE)) and
            "cmui_help_panel(fb, rect, \"Help\", lines, count);" in config and
            "AUDIO CHANGES APPLY IMMEDIATELY" not in phasor_config and
            "LEFT/RIGHT ADJUSTS" not in phasor_config and
            "NO SSI-263 SPEECH" not in phasor_config,
            "Phasor help must provide the six requested item overrides with panel-safe lines")
    require("CARD_CTRL_REG_SSI263_SAMPLE_BASE" not in top and
            "CARD_CTRL_SSI263_SAMPLE_BASE_REG" not in card_regs and
            "card_control_publish_ssi263_samples" not in frontend_main and
            "ssi263_phoneme_samples" not in vitis_script,
            "frontend must not expose a PS-side SSI263 sample table")


def test_phasor_is_apple_bus_driven() -> None:
    mockingboard = read(MOCKINGBOARD_SV)
    apple_top = read(APPLE_TOP_SV)
    card_regs = read(CARD_CONTROL_REGS_H)
    sources = "\n".join([mockingboard, apple_top, card_regs]).lower()

    require("phasor_host" not in sources and
            "host_psg" not in sources and
            "host_ssi" not in sources and
            "host_audio" not in sources,
            "Phasor RTL and registers must not expose a PS audio-control path")
    require(".apple_res(ab_read.res)" in mockingboard and
            "psg_ce_extra_q <= phasor_native && via_bus_clock;" in mockingboard and
            "if (phasor_native) begin" in mockingboard,
            "normal Apple-driven Phasor reset, clock, and mix paths must remain direct")


def test_virtual_irq_uses_bidirectional_open_collector_lane() -> None:
    wrapper = read(APPLE_BUS_WRAPPER_SV)
    apple_top = read(APPLE_TOP_SV)
    top = read(APPLETINI_YARZ_TOP_SV)

    require("inout  wire                   apple_irq_pin" in wrapper and
            "wire apple_irq_drive_low = !physical_bus_isolate &&" in wrapper and
            "ab_write.assert_irq &&" in wrapper and
            "(!host_is_iiplus || !irq_rearm_release_q);" in wrapper and
            "assign apple_irq_pin = apple_irq_drive_low ? 1'b0 : 1'bz;" in wrapper and
            "apple_irq_n_out" not in wrapper,
            "bus wrapper must assert IRQ low/high-Z through the physical "
            "bidirectional lane, with II+-only phase-locked re-arm notches")
    require("inout apple_irq_pin" in apple_top and
            "apple_irq_n_out" not in apple_top,
            "apple_top must preserve the physical IRQ lane as bidirectional")
    require("inout  logic        a2fpga_irq_n" in top and
            "assign a2ctrl_irq_n = 1'b1;" in top and
            "apple_irq_n_out" not in top,
            "top-level must use A2FPGA.IRQ because the planned A2CTRL.IRQ pin is DNC")


TESTS = [
    test_native_ssi263_hdl_source_closure,
    test_phasor_mode_switch_and_reset_contract,
    test_four_ay_chips_and_phasor_chip_selects,
    test_dual_native_ssi263_contract,
    test_ssi263_exhaustive_address_decode_model,
    test_ssi263_dual_request_state_model,
    test_via_ifr_read_uses_committed_timer_flags,
    test_phasor_timer_low_reads_can_add_one_tick,
    test_via_timer_reads_preserve_pre_tick_value,
    test_via_apple_reset_preserves_timer_latches,
    test_phasor_irq_is_suppressed_during_apple_reset,
    test_phasor_slot4_disable_removes_the_card,
    test_phasor_pan_registers_and_menu_schema,
    test_phasor_is_apple_bus_driven,
    test_virtual_irq_uses_bidirectional_open_collector_lane,
]


def main() -> int:
    failures = []
    for test in TESTS:
        try:
            test()
        except TestFailure as exc:
            failures.append((test.__name__, str(exc)))
            print(f"FAIL {test.__name__}: {exc}")
        else:
            print(f"PASS {test.__name__}")
    if failures:
        print(f"{len(TESTS) - len(failures)} of {len(TESTS)} Phasor tests passed; "
              f"{len(failures)} failed")
        return 1
    print(f"{len(TESTS)} Phasor tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
