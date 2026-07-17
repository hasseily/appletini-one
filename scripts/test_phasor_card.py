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
    require("if (value[11]) begin\n"
            "        mix4_to_pcm = 16'sh7FFF;" in source and
            "mix4_to_pcm = $signed({1'b0, value[10:0], 4'b0000});" in source,
            "native Phasor gain must use Mockingboard scale with positive saturation")
    require("if (phasor_native) begin\n"
            "                base_l_next = mix_speech(" in source and
            "speech_audio_q <= sat_add16(ssi0_audio, ssi1_audio);" in source and
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
            "native Phasor must mix four AYs while Echo+ must not leak disabled VIA0 audio")


def test_ssi263_applewin_behavior_contract() -> None:
    source = read(MOCKINGBOARD_SV)
    via = read(VIA6522_V)
    sources = read(REPO_ROOT / "hdl" / "hdl_sources.txt")
    apple_top = read(APPLE_TOP_SV)
    top_shell = read(APPLETINI_YARZ_TOP_SV)
    create_project = read(CREATE_PROJECT_TCL)
    voice = read(REPO_ROOT / "hdl" / "apple" / "ssi263_voice.sv")
    bus_wrapper = read(REPO_ROOT / "hdl" / "apple" / "ssi263_bus_wrapper.sv")
    formant_backend = read(REPO_ROOT / "hdl" / "apple" / "ssi263_formant_backend.sv")
    sc01a_core = read(REPO_ROOT / "hdl" / "apple" / "sc01a_digital_core.sv")
    formant_pkg = read(REPO_ROOT / "hdl" / "apple" / "ssi263_formant_pkg.sv")
    formant_compare = read(COMPARE_SSI263_FORMANT_PY)
    card_regs = read(REPO_ROOT / "ps_sources" / "frontend" / "card_control_regs.h")
    formant_instance = sv_instance_block(bus_wrapper, "ssi263_formant_backend formant_backend_i")

    require(source.count(".apple_res(ab_read.res)") == 2,
            "Apple RESET must directly reset both SSI263 voices")
    require("apple/ssi263_formant_pkg.sv" in sources and
            "apple/sc01a_digital_core.sv" in sources and
            "apple/ssi263_formant_backend.sv" in sources and
            "apple/ssi263_bus_wrapper.sv" in sources and
            "apple/ssi263_voice.sv" in sources and
            "apple/ssi263_phoneme_pkg.sv" not in sources and
            "apple/ssi263_ddr_fetcher.sv" not in sources and
            "apple/ssi263_sample_backend.sv" not in sources and
            "ssi263_phoneme_samples" not in sources,
            "Vivado must use only the formant SSI263 backend, with no sample package/backend/fetcher sources")
    require("wire mockingboard_mode = (phasor_mode_q == PH_MOCKINGBOARD);" in source and
            "(mockingboard_mode && !ab_read.addr[7])" in source and
            "(mockingboard_mode && ab_read.addr[7])" in source,
            "Mockingboard mode must preserve AppleWin's full VIA half-page aliases")
    require("wire ssi_primary_write = ssi_write_region && ab_read.addr[6];" in source and
            "wire ssi_secondary_write = ssi_write_region && ab_read.addr[5];" in source and
            "wire ssi_native_read_region =" in source and
            "!ab_read.addr[4] && !ab_read.addr[7]" in source,
            "SSI263 writes and native D7 reads must use AppleWin's Phasor address decode")
    require(".SSI263_TYPE(0)" in source and ".HAS_SC01(1'b1)" in source and
            ".SSI263_TYPE(2)" in source and ".HAS_SC01(1'b0)" in source,
            "default Phasor population must match AppleWin: secondary SSI empty, SC-01, primary SSI263AP")
    require("via0_votrax_write" in source and
            "via0_votrax_wdata = (ab_read.data & via0_ddrb) | (via0_ddrb ^ 8'hFF)" in source,
            "SC-01 writes must use AppleWin's DDRB-masked VIA-A ORB bus view")
    require("input wire [6:0] ifr_set_ext" in via and
            "input wire [6:0] ifr_clr_ext" in via and
            "output wire [7:0] pcr_out" in via and
            "output wire [7:0] ddrb_out" in via,
            "VIA must expose PCR/DDRB and external IFR hooks for SSI263/SC-01 edge cases")
    require("ssi263_bus_wrapper" in voice and
            "ssi263_formant_backend" in bus_wrapper and
            "ssi263_sample_backend" not in bus_wrapper,
            "SSI263 must be split into an Apple-visible bus wrapper and formant-only audio backend")
    require("input logic audio_sample_tick" in source and
            ".audio_sample_tick(audio_sample_tick)" in apple_top and
            ".audio_tick(audio_sample_tick)" in source and
            "input  logic               audio_tick" in voice and
            "input  logic               audio_tick" in bus_wrapper and
            "sample_audio_tick" not in source and
            "formant_audio_tick" not in source,
            "SSI263 formant playback must run from the 48 kHz mixer tick with no 22.05 kHz sample clock")
    require("formant_enable" not in bus_wrapper and
            "assign audio = formant_audio;" in bus_wrapper and
            "assign formant_backend_start = backend_start_q;" in bus_wrapper and
            "assign formant_backend_reset = backend_warm_reset;" in bus_wrapper and
            "assign backend_done = formant_backend_done;" in bus_wrapper and
            ".audio_tick(audio_tick)" in formant_instance and
            ".warm_reset(formant_backend_reset)" in formant_instance and
            ".start(formant_backend_start)" in formant_instance and
            ".phoneme_done(formant_backend_done)" in formant_instance and
            ".audio(formant_audio)" in formant_instance and
            ".start_sc01_phone(backend_sc01_phone_q)" in formant_instance and
            "backend_phoneme_q <= votrax ? votrax_to_ssi263(phoneme) : phoneme;" in bus_wrapper and
            "backend_sc01_phone_q <= votrax ? phoneme : ssi263_to_sc01_phone(phoneme);" in bus_wrapper and
            "logic backend_started_this_cycle;" in bus_wrapper and
            "backend_started_this_cycle = 1'b1;" in bus_wrapper and
            "if (backend_done && !backend_started_this_cycle) begin" in bus_wrapper and
            "output logic        phoneme_done" in formant_backend and
            "assign phoneme_done = phoneme_done_q;" in formant_backend and
            ".phoneme_done(core_phoneme_done)" in formant_backend and
            "output logic       phoneme_done" in sc01a_core and
            "phoneme_done <= 1'b1;" in sc01a_core and
            "end else if (audio_tick) begin" in sc01a_core and
            "ssi263_phoneme_length" not in formant_backend and
            "sample_remaining_q" not in formant_backend and
            "repeat_phoneme" not in formant_backend,
            "SSI263 formant mode must use independent formant-duration completion, not sample lengths")
    require("CRC32 fc416227" in formant_pkg and
            "SHA1 1d6da90b1807a01b5e186ef08476119a862b5e6d" in formant_pkg and
            "function automatic logic [63:0] sc01a_word_by_phone" in formant_pkg and
            "function automatic logic [5:0] ssi263_to_sc01_audio_phone" in formant_pkg and
            "ssi263_to_sc01_audio_phone = ssi263_to_sc01_phone(phoneme);" in formant_pkg and
            "durphon_key" not in formant_pkg and
            "8'hC7: ssi263_to_sc01_audio_phone" not in formant_pkg and
            "6'h28: ssi263_to_sc01_audio_phone" not in formant_pkg and
            "sc01a_f1 = {word[0], word[7], word[14], word[21]};" in formant_pkg and
            "sc01a_cld = {word[34], word[32], word[30], word[28]};" in formant_pkg and
            "sc01a_duration = {~word[37], ~word[38], ~word[39], ~word[40]," in formant_pkg and
            "sc01a_formant_duration" not in formant_pkg and
            "rom_duration <= sc01a_duration(phone);" in sc01a_core and
            "sc01a_pause = (phone == 6'h03) || (phone == 6'h3E);" in formant_pkg and
            "import ssi263_formant_pkg::*;" in formant_backend and
            "sc01a_digital_core digital_core_i" in formant_backend and
            "mirror_digital_core_state();" in formant_backend and
            "run_chip_update(" not in formant_backend and
            "advance_pitch_noise(" not in formant_backend and
            "noise_next = {noise_q[13:0], noise_in};" not in formant_backend and
            "Galibert's MAME model uses the SC-01A 15-bit NXOR noise register." in sc01a_core and
            "logic [14:0] noise_q;" in sc01a_core and
            "sc01_noise_next" in sc01a_core and
            "sc01_noise_out = ~(^state[14:13]);" in sc01a_core and
            "cur_fc <= interpolate8(cur_fc, rom_fc);" in sc01a_core and
            "cur_va <= interpolate8(cur_va, rom_va);" in sc01a_core and
            "localparam logic [16:0] SC01_CONTROL_UPDATE_RATE = 17'd20000;" in sc01a_core and
            "localparam logic signed [16:0] FORMANT_SLEW_STEP = 17'sd3000;" in formant_backend and
            "FORMANT_IDLE_DECAY_SAMPLES = 10'd512" in formant_backend and
            "function automatic logic signed [15:0] slew_limit16" in formant_backend and
            "function automatic logic [2:0] noise_stop_burst_gain;" in formant_backend and
            "function automatic logic [2:0] duration_speed_step;" in sc01a_core and
            "function automatic logic [4:0] rate_period_units;" in sc01a_core and
            "task automatic rate_scaled_speed_step" in sc01a_core and
            "rate = rate_inflection[7:4];" in sc01a_core and
            "speed_step = 6'd1;" in sc01a_core and
            "rate_scaled_speed_step(base_speed_step, speed_step);" in sc01a_core and
            "input  logic [2:0] articulation," in sc01a_core and
            "function automatic logic [2:0] articulation_shift;" in sc01a_core and
            "logic [11:0] target_inflection_q;" in sc01a_core and
            "logic [11:0] active_inflection_q;" in sc01a_core and
            "task automatic advance_inflection;" in sc01a_core and
            "function automatic logic [9:0] ssi263_pitch_period_limit" in sc01a_core and
            "span   = 13'd4096 - {1'b0, infl};" in sc01a_core and
            "period = scaled[14:5];" in sc01a_core and
            "base_limit = 8'hE0 ^ {infl[11:10], 5'd0};" in sc01a_core and
            "pitch_noise_gate <= is_votrax_q ?" in sc01a_core and
            ".articulation((start_votrax || is_votrax_q) ? 3'd5 : ctrl_art_amp[6:4])," in formant_backend and
            "ssi263_to_sc01_audio_phone(duration_phoneme," in formant_backend and
            "localparam int NOISE_SHAPER_INPUT_SHIFT = 6;" in formant_backend and
            "noise_shaper_input = sat24_from56(shifted);" in formant_backend and
            "SYNTH_BYPASS_APPLY" in formant_backend and
            "synth_bypass_high_q <=" in formant_backend and
            "task automatic clear_synth_pipeline;" in formant_backend and
            "task automatic clear_filter_history;" in formant_backend and
            "clear_synth_pipeline();" in formant_backend and
            "if (!votrax) begin\n                clear_filter_history();\n            end" in formant_backend and
            "clear_pipeline();" in formant_backend and
            "rom_silence <= sc01a_pause(phone);" in sc01a_core and
            "if (pause_q) begin" not in formant_backend and
            "idle_decay_count_q <= 10'd0;" in formant_backend and
            "stop_audio();" in formant_backend and
            "presence_bypass_from24" in formant_backend and
            "fricative_bypass_from24" in formant_backend and
            "localparam int F2N_INPUT_GAIN_SHIFT = 3;" in formant_backend and
            "FILTER_F2N" in formant_backend and
            "function automatic logic signed [23:0] f2n_boost_from24" in formant_backend and
            "synth_f2n_in_q" in formant_backend and
            "synth_f2n_q" in formant_backend and
            "synth_vn_q" in formant_backend and
            "SYNTH_F2N_SCALE" in formant_backend and
            "SYNTH_F2N_INPUT_GAIN" in formant_backend and
            "SYNTH_F2N_MIX" in formant_backend and
            "synth_f2n_scaled_q <= scale4_from24(synth_fn_q, filt_fc_q);" in formant_backend and
            "synth_f2n_in_q <= f2n_boost_from24(synth_f2n_scaled_q);" in formant_backend and
            "{{4{synth_f2n_q[23]}}, synth_f2n_q}" in formant_backend and
            "filter_stage_q <= FILTER_F2N;" in formant_backend and
            "filter_stage_q <= FILTER_F3;" in formant_backend and
            "synth_noise_mix_gain_q <= 5'd5 + {1'b0, (4'hF ^ synth_noise_fc_q)};" in formant_backend and
            "function automatic logic nonclosure_noise_phone;" in formant_backend and
            "nonclosure_noise_phone = (filt_fa_q != 4'd0) && !rom_closure_q;" in formant_backend and
            "function automatic logic ch_fricative_phone;" in formant_backend and
            "ch_fricative_phone = (rom_phone_q == 6'h10)" in formant_backend and
            "function automatic logic [2:0] consonant_attack_level;" in formant_backend and
            "function automatic logic signed [23:0] consonant_attack_boost_from24" in formant_backend and
            "function automatic logic voiced_stop_phone;" in formant_backend and
            "function automatic logic [2:0] voiced_stop_attack_gain;" in formant_backend and
            "SYNTH_ATTACK_BOOST" in formant_backend and
            "synth_attack_fx_q <= consonant_attack_mix_from24" in formant_backend and
            "ch_fricative_phone() ?" not in formant_backend and
            "nonclosure_noise_phone() ?" not in formant_backend and
            "if (ch_fricative_phone()) begin" in formant_backend and
            "end else if (nonclosure_noise_phone()) begin" in formant_backend and
            "synth_bypass_mode_q <= BYPASS_FRICATIVE;" in formant_backend and
            "end else if (filt_va_q != 4'd0 && filt_fa_q == 4'd0) begin" in formant_backend and
            "synth_bypass_mode_q <= BYPASS_PRESENCE;" in formant_backend and
            "synth_bypass_mode_q <= BYPASS_NONE;" in formant_backend and
            "synth_enhanced_fx_q <= sat24_from28(" in formant_backend and
            "presence_boost_from24" in formant_backend and
            "presence_low_next_from24" in formant_backend and
            "soft_limit16_from24" in formant_backend and
            "filter_stage_q <= FILTER_FX;" in formant_backend and
            "mac_coeff_q <= sc01a_fx_coeff(mac_tap_q);" in formant_backend and
            "formant_output_gain" in formant_backend and
            "high_shelf_from24" not in formant_backend and
            "VOTRAX_OUTPUT_SCALE = 4'd4" in formant_backend and
            "SSI263_OUTPUT_SCALE = 4'd10" in formant_backend and
            "SYNTH_AMP_SCALE" in formant_backend and
            "SYNTH_PRESENCE" in formant_backend and
            "SYNTH_OUTPUT_SCALE" in formant_backend and
            "SYNTH_OUTPUT_GAIN" in formant_backend and
            "SYNTH_OUTPUT_LIMIT" in formant_backend and
            "function automatic logic [2:0] current_closure_gain;" in formant_backend and
            "current_closure_gain = voiced_stop_attack_gain();" in formant_backend and
            "affricate_closure_gain" not in formant_backend and
            "current_closure_gain = rom_closure_q ? noise_stop_burst_gain() : 3'd7;" in formant_backend and
            "noise_stop_burst_gain()" in formant_backend and
            "task automatic start_synth_sample" in formant_backend and
            "synth_presence_q <= presence_boost_from24" in formant_backend and
            "synth_output_scaled_q <= scale4_from24" in formant_backend and
            "is_votrax_q ? VOTRAX_OUTPUT_SCALE : SSI263_OUTPUT_SCALE" in formant_backend and
            "synth_output_gain_q <= formant_output_gain" in formant_backend and
            "synth_output_limited_q <= soft_limit16_from24" in formant_backend and
            "audio_q <= slew_limit16(audio_q," in formant_backend and
            "start_synth_sample(4'd0," in formant_backend and
            "end else if (ticks_q == 5'h10) begin" in formant_backend,
            "SSI263 formant backend must use the MAME SC-01-A ROM decode and timing edge cases")
    require("MameLikeVotrax" in formant_compare and
            "HdlLikeFormant" in formant_compare and
            "HDL_CONTROL_RATE = 20_000" in formant_compare and
            "def advance_pitch_noise(self) -> None:" in formant_compare and
            "def duration_speed_step(self) -> int:" in formant_compare and
            "def rate_period_units(self) -> int:" in formant_compare and
            "def rate_scaled_speed_step(self, base_step: int) -> int:" in formant_compare and
            "def control_speed_step(self) -> int:" in formant_compare and
            "if self.is_votrax:" in formant_compare and
            "return self.rate_scaled_speed_step(self.duration_speed_step())" in formant_compare and
            "self.advance_inflection()" in formant_compare and
            "self.advance_pitch_noise()" in formant_compare and
            "self.rate_inflection = rate_inflection & 0xFF" in formant_compare and
            "self.articulation = articulation & 0x7" in formant_compare and
            "def ssi263_inflection12(self) -> int:" in formant_compare and
            "def transitioned_inflection_mode(self) -> bool:" in formant_compare and
            "def advance_inflection(self) -> None:" in formant_compare and
            "def pitch_period_limit(self) -> int:" in formant_compare and
            "def sc01_noise_next(state: int, cur_noise: bool) -> int:" in formant_compare and
            "self.cur_noise = bool(self.sc01_noise_out(self.noise))" in formant_compare and
            "span = 0x1000 - infl" in formant_compare and
            "period = (span * 5) >> 5" in formant_compare and
            "return max(1, period)" in formant_compare and
            "pitch_limit = self.pitch_period_limit()" in formant_compare and
            "DEFAULT_NOISE_SHAPER_INPUT_SHIFT = 6" in formant_compare and
            "FORMANT_SLEW_STEP = 3000" in formant_compare and
            "FORMANT_IDLE_DECAY_SAMPLES = 512" in formant_compare and
            "def slew_limit16(previous: int, target: int, step: int = FORMANT_SLEW_STEP) -> int:" in formant_compare and
            "def noise_stop_burst_gain(self) -> int:" in formant_compare and
            "noise_in = 0 if silent_decay else sat24(scale4(noise, self.filt_fa) << self.noise_shift)" in formant_compare and
            "def formant_output_gain(sample: int) -> int:" in formant_compare and
            "def soft_limit16(value: int) -> int:" in formant_compare and
            "def presence_bypass(lowpassed: int, pre_lowpass: int) -> int:" in formant_compare and
            "def fricative_bypass(lowpassed: int, pre_lowpass: int) -> int:" in formant_compare and
            "def nonclosure_noise_phone(self) -> bool:" in formant_compare and
            "def ch_fricative_phone(self) -> bool:" in formant_compare and
            "def affricate_closure_gain" not in formant_compare and
            "def consonant_attack_level(self) -> int:" in formant_compare and
            "def consonant_attack_boost(self, sample: int) -> int:" in formant_compare and
            "def voiced_stop_phone(self) -> bool:" in formant_compare and
            "def voiced_stop_attack_gain(self) -> int:" in formant_compare and
            "def noise_mix_gain(self) -> int:" in formant_compare and
            "F2N_INPUT_GAIN_SHIFT = 3" in formant_compare and
            "self.ff2n = FixedFilter(3, 3)" in formant_compare and
            "def f2n_input_gain(self, fn_out: int) -> int:" in formant_compare and
            "f2n = self.ff2n.apply" in formant_compare and
            "vn = sat24(f2 + f2n)" in formant_compare and
            "f3 = self.ff3.apply(vn," in formant_compare and
            "mixed = sat24(f3 + scale20(fn, self.noise_mix_gain()))" in formant_compare and
            "if self.ch_fricative_phone():" in formant_compare and
            "return 5 + (0xF ^ self.filt_fc)" in formant_compare and
            "return 16 if rate == 0 else 16 - rate" in formant_compare and
            "enhanced = presence_bypass(fx, closed)" in formant_compare and
            "enhanced = fricative_bypass(fx, closed)" in formant_compare and
            "enhanced = self.consonant_attack_boost(enhanced)" in formant_compare and
            "def presence_boost(self, sample: int) -> int:" in formant_compare and
            "return self.noise_stop_burst_gain() if self.params.closure else 7" in formant_compare and
            "presence = self.presence_boost(scaled)" in formant_compare and
            "SSI263_OUTPUT_SCALE = 10" in formant_compare and
            "def current_closure_gain(self) -> int:" in formant_compare and
            "silent_decay = self.ticks == 0x10" in formant_compare and
            "self.clear_filter_history()" in formant_compare and
            "gap_samples = max(0, int(round(HDL_SAMPLE_RATE * inter_phone_gap_ms / 1000.0)))" in formant_compare and
            "self.audio = slew_limit16(self.audio, target)" in formant_compare and
            "mame_like_reference.wav" in formant_compare and
            "hdl_like_current.wav" in formant_compare and
            "metrics.csv" in formant_compare and
            "sc01a_word_by_phone" in formant_compare,
            "SSI263 formant work must keep a repeatable MAME-vs-HDL corpus comparison tool")
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
    require("task automatic repeat_completed_ssi263;" in bus_wrapper and
            "start_backend(duration_phoneme_q[5:0], 1'b0);" in bus_wrapper and
            "SSI_INFLECT: begin" in bus_wrapper and
            "SSI_RATEINF: begin" in bus_wrapper and
            bus_wrapper.count("repeat_completed_ssi263();") == 2,
            "SSI263 reg1/reg2 completion clears must restart the active phoneme for repeated D7/IRQ completion")
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


