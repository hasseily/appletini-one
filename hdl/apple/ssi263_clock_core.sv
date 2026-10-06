`timescale 1ns / 1ps

// SSI-263A steady-state divider laws, independent of the audio sample clock.
// XCK is an enable in clk's domain; none of these outputs is a fabric clock.
//
// Datasheet, Operation Description / Programming Inflection and Filter
// Frequency Setting (page 2), and DIV2 pin description (page 4):
//   pitch  = effective_XCK / (8 * (4096 - I))
//   filter = effective_XCK / (2 * (256 - FF))
// These are whole-cycle enables, not the prototype's internal phase signals.
//
// Caller policy, NOT a claim about unmeasured silicon write/CTL phase:
// - run=0 preloads that divider and suppresses its output.
// - reload preloads it even without XCK; a coincident XCK is not counted.
// - After reset, preload each divider before enabling it. Reset seeds bounded
//   counters, not the target registers or a claimed silicon power-up phase.
// - While running, a changed target takes effect at the next cycle boundary.
// - rstn clears DIV2 phase. Hold DIV2 fixed during a run; reload both dividers
//   after changing it. DIV2=1 accepts the second, fourth, ... raw XCK enable.
// Keep CTL, power-down, warm-reset and bus-write policy outside this module.
module ssi263_clock_core (
    input  logic        clk,
    input  logic        rstn,
    input  logic        xck_ce,
    input  logic        div2,
    input  logic        pitch_run,
    input  logic        pitch_reload,
    input  logic [11:0] immediate_inflection,
    input  logic        filter_run,
    input  logic        filter_reload,
    input  logic [7:0]  filter_frequency,
    output logic        effective_xck_ce,
    output logic        pitch_ce,
    output logic        filter_ce
);

    logic        div2_phase_q;
    logic [14:0] pitch_remaining_q;
    logic [8:0]  filter_remaining_q;
    wire effective_tick = xck_ce && (!div2 || div2_phase_q);

    // Store period minus one: the longest periods are 32768 and 512 XCKs.
    // Concatenation keeps both endpoints exact without a truncated shift.
    wire [14:0] pitch_reload_value = {~immediate_inflection, 3'b111};
    wire [8:0]  filter_reload_value = {~filter_frequency, 1'b1};

    always_ff @(posedge clk) begin
        if (!rstn) begin
            div2_phase_q       <= 1'b0;
            pitch_remaining_q  <= 15'h7fff;
            filter_remaining_q <= 9'h1ff;
            effective_xck_ce   <= 1'b0;
            pitch_ce           <= 1'b0;
            filter_ce          <= 1'b0;
        end else begin
            effective_xck_ce <= effective_tick;
            pitch_ce <= 1'b0;
            filter_ce <= 1'b0;

            if (xck_ce)
                div2_phase_q <= ~div2_phase_q;

            if (!pitch_run || pitch_reload) begin
                pitch_remaining_q <= pitch_reload_value;
            end else if (effective_tick) begin
                if (pitch_remaining_q == 15'd0) begin
                    pitch_remaining_q <= pitch_reload_value;
                    pitch_ce <= 1'b1;
                end else begin
                    pitch_remaining_q <= pitch_remaining_q - 15'd1;
                end
            end

            if (!filter_run || filter_reload) begin
                filter_remaining_q <= filter_reload_value;
            end else if (effective_tick) begin
                if (filter_remaining_q == 9'd0) begin
                    filter_remaining_q <= filter_reload_value;
                    filter_ce <= 1'b1;
                end else begin
                    filter_remaining_q <= filter_remaining_q - 9'd1;
                end
            end
        end
    end

endmodule
