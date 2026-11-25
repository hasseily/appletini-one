`timescale 1ns / 1ps

// Mockingboard / Phasor output chain: pan, channel sums, speech mix, tone
// controls and the final limit.
//
// Left and right use the same arithmetic. One copy of it serves both: the
// left sample enters on one fabric clock and the right sample on the next.
// Each pipeline register then holds left and right in turn. A register that
// keeps state from sample to sample (the three tone filters) is a chain of
// two, so that each side meets its own state again two clocks later.
//
// The tone filters run once in two fabric clocks for each side. Their
// shifts are one less than a filter that runs in each clock would use, which
// gives the same corner frequencies (about 324 Hz, 1.3 kHz and 2.6 kHz).
module mockingboard_audio_mix (
    input  logic               clk,
    input  logic               rstn,
    input  logic               card_enabled,
    input  logic               phasor_native,
    input  logic               echo_plus,
    input  logic [7:0]         channel [0:11], // PSG 0-3, channels A, B, C
    input  logic [47:0]        pan,
    input  logic [31:0]        audio_control,
    input  logic signed [17:0] ssi_mix_l,
    input  logic signed [17:0] ssi_mix_r,
    output logic signed [15:0] audio_l,
    output logic signed [15:0] audio_r
);

function automatic logic [4:0] pan_gain_l(input logic [3:0] value);
    case (value)
        4'd0, 4'd1, 4'd2, 4'd3,
        4'd4, 4'd5, 4'd6, 4'd7,
        4'd8:   pan_gain_l = 5'd16;
        4'd9:   pan_gain_l = 5'd14;
        4'd10:  pan_gain_l = 5'd11;
        4'd11:  pan_gain_l = 5'd9;
        4'd12:  pan_gain_l = 5'd7;
        4'd13:  pan_gain_l = 5'd5;
        4'd14:  pan_gain_l = 5'd2;
        default: pan_gain_l = 5'd0;
    endcase
endfunction

function automatic logic [4:0] pan_gain_r(input logic [3:0] value);
    case (value)
        4'd0:   pan_gain_r = 5'd0;
        4'd1:   pan_gain_r = 5'd2;
        4'd2:   pan_gain_r = 5'd4;
        4'd3:   pan_gain_r = 5'd6;
        4'd4:   pan_gain_r = 5'd8;
        4'd5:   pan_gain_r = 5'd10;
        4'd6:   pan_gain_r = 5'd12;
        4'd7:   pan_gain_r = 5'd14;
        default: pan_gain_r = 5'd16;
    endcase
endfunction

function automatic logic [8:0] apply_pan_gain(input logic [7:0] sample,
                                              input logic [4:0] gain);
    logic fractional;
    logic [8:0] half, quarter, eighth, sixteenth;
    logic [8:0] s;
    s = {1'b0, sample};
    // Select each shift before adding to preserve truncation per term.
    case (gain)
        5'd0, 5'd2, 5'd4, 5'd5, 5'd6, 5'd7,
        5'd8, 5'd9, 5'd10, 5'd11, 5'd12, 5'd14: fractional = 1'b1;
        default: fractional = 1'b0;
    endcase
    half = gain[3] ? (s >> 1) : 9'd0;
    quarter = gain[2] ? (s >> 2) : 9'd0;
    eighth = gain[1] ? (s >> 3) : 9'd0;
    sixteenth = gain[0] ? (s >> 4) : 9'd0;
    apply_pan_gain = fractional ? (half + quarter) + (eighth + sixteenth) : s;
endfunction

function automatic signed [16:0] mix2_to_wide(input logic [10:0] value);
    mix2_to_wide = $signed({2'b00, value, 4'b0000});
endfunction

function automatic signed [16:0] mix4_to_wide(input logic [11:0] value);
    // Keep the full four-AY sum until signed SSI is added. Clamping AY first
    // would lose valid cancellation from a negative speech sample.
    mix4_to_wide = $signed({1'b0, value, 4'b0000});
endfunction

function automatic signed [15:0] sat_add16(input signed [15:0] a,
                                           input signed [15:0] b);
    logic signed [16:0] sum;
    begin
        sum = {a[15], a} + {b[15], b};
        if (sum > 17'sd32767) begin
            sat_add16 = 16'sh7FFF;
        end else if (sum < -17'sd32768) begin
            sat_add16 = -16'sd32768;
        end else begin
            sat_add16 = sum[15:0];
        end
    end
endfunction

function automatic signed [15:0] mix_speech(input signed [16:0] base,
                                            input signed [17:0] speech);
    logic signed [18:0] combined;
    begin
        combined=19'(base)+19'(speech);
        if (combined > 19'sd32767) mix_speech=16'sh7fff;
        else if (combined < -19'sd32768) mix_speech=16'sh8000;
        else mix_speech=combined[15:0];
    end
endfunction

function automatic signed [4:0] clamp_audio_control(input logic [4:0] raw);
    logic signed [4:0] value;
    begin
        value = raw;
        if (value > 5'sd8) begin
            clamp_audio_control = 5'sd8;
        end else if (value < -5'sd8) begin
            clamp_audio_control = -5'sd8;
        end else begin
            clamp_audio_control = value;
        end
    end
endfunction

function automatic signed [31:0] pcm_to_tone_fp(input signed [15:0] sample);
    pcm_to_tone_fp = {{4{sample[15]}}, sample, 12'b0000_0000_0000};
endfunction

function automatic signed [15:0] tone_fp_to_pcm(input signed [31:0] sample);
    tone_fp_to_pcm = sample[27:12];
endfunction

function automatic signed [20:0] audio_control_adjust(input signed [15:0] sample,
                                                      input signed [4:0] control);
    logic signed [20:0] widened;
    logic signed [20:0] adjusted;
    logic signed [4:0] magnitude;
    begin
        widened = {{5{sample[15]}}, sample};
        magnitude = (control < 0) ? -control : control;
        case (magnitude[3:0])
            4'd0:    adjusted = 21'sd0;
            4'd1:    adjusted = widened >>> 3;
            4'd2:    adjusted = widened >>> 2;
            4'd3:    adjusted = (widened >>> 2) + (widened >>> 3);
            4'd4:    adjusted = widened >>> 1;
            4'd5:    adjusted = (widened >>> 1) + (widened >>> 3);
            4'd6:    adjusted = (widened >>> 1) + (widened >>> 2);
            4'd7:    adjusted = (widened >>> 1) + (widened >>> 2) + (widened >>> 3);
            default: adjusted = widened;
        endcase
        audio_control_adjust = (control < 0) ? -adjusted : adjusted;
    end
endfunction

// Bass, mid and treble: the part of the band that is added to the mix.
// A cut removes 1/8 of the band for each step, and step 8 removes all of it.
// A boost grows faster toward the end of the range:
//   step   1    2    3    4    5    6    7    8
//   adds  1/8  1/4  1/2  3/4   1   1.5   2    3   times the band
//   gain  +1.0 +1.9 +3.5 +4.9 +6.0 +8.0 +9.5 +12.0 dB
function automatic signed [20:0] tone_band_adjust(input signed [15:0] sample,
                                                  input signed [4:0] control);
    logic signed [20:0] widened;
    begin
        widened = {{5{sample[15]}}, sample};
        if (control <= 0) begin
            tone_band_adjust = audio_control_adjust(sample, control);
        end else begin
            case (control[3:0])
                4'd1:    tone_band_adjust = widened >>> 3;
                4'd2:    tone_band_adjust = widened >>> 2;
                4'd3:    tone_band_adjust = widened >>> 1;
                4'd4:    tone_band_adjust = (widened >>> 1) + (widened >>> 2);
                4'd5:    tone_band_adjust = widened;
                4'd6:    tone_band_adjust = widened + (widened >>> 1);
                4'd7:    tone_band_adjust = widened <<< 1;
                default: tone_band_adjust = (widened <<< 1) + widened;
            endcase
        end
    end
endfunction

function automatic signed [20:0] warmth_treble_adjust(input signed [15:0] sample,
                                                       input signed [4:0] control);
    logic signed [20:0] shaped;
    begin
        shaped = audio_control_adjust(sample, control) >>> 2;
        warmth_treble_adjust = -shaped;
    end
endfunction

function automatic signed [15:0] sat16_from21(input signed [20:0] sample);
    begin
        if (sample > 21'sd32767) begin
            sat16_from21 = 16'sh7FFF;
        end else if (sample < -21'sd32768) begin
            sat16_from21 = -16'sd32768;
        end else begin
            sat16_from21 = sample[15:0];
        end
    end
endfunction

function automatic signed [20:0] warm_shape_from21(input signed [20:0] sample,
                                                   input signed [4:0] warmth);
    logic signed [20:0] shaped;
    logic signed [20:0] knee;
    logic signed [20:0] warmth_ext;
    logic signed [20:0] excess;
    begin
        shaped = sample;
        if (warmth > 0) begin
            warmth_ext = {{16{warmth[4]}}, warmth};
            knee = 21'sd28672 - (warmth_ext <<< 10);
            if (shaped > knee) begin
                excess = shaped - knee;
                shaped = knee + (excess >>> 1) + (excess >>> 3);
            end else if (shaped < -knee) begin
                excess = (-knee) - shaped;
                shaped = (-knee) - (excess >>> 1) - (excess >>> 3);
            end
        end
        warm_shape_from21 = shaped;
    end
endfunction

function automatic logic [9:0] sum3(input logic [8:0] a,
                                    input logic [8:0] b,
                                    input logic [8:0] c);
    sum3 = {1'b0, a} + {1'b0, b} + {1'b0, c};
endfunction

function automatic logic [10:0] sum2_10(input logic [9:0] a,
                                        input logic [9:0] b);
    sum2_10 = {1'b0, a} + {1'b0, b};
endfunction

function automatic logic [11:0] sum4_10(input logic [9:0] a,
                                        input logic [9:0] b,
                                        input logic [9:0] c,
                                        input logic [9:0] d);
    sum4_10 = {2'b0, a} + {2'b0, b} + {2'b0, c} + {2'b0, d};
endfunction

function automatic logic [9:0] mix3_l(input logic [7:0] a,
                                      input logic [7:0] b,
                                      input logic [7:0] c,
                                      input logic [11:0] pan_word);
    mix3_l = sum3(apply_pan_gain(a, pan_gain_l(pan_word[3:0])),
                  apply_pan_gain(b, pan_gain_l(pan_word[7:4])),
                  apply_pan_gain(c, pan_gain_l(pan_word[11:8])));
endfunction

function automatic logic [9:0] mix3_r(input logic [7:0] a,
                                      input logic [7:0] b,
                                      input logic [7:0] c,
                                      input logic [11:0] pan_word);
    mix3_r = sum3(apply_pan_gain(a, pan_gain_r(pan_word[3:0])),
                  apply_pan_gain(b, pan_gain_r(pan_word[7:4])),
                  apply_pan_gain(c, pan_gain_r(pan_word[11:8])));
endfunction

// 0: this clock takes the left inputs, 1: the right inputs. Stage n of the
// pipeline holds the side that entered n clocks before.
logic side_q;
logic [8:0]  gain_q [0:11];
logic [9:0]  sum_q [0:3];
logic [10:0] mockingboard_mix_q;
logic [10:0] echo_mix_q;
logic [11:0] phasor_mix_q;
logic signed [15:0] tone_base_q;
// Filter state: [1] belongs to the side now in tone_base_q.
logic signed [31:0] tone_low_q [0:1];
logic signed [31:0] tone_warm_lp_q [0:1];
logic signed [31:0] tone_mid_lp_q [0:1];
logic signed [15:0] tone_apply_base_q;
logic signed [15:0] tone_bass_q;
logic signed [15:0] tone_warm_q;
logic signed [15:0] tone_mid_q;
logic signed [15:0] tone_treble_q;
logic signed [4:0]  tone_bass_control_q;
logic signed [4:0]  tone_mid_control_q;
logic signed [4:0]  tone_treble_control_q;
logic signed [4:0]  tone_warm_control_q;
logic signed [4:0]  tone_volume_control_q;
logic signed [20:0] tone_base_ext_q;
logic signed [20:0] tone_bass_adjust_q;
logic signed [20:0] tone_warm_adjust_q;
logic signed [20:0] tone_warm_treble_adjust_q;
logic signed [20:0] tone_mid_adjust_q;
logic signed [20:0] tone_treble_adjust_q;
logic signed [20:0] tone_volume_adjust_q;
logic signed [20:0] tone_shaped_q;
logic signed [20:0] tone_warm_shaped_q;

// The pan word of each channel, and the side each stage holds.
wire [3:0] pan_code [0:11];
for (genvar n = 0; n < 12; n++) begin : pan_select
    assign pan_code[n] = pan[4*n +: 4];
end
// A register that is n registers after the inputs holds this clock's side
// when n is even, and the other side when n is odd.

// The mix registers are three registers after the inputs, so they hold the
// other side from this clock's inputs. Their speech is that side's.
wire signed [17:0] side_speech = side_q ? ssi_mix_l : ssi_mix_r;
// The sound chips of the card mode, with the speech, limited to 16 bits.
// Plain wires: xsim gave a stale result for mix_speech() in this place.
wire signed [16:0] mix_wide =
    phasor_native ? mix4_to_wide(phasor_mix_q) :
    echo_plus     ? mix2_to_wide(echo_mix_q) :
                    mix2_to_wide(mockingboard_mix_q);
wire signed [18:0] base_sum =
    {{2{mix_wide[16]}}, mix_wide} + {side_speech[17], side_speech};
wire signed [15:0] base_next =
    (base_sum > 19'sd32767)  ? 16'sh7fff :
    (base_sum < -19'sd32768) ? 16'sh8000 : base_sum[15:0];
// Filter outputs of the side in tone_base_q.
wire signed [15:0] low = tone_fp_to_pcm(tone_low_q[1]);
wire signed [15:0] warm_lp = tone_fp_to_pcm(tone_warm_lp_q[1]);
wire signed [15:0] mid_lp = tone_fp_to_pcm(tone_mid_lp_q[1]);

always_ff @(posedge clk) begin
    if (!rstn || !card_enabled) begin
        side_q <= 1'b0;
        for (int n = 0; n < 12; n++) gain_q[n] <= '0;
        for (int n = 0; n < 4; n++) sum_q[n] <= '0;
        mockingboard_mix_q <= '0;
        echo_mix_q <= '0;
        phasor_mix_q <= '0;
        tone_base_q <= '0;
        for (int n = 0; n < 2; n++) begin
            tone_low_q[n] <= '0;
            tone_warm_lp_q[n] <= '0;
            tone_mid_lp_q[n] <= '0;
        end
        tone_apply_base_q <= '0;
        tone_bass_q <= '0;
        tone_warm_q <= '0;
        tone_mid_q <= '0;
        tone_treble_q <= '0;
        tone_bass_control_q <= '0;
        tone_mid_control_q <= '0;
        tone_treble_control_q <= '0;
        tone_warm_control_q <= '0;
        tone_volume_control_q <= '0;
        tone_base_ext_q <= '0;
        tone_bass_adjust_q <= '0;
        tone_warm_adjust_q <= '0;
        tone_warm_treble_adjust_q <= '0;
        tone_mid_adjust_q <= '0;
        tone_treble_adjust_q <= '0;
        tone_volume_adjust_q <= '0;
        tone_shaped_q <= '0;
        tone_warm_shaped_q <= '0;
        audio_l <= '0;
        audio_r <= '0;
    end else begin
        side_q <= !side_q;

        // Stage 1: pan gain of the twelve channels, for this clock's side.
        for (int n = 0; n < 12; n++)
            gain_q[n] <= apply_pan_gain(channel[n],
                side_q ? pan_gain_r(pan_code[n]) : pan_gain_l(pan_code[n]));
        // Stage 2: one sum for each sound chip.
        for (int n = 0; n < 4; n++)
            sum_q[n] <= sum3(gain_q[3*n], gain_q[3*n + 1], gain_q[3*n + 2]);
        // Stage 3: the chip sums of each card mode.
        mockingboard_mix_q <= sum2_10(sum_q[0], sum_q[1]);
        echo_mix_q <= sum2_10(sum_q[1], sum_q[3]);
        phasor_mix_q <= sum4_10(sum_q[0], sum_q[1], sum_q[2], sum_q[3]);

        begin : final_audio_mix
            // Stage 4: the sound chips of the card mode, with the speech.
            tone_base_q <= base_next;

            // Tone filters of the side in tone_base_q.
            tone_low_q[0] <= tone_low_q[1] +
                ((pcm_to_tone_fp(tone_base_q) - tone_low_q[1]) >>> 15);
            tone_warm_lp_q[0] <= tone_warm_lp_q[1] +
                ((pcm_to_tone_fp(tone_base_q) - tone_warm_lp_q[1]) >>> 13);
            tone_mid_lp_q[0] <= tone_mid_lp_q[1] +
                ((pcm_to_tone_fp(tone_base_q) - tone_mid_lp_q[1]) >>> 12);
            tone_low_q[1] <= tone_low_q[0];
            tone_warm_lp_q[1] <= tone_warm_lp_q[0];
            tone_mid_lp_q[1] <= tone_mid_lp_q[0];

            tone_apply_base_q <= tone_base_q;
            tone_bass_q <= low;
            tone_warm_q <= sat16_from21({{5{warm_lp[15]}}, warm_lp} -
                                        {{5{low[15]}}, low});
            tone_mid_q <= sat16_from21({{5{mid_lp[15]}}, mid_lp} -
                                       {{5{low[15]}}, low});
            tone_treble_q <= sat16_from21({{5{tone_base_q[15]}}, tone_base_q} -
                                          {{5{mid_lp[15]}}, mid_lp});

            tone_bass_control_q <= clamp_audio_control(audio_control[4:0]);
            tone_mid_control_q <= clamp_audio_control(audio_control[9:5]);
            tone_treble_control_q <= clamp_audio_control(audio_control[14:10]);
            tone_warm_control_q <= clamp_audio_control(audio_control[19:15]);
            tone_volume_control_q <= clamp_audio_control(audio_control[24:20]);

            tone_base_ext_q <= {{5{tone_apply_base_q[15]}}, tone_apply_base_q};
            tone_bass_adjust_q <= tone_band_adjust(tone_bass_q, tone_bass_control_q);
            tone_warm_adjust_q <= audio_control_adjust(tone_warm_q, tone_warm_control_q);
            tone_warm_treble_adjust_q <= warmth_treble_adjust(tone_treble_q, tone_warm_control_q);
            tone_mid_adjust_q <= tone_band_adjust(tone_mid_q, tone_mid_control_q);
            tone_treble_adjust_q <= tone_band_adjust(tone_treble_q, tone_treble_control_q);
            tone_volume_adjust_q <= audio_control_adjust(tone_apply_base_q, tone_volume_control_q);

            tone_shaped_q <= tone_base_ext_q +
                             tone_bass_adjust_q +
                             tone_warm_adjust_q +
                             tone_mid_adjust_q +
                             tone_treble_adjust_q +
                             tone_warm_treble_adjust_q +
                             tone_volume_adjust_q;
            tone_warm_shaped_q <= warm_shape_from21(tone_shaped_q, tone_warm_control_q);

            // tone_warm_shaped_q is eight registers after the inputs: the
            // same side as this clock's inputs.
            if (side_q)
                audio_r <= sat16_from21(tone_warm_shaped_q);
            else
                audio_l <= sat16_from21(tone_warm_shaped_q);
        end
    end
end

endmodule
