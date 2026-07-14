`timescale 1ns / 1ps

module audio_pcm16_ddr_fetcher #(
    parameter int unsigned SAMPLE_ADDR_WIDTH = 18
) (
    input  logic                                 clk,
    input  logic                                 rstn,
    input  logic [31:0]                          sample_base_addr,

    input  logic [SAMPLE_ADDR_WIDTH-1:0]         voice0_addr,
    input  logic                                 voice0_req,
    output logic signed [15:0]                   voice0_data,
    output logic                                 voice0_valid,

    input  logic [SAMPLE_ADDR_WIDTH-1:0]         voice1_addr,
    input  logic                                 voice1_req,
    output logic signed [15:0]                   voice1_data,
    output logic                                 voice1_valid,

    Axi3_read_if.master                          axi_read
);

    localparam logic [1:0] S_IDLE = 2'd0;
    localparam logic [1:0] S_AR   = 2'd1;
    localparam logic [1:0] S_R    = 2'd2;
    localparam int unsigned BYTE_ADDR_PAD_WIDTH = 31 - SAMPLE_ADDR_WIDTH;

    logic [1:0]  state_q;
    logic        target_q;
    logic        last_grant_q;
    logic [31:0] beat_addr_q;
    logic [1:0]  sample_lane_q;

    wire choose_voice0 = voice0_req && (!voice1_req || last_grant_q);
    wire choose_voice1 = voice1_req && !choose_voice0;
    wire [SAMPLE_ADDR_WIDTH-1:0] selected_sample_addr =
        choose_voice0 ? voice0_addr : voice1_addr;
    wire [31:0] selected_byte_addr =
        sample_base_addr + {{BYTE_ADDR_PAD_WIDTH{1'b0}}, selected_sample_addr, 1'b0};

    function automatic logic signed [15:0] sample_from_beat(
        input logic [63:0] beat,
        input logic [1:0] lane
    );
        begin
            unique case (lane)
                2'd0: sample_from_beat = beat[15:0];
                2'd1: sample_from_beat = beat[31:16];
                2'd2: sample_from_beat = beat[47:32];
                default: sample_from_beat = beat[63:48];
            endcase
        end
    endfunction

    always_comb begin
        axi_read.araddr  = beat_addr_q;
        axi_read.arlen   = 4'd0;
        axi_read.arsize  = 3'd3;
        axi_read.arburst = 2'b01;
        axi_read.arvalid = (state_q == S_AR);
        axi_read.rready  = (state_q == S_R);
    end

    always_ff @(posedge clk) begin
        if (!rstn) begin
            state_q       <= S_IDLE;
            target_q      <= 1'b0;
            last_grant_q  <= 1'b0;
            beat_addr_q   <= 32'h00000000;
            sample_lane_q <= 2'd0;
            voice0_data   <= 16'sd0;
            voice1_data   <= 16'sd0;
            voice0_valid  <= 1'b0;
            voice1_valid  <= 1'b0;
        end else begin
            voice0_valid <= 1'b0;
            voice1_valid <= 1'b0;

            unique case (state_q)
                S_IDLE: begin
                    if (voice0_req || voice1_req) begin
                        if (sample_base_addr == 32'h00000000) begin
                            if (choose_voice0) begin
                                voice0_data <= 16'sd0;
                                voice0_valid <= 1'b1;
                                last_grant_q <= 1'b0;
                            end else begin
                                voice1_data <= 16'sd0;
                                voice1_valid <= 1'b1;
                                last_grant_q <= 1'b1;
                            end
                        end else begin
                            target_q      <= choose_voice1;
                            last_grant_q  <= choose_voice1;
                            beat_addr_q   <= {selected_byte_addr[31:3], 3'b000};
                            sample_lane_q <= selected_byte_addr[2:1];
                            state_q       <= S_AR;
                        end
                    end
                end

                S_AR: begin
                    if (axi_read.arready)
                        state_q <= S_R;
                end

                S_R: begin
                    if (axi_read.rvalid) begin
                        if (target_q == 1'b0) begin
                            voice0_data <= axi_read.rresp[1] ? 16'sd0 :
                                sample_from_beat(axi_read.rdata, sample_lane_q);
                            voice0_valid <= 1'b1;
                        end else begin
                            voice1_data <= axi_read.rresp[1] ? 16'sd0 :
                                sample_from_beat(axi_read.rdata, sample_lane_q);
                            voice1_valid <= 1'b1;
                        end

                        if (axi_read.rlast)
                            state_q <= S_IDLE;
                    end
                end

                default: state_q <= S_IDLE;
            endcase
        end
    end

endmodule
