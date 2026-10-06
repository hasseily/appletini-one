#!/usr/bin/env python3
"""Source-level regression tests for the Phasor-compatible sound card.

These tests run without Vivado or hardware:

    python scripts/test_phasor_card.py
"""

from __future__ import annotations

import importlib.util
import re
import sys
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
COMPARE_SSI263_FORMANT_PY = REPO_ROOT / "scripts" / "compare_ssi263_formant.py"


class TestFailure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def sv_instance_block(source: str, instance_header: str) -> str:
    require(instance_header in source, f"missing SystemVerilog instance: {instance_header}")
    return source.split(instance_header, 1)[1].split(");", 1)[0]


def load_formant_compare_module():
    spec = importlib.util.spec_from_file_location("compare_ssi263_formant",
                                                 COMPARE_SSI263_FORMANT_PY)
    require(spec is not None and spec.loader is not None,
            "failed to load SSI263 formant comparator module")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


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
    require("mix4_to_wide = $signed({1'b0, value, 4'b0000});" in source and
            "input signed [17:0] speech" in source and
            "logic signed [18:0] combined;" in source,
            "AY scale must stay fixed and the complete SSI/AY sum must fit before clipping")
    require("if (phasor_native) begin\n"
            "                base_l_next = mix_speech(" in source and
            "mix4_to_wide(psg_phasor_l_mix_q),\n"
            "                    ssi_mix_l);" in source and
            "mix4_to_wide(psg_phasor_r_mix_q),\n"
            "                    ssi_mix_r);" in source and
            "speech_audio_q" not in source and
            "psg_phasor_l_mix_q <= sum4_10(psg0_l_sum_q," in source and
            "mix4_to_wide(psg_phasor_l_mix_q)" in source and
            "end else if (echo_plus) begin\n"
            "                base_l_next = mix_speech(" in source and
            "psg_echo_l_mix_q <= sum2_10(psg1_l_sum_q, psg3_l_sum_q);" in source and
            "mix2_to_wide(psg_echo_l_mix_q)" in source and
            "psg_mockingboard_l_mix_q <= sum2_10(psg0_l_sum_q, psg1_l_sum_q);" in source and
            "mix2_to_wide(psg_mockingboard_l_mix_q)" in source and
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
            "native Phasor must mix four AYs while Echo+ must not leak disabled VIA0 audio")


