`timescale 1ns / 1ps

// Applied Engineering Phasor-compatible sound card.
//
// The card resets into Mockingboard-compatible mode: two 6522 VIAs, each
// driving one AY-3-8913-compatible PSG. Phasor native mode is selected through
// the Phasor C0nX mode switch and adds a second AY behind each VIA. In native
// mode VIA ORB bit 4 selects the primary AY and bit 3 selects the secondary AY;
// both chip selects are active-low, matching the Phasor GAL behavior documented
// by AppleWin. Both speech sockets expose SSI263AP bus behavior and use the
// native SSI-263 engine for audio.
module mockingboard(
    input clk,
    input rstn,
    input logic apple_q3_raw,
    input logic standalone,
    input logic iigs_data_tick,  // Late PHI0 pulse, independent of GS bus selection.
    // The card reads ab_read.data only at data_en. An SSI-263 read drives
    // D7 only, so D6-D0 come from the live bus byte at serve_en.
    input globals::AppleBus_read ab_read,
    input logic [7:0] serve_data,
    input globals::SoftSwitchState sss,
    input logic [2:0] slot_assign,
    input logic [47:0] pan,
    input logic [7:0] ssi_pan,
    input logic [31:0] audio_control,
    input logic audio_sample_tick,
    output globals::AppleBus_write ab_write,
    output logic signed [15:0] audio_l,
    output logic signed [15:0] audio_r,
    // Debug taps for the speech-completion IRQ chain (freeze diagnosis):
    // combined across both SSI-263 voices.
    output logic dbg_ssi_irq,
    output logic dbg_ssi_backend_done,
    output logic dbg_ssi_enable_ints,
    // Fifth slot of the sound chip bank, for the SuperSprite card.
    input  logic       guest_psg_ce = 1'b0,
    input  logic       guest_psg_reset = 1'b1,
    input  logic       guest_psg_bdir = 1'b0,
    input  logic       guest_psg_bc = 1'b0,
    input  logic [7:0] guest_psg_di = 8'h00,
    output logic [7:0] guest_psg_dout,
    output logic [7:0] guest_psg_channel [3]
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
 * side effects key on data_en. VIA timers use the continuous PHI0 tick. */
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
// The AY cores and native-mode extra tick need elapsed PHI0 time.
// The GS slot gate removes data_en on unrelated and fast accesses.
// Keep the continuous GS pulse at the same late phase as legacy data_en.
wire via_bus_clock = card_enabled && (ab_read.data_en || iigs_data_tick);
wire via_timer_clock = card_enabled && ab_read.phi0_tick;
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
logic ssi0_backend_done;
logic ssi1_backend_done;
logic ssi0_enable_ints;
logic ssi1_enable_ints;
assign dbg_ssi_irq          = ssi0_direct_irq | ssi1_direct_irq;
assign dbg_ssi_backend_done = ssi0_backend_done | ssi1_backend_done;
assign dbg_ssi_enable_ints  = ssi0_enable_ints | ssi1_enable_ints;
logic signed [15:0] ssi0_audio;
logic signed [15:0] ssi1_audio;
logic ssi0_audio_valid, ssi1_audio_valid;
logic signed [17:0] ssi_mix_l, ssi_mix_r;
logic ssi_xck_ce;

wire ssi_read_drive = ssi_secondary_read || ssi_primary_read;
wire [7:0] ssi_read_data = {ssi_secondary_read ? ssi0_d7 : ssi1_d7, serve_data[6:0]};

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
    !echo_plus &&
    (!phasor_native || via0_ay0_drive_native);
wire via0_ay1_drive =
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

// Both physical Phasor speech sockets contain SSI263AP devices. They use the
// same native SSI263 synthesis path, but expose no SC-01/Votrax device.
ssi263_xck_ce ssi_xck_ce_i (
    .clk(clk),
    .rstn(rstn),
    .q3_raw(apple_q3_raw),
    .standalone(standalone),
    .phi0(ab_read.phi0),
    .xck_ce(ssi_xck_ce)
);

ssi263_voice #(
    .SSI263_TYPE(2),
    .HAS_SC01(1'b0)
) ssi263_secondary_i (
    .clk(clk),
    .rstn(rstn),
    .apple_res(ab_read.res),
    .card_enabled(card_enabled),
    .card_mode(phasor_mode_q),
    .audio_tick(audio_sample_tick),
    .xck_ce(ssi_xck_ce),
    .ssi_write_strobe(ssi_secondary_write),
    .ssi_reg(ab_read.addr[2:0]),
    .ssi_wdata(ab_read.data),
    .ssi_d7(ssi0_d7),
    .votrax_write_strobe(1'b0),
    .votrax_wdata(8'h00),
    .via_pcr(via0_pcr),
    .via_ifr_set(via0_ifr_set),
    .via_ifr_clr(via0_ifr_clr),
    .audio(ssi0_audio),
    .audio_valid(ssi0_audio_valid),
    .direct_irq(ssi0_direct_irq),
    .dbg_backend_done(ssi0_backend_done),
    .dbg_enable_ints(ssi0_enable_ints)
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
    .xck_ce(ssi_xck_ce),
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
    .audio_valid(ssi1_audio_valid),
    .direct_irq(ssi1_direct_irq),
    .dbg_backend_done(ssi1_backend_done),
    .dbg_enable_ints(ssi1_enable_ints)
);

// Signed audio_control[31:27] gives -5..+5 dB; reset/menu default is 0 dB.
// The mixer uses per-engine valid strobes, independent of native XCK jobs.
ssi263_stereo_mixer ssi_mixer_i (
    .clk(clk), .rstn(rstn && card_enabled),
    .audio_tick(audio_sample_tick),
    .sample0(ssi0_audio), .sample1(ssi1_audio),
    .sample0_valid(ssi0_audio_valid), .sample1_valid(ssi1_audio_valid),
    .volume_db($signed(audio_control[31:27])), .pan(ssi_pan),
    .speech_l(ssi_mix_l), .speech_r(ssi_mix_r), .mix_valid()
);

// The four sound chips, and the one of the SuperSprite card, share one copy
// of the chip logic.
ym2149_bank #(.N(5)) psg_bank_i (
    .clk(clk),
    .ce({guest_psg_ce, {4{psg_clock}}}),
    .reset({guest_psg_reset,
            via_reset || !via1_portb_bus[2], via_reset || !via0_portb_bus[2],
            via_reset || !via1_portb_bus[2], via_reset || !via0_portb_bus[2]}),
    .bdir({guest_psg_bdir,
           via1_ay1_drive && via1_portb_bus[1], via0_ay1_drive && via0_portb_bus[1],
           via1_ay0_drive && via1_portb_bus[1], via0_ay0_drive && via0_portb_bus[1]}),
    .bc({guest_psg_bc,
         via1_ay1_drive && via1_portb_bus[0], via0_ay1_drive && via0_portb_bus[0],
         via1_ay0_drive && via1_portb_bus[0], via0_ay0_drive && via0_portb_bus[0]}),
    .mode({1'b0, {4{psg_volume_mode_ay8913}}}),
    .di('{via0_porta_out, via1_porta_out, via0_porta_out, via1_porta_out,
          guest_psg_di}),
    .dout('{psg0_data_out, psg1_data_out, psg2_data_out, psg3_data_out,
            guest_psg_dout}),
    .channel('{psg0_chan_a, psg0_chan_b, psg0_chan_c,
               psg1_chan_a, psg1_chan_b, psg1_chan_c,
               psg2_chan_a, psg2_chan_b, psg2_chan_c,
               psg3_chan_a, psg3_chan_b, psg3_chan_c,
               guest_psg_channel[0], guest_psg_channel[1],
               guest_psg_channel[2]})
);

mockingboard_audio_mix audio_mix_i (
    .clk(clk),
    .rstn(rstn),
    .card_enabled(card_enabled),
    .phasor_native(phasor_native),
    .echo_plus(echo_plus),
    .channel('{psg0_chan_a, psg0_chan_b, psg0_chan_c,
               psg1_chan_a, psg1_chan_b, psg1_chan_c,
               psg2_chan_a, psg2_chan_b, psg2_chan_c,
               psg3_chan_a, psg3_chan_b, psg3_chan_c}),
    .pan(pan),
    .audio_control(audio_control),
    .ssi_mix_l(ssi_mix_l),
    .ssi_mix_r(ssi_mix_r),
    .audio_l(audio_l),
    .audio_r(audio_r)
);


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
