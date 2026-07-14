`timescale 1ns / 1ps

import ssi263_formant_pkg::*;

// SSI263/SC-01A formant synthesis backend.
//
// This path owns formant audio and formant-duration completion. The
// Apple-visible SSI263 register, D7, IRQ, and reset behavior still live in
// ssi263_bus_wrapper.
module ssi263_formant_backend (
    input  logic        clk,
    input  logic        rstn,
    input  logic        card_enabled,
    input  logic        warm_reset,
    input  logic        audio_tick,

    input  logic        start,
    input  logic [5:0]  start_phoneme,
    input  logic [5:0]  start_sc01_phone,
    input  logic        start_votrax,

    input  logic [1:0]  current_function,
    input  logic [7:0]  duration_phoneme,
    input  logic [7:0]  inflection,
    input  logic [7:0]  rate_inflection,
    input  logic [7:0]  ctrl_art_amp,
    input  logic [7:0]  filter_freq,

    output logic        phoneme_done,
    output logic signed [15:0] audio
);

    localparam logic [7:0] CONTROL_MASK        = 8'h80;
    localparam logic [7:0] AMPLITUDE_MASK      = 8'h0F;
    localparam logic [7:0] FILTER_FREQ_SILENCE = 8'hFF;
    // Fixed formant backend tuning, derived from hardware listening tests.
    localparam logic [3:0] VOTRAX_OUTPUT_SCALE = 4'd4;
    localparam logic [3:0] SSI263_OUTPUT_SCALE = 4'd10;
    localparam int NOISE_SHAPER_INPUT_SHIFT = 6;
    // Gain (arithmetic left shift, saturated) applied to the FN-shaped noise
    // before it enters the noise half of the F2 filter -- the main fricative
    // path. The FN resonator delivers little energy for short fricatives, so
    // without a boost they render ~20-30x below the vowels and are inaudible.
    // 0 = unity; each +1 doubles the F2N contribution. Tune by ear: too low
    // = no fricatives, too high = clipped/hissy.
    localparam int F2N_INPUT_GAIN_SHIFT = 3;
    localparam logic signed [16:0] FORMANT_SLEW_STEP = 17'sd3000;
    localparam logic [9:0] FORMANT_IDLE_DECAY_SAMPLES = 10'd512;

    typedef enum logic [4:0] {
        SYNTH_IDLE,
        SYNTH_EXCITE,
        SYNTH_FILTER_PREP,
        SYNTH_FILTER_MULT,
        SYNTH_FILTER_ACCUM,
        SYNTH_F2N_SCALE,
        SYNTH_F2N_INPUT_GAIN,
        SYNTH_F2N_MIX,
        SYNTH_NOISE_GAIN,
        SYNTH_NOISE_MIX,
        SYNTH_NOISE_FINE,
        SYNTH_NOISE_SCALE,
        SYNTH_CLOSURE,
        SYNTH_SCALE,
        SYNTH_BYPASS_APPLY,
        SYNTH_ATTACK_BOOST,
        SYNTH_AMP_SCALE,
        SYNTH_PRESENCE,
        SYNTH_OUTPUT_SCALE,
        SYNTH_OUTPUT_GAIN,
        SYNTH_OUTPUT_LIMIT,
        SYNTH_OUT
    } synth_state_t;

    typedef enum logic [2:0] {
        FILTER_F1,
        FILTER_F2,
        FILTER_FN,
        FILTER_F2N,
        FILTER_F3,
        FILTER_F4,
        FILTER_FX
    } filter_stage_t;

    typedef enum logic [1:0] {
        BYPASS_NONE,
        BYPASS_PRESENCE,
        BYPASS_FRICATIVE
    } bypass_mode_t;

    logic       active_valid_q;
    logic       is_votrax_q;
    logic [4:0]  ticks_q;
    logic [9:0]  pitch_q;
    logic        pitch_gate_q;
    logic [4:0]  closure_q;
    logic [9:0]  idle_decay_count_q;
    logic        cur_noise_q;

    logic [3:0] rom_fa_q;
    logic [3:0] rom_fc_q;
    logic [3:0] rom_va_q;
    logic [3:0] rom_f1_q;
    logic [3:0] rom_f2_q;
    logic [3:0] rom_f2q_q;
    logic [3:0] rom_f3_q;
    logic [3:0] rom_cld_q;
    logic [3:0] rom_vd_q;
    logic [6:0] rom_duration_q;
    logic       rom_closure_q;
    logic       rom_pause_q;
    logic [5:0] rom_phone_q;

    logic [7:0] cur_fa_q;
    logic [7:0] cur_fc_q;
    logic [7:0] cur_va_q;
    logic [7:0] cur_f1_q;
    logic [7:0] cur_f2_q;
    logic [7:0] cur_f2q_q;
    logic [7:0] cur_f3_q;

    logic [3:0] filt_fa_q;
    logic [3:0] filt_fc_q;
    logic [3:0] filt_va_q;
    logic [3:0] filt_f1_q;
    logic [4:0] filt_f2_q;
    logic [3:0] filt_f2q_q;
    logic [3:0] filt_f3_q;

    logic signed [15:0] audio_q;
    logic unused_phoneme_controls;

    synth_state_t synth_state_q;
    filter_stage_t filter_stage_q;
    logic [2:0]   mac_tap_q;
    logic signed [55:0] mac_accum_q;
    logic signed [23:0] mac_sample_q;
    logic signed [15:0] mac_coeff_q;
    logic signed [55:0] mac_product_q;
    logic [3:0]   synth_voice_gain_q;
    logic [3:0]   synth_noise_gain_q;
    logic [3:0]   synth_noise_fc_q;
    logic [4:0]   synth_noise_mix_gain_q;
    logic [3:0]   synth_amp_q;
    logic [2:0]   synth_closure_gain_q;
    logic signed [15:0] synth_voice_source_q;
    logic signed [15:0] synth_noise_source_q;
    logic signed [23:0] synth_voice_input_q;
    logic signed [23:0] synth_noise_input_q;
    logic signed [23:0] synth_f1_q;
    logic signed [23:0] synth_f2_q;
    logic signed [23:0] synth_f2n_scaled_q; // FN output scaled by FC
    logic signed [23:0] synth_f2n_in_q;  // FN output scaled by FC, F2N input
    logic signed [23:0] synth_f2n_q;     // F2N output (noise half of F2)
    logic signed [23:0] synth_vn_q;      // v + n2 mix that feeds F3
    logic signed [23:0] synth_f3_q;
    logic signed [23:0] synth_fn_q;
    logic signed [23:0] synth_f4_q;
    logic signed [23:0] synth_fx_q;
    logic signed [23:0] synth_mixed_q;
    logic signed [23:0] synth_noise_scale_sample_q;
    logic signed [23:0] synth_noise_base_q;
    logic signed [23:0] synth_shaped_noise_q;
    logic signed [23:0] synth_closed_q;
    logic signed [27:0] synth_bypass_low_q;
    logic signed [27:0] synth_bypass_high_q;
    bypass_mode_t synth_bypass_mode_q;
    logic signed [23:0] synth_enhanced_fx_q;
    logic signed [23:0] synth_attack_fx_q;
    logic signed [23:0] synth_scaled_q;
    logic signed [23:0] synth_presence_q;
    logic signed [23:0] synth_output_scaled_q;
    logic signed [23:0] synth_output_gain_q;
    logic signed [15:0] synth_output_limited_q;

    logic signed [23:0] f1_x1_q;
    logic signed [23:0] f1_x2_q;
    logic signed [23:0] f1_x3_q;
    logic signed [23:0] f1_y1_q;
    logic signed [23:0] f1_y2_q;
    logic signed [23:0] f1_y3_q;

    logic signed [23:0] f2_x1_q;
    logic signed [23:0] f2_x2_q;
    logic signed [23:0] f2_x3_q;
    logic signed [23:0] f2_y1_q;
    logic signed [23:0] f2_y2_q;
    logic signed [23:0] f2_y3_q;

    // Noise half of the F2 formant filter (MAME's m_f2n). Same coefficient
    // table as the voice F2, independent delay-line state. This is the main
    // fricative path: FN-shaped noise scaled by FC, F2-filtered, mixed into
    // the voice ahead of F3.
    logic signed [23:0] f2n_x1_q;
    logic signed [23:0] f2n_x2_q;
    logic signed [23:0] f2n_x3_q;
    logic signed [23:0] f2n_y1_q;
    logic signed [23:0] f2n_y2_q;
    logic signed [23:0] f2n_y3_q;

    logic signed [23:0] f3_x1_q;
    logic signed [23:0] f3_x2_q;
    logic signed [23:0] f3_x3_q;
    logic signed [23:0] f3_y1_q;
    logic signed [23:0] f3_y2_q;
    logic signed [23:0] f3_y3_q;

    logic signed [23:0] f4_x1_q;
    logic signed [23:0] f4_x2_q;
    logic signed [23:0] f4_x3_q;
    logic signed [23:0] f4_y1_q;
    logic signed [23:0] f4_y2_q;
    logic signed [23:0] f4_y3_q;

    logic signed [23:0] fn_x1_q;
    logic signed [23:0] fn_x2_q;
    logic signed [23:0] fn_y1_q;
    logic signed [23:0] fn_y2_q;

    logic signed [23:0] fx_y1_q;
    logic signed [23:0] presence_low_q;

    logic [5:0] core_start_phone;
    logic [4:0] core_ticks;
    logic [9:0] core_pitch;
    logic       core_pitch_noise_gate;
    logic [4:0] core_closure_age;
    logic       core_noise_bit;
    logic [3:0] core_rom_fa;
    logic [3:0] core_rom_fc;
    logic [3:0] core_rom_va;
    logic [3:0] core_rom_f1;
    logic [3:0] core_rom_f2;
    logic [3:0] core_rom_f2q;
    logic [3:0] core_rom_f3;
    logic [3:0] core_rom_cld;
    logic [3:0] core_rom_vd;
    logic [6:0] core_rom_duration;
    logic       core_rom_closure;
    logic       core_rom_silence;
    logic [5:0] core_rom_phone;
    logic [7:0] core_cur_fa;
    logic [7:0] core_cur_fc;
    logic [7:0] core_cur_va;
    logic [7:0] core_cur_f1;
    logic [7:0] core_cur_f2;
    logic [7:0] core_cur_f2q;
    logic [7:0] core_cur_f3;
    logic [3:0] core_filt_fa;
    logic [3:0] core_filt_fc;
    logic [3:0] core_filt_va;
    logic [3:0] core_filt_f1;
    logic [4:0] core_filt_f2;
    logic [3:0] core_filt_f2q;
    logic [3:0] core_filt_f3;
    logic       core_phoneme_done;
    logic       phoneme_done_q;

    assign audio = audio_q;
    assign phoneme_done = phoneme_done_q;
    assign unused_phoneme_controls = ^start_phoneme;
    assign core_start_phone = start_votrax ?
                              start_sc01_phone :
                              ssi263_to_sc01_audio_phone(duration_phoneme,
                                                         current_function);

    sc01a_digital_core digital_core_i (
        .clk(clk),
        .rstn(rstn),
        .reset(!card_enabled || warm_reset),
        .audio_tick(audio_tick),
        .start(start),
        .start_phone(core_start_phone),
        .start_votrax(start_votrax),
        .current_function(current_function),
        .duration_phoneme(duration_phoneme),
        .inflection(inflection),
        .rate_inflection(rate_inflection),
        .articulation((start_votrax || is_votrax_q) ? 3'd5 : ctrl_art_amp[6:4]),
        .phoneme_done(core_phoneme_done),
        .ticks(core_ticks),
        .pitch(core_pitch),
        .pitch_noise_gate(core_pitch_noise_gate),
        .closure_age(core_closure_age),
        .noise_bit(core_noise_bit),
        .rom_fa(core_rom_fa),
        .rom_fc(core_rom_fc),
        .rom_va(core_rom_va),
        .rom_f1(core_rom_f1),
        .rom_f2(core_rom_f2),
        .rom_f2q(core_rom_f2q),
        .rom_f3(core_rom_f3),
        .rom_cld(core_rom_cld),
        .rom_vd(core_rom_vd),
        .rom_duration(core_rom_duration),
        .rom_closure(core_rom_closure),
        .rom_silence(core_rom_silence),
        .rom_phone(core_rom_phone),
        .cur_fa(core_cur_fa),
        .cur_fc(core_cur_fc),
        .cur_va(core_cur_va),
        .cur_f1(core_cur_f1),
        .cur_f2(core_cur_f2),
        .cur_f2q(core_cur_f2q),
        .cur_f3(core_cur_f3),
        .filt_fa(core_filt_fa),
        .filt_fc(core_filt_fc),
        .filt_va(core_filt_va),
        .filt_f1(core_filt_f1),
        .filt_f2(core_filt_f2),
        .filt_f2q(core_filt_f2q),
        .filt_f3(core_filt_f3)
    );

    function automatic logic signed [15:0] glottal_wave(input logic [3:0] index);
        begin
            case (index)
                4'd0:    glottal_wave = 16'sd0;
                4'd1:    glottal_wave = -16'sd4681;
                4'd2:    glottal_wave = 16'sd8192;
                4'd3:    glottal_wave = 16'sd7022;
                4'd4:    glottal_wave = 16'sd5851;
                4'd5:    glottal_wave = 16'sd4681;
                4'd6:    glottal_wave = 16'sd3511;
                4'd7:    glottal_wave = 16'sd2340;
                4'd8:    glottal_wave = 16'sd1170;
                default: glottal_wave = 16'sd0;
            endcase
        end
    endfunction

    function automatic logic signed [15:0] voice_source_from_pitch(input logic [9:0] pitch);
        begin
            // Open glottal phase = first 72 counts of the period (9 table entries
            // x 8 counts); the rest of the period is the closed phase (silence).
            if (pitch >= 10'd72) begin
                voice_source_from_pitch = 16'sd0;
            end else begin
                voice_source_from_pitch = glottal_wave(pitch[6:3]);
            end
        end
    endfunction

    // Noise is gated by the digital core (pitch_noise_gate): Votrax keeps the
    // SC-01/MAME pitch[6] AM, SSI263 a period-stable ~50%-duty gate.
    function automatic logic signed [15:0] noise_source(input logic gate,
                                                        input logic noise_bit);
        begin
            noise_source = (gate && noise_bit) ? 16'sd8192 : -16'sd8192;
        end
    endfunction

    function automatic logic signed [23:0] scale4(input logic signed [15:0] sample,
                                                  input logic [3:0] gain);
        logic signed [23:0] widened;
        begin
            widened = {{8{sample[15]}}, sample};
            case (gain)
                4'd0:    scale4 = 24'sd0;
                4'd1:    scale4 = widened >>> 4;
                4'd2:    scale4 = widened >>> 3;
                4'd3:    scale4 = (widened >>> 3) + (widened >>> 4);
                4'd4:    scale4 = widened >>> 2;
                4'd5:    scale4 = (widened >>> 2) + (widened >>> 4);
                4'd6:    scale4 = (widened >>> 2) + (widened >>> 3);
                4'd7:    scale4 = (widened >>> 2) + (widened >>> 3) + (widened >>> 4);
                4'd8:    scale4 = widened >>> 1;
                4'd9:    scale4 = (widened >>> 1) + (widened >>> 4);
                4'd10:   scale4 = (widened >>> 1) + (widened >>> 3);
                4'd11:   scale4 = (widened >>> 1) + (widened >>> 3) + (widened >>> 4);
                4'd12:   scale4 = (widened >>> 1) + (widened >>> 2);
                4'd13:   scale4 = (widened >>> 1) + (widened >>> 2) + (widened >>> 4);
                4'd14:   scale4 = (widened >>> 1) + (widened >>> 2) + (widened >>> 3);
                default: scale4 = widened;
            endcase
        end
    endfunction

    function automatic logic signed [15:0] sat16_from24(input logic signed [23:0] sample);
        begin
            if (sample > 24'sd32767) begin
                sat16_from24 = 16'sh7FFF;
            end else if (sample < -24'sd32768) begin
                sat16_from24 = -16'sd32768;
            end else begin
                sat16_from24 = sample[15:0];
            end
        end
    endfunction

    function automatic logic signed [15:0] soft_limit16_from24(input logic signed [23:0] sample);
        logic signed [23:0] limited;
        begin
            if (sample > 24'sd8192) begin
                limited = 24'sd8192 + ((sample - 24'sd8192) >>> 2);
            end else if (sample < -24'sd8192) begin
                limited = -24'sd8192 + ((sample + 24'sd8192) >>> 2);
            end else begin
                limited = sample;
            end
            soft_limit16_from24 = sat16_from24(limited);
        end
    endfunction

    function automatic logic signed [15:0] slew_limit16(input logic signed [15:0] previous,
                                                        input logic signed [15:0] target);
        logic signed [16:0] previous_ext;
        logic signed [16:0] target_ext;
        logic signed [16:0] diff;
        logic signed [16:0] next_ext;
        begin
            previous_ext = {previous[15], previous};
            target_ext = {target[15], target};
            diff = target_ext - previous_ext;

            if (diff > FORMANT_SLEW_STEP) begin
                next_ext = previous_ext + FORMANT_SLEW_STEP;
            end else if (diff < -FORMANT_SLEW_STEP) begin
                next_ext = previous_ext - FORMANT_SLEW_STEP;
            end else begin
                next_ext = target_ext;
            end

            slew_limit16 = next_ext[15:0];
        end
    endfunction

    function automatic logic [2:0] noise_stop_burst_gain;
        logic [4:0] vd_ext;
        logic [4:0] burst_age;
        begin
            vd_ext = {1'b0, rom_vd_q};
            if (ticks_q <= vd_ext) begin
                noise_stop_burst_gain = 3'd0;
            end else begin
                burst_age = ticks_q - vd_ext;
                case (burst_age)
                    5'd1:   noise_stop_burst_gain = 3'd7;
                    5'd2:   noise_stop_burst_gain = 3'd7;
                    5'd3:   noise_stop_burst_gain = 3'd5;
                    5'd4:   noise_stop_burst_gain = 3'd3;
                    5'd5:   noise_stop_burst_gain = 3'd2;
                    default: noise_stop_burst_gain = 3'd0;
                endcase
            end
        end
    endfunction

    function automatic logic voiced_stop_phone;
        begin
            voiced_stop_phone = rom_closure_q &&
                                rom_fa_q == 4'd0 &&
                                rom_va_q != 4'd0;
        end
    endfunction

    function automatic logic [2:0] voiced_stop_attack_gain;
        logic [4:0] cld_ext;
        logic [4:0] attack_age;
        begin
            cld_ext = {1'b0, rom_cld_q};
            if (ticks_q < cld_ext) begin
                voiced_stop_attack_gain = 3'd0;
            end else begin
                attack_age = ticks_q - cld_ext;
                case (attack_age)
                    5'd0:   voiced_stop_attack_gain = 3'd7;
                    5'd1:   voiced_stop_attack_gain = 3'd7;
                    5'd2:   voiced_stop_attack_gain = 3'd6;
                    5'd3:   voiced_stop_attack_gain = 3'd4;
                    5'd4:   voiced_stop_attack_gain = 3'd2;
                    5'd5:   voiced_stop_attack_gain = 3'd1;
                    default: voiced_stop_attack_gain = 3'd0;
                endcase
            end
        end
    endfunction

    function automatic logic ch_fricative_phone;
        begin
            ch_fricative_phone = (rom_phone_q == 6'h10) &&
                                 (filt_fa_q != 4'd0) &&
                                 (filt_va_q == 4'd0) &&
                                 !rom_closure_q;
        end
    endfunction

    function automatic logic [2:0] current_closure_gain;
        begin
            if (voiced_stop_phone()) begin
                current_closure_gain = voiced_stop_attack_gain();
            end else if (filt_fa_q != 4'd0 && filt_va_q == 4'd0) begin
                current_closure_gain = rom_closure_q ? noise_stop_burst_gain() : 3'd7;
            end else begin
                current_closure_gain = 3'h7 ^ closure_q[4:2];
            end
        end
    endfunction

    function automatic logic nonclosure_noise_phone;
        begin
            nonclosure_noise_phone = (filt_fa_q != 4'd0) && !rom_closure_q;
        end
    endfunction

    function automatic logic [2:0] consonant_attack_level;
        logic [4:0] start_tick;
        logic [4:0] attack_age;
        begin
            consonant_attack_level = 3'd0;
            if (ch_fricative_phone()) begin
                consonant_attack_level = 3'd0;
            end else if (rom_fa_q != 4'd0 || rom_va_q != 4'd0) begin
                if (rom_closure_q && rom_cld_q <= rom_vd_q) begin
                    start_tick = {1'b0, rom_cld_q};
                end else if (rom_fa_q != 4'd0) begin
                    start_tick = {1'b0, rom_vd_q};
                end else begin
                    start_tick = {1'b0, rom_cld_q};
                end

                if (ticks_q >= start_tick) begin
                    attack_age = ticks_q - start_tick;
                    case (attack_age)
                        5'd0:   consonant_attack_level = 3'd3;
                        5'd1:   consonant_attack_level = 3'd2;
                        5'd2:   consonant_attack_level = 3'd1;
                        default: consonant_attack_level = 3'd0;
                    endcase
                end
            end
        end
    endfunction

    function automatic logic signed [23:0] consonant_attack_boost_from24(
        input logic signed [23:0] sample,
        input logic [2:0] level
    );
        logic signed [27:0] sample_wide;
        begin
            sample_wide = {{4{sample[23]}}, sample};
            case (level)
                3'd3: begin
                    consonant_attack_boost_from24 =
                        sat24_from28(sample_wide + (sample_wide >>> 1));
                end
                3'd2: begin
                    consonant_attack_boost_from24 =
                        sat24_from28(sample_wide + (sample_wide >>> 2));
                end
                3'd1: begin
                    consonant_attack_boost_from24 =
                        sat24_from28(sample_wide + (sample_wide >>> 3));
                end
                default: begin
                    consonant_attack_boost_from24 = sample;
                end
            endcase
        end
    endfunction

    function automatic logic signed [23:0] consonant_attack_mix_from24(
        input logic signed [23:0] sample
    );
        begin
            if (filt_fa_q != 4'd0 || rom_closure_q) begin
                consonant_attack_mix_from24 =
                    consonant_attack_boost_from24(sample,
                                                  consonant_attack_level());
            end else begin
                consonant_attack_mix_from24 = sample;
            end
        end
    endfunction

    function automatic logic signed [23:0] sat24_from56(input logic signed [55:0] sample);
        begin
            if (sample > 56'sd8388607) begin
                sat24_from56 = 24'sh7FFFFF;
            end else if (sample < -56'sd8388608) begin
                sat24_from56 = -24'sd8388608;
            end else begin
                sat24_from56 = sample[23:0];
            end
        end
    endfunction

    function automatic logic signed [23:0] sat24_from28(input logic signed [27:0] sample);
        begin
            if (sample > 28'sd8388607) begin
                sat24_from28 = 24'sh7FFFFF;
            end else if (sample < -28'sd8388608) begin
                sat24_from28 = -24'sd8388608;
            end else begin
                sat24_from28 = sample[23:0];
            end
        end
    endfunction

    function automatic logic signed [23:0] noise_shaper_input(
        input logic signed [15:0] sample,
        input logic [3:0] gain
    );
        logic signed [23:0] scaled;
        logic signed [55:0] shifted;
        begin
            scaled = scale4(sample, gain);
            shifted = {{32{scaled[23]}}, scaled} <<< NOISE_SHAPER_INPUT_SHIFT;
            noise_shaper_input = sat24_from56(shifted);
        end
    endfunction

    function automatic logic signed [23:0] scale4_from24(input logic signed [23:0] sample,
                                                         input logic [3:0] gain);
        begin
            case (gain)
                4'd0:    scale4_from24 = 24'sd0;
                4'd1:    scale4_from24 = sample >>> 4;
                4'd2:    scale4_from24 = sample >>> 3;
                4'd3:    scale4_from24 = (sample >>> 3) + (sample >>> 4);
                4'd4:    scale4_from24 = sample >>> 2;
                4'd5:    scale4_from24 = (sample >>> 2) + (sample >>> 4);
                4'd6:    scale4_from24 = (sample >>> 2) + (sample >>> 3);
                4'd7:    scale4_from24 = (sample >>> 2) + (sample >>> 3) + (sample >>> 4);
                4'd8:    scale4_from24 = sample >>> 1;
                4'd9:    scale4_from24 = (sample >>> 1) + (sample >>> 4);
                4'd10:   scale4_from24 = (sample >>> 1) + (sample >>> 3);
                4'd11:   scale4_from24 = (sample >>> 1) + (sample >>> 3) + (sample >>> 4);
                4'd12:   scale4_from24 = (sample >>> 1) + (sample >>> 2);
                4'd13:   scale4_from24 = (sample >>> 1) + (sample >>> 2) + (sample >>> 4);
                4'd14:   scale4_from24 = (sample >>> 1) + (sample >>> 2) + (sample >>> 3);
                default: scale4_from24 = sample;
            endcase
        end
    endfunction

    // Main fricative path input: FN-shaped noise scaled by FC (MAME step 7),
    // then boosted by F2N_INPUT_GAIN_SHIFT and saturated. Feeds the noise half
    // of the F2 filter.
    function automatic logic signed [23:0] f2n_boost_from24(
        input logic signed [23:0] scaled
    );
        logic signed [55:0] wide;
        begin
            wide = {{32{scaled[23]}}, scaled} <<< F2N_INPUT_GAIN_SHIFT;
            f2n_boost_from24 = sat24_from56(wide);
        end
    endfunction

    function automatic logic signed [23:0] scale7_from24(input logic signed [23:0] sample,
                                                         input logic [2:0] gain);
        begin
            case (gain)
                3'd0:    scale7_from24 = 24'sd0;
                3'd1:    scale7_from24 = (sample >>> 3) + (sample >>> 6) + (sample >>> 9);
                3'd2:    scale7_from24 = (sample >>> 2) + (sample >>> 5) + (sample >>> 8);
                3'd3:    scale7_from24 = (sample >>> 2) + (sample >>> 3) +
                                          (sample >>> 5) + (sample >>> 6);
                3'd4:    scale7_from24 = (sample >>> 1) + (sample >>> 4);
                3'd5:    scale7_from24 = (sample >>> 1) + (sample >>> 3) +
                                          (sample >>> 4) + (sample >>> 6);
                3'd6:    scale7_from24 = sample - (sample >>> 3) - (sample >>> 6);
                default: scale7_from24 = sample;
            endcase
        end
    endfunction

    function automatic logic signed [23:0] scale20_from24(input logic signed [23:0] sample,
                                                          input logic [4:0] gain);
        begin
            case (gain)
                5'd0:    scale20_from24 = 24'sd0;
                5'd1:    scale20_from24 = sample >>> 4;
                5'd2:    scale20_from24 = sample >>> 3;
                5'd3:    scale20_from24 = (sample >>> 3) + (sample >>> 5);
                5'd4:    scale20_from24 = sample >>> 2;
                5'd5:    scale20_from24 = sample >>> 2;
                5'd6:    scale20_from24 = (sample >>> 2) + (sample >>> 4) -
                                           (sample >>> 6);
                5'd7:    scale20_from24 = (sample >>> 2) + (sample >>> 3) -
                                           (sample >>> 5);
                5'd8:    scale20_from24 = (sample >>> 1) - (sample >>> 3) +
                                           (sample >>> 5);
                5'd9:    scale20_from24 = (sample >>> 1) - (sample >>> 4);
                5'd10:   scale20_from24 = sample >>> 1;
                5'd11:   scale20_from24 = (sample >>> 1) + (sample >>> 4);
                5'd12:   scale20_from24 = (sample >>> 1) + (sample >>> 3) -
                                           (sample >>> 5);
                5'd13:   scale20_from24 = (sample >>> 1) + (sample >>> 3) +
                                           (sample >>> 5);
                5'd14:   scale20_from24 = (sample >>> 1) + (sample >>> 2) -
                                           (sample >>> 4);
                5'd15:   scale20_from24 = (sample >>> 1) + (sample >>> 2);
                5'd16:   scale20_from24 = sample - (sample >>> 3) - (sample >>> 4);
                5'd17:   scale20_from24 = sample - (sample >>> 3) - (sample >>> 5);
                5'd18:   scale20_from24 = sample - (sample >>> 3) + (sample >>> 5);
                5'd19:   scale20_from24 = sample - (sample >>> 4);
                default: scale20_from24 = sample;
            endcase
        end
    endfunction

    function automatic logic signed [23:0] scale20_base_from24(input logic signed [23:0] sample,
                                                               input logic [4:0] gain);
        begin
            case (gain)
                5'd0:    scale20_base_from24 = 24'sd0;
                5'd1:    scale20_base_from24 = sample >>> 4;
                5'd2:    scale20_base_from24 = sample >>> 3;
                5'd3:    scale20_base_from24 = sample >>> 3;
                5'd4:    scale20_base_from24 = sample >>> 2;
                5'd5,
                5'd6,
                5'd7:    scale20_base_from24 = sample >>> 2;
                5'd8,
                5'd9,
                5'd10,
                5'd11,
                5'd12,
                5'd13:   scale20_base_from24 = sample >>> 1;
                5'd14,
                5'd15:   scale20_base_from24 = (sample >>> 1) + (sample >>> 2);
                5'd16,
                5'd17,
                5'd18:   scale20_base_from24 = sample - (sample >>> 3);
                5'd19:   scale20_base_from24 = sample;
                default: scale20_base_from24 = sample;
            endcase
        end
    endfunction

    function automatic logic signed [23:0] scale20_fine_from24(input logic signed [23:0] sample,
                                                               input logic [4:0] gain);
        begin
            case (gain)
                5'd3:    scale20_fine_from24 = sample >>> 5;
                5'd6:    scale20_fine_from24 = (sample >>> 4) - (sample >>> 6);
                5'd7:    scale20_fine_from24 = (sample >>> 3) - (sample >>> 5);
                5'd8:    scale20_fine_from24 = -(sample >>> 3) + (sample >>> 5);
                5'd9:    scale20_fine_from24 = -(sample >>> 4);
                5'd11:   scale20_fine_from24 = sample >>> 4;
                5'd12:   scale20_fine_from24 = (sample >>> 3) - (sample >>> 5);
                5'd13:   scale20_fine_from24 = (sample >>> 3) + (sample >>> 5);
                5'd14:   scale20_fine_from24 = -(sample >>> 4);
                5'd16:   scale20_fine_from24 = -(sample >>> 4);
                5'd17:   scale20_fine_from24 = -(sample >>> 5);
                5'd18:   scale20_fine_from24 = sample >>> 5;
                5'd19:   scale20_fine_from24 = -(sample >>> 4);
                default: scale20_fine_from24 = 24'sd0;
            endcase
        end
    endfunction

    function automatic logic signed [23:0] formant_output_gain(input logic signed [23:0] sample);
        logic signed [55:0] widened;
        begin
            widened = {{32{sample[23]}}, sample};
            formant_output_gain = sat24_from56(widened <<< 1);
        end
    endfunction

    function automatic logic signed [23:0] presence_low_next_from24(
        input logic signed [23:0] sample,
        input logic signed [23:0] low
    );
        logic signed [27:0] sample_wide;
        logic signed [27:0] low_wide;
        logic signed [27:0] delta;
        begin
            sample_wide = {{4{sample[23]}}, sample};
            low_wide = {{4{low[23]}}, low};
            delta = sample_wide - low_wide;
            presence_low_next_from24 = sat24_from28(low_wide + (delta >>> 3));
        end
    endfunction

    function automatic logic signed [23:0] presence_boost_from24(
        input logic signed [23:0] sample,
        input logic signed [23:0] low
    );
        logic signed [27:0] sample_wide;
        logic signed [27:0] low_wide;
        logic signed [27:0] delta;
        begin
            sample_wide = {{4{sample[23]}}, sample};
            low_wide = {{4{low[23]}}, low};
            delta = sample_wide - low_wide;
            presence_boost_from24 = sat24_from28(sample_wide + (delta >>> 1));
        end
    endfunction

    function automatic logic signed [23:0] presence_bypass_from24(
        input logic signed [23:0] lowpassed,
        input logic signed [23:0] pre_lowpass
    );
        logic signed [27:0] low_wide;
        logic signed [27:0] pre_wide;
        logic signed [27:0] high_wide;
        begin
            low_wide = {{4{lowpassed[23]}}, lowpassed};
            pre_wide = {{4{pre_lowpass[23]}}, pre_lowpass};
            high_wide = pre_wide - low_wide;
            presence_bypass_from24 = sat24_from28(low_wide +
                                                  (high_wide >>> 1) +
                                                  (high_wide >>> 2));
        end
    endfunction

    function automatic logic signed [23:0] fricative_bypass_from24(
        input logic signed [23:0] lowpassed,
        input logic signed [23:0] pre_lowpass
    );
        logic signed [27:0] low_wide;
        logic signed [27:0] pre_wide;
        logic signed [27:0] high_wide;
        begin
            low_wide = {{4{lowpassed[23]}}, lowpassed};
            pre_wide = {{4{pre_lowpass[23]}}, pre_lowpass};
            high_wide = pre_wide - low_wide;
            fricative_bypass_from24 = sat24_from28(pre_wide + (high_wide >>> 2));
        end
    endfunction

    function automatic logic signed [55:0] mac_product(input logic signed [23:0] sample,
                                                       input logic signed [15:0] coeff);
        logic signed [39:0] product;
        begin
            product = sample * coeff;
            mac_product = {{16{product[39]}}, product};
        end
    endfunction

    function automatic logic [2:0] filter_last_tap(input filter_stage_t stage);
        begin
            case (stage)
                FILTER_FN:   filter_last_tap = 3'd4;
                FILTER_FX:   filter_last_tap = 3'd1;
                default:     filter_last_tap = 3'd6;
            endcase
        end
    endfunction

    function automatic logic signed [23:0] filter4_tap_sample(
        input logic [2:0] tap,
        input logic signed [23:0] x0,
        input logic signed [23:0] x1,
        input logic signed [23:0] x2,
        input logic signed [23:0] x3,
        input logic signed [23:0] y1,
        input logic signed [23:0] y2,
        input logic signed [23:0] y3
    );
        begin
            case (tap)
                3'd0:    filter4_tap_sample = x0;
                3'd1:    filter4_tap_sample = x1;
                3'd2:    filter4_tap_sample = x2;
                3'd3:    filter4_tap_sample = x3;
                3'd4:    filter4_tap_sample = y1;
                3'd5:    filter4_tap_sample = y2;
                3'd6:    filter4_tap_sample = y3;
                default: filter4_tap_sample = 24'sd0;
            endcase
        end
    endfunction

    function automatic logic signed [23:0] filter3_tap_sample(
        input logic [2:0] tap,
        input logic signed [23:0] x0,
        input logic signed [23:0] x1,
        input logic signed [23:0] x2,
        input logic signed [23:0] y1,
        input logic signed [23:0] y2
    );
        begin
            case (tap)
                3'd0:    filter3_tap_sample = x0;
                3'd1:    filter3_tap_sample = x1;
                3'd2:    filter3_tap_sample = x2;
                3'd3:    filter3_tap_sample = y1;
                3'd4:    filter3_tap_sample = y2;
                default: filter3_tap_sample = 24'sd0;
            endcase
        end
    endfunction

    function automatic logic signed [23:0] filter1_tap_sample(
        input logic [2:0] tap,
        input logic signed [23:0] x0,
        input logic signed [23:0] y1
    );
        begin
            case (tap)
                3'd0:    filter1_tap_sample = x0;
                3'd1:    filter1_tap_sample = y1;
                default: filter1_tap_sample = 24'sd0;
            endcase
        end
    endfunction

    task automatic clear_synth_pipeline;
        begin
            synth_state_q <= SYNTH_IDLE;
            filter_stage_q <= FILTER_F1;
            mac_tap_q <= 3'd0;
            mac_accum_q <= 56'sd0;
            mac_sample_q <= 24'sd0;
            mac_coeff_q <= 16'sd0;
            mac_product_q <= 56'sd0;
            synth_voice_gain_q <= 4'd0;
            synth_noise_gain_q <= 4'd0;
            synth_noise_fc_q <= 4'd0;
            synth_noise_mix_gain_q <= 5'd0;
            synth_amp_q <= 4'd0;
            synth_closure_gain_q <= 3'd0;
            synth_voice_source_q <= 16'sd0;
            synth_noise_source_q <= 16'sd0;
            synth_voice_input_q <= 24'sd0;
            synth_noise_input_q <= 24'sd0;
            synth_f1_q <= 24'sd0;
            synth_f2_q <= 24'sd0;
            synth_f2n_scaled_q <= 24'sd0;
            synth_f2n_in_q <= 24'sd0;
            synth_f2n_q <= 24'sd0;
            synth_vn_q <= 24'sd0;
            synth_f3_q <= 24'sd0;
            synth_fn_q <= 24'sd0;
            synth_f4_q <= 24'sd0;
            synth_fx_q <= 24'sd0;
            synth_mixed_q <= 24'sd0;
            synth_noise_scale_sample_q <= 24'sd0;
            synth_noise_base_q <= 24'sd0;
            synth_shaped_noise_q <= 24'sd0;
            synth_closed_q <= 24'sd0;
            synth_bypass_low_q <= 28'sd0;
            synth_bypass_high_q <= 28'sd0;
            synth_bypass_mode_q <= BYPASS_NONE;
            synth_enhanced_fx_q <= 24'sd0;
            synth_attack_fx_q <= 24'sd0;
            synth_scaled_q <= 24'sd0;
            synth_presence_q <= 24'sd0;
            synth_output_scaled_q <= 24'sd0;
            synth_output_gain_q <= 24'sd0;
            synth_output_limited_q <= 16'sd0;
        end
    endtask

    task automatic clear_filter_history;
        begin
            f1_x1_q <= 24'sd0;
            f1_x2_q <= 24'sd0;
            f1_x3_q <= 24'sd0;
            f1_y1_q <= 24'sd0;
            f1_y2_q <= 24'sd0;
            f1_y3_q <= 24'sd0;
            f2_x1_q <= 24'sd0;
            f2_x2_q <= 24'sd0;
            f2_x3_q <= 24'sd0;
            f2_y1_q <= 24'sd0;
            f2_y2_q <= 24'sd0;
            f2_y3_q <= 24'sd0;
            f2n_x1_q <= 24'sd0;
            f2n_x2_q <= 24'sd0;
            f2n_x3_q <= 24'sd0;
            f2n_y1_q <= 24'sd0;
            f2n_y2_q <= 24'sd0;
            f2n_y3_q <= 24'sd0;
            f3_x1_q <= 24'sd0;
            f3_x2_q <= 24'sd0;
            f3_x3_q <= 24'sd0;
            f3_y1_q <= 24'sd0;
            f3_y2_q <= 24'sd0;
            f3_y3_q <= 24'sd0;
            f4_x1_q <= 24'sd0;
            f4_x2_q <= 24'sd0;
            f4_x3_q <= 24'sd0;
            f4_y1_q <= 24'sd0;
            f4_y2_q <= 24'sd0;
            f4_y3_q <= 24'sd0;
            fn_x1_q <= 24'sd0;
            fn_x2_q <= 24'sd0;
            fn_y1_q <= 24'sd0;
            fn_y2_q <= 24'sd0;
            fx_y1_q <= 24'sd0;
            presence_low_q <= 24'sd0;
        end
    endtask

    task automatic clear_pipeline;
        begin
            clear_synth_pipeline();
            clear_filter_history();
        end
    endtask

    task automatic start_synth_sample(input logic [3:0] voice_gain,
                                      input logic [3:0] noise_gain,
                                      input logic [3:0] noise_fc,
                                      input logic [3:0] amp,
                                      input logic [2:0] closure_gain,
                                      input logic signed [15:0] voice_source,
                                      input logic signed [15:0] noise_source);
        begin
            synth_state_q <= SYNTH_EXCITE;
            synth_voice_gain_q <= voice_gain;
            synth_noise_gain_q <= noise_gain;
            synth_noise_fc_q <= noise_fc;
            synth_amp_q <= amp;
            synth_closure_gain_q <= closure_gain;
            synth_voice_source_q <= voice_source;
            synth_noise_source_q <= noise_source;
        end
    endtask

    task automatic stop_audio;
        begin
            active_valid_q <= 1'b0;
            is_votrax_q <= 1'b0;
            idle_decay_count_q <= 10'd0;
            audio_q <= 16'sd0;
            clear_pipeline();
        end
    endtask

    task automatic play_phoneme(input logic votrax);
        begin
            active_valid_q <= 1'b1;
            is_votrax_q <= votrax;
            idle_decay_count_q <= 10'd0;
            clear_synth_pipeline();
            if (!votrax) begin
                clear_filter_history();
            end
        end
    endtask

    task automatic mirror_digital_core_state;
        begin
            ticks_q <= core_ticks;
            pitch_q <= core_pitch;
            pitch_gate_q <= core_pitch_noise_gate;
            closure_q <= core_closure_age;
            cur_noise_q <= core_noise_bit;

            rom_fa_q <= core_rom_fa;
            rom_fc_q <= core_rom_fc;
            rom_va_q <= core_rom_va;
            rom_f1_q <= core_rom_f1;
            rom_f2_q <= core_rom_f2;
            rom_f2q_q <= core_rom_f2q;
            rom_f3_q <= core_rom_f3;
            rom_cld_q <= core_rom_cld;
            rom_vd_q <= core_rom_vd;
            rom_duration_q <= core_rom_duration;
            rom_closure_q <= core_rom_closure;
            rom_pause_q <= core_rom_silence;
            rom_phone_q <= core_rom_phone;

            cur_fa_q <= core_cur_fa;
            cur_fc_q <= core_cur_fc;
            cur_va_q <= core_cur_va;
            cur_f1_q <= core_cur_f1;
            cur_f2_q <= core_cur_f2;
            cur_f2q_q <= core_cur_f2q;
            cur_f3_q <= core_cur_f3;

            filt_fa_q <= core_filt_fa;
            filt_fc_q <= core_filt_fc;
            filt_va_q <= core_filt_va;
            filt_f1_q <= core_filt_f1;
            filt_f2_q <= core_filt_f2;
            filt_f2q_q <= core_filt_f2q;
            filt_f3_q <= core_filt_f3;
        end
    endtask

    task automatic reset_power_state;
        begin
            ticks_q <= 5'd0;
            pitch_q <= 10'd0;
            pitch_gate_q <= 1'b0;
            closure_q <= 5'd0;
            idle_decay_count_q <= 10'd0;
            cur_noise_q <= 1'b0;
            rom_fa_q <= 4'd0;
            rom_fc_q <= 4'd0;
            rom_va_q <= 4'd0;
            rom_f1_q <= 4'd7;
            rom_f2_q <= 4'd9;
            rom_f2q_q <= 4'd4;
            rom_f3_q <= 4'hC;
            rom_cld_q <= 4'd1;
            rom_vd_q <= 4'd1;
            rom_duration_q <= 7'h0F;
            rom_closure_q <= 1'b1;
            rom_pause_q <= 1'b0;
            rom_phone_q <= 6'h3F;
            cur_fa_q <= 8'd0;
            cur_fc_q <= 8'd0;
            cur_va_q <= 8'd0;
            cur_f1_q <= 8'd0;
            cur_f2_q <= 8'd0;
            cur_f2q_q <= 8'd0;
            cur_f3_q <= 8'd0;
            filt_fa_q <= 4'd0;
            filt_fc_q <= 4'd0;
            filt_va_q <= 4'd0;
            filt_f1_q <= 4'd0;
            filt_f2_q <= 5'd0;
            filt_f2q_q <= 4'd0;
            filt_f3_q <= 4'd0;
            phoneme_done_q <= 1'b0;
            stop_audio();
        end
    endtask

    always_ff @(posedge clk) begin
        logic signed [55:0] mac_next;
        logic signed [23:0] filter_out;
        logic signed [55:0] f3_wide;
        logic signed [55:0] noise_wide;
        logic signed [55:0] mixed_next;
        logic signed [23:0] closed_next;

        if (!rstn || !card_enabled) begin
            reset_power_state();
        end else if (warm_reset) begin
            phoneme_done_q <= 1'b0;
            stop_audio();
        end else begin
            phoneme_done_q <= 1'b0;
            mirror_digital_core_state();
            if (start) begin
                play_phoneme(start_votrax);
            end else begin
            if (active_valid_q && core_phoneme_done) begin
                phoneme_done_q <= 1'b1;
            end

            if (!audio_tick) begin
                case (synth_state_q)
                    SYNTH_EXCITE: begin
                        synth_voice_input_q <= scale4(synth_voice_source_q,
                                                      synth_voice_gain_q);
                        synth_noise_input_q <= noise_shaper_input(synth_noise_source_q,
                                                                  synth_noise_gain_q);
                        filter_stage_q <= FILTER_F1;
                        mac_accum_q <= 56'sd0;
                        mac_tap_q <= 3'd0;
                        synth_state_q <= SYNTH_FILTER_PREP;
                    end

                    SYNTH_FILTER_PREP: begin
                        case (filter_stage_q)
                            FILTER_F1: begin
                                mac_sample_q <= filter4_tap_sample(mac_tap_q,
                                                                   synth_voice_input_q,
                                                                   f1_x1_q,
                                                                   f1_x2_q,
                                                                   f1_x3_q,
                                                                   f1_y1_q,
                                                                   f1_y2_q,
                                                                   f1_y3_q);
                                mac_coeff_q <= sc01a_f1_coeff(filt_f1_q, mac_tap_q);
                            end

                            FILTER_F2: begin
                                mac_sample_q <= filter4_tap_sample(mac_tap_q,
                                                                   synth_f1_q,
                                                                   f2_x1_q,
                                                                   f2_x2_q,
                                                                   f2_x3_q,
                                                                   f2_y1_q,
                                                                   f2_y2_q,
                                                                   f2_y3_q);
                                mac_coeff_q <= sc01a_f2_coeff(filt_f2_q,
                                                              filt_f2q_q,
                                                              mac_tap_q);
                            end

                            FILTER_F2N: begin
                                mac_sample_q <= filter4_tap_sample(mac_tap_q,
                                                                   synth_f2n_in_q,
                                                                   f2n_x1_q,
                                                                   f2n_x2_q,
                                                                   f2n_x3_q,
                                                                   f2n_y1_q,
                                                                   f2n_y2_q,
                                                                   f2n_y3_q);
                                mac_coeff_q <= sc01a_f2_coeff(filt_f2_q,
                                                              filt_f2q_q,
                                                              mac_tap_q);
                            end

                            FILTER_F3: begin
                                mac_sample_q <= filter4_tap_sample(mac_tap_q,
                                                                   synth_vn_q,
                                                                   f3_x1_q,
                                                                   f3_x2_q,
                                                                   f3_x3_q,
                                                                   f3_y1_q,
                                                                   f3_y2_q,
                                                                   f3_y3_q);
                                mac_coeff_q <= sc01a_f3_coeff(filt_f3_q, mac_tap_q);
                            end

                            FILTER_FN: begin
                                mac_sample_q <= filter3_tap_sample(mac_tap_q,
                                                                   synth_noise_input_q,
                                                                   fn_x1_q,
                                                                   fn_x2_q,
                                                                   fn_y1_q,
                                                                   fn_y2_q);
                                mac_coeff_q <= sc01a_fn_coeff(mac_tap_q);
                            end

                            FILTER_F4: begin
                                mac_sample_q <= filter4_tap_sample(mac_tap_q,
                                                                   synth_mixed_q,
                                                                   f4_x1_q,
                                                                   f4_x2_q,
                                                                   f4_x3_q,
                                                                   f4_y1_q,
                                                                   f4_y2_q,
                                                                   f4_y3_q);
                                mac_coeff_q <= sc01a_f4_coeff(mac_tap_q);
                            end

                            FILTER_FX: begin
                                mac_sample_q <= filter1_tap_sample(mac_tap_q,
                                                                   synth_closed_q,
                                                                   fx_y1_q);
                                mac_coeff_q <= sc01a_fx_coeff(mac_tap_q);
                            end

                            default: begin
                                mac_sample_q <= 24'sd0;
                                mac_coeff_q <= 16'sd0;
                            end
                        endcase
                        synth_state_q <= SYNTH_FILTER_MULT;
                    end

                    SYNTH_FILTER_MULT: begin
                        mac_product_q <= mac_product(mac_sample_q, mac_coeff_q);
                        synth_state_q <= SYNTH_FILTER_ACCUM;
                    end

                    SYNTH_FILTER_ACCUM: begin
                        mac_next = mac_accum_q + mac_product_q;
                        if (mac_tap_q == filter_last_tap(filter_stage_q)) begin
                            filter_out = sat24_from56(mac_next >>> SC01_COEFF_FRAC_BITS);
                            mac_accum_q <= 56'sd0;
                            mac_tap_q <= 3'd0;

                            case (filter_stage_q)
                                FILTER_F1: begin
                                    synth_f1_q <= filter_out;
                                    f1_x3_q <= f1_x2_q;
                                    f1_x2_q <= f1_x1_q;
                                    f1_x1_q <= synth_voice_input_q;
                                    f1_y3_q <= f1_y2_q;
                                    f1_y2_q <= f1_y1_q;
                                    f1_y1_q <= filter_out;
                                    filter_stage_q <= FILTER_F2;
                                    synth_state_q <= SYNTH_FILTER_PREP;
                                end

                                FILTER_F2: begin
                                    synth_f2_q <= filter_out;
                                    f2_x3_q <= f2_x2_q;
                                    f2_x2_q <= f2_x1_q;
                                    f2_x1_q <= synth_f1_q;
                                    f2_y3_q <= f2_y2_q;
                                    f2_y2_q <= f2_y1_q;
                                    f2_y1_q <= filter_out;
                                    filter_stage_q <= FILTER_FN;
                                    synth_state_q <= SYNTH_FILTER_PREP;
                                end

                                FILTER_F3: begin
                                    synth_f3_q <= filter_out;
                                    f3_x3_q <= f3_x2_q;
                                    f3_x2_q <= f3_x1_q;
                                    f3_x1_q <= synth_vn_q;
                                    f3_y3_q <= f3_y2_q;
                                    f3_y2_q <= f3_y1_q;
                                    f3_y1_q <= filter_out;
                                    // F3 done -> step-11 secondary noise insertion.
                                    synth_state_q <= SYNTH_NOISE_GAIN;
                                end

                                FILTER_FN: begin
                                    synth_fn_q <= filter_out;
                                    fn_x2_q <= fn_x1_q;
                                    fn_x1_q <= synth_noise_input_q;
                                    fn_y2_q <= fn_y1_q;
                                    fn_y1_q <= filter_out;
                                    // Pipeline the MAME step-7 gain so the FN
                                    // MAC result does not feed scale/saturate
                                    // logic in the same 133 MHz cycle.
                                    synth_state_q <= SYNTH_F2N_SCALE;
                                end

                                FILTER_F2N: begin
                                    synth_f2n_q <= filter_out;
                                    f2n_x3_q <= f2n_x2_q;
                                    f2n_x2_q <= f2n_x1_q;
                                    f2n_x1_q <= synth_f2n_in_q;
                                    f2n_y3_q <= f2n_y2_q;
                                    f2n_y2_q <= f2n_y1_q;
                                    f2n_y1_q <= filter_out;
                                    // Pipeline the MAME step-9 mix so the F2N
                                    // MAC result does not feed adder/saturate
                                    // logic in the same 133 MHz cycle.
                                    synth_state_q <= SYNTH_F2N_MIX;
                                end

                                FILTER_F4: begin
                                    synth_f4_q <= filter_out;
                                    f4_x3_q <= f4_x2_q;
                                    f4_x2_q <= f4_x1_q;
                                    f4_x1_q <= synth_mixed_q;
                                    f4_y3_q <= f4_y2_q;
                                    f4_y2_q <= f4_y1_q;
                                    f4_y1_q <= filter_out;
                                    synth_state_q <= SYNTH_CLOSURE;
                                end

                                FILTER_FX: begin
                                    synth_fx_q <= filter_out;
                                    fx_y1_q <= filter_out;
                                    synth_state_q <= SYNTH_SCALE;
                                end

                                default: begin
                                    synth_state_q <= SYNTH_IDLE;
                                end
                            endcase
                        end else begin
                            mac_accum_q <= mac_next;
                            mac_tap_q <= mac_tap_q + 3'd1;
                            synth_state_q <= SYNTH_FILTER_PREP;
                        end
                    end

                    SYNTH_F2N_SCALE: begin
                        // First half of MAME step 7: scale FN-shaped noise by
                        // FC. Boost/saturation is in the next cycle.
                        synth_f2n_scaled_q <= scale4_from24(synth_fn_q, filt_fc_q);
                        synth_state_q <= SYNTH_F2N_INPUT_GAIN;
                    end

                    SYNTH_F2N_INPUT_GAIN: begin
                        // Second half of MAME step 7: boost the scaled FN
                        // noise and feed the noise half of the F2 filter.
                        synth_f2n_in_q <= f2n_boost_from24(synth_f2n_scaled_q);
                        filter_stage_q <= FILTER_F2N;
                        synth_state_q <= SYNTH_FILTER_PREP;
                    end

                    SYNTH_F2N_MIX: begin
                        // MAME step 9: mix voice F2 + noise F2 ahead of F3
                        // (vn = v + n2).
                        synth_vn_q <= sat24_from28(
                            {{4{synth_f2_q[23]}}, synth_f2_q} +
                            {{4{synth_f2n_q[23]}}, synth_f2n_q});
                        filter_stage_q <= FILTER_F3;
                        synth_state_q <= SYNTH_FILTER_PREP;
                    end

                    SYNTH_NOISE_GAIN: begin
                        // MAME step 11: secondary noise insertion after F3,
                        // gain (5 + (15 ^ FC))/20. The main fricative path is
                        // the F2-noise filter mixed in ahead of F3 (above).
                        synth_noise_mix_gain_q <= 5'd5 + {1'b0, (4'hF ^ synth_noise_fc_q)};
                        synth_state_q <= SYNTH_NOISE_MIX;
                    end

                    SYNTH_NOISE_MIX: begin
                        synth_noise_scale_sample_q <= synth_fn_q;
                        synth_noise_base_q <= scale20_base_from24(synth_fn_q,
                                                                  synth_noise_mix_gain_q);
                        synth_state_q <= SYNTH_NOISE_FINE;
                    end

                    SYNTH_NOISE_FINE: begin
                        synth_shaped_noise_q <=
                            synth_noise_base_q +
                            scale20_fine_from24(synth_noise_scale_sample_q,
                                                synth_noise_mix_gain_q);
                        synth_state_q <= SYNTH_NOISE_SCALE;
                    end

                    SYNTH_NOISE_SCALE: begin
                        f3_wide = {{32{synth_f3_q[23]}}, synth_f3_q};
                        noise_wide = {{32{synth_shaped_noise_q[23]}}, synth_shaped_noise_q};
                        mixed_next = f3_wide + noise_wide;
                        synth_mixed_q <= sat24_from56(mixed_next);
                        filter_stage_q <= FILTER_F4;
                        mac_accum_q <= 56'sd0;
                        mac_tap_q <= 3'd0;
                        synth_state_q <= SYNTH_FILTER_PREP;
                    end

                    SYNTH_CLOSURE: begin
                        closed_next = scale7_from24(synth_f4_q,
                                                    synth_closure_gain_q);
                        synth_closed_q <= closed_next;
                        filter_stage_q <= FILTER_FX;
                        mac_accum_q <= 56'sd0;
                        mac_tap_q <= 3'd0;
                        synth_state_q <= SYNTH_FILTER_PREP;
                    end

                    SYNTH_SCALE: begin
                        synth_bypass_low_q <= {{4{synth_fx_q[23]}}, synth_fx_q};
                        synth_bypass_high_q <=
                            {{4{synth_closed_q[23]}}, synth_closed_q} -
                            {{4{synth_fx_q[23]}}, synth_fx_q};
                        if (ch_fricative_phone()) begin
                            synth_bypass_mode_q <= BYPASS_PRESENCE;
                        end else if (nonclosure_noise_phone()) begin
                            synth_bypass_mode_q <= BYPASS_FRICATIVE;
                        end else if (filt_va_q != 4'd0 && filt_fa_q == 4'd0) begin
                            synth_bypass_mode_q <= BYPASS_PRESENCE;
                        end else begin
                            synth_bypass_mode_q <= BYPASS_NONE;
                        end
                        synth_state_q <= SYNTH_BYPASS_APPLY;
                    end

                    SYNTH_BYPASS_APPLY: begin
                        case (synth_bypass_mode_q)
                            BYPASS_PRESENCE: begin
                                synth_enhanced_fx_q <= sat24_from28(
                                    synth_bypass_low_q +
                                    (synth_bypass_high_q >>> 1) +
                                    (synth_bypass_high_q >>> 2));
                            end

                            BYPASS_FRICATIVE: begin
                                synth_enhanced_fx_q <= sat24_from28(
                                    synth_bypass_low_q +
                                    synth_bypass_high_q +
                                    (synth_bypass_high_q >>> 2));
                            end

                            default: begin
                                synth_enhanced_fx_q <= sat24_from28(
                                    synth_bypass_low_q);
                            end
                        endcase
                        synth_state_q <= SYNTH_ATTACK_BOOST;
                    end

                    SYNTH_ATTACK_BOOST: begin
                        synth_attack_fx_q <= consonant_attack_mix_from24(
                            synth_enhanced_fx_q);
                        synth_state_q <= SYNTH_AMP_SCALE;
                    end

                    SYNTH_AMP_SCALE: begin
                        synth_scaled_q <= scale4_from24(synth_attack_fx_q,
                                                        synth_amp_q);
                        synth_state_q <= SYNTH_PRESENCE;
                    end

                    SYNTH_PRESENCE: begin
                        synth_presence_q <= presence_boost_from24(synth_scaled_q,
                                                                  presence_low_q);
                        presence_low_q <= presence_low_next_from24(synth_scaled_q,
                                                                   presence_low_q);
                        synth_state_q <= SYNTH_OUTPUT_SCALE;
                    end

                    SYNTH_OUTPUT_SCALE: begin
                        synth_output_scaled_q <= scale4_from24(
                            synth_presence_q,
                            is_votrax_q ? VOTRAX_OUTPUT_SCALE : SSI263_OUTPUT_SCALE);
                        synth_state_q <= SYNTH_OUTPUT_GAIN;
                    end

                    SYNTH_OUTPUT_GAIN: begin
                        synth_output_gain_q <= formant_output_gain(synth_output_scaled_q);
                        synth_state_q <= SYNTH_OUTPUT_LIMIT;
                    end

                    SYNTH_OUTPUT_LIMIT: begin
                        synth_output_limited_q <= soft_limit16_from24(synth_output_gain_q);
                        synth_state_q <= SYNTH_OUT;
                    end

                    SYNTH_OUT: begin
                        audio_q <= slew_limit16(audio_q, synth_output_limited_q);
                        synth_state_q <= SYNTH_IDLE;
                    end

                    default: begin
                        synth_state_q <= SYNTH_IDLE;
                    end
                endcase
            end

            if (audio_tick) begin
                if (!active_valid_q) begin
                    start_synth_sample(4'd0,
                                       4'd0,
                                       4'd0,
                                       4'd0,
                                       3'd0,
                                       16'sd0,
                                       16'sd0);
                end else begin
                    if (!is_votrax_q &&
                        ((ctrl_art_amp & CONTROL_MASK) != 8'd0 ||
                         filter_freq == FILTER_FREQ_SILENCE ||
                         (ctrl_art_amp & AMPLITUDE_MASK) == 8'd0)) begin
                        start_synth_sample(4'd0,
                                           4'd0,
                                           4'd0,
                                           4'd0,
                                           current_closure_gain(),
                                           16'sd0,
                                           16'sd0);
                    end else if (ticks_q == 5'h10) begin
                        if (idle_decay_count_q == FORMANT_IDLE_DECAY_SAMPLES) begin
                            stop_audio();
                        end else begin
                            idle_decay_count_q <= idle_decay_count_q + 10'd1;
                            start_synth_sample(4'd0,
                                               4'd0,
                                               filt_fc_q,
                                               is_votrax_q ? 4'd15 : ctrl_art_amp[3:0],
                                               current_closure_gain(),
                                               16'sd0,
                                               16'sd0);
                        end
                    end else begin
                        idle_decay_count_q <= 10'd0;
                        start_synth_sample(filt_va_q,
                                           filt_fa_q,
                                           filt_fc_q,
                                           is_votrax_q ? 4'd15 : ctrl_art_amp[3:0],
                                           current_closure_gain(),
                                           voice_source_from_pitch(pitch_q),
                                           noise_source(pitch_gate_q, cur_noise_q));
                    end
                end
            end
            end
        end
    end

endmodule
