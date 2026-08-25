`timescale 1ns / 1ps

// One Phasor SSI-263 socket output stage.
//
// The SSI wrapper exports the low-level U148 Line Out reconstruction.  The
// prototype applies common gain after POT3 has set the voice/noise ratio.  Keep
// that gain outside the SSI tract model and before the PSG mixer so it affects
// both speech sources without changing either AY/YM path.
module phasor_ssi263_output_stage #(
    parameter integer OUTPUT_GAIN_SHIFT = 5
) (
    input  logic               clk,
    input  logic               rstn,
    input  logic               card_enabled,
    input  logic signed [15:0] line_audio,
    output logic signed [15:0] card_audio,
    output logic               clipped
);

    logic signed [20:0] gained_audio;
    logic signed [15:0] gained_sample;
    logic               gained_clipped;
    logic signed [15:0] gained_sample_q;
    logic               gained_clipped_q;

    always_comb begin
        gained_audio =
            $signed({{5{line_audio[15]}}, line_audio}) <<< OUTPUT_GAIN_SHIFT;
        gained_clipped = 1'b0;
        if (gained_audio > 21'sd32767) begin
            gained_sample = 16'sh7FFF;
            gained_clipped = 1'b1;
        end else if (gained_audio < -21'sd32768) begin
            gained_sample = -16'sd32768;
            gained_clipped = 1'b1;
        end else begin
            gained_sample = gained_audio[15:0];
        end
    end

    // Register the stage for one fabric cycle.  This is 7.5 ns at the
    // production clock, not one 48 kHz sample, and keeps the new saturation
    // check out of the existing speech-plus-PSG timing path.
    always_ff @(posedge clk) begin
        if (!rstn) begin
            gained_sample_q <= 16'sd0;
            gained_clipped_q <= 1'b0;
        end else begin
            gained_sample_q <= gained_sample;
            gained_clipped_q <= gained_clipped;
        end
    end

    // card_enabled is a backplane boundary.  Mask the registered result at
    // once without resetting or otherwise changing either SSI die.
    assign card_audio = card_enabled ? gained_sample_q : 16'sd0;
    assign clipped = card_enabled && gained_clipped_q;

endmodule
