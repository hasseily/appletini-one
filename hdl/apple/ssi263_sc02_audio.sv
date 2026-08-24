`timescale 1ns / 1ps

// Native SSI-263A / SC-02 excitation and switched-capacitor audio checkpoint.
//
// This block deliberately has no SC-01 phone map or coefficient tables.  It
// consumes the persistent controls and clock enables produced by
// ssi263_sc02_core.  The pitch shaper and HCC4006 recurrence follow sheets 3
// and 6 structurally.  Each analog section is a stable, phase-held, two-pole
// low-pass whose angle follows the source capacitor totals; F2 resonance
// controls its pole radius.  The unity-DC numerator models the output of the
// second switched integrator instead of the first-integrator tap used by the
// old complex-rotation checkpoint.
// The tract follows sheets 1 and 2 exactly at section level:
//
//   VOICED -> F1 -> F2 -> (+FRIC_1) -> F3 -> F4 -> (+FRIC_2) -> F5
//
// FRIC_1 and FRIC_2 are injection nodes, not independent output resonators.
// This gives usable formant peaks without borrowing SC-01 tables, but it is
// not yet the final phase-by-phase nodal solution of the LF356/CD4016 network.
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

    localparam logic [14:0] F1_RADIUS_Q14 = 15'd15401;   // 0.940
    localparam logic [14:0] F3_RADIUS_Q14 = 15'd15237;   // 0.930
    localparam logic [14:0] F4_RADIUS_Q14 = 15'd15073;   // 0.920
    localparam logic [14:0] F5_RADIUS_Q14 = 15'd14746;   // 0.900
    localparam logic signed [15:0] F5_COS_Q14 = 16'sd1159;
    // Sheets 1 and 2 couple FRIC_1 through 1000 pF and FRIC_2 through
    // 2700+1000 pF.  These calibrated Q14 gains preserve that 3.7:1 ratio
    // while an all-phone sweep sets the absolute scale below clipping.  The
    // final two-phase nodal model can replace the absolute calibration.
    localparam integer FRIC1_COUPLING_Q14 = 208;
    localparam integer FRIC2_COUPLING_Q14 = 768;
    // Leave 12 dB of line-output headroom for the largest ROM/formant pair and
    // for the card's later PSG/speech mix.  Internal tract state remains full
    // precision; an exhaustive all-phone model found no rail hits at 1/4.
    localparam integer LINE_OUTPUT_SHIFT = 2;

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
    logic signed [23:0] fric_source;
    logic signed [23:0] fric1_injection;
    logic signed [23:0] fric2_injection;
    logic signed [23:0] voice_magnitude;
    logic signed [23:0] fric_positive_magnitude;
    logic signed [23:0] fric_negative_magnitude;

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
    logic [7:0] filter_frequency_q;

    // Two registered RTL product lanes form a 32-cycle scheduler.  Each
    // section first derives a1=2*r*cos(theta), r2=r*r, and
    // b0=1-a1+r2, then evaluates y=a1*y1-r2*y2+b0*x.  The normalized
    // numerator removes the unwanted zero from the old complex-rotation tap.
    // The default XCK/DIV2 profile leaves 149 fabric clocks between the
    // fastest filter phases, with margin for two independent instances.
    logic engine_busy_q;
    logic engine_overrun_q;
    logic [2:0] engine_section_q;
    logic [3:0] engine_stage_q;
    logic signed [23:0] engine_state_y1;
    logic signed [23:0] engine_state_y2;
    logic signed [23:0] engine_input;
    logic signed [15:0] engine_cosine;
    logic [14:0] engine_radius;
    logic signed [23:0] engine_operand_a;
    logic signed [23:0] engine_operand_b;
    logic signed [16:0] engine_coefficient_a;
    logic signed [16:0] engine_coefficient_b;
    logic signed [40:0] engine_product_a;
    logic signed [40:0] engine_product_b;
    logic signed [40:0] product_a_q;
    logic signed [40:0] product_b_q;
    logic signed [16:0] pole_a1_q;
    logic signed [16:0] pole_r2_q;
    logic signed [16:0] pole_b0_q;
    logic signed [16:0] pole_a1_next;
    logic signed [16:0] pole_r2_next;
    logic signed [17:0] pole_b0_wide;
    logic signed [23:0] output_sum_q;
    logic signed [23:0] engine_section_next;
    logic signed [23:0] engine_output_next;
    logic signed [47:0] product_a_q_ext;
    logic signed [47:0] product_b_q_ext;
    logic signed [47:0] recurrence_accumulator;
    logic signed [47:0] recurrence_accumulator_q;
    logic signed [47:0] section_accumulator_q;
    logic signed [47:0] rounded_section_accumulator;

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

    function automatic logic signed [23:0] sat24_add(
        input logic signed [23:0] a,
        input logic signed [23:0] b
    );
        logic signed [24:0] sum;
        begin
            sum = {a[23], a} + {b[23], b};
            if (sum > 25'sd8388607)
                sat24_add = 24'sh7FFFFF;
            else if (sum < -25'sd8388608)
                sat24_add = -24'sd8388608;
            else
                sat24_add = sum[23:0];
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
    // remains exact; mapping those totals to stable resonator poles is the
    // fixed-point approximation documented at the top of this block.
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
            if (code[3]) total = total + 2300;
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
            // The code-0 branch is two 200 pF banks in series: 100 pF.
            if (code[0]) total = total + 100;
            if (code[1]) total = total + 400;
            if (code[2]) total = total + 820;
            if (code[3]) total = total + 1620;
            f4_capacitance = total[12:0];
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

    function automatic logic [12:0] fric_pos_capacitance(input logic [3:0] code);
        integer total;
        begin
            total = 0;
            if (code[0]) total = total + 270;
            if (code[1]) total = total + 512;
            if (code[2]) total = total + 1068;
            if (code[3]) total = total + 2160;
            fric_pos_capacitance = total[12:0];
        end
    endfunction

    function automatic logic [12:0] fric_neg_capacitance(input logic [3:0] code);
        integer total;
        begin
            total = 0;
            if (code[0]) total = total + 270;
            if (code[1]) total = total + 530;
            if (code[2]) total = total + 1082;
            if (code[3]) total = total + 2160;
            fric_neg_capacitance = total[12:0];
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

    // Q2.14 sine/cosine pairs.  Angles increase with the exact capacitor
    // totals above, so changing a code moves the section's normalized center.
    // The live FILT divider changes the rate of every complete Phi0/Phi1 pair,
    // scaling every center in wall-clock Hz without changing these ratios.
    function automatic logic signed [15:0] f1_cos_q14(input logic [3:0] code);
        begin
            case (code)
                4'h0: f1_cos_q14 = 16'sd16355;
                4'h1: f1_cos_q14 = 16'sd16337;
                4'h2: f1_cos_q14 = 16'sd16314;
                4'h3: f1_cos_q14 = 16'sd16289;
                4'h4: f1_cos_q14 = 16'sd16257;
                4'h5: f1_cos_q14 = 16'sd16223;
                4'h6: f1_cos_q14 = 16'sd16183;
                4'h7: f1_cos_q14 = 16'sd16140;
                4'h8: f1_cos_q14 = 16'sd16097;
                4'h9: f1_cos_q14 = 16'sd16048;
                4'hA: f1_cos_q14 = 16'sd15990;
                4'hB: f1_cos_q14 = 16'sd15932;
                4'hC: f1_cos_q14 = 16'sd15867;
                4'hD: f1_cos_q14 = 16'sd15801;
                4'hE: f1_cos_q14 = 16'sd15726;
                default: f1_cos_q14 = 16'sd15652;
            endcase
        end
    endfunction

    function automatic logic signed [15:0] f1_sin_q14(input logic [3:0] code);
        begin
            case (code)
                4'h0: f1_sin_q14 = 16'sd982;
                4'h1: f1_sin_q14 = 16'sd1239;
                4'h2: f1_sin_q14 = 16'sd1511;
                4'h3: f1_sin_q14 = 16'sd1766;
                4'h4: f1_sin_q14 = 16'sd2037;
                4'h5: f1_sin_q14 = 16'sd2292;
                4'h6: f1_sin_q14 = 16'sd2561;
                4'h7: f1_sin_q14 = 16'sd2815;
                4'h8: f1_sin_q14 = 16'sd3052;
                4'h9: f1_sin_q14 = 16'sd3303;
                4'hA: f1_sin_q14 = 16'sd3570;
                4'hB: f1_sin_q14 = 16'sd3820;
                4'hC: f1_sin_q14 = 16'sd4085;
                4'hD: f1_sin_q14 = 16'sd4333;
                4'hE: f1_sin_q14 = 16'sd4596;
                default: f1_sin_q14 = 16'sd4842;
            endcase
        end
    endfunction

    function automatic logic signed [15:0] f2_cos_q14(input logic [3:0] code);
        begin
            case (code)
                4'h0: f2_cos_q14 = 16'sd16119;
                4'h1: f2_cos_q14 = 16'sd15973;
                4'h2: f2_cos_q14 = 16'sd15796;
                4'h3: f2_cos_q14 = 16'sd15588;
                4'h4: f2_cos_q14 = 16'sd15349;
                4'h5: f2_cos_q14 = 16'sd15079;
                4'h6: f2_cos_q14 = 16'sd14781;
                4'h7: f2_cos_q14 = 16'sd14453;
                4'h8: f2_cos_q14 = 16'sd14016;
                4'h9: f2_cos_q14 = 16'sd13626;
                4'hA: f2_cos_q14 = 16'sd13209;
                4'hB: f2_cos_q14 = 16'sd12767;
                4'hC: f2_cos_q14 = 16'sd12299;
                4'hD: f2_cos_q14 = 16'sd11807;
                4'hE: f2_cos_q14 = 16'sd11292;
                default: f2_cos_q14 = 16'sd10754;
            endcase
        end
    endfunction

    function automatic logic signed [15:0] f2_sin_q14(input logic [3:0] code);
        begin
            case (code)
                4'h0: f2_sin_q14 = 16'sd2933;
                4'h1: f2_sin_q14 = 16'sd3645;
                4'h2: f2_sin_q14 = 16'sd4350;
                4'h3: f2_sin_q14 = 16'sd5046;
                4'h4: f2_sin_q14 = 16'sd5732;
                4'h5: f2_sin_q14 = 16'sd6407;
                4'h6: f2_sin_q14 = 16'sd7069;
                4'h7: f2_sin_q14 = 16'sd7717;
                4'h8: f2_sin_q14 = 16'sd8484;
                4'h9: f2_sin_q14 = 16'sd9097;
                4'hA: f2_sin_q14 = 16'sd9693;
                4'hB: f2_sin_q14 = 16'sd10269;
                4'hC: f2_sin_q14 = 16'sd10825;
                4'hD: f2_sin_q14 = 16'sd11359;
                4'hE: f2_sin_q14 = 16'sd11872;
                default: f2_sin_q14 = 16'sd12361;
            endcase
        end
    endfunction

    function automatic logic signed [15:0] f3_cos_q14(input logic [3:0] code);
        begin
            case (code)
                4'h0: f3_cos_q14 = 16'sd14753;
                4'h1: f3_cos_q14 = 16'sd14397;
                4'h2: f3_cos_q14 = 16'sd14009;
                4'h3: f3_cos_q14 = 16'sd13589;
                4'h4: f3_cos_q14 = 16'sd13183;
                4'h5: f3_cos_q14 = 16'sd12705;
                4'h6: f3_cos_q14 = 16'sd12199;
                4'h7: f3_cos_q14 = 16'sd11665;
                4'h8: f3_cos_q14 = 16'sd11159;
                4'h9: f3_cos_q14 = 16'sd10576;
                4'hA: f3_cos_q14 = 16'sd9969;
                4'hB: f3_cos_q14 = 16'sd9340;
                4'hC: f3_cos_q14 = 16'sd8752;
                4'hD: f3_cos_q14 = 16'sd8083;
                4'hE: f3_cos_q14 = 16'sd7396;
                default: f3_cos_q14 = 16'sd6693;
            endcase
        end
    endfunction

    function automatic logic signed [15:0] f3_sin_q14(input logic [3:0] code);
        begin
            case (code)
                4'h0: f3_sin_q14 = 16'sd7126;
                4'h1: f3_sin_q14 = 16'sd7820;
                4'h2: f3_sin_q14 = 16'sd8496;
                4'h3: f3_sin_q14 = 16'sd9152;
                4'h4: f3_sin_q14 = 16'sd9729;
                4'h5: f3_sin_q14 = 16'sd10345;
                4'h6: f3_sin_q14 = 16'sd10937;
                4'h7: f3_sin_q14 = 16'sd11505;
                4'h8: f3_sin_q14 = 16'sd11996;
                4'h9: f3_sin_q14 = 16'sd12513;
                4'hA: f3_sin_q14 = 16'sd13002;
                4'hB: f3_sin_q14 = 16'sd13461;
                4'hC: f3_sin_q14 = 16'sd13851;
                4'hD: f3_sin_q14 = 16'sd14251;
                4'hE: f3_sin_q14 = 16'sd14620;
                default: f3_sin_q14 = 16'sd14955;
            endcase
        end
    endfunction

    function automatic logic signed [15:0] f4_cos_q14(input logic [3:0] code);
        begin
            case (code)
                4'h0: f4_cos_q14 = 16'sd11988;
                4'h1: f4_cos_q14 = 16'sd11719;
                4'h2: f4_cos_q14 = 16'sd10872;
                4'h3: f4_cos_q14 = 16'sd10577;
                4'h4: f4_cos_q14 = 16'sd9594;
                4'h5: f4_cos_q14 = 16'sd9275;
                4'h6: f4_cos_q14 = 16'sd8287;
                4'h7: f4_cos_q14 = 16'sd7948;
                4'h8: f4_cos_q14 = 16'sd6906;
                4'h9: f4_cos_q14 = 16'sd6550;
                4'hA: f4_cos_q14 = 16'sd5461;
                4'hB: f4_cos_q14 = 16'sd5092;
                4'hC: f4_cos_q14 = 16'sd3892;
                4'hD: f4_cos_q14 = 16'sd3512;
                4'hE: f4_cos_q14 = 16'sd2361;
                default: f4_cos_q14 = 16'sd1974;
            endcase
        end
    endfunction

    function automatic logic signed [15:0] f4_sin_q14(input logic [3:0] code);
        begin
            case (code)
                4'h0: f4_sin_q14 = 16'sd11168;
                4'h1: f4_sin_q14 = 16'sd11450;
                4'h2: f4_sin_q14 = 16'sd12257;
                4'h3: f4_sin_q14 = 16'sd12513;
                4'h4: f4_sin_q14 = 16'sd13281;
                4'h5: f4_sin_q14 = 16'sd13506;
                4'h6: f4_sin_q14 = 16'sd14134;
                4'h7: f4_sin_q14 = 16'sd14327;
                4'h8: f4_sin_q14 = 16'sd14858;
                4'h9: f4_sin_q14 = 16'sd15018;
                4'hA: f4_sin_q14 = 16'sd15447;
                4'hB: f4_sin_q14 = 16'sd15573;
                4'hC: f4_sin_q14 = 16'sd15915;
                4'hD: f4_sin_q14 = 16'sd16003;
                4'hE: f4_sin_q14 = 16'sd16213;
                default: f4_sin_q14 = 16'sd16265;
            endcase
        end
    endfunction

    function automatic logic [14:0] f2_radius_q14(input logic [3:0] code);
        begin
            case (code)
                4'h0: f2_radius_q14 = 15'd14418;
                4'h1: f2_radius_q14 = 15'd14512;
                4'h2: f2_radius_q14 = 15'd14602;
                4'h3: f2_radius_q14 = 15'd14697;
                4'h4: f2_radius_q14 = 15'd14791;
                4'h5: f2_radius_q14 = 15'd14885;
                4'h6: f2_radius_q14 = 15'd14975;
                4'h7: f2_radius_q14 = 15'd15070;
                4'h8: f2_radius_q14 = 15'd15404;
                4'h9: f2_radius_q14 = 15'd15499;
                4'hA: f2_radius_q14 = 15'd15589;
                4'hB: f2_radius_q14 = 15'd15683;
                4'hC: f2_radius_q14 = 15'd15778;
                4'hD: f2_radius_q14 = 15'd15872;
                4'hE: f2_radius_q14 = 15'd15962;
                default: f2_radius_q14 = 15'd16056;
            endcase
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

    function automatic logic [12:0] fric_pos_gain(input logic [3:0] code);
        begin
            // Exact rounded Cselected/C133 values in Q12; C133 is 3900 pF.
            case (code)
                4'h0: fric_pos_gain = 13'd0;
                4'h1: fric_pos_gain = 13'd284;
                4'h2: fric_pos_gain = 13'd538;
                4'h3: fric_pos_gain = 13'd821;
                4'h4: fric_pos_gain = 13'd1122;
                4'h5: fric_pos_gain = 13'd1405;
                4'h6: fric_pos_gain = 13'd1659;
                4'h7: fric_pos_gain = 13'd1943;
                4'h8: fric_pos_gain = 13'd2269;
                4'h9: fric_pos_gain = 13'd2552;
                4'hA: fric_pos_gain = 13'd2806;
                4'hB: fric_pos_gain = 13'd3090;
                4'hC: fric_pos_gain = 13'd3390;
                4'hD: fric_pos_gain = 13'd3674;
                4'hE: fric_pos_gain = 13'd3928;
                default: fric_pos_gain = 13'd4212;
            endcase
        end
    endfunction

    function automatic logic [12:0] fric_neg_gain(input logic [3:0] code);
        begin
            // Exact rounded Cselected/C138 values in Q12; C138 is 3900 pF.
            case (code)
                4'h0: fric_neg_gain = 13'd0;
                4'h1: fric_neg_gain = 13'd284;
                4'h2: fric_neg_gain = 13'd557;
                4'h3: fric_neg_gain = 13'd840;
                4'h4: fric_neg_gain = 13'd1136;
                4'h5: fric_neg_gain = 13'd1420;
                4'h6: fric_neg_gain = 13'd1693;
                4'h7: fric_neg_gain = 13'd1977;
                4'h8: fric_neg_gain = 13'd2269;
                4'h9: fric_neg_gain = 13'd2552;
                4'hA: fric_neg_gain = 13'd2825;
                4'hB: fric_neg_gain = 13'd3109;
                4'hC: fric_neg_gain = 13'd3405;
                4'hD: fric_neg_gain = 13'd3689;
                4'hE: fric_neg_gain = 13'd3962;
                default: fric_neg_gain = 13'd4245;
            endcase
        end
    endfunction

    function automatic logic [12:0] filter_amp_gain(input logic [3:0] code);
        begin
            // Normalize the four controlled branches to their 1126 pF sum.
            // C172/C173 and the final nodal solution will set the final gain.
            case (code)
                4'h0: filter_amp_gain = 13'd0;
                4'h1: filter_amp_gain = 13'd276;
                4'h2: filter_amp_gain = 13'd546;
                4'h3: filter_amp_gain = 13'd822;
                4'h4: filter_amp_gain = 13'd1091;
                4'h5: filter_amp_gain = 13'd1368;
                4'h6: filter_amp_gain = 13'd1637;
                4'h7: filter_amp_gain = 13'd1913;
                4'h8: filter_amp_gain = 13'd2183;
                4'h9: filter_amp_gain = 13'd2459;
                4'hA: filter_amp_gain = 13'd2728;
                4'hB: filter_amp_gain = 13'd3005;
                4'hC: filter_amp_gain = 13'd3274;
                4'hD: filter_amp_gain = 13'd3550;
                4'hE: filter_amp_gain = 13'd3820;
                default: filter_amp_gain = 13'd4096;
            endcase
        end
    endfunction

    always_comb begin
        voiced_q = (voice_shape_q == 4'hF);
        dc_input = (phone_active && !powered_down) ?
                   reconstruction_hold_q : 24'sd0;
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

        // Unit amplitude is exactly 2^16, so Q12 gain becomes gain << 4.
        // Keep these source paths free of general multipliers.
        voice_magnitude = $signed(
            {11'd0, voice_gain(source_voice_amp_code)}
        ) <<< 4;
        fric_positive_magnitude = $signed(
            {11'd0, fric_pos_gain(source_fric_amp_code)}
        ) <<< 4;
        fric_negative_magnitude = $signed(
            {11'd0, fric_neg_gain(source_fric_amp_code)}
        ) <<< 4;

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

        if (!source_fricative || !phone_active || powered_down) begin
            fric_source = 24'sd0;
        end else if (noise_bit) begin
            fric_source = fric_positive_magnitude;
        end else begin
            fric_source = -fric_negative_magnitude;
        end

        // Exact shift/add forms for Q14 208 and 768 avoid two more general
        // multipliers: 208=128+64+16 and 768=512+256.
        fric1_injection = sat24_add(
            sat24_add(fric_source >>> 7, fric_source >>> 8),
            fric_source >>> 10
        );
        fric2_injection = sat24_add(
            fric_source >>> 5,
            fric_source >>> 6
        );
    end

    always_comb begin
        engine_state_y1 = 24'sd0;
        engine_state_y2 = 24'sd0;
        engine_input = 24'sd0;
        engine_cosine = 16'sd16384;
        engine_radius = 15'd0;

        case (engine_section_q)
            3'd0: begin
                engine_state_y1 = f1_state_q;
                engine_state_y2 = f1_history_q;
                engine_input = f1_input_q;
                engine_cosine = f1_cos_q14(f1_code);
                engine_radius = F1_RADIUS_Q14;
            end
            3'd1: begin
                engine_state_y1 = f2_state_q;
                engine_state_y2 = f2_history_q;
                engine_input = f2_input_q;
                engine_cosine = f2_cos_q14(f2_code);
                engine_radius = f2_radius_q14(f2_res_code);
            end
            3'd2: begin
                engine_state_y1 = f3_state_q;
                engine_state_y2 = f3_history_q;
                engine_input = f3_input_q;
                engine_cosine = f3_cos_q14(f3_code);
                engine_radius = F3_RADIUS_Q14;
            end
            3'd3: begin
                engine_state_y1 = f4_state_q;
                engine_state_y2 = f4_history_q;
                engine_input = f4_input_q;
                engine_cosine = f4_cos_q14(f4_code);
                engine_radius = F4_RADIUS_Q14;
            end
            default: begin
                engine_state_y1 = f5_state_q;
                engine_state_y2 = f5_history_q;
                engine_input = f5_input_q;
                engine_cosine = F5_COS_Q14;
                engine_radius = F5_RADIUS_Q14;
            end
        endcase

        engine_operand_a = 24'sd0;
        engine_operand_b = 24'sd0;
        engine_coefficient_a = 17'sd0;
        engine_coefficient_b = 17'sd0;
        case (engine_stage_q)
            4'd0: begin
                // Coefficient setup: r*cos(theta) and r*r are Q28.
                engine_operand_a = {
                    {8{engine_cosine[15]}}, engine_cosine
                };
                engine_operand_b = $signed({9'd0, engine_radius});
                engine_coefficient_a = $signed({2'b00, engine_radius});
                engine_coefficient_b = $signed({2'b00, engine_radius});
            end
            4'd2: begin
                // Pole recurrence: a1*y[n-1] and r2*y[n-2].
                engine_operand_a = engine_state_y1;
                engine_operand_b = engine_state_y2;
                engine_coefficient_a = pole_a1_q;
                engine_coefficient_b = pole_r2_q;
            end
            4'd3: begin
                // Unity-DC input term: b0*x[n].
                engine_operand_a = engine_input;
                engine_coefficient_a = pole_b0_q;
            end
            4'd6: begin
                // Reuse lane A for the final sheet-2 filter amplitude.
                engine_operand_a = output_sum_q;
                engine_coefficient_a = $signed(
                    {4'b0000, filter_amp_gain(filter_amp_code)}
                );
            end
            default: begin
            end
        endcase
        engine_product_a = engine_operand_a * engine_coefficient_a;
        engine_product_b = engine_operand_b * engine_coefficient_b;
        product_a_q_ext = {{7{product_a_q[40]}}, product_a_q};
        product_b_q_ext = {{7{product_b_q[40]}}, product_b_q};

        // Stage 1 consumes the Q28 coefficient products registered at stage
        // 0.  All three coefficients fit a signed Q3.14 value.
        pole_a1_next = $signed(
            (product_a_q_ext + 48'sd4096) >>> 13
        );
        pole_r2_next = $signed(
            (product_b_q_ext + 48'sd8192) >>> 14
        );
        pole_b0_wide = 18'sd16384 -
                       {{1{pole_a1_next[16]}}, pole_a1_next} +
                       {{1{pole_r2_next[16]}}, pole_r2_next};

        recurrence_accumulator = product_a_q_ext - product_b_q_ext;
        // Round the complete recurrence once, then saturate once.  Keeping
        // both pole terms and the input term in Q14 until this point avoids
        // the level and bias errors caused by saturating each term alone.
        rounded_section_accumulator = section_accumulator_q +
            (section_accumulator_q[47] ? 48'sd8191 : 48'sd8192);
        engine_section_next = sat24_from48(
            rounded_section_accumulator >>> 14
        );
        engine_output_next = sat24_from48(product_a_q_ext >>> 12);
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
            filter_frequency_q <= 8'hFF;
            engine_busy_q <= 1'b0;
            engine_overrun_q <= 1'b0;
            engine_section_q <= 3'd0;
            engine_stage_q <= 4'd0;
            product_a_q <= 41'sd0;
            product_b_q <= 41'sd0;
            pole_a1_q <= 17'sd0;
            pole_r2_q <= 17'sd0;
            pole_b0_q <= 17'sd0;
            recurrence_accumulator_q <= 48'sd0;
            section_accumulator_q <= 48'sd0;
            output_sum_q <= 24'sd0;
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

            // U75 advances on the rising edge.  U73 shifts on the following
            // falling edge, so the forcing term uses the new U75 count while
            // every HCC4006 feedback tap uses the old register state.
            if (noise_advance) begin
                noise_count_q <= noise_count_next;
                noise_d1_q <= {noise_d1_q[2:0], noise_d3_q[3]};
                noise_d2_q <= {noise_d2_q[3:0], noise_d4_q[4]};
                noise_d3_q <= {noise_d3_q[2:0], noise_d2_q[4]};
                noise_d4_q <= {noise_d4_q[3:0], noise_feedback};
            end

            // CLOSURE lasts for one fabric clock.  It discharges the held
            // output but does not erase the filter's persistent charge.
            if (closure)
                output_hold_q <= 24'sd0;

            if (filter_phase_ce) begin
                filter_frequency_q <= filter_frequency;

                if (engine_busy_q)
                    engine_overrun_q <= 1'b1;

                if (!filter_phase) begin
                    // Phi0 holds one input sample per physical section.  The
                    // states then stay fixed while the MAC scheduler works.
                    f1_input_q <= voice_source;
                    f2_input_q <= f1_state_q;
                    // Sheet 1 ties FRIC_1 to the F2-output/F3-input node.
                    f3_input_q <= sat24_add(
                        f2_state_q,
                        source_fric1_sw ? fric1_injection : 24'sd0
                    );
                    f4_input_q <= f3_state_q;
                    // Sheet 2 ties FRIC_2 to the F4-output/F5-input node.
                    f5_input_q <= sat24_add(
                        f4_state_q,
                        source_fric2_sw ? fric2_injection : 24'sd0
                    );
                end

                if (filter_phase && !engine_busy_q) begin
                    engine_busy_q <= 1'b1;
                    engine_section_q <= 3'd0;
                    engine_stage_q <= 4'd0;
                end
            end

            // Every DSP result first enters product_a_q/product_b_q.  Adds,
            // saturation, and state commits occur only on later clocks.  One
            // section takes six clocks; five sections plus two output clocks
            // take 32 clocks.  The fastest intended phase gap is 133.
            if (engine_busy_q) begin
                case (engine_stage_q)
                    4'd0: begin
                        product_a_q <= engine_product_a;
                        product_b_q <= engine_product_b;
                        engine_stage_q <= 4'd1;
                    end
                    4'd1: begin
                        pole_a1_q <= pole_a1_next;
                        pole_r2_q <= pole_r2_next;
                        pole_b0_q <= pole_b0_wide[16:0];
                        engine_stage_q <= 4'd2;
                    end
                    4'd2: begin
                        product_a_q <= engine_product_a;
                        product_b_q <= engine_product_b;
                        engine_stage_q <= 4'd3;
                    end
                    4'd3: begin
                        product_a_q <= engine_product_a;
                        recurrence_accumulator_q <= recurrence_accumulator;
                        engine_stage_q <= 4'd4;
                    end
                    4'd4: begin
                        section_accumulator_q <= recurrence_accumulator_q +
                                                 product_a_q_ext;
                        engine_stage_q <= 4'd5;
                    end
                    4'd5: begin
                        case (engine_section_q)
                            3'd0: begin
                                f1_state_q <= engine_section_next;
                                f1_history_q <= engine_state_y1;
                            end
                            3'd1: begin
                                f2_state_q <= engine_section_next;
                                f2_history_q <= engine_state_y1;
                            end
                            3'd2: begin
                                f3_state_q <= engine_section_next;
                                f3_history_q <= engine_state_y1;
                            end
                            3'd3: begin
                                f4_state_q <= engine_section_next;
                                f4_history_q <= engine_state_y1;
                            end
                            default: begin
                                f5_state_q <= engine_section_next;
                                f5_history_q <= engine_state_y1;
                            end
                        endcase

                        if (engine_section_q == 3'd4) begin
                            output_sum_q <= engine_section_next;
                            engine_stage_q <= 4'd6;
                        end
                        else begin
                            engine_section_q <= engine_section_q + 3'd1;
                            engine_stage_q <= 4'd0;
                        end
                    end
                    4'd6: begin
                        product_a_q <= engine_product_a;
                        engine_stage_q <= 4'd7;
                    end
                    default: begin
                        if (!closure) begin
                            output_hold_q <= engine_output_next;
                            // output_hold_q is the recurring switched node.
                            // The external analog output reconstructs completed
                            // section results; it does not expose a random Phi
                            // carrier phase to the board's 48 kHz sampler.
                            reconstruction_hold_q <= engine_output_next;
                        end
                        engine_busy_q <= 1'b0;
                        engine_stage_q <= 4'd0;
                    end
                endcase
            end

            // C381 AC-couples the reconstructed output.  Spread its one-pole
            // DC blocker over two fabric clocks so it cannot add a long carry
            // chain to the tract scheduler.  At 48 kHz, 255/256 gives a pole
            // near 30 Hz.  Keep x-x[n-1] and y+delta wide, apply the leak,
            // then clamp once; early 24-bit clamps distort large reversals.
            // Inactive samples feed zero so the stored offset decays without
            // exposing a tail on the PCM output.
            if (audio_tick) begin
                dc_delta_q <= {dc_input[23], dc_input} -
                              {dc_previous_input_q[23],
                               dc_previous_input_q};
                dc_previous_input_q <= dc_input;
                dc_active_stage1_q <= phone_active && !powered_down;
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
