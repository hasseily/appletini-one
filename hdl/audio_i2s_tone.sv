// Mono DDS square-wave source serialized to both I2S channels.

module audio_i2s_tone (
    input  wire        bclk_in,
    input  wire        resetn,

    input  wire        enable,
    input  wire        mute,
    input  wire [31:0] tone_step,
    input  wire [23:0] amplitude,

    output wire        i2s_bclk,
    output reg         i2s_lrck,
    output reg         i2s_sdata
);

    assign i2s_bclk = bclk_in;

    reg [31:0] phase_acc = 32'd0;
    reg signed [23:0] sample_l = 24'sd0;
    reg signed [23:0] sample_r = 24'sd0;
    wire signed [23:0] amp_s = {1'b0, amplitude[22:0]};

    reg [31:0] shift_reg = 32'd0;
    reg [6:0]  bit_pos   = 7'd0;

    always @(negedge bclk_in or negedge resetn) begin
        if (!resetn || !enable) begin
            phase_acc <= 32'd0;
            sample_l  <= 24'sd0;
            sample_r  <= 24'sd0;
            shift_reg <= 32'd0;
            bit_pos   <= 7'd0;
            i2s_lrck  <= 1'b0;
            i2s_sdata <= 1'b0;
        end else begin
            if (bit_pos == 7'd0) begin
                phase_acc <= phase_acc + tone_step;
                sample_l <= mute ? 24'sd0 : (phase_acc[31] ? -amp_s : amp_s);
                sample_r <= sample_l;

                i2s_lrck  <= 1'b0;
                i2s_sdata <= 1'b0;
                shift_reg <= {sample_l, 8'h00};
                bit_pos   <= 7'd1;
            end else if (bit_pos == 7'd32) begin
                i2s_lrck  <= 1'b1;
                i2s_sdata <= 1'b0;
                shift_reg <= {sample_r, 8'h00};
                bit_pos   <= 7'd33;
            end else begin
                i2s_sdata <= shift_reg[31];
                shift_reg <= {shift_reg[30:0], 1'b0};
                bit_pos   <= (bit_pos == 7'd63) ? 7'd0 : bit_pos + 7'd1;
            end
        end
    end

endmodule
