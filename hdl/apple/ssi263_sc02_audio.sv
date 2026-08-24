`timescale 1ns / 1ps

// Native SSI-263A / SC-02 excitation and switched-capacitor audio model.
//
// This block deliberately has no SC-01 phone map or coefficient tables.  It
// consumes the persistent controls and clock enables produced by
// ssi263_sc02_core.  The pitch shaper and HCC4006 recurrence follow sheets 3
// and 6 structurally.  Each formant section keeps the charge state p and
// output state y of the two switched integrators.  Its alpha, a, b, and g
// coefficients are the exact capacitor ratios drawn on sheets 1 and 2.  F2
// includes the charge load from its fixed and selected resonance capacitors.
// The ideal tract follows the section topology on sheets 1 and 2:
//
//   VOICED -> F1 -> F2(+FRIC_1) -> F3 -> F4 -> F5(+FRIC_2)
//
// FRIC_1 and FRIC_2 are injection nodes, not independent output resonators.
// LF356 bandwidth, CD4016 resistance, and stray capacitance remain outside the
// ideal charge model.  The section graph and terms are independent of SC-01.
module ssi263_sc02_audio #(
    parameter logic [3:0] NOISE_D1_SEED = 4'h1,
    parameter logic [4:0] NOISE_D2_SEED = 5'h00,
    parameter logic [3:0] NOISE_D3_SEED = 4'h0,
    parameter logic [4:0] NOISE_D4_SEED = 5'h00,
    parameter logic [3:0] NOISE_COUNT_SEED = 4'hF
) (
    input  logic               clk,
    input  logic               rstn,

    input  logic               audio_tick,
    input  logic               phone_active,
    input  logic               powered_down,

    // Proved sheet-6 source and switch controls from ssi263_sc02_core.
    input  logic               fricative,
    input  logic               voiced,
    input  logic               pw_2,
    input  logic               pw_3,
    // Rising edge of the gated U41C clock that drives U75 and U73 together.
    // PW3 is held phone state; the core resolves its recurring SEL1/U62 gate.
    input  logic               noise_clock_ce,
    input  logic               fric1_sw,
    input  logic               fric2_sw,
    // Held U62 Q from the core.  U62 already divides raw U59 VOICECLK by two;
    // keeping it in the core avoids two copies of its reset/state semantics.
    input  logic               voice_toggle,
    input  logic               filter_phase_ce,
    input  logic               filter_phase,
    input  logic [7:0]         filter_frequency,

    input  logic [3:0]         f1_code,
    input  logic [3:0]         f2_code,
    input  logic [3:0]         f2_res_code,
    input  logic [3:0]         f3_code,
    input  logic [3:0]         f4_code,
    input  logic [3:0]         filter_amp_code,
    input  logic [3:0]         voice_amp_code,
    input  logic [3:0]         fric_amp_code,

    // CLOSURE is the one-clock U52C pulse at the U49/U43 filter boundary.  It
    // is filter timing, not a static phone flag or a pitch-terminal pulse.
    input  logic               closure,

    output logic signed [15:0] audio_sample
);

    // Signed 17-bit values with fourteen fractional bits.  Values above one
    // occur in the drawn F4/F5 charge ratios, so keep the full signed range.
    localparam logic signed [16:0] F1_ALPHA_Q14 = 17'sd16104;
    localparam logic signed [16:0] F1_A_Q14     = 17'sd3781;
    localparam logic signed [16:0] F1_G_Q14     = 17'sd3781;
    localparam logic signed [16:0] F2_FRIC_H_Q14 = 17'sd2409;
    localparam logic signed [16:0] F3_ALPHA_Q14 = 17'sd15715;
    localparam logic signed [16:0] F3_A_Q14     = 17'sd13040;
    localparam logic signed [16:0] F3_G_Q14     = 17'sd6687;
    localparam logic signed [16:0] F4_ALPHA_Q14 = 17'sd15656;
    localparam logic signed [16:0] F4_A_Q14     = 17'sd17112;
    localparam logic signed [16:0] F5_ALPHA_Q14 = 17'sd15154;
    localparam logic signed [16:0] F5_A_Q14     = 17'sd20645;
    localparam logic signed [16:0] F5_B_Q14     = 17'sd22320;
    localparam logic signed [16:0] F5_FRIC_BASE_G_Q14 = 17'sd5051;
    localparam logic signed [16:0] F5_FRIC_SW_G_Q14   = 17'sd16252;
    localparam logic signed [16:0] FILTER_OUTPUT_ALPHA_Q14 = 17'sd16086;
    // Leave 18 dB of line-output headroom for the largest ROM/formant pair and
    // for the card's later PSG/speech mix.  The schematic charge-state tract
    // and DC blocker stay at full precision; only the AO-to-PCM map uses 1/8.
    localparam integer LINE_OUTPUT_SHIFT = 3;

    logic pitch_sync1_q;
    logic pitch_sync2_q;
    logic voice_load_pending_q;
    logic [3:0] voice_shape_q;
    logic voiced_q;

    logic [3:0] noise_d1_q;
    logic [4:0] noise_d2_q;
    logic [3:0] noise_d3_q;
    logic [4:0] noise_d4_q;
    logic [3:0] noise_count_q;
    logic [3:0] noise_count_next;
    logic noise_force;
    logic noise_feedback;
    logic noise_bit;
    logic u72a_fric_gate;
    logic noise_advance;

    logic signed [23:0] voice_source;
    logic signed [23:0] fric1_source;
    logic signed [23:0] fric2_source;
    logic signed [23:0] voice_magnitude;
    logic signed [17:0] fric_drive;
    logic signed [17:0] fric_drive_history_q;
    logic signed [18:0] fric_drive_delta;
    logic signed [18:0] fric1_gain_extended;
    logic signed [18:0] fric2_gain_extended;
    logic signed [18:0] fric2_drive_charge;
    logic signed [47:0] fric2_source_accumulator;
    logic signed [23:0] fric2_source_state_q;
    logic signed [23:0] fric2_source_next;

    // Phi0 samples both noise amplifier nodes and route switches.  U156C and
    // U129D reset both C143 plates each pair, while C150 follows unreset U152
    // and switched C151 retains a separate source-side charge while open.
    logic signed [23:0] fric1_source_phi0_q;
    logic signed [23:0] fric2_source_phi0_q;
    logic signed [23:0] fric2_base_history_q;
    logic signed [23:0] fric2_sw_history_q;
    logic               fric1_sw_phi0_q;
    logic               fric2_sw_phi0_q;
    logic [3:0]         filter_amp_phi0_q;
    logic [3:0]         f1_code_phi0_q;
    logic [3:0]         f2_code_phi0_q;
    logic [3:0]         f2_res_code_phi0_q;
    logic [3:0]         f3_code_phi0_q;
    logic [3:0]         f4_code_phi0_q;

    // B/D/P/T/K are silent stops (PW2=1, PW3=0).  The following phone's
    // normal source passes through parameters that start at the stop's tract
    // shape; no periodic or guessed stop-source burst is added here.
    logic stop_class;
    logic source_voiced;
    logic source_fricative;
    logic source_pw3;
    logic source_fric1_sw;
    logic source_fric2_sw;
    logic [3:0] source_voice_amp_code;
    logic [3:0] source_fric_amp_code;

    logic signed [23:0] f1_state_q;
    logic signed [23:0] f1_history_q;
    logic signed [23:0] f2_state_q;
    logic signed [23:0] f2_history_q;
    logic signed [23:0] f3_state_q;
    logic signed [23:0] f3_history_q;
    logic signed [23:0] f4_state_q;
    logic signed [23:0] f4_history_q;
    logic signed [23:0] f5_state_q;
    logic signed [23:0] f5_history_q;
    logic signed [23:0] f1_input_q;
    logic signed [23:0] f2_input_q;
    logic signed [23:0] f3_input_q;
    logic signed [23:0] f4_input_q;
    logic signed [23:0] f5_input_q;
    logic signed [23:0] f1_input_history_q;
    logic signed [23:0] f3_side_input_q;
    logic signed [23:0] f3_side_history_q;
    logic signed [23:0] output_hold_q;
    logic signed [23:0] reconstruction_hold_q;
    logic signed [23:0] dc_previous_input_q;
    logic signed [23:0] dc_input;
    logic signed [24:0] dc_delta_q;
    logic signed [26:0] dc_sum_q;
    logic signed [26:0] dc_filtered_wide;
    logic signed [25:0] dc_output_q;
    logic dc_stage1_pending_q;
    logic dc_stage2_pending_q;
    logic dc_active_stage1_q;
    logic dc_active_stage2_q;
    // Two registered logical product lanes form a 34-cycle scheduler.  Each
    // section keeps six fixed slots.  F5 takes one slot for the independent
    // C150/C151 histories and one to register its completed state before the
    // U146 delta subtraction.  fN_state_q is y; fN_history_q is the matching
    // p charge state so existing focused probes remain useful.
    // The default XCK/DIV2 profile leaves at least 133 fabric clocks between
    // the fastest filter phases, with margin for two independent instances.
    logic engine_busy_q;
    logic engine_overrun_q;
    logic [2:0] engine_section_q;
    logic [3:0] engine_stage_q;
    logic signed [23:0] engine_state_y_q;
    logic signed [23:0] engine_state_p_q;
    logic signed [24:0] engine_main_delta_q;
    logic signed [16:0] engine_alpha_q;
    logic signed [16:0] engine_a_q;
    logic signed [16:0] engine_b_q;
    logic signed [24:0] engine_side_delta_q;
    logic signed [16:0] engine_g_q;
    logic signed [24:0] engine_side2_delta_q;
    logic signed [16:0] engine_g2_q;
    logic signed [24:0] engine_output_delta_q;
    logic signed [16:0] engine_h_q;
    logic signed [24:0] engine_operand_a;
    logic signed [24:0] engine_operand_b;
    logic signed [16:0] engine_coefficient_a;
    logic signed [16:0] engine_coefficient_b;
    logic signed [41:0] engine_product_a;
    logic signed [41:0] engine_product_b;
    logic signed [41:0] product_a_q;
    logic signed [41:0] product_b_q;
    logic signed [24:0] output_sum_q;
    logic signed [23:0] output_old_state_q;
    logic signed [23:0] engine_charge_next;
    logic signed [23:0] engine_section_next;
    logic signed [23:0] engine_output_next;
    logic signed [47:0] product_a_q_ext;
    logic signed [47:0] product_b_q_ext;
    logic signed [47:0] recurrence_accumulator;
    logic signed [47:0] recurrence_accumulator_q;
    logic signed [47:0] charge_accumulator;
    logic signed [47:0] rounded_charge_accumulator;
    logic signed [47:0] state_y_q14;
    logic signed [47:0] section_accumulator;
    logic signed [47:0] section_accumulator_q;
    logic signed [47:0] complete_section_accumulator;
    logic signed [47:0] rounded_section_accumulator;
    logic signed [47:0] output_accumulator;
    logic signed [47:0] rounded_output_accumulator;
    logic signed [23:0] charge_work_q;

    function automatic logic signed [23:0] sat24_from48(
        input logic signed [47:0] value
    );
        begin
            /* A value fits in 24 bits exactly when bits 46:23 extend its
             * sign. Test those bits directly instead of inferring a wide
             * signed magnitude compare and its carry chain. */
            if (!value[47] && (|value[46:23]))
                sat24_from48 = 24'sh7FFFFF;
            else if (value[47] && !(&value[46:23]))
                sat24_from48 = -24'sd8388608;
            else
                sat24_from48 = value[23:0];
        end
    endfunction

    function automatic logic signed [25:0] sat26_from27(
        input logic signed [26:0] value
    );
        begin
            if (!value[26] && value[25])
                sat26_from27 = {1'b0, 25'h1FFFFFF};
            else if (value[26] && !value[25])
                sat26_from27 = {1'b1, 25'd0};
            else
                sat26_from27 = value[25:0];
        end
    endfunction

    function automatic logic signed [15:0] sat16_from27(
        input logic signed [26:0] value
    );
        begin
            if (!value[26] && (|value[25:15]))
                sat16_from27 = 16'sh7FFF;
            else if (value[26] && !(&value[25:15]))
                sat16_from27 = -16'sd32768;
            else
                sat16_from27 = value[15:0];
        end
    endfunction

    // Physical switched-capacitor totals in pF.  The small integer arithmetic
    // remains exact and feeds the Q14 charge-ratio tables below.
    function automatic logic [12:0] f1_capacitance(input logic [3:0] code);
        integer total;
        begin
            total = 0;
            if (code[0]) total = total + 160;
            if (code[1]) total = total + 330;
            if (code[2]) total = total + 660;
            if (code[3]) total = total + 1300;
            f1_capacitance = total[12:0];
        end
    endfunction

    function automatic logic [12:0] f2_capacitance(input logic [3:0] code);
        integer total;
        begin
            total = 0;
            if (code[0]) total = total + 280;
            if (code[1]) total = total + 560;
            if (code[2]) total = total + 1120;
            if (code[3]) total = total + 2300;
            f2_capacitance = total[12:0];
        end
    endfunction

    function automatic logic [12:0] f2_res_capacitance(input logic [3:0] code);
        integer total;
        begin
            total = 0;
            if (code[0]) total = total + 220;
            if (code[1]) total = total + 430;
            if (code[2]) total = total + 870;
            if (code[3]) total = total + 1800;
            f2_res_capacitance = total[12:0];
        end
    endfunction

    function automatic logic [12:0] f3_capacitance(input logic [3:0] code);
        integer total;
        begin
            total = 0;
            if (code[0]) total = total + 210;
            if (code[1]) total = total + 420;
            if (code[2]) total = total + 820;
            if (code[3]) total = total + 1640;
            f3_capacitance = total[12:0];
        end
    endfunction

    function automatic logic [12:0] f4_capacitance(input logic [3:0] code);
        integer total;
        begin
            total = 0;
            if (code[0]) total = total + 200;
            if (code[1]) total = total + 400;
            if (code[2]) total = total + 820;
            if (code[3]) total = total + 1620;
            f4_capacitance = total[12:0];
        end
    endfunction

    // Exact rounded capacitor ratios in Q14.  Keeping these as named case
    // tables avoids synthesizing constant dividers and makes the schematic
    // values directly checkable in the focused bench.
    function automatic logic signed [16:0] f1_b_q14(
        input logic [3:0] code
    );
        begin
            case (code)
                4'h0: f1_b_q14 = 17'sd356;
                4'h1: f1_b_q14 = 17'sd584;
                4'h2: f1_b_q14 = 17'sd826;
                4'h3: f1_b_q14 = 17'sd1054;
                4'h4: f1_b_q14 = 17'sd1296;
                4'h5: f1_b_q14 = 17'sd1524;
                4'h6: f1_b_q14 = 17'sd1767;
                4'h7: f1_b_q14 = 17'sd1995;
                4'h8: f1_b_q14 = 17'sd2208;
                4'h9: f1_b_q14 = 17'sd2436;
                4'hA: f1_b_q14 = 17'sd2678;
                4'hB: f1_b_q14 = 17'sd2906;
                4'hC: f1_b_q14 = 17'sd3149;
                4'hD: f1_b_q14 = 17'sd3377;
                4'hE: f1_b_q14 = 17'sd3619;
                default: f1_b_q14 = 17'sd3847;
            endcase
        end
    endfunction

    function automatic logic signed [16:0] f2_b_q14(
        input logic [3:0] code
    );
        begin
            case (code)
                4'h0: f2_b_q14 = 17'sd1205;
                4'h1: f2_b_q14 = 17'sd1879;
                4'h2: f2_b_q14 = 17'sd2554;
                4'h3: f2_b_q14 = 17'sd3229;
                4'h4: f2_b_q14 = 17'sd3903;
                4'h5: f2_b_q14 = 17'sd4578;
                4'h6: f2_b_q14 = 17'sd5253;
                4'h7: f2_b_q14 = 17'sd5927;
                4'h8: f2_b_q14 = 17'sd6746;
                4'h9: f2_b_q14 = 17'sd7421;
                4'hA: f2_b_q14 = 17'sd8096;
                4'hB: f2_b_q14 = 17'sd8770;
                4'hC: f2_b_q14 = 17'sd9445;
                4'hD: f2_b_q14 = 17'sd10120;
                4'hE: f2_b_q14 = 17'sd10794;
                default: f2_b_q14 = 17'sd11469;
            endcase
        end
    endfunction

    function automatic logic signed [16:0] f2_alpha_q14(
        input logic [3:0] code
    );
        begin
            // 6800/(6800 + 200 + CRES_selected).  The 200 pF fixed load
            // and selected RES bank bound the otherwise lossless ideal F2.
            case (code)
                4'h0: f2_alpha_q14 = 17'sd15916;
                4'h1: f2_alpha_q14 = 17'sd15431;
                4'h2: f2_alpha_q14 = 17'sd14995;
                4'h3: f2_alpha_q14 = 17'sd14564;
                4'h4: f2_alpha_q14 = 17'sd14156;
                4'h5: f2_alpha_q14 = 17'sd13771;
                4'h6: f2_alpha_q14 = 17'sd13423;
                4'h7: f2_alpha_q14 = 17'sd13076;
                4'h8: f2_alpha_q14 = 17'sd12660;
                4'h9: f2_alpha_q14 = 17'sd12352;
                4'hA: f2_alpha_q14 = 17'sd12071;
                4'hB: f2_alpha_q14 = 17'sd11790;
                4'hC: f2_alpha_q14 = 17'sd11521;
                4'hD: f2_alpha_q14 = 17'sd11265;
                4'hE: f2_alpha_q14 = 17'sd11031;
                default: f2_alpha_q14 = 17'sd10796;
            endcase
        end
    endfunction

    function automatic logic signed [16:0] f2_a_q14(
        input logic [3:0] code
    );
        begin
            // 4700/(6800 + 200 + CRES_selected).  Use the same loaded
            // denominator as alpha so the first integrator conserves charge.
            case (code)
                4'h0: f2_a_q14 = 17'sd11001;
                4'h1: f2_a_q14 = 17'sd10665;
                4'h2: f2_a_q14 = 17'sd10364;
                4'h3: f2_a_q14 = 17'sd10066;
                4'h4: f2_a_q14 = 17'sd9785;
                4'h5: f2_a_q14 = 17'sd9519;
                4'h6: f2_a_q14 = 17'sd9278;
                4'h7: f2_a_q14 = 17'sd9038;
                4'h8: f2_a_q14 = 17'sd8751;
                4'h9: f2_a_q14 = 17'sd8537;
                4'hA: f2_a_q14 = 17'sd8343;
                4'hB: f2_a_q14 = 17'sd8149;
                4'hC: f2_a_q14 = 17'sd7963;
                4'hD: f2_a_q14 = 17'sd7786;
                4'hE: f2_a_q14 = 17'sd7624;
                default: f2_a_q14 = 17'sd7462;
            endcase
        end
    endfunction

    function automatic logic signed [16:0] f3_b_q14(
        input logic [3:0] code
    );
        begin
            case (code)
                4'h0: f3_b_q14 = 17'sd2858;
                4'h1: f3_b_q14 = 17'sd3591;
                4'h2: f3_b_q14 = 17'sd4323;
                4'h3: f3_b_q14 = 17'sd5055;
                4'h4: f3_b_q14 = 17'sd5717;
                4'h5: f3_b_q14 = 17'sd6449;
                4'h6: f3_b_q14 = 17'sd7181;
                4'h7: f3_b_q14 = 17'sd7913;
                4'h8: f3_b_q14 = 17'sd8575;
                4'h9: f3_b_q14 = 17'sd9308;
                4'hA: f3_b_q14 = 17'sd10040;
                4'hB: f3_b_q14 = 17'sd10772;
                4'hC: f3_b_q14 = 17'sd11434;
                4'hD: f3_b_q14 = 17'sd12166;
                4'hE: f3_b_q14 = 17'sd12898;
                default: f3_b_q14 = 17'sd13630;
            endcase
        end
    endfunction

    function automatic logic signed [16:0] f4_b_q14(
        input logic [3:0] code
    );
        begin
            // The second sample always includes C155=1200+470=1670 pF.
            case (code)
                4'h0: f4_b_q14 = 17'sd6363;
                4'h1: f4_b_q14 = 17'sd7125;
                4'h2: f4_b_q14 = 17'sd7887;
                4'h3: f4_b_q14 = 17'sd8649;
                4'h4: f4_b_q14 = 17'sd9487;
                4'h5: f4_b_q14 = 17'sd10250;
                4'h6: f4_b_q14 = 17'sd11012;
                4'h7: f4_b_q14 = 17'sd11774;
                4'h8: f4_b_q14 = 17'sd12536;
                4'h9: f4_b_q14 = 17'sd13298;
                4'hA: f4_b_q14 = 17'sd14060;
                4'hB: f4_b_q14 = 17'sd14822;
                4'hC: f4_b_q14 = 17'sd15660;
                4'hD: f4_b_q14 = 17'sd16422;
                4'hE: f4_b_q14 = 17'sd17184;
                default: f4_b_q14 = 17'sd17946;
            endcase
        end
    endfunction

    function automatic logic [12:0] voice_capacitance(input logic [3:0] code);
        integer total;
        begin
            total = 0;
            if (code[0]) total = total + 220;
            if (code[1]) total = total + 430;
            if (code[2]) total = total + 870;
            if (code[3]) total = total + 1800;
            voice_capacitance = total[12:0];
        end
    endfunction

    function automatic logic [12:0] fric1_capacitance(input logic [3:0] code);
        integer total;
        begin
            total = 0;
            if (code[0]) total = total + 270;
            if (code[1]) total = total + 512;
            if (code[2]) total = total + 1068;
            if (code[3]) total = total + 2160;
            fric1_capacitance = total[12:0];
        end
    endfunction

    function automatic logic [12:0] fric2_capacitance(input logic [3:0] code);
        integer total;
        begin
            total = 0;
            if (code[0]) total = total + 270;
            if (code[1]) total = total + 530;
            if (code[2]) total = total + 1082;
            if (code[3]) total = total + 2160;
            fric2_capacitance = total[12:0];
        end
    endfunction

    function automatic logic [12:0] filter_amp_capacitance(
        input logic [3:0] code
    );
        integer total;
        begin
            total = 0;
            if (code[0]) total = total + 76;
            if (code[1]) total = total + 150;
            if (code[2]) total = total + 300;
            if (code[3]) total = total + 600;
            filter_amp_capacitance = total[12:0];
        end
    endfunction

    function automatic logic [12:0] voice_gain(input logic [3:0] code);
        begin
            // Exact rounded Cselected/C205 values in Q12; C205 is 3300 pF.
            case (code)
                4'h0: voice_gain = 13'd0;
                4'h1: voice_gain = 13'd273;
                4'h2: voice_gain = 13'd534;
                4'h3: voice_gain = 13'd807;
                4'h4: voice_gain = 13'd1080;
                4'h5: voice_gain = 13'd1353;
                4'h6: voice_gain = 13'd1614;
                4'h7: voice_gain = 13'd1887;
                4'h8: voice_gain = 13'd2234;
                4'h9: voice_gain = 13'd2507;
                4'hA: voice_gain = 13'd2768;
                4'hB: voice_gain = 13'd3041;
                4'hC: voice_gain = 13'd3314;
                4'hD: voice_gain = 13'd3587;
                4'hE: voice_gain = 13'd3848;
                default: voice_gain = 13'd4121;
            endcase
        end
    endfunction

    function automatic logic [12:0] fric1_gain(input logic [3:0] code);
        begin
            // Exact rounded Cselected/C133 values in Q12; C133 is 3900 pF.
            case (code)
                4'h0: fric1_gain = 13'd0;
                4'h1: fric1_gain = 13'd284;
                4'h2: fric1_gain = 13'd538;
                4'h3: fric1_gain = 13'd821;
                4'h4: fric1_gain = 13'd1122;
                4'h5: fric1_gain = 13'd1405;
                4'h6: fric1_gain = 13'd1659;
                4'h7: fric1_gain = 13'd1943;
                4'h8: fric1_gain = 13'd2269;
                4'h9: fric1_gain = 13'd2552;
                4'hA: fric1_gain = 13'd2806;
                4'hB: fric1_gain = 13'd3090;
                4'hC: fric1_gain = 13'd3390;
                4'hD: fric1_gain = 13'd3674;
                4'hE: fric1_gain = 13'd3928;
                default: fric1_gain = 13'd4212;
            endcase
        end
    endfunction

    function automatic logic [12:0] fric2_gain(input logic [3:0] code);
        begin
            // Exact rounded Cselected/C138 values in Q12; C138 is 3900 pF.
            case (code)
                4'h0: fric2_gain = 13'd0;
                4'h1: fric2_gain = 13'd284;
                4'h2: fric2_gain = 13'd557;
                4'h3: fric2_gain = 13'd840;
                4'h4: fric2_gain = 13'd1136;
                4'h5: fric2_gain = 13'd1420;
                4'h6: fric2_gain = 13'd1693;
                4'h7: fric2_gain = 13'd1977;
                4'h8: fric2_gain = 13'd2269;
                4'h9: fric2_gain = 13'd2552;
                4'hA: fric2_gain = 13'd2825;
                4'hB: fric2_gain = 13'd3109;
                4'hC: fric2_gain = 13'd3405;
                4'hD: fric2_gain = 13'd3689;
                4'hE: fric2_gain = 13'd3962;
                default: fric2_gain = 13'd4245;
            endcase
        end
    endfunction

    function automatic logic signed [16:0] filter_amp_gain(
        input logic [3:0] code
    );
        begin
            // Exact rounded Cselected/(C172+C173) values in Q14.  U145A
            // resets the feedback path in Phi0; U145B inserts 2700 pF in
            // Phi1, while the 50 pF C173 branch remains present.
            case (code)
                4'h0: filter_amp_gain = 17'sd0;
                4'h1: filter_amp_gain = 17'sd453;
                4'h2: filter_amp_gain = 17'sd894;
                4'h3: filter_amp_gain = 17'sd1346;
                4'h4: filter_amp_gain = 17'sd1787;
                4'h5: filter_amp_gain = 17'sd2240;
                4'h6: filter_amp_gain = 17'sd2681;
                4'h7: filter_amp_gain = 17'sd3134;
                4'h8: filter_amp_gain = 17'sd3575;
                4'h9: filter_amp_gain = 17'sd4027;
                4'hA: filter_amp_gain = 17'sd4468;
                4'hB: filter_amp_gain = 17'sd4921;
                4'hC: filter_amp_gain = 17'sd5362;
                4'hD: filter_amp_gain = 17'sd5815;
                4'hE: filter_amp_gain = 17'sd6256;
                default: filter_amp_gain = 17'sd6709;
            endcase
        end
    endfunction

    always_comb begin
        voiced_q = (voice_shape_q == 4'hF);
        dc_input = powered_down ? 24'sd0 : reconstruction_hold_q;
        dc_filtered_wide = dc_sum_q - (dc_sum_q >>> 8);
        stop_class = pw_2 && !pw_3;
        source_voiced = stop_class ? 1'b0 : voiced;
        source_fricative = stop_class ? 1'b0 : fricative;
        source_pw3 = pw_3;
        source_fric1_sw = fric1_sw;
        source_fric2_sw = fric2_sw;
        source_voice_amp_code = voice_amp_code;
        source_fric_amp_code = fric_amp_code;

        noise_count_next = (noise_count_q == 4'hF) ?
                           4'h0 : noise_count_q + 4'h1;
        noise_force = ~(noise_count_next[2] | noise_count_next[3]);
        noise_feedback = noise_force ^ noise_d1_q[3] ^ noise_d2_q[4] ^
                         noise_d4_q[3] ^ noise_d4_q[4];
        // U73 pin 9 is D4+5.  U163F inverts the U51D result, and U149C
        // applies the PW3/U62 gate.  Pure fricatives have VOICE_AMP_ZERO=1,
        // so the unresolved U68/AMPCT0 term cannot suppress this path.
        u72a_fric_gate = !(source_pw3 && !voice_toggle);
        noise_bit = !noise_d4_q[4] && u72a_fric_gate;
        // U41C clocks U75 and U73 without a CTL, PD, phone, or FRICATIVE
        // gate.  The core has already resolved that physical clock edge.
        noise_advance = noise_clock_ce;

        // The normalized unit amplitude is 2^16, so Q12 gain becomes
        // gain << 4.  Keep these source paths free of general multipliers.
        voice_magnitude = $signed(
            {11'd0, voice_gain(source_voice_amp_code)}
        ) <<< 4;
        // U157 and U152 are distinct switched-capacitor amplifiers.  Their
        // selected banks differ, and sheets 1/2 route them to C143 and
        // C150/C151 respectively; do not merge them into two polarities of
        // one synthetic source.
        // Use a normalized 0/2^16 HCC drive so the drawn capacitor ratios
        // and signs remain exact in fixed point.  The schematic does not set
        // the absolute U150 pulse swing or LF356/CD4016 nonideal response.
        fric_drive = 18'sd0;
        if (source_fricative && phone_active && !powered_down)
            fric_drive = noise_bit ? 18'sd65536 : 18'sd0;
        fric_drive_delta =
            $signed({fric_drive[17], fric_drive}) -
            $signed({fric_drive_history_q[17], fric_drive_history_q});
        fric1_gain_extended = $signed(
            {6'd0, fric1_gain(source_fric_amp_code)}
        );
        fric2_gain_extended = $signed(
            {6'd0, fric2_gain(source_fric_amp_code)}
        );
        case (fric_drive_delta)
            19'sd65536: begin
                fric2_drive_charge = fric2_gain_extended <<< 4;
            end
            -19'sd65536: begin
                fric2_drive_charge = -(fric2_gain_extended <<< 4);
            end
            default: begin
                fric2_drive_charge = 19'sd0;
            end
        endcase
        fric2_source_accumulator =
            {{24{fric2_source_state_q[23]}}, fric2_source_state_q} -
            {{29{fric2_drive_charge[18]}}, fric2_drive_charge};
        fric2_source_next = sat24_from48(fric2_source_accumulator);

        if (!source_voiced || !phone_active || powered_down) begin
            voice_source = 24'sd0;
        end else if (voiced_q) begin
            // Sheet 1 selects the AGND follower while VOICED is high.  This
            // is the zero source level, not the positive half of a square.
            voice_source = 24'sd0;
        end else begin
            // /VOICED selects the adjustable source for the 15-count U60
            // pulse.  Its step is one source magnitude, not a 2A bipolar step.
            voice_source = voice_magnitude;
        end

        // U156B grounds the selected input bank and U156C resets C133/U157
        // in Phi1.  U156A therefore regenerates this level from the current
        // HCC bit in every Phi0; it is not an HCC-edge accumulator.
        fric1_source = (fric_drive == 18'sd65536) ?
            -$signed({5'd0, fric1_gain_extended}) <<< 4 : 24'sd0;
        // U152 retains charge across FRIC_AMP code changes; expose the value
        // that the next Phi0 edge will commit and snapshot.
        fric2_source = fric2_source_next;

    end

    always_comb begin
        // Derive every value that consumes a registered DSP result before
        // selecting the next DSP operands.  Stage 2 uses engine_charge_next;
        // computing it later in this same procedural block would make the
        // multiplier see the prior section's value for one evaluation.
        product_a_q_ext = {{6{product_a_q[41]}}, product_a_q};
        product_b_q_ext = {{6{product_b_q[41]}}, product_b_q};

        // Stage 1 stores the two main p products without rounding.  Stage 2
        // adds the registered side product and rounds the complete p sum.
        recurrence_accumulator = product_a_q_ext + product_b_q_ext;
        charge_accumulator = recurrence_accumulator_q + product_a_q_ext;
        rounded_charge_accumulator = charge_accumulator +
            (charge_accumulator[47] ? 48'sd8191 : 48'sd8192);
        engine_charge_next = sat24_from48(
            rounded_charge_accumulator >>> 14
        );

        // Stage 3 subtracts b*p from y in Q14.  Stage 4 rounds and clamps the
        // complete output update once; no intermediate product is saturated.
        state_y_q14 =
            $signed({{24{engine_state_y_q[23]}}, engine_state_y_q}) <<< 14;
        section_accumulator = state_y_q14 - product_a_q_ext;
        // F2 adds the C143 charge directly at its second integrator.  The
        // output-side product is zero for the other four sections.  Round
        // after the complete y update so the capacitor terms share one
        // quantization point.
        complete_section_accumulator = section_accumulator_q +
            product_a_q_ext;
        rounded_section_accumulator = complete_section_accumulator +
            (complete_section_accumulator[47] ? 48'sd8191 : 48'sd8192);
        engine_section_next = sat24_from48(
            rounded_section_accumulator >>> 14
        );
        output_accumulator = product_a_q_ext + product_b_q_ext;
        rounded_output_accumulator = output_accumulator +
            (output_accumulator[47] ? 48'sd8191 : 48'sd8192);
        engine_output_next = sat24_from48(
            rounded_output_accumulator >>> 14
        );

        engine_operand_a = 25'sd0;
        engine_operand_b = 25'sd0;
        engine_coefficient_a = 17'sd0;
        engine_coefficient_b = 17'sd0;
        case (engine_stage_q)
            4'd0: begin
                // Main charge update: alpha*p + a*(y-main input).
                engine_operand_a = {
                    engine_state_p_q[23], engine_state_p_q
                };
                engine_operand_b = engine_main_delta_q;
                engine_coefficient_a = engine_alpha_q;
                engine_coefficient_b = engine_a_q;
            end
            4'd1: begin
                // F1 adds a*(x[n-1]-x[n]); F3 adds the C127 F1-history
                // term; F5 adds the always-connected C150 FRIC2-history
                // term.  Other sections load zero.
                engine_operand_a = engine_side_delta_q;
                engine_coefficient_a = engine_g_q;
            end
            4'd2: begin
                // F5 has an always-connected C150 path and a separately
                // switched C151 path.  This slot launches C151 after C150
                // has entered the accumulator.
                engine_operand_a = engine_side2_delta_q;
                engine_coefficient_a = engine_g2_q;
            end
            4'd3: begin
                // The second switched integrator consumes the registered,
                // fully rounded first-integrator charge.  Keeping the DSP in
                // this slot removes it from the wide charge-add path.
                engine_operand_a = {
                    charge_work_q[23], charge_work_q
                };
                engine_coefficient_a = engine_b_q;
            end
            4'd4: begin
                // C143 enters F2 at the second integrator, after b*p.  Its
                // current U157 level joins y before the one final round.
                engine_operand_a = engine_output_delta_q;
                engine_coefficient_a = engine_h_q;
            end
            4'd6: begin
                // C172 retains 2700/2750 of U146's prior output.  The selected
                // FL_AMP bank adds Csel/2750 of the F5 transition.  C173 is
                // the always-connected 50 pF feedback branch.
                engine_operand_a = {output_hold_q[23], output_hold_q};
                engine_coefficient_a = FILTER_OUTPUT_ALPHA_Q14;
                engine_operand_b = output_sum_q;
                engine_coefficient_b = filter_amp_gain(filter_amp_phi0_q);
            end
            default: begin
            end
        endcase
        engine_product_a = engine_operand_a * engine_coefficient_a;
        engine_product_b = engine_operand_b * engine_coefficient_b;
    end

    always_ff @(posedge clk) begin
        if (!rstn) begin
            pitch_sync1_q <= 1'b0;
            pitch_sync2_q <= 1'b0;
            voice_load_pending_q <= 1'b0;
            voice_shape_q <= 4'hF;

            noise_d1_q <= NOISE_D1_SEED;
            noise_d2_q <= NOISE_D2_SEED;
            noise_d3_q <= NOISE_D3_SEED;
            noise_d4_q <= NOISE_D4_SEED;
            noise_count_q <= NOISE_COUNT_SEED;
            fric_drive_history_q <= 18'sd0;
            fric2_source_state_q <= 24'sd0;

            f1_state_q <= 24'sd0;
            f1_history_q <= 24'sd0;
            f2_state_q <= 24'sd0;
            f2_history_q <= 24'sd0;
            f3_state_q <= 24'sd0;
            f3_history_q <= 24'sd0;
            f4_state_q <= 24'sd0;
            f4_history_q <= 24'sd0;
            f5_state_q <= 24'sd0;
            f5_history_q <= 24'sd0;
            f1_input_q <= 24'sd0;
            f2_input_q <= 24'sd0;
            f3_input_q <= 24'sd0;
            f4_input_q <= 24'sd0;
            f5_input_q <= 24'sd0;
            f1_input_history_q <= 24'sd0;
            f3_side_input_q <= 24'sd0;
            f3_side_history_q <= 24'sd0;
            fric1_source_phi0_q <= 24'sd0;
            fric2_source_phi0_q <= 24'sd0;
            fric2_base_history_q <= 24'sd0;
            fric2_sw_history_q <= 24'sd0;
            fric1_sw_phi0_q <= 1'b0;
            fric2_sw_phi0_q <= 1'b0;
            filter_amp_phi0_q <= 4'h0;
            f1_code_phi0_q <= 4'h0;
            f2_code_phi0_q <= 4'h0;
            f2_res_code_phi0_q <= 4'h0;
            f3_code_phi0_q <= 4'h0;
            f4_code_phi0_q <= 4'h0;
            output_hold_q <= 24'sd0;
            reconstruction_hold_q <= 24'sd0;
            dc_previous_input_q <= 24'sd0;
            dc_delta_q <= 25'sd0;
            dc_sum_q <= 27'sd0;
            dc_output_q <= 26'sd0;
            dc_stage1_pending_q <= 1'b0;
            dc_stage2_pending_q <= 1'b0;
            dc_active_stage1_q <= 1'b0;
            dc_active_stage2_q <= 1'b0;
            engine_busy_q <= 1'b0;
            engine_overrun_q <= 1'b0;
            engine_section_q <= 3'd0;
            engine_stage_q <= 4'd0;
            product_a_q <= 42'sd0;
            product_b_q <= 42'sd0;
            recurrence_accumulator_q <= 48'sd0;
            section_accumulator_q <= 48'sd0;
            charge_work_q <= 24'sd0;
            engine_state_y_q <= 24'sd0;
            engine_state_p_q <= 24'sd0;
            engine_main_delta_q <= 25'sd0;
            engine_alpha_q <= 17'sd0;
            engine_a_q <= 17'sd0;
            engine_b_q <= 17'sd0;
            engine_side_delta_q <= 25'sd0;
            engine_g_q <= 17'sd0;
            engine_side2_delta_q <= 25'sd0;
            engine_g2_q <= 17'sd0;
            engine_output_delta_q <= 25'sd0;
            engine_h_q <= 17'sd0;
            output_sum_q <= 25'sd0;
            output_old_state_q <= 24'sd0;
            audio_sample <= 16'sd0;
        end else begin
            // U61 samples the core's held U62 Q on Phi0_X; U34A recognizes
            // its rising transition only.  These counters keep running while
            // the source is muted.  VOICED/phone/PD gate only the analog feed.
            if (filter_phase_ce && !filter_phase) begin
                pitch_sync1_q <= voice_toggle;
                pitch_sync2_q <= pitch_sync1_q;
                if (pitch_sync1_q && !pitch_sync2_q)
                    voice_load_pending_q <= 1'b1;
            end

            // U60 clocks on the opposite Phi0_X edge, loads zero, counts to
            // 15, and then holds through its terminal-count feedback.
            if (filter_phase_ce && filter_phase) begin
                if (voice_load_pending_q) begin
                    voice_shape_q <= 4'h0;
                    voice_load_pending_q <= 1'b0;
                end else if (voice_shape_q != 4'hF) begin
                    voice_shape_q <= voice_shape_q + 4'h1;
                end
            end

            // U75 advances on the rising edge. U73 shifts on the following
            // falling edge, so the forcing term uses the new U75 count while
            // every HCC4006 feedback tap uses the old register state.
            if (noise_advance) begin
                noise_count_q <= noise_count_next;
                noise_d1_q <= {noise_d1_q[2:0], noise_d3_q[3]};
                noise_d2_q <= {noise_d2_q[3:0], noise_d4_q[4]};
                noise_d3_q <= {noise_d3_q[2:0], noise_d2_q[4]};
                noise_d4_q <= {noise_d4_q[3:0], noise_feedback};
            end

            // U152 has no phase reset.  Update its charge state whenever the
            // shared HCC drive changes, using the FRIC_AMP code held at that
            // edge.  A code change at a steady drive creates no false level
            // jump; the next real source edge uses the new capacitor bank.
            if (fric_drive != fric_drive_history_q) begin
                fric2_source_state_q <= fric2_source_next;
                fric_drive_history_q <= fric_drive;
            end

            // U145D closes at the Phi1-to-Phi0 boundary.  It copies the U146
            // result completed during the prior Phi1 into C100/U148.  Low
            // holds the prior sample; CLOSURE never discharges either node.
            if (closure)
                reconstruction_hold_q <= output_hold_q;

            if (filter_phase_ce) begin
                if (engine_busy_q)
                    engine_overrun_q <= 1'b1;

                if (!filter_phase) begin
                    // Phi0 holds one input sample per physical section.  The
                    // states then stay fixed while the MAC scheduler works.
                    f1_input_q <= voice_source;
                    f2_input_q <= f1_state_q;
                    // C127 carries F1 into the first F3 integrator.  Keep
                    // this sample separate from the serial F2 input so
                    // its one-pair difference term uses one coherent Phi0.
                    f3_side_input_q <= f1_state_q;
                    f3_input_q <= f2_state_q;
                    fric1_source_phi0_q <= fric1_source;
                    fric2_source_phi0_q <= fric2_source_next;
                    fric1_sw_phi0_q <= source_fric1_sw;
                    f4_input_q <= f3_state_q;
                    f5_input_q <= f4_state_q;
                    fric2_sw_phi0_q <= source_fric2_sw;
                    filter_amp_phi0_q <= filter_amp_code;
                    f1_code_phi0_q <= f1_code;
                    f2_code_phi0_q <= f2_code;
                    f2_res_code_phi0_q <= f2_res_code;
                    f3_code_phi0_q <= f3_code;
                    f4_code_phi0_q <= f4_code;
                end

                if (filter_phase && !engine_busy_q) begin
                    engine_busy_q <= 1'b1;
                    engine_section_q <= 3'd0;
                    engine_stage_q <= 4'd0;
                    // Load all F1 operands before stage 0.  No section select,
                    // subtraction, or coefficient table remains at a DSP pin.
                    engine_state_y_q <= f1_state_q;
                    engine_state_p_q <= f1_history_q;
                    engine_main_delta_q <=
                        $signed({f1_state_q[23], f1_state_q}) -
                        $signed({f1_input_q[23], f1_input_q});
                    engine_alpha_q <= F1_ALPHA_Q14;
                    engine_a_q <= F1_A_Q14;
                    engine_b_q <= f1_b_q14(f1_code_phi0_q);
                    engine_side_delta_q <=
                        $signed({f1_input_history_q[23],
                                 f1_input_history_q}) -
                        $signed({f1_input_q[23], f1_input_q});
                    engine_g_q <= F1_G_Q14;
                    engine_side2_delta_q <= 25'sd0;
                    engine_g2_q <= 17'sd0;
                    engine_output_delta_q <= 25'sd0;
                    engine_h_q <= 17'sd0;
                end
            end

            // Every DSP result first enters product_a_q/product_b_q.  Adds,
            // saturation, and state commits occur only on later clocks.  One
            // section takes six clocks.  F5's separate C150/C151 terms and
            // registered U146 delta take eight; the engine takes 34 clocks.
            // The fastest intended phase gap is 133.
            if (engine_busy_q) begin
                case (engine_stage_q)
                    4'd0: begin
                        // alpha*p and a*(y-main input)
                        product_a_q <= engine_product_a;
                        product_b_q <= engine_product_b;
                        engine_stage_q <= 4'd1;
                    end
                    4'd1: begin
                        // Save both main products and launch the optional
                        // F1/F3 side-history product.
                        recurrence_accumulator_q <= recurrence_accumulator;
                        product_a_q <= engine_product_a;
                        engine_stage_q <= 4'd2;
                    end
                    4'd2: begin
                        if (engine_section_q == 3'd4) begin
                            // F5 first accumulates always-connected C150,
                            // then launches separately switched C151.  Keep
                            // both products wide until stage 8 rounds once.
                            recurrence_accumulator_q <= charge_accumulator;
                            product_a_q <= engine_product_a;
                            engine_stage_q <= 4'd8;
                        end else begin
                            // Round and register the complete p update.  The
                            // next slot launches b*p from this short path.
                            charge_work_q <= engine_charge_next;
                            engine_stage_q <= 4'd3;
                        end
                    end
                    4'd8: begin
                        charge_work_q <= engine_charge_next;
                        engine_stage_q <= 4'd3;
                    end
                    4'd3: begin
                        // Register b*p before the wide y subtraction.
                        product_a_q <= engine_product_a;
                        engine_stage_q <= 4'd4;
                    end
                    4'd4: begin
                        // Keep y-b*p wide and launch the optional C143 term.
                        section_accumulator_q <= section_accumulator;
                        product_a_q <= engine_product_a;
                        engine_stage_q <= 4'd5;
                    end
                    4'd5: begin
                        case (engine_section_q)
                            3'd0: begin
                                f1_state_q <= engine_section_next;
                                f1_history_q <= charge_work_q;
                                f1_input_history_q <= f1_input_q;
                                // Hand off the old F2 state and Phi0 input.
                                engine_state_y_q <= f2_state_q;
                                engine_state_p_q <= f2_history_q;
                                engine_main_delta_q <=
                                    $signed({f2_state_q[23], f2_state_q}) -
                                    $signed({f2_input_q[23], f2_input_q});
                                engine_alpha_q <=
                                    f2_alpha_q14(f2_res_code_phi0_q);
                                engine_a_q <= f2_a_q14(f2_res_code_phi0_q);
                                engine_b_q <= f2_b_q14(f2_code_phi0_q);
                                engine_side_delta_q <= 25'sd0;
                                engine_g_q <= 17'sd0;
                                engine_side2_delta_q <= 25'sd0;
                                engine_g2_q <= 17'sd0;
                                engine_output_delta_q <=
                                    fric1_sw_phi0_q ?
                                    -$signed({fric1_source_phi0_q[23],
                                              fric1_source_phi0_q}) :
                                    25'sd0;
                                engine_h_q <= fric1_sw_phi0_q ?
                                    F2_FRIC_H_Q14 : 17'sd0;
                            end
                            3'd1: begin
                                f2_state_q <= engine_section_next;
                                f2_history_q <= charge_work_q;
                                // Hand off F3, including the drawn C127 path.
                                engine_state_y_q <= f3_state_q;
                                engine_state_p_q <= f3_history_q;
                                engine_main_delta_q <=
                                    $signed({f3_state_q[23], f3_state_q}) -
                                    $signed({f3_input_q[23], f3_input_q});
                                engine_alpha_q <= F3_ALPHA_Q14;
                                engine_a_q <= F3_A_Q14;
                                engine_b_q <= f3_b_q14(f3_code_phi0_q);
                                engine_side_delta_q <=
                                    $signed({f3_side_history_q[23],
                                             f3_side_history_q}) -
                                    $signed({f3_side_input_q[23],
                                             f3_side_input_q});
                                engine_g_q <= F3_G_Q14;
                                engine_side2_delta_q <= 25'sd0;
                                engine_g2_q <= 17'sd0;
                                engine_output_delta_q <= 25'sd0;
                                engine_h_q <= 17'sd0;
                            end
                            3'd2: begin
                                f3_state_q <= engine_section_next;
                                f3_history_q <= charge_work_q;
                                f3_side_history_q <= f3_side_input_q;
                                // Hand off the old F4 state and Phi0 input.
                                engine_state_y_q <= f4_state_q;
                                engine_state_p_q <= f4_history_q;
                                engine_main_delta_q <=
                                    $signed({f4_state_q[23], f4_state_q}) -
                                    $signed({f4_input_q[23], f4_input_q});
                                engine_alpha_q <= F4_ALPHA_Q14;
                                engine_a_q <= F4_A_Q14;
                                engine_b_q <= f4_b_q14(f4_code_phi0_q);
                                engine_side_delta_q <= 25'sd0;
                                engine_g_q <= 17'sd0;
                                engine_side2_delta_q <= 25'sd0;
                                engine_g2_q <= 17'sd0;
                                engine_output_delta_q <= 25'sd0;
                                engine_h_q <= 17'sd0;
                            end
                            3'd3: begin
                                f4_state_q <= engine_section_next;
                                f4_history_q <= charge_work_q;
                                // Hand off the fixed F5 state and Phi0 input.
                                engine_state_y_q <= f5_state_q;
                                engine_state_p_q <= f5_history_q;
                                engine_main_delta_q <=
                                    $signed({f5_state_q[23], f5_state_q}) -
                                    $signed({f5_input_q[23], f5_input_q});
                                engine_alpha_q <= F5_ALPHA_Q14;
                                engine_a_q <= F5_A_Q14;
                                engine_b_q <= F5_B_Q14;
                                // C150 is always connected to U152.  C151
                                // has its own charge history and contributes
                                // only while U159C is closed.
                                engine_side_delta_q <=
                                    $signed({fric2_base_history_q[23],
                                             fric2_base_history_q}) -
                                    $signed({fric2_source_phi0_q[23],
                                             fric2_source_phi0_q});
                                engine_g_q <= F5_FRIC_BASE_G_Q14;
                                engine_side2_delta_q <=
                                    fric2_sw_phi0_q ?
                                    $signed({fric2_sw_history_q[23],
                                             fric2_sw_history_q}) -
                                    $signed({fric2_source_phi0_q[23],
                                             fric2_source_phi0_q}) :
                                    25'sd0;
                                engine_g2_q <= fric2_sw_phi0_q ?
                                    F5_FRIC_SW_G_Q14 : 17'sd0;
                                engine_output_delta_q <= 25'sd0;
                                engine_h_q <= 17'sd0;
                            end
                            default: begin
                                f5_state_q <= engine_section_next;
                                f5_history_q <= charge_work_q;
                                fric2_base_history_q <=
                                    fric2_source_phi0_q;
                                if (fric2_sw_phi0_q)
                                    fric2_sw_history_q <=
                                        fric2_source_phi0_q;
                            end
                        endcase

                        if (engine_section_q == 3'd4) begin
                            // Save old F5 beside the committed new state.  A
                            // short next slot forms the U146 delta without
                            // extending the saturated section-result path.
                            output_old_state_q <= engine_state_y_q;
                            engine_stage_q <= 4'd9;
                        end
                        else begin
                            engine_section_q <= engine_section_q + 3'd1;
                            engine_stage_q <= 4'd0;
                        end
                    end
                    4'd9: begin
                        // U146 sees old F5 minus new F5 through the selected
                        // FL_AMP bank, not the held F5 level.
                        output_sum_q <=
                            $signed({output_old_state_q[23],
                                     output_old_state_q}) -
                            $signed({f5_state_q[23], f5_state_q});
                        engine_stage_q <= 4'd6;
                    end
                    4'd6: begin
                        product_a_q <= engine_product_a;
                        product_b_q <= engine_product_b;
                        engine_stage_q <= 4'd7;
                    end
                    default: begin
                        // U146 completes during Phi1.  C100/U148 will copy
                        // this internal hold only on the next CLOSURE pulse.
                        output_hold_q <= engine_output_next;
                        engine_busy_q <= 1'b0;
                        engine_stage_q <= 4'd0;
                    end
                endcase
            end

            // C381 AC-couples the reconstructed output.  Spread its one-pole
            // DC blocker over two fabric clocks so it cannot add a long carry
            // chain to the tract scheduler.  At 48 kHz, 255/256 gives a pole
            // near 30 Hz as a boundary model; the external load, which sets
            // the real C381 pole, is not drawn.  Keep x-x[n-1] and y+delta
            // wide, apply the leak,
            // then clamp once; early 24-bit clamps distort large reversals.
            // Phone end does not mute U148, so the formant and output holds
            // may ring down.  Powerdown alone forces the boundary input low.
            if (audio_tick) begin
                dc_delta_q <= {dc_input[23], dc_input} -
                              {dc_previous_input_q[23],
                               dc_previous_input_q};
                dc_previous_input_q <= dc_input;
                dc_active_stage1_q <= !powered_down;
                dc_stage1_pending_q <= 1'b1;
            end

            if (dc_stage1_pending_q) begin
                dc_sum_q <= {{1{dc_output_q[25]}}, dc_output_q} +
                            {{2{dc_delta_q[24]}}, dc_delta_q};
                dc_active_stage2_q <= dc_active_stage1_q;
                dc_stage1_pending_q <= 1'b0;
                dc_stage2_pending_q <= 1'b1;
            end

            if (dc_stage2_pending_q) begin
                dc_output_q <= sat26_from27(dc_filtered_wide);
                if (dc_active_stage2_q)
                    audio_sample <= sat16_from27(
                        dc_filtered_wide >>> LINE_OUTPUT_SHIFT
                    );
                else
                    audio_sample <= 16'sd0;
                dc_stage2_pending_q <= 1'b0;
            end
        end
    end

endmodule