def test_ssi263_applewin_behavior_contract() -> None:
    source = read(MOCKINGBOARD_SV)
    via = read(VIA6522_V)
    sources = read(REPO_ROOT / "hdl/hdl_sources.txt")
    apple_top = read(APPLE_TOP_SV)
    top_shell = read(APPLETINI_YARZ_TOP_SV)
    create_project = read(CREATE_PROJECT_TCL)
    voice = read(REPO_ROOT / "hdl/apple/ssi263_voice.sv")
    bus_wrapper = read(REPO_ROOT / "hdl/apple/ssi263_bus_wrapper.sv")
    for module in ("native_controller", "native_pitch", "native_source", "native_tract",
                   "native_engine", "response_timing", "stereo_mixer", "bus_wrapper", "voice"):
        require(f"apple/ssi263_{module}.sv" in sources,
                f"Vivado must include the native SSI {module}")
    for legacy in ("ssi263_formant_backend", "sc01a_digital_core", "ssi263_formant_pkg"):
        require(f"apple/{legacy}.sv" not in sources and legacy not in bus_wrapper,
                f"Production SSI must not depend on the legacy {legacy}")
    require("ssi263_native_engine" in bus_wrapper and "ssi263_response_timing" in bus_wrapper and
            "ssi263_bus_wrapper" in voice,
            "Bus wrapper must select native audio and retain separate SSI response timing")
    require(source.count(".SSI263_TYPE(2)") == 2 and source.count(".HAS_SC01(1'b0)") == 2,
            "Phasor must expose two SSI263AP sockets")
    require("output logic               audio_valid" in voice and
            "output logic               audio_valid" in bus_wrapper,
            "Both wrappers must expose the completed native PCM sample strobe")
    require(source.count(".apple_res(ab_read.res)") == 2,
            "Apple RESET must directly reset both SSI263 voices")
    require("wire mockingboard_mode = (phasor_mode_q == PH_MOCKINGBOARD);" in source and
            "(mockingboard_mode && !ab_read.addr[7])" in source and
            "(mockingboard_mode && ab_read.addr[7])" in source,
            "Mockingboard mode must preserve AppleWin's full VIA half-page aliases")
    require("wire ssi_primary_write = ssi_write_region && ab_read.addr[6];" in source and
            "wire ssi_secondary_write = ssi_write_region && ab_read.addr[5];" in source and
            "wire ssi_native_read_region =" in source and
            "!ab_read.addr[4] && !ab_read.addr[7]" in source,
            "SSI263 writes and native D7 reads must use AppleWin's Phasor address decode")
    require("via0_votrax_mode" not in source and
            "via0_votrax_write" not in source and
            source.count(".votrax_write_strobe(1'b0)") == 2 and
            source.count(".votrax_wdata(8'h00)") == 2,
            "dual-SSI Phasor wiring must not expose a Votrax bus device or alter AY control")
    require("input wire [6:0] ifr_set_ext" in via and
            "input wire [6:0] ifr_clr_ext" in via and
            "output wire [7:0] pcr_out" in via and
            "output wire [7:0] ddrb_out" in via,
            "VIA must expose PCR/DDRB and external IFR hooks for SSI263/SC-01 edge cases")
    require("input logic audio_sample_tick" in source and
            ".audio_sample_tick(audio_sample_tick)" in apple_top and
            ".audio_tick(audio_sample_tick)" in source and
            "input  logic               audio_tick" in voice and
            "input  logic               audio_tick" in bus_wrapper and
            "sample_audio_tick" not in source and
            "formant_audio_tick" not in source,
            "SSI263 formant playback must run from the 48 kHz mixer tick with no 22.05 kHz sample clock")
    require("via_ifr_set[IFR_CA1_SSI263] <= 1'b1;" in bus_wrapper and
            "via_ifr_set[IFR_CB1_VOTRAX] <= 1'b1;" in bus_wrapper and
            "via_ifr_clr[IFR_CB1_VOTRAX] <= 1'b1;" in bus_wrapper and
            "direct_irq_q <= 1'b1;" in bus_wrapper,
            "SSI263 completion must route through Mockingboard VIA IFR, SC-01 IFR, and Phasor direct IRQ")
    mode_change = bus_wrapper[
        bus_wrapper.index("if (card_mode != card_mode_prev_q) begin"):
        bus_wrapper.index("if (ssi_write_strobe && SSI263_TYPE != SSI263_EMPTY) begin")
    ]
    require("if (current_enable_ints_q && card_mode == PH_MOCKINGBOARD && !via_pcr[0]) begin\n"
            "                                via_ifr_set[IFR_CA1_SSI263] <= 1'b1;" in mode_change and
            "end else if (current_enable_ints_q && card_mode == PH_PHASOR) begin\n"
            "                                direct_irq_q <= 1'b1;" in mode_change and
            "if (card_mode != PH_PHASOR) begin\n"
            "                        direct_irq_q <= 1'b0;" in mode_change,
            "SSI263 mode changes must re-route a pending D7 request into the new "
            "mode's IRQ path (mb-audit T263_8: PH->MB sets IFR.IxR_SSI263, "
            "->PH asserts the direct IRQ) while masking the direct IRQ outside PH")
    require("input logic [31:0] ssi_sample_base_addr" not in source and
            "Axi3_read_if.master ssi_sample_read" not in source and
            "ssi263_ddr_fetcher" not in source,
            "Mockingboard must use the on-chip SSI263 backend without a DDR sample interface")
    require("Axi3_read_if.master  axi_audio_read" in apple_top and
            "ssi_audio_read" not in apple_top and
            "Axi3_read_if #(.ADDR_WIDTH(32), .DATA_WIDTH(64)) disk2_sound_read();" in apple_top and
            ".sample_read(disk2_sound_read)" in apple_top and
            "axi3_read_arbiter_2 audio_read_arbiter_i" not in apple_top and
            "assign axi_audio_read.araddr = disk2_sound_read.araddr;" in apple_top and
            "assign disk2_sound_read.rvalid = axi_audio_read.rvalid;" in apple_top and
            ".axi_audio_read(s_axi_hp2_read)" in top_shell and
            "S_AXI_HP2_0" in top_shell and
            "CONFIG.PCW_USE_S_AXI_HP2 {1}" in create_project and
            "processing_system7_0/S_AXI_HP2" in create_project and
            "processing_system7_0/S_AXI_HP2/HP2_DDR_LOWOCM" in create_project,
            "Disk II audio sample reads must keep using the dedicated Zynq HP2 audio path")