def test_ssi263_rate_and_inflection_affect_hdl_model() -> None:
    formant_compare = load_formant_compare_module()
    data = formant_compare.parse_generated_data(formant_compare.FORMANT_PKG)

    def samples_until_done(rate_inflection: int) -> int:
        model = formant_compare.HdlLikeFormant(data.phones,
                                               inflection=0x20,
                                               rate_inflection=rate_inflection,
                                               amplitude=15)
        model.start_phone(0x00, 0xC0)
        samples = 0
        while model.ticks != 0x10 and samples < 200000:
            model.chip_accum += formant_compare.HDL_CONTROL_RATE
            if model.chip_accum >= formant_compare.HDL_SAMPLE_RATE:
                model.chip_accum -= formant_compare.HDL_SAMPLE_RATE
                model.advance_inflection()
                speed_step = model.control_speed_step()
                if speed_step:
                    model.chip_update(speed_step)
                model.advance_pitch_noise()
            samples += 1
        return samples

    slow = samples_until_done(0x08)
    default = samples_until_done(0xA8)
    fast = samples_until_done(0xB8)
    require(slow > default > fast,
            "SSI263 RATE high nibble must change HDL-like phoneme duration")

    def sc01_samples_until_done(rate_inflection: int) -> int:
        model = formant_compare.HdlLikeFormant(data.phones,
                                               inflection=0x20,
                                               rate_inflection=rate_inflection,
                                               amplitude=15,
                                               votrax=True)
        model.start_phone(0x00)
        samples = 0
        while model.ticks != 0x10 and samples < 200000:
            model.chip_accum += formant_compare.HDL_CONTROL_RATE
            if model.chip_accum >= formant_compare.HDL_SAMPLE_RATE:
                model.chip_accum -= formant_compare.HDL_SAMPLE_RATE
                model.advance_inflection()
                speed_step = model.control_speed_step()
                if speed_step:
                    model.chip_update(speed_step)
                model.advance_pitch_noise()
            samples += 1
        return samples

    sc01_slow_rate = sc01_samples_until_done(0x08)
    sc01_fast_rate = sc01_samples_until_done(0xB8)
    require(sc01_slow_rate == sc01_fast_rate and sc01_slow_rate < 200000,
            "SC-01/Votrax native playback must ignore SSI263 RATE and keep the default speed")

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


def test_via_ifr_read_sees_pending_timer_underflow() -> None:
    via = read(VIA6522_V)

    require("wire [6:0]  ifr_read = {irq_t1 || (addr == ADDR_IFR && timer1_undf && (irq_t1_one_shot || acr[6]))," in via and
            "irq_t2 || (addr == ADDR_IFR && timer2_undf && irq_t2_one_shot)," in via and
            "ADDR_IFR:                   data_out = {irq_p, ifr_read};" in via,
            "VIA IFR reads must expose pending T1/T2 underflow without changing stored IRQ state")


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
            "SSI-263/SC-01 speech chips." in phasor_help and
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


TESTS = [
    test_phasor_mode_switch_and_reset_contract,
    test_four_ay_chips_and_phasor_chip_selects,
    test_ssi263_applewin_behavior_contract,
    test_ssi263_rate_and_inflection_affect_hdl_model,
    test_via_ifr_read_sees_pending_timer_underflow,
    test_phasor_timer_low_reads_can_add_one_tick,
    test_via_apple_reset_preserves_timer_latches,
    test_phasor_irq_is_suppressed_during_apple_reset,
    test_phasor_pan_registers_and_menu_schema,
    test_phasor_is_apple_bus_driven,
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
