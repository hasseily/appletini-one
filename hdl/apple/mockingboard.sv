`timescale 1ns / 1ps

// Applied Engineering Phasor-compatible sound card.
//
// The card resets into Mockingboard-compatible mode: two 6522 VIAs, each
// driving one AY-3-8913-compatible PSG. Phasor native mode is selected through
// the Phasor C0nX mode switch and adds a second AY behind each VIA. In native
// mode VIA ORB bit 4 selects the primary AY and bit 3 selects the secondary AY;
// both chip selects are active-low, matching the Phasor GAL behavior documented
// by AppleWin. The SSI263/SC-01 speech side preserves AppleWin's bus-visible
// edge cases and uses the local formant backend for audio.
module mockingboard(
    input clk,
    input rstn,
    input globals::AppleBus_read ab_read,
    input globals::SoftSwitchState sss,
    input logic [2:0] slot_assign,
    input logic [47:0] pan,
    input logic [31:0] audio_control,
    input logic audio_sample_tick,
    output globals::AppleBus_write ab_write,
    output logic signed [15:0] audio_l,
    output logic signed [15:0] audio_r
);

localparam logic [2:0] PH_MOCKINGBOARD = 3'd0;
localparam logic [2:0] PH_PHASOR       = 3'd5;
localparam logic [2:0] PH_ECHOPLUS     = 3'd7;
// MODE=1 selects the AY8910/AY8913 16-level output table; MODE=0 selects
// the YM2149 table. The PS menu drives this through audio_control bit 25.
wire psg_volume_mode_ay8913 = audio_control[25];
// audio_control bit 26: lock the card to Mockingboard-compatible mode. When
// set, the Apple-bus $C0nX Phasor mode switch is ignored entirely so software
// can never detect or enable Phasor-native / Echo+ behavior.
wire mockingboard_only = audio_control[26];

globals::AppleBus_write ab_write_q = 0;

logic [2:0] phasor_mode_q = PH_MOCKINGBOARD;
logic psg_ce_extra_q = 1'b0;

wire card_enabled = (slot_assign != 3'd0);
wire mockingboard_mode = (phasor_mode_q == PH_MOCKINGBOARD);
wire phasor_native = (phasor_mode_q == PH_PHASOR);
wire echo_plus = (phasor_mode_q == PH_ECHOPLUS);
wire phasor_extended = phasor_native || echo_plus;
wire ssi_visible_mode = mockingboard_mode || phasor_native;
wire [3:0] phasor_mode_nibble = {1'b1, slot_assign};

/* ab_read.addr/rw are the authoritative PHI0-high sample (valid for 6502
 * and DMA masters alike). Read serving keys on serve_en; register-write
 * side effects key on data_en; via_timer_clock keeps sss_en as its pure
 * 1 MHz cadence tick. */
wire slot_io_hit =
    card_enabled &&
    sss.slot_access &&
    (ab_read.addr[15:12] == 4'hC) &&
    (ab_read.addr[11] == 1'b0) &&
    (ab_read.addr[10:8] == slot_assign);
wire phasor_mode_hit =
    card_enabled &&
    (ab_read.addr[15:8] == 8'hC0) &&
    (ab_read.addr[7:4] == phasor_mode_nibble);
// AppleWin's Phasor decode intentionally preserves the real card's aliases:
// Mockingboard mode selects VIA-A over $Cn00-$Cn7F and VIA-B over $Cn80-$CnFF.
// Native mode interleaves VIA-A on addr bit 4 and VIA-B on addr bit 7.
wire via0_hit =
    slot_io_hit &&
    ((phasor_native && ab_read.addr[4]) ||
     (mockingboard_mode && !ab_read.addr[7]));
wire via1_hit =
    slot_io_hit &&
    ((phasor_native && ab_read.addr[7]) ||
     (mockingboard_mode && ab_read.addr[7]) ||
     echo_plus);
wire card_reset = !rstn || !card_enabled;
wire apple_reset = !ab_read.res;
wire via_reset = card_reset || apple_reset;
wire via_bus_clock = card_enabled && ab_read.data_en;
wire via_timer_clock = card_enabled && ab_read.sss_en;
// A real Phasor presents timer probes one 1 MHz tick later than a plain
// Mockingboard -- but only in Phasor native mode. In Mockingboard-compat
// mode a real Phasor's timers are indistinguishable from a plain
// Mockingboard: mb-audit T6522_C measures back-to-back T1C_l reads and
// expects exactly $05 (the classic Mockingboard-detection delta, which is
// also Skyfox's discovery probe), and a real Phasor passes it. Applying
// the extra tick in MB mode inflated that delta to $06 (mb-audit
// 11:0C:00 Expected:05 Actual:06) and broke MB-only software detection.
// It also let MB-mode ISR T1C_l reads (the standard IFR-clear) consume
// real counts. Mockingboard-only mode keeps the plain timing as well.
wire phasor_timer_read_extra_clock = !mockingboard_only && phasor_native;
wire via0_strobe = ab_read.data_en && via0_hit;
wire via1_strobe = ab_read.data_en && via1_hit;
wire ssi_write_region =
    slot_io_hit && ab_read.data_en && !ab_read.rw && ssi_visible_mode;
wire ssi_primary_write = ssi_write_region && ab_read.addr[6];
wire ssi_secondary_write = ssi_write_region && ab_read.addr[5];
wire ssi_native_read_region =
    slot_io_hit && ab_read.serve_en && ab_read.rw && phasor_native &&
    !ab_read.addr[4] && !ab_read.addr[7] && (ab_read.addr[6] || ab_read.addr[5]);
wire ssi_primary_read = ssi_native_read_region && ab_read.addr[6];
wire ssi_secondary_read = ssi_native_read_region && ab_read.addr[5];

logic via0_irq;
logic via1_irq;
logic [6:0] via0_ifr_set;
logic [6:0] via0_ifr_clr;
logic [6:0] via1_ifr_set;
logic [6:0] via1_ifr_clr;
logic [7:0] via0_data_out;
logic [7:0] via0_porta_in;
logic [7:0] via0_porta_out;
logic [7:0] via0_portb_out;
logic [7:0] via0_portb_bus;
logic [7:0] via0_pcr;
logic [7:0] via0_ddrb;
logic [7:0] via1_data_out;
logic [7:0] via1_porta_in;
logic [7:0] via1_porta_out;
logic [7:0] via1_portb_out;
logic [7:0] via1_portb_bus;
logic [7:0] via1_pcr;
logic [7:0] via1_ddrb;

logic ssi0_d7;
logic ssi1_d7;
logic ssi0_direct_irq;
logic ssi1_direct_irq;
logic signed [15:0] ssi0_audio;
logic signed [15:0] ssi1_audio;
logic signed [15:0] speech_audio_q;
logic signed [31:0] tone_l_low_q;
logic signed [31:0] tone_l_warm_lp_q;
logic signed [31:0] tone_l_mid_lp_q;
logic signed [31:0] tone_r_low_q;
logic signed [31:0] tone_r_warm_lp_q;
logic signed [31:0] tone_r_mid_lp_q;
logic signed [15:0] tone_base_l_q;
logic signed [15:0] tone_base_r_q;
logic signed [15:0] tone_apply_base_l_q;
logic signed [15:0] tone_apply_base_r_q;
logic signed [15:0] tone_bass_l_q;
logic signed [15:0] tone_warm_l_q;
logic signed [15:0] tone_mid_l_q;
logic signed [15:0] tone_treble_l_q;
logic signed [15:0] tone_bass_r_q;
logic signed [15:0] tone_warm_r_q;
logic signed [15:0] tone_mid_r_q;
logic signed [15:0] tone_treble_r_q;
logic signed [4:0] tone_bass_control_q;
logic signed [4:0] tone_mid_control_q;
logic signed [4:0] tone_treble_control_q;
logic signed [4:0] tone_warm_control_q;
logic signed [4:0] tone_volume_control_q;
logic signed [20:0] tone_base_ext_l_q;
logic signed [20:0] tone_base_ext_r_q;
logic signed [20:0] tone_bass_adjust_l_q;
logic signed [20:0] tone_warm_adjust_l_q;
logic signed [20:0] tone_warm_treble_adjust_l_q;
logic signed [20:0] tone_mid_adjust_l_q;
logic signed [20:0] tone_treble_adjust_l_q;
logic signed [20:0] tone_volume_adjust_l_q;
logic signed [20:0] tone_bass_adjust_r_q;
logic signed [20:0] tone_warm_adjust_r_q;
logic signed [20:0] tone_warm_treble_adjust_r_q;
logic signed [20:0] tone_mid_adjust_r_q;
logic signed [20:0] tone_treble_adjust_r_q;
logic signed [20:0] tone_volume_adjust_r_q;
logic signed [20:0] tone_shaped_l_q;
logic signed [20:0] tone_shaped_r_q;
logic signed [20:0] tone_warm_shaped_l_q;
logic signed [20:0] tone_warm_shaped_r_q;

wire ssi_read_drive = ssi_secondary_read ? 1'b0 : ssi_primary_read;
wire [7:0] ssi_read_data = {ssi_secondary_read ? ssi0_d7 : ssi1_d7, ab_read.data[6:0]};

logic [7:0] psg0_data_out;
logic [7:0] psg1_data_out;
logic [7:0] psg2_data_out;
logic [7:0] psg3_data_out;
logic [7:0] psg0_chan_a;
logic [7:0] psg0_chan_b;
logic [7:0] psg0_chan_c;
logic [7:0] psg1_chan_a;
logic [7:0] psg1_chan_b;
logic [7:0] psg1_chan_c;
logic [7:0] psg2_chan_a;
logic [7:0] psg2_chan_b;
logic [7:0] psg2_chan_c;
logic [7:0] psg3_chan_a;
logic [7:0] psg3_chan_b;
logic [7:0] psg3_chan_c;

logic [8:0] psg0_l_a_q;
logic [8:0] psg0_l_b_q;
logic [8:0] psg0_l_c_q;
logic [8:0] psg1_l_a_q;
logic [8:0] psg1_l_b_q;
logic [8:0] psg1_l_c_q;
logic [8:0] psg2_l_a_q;
logic [8:0] psg2_l_b_q;
logic [8:0] psg2_l_c_q;
logic [8:0] psg3_l_a_q;
logic [8:0] psg3_l_b_q;
logic [8:0] psg3_l_c_q;
logic [8:0] psg0_r_a_q;
logic [8:0] psg0_r_b_q;
logic [8:0] psg0_r_c_q;
logic [8:0] psg1_r_a_q;
logic [8:0] psg1_r_b_q;
logic [8:0] psg1_r_c_q;
logic [8:0] psg2_r_a_q;
logic [8:0] psg2_r_b_q;
logic [8:0] psg2_r_c_q;
logic [8:0] psg3_r_a_q;
logic [8:0] psg3_r_b_q;
logic [8:0] psg3_r_c_q;

logic [9:0] psg0_l_sum_q;
logic [9:0] psg1_l_sum_q;
logic [9:0] psg2_l_sum_q;
logic [9:0] psg3_l_sum_q;
logic [9:0] psg0_r_sum_q;
logic [9:0] psg1_r_sum_q;
logic [9:0] psg2_r_sum_q;
logic [9:0] psg3_r_sum_q;
logic [10:0] psg_mockingboard_l_mix_q;
logic [10:0] psg_mockingboard_r_mix_q;
logic [10:0] psg_echo_l_mix_q;
logic [10:0] psg_echo_r_mix_q;
logic [11:0] psg_phasor_l_mix_q;
logic [11:0] psg_phasor_r_mix_q;

logic via0_ay0_selected_q = 1'b0;
logic via0_ay1_selected_q = 1'b0;
logic via1_ay0_selected_q = 1'b0;
logic via1_ay1_selected_q = 1'b0;

wire via0_ay0_cs = !via0_portb_bus[4];
wire via0_ay1_cs = !via0_portb_bus[3];
wire via1_ay0_cs = !via1_portb_bus[4];
wire via1_ay1_cs = !via1_portb_bus[3];
wire via0_psg_reset_func = !via0_portb_bus[2];
wire via1_psg_reset_func = !via1_portb_bus[2];
wire via0_psg_latch_func = via0_portb_bus[2] && (via0_portb_bus[1:0] == 2'b11);
wire via1_psg_latch_func = via1_portb_bus[2] && (via1_portb_bus[1:0] == 2'b11);
wire via0_psg_read_write_func = via0_portb_bus[2] && (via0_portb_bus[1] ^ via0_portb_bus[0]);
wire via1_psg_read_write_func = via1_portb_bus[2] && (via1_portb_bus[1] ^ via1_portb_bus[0]);
wire via0_votrax_mode = (via0_pcr == 8'hB0);
wire via0_votrax_write =
    via0_strobe && !ab_read.rw && (ab_read.addr[3:0] == 4'h0) && via0_votrax_mode;
wire [7:0] via0_votrax_wdata = (ab_read.data & via0_ddrb) | (via0_ddrb ^ 8'hFF);
wire psg_clock = via_bus_clock || psg_ce_extra_q;

wire via0_ay0_drive_native =
    via0_psg_latch_func ? (via0_ay0_cs || via0_ay1_cs) :
    (via0_psg_read_write_func && via0_ay0_cs && via0_ay0_selected_q);
wire via0_ay1_drive_native =
    via0_psg_latch_func ? via0_ay1_cs :
    (via0_psg_read_write_func && (via0_ay0_cs || via0_ay1_cs) && via0_ay1_selected_q);
wire via1_ay0_drive_native =
    via1_psg_latch_func ? (via1_ay0_cs || via1_ay1_cs) :
    (via1_psg_read_write_func && via1_ay0_cs && via1_ay0_selected_q);
wire via1_ay1_drive_native =
    via1_psg_latch_func ? via1_ay1_cs :
    (via1_psg_read_write_func && (via1_ay0_cs || via1_ay1_cs) && via1_ay1_selected_q);

wire via0_ay0_drive =
    !via0_votrax_mode &&
    !echo_plus &&
    (!phasor_native || via0_ay0_drive_native);
wire via0_ay1_drive =
    !via0_votrax_mode &&
    !echo_plus &&
    phasor_native &&
    via0_ay1_drive_native;
wire via1_ay0_drive =
    phasor_native ? via1_ay0_drive_native :
    (!phasor_extended ||
     (via1_psg_latch_func ? (via1_ay0_cs || via1_ay1_cs) : via1_ay0_cs));
wire via1_ay1_drive =
    phasor_native ? via1_ay1_drive_native :
    (phasor_extended &&
    (via1_psg_latch_func ? via1_ay1_cs :
     (via1_psg_read_write_func && (via1_ay1_cs || (via1_ay0_cs && via1_ay1_selected_q)))));

function automatic logic [7:0] selected_psg_data(input logic primary_sel,
                                                 input logic [7:0] primary_data,
                                                 input logic secondary_sel,
                                                 input logic [7:0] secondary_data);
    begin
        if (primary_sel && secondary_sel) begin
            selected_psg_data = primary_data | secondary_data;
        end else if (primary_sel) begin
            selected_psg_data = primary_data;
        end else if (secondary_sel) begin
            selected_psg_data = secondary_data;
        end else begin
            selected_psg_data = 8'hFF;
        end
    end
endfunction

assign via0_porta_in = selected_psg_data(via0_ay0_drive, psg0_data_out,
                                         via0_ay1_drive, psg2_data_out);
assign via1_porta_in = selected_psg_data(via1_ay0_drive, psg1_data_out,
                                         via1_ay1_drive, psg3_data_out);

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
    logic [8:0] s;
    begin
        s = {1'b0, sample};
        case (gain)
            5'd0:    apply_pan_gain = 9'd0;
            5'd2:    apply_pan_gain = s >> 3;
            5'd4:    apply_pan_gain = s >> 2;
            5'd5:    apply_pan_gain = (s >> 2) + (s >> 4);
            5'd6:    apply_pan_gain = (s >> 2) + (s >> 3);
            5'd7:    apply_pan_gain = (s >> 2) + (s >> 3) + (s >> 4);
            5'd8:    apply_pan_gain = s >> 1;
            5'd9:    apply_pan_gain = (s >> 1) + (s >> 4);
            5'd10:   apply_pan_gain = (s >> 1) + (s >> 3);
            5'd11:   apply_pan_gain = (s >> 1) + (s >> 3) + (s >> 4);
            5'd12:   apply_pan_gain = (s >> 1) + (s >> 2);
            5'd14:   apply_pan_gain = (s >> 1) + (s >> 2) + (s >> 3);
            default: apply_pan_gain = s;
        endcase
    end
endfunction

function automatic signed [15:0] mix2_to_pcm(input logic [10:0] value);
    mix2_to_pcm = $signed({1'b0, value, 4'b0000});
endfunction

function automatic signed [15:0] mix4_to_pcm(input logic [11:0] value);
    if (value[11]) begin
        mix4_to_pcm = 16'sh7FFF;
    end else begin
        mix4_to_pcm = $signed({1'b0, value[10:0], 4'b0000});
    end
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

function automatic signed [15:0] mix_speech(input signed [15:0] base,
                                            input signed [15:0] speech);
    mix_speech = sat_add16(base, speech);
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

always_ff @(posedge clk) begin
    // mockingboard_only holds the mode at PH_MOCKINGBOARD every cycle, which
    // makes the $C0nX mode switch inert. Clearing the toggle restores the
    // normal Apple-bus mode switching.
    if (!rstn || !ab_read.res || !card_enabled || mockingboard_only) begin
        phasor_mode_q <= PH_MOCKINGBOARD;
    end else if (ab_read.serve_en && ab_read.cycle_valid && phasor_mode_hit) begin
        logic [2:0] next_mode;

        next_mode = phasor_mode_q;
        if (ab_read.addr[3]) begin
            next_mode = PH_MOCKINGBOARD;
        end
        phasor_mode_q <= next_mode | ab_read.addr[2:0];
    end
end

always_ff @(posedge clk) begin
    if (!rstn || !card_enabled) begin
        psg_ce_extra_q <= 1'b0;
    end else begin
        // The Phasor native mode doubles the PSG clock (one octave up vs
        // Mockingboard).
        psg_ce_extra_q <= phasor_native && via_bus_clock;
    end
end

always_ff @(posedge clk) begin
    if (via_reset || !phasor_extended) begin
        via0_ay0_selected_q <= 1'b0;
        via0_ay1_selected_q <= 1'b0;
        via1_ay0_selected_q <= 1'b0;
        via1_ay1_selected_q <= 1'b0;
    end else if (psg_clock) begin
        if (!echo_plus) begin
            if (via0_psg_reset_func) begin
                via0_ay0_selected_q <= 1'b0;
                via0_ay1_selected_q <= 1'b0;
            end else if (via0_psg_latch_func) begin
                if (via0_ay1_cs) begin
                    via0_ay0_selected_q <= 1'b1;
                    via0_ay1_selected_q <= 1'b1;
                end else if (via0_ay0_cs) begin
                    via0_ay0_selected_q <= 1'b1;
                    via0_ay1_selected_q <= 1'b0;
                end
            end
        end

        if (via1_psg_reset_func) begin
            via1_ay0_selected_q <= 1'b0;
            via1_ay1_selected_q <= 1'b0;
        end else if (via1_psg_latch_func) begin
            if (via1_ay1_cs) begin
                via1_ay0_selected_q <= 1'b1;
                via1_ay1_selected_q <= 1'b1;
            end else if (via1_ay0_cs) begin
                via1_ay0_selected_q <= 1'b1;
                via1_ay1_selected_q <= 1'b0;
            end
        end
    end
end

via6522 via0(
    .clk(clk),
    .reset(via_reset),
    .power_reset(card_reset),
    .we(!ab_read.rw),
    .porta_in(via0_porta_in),
    .portb_in(8'hFF),
    .ifr_set_ext(via0_ifr_set),
    .ifr_clr_ext(via0_ifr_clr),
    .ca1_in(1'b1),
    .ca2_in(1'b0),
    .cb1_in(1'b0),
    .cb2_in(1'b0),
    .strobe(via0_strobe),
    .slow_clock(via_timer_clock),
    .timer_read_extra_clock(phasor_timer_read_extra_clock),
    .addr(ab_read.addr[3:0]),
    .data_in(ab_read.data),
    .data_out(via0_data_out),
    .irq(via0_irq),
    .porta_out(via0_porta_out),
    .portb_out(via0_portb_out),
    .portb_bus(via0_portb_bus),
    .pcr_out(via0_pcr),
    .ddrb_out(via0_ddrb),
    .ca2_out(),
    .cb1_out(),
    .cb2_out()
);

via6522 via1(
    .clk(clk),
    .reset(via_reset),
    .power_reset(card_reset),
    .we(!ab_read.rw),
    .porta_in(via1_porta_in),
    .portb_in(8'hFF),
    .ifr_set_ext(via1_ifr_set),
    .ifr_clr_ext(via1_ifr_clr),
    .ca1_in(1'b1),
    .ca2_in(1'b0),
    .cb1_in(1'b0),
    .cb2_in(1'b0),
    .strobe(via1_strobe),
    .slow_clock(via_timer_clock),
    .timer_read_extra_clock(phasor_timer_read_extra_clock),
    .addr(ab_read.addr[3:0]),
    .data_in(ab_read.data),
    .data_out(via1_data_out),
    .irq(via1_irq),
    .porta_out(via1_porta_out),
    .portb_out(via1_portb_out),
    .portb_bus(via1_portb_bus),
    .pcr_out(via1_pcr),
    .ddrb_out(via1_ddrb),
    .ca2_out(),
    .cb1_out(),
    .cb2_out()
);

// AppleWin default Phasor population: subunit 0 has no SSI263 socket chip but
// does have the SC-01/Votrax path; subunit 1 is the main SSI263AP.
ssi263_voice #(
    .SSI263_TYPE(0),
    .HAS_SC01(1'b1)
) ssi263_secondary_i (
    .clk(clk),
    .rstn(rstn),
    .apple_res(ab_read.res),
    .card_enabled(card_enabled),
    .card_mode(phasor_mode_q),
    .audio_tick(audio_sample_tick),
    .ssi_write_strobe(ssi_secondary_write),
    .ssi_reg(ab_read.addr[2:0]),
    .ssi_wdata(ab_read.data),
    .ssi_d7(ssi0_d7),
    .votrax_write_strobe(via0_votrax_write),
    .votrax_wdata(via0_votrax_wdata),
    .via_pcr(via0_pcr),
    .via_ifr_set(via0_ifr_set),
    .via_ifr_clr(via0_ifr_clr),
    .audio(ssi0_audio),
    .direct_irq(ssi0_direct_irq)
);

ssi263_voice #(
    .SSI263_TYPE(2),
    .HAS_SC01(1'b0)
) ssi263_primary_i (
    .clk(clk),
    .rstn(rstn),
    .apple_res(ab_read.res),
    .card_enabled(card_enabled),
    .card_mode(phasor_mode_q),
    .audio_tick(audio_sample_tick),
    .ssi_write_strobe(ssi_primary_write),
    .ssi_reg(ab_read.addr[2:0]),
    .ssi_wdata(ab_read.data),
    .ssi_d7(ssi1_d7),
    .votrax_write_strobe(1'b0),
    .votrax_wdata(8'h00),
    .via_pcr(via1_pcr),
    .via_ifr_set(via1_ifr_set),
    .via_ifr_clr(via1_ifr_clr),
    .audio(ssi1_audio),
    .direct_irq(ssi1_direct_irq)
);

YM2149 psg0(
    .CLK(clk),
    .CE(psg_clock),
    .RESET(via_reset || (!via0_votrax_mode && !via0_portb_bus[2])),
    .BDIR(via0_ay0_drive ? via0_portb_bus[1] : 1'b0),
    .BC(via0_ay0_drive ? via0_portb_bus[0] : 1'b0),
    .DI(via0_porta_out),
    .DO(psg0_data_out),
    .CHANNEL_A(psg0_chan_a),
    .CHANNEL_B(psg0_chan_b),
    .CHANNEL_C(psg0_chan_c),
    .SEL(1'b0),
    .MODE(psg_volume_mode_ay8913),
    .ACTIVE(),
    .IOA_in(8'h00),
    .IOA_out(),
    .IOB_in(8'h00),
    .IOB_out()
);

YM2149 psg1(
    .CLK(clk),
    .CE(psg_clock),
    .RESET(via_reset || !via1_portb_bus[2]),
    .BDIR(via1_ay0_drive ? via1_portb_bus[1] : 1'b0),
    .BC(via1_ay0_drive ? via1_portb_bus[0] : 1'b0),
    .DI(via1_porta_out),
    .DO(psg1_data_out),
    .CHANNEL_A(psg1_chan_a),
    .CHANNEL_B(psg1_chan_b),
    .CHANNEL_C(psg1_chan_c),
    .SEL(1'b0),
    .MODE(psg_volume_mode_ay8913),
    .ACTIVE(),
    .IOA_in(8'h00),
    .IOA_out(),
    .IOB_in(8'h00),
    .IOB_out()
);

YM2149 psg2(
    .CLK(clk),
    .CE(psg_clock),
    .RESET(via_reset || (!via0_votrax_mode && !via0_portb_bus[2])),
    .BDIR(via0_ay1_drive ? via0_portb_bus[1] : 1'b0),
    .BC(via0_ay1_drive ? via0_portb_bus[0] : 1'b0),
    .DI(via0_porta_out),
    .DO(psg2_data_out),
    .CHANNEL_A(psg2_chan_a),
    .CHANNEL_B(psg2_chan_b),
    .CHANNEL_C(psg2_chan_c),
    .SEL(1'b0),
    .MODE(psg_volume_mode_ay8913),
    .ACTIVE(),
    .IOA_in(8'h00),
    .IOA_out(),
    .IOB_in(8'h00),
    .IOB_out()
);

YM2149 psg3(
    .CLK(clk),
    .CE(psg_clock),
    .RESET(via_reset || !via1_portb_bus[2]),
    .BDIR(via1_ay1_drive ? via1_portb_bus[1] : 1'b0),
    .BC(via1_ay1_drive ? via1_portb_bus[0] : 1'b0),
    .DI(via1_porta_out),
    .DO(psg3_data_out),
    .CHANNEL_A(psg3_chan_a),
    .CHANNEL_B(psg3_chan_b),
    .CHANNEL_C(psg3_chan_c),
    .SEL(1'b0),
    .MODE(psg_volume_mode_ay8913),
    .ACTIVE(),
    .IOA_in(8'h00),
    .IOA_out(),
    .IOB_in(8'h00),
    .IOB_out()
);

always_ff @(posedge clk) begin
    if (!rstn || !card_enabled) begin
        psg0_l_a_q <= '0;
        psg0_l_b_q <= '0;
        psg0_l_c_q <= '0;
        psg1_l_a_q <= '0;
        psg1_l_b_q <= '0;
        psg1_l_c_q <= '0;
        psg2_l_a_q <= '0;
        psg2_l_b_q <= '0;
        psg2_l_c_q <= '0;
        psg3_l_a_q <= '0;
        psg3_l_b_q <= '0;
        psg3_l_c_q <= '0;
        psg0_r_a_q <= '0;
        psg0_r_b_q <= '0;
        psg0_r_c_q <= '0;
        psg1_r_a_q <= '0;
        psg1_r_b_q <= '0;
        psg1_r_c_q <= '0;
        psg2_r_a_q <= '0;
        psg2_r_b_q <= '0;
        psg2_r_c_q <= '0;
        psg3_r_a_q <= '0;
        psg3_r_b_q <= '0;
        psg3_r_c_q <= '0;
        psg0_l_sum_q <= '0;
        psg1_l_sum_q <= '0;
        psg2_l_sum_q <= '0;
        psg3_l_sum_q <= '0;
        psg0_r_sum_q <= '0;
        psg1_r_sum_q <= '0;
        psg2_r_sum_q <= '0;
        psg3_r_sum_q <= '0;
        psg_mockingboard_l_mix_q <= '0;
        psg_mockingboard_r_mix_q <= '0;
        psg_echo_l_mix_q <= '0;
        psg_echo_r_mix_q <= '0;
        psg_phasor_l_mix_q <= '0;
        psg_phasor_r_mix_q <= '0;
        speech_audio_q <= '0;
        tone_l_low_q <= '0;
        tone_l_warm_lp_q <= '0;
        tone_l_mid_lp_q <= '0;
        tone_r_low_q <= '0;
        tone_r_warm_lp_q <= '0;
        tone_r_mid_lp_q <= '0;
        tone_base_l_q <= '0;
        tone_base_r_q <= '0;
        tone_apply_base_l_q <= '0;
        tone_apply_base_r_q <= '0;
        tone_bass_l_q <= '0;
        tone_warm_l_q <= '0;
        tone_mid_l_q <= '0;
        tone_treble_l_q <= '0;
        tone_bass_r_q <= '0;
        tone_warm_r_q <= '0;
        tone_mid_r_q <= '0;
        tone_treble_r_q <= '0;
        tone_bass_control_q <= '0;
        tone_mid_control_q <= '0;
        tone_treble_control_q <= '0;
        tone_warm_control_q <= '0;
        tone_volume_control_q <= '0;
        tone_base_ext_l_q <= '0;
        tone_base_ext_r_q <= '0;
        tone_bass_adjust_l_q <= '0;
        tone_warm_adjust_l_q <= '0;
        tone_warm_treble_adjust_l_q <= '0;
        tone_mid_adjust_l_q <= '0;
        tone_treble_adjust_l_q <= '0;
        tone_volume_adjust_l_q <= '0;
        tone_bass_adjust_r_q <= '0;
        tone_warm_adjust_r_q <= '0;
        tone_warm_treble_adjust_r_q <= '0;
        tone_mid_adjust_r_q <= '0;
        tone_treble_adjust_r_q <= '0;
        tone_volume_adjust_r_q <= '0;
        tone_shaped_l_q <= '0;
        tone_shaped_r_q <= '0;
        tone_warm_shaped_l_q <= '0;
        tone_warm_shaped_r_q <= '0;
        audio_l <= '0;
        audio_r <= '0;
    end else begin
        psg0_l_a_q <= apply_pan_gain(psg0_chan_a, pan_gain_l(pan[3:0]));
        psg0_l_b_q <= apply_pan_gain(psg0_chan_b, pan_gain_l(pan[7:4]));
        psg0_l_c_q <= apply_pan_gain(psg0_chan_c, pan_gain_l(pan[11:8]));
        psg1_l_a_q <= apply_pan_gain(psg1_chan_a, pan_gain_l(pan[15:12]));
        psg1_l_b_q <= apply_pan_gain(psg1_chan_b, pan_gain_l(pan[19:16]));
        psg1_l_c_q <= apply_pan_gain(psg1_chan_c, pan_gain_l(pan[23:20]));
        psg2_l_a_q <= apply_pan_gain(psg2_chan_a, pan_gain_l(pan[27:24]));
        psg2_l_b_q <= apply_pan_gain(psg2_chan_b, pan_gain_l(pan[31:28]));
        psg2_l_c_q <= apply_pan_gain(psg2_chan_c, pan_gain_l(pan[35:32]));
        psg3_l_a_q <= apply_pan_gain(psg3_chan_a, pan_gain_l(pan[39:36]));
        psg3_l_b_q <= apply_pan_gain(psg3_chan_b, pan_gain_l(pan[43:40]));
        psg3_l_c_q <= apply_pan_gain(psg3_chan_c, pan_gain_l(pan[47:44]));
        psg0_r_a_q <= apply_pan_gain(psg0_chan_a, pan_gain_r(pan[3:0]));
        psg0_r_b_q <= apply_pan_gain(psg0_chan_b, pan_gain_r(pan[7:4]));
        psg0_r_c_q <= apply_pan_gain(psg0_chan_c, pan_gain_r(pan[11:8]));
        psg1_r_a_q <= apply_pan_gain(psg1_chan_a, pan_gain_r(pan[15:12]));
        psg1_r_b_q <= apply_pan_gain(psg1_chan_b, pan_gain_r(pan[19:16]));
        psg1_r_c_q <= apply_pan_gain(psg1_chan_c, pan_gain_r(pan[23:20]));
        psg2_r_a_q <= apply_pan_gain(psg2_chan_a, pan_gain_r(pan[27:24]));
        psg2_r_b_q <= apply_pan_gain(psg2_chan_b, pan_gain_r(pan[31:28]));
        psg2_r_c_q <= apply_pan_gain(psg2_chan_c, pan_gain_r(pan[35:32]));
        psg3_r_a_q <= apply_pan_gain(psg3_chan_a, pan_gain_r(pan[39:36]));
        psg3_r_b_q <= apply_pan_gain(psg3_chan_b, pan_gain_r(pan[43:40]));
        psg3_r_c_q <= apply_pan_gain(psg3_chan_c, pan_gain_r(pan[47:44]));

        psg0_l_sum_q <= sum3(psg0_l_a_q, psg0_l_b_q, psg0_l_c_q);
        psg1_l_sum_q <= sum3(psg1_l_a_q, psg1_l_b_q, psg1_l_c_q);
        psg2_l_sum_q <= sum3(psg2_l_a_q, psg2_l_b_q, psg2_l_c_q);
        psg3_l_sum_q <= sum3(psg3_l_a_q, psg3_l_b_q, psg3_l_c_q);
        psg0_r_sum_q <= sum3(psg0_r_a_q, psg0_r_b_q, psg0_r_c_q);
        psg1_r_sum_q <= sum3(psg1_r_a_q, psg1_r_b_q, psg1_r_c_q);
        psg2_r_sum_q <= sum3(psg2_r_a_q, psg2_r_b_q, psg2_r_c_q);
        psg3_r_sum_q <= sum3(psg3_r_a_q, psg3_r_b_q, psg3_r_c_q);
        psg_mockingboard_l_mix_q <= sum2_10(psg0_l_sum_q, psg1_l_sum_q);
        psg_mockingboard_r_mix_q <= sum2_10(psg0_r_sum_q, psg1_r_sum_q);
        psg_echo_l_mix_q <= sum2_10(psg1_l_sum_q, psg3_l_sum_q);
        psg_echo_r_mix_q <= sum2_10(psg1_r_sum_q, psg3_r_sum_q);
        psg_phasor_l_mix_q <= sum4_10(psg0_l_sum_q,
                                      psg1_l_sum_q,
                                      psg2_l_sum_q,
                                      psg3_l_sum_q);
        psg_phasor_r_mix_q <= sum4_10(psg0_r_sum_q,
                                      psg1_r_sum_q,
                                      psg2_r_sum_q,
                                      psg3_r_sum_q);
        speech_audio_q <= sat_add16(ssi0_audio, ssi1_audio);

        begin : final_audio_mix
            logic signed [15:0] base_l_next;
            logic signed [15:0] base_r_next;
            logic signed [31:0] l_low_next;
            logic signed [31:0] l_warm_lp_next;
            logic signed [31:0] l_mid_lp_next;
            logic signed [31:0] r_low_next;
            logic signed [31:0] r_warm_lp_next;
            logic signed [31:0] r_mid_lp_next;
            logic signed [15:0] l_low;
            logic signed [15:0] l_warm_lp;
            logic signed [15:0] l_mid_lp;
            logic signed [15:0] r_low;
            logic signed [15:0] r_warm_lp;
            logic signed [15:0] r_mid_lp;
            logic signed [15:0] l_warm_band;
            logic signed [15:0] l_mid_band;
            logic signed [15:0] l_treble_band;
            logic signed [15:0] r_warm_band;
            logic signed [15:0] r_mid_band;
            logic signed [15:0] r_treble_band;

            if (phasor_native) begin
                base_l_next = mix_speech(
                    mix4_to_pcm(psg_phasor_l_mix_q),
                    speech_audio_q);
                base_r_next = mix_speech(
                    mix4_to_pcm(psg_phasor_r_mix_q),
                    speech_audio_q);
            end else if (echo_plus) begin
                base_l_next = mix_speech(
                    mix2_to_pcm(psg_echo_l_mix_q),
                    speech_audio_q);
                base_r_next = mix_speech(
                    mix2_to_pcm(psg_echo_r_mix_q),
                    speech_audio_q);
            end else begin
                base_l_next = mix_speech(
                    mix2_to_pcm(psg_mockingboard_l_mix_q),
                    speech_audio_q);
                base_r_next = mix_speech(
                    mix2_to_pcm(psg_mockingboard_r_mix_q),
                    speech_audio_q);
            end

            tone_base_l_q <= base_l_next;
            tone_base_r_q <= base_r_next;

            l_low_next = tone_l_low_q +
                ((pcm_to_tone_fp(tone_base_l_q) - tone_l_low_q) >>> 16);
            l_warm_lp_next = tone_l_warm_lp_q +
                ((pcm_to_tone_fp(tone_base_l_q) - tone_l_warm_lp_q) >>> 14);
            l_mid_lp_next = tone_l_mid_lp_q +
                ((pcm_to_tone_fp(tone_base_l_q) - tone_l_mid_lp_q) >>> 13);
            r_low_next = tone_r_low_q +
                ((pcm_to_tone_fp(tone_base_r_q) - tone_r_low_q) >>> 16);
            r_warm_lp_next = tone_r_warm_lp_q +
                ((pcm_to_tone_fp(tone_base_r_q) - tone_r_warm_lp_q) >>> 14);
            r_mid_lp_next = tone_r_mid_lp_q +
                ((pcm_to_tone_fp(tone_base_r_q) - tone_r_mid_lp_q) >>> 13);

            tone_l_low_q <= l_low_next;
            tone_l_warm_lp_q <= l_warm_lp_next;
            tone_l_mid_lp_q <= l_mid_lp_next;
            tone_r_low_q <= r_low_next;
            tone_r_warm_lp_q <= r_warm_lp_next;
            tone_r_mid_lp_q <= r_mid_lp_next;

            l_low = tone_fp_to_pcm(tone_l_low_q);
            l_warm_lp = tone_fp_to_pcm(tone_l_warm_lp_q);
            l_mid_lp = tone_fp_to_pcm(tone_l_mid_lp_q);
            r_low = tone_fp_to_pcm(tone_r_low_q);
            r_warm_lp = tone_fp_to_pcm(tone_r_warm_lp_q);
            r_mid_lp = tone_fp_to_pcm(tone_r_mid_lp_q);
            l_warm_band = sat16_from21({{5{l_warm_lp[15]}}, l_warm_lp} -
                                       {{5{l_low[15]}}, l_low});
            l_mid_band = sat16_from21({{5{l_mid_lp[15]}}, l_mid_lp} -
                                      {{5{l_low[15]}}, l_low});
            l_treble_band = sat16_from21({{5{tone_base_l_q[15]}}, tone_base_l_q} -
                                         {{5{l_mid_lp[15]}}, l_mid_lp});
            r_warm_band = sat16_from21({{5{r_warm_lp[15]}}, r_warm_lp} -
                                       {{5{r_low[15]}}, r_low});
            r_mid_band = sat16_from21({{5{r_mid_lp[15]}}, r_mid_lp} -
                                      {{5{r_low[15]}}, r_low});
            r_treble_band = sat16_from21({{5{tone_base_r_q[15]}}, tone_base_r_q} -
                                         {{5{r_mid_lp[15]}}, r_mid_lp});
            tone_apply_base_l_q <= tone_base_l_q;
            tone_apply_base_r_q <= tone_base_r_q;
            tone_bass_l_q <= l_low;
            tone_warm_l_q <= l_warm_band;
            tone_mid_l_q <= l_mid_band;
            tone_treble_l_q <= l_treble_band;
            tone_bass_r_q <= r_low;
            tone_warm_r_q <= r_warm_band;
            tone_mid_r_q <= r_mid_band;
            tone_treble_r_q <= r_treble_band;

            tone_bass_control_q <= clamp_audio_control(audio_control[4:0]);
            tone_mid_control_q <= clamp_audio_control(audio_control[9:5]);
            tone_treble_control_q <= clamp_audio_control(audio_control[14:10]);
            tone_warm_control_q <= clamp_audio_control(audio_control[19:15]);
            tone_volume_control_q <= clamp_audio_control(audio_control[24:20]);

            tone_base_ext_l_q <= {{5{tone_apply_base_l_q[15]}}, tone_apply_base_l_q};
            tone_base_ext_r_q <= {{5{tone_apply_base_r_q[15]}}, tone_apply_base_r_q};
            tone_bass_adjust_l_q <= audio_control_adjust(tone_bass_l_q, tone_bass_control_q);
            tone_warm_adjust_l_q <= audio_control_adjust(tone_warm_l_q, tone_warm_control_q);
            tone_warm_treble_adjust_l_q <= warmth_treble_adjust(tone_treble_l_q, tone_warm_control_q);
            tone_mid_adjust_l_q <= audio_control_adjust(tone_mid_l_q, tone_mid_control_q);
            tone_treble_adjust_l_q <= audio_control_adjust(tone_treble_l_q, tone_treble_control_q);
            tone_volume_adjust_l_q <= audio_control_adjust(tone_apply_base_l_q, tone_volume_control_q);
            tone_bass_adjust_r_q <= audio_control_adjust(tone_bass_r_q, tone_bass_control_q);
            tone_warm_adjust_r_q <= audio_control_adjust(tone_warm_r_q, tone_warm_control_q);
            tone_warm_treble_adjust_r_q <= warmth_treble_adjust(tone_treble_r_q, tone_warm_control_q);
            tone_mid_adjust_r_q <= audio_control_adjust(tone_mid_r_q, tone_mid_control_q);
            tone_treble_adjust_r_q <= audio_control_adjust(tone_treble_r_q, tone_treble_control_q);
            tone_volume_adjust_r_q <= audio_control_adjust(tone_apply_base_r_q, tone_volume_control_q);

            tone_shaped_l_q <= tone_base_ext_l_q +
                               tone_bass_adjust_l_q +
                               tone_warm_adjust_l_q +
                               tone_mid_adjust_l_q +
                               tone_treble_adjust_l_q +
                               tone_warm_treble_adjust_l_q +
                               tone_volume_adjust_l_q;
            tone_shaped_r_q <= tone_base_ext_r_q +
                               tone_bass_adjust_r_q +
                               tone_warm_adjust_r_q +
                               tone_mid_adjust_r_q +
                               tone_treble_adjust_r_q +
                               tone_warm_treble_adjust_r_q +
                               tone_volume_adjust_r_q;

            tone_warm_shaped_l_q <= warm_shape_from21(tone_shaped_l_q, tone_warm_control_q);
            tone_warm_shaped_r_q <= warm_shape_from21(tone_shaped_r_q, tone_warm_control_q);
            audio_l <= sat16_from21(tone_warm_shaped_l_q);
            audio_r <= sat16_from21(tone_warm_shaped_r_q);
        end
    end
end

assign ab_write = ab_write_q;

always @(posedge clk) begin
    if (!rstn) begin
        ab_write_q <= '0;
    end else begin
        ab_write_q.assert_inh <= 1'b0;
        ab_write_q.assert_res <= 1'b0;
        ab_write_q.assert_irq <= card_enabled && ab_read.res &&
                                  (via0_irq | via1_irq | ssi0_direct_irq | ssi1_direct_irq);
        ab_write_q.assert_rdy <= 1'b0;
        ab_write_q.assert_nmi <= 1'b0;
        ab_write_q.assert_dma <= 1'b0;
        ab_write_q.wr_addr <= 16'h0000;
        ab_write_q.wr_rw <= 1'b0;
        ab_write_q.wr_addr_rw_en <= 1'b0;

        if (!card_enabled || ab_read.data_en || ab_read.addr_en) begin
            ab_write_q.wr_data <= 8'h00;
            ab_write_q.wr_data_en <= 1'b0;
        end

        if (ab_read.serve_en && ab_read.rw && ssi_read_drive) begin
            ab_write_q.wr_data <= ssi_read_data;
            ab_write_q.wr_data_en <= 1'b1;
        end else if (ab_read.serve_en && ab_read.rw && (via0_hit || via1_hit)) begin
            /* Keyed on serve_en (the authoritative PHI0-high sample);
             * registers wr_data before the TAP_DATA_EMIT drive window. */
            if (via0_hit && via1_hit) begin
                ab_write_q.wr_data <= via0_data_out | via1_data_out;
            end else if (via0_hit) begin
                ab_write_q.wr_data <= via0_data_out;
            end else begin
                ab_write_q.wr_data <= via1_data_out;
            end
            ab_write_q.wr_data_en <= 1'b1;
        end
    end
end

endmodule
