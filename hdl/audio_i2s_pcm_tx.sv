`timescale 1ns / 1ps

module audio_i2s_pcm_tx (
    input  logic               bclk_in,
    input  logic               resetn,
    input  logic               enable,
    input  logic               mute,
    input  logic signed [23:0] sample_l,
    input  logic signed [23:0] sample_r,
    output logic               i2s_bclk,
    output logic               i2s_lrck,
    output logic               i2s_sdata
);

    logic [31:0] shift_reg;
    logic [6:0] bit_pos;
    logic signed [23:0] left_q;
    logic signed [23:0] right_q;

    assign i2s_bclk = bclk_in;

    always_ff @(negedge bclk_in or negedge resetn) begin
        if (!resetn) begin
            shift_reg <= 32'd0;
            bit_pos <= 7'd0;
            left_q <= 24'sd0;
            right_q <= 24'sd0;
            i2s_lrck <= 1'b0;
            i2s_sdata <= 1'b0;
        end else if (!enable) begin
            shift_reg <= 32'd0;
            bit_pos <= 7'd0;
            left_q <= 24'sd0;
            right_q <= 24'sd0;
            i2s_lrck <= 1'b0;
            i2s_sdata <= 1'b0;
        end else begin
            if (bit_pos == 7'd0) begin
                left_q <= mute ? 24'sd0 : sample_l;
                right_q <= mute ? 24'sd0 : sample_r;
                i2s_lrck <= 1'b0;
                i2s_sdata <= 1'b0;
                shift_reg <= {mute ? 24'sd0 : sample_l, 8'h00};
                bit_pos <= 7'd1;
            end else if (bit_pos == 7'd32) begin
                i2s_lrck <= 1'b1;
                i2s_sdata <= 1'b0;
                shift_reg <= {right_q, 8'h00};
                bit_pos <= 7'd33;
            end else begin
                i2s_sdata <= shift_reg[31];
                shift_reg <= {shift_reg[30:0], 1'b0};
                bit_pos <= (bit_pos == 7'd63) ? 7'd0 : bit_pos + 7'd1;
            end
        end
    end

endmodule
