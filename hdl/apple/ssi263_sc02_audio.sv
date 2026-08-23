`timescale 1ns / 1ps

// Native SSI-263A / SC-02 excitation and switched-capacitor audio checkpoint.
//
// This block deliberately has no SC-01 phone map or coefficient tables.  It
// consumes the persistent controls and clock enables produced by
// ssi263_sc02_core.  The pitch shaper and HCC4006 recurrence follow sheets 3
// and 6 structurally.  The analog filter is a stable phase-held resonator
// checkpoint.  Each controlled section has a conjugate pole pair whose angle
// follows the source capacitor totals; F2 resonance controls its pole radius.
// F5 and the two fricative sections are separate fixed resonators.  This gives
// usable formant peaks without borrowing SC-01 tables, but it is not yet the
// final phase-by-phase nodal solution of the LF356/CD4016 network.
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
    localparam logic [14:0] FRIC1_RADIUS_Q14 = 15'd14090; // 0.860
    localparam logic [14:0] FRIC2_RADIUS_Q14 = 15'd13107; // 0.800
    // The analog op-amp network supplies inter-section gain that a pole-only
    // model does not contain.  A fixed 0.25 injection keeps a five-section
    // cascade above fixed-point quantization without changing pole centers.
    localparam logic signed [16:0] SECTION_DRIVE_Q14 = 17'sd4096;

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
    logic noise_advance;

    logic signed [23:0] voice_source;
    logic signed [23:0] fric_source;
    logic signed [23:0] voice_magnitude;
    logic signed [23:0] fric_positive_magnitude;
    logic signed [23:0] fric_negative_magnitude;

    logic signed [23:0] f1_state_q;
    logic signed [23:0] f1_quadrature_q;
    logic signed [23:0] f2_state_q;
    logic signed [23:0] f2_res_state_q;
    logic signed [23:0] f3_state_q;
    logic signed [23:0] f3_quadrature_q;
    logic signed [23:0] f4_state_q;
    logic signed [23:0] f4_quadrature_q;
    logic signed [23:0] f5_state_q;
    logic signed [23:0] f5_quadrature_q;
    logic signed [23:0] fric1_state_q;
    logic signed [23:0] fric1_quadrature_q;
    logic signed [23:0] fric2_state_q;
    logic signed [23:0] fric2_quadrature_q;

    logic signed [23:0] f1_input_q;
    logic signed [23:0] f2_input_q;
    logic signed [23:0] f3_input_q;
    logic signed [23:0] f4_input_q;
    logic signed [23:0] f5_input_q;
    logic signed [23:0] fric1_input_q;
    logic signed [23:0] fric2_input_q;

    logic signed [23:0] output_hold_q;
    logic [7:0] filter_frequency_q;

    // Two registered RTL product lanes form a 53-cycle scheduler. Vivado
    // 2025.2 maps the registered sum/difference uses to four DSP48E1 cells;
    // the key bound is one DSP operation between fabric registers.
    // The default XCK/DIV2 profile leaves 149 fabric clocks between the
    // fastest filter phases, with margin for two independent instances.
    logic engine_busy_q;
    logic engine_overrun_q;
    logic [2:0] engine_section_q;
    logic [3:0] engine_stage_q;
    logic signed [23:0] engine_state_i;
    logic signed [23:0] engine_state_q;
    logic signed [23:0] engine_input;
    logic signed [15:0] engine_cosine;
    logic signed [15:0] engine_sine;
    logic [14:0] engine_radius;
    logic signed [23:0] engine_operand_a;
    logic signed [23:0] engine_operand_b;
    logic signed [16:0] engine_coefficient_a;
    logic signed [16:0] engine_coefficient_b;
    logic signed [40:0] engine_product_a;
    logic signed [40:0] engine_product_b;
    logic signed [40:0] product_a_q;
    logic signed [40:0] product_b_q;
    logic signed [23:0] rotated_i_q;
    logic signed [23:0] rotated_q_q;
    logic signed [23:0] damped_i_q;
    logic signed [23:0] damped_q_q;
    logic signed [23:0] drive_i_q;
    logic signed [23:0] fric_sum_q;
    logic signed [23:0] output_sum_q;
    logic signed [23:0] engine_next_i;
    logic signed [47:0] product_a_q_ext;
    logic signed [47:0] product_b_q_ext;
    logic signed [47:0] rotate_i_accumulator;
    logic signed [47:0] rotate_q_accumulator;

    function automatic logic signed [23:0] sat24_from48(
        input logic signed [47:0] value
    );
        begin
            if (value > 48'sd8388607)
                sat24_from48 = 24'sh7FFFFF;
            else if (value < -48'sd8388608)
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

    function automatic logic signed [23:0] sat24_sub(
        input logic signed [23:0] a,
        input logic signed [23:0] b
    );
        logic signed [24:0] difference;
        begin
            difference = {a[23], a} - {b[23], b};
            if (difference > 25'sd8388607)
                sat24_sub = 24'sh7FFFFF;
            else if (difference < -25'sd8388608)
                sat24_sub = -24'sd8388608;
            else
                sat24_sub = difference[23:0];
        end
    endfunction

    function automatic logic signed [15:0] sat16_from24_q16(
        input logic signed [23:0] value
    );
        logic signed [23:0] scaled;
        begin
            scaled = value >>> 1;
            if (scaled > 24'sd32767)
                sat16_from24_q16 = 16'sh7FFF;
            else if (scaled < -24'sd32768)
                sat16_from24_q16 = -16'sd32768;
            else
                sat16_from24_q16 = scaled[15:0];
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
            if (code[0]) total = total + 250;
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
            if (code[0]) total = total + 270;
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
                4'h1: f2_cos_q14 = 16'sd15991;
                4'h2: f2_cos_q14 = 16'sd15796;
                4'h3: f2_cos_q14 = 16'sd15612;
                4'h4: f2_cos_q14 = 16'sd15349;
                4'h5: f2_cos_q14 = 16'sd15110;
                4'h6: f2_cos_q14 = 16'sd14781;
                4'h7: f2_cos_q14 = 16'sd14489;
                4'h8: f2_cos_q14 = 16'sd14016;
                4'h9: f2_cos_q14 = 16'sd13669;
                4'hA: f2_cos_q14 = 16'sd13210;
                4'hB: f2_cos_q14 = 16'sd12816;
                4'hC: f2_cos_q14 = 16'sd12299;
                4'hD: f2_cos_q14 = 16'sd11861;
                4'hE: f2_cos_q14 = 16'sd11292;
                default: f2_cos_q14 = 16'sd10813;
            endcase
        end
    endfunction

    function automatic logic signed [15:0] f2_sin_q14(input logic [3:0] code);
        begin
            case (code)
                4'h0: f2_sin_q14 = 16'sd2933;
                4'h1: f2_sin_q14 = 16'sd3569;
                4'h2: f2_sin_q14 = 16'sd4350;
                4'h3: f2_sin_q14 = 16'sd4972;
                4'h4: f2_sin_q14 = 16'sd5732;
                4'h5: f2_sin_q14 = 16'sd6335;
                4'h6: f2_sin_q14 = 16'sd7069;
                4'h7: f2_sin_q14 = 16'sd7648;
                4'h8: f2_sin_q14 = 16'sd8484;
                4'h9: f2_sin_q14 = 16'sd9032;
                4'hA: f2_sin_q14 = 16'sd9692;
                4'hB: f2_sin_q14 = 16'sd10208;
                4'hC: f2_sin_q14 = 16'sd10824;
                4'hD: f2_sin_q14 = 16'sd11303;
                4'hE: f2_sin_q14 = 16'sd11871;
                default: f2_sin_q14 = 16'sd12309;
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
                4'h1: f2_radius_q14 = 15'd14549;
                4'h2: f2_radius_q14 = 15'd14627;
                4'h3: f2_radius_q14 = 15'd14758;
                4'h4: f2_radius_q14 = 15'd14841;
                4'h5: f2_radius_q14 = 15'd14972;
                4'h6: f2_radius_q14 = 15'd15050;
                4'h7: f2_radius_q14 = 15'd15181;
                4'h8: f2_radius_q14 = 15'd15293;
                4'h9: f2_radius_q14 = 15'd15424;
                4'hA: f2_radius_q14 = 15'd15502;
                4'hB: f2_radius_q14 = 15'd15633;
                4'hC: f2_radius_q14 = 15'd15716;
                4'hD: f2_radius_q14 = 15'd15847;
                4'hE: f2_radius_q14 = 15'd15925;
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

        noise_count_next = (noise_count_q == 4'hF) ?
                           4'h0 : noise_count_q + 4'h1;
        noise_force = ~(noise_count_next[2] | noise_count_next[3]);
        noise_feedback = noise_force ^ noise_d1_q[3] ^ noise_d2_q[4] ^
                         noise_d4_q[3] ^ noise_d4_q[4];
        noise_bit = noise_d3_q[3];
        // U41C clocks U75 and U73 without a CTL, PD, phone, or FRICATIVE
        // gate.  The core has already resolved that physical clock edge.
        noise_advance = noise_clock_ce;

        // Unit amplitude is exactly 2^16, so Q12 gain becomes gain << 4.
        // Keep these source paths free of general multipliers.
        voice_magnitude = $signed(
            {11'd0, voice_gain(voice_amp_code)}
        ) <<< 4;
        fric_positive_magnitude = $signed(
            {11'd0, fric_pos_gain(fric_amp_code)}
        ) <<< 4;
        fric_negative_magnitude = $signed(
            {11'd0, fric_neg_gain(fric_amp_code)}
        ) <<< 4;

        if (!voiced || !phone_active || powered_down) begin
            voice_source = 24'sd0;
        end else if (voiced_q) begin
            voice_source = voice_magnitude;
        end else begin
            voice_source = -voice_magnitude;
        end

        if (!fricative || !phone_active || powered_down) begin
            fric_source = 24'sd0;
        end else if (noise_bit) begin
            fric_source = fric_positive_magnitude;
        end else begin
            fric_source = -fric_negative_magnitude;
        end
    end

    always_comb begin
        engine_state_i = 24'sd0;
        engine_state_q = 24'sd0;
        engine_input = 24'sd0;
        engine_cosine = 16'sd16384;
        engine_sine = 16'sd0;
        engine_radius = 15'd0;

        case (engine_section_q)
            3'd0: begin
                engine_state_i = f1_state_q;
                engine_state_q = f1_quadrature_q;
                engine_input = f1_input_q;
                engine_cosine = f1_cos_q14(f1_code);
                engine_sine = f1_sin_q14(f1_code);
                engine_radius = F1_RADIUS_Q14;
            end
            3'd1: begin
                engine_state_i = f2_state_q;
                engine_state_q = f2_res_state_q;
                engine_input = f2_input_q;
                engine_cosine = f2_cos_q14(f2_code);
                engine_sine = f2_sin_q14(f2_code);
                engine_radius = f2_radius_q14(f2_res_code);
            end
            3'd2: begin
                engine_state_i = f3_state_q;
                engine_state_q = f3_quadrature_q;
                engine_input = f3_input_q;
                engine_cosine = f3_cos_q14(f3_code);
                engine_sine = f3_sin_q14(f3_code);
                engine_radius = F3_RADIUS_Q14;
            end
            3'd3: begin
                engine_state_i = f4_state_q;
                engine_state_q = f4_quadrature_q;
                engine_input = f4_input_q;
                engine_cosine = f4_cos_q14(f4_code);
                engine_sine = f4_sin_q14(f4_code);
                engine_radius = F4_RADIUS_Q14;
            end
            3'd4: begin
                engine_state_i = f5_state_q;
                engine_state_q = f5_quadrature_q;
                engine_input = f5_input_q;
                engine_cosine = 16'sd1159;
                engine_sine = 16'sd16343;
                engine_radius = F5_RADIUS_Q14;
            end
            3'd5: begin
                engine_state_i = fric1_state_q;
                engine_state_q = fric1_quadrature_q;
                engine_input = fric1_input_q;
                engine_cosine = 16'sd8152;
                engine_sine = 16'sd14212;
                engine_radius = FRIC1_RADIUS_Q14;
            end
            default: begin
                engine_state_i = fric2_state_q;
                engine_state_q = fric2_quadrature_q;
                engine_input = fric2_input_q;
                engine_cosine = 16'sd3107;
                engine_sine = 16'sd16087;
                engine_radius = FRIC2_RADIUS_Q14;
            end
        endcase

        engine_operand_a = 24'sd0;
        engine_operand_b = 24'sd0;
        engine_coefficient_a = 17'sd0;
        engine_coefficient_b = 17'sd0;
        case (engine_stage_q)
            4'd0: begin
                engine_operand_a = engine_state_i;
                engine_operand_b = engine_state_q;
                engine_coefficient_a = {{1{engine_cosine[15]}}, engine_cosine};
                engine_coefficient_b = {{1{engine_sine[15]}}, engine_sine};
            end
            4'd1: begin
                engine_operand_a = engine_state_i;
                engine_operand_b = engine_state_q;
                engine_coefficient_a = {{1{engine_sine[15]}}, engine_sine};
                engine_coefficient_b = {{1{engine_cosine[15]}}, engine_cosine};
            end
            4'd3: begin
                engine_operand_a = rotated_i_q;
                engine_operand_b = rotated_q_q;
                engine_coefficient_a = $signed({2'b00, engine_radius});
                engine_coefficient_b = $signed({2'b00, engine_radius});
            end
            4'd4: begin
                engine_operand_a = engine_input;
                engine_coefficient_a = SECTION_DRIVE_Q14;
            end
            4'd9: begin
                // Reuse RTL product lane A for final Q12 gain. Stages 7/8
                // registered the source sum, and stage 10 saturates this
                // registered product on a separate clock.
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
        rotate_i_accumulator = product_a_q_ext - product_b_q_ext;
        rotate_q_accumulator = product_a_q_ext + product_b_q_ext;
        engine_next_i = sat24_add(damped_i_q, drive_i_q);
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
            f1_quadrature_q <= 24'sd0;
            f2_state_q <= 24'sd0;
            f2_res_state_q <= 24'sd0;
            f3_state_q <= 24'sd0;
            f3_quadrature_q <= 24'sd0;
            f4_state_q <= 24'sd0;
            f4_quadrature_q <= 24'sd0;
            f5_state_q <= 24'sd0;
            f5_quadrature_q <= 24'sd0;
            fric1_state_q <= 24'sd0;
            fric1_quadrature_q <= 24'sd0;
            fric2_state_q <= 24'sd0;
            fric2_quadrature_q <= 24'sd0;

            f1_input_q <= 24'sd0;
            f2_input_q <= 24'sd0;
            f3_input_q <= 24'sd0;
            f4_input_q <= 24'sd0;
            f5_input_q <= 24'sd0;
            fric1_input_q <= 24'sd0;
            fric2_input_q <= 24'sd0;

            output_hold_q <= 24'sd0;
            filter_frequency_q <= 8'hFF;
            engine_busy_q <= 1'b0;
            engine_overrun_q <= 1'b0;
            engine_section_q <= 3'd0;
            engine_stage_q <= 4'd0;
            product_a_q <= 41'sd0;
            product_b_q <= 41'sd0;
            rotated_i_q <= 24'sd0;
            rotated_q_q <= 24'sd0;
            damped_i_q <= 24'sd0;
            damped_q_q <= 24'sd0;
            drive_i_q <= 24'sd0;
            fric_sum_q <= 24'sd0;
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
                    f3_input_q <= f2_state_q;
                    f4_input_q <= f3_state_q;
                    f5_input_q <= f4_state_q;
                    fric1_input_q <= fric1_sw ? fric_source : 24'sd0;
                    // FRIC1 and its complement FRIC2 select two separate
                    // noise resonators; they do not form a serial cascade.
                    fric2_input_q <= fric2_sw ? fric_source : 24'sd0;
                end

                if (filter_phase && !engine_busy_q) begin
                    engine_busy_q <= 1'b1;
                    engine_section_q <= 3'd0;
                    engine_stage_q <= 4'd0;
                end
            end

            // Every DSP result first enters product_a_q/product_b_q.  Adds,
            // saturation, and state commits occur only on later clocks.  One
            // section takes seven clocks; seven sections plus four output
            // clocks take 53 clocks.  The fastest intended phase gap is 133.
            if (engine_busy_q) begin
                case (engine_stage_q)
                    4'd0: begin
                        product_a_q <= engine_product_a;
                        product_b_q <= engine_product_b;
                        engine_stage_q <= 4'd1;
                    end
                    4'd1: begin
                        product_a_q <= engine_product_a;
                        product_b_q <= engine_product_b;
                        rotated_i_q <= sat24_from48(
                            rotate_i_accumulator >>> 14
                        );
                        engine_stage_q <= 4'd2;
                    end
                    4'd2: begin
                        rotated_q_q <= sat24_from48(
                            rotate_q_accumulator >>> 14
                        );
                        engine_stage_q <= 4'd3;
                    end
                    4'd3: begin
                        product_a_q <= engine_product_a;
                        product_b_q <= engine_product_b;
                        engine_stage_q <= 4'd4;
                    end
                    4'd4: begin
                        product_a_q <= engine_product_a;
                        damped_i_q <= sat24_from48(
                            product_a_q_ext >>> 14
                        );
                        damped_q_q <= sat24_from48(
                            product_b_q_ext >>> 14
                        );
                        engine_stage_q <= 4'd5;
                    end
                    4'd5: begin
                        drive_i_q <= sat24_from48(product_a_q_ext >>> 14);
                        engine_stage_q <= 4'd6;
                    end
                    4'd6: begin
                        case (engine_section_q)
                            3'd0: begin
                                f1_state_q <= engine_next_i;
                                f1_quadrature_q <= damped_q_q;
                            end
                            3'd1: begin
                                f2_state_q <= engine_next_i;
                                f2_res_state_q <= damped_q_q;
                            end
                            3'd2: begin
                                f3_state_q <= engine_next_i;
                                f3_quadrature_q <= damped_q_q;
                            end
                            3'd3: begin
                                f4_state_q <= engine_next_i;
                                f4_quadrature_q <= damped_q_q;
                            end
                            3'd4: begin
                                f5_state_q <= engine_next_i;
                                f5_quadrature_q <= damped_q_q;
                            end
                            3'd5: begin
                                fric1_state_q <= engine_next_i;
                                fric1_quadrature_q <= damped_q_q;
                            end
                            default: begin
                                fric2_state_q <= engine_next_i;
                                fric2_quadrature_q <= damped_q_q;
                            end
                        endcase

                        if (engine_section_q == 3'd6)
                            engine_stage_q <= 4'd7;
                        else begin
                            engine_section_q <= engine_section_q + 3'd1;
                            engine_stage_q <= 4'd0;
                        end
                    end
                    4'd7: begin
                        fric_sum_q <= sat24_add(
                            fric1_state_q, fric2_state_q
                        );
                        engine_stage_q <= 4'd8;
                    end
                    4'd8: begin
                        output_sum_q <= sat24_add(f5_state_q, fric_sum_q);
                        engine_stage_q <= 4'd9;
                    end
                    4'd9: begin
                        product_a_q <= engine_product_a;
                        engine_stage_q <= 4'd10;
                    end
                    default: begin
                        if (!closure) begin
                            output_hold_q <= sat24_from48(
                                product_a_q_ext >>> 12
                            );
                        end
                        engine_busy_q <= 1'b0;
                        engine_stage_q <= 4'd0;
                    end
                endcase
            end

            // The chip-side value is a held charge-domain output.  The board's
            // 48 kHz request samples it; no state interpolation occurs here.
            if (audio_tick) begin
                if (!phone_active || powered_down || closure)
                    audio_sample <= 16'sd0;
                else
                    audio_sample <= sat16_from24_q16(output_hold_q);
            end
        end
    end

endmodule
