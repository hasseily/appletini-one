`timescale 1ns / 1ps

import ssi263_formant_pkg::*;

// Apple-visible SSI263/SC-01 wrapper.
//
// This module preserves the Phasor/Mockingboard register, D7, IRQ, and reset
// behavior. Audio generation is handled by the formant backend; bus-visible
// SSI263 semantics stay isolated here.
module ssi263_bus_wrapper #(
    // AppleWin types: 0=empty, 1=SSI263P, 2=SSI263AP.
    parameter int unsigned SSI263_TYPE = 2,
    parameter bit HAS_SC01 = 1'b0
) (
    input  logic               clk,
    input  logic               rstn,
    input  logic               apple_res,
    input  logic               card_enabled,
    input  logic [2:0]         card_mode,
    input  logic               audio_tick,

    input  logic               ssi_write_strobe,
    input  logic [2:0]         ssi_reg,
    input  logic [7:0]         ssi_wdata,
    output logic               ssi_d7,

    input  logic               votrax_write_strobe,
    input  logic [7:0]         votrax_wdata,

    input  logic [7:0]         via_pcr,
    output logic [6:0]         via_ifr_set,
    output logic [6:0]         via_ifr_clr,

    output logic signed [15:0] audio,
    output logic               direct_irq
);

    localparam logic [2:0] PH_MOCKINGBOARD = 3'd0;
    localparam logic [2:0] PH_PHASOR       = 3'd5;

    localparam int unsigned SSI263_EMPTY = 0;
    localparam int unsigned SSI263_P     = 1;
    localparam int unsigned SSI263_AP    = 2;

    localparam logic [2:0] SSI_DURPHON = 3'd0;
    localparam logic [2:0] SSI_INFLECT = 3'd1;
    localparam logic [2:0] SSI_RATEINF = 3'd2;
    localparam logic [2:0] SSI_CTTRAMP = 3'd3;

    localparam logic [7:0] MODE_PHONEME_TRANSITIONED_INFLECTION = 8'hC0;
    localparam logic [7:0] MODE_IRQ_DISABLED                    = 8'h00;
    localparam logic [7:0] CONTROL_MASK                         = 8'h80;
    localparam logic [7:0] FILTER_FREQ_SILENCE                  = 8'hFF;

    localparam int unsigned IFR_CA1_SSI263 = 1;
    localparam int unsigned IFR_CB1_VOTRAX = 4;

    logic [7:0] duration_phoneme_q;
    logic [7:0] inflection_q;
    logic [7:0] rate_inflection_q;
    logic [7:0] ctrl_art_amp_q;
    logic [7:0] filter_freq_q;

    logic [1:0] current_function_q;
    logic       current_enable_ints_q;
    logic       d7_q;
    logic       active_is_votrax_q;

    logic [2:0] card_mode_prev_q;
    logic       direct_irq_q;

    logic       backend_start_q;
    logic [5:0] backend_phoneme_q;
    logic [5:0] backend_sc01_phone_q;
    logic       backend_votrax_q;
    logic       backend_warm_reset;
    logic       formant_backend_start;
    logic       formant_backend_reset;
    logic       formant_backend_done;
    logic       backend_done;
    logic signed [15:0] formant_audio;

    assign ssi_d7 = d7_q;
    assign direct_irq = direct_irq_q;
    assign audio = formant_audio;
    assign formant_backend_start = backend_start_q;
    assign formant_backend_reset = backend_warm_reset;
    assign backend_done = formant_backend_done;

    assign backend_warm_reset =
        !apple_res && (SSI263_TYPE != SSI263_P) && (SSI263_TYPE != SSI263_EMPTY);

    function automatic logic [5:0] votrax_to_ssi263(input logic [5:0] phoneme);
        case (phoneme)
            6'h00: votrax_to_ssi263 = 6'h02;
            6'h01: votrax_to_ssi263 = 6'h0A;
            6'h02: votrax_to_ssi263 = 6'h0B;
            6'h03: votrax_to_ssi263 = 6'h00;
            6'h04: votrax_to_ssi263 = 6'h28;
            6'h05: votrax_to_ssi263 = 6'h08;
            6'h06: votrax_to_ssi263 = 6'h08;
            6'h07: votrax_to_ssi263 = 6'h2F;
            6'h08: votrax_to_ssi263 = 6'h0E;
            6'h09: votrax_to_ssi263 = 6'h07;
            6'h0A: votrax_to_ssi263 = 6'h07;
            6'h0B: votrax_to_ssi263 = 6'h07;
            6'h0C: votrax_to_ssi263 = 6'h37;
            6'h0D: votrax_to_ssi263 = 6'h38;
            6'h0E: votrax_to_ssi263 = 6'h24;
            6'h0F: votrax_to_ssi263 = 6'h33;
            6'h10: votrax_to_ssi263 = 6'h32;
            6'h11: votrax_to_ssi263 = 6'h32;
            6'h12: votrax_to_ssi263 = 6'h2F;
            6'h13: votrax_to_ssi263 = 6'h10;
            6'h14: votrax_to_ssi263 = 6'h39;
            6'h15: votrax_to_ssi263 = 6'h0F;
            6'h16: votrax_to_ssi263 = 6'h13;
            6'h17: votrax_to_ssi263 = 6'h13;
            6'h18: votrax_to_ssi263 = 6'h20;
            6'h19: votrax_to_ssi263 = 6'h29;
            6'h1A: votrax_to_ssi263 = 6'h25;
            6'h1B: votrax_to_ssi263 = 6'h2C;
            6'h1C: votrax_to_ssi263 = 6'h26;
            6'h1D: votrax_to_ssi263 = 6'h34;
            6'h1E: votrax_to_ssi263 = 6'h25;
            6'h1F: votrax_to_ssi263 = 6'h30;
            6'h20: votrax_to_ssi263 = 6'h08;
            6'h21: votrax_to_ssi263 = 6'h09;
            6'h22: votrax_to_ssi263 = 6'h03;
            6'h23: votrax_to_ssi263 = 6'h1B;
            6'h24: votrax_to_ssi263 = 6'h0E;
            6'h25: votrax_to_ssi263 = 6'h27;
            6'h26: votrax_to_ssi263 = 6'h11;
            6'h27: votrax_to_ssi263 = 6'h07;
            6'h28: votrax_to_ssi263 = 6'h16;
            6'h29: votrax_to_ssi263 = 6'h05;
            6'h2A: votrax_to_ssi263 = 6'h28;
            6'h2B: votrax_to_ssi263 = 6'h1D;
            6'h2C: votrax_to_ssi263 = 6'h01;
            6'h2D: votrax_to_ssi263 = 6'h23;
            6'h2E: votrax_to_ssi263 = 6'h0C;
            6'h2F: votrax_to_ssi263 = 6'h0D;
            6'h30: votrax_to_ssi263 = 6'h10;
            6'h31: votrax_to_ssi263 = 6'h1A;
            6'h32: votrax_to_ssi263 = 6'h19;
            6'h33: votrax_to_ssi263 = 6'h18;
            6'h34: votrax_to_ssi263 = 6'h11;
            6'h35: votrax_to_ssi263 = 6'h11;
            6'h36: votrax_to_ssi263 = 6'h14;
            6'h37: votrax_to_ssi263 = 6'h14;
            6'h38: votrax_to_ssi263 = 6'h35;
            6'h39: votrax_to_ssi263 = 6'h36;
            6'h3A: votrax_to_ssi263 = 6'h1C;
            6'h3B: votrax_to_ssi263 = 6'h0A;
            6'h3C: votrax_to_ssi263 = 6'h01;
            6'h3D: votrax_to_ssi263 = 6'h10;
            default: votrax_to_ssi263 = 6'h00;
        endcase
    endfunction

    task automatic set_device_mode_and_ints;
        begin
            if ((duration_phoneme_q & 8'hC0) != MODE_IRQ_DISABLED) begin
                current_function_q <= duration_phoneme_q[7:6];
                current_enable_ints_q <= 1'b1;
            end else begin
                current_enable_ints_q <= 1'b0;
            end
        end
    endtask

    task automatic start_backend(input logic [5:0] phoneme,
                                 input logic votrax);
        begin
            backend_start_q <= 1'b1;
            backend_phoneme_q <= votrax ? votrax_to_ssi263(phoneme) : phoneme;
            backend_sc01_phone_q <= votrax ? phoneme : ssi263_to_sc01_phone(phoneme);
            backend_votrax_q <= votrax;
            active_is_votrax_q <= votrax;
        end
    endtask

    task automatic set_speech_irq;
        begin
            if (!active_is_votrax_q && (ctrl_art_amp_q & CONTROL_MASK) == 8'd0) begin
                if (current_enable_ints_q) begin
                    if (card_mode == PH_MOCKINGBOARD) begin
                        if (!d7_q && !via_pcr[0]) begin
                            via_ifr_set[IFR_CA1_SSI263] <= 1'b1;
                        end
                    end else if (card_mode == PH_PHASOR) begin
                        direct_irq_q <= 1'b1;
                    end
                end
                d7_q <= 1'b1;
            end

            if (active_is_votrax_q && via_pcr == 8'hB0) begin
                via_ifr_set[IFR_CB1_VOTRAX] <= 1'b1;
            end
        end
    endtask

    // A completed SSI263 phoneme remains ready; clearing A/!R by writing reg1
    // or reg2 lets the same phoneme run to completion again.
    task automatic repeat_completed_ssi263;
        begin
            if (d7_q && !active_is_votrax_q &&
                (ctrl_art_amp_q & CONTROL_MASK) == 8'd0) begin
                start_backend(duration_phoneme_q[5:0], 1'b0);
            end
        end
    endtask

    task automatic reset_power_state;
        begin
            duration_phoneme_q <= MODE_PHONEME_TRANSITIONED_INFLECTION;
            inflection_q <= 8'd0;
            rate_inflection_q <= 8'd0;
            ctrl_art_amp_q <= CONTROL_MASK;
            filter_freq_q <= FILTER_FREQ_SILENCE;
            current_function_q <= 2'd0;
            current_enable_ints_q <= 1'b0;
            d7_q <= 1'b0;
            active_is_votrax_q <= 1'b0;
            card_mode_prev_q <= PH_MOCKINGBOARD;
            direct_irq_q <= 1'b0;
            backend_start_q <= 1'b0;
            backend_phoneme_q <= 6'd0;
            backend_sc01_phone_q <= 6'h3F;
            backend_votrax_q <= 1'b0;
        end
    endtask

    task automatic reset_warm_state;
        begin
            duration_phoneme_q <= MODE_PHONEME_TRANSITIONED_INFLECTION;
            inflection_q <= 8'd0;
            rate_inflection_q <= 8'd0;
            ctrl_art_amp_q <= (SSI263_TYPE == SSI263_AP) ? CONTROL_MASK : 8'd0;
            filter_freq_q <= 8'd0;
            current_function_q <= 2'd0;
            current_enable_ints_q <= 1'b0;
            d7_q <= 1'b0;
            direct_irq_q <= 1'b0;
            backend_start_q <= 1'b0;
        end
    endtask

    ssi263_formant_backend formant_backend_i (
        .clk(clk),
        .rstn(rstn),
        .card_enabled(card_enabled),
        .warm_reset(formant_backend_reset),
        .audio_tick(audio_tick),
        .start(formant_backend_start),
        .start_phoneme(backend_phoneme_q),
        .start_sc01_phone(backend_sc01_phone_q),
        .start_votrax(backend_votrax_q),
        .current_function(current_function_q),
        .duration_phoneme(duration_phoneme_q),
        .inflection(inflection_q),
        .rate_inflection(rate_inflection_q),
        .ctrl_art_amp(ctrl_art_amp_q),
        .filter_freq(filter_freq_q),
        .phoneme_done(formant_backend_done),
        .audio(formant_audio)
    );

    always_ff @(posedge clk) begin
        logic backend_started_this_cycle;

        backend_started_this_cycle = 1'b0;
        via_ifr_set <= 7'd0;
        via_ifr_clr <= 7'd0;

        if (!rstn || !card_enabled) begin
            reset_power_state();
        end else begin
            backend_start_q <= 1'b0;
            card_mode_prev_q <= card_mode;

            if (!apple_res) begin
                if (SSI263_TYPE == SSI263_P) begin
                    if (ctrl_art_amp_q[7] == 1'b0) begin
                        set_device_mode_and_ints();
                    end
                end else if (SSI263_TYPE != SSI263_EMPTY) begin
                    reset_warm_state();
                end
            end else begin
                if (card_mode != card_mode_prev_q) begin
                    // The SSI263 A/R request is level-routed: with a completed
                    // phoneme (D7) still asserting the line, a mode change
                    // re-routes it rather than discarding it. Real Phasor
                    // hardware sets 6522 IFR.IxR_SSI263 when switching to
                    // Mockingboard mode with D7 pending, and asserts the direct
                    // 6502 IRQ when switching into Phasor mode.
                    if (d7_q) begin
                        if (!active_is_votrax_q && (ctrl_art_amp_q & CONTROL_MASK) == 8'd0) begin
                            if (current_enable_ints_q && card_mode == PH_MOCKINGBOARD && !via_pcr[0]) begin
                                via_ifr_set[IFR_CA1_SSI263] <= 1'b1;
                            end else if (current_enable_ints_q && card_mode == PH_PHASOR) begin
                                direct_irq_q <= 1'b1;
                            end
                        end
                    end
                    if (card_mode != PH_PHASOR) begin
                        direct_irq_q <= 1'b0;
                    end
                end

                if (ssi_write_strobe && SSI263_TYPE != SSI263_EMPTY) begin
                    if (ssi_reg <= SSI_RATEINF) begin
                        direct_irq_q <= 1'b0;
                        d7_q <= 1'b0;
                    end

                    case (ssi_reg)
                        SSI_DURPHON: begin
                            duration_phoneme_q <= ssi_wdata;
                            active_is_votrax_q <= 1'b0;
                            if ((ctrl_art_amp_q & CONTROL_MASK) == 8'd0) begin
                                start_backend(ssi_wdata[5:0], 1'b0);
                                backend_started_this_cycle = 1'b1;
                            end
                        end

                        SSI_INFLECT: begin
                            inflection_q <= ssi_wdata;
                            repeat_completed_ssi263();
                            if (d7_q && !active_is_votrax_q &&
                                (ctrl_art_amp_q & CONTROL_MASK) == 8'd0) begin
                                backend_started_this_cycle = 1'b1;
                            end
                        end

                        SSI_RATEINF: begin
                            rate_inflection_q <= ssi_wdata;
                            repeat_completed_ssi263();
                            if (d7_q && !active_is_votrax_q &&
                                (ctrl_art_amp_q & CONTROL_MASK) == 8'd0) begin
                                backend_started_this_cycle = 1'b1;
                            end
                        end

                        SSI_CTTRAMP: begin
                            if (ctrl_art_amp_q[7] && !ssi_wdata[7]) begin
                                set_device_mode_and_ints();
                                start_backend(duration_phoneme_q[5:0], 1'b0);
                                backend_started_this_cycle = 1'b1;
                            end

                            ctrl_art_amp_q <= ssi_wdata;
                            if (ssi_wdata[7]) begin
                                direct_irq_q <= 1'b0;
                                d7_q <= 1'b0;
                            end
                        end

                        default: begin
                            filter_freq_q <= ssi_wdata;
                        end
                    endcase
                end

                if (votrax_write_strobe && HAS_SC01) begin
                    via_ifr_clr[IFR_CB1_VOTRAX] <= 1'b1;
                    duration_phoneme_q <= 8'd0;
                    start_backend(votrax_wdata[5:0], 1'b1);
                    backend_started_this_cycle = 1'b1;
                end

                if (backend_done && !backend_started_this_cycle) begin
                    set_speech_irq();
                end
            end
        end
    end

endmodule
