`timescale 1ns / 1ps

module audio_spdif_pcm_tx (
    input  logic               clk,
    input  logic               resetn,
    input  logic               enable,
    input  logic               mute,
    input  logic signed [23:0] sample_l,
    input  logic signed [23:0] sample_r,
    output logic               spdif_out
);

    localparam logic [31:0] HALF_BIT_INC = 32'h0BD377B1;

    localparam logic [7:0] PRE_X_PREV0 = 8'b11100010;
    localparam logic [7:0] PRE_Y_PREV0 = 8'b11100100;
    localparam logic [7:0] PRE_Z_PREV0 = 8'b11101000;
    localparam logic [7:0] PRE_X_PREV1 = 8'b00011101;
    localparam logic [7:0] PRE_Y_PREV1 = 8'b00011011;
    localparam logic [7:0] PRE_Z_PREV1 = 8'b00010111;

    logic [32:0] hb_acc;
    logic [31:0] subframe;
    logic [8:0] block_idx;
    logic channel_right;
    logic in_preamble;
    logic [2:0] pre_idx;
    logic [7:0] preamble;
    logic [5:0] data_bit_idx;
    logic half_phase;
    logic bmc_level;

    wire half_bit_tick = hb_acc[32];
    wire [32:0] hb_next = {1'b0, hb_acc[31:0]} + {1'b0, HALF_BIT_INC};

    function automatic [31:0] make_subframe(
        input logic [23:0] sample24,
        input logic        chan_right
    );
        logic [31:0] s;
        begin
            s = 32'd0;
            s[27:4] = sample24;
            s[28] = 1'b0;
            s[29] = 1'b0;
            s[30] = chan_right;
            s[31] = ^s[30:4];
            make_subframe = s;
        end
    endfunction

    function automatic [7:0] choose_preamble(
        input logic [7:0] prev0_bits,
        input logic [7:0] prev1_bits,
        input logic       prev_level
    );
        choose_preamble = prev_level ? prev1_bits : prev0_bits;
    endfunction

    task automatic load_next_subframe(
        input logic next_right,
        input logic [8:0] next_block_idx,
        input logic prev_level
    );
        logic [23:0] sample24;
        begin
            sample24 = mute ? 24'd0 : (next_right ? sample_r : sample_l);
            subframe <= make_subframe(sample24, next_right);

            if (!next_right) begin
                preamble <= (next_block_idx == 9'd0) ?
                    choose_preamble(PRE_Z_PREV0, PRE_Z_PREV1, prev_level) :
                    choose_preamble(PRE_X_PREV0, PRE_X_PREV1, prev_level);
            end else begin
                preamble <= choose_preamble(PRE_Y_PREV0, PRE_Y_PREV1, prev_level);
            end
        end
    endtask

    always_ff @(posedge clk or negedge resetn) begin
        if (!resetn) begin
            hb_acc <= 33'd0;
            subframe <= 32'd0;
            block_idx <= 9'd0;
            channel_right <= 1'b0;
            in_preamble <= 1'b1;
            pre_idx <= 3'd0;
            preamble <= PRE_Z_PREV0;
            data_bit_idx <= 6'd4;
            half_phase <= 1'b0;
            bmc_level <= 1'b0;
            spdif_out <= 1'b0;
        end else begin
            hb_acc <= hb_next;

            if (!enable) begin
                subframe <= 32'd0;
                block_idx <= 9'd0;
                channel_right <= 1'b0;
                in_preamble <= 1'b1;
                pre_idx <= 3'd0;
                preamble <= PRE_Z_PREV0;
                data_bit_idx <= 6'd4;
                half_phase <= 1'b0;
                bmc_level <= 1'b0;
                spdif_out <= 1'b0;
            end else if (half_bit_tick) begin
                if (in_preamble) begin
                    bmc_level <= preamble[7 - pre_idx];
                    spdif_out <= preamble[7 - pre_idx];
                    if (pre_idx == 3'd7) begin
                        in_preamble <= 1'b0;
                        pre_idx <= 3'd0;
                        data_bit_idx <= 6'd4;
                        half_phase <= 1'b0;
                    end else begin
                        pre_idx <= pre_idx + 3'd1;
                    end
                end else if (!half_phase) begin
                    bmc_level <= !bmc_level;
                    spdif_out <= !bmc_level;
                    half_phase <= 1'b1;
                end else begin
                    if (subframe[data_bit_idx]) begin
                        bmc_level <= !bmc_level;
                        spdif_out <= !bmc_level;
                    end else begin
                        spdif_out <= bmc_level;
                    end
                    half_phase <= 1'b0;

                    if (data_bit_idx == 6'd31) begin
                        logic next_right;
                        logic [8:0] next_block_idx;
                        logic end_level;

                        in_preamble <= 1'b1;
                        end_level = subframe[data_bit_idx] ? !bmc_level : bmc_level;
                        if (channel_right) begin
                            next_right = 1'b0;
                            next_block_idx = (block_idx == 9'd191) ? 9'd0 : block_idx + 9'd1;
                        end else begin
                            next_right = 1'b1;
                            next_block_idx = block_idx;
                        end

                        channel_right <= next_right;
                        block_idx <= next_block_idx;
                        load_next_subframe(next_right, next_block_idx, end_level);
                    end else begin
                        data_bit_idx <= data_bit_idx + 6'd1;
                    end
                end
            end
        end
    end

endmodule