def test_retained_baseline_rate_and_inflection_reference() -> None:
    formant_compare = load_formant_compare_module()
    data = formant_compare.parse_generated_data(formant_compare.FORMANT_PKG)

    control_steps = []
    for rate_inflection in (0x08, 0xA8, 0xF8):
        model = formant_compare.HdlLikeFormant(
            data.phones,
            inflection=0x20,
            rate_inflection=rate_inflection,
            amplitude=15,
        )
        model.start_phone(0x00, 0xC0)
        control_steps.append(model.control_speed_step())
    require(control_steps == [1, 1, 1],
            "SSI263 RATE must not change articulation cadence")

    frame_ticks = [4096 * (16 - rate) for rate in range(16)]
    require(all(left > right for left, right in
                zip(frame_ticks, frame_ticks[1:])) and
            frame_ticks[0] == 65536 and frame_ticks[-1] == 4096,
            "SSI263 RATE frame formula must cover R=0 through R=15")

    periods = [
        formant_compare.HdlLikeFormant(data.phones,
                                       inflection=inflection,
                                       rate_inflection=0xA8,
                                       amplitude=15).pitch_period_limit()
        for inflection in (0x00, 0x20, 0x40, 0x80, 0xC0, 0xFE, 0xFF)
    ]
    require(all(left >= right for left, right in zip(periods, periods[1:])) and
            periods[0] > periods[-1] and
            (periods[0] - periods[-1]) >= 250 and
            periods[-1] >= 1,
            "SSI263 INFLECT/RATEINF pitch target must use the 10-bit data-sheet period law")

    slow_art = formant_compare.HdlLikeFormant(data.phones, articulation=0)
    fast_art = formant_compare.HdlLikeFormant(data.phones, articulation=7)
    slow_art.start_phone(0x00, 0xC0)
    fast_art.start_phone(0x00, 0xC0)
    for _ in range(48):
        slow_art.chip_update(1)
        fast_art.chip_update(1)
    require(fast_art.cur_f1 > slow_art.cur_f1,
            "SSI263 articulation bits must change HDL-like formant transition speed")

    ramp_model = formant_compare.HdlLikeFormant(data.phones,
                                                inflection=0x00,
                                                rate_inflection=0x08,
                                                amplitude=15)
    ramp_model.start_phone(0x00, 0xC0)
    ramp_model.inflection = 0xA0
    ramp_model.rate_inflection = 0xA8
    ramp_model.start_phone(0x00, 0xC0)
    initial_active = ramp_model.active_inflection
    ramp_model.advance_inflection()
    require(ramp_model.active_inflection != initial_active and
            ramp_model.active_inflection != ramp_model.target_inflection,
            "SSI263 transitioned inflection must ramp toward the target instead of applying immediately")

    slow_ramp = formant_compare.HdlLikeFormant(data.phones,
                                               inflection=0x00,
                                               rate_inflection=0x08,
                                               amplitude=15)
    fast_ramp = formant_compare.HdlLikeFormant(data.phones,
                                               inflection=0x00,
                                               rate_inflection=0x08,
                                               amplitude=15)
    slow_ramp.start_phone(0x00, 0xC0)
    fast_ramp.start_phone(0x00, 0xC0)
    slow_ramp.inflection = 0xA0
    fast_ramp.inflection = 0xA7
    slow_ramp.rate_inflection = 0xA8
    fast_ramp.rate_inflection = 0xA8
    slow_ramp.start_phone(0x00, 0xC0)
    fast_ramp.start_phone(0x00, 0xC0)
    slow_ramp.advance_inflection()
    fast_ramp.advance_inflection()
    require(((fast_ramp.active_inflection >> 6) & 0x1F) >
            ((slow_ramp.active_inflection >> 6) & 0x1F),
            "SSI263 inflection slope bits must change target-pitch ramp speed")


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
            "PHASOR_PAN_RESET                 = 56'hF05B5B5B5B5B5B" in top,
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
            '"Slot 4 Phasor"' in config and
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
            "REG_WRITE(CARD_CTRL_PHASOR_PAN_HI_REG, pan_hi);" in frontend_main and
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
    require("PHASOR_AUDIO_RESET               = 32'h1204_0000" in top and
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
                "PHASOR_SSI_VOLUME_FOCUS",
                "PHASOR_SSI_PAN_FOCUS_BASE",
                "PHASOR_SSI_PAN_FOCUS_BASE + 1U",
            ] and
            "Phasor sound card:" in phasor_help and
            "two SSI-263 speech chips." in phasor_help and
            "TAB_WITH_OVERRIDES(CONFIG_TAB_MOCKINGBOARD, phasor, phasor_overrides)" in help_c and
            re.search(r'"\s*\n\s*"', phasor_help) is None and
            all(len(line) <= 100 for line in
                re.findall(r'^\s*"([^"]*)', phasor_help, re.MULTILINE)) and
            "cmui_help_panel(fb, rect, \"Help\", lines, count);" in config and
            "AUDIO CHANGES APPLY IMMEDIATELY" not in phasor_config and
            "LEFT/RIGHT ADJUSTS" not in phasor_config and
            "NO SSI-263 SPEECH" not in phasor_config,
            "Phasor help must cover all audio controls with panel-safe lines")
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
            "bidirectional lane only while the physical bus is active, with "
            "II+-only phase-locked re-arm notches")
    require("inout apple_irq_pin" in apple_top and
            "apple_irq_n_out" not in apple_top,
            "apple_top must preserve the physical IRQ lane as bidirectional")
    require("inout  logic        a2fpga_irq_n" in top and
            "assign a2ctrl_irq_n = 1'b1;" in top and
            "apple_irq_n_out" not in top,
            "top-level must use A2FPGA.IRQ because the planned A2CTRL.IRQ pin is DNC")


TESTS = [
    test_phasor_mode_switch_and_reset_contract,
    test_four_ay_chips_and_phasor_chip_selects,
    test_ssi263_applewin_behavior_contract,
    test_retained_baseline_rate_and_inflection_reference,
    test_via_ifr_read_uses_committed_timer_flags,
    test_phasor_timer_low_reads_can_add_one_tick,
    test_via_timer_reads_preserve_pre_tick_value,
    test_via_apple_reset_preserves_timer_latches,
    test_phasor_irq_is_suppressed_during_apple_reset,
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
