`timescale 1ns / 1ps

// Small FWFT model for module-level tests. The capture blocks use equal read
// and write widths and one clock, so the model need not cover other XPM modes.
module xpm_fifo_sync #(
    parameter string FIFO_MEMORY_TYPE    = "auto",
    parameter int    FIFO_WRITE_DEPTH    = 2048,
    parameter int    WRITE_DATA_WIDTH    = 32,
    parameter int    READ_DATA_WIDTH     = WRITE_DATA_WIDTH,
    parameter string READ_MODE           = "std",
    parameter int    FIFO_READ_LATENCY   = 1,
    parameter string USE_ADV_FEATURES    = "0707",
    parameter int    WR_DATA_COUNT_WIDTH = 1,
    parameter int    FULL_RESET_VALUE    = 0,
    parameter string DOUT_RESET_VALUE    = "0",
    parameter string ECC_MODE            = "no_ecc",
    parameter int    WAKEUP_TIME         = 0
) (
    input  logic                           rst,
    input  logic                           wr_clk,
    input  logic [WRITE_DATA_WIDTH-1:0]    din,
    input  logic                           wr_en,
    input  logic                           rd_en,
    output logic [READ_DATA_WIDTH-1:0]     dout,
    output logic                           full,
    output logic                           empty,
    output logic                           almost_empty,
    output logic                           almost_full,
    output logic                           data_valid,
    output logic                           dbiterr,
    output logic                           overflow,
    output logic                           prog_empty,
    output logic                           prog_full,
    output logic [WR_DATA_COUNT_WIDTH-1:0] rd_data_count,
    output logic                           rd_rst_busy,
    output logic                           sbiterr,
    output logic                           underflow,
    output logic                           wr_ack,
    output logic [WR_DATA_COUNT_WIDTH-1:0] wr_data_count,
    output logic                           wr_rst_busy,
    input  logic                           injectdbiterr,
    input  logic                           injectsbiterr,
    input  logic                           sleep
);

    localparam int ADDR_WIDTH = $clog2(FIFO_WRITE_DEPTH);

    logic [WRITE_DATA_WIDTH-1:0] memory [0:FIFO_WRITE_DEPTH-1];
    logic [ADDR_WIDTH-1:0]       write_ptr_q;
    logic [ADDR_WIDTH-1:0]       read_ptr_q;
    integer                      count_q;

    initial begin
        if (WRITE_DATA_WIDTH != READ_DATA_WIDTH)
            $fatal(1, "xpm_fifo_sync_model needs equal port widths");
        if (READ_MODE != "fwft")
            $fatal(1, "xpm_fifo_sync_model only supports FWFT mode");
    end

    always_comb begin
        full          = (count_q == FIFO_WRITE_DEPTH);
        empty         = (count_q == 0);
        dout          = empty ? '0 : memory[read_ptr_q];
        almost_empty  = (count_q <= 1);
        almost_full   = (count_q >= FIFO_WRITE_DEPTH - 1);
        data_valid    = !empty;
        prog_empty    = empty;
        prog_full     = full;
        rd_data_count = WR_DATA_COUNT_WIDTH'(count_q);
        wr_data_count = WR_DATA_COUNT_WIDTH'(count_q);
        rd_rst_busy   = rst;
        wr_rst_busy   = rst;
        dbiterr       = 1'b0;
        sbiterr       = 1'b0;
    end

    always_ff @(posedge wr_clk) begin
        if (rst) begin
            write_ptr_q <= '0;
            read_ptr_q  <= '0;
            count_q     <= 0;
            overflow    <= 1'b0;
            underflow   <= 1'b0;
            wr_ack      <= 1'b0;
        end else begin
            overflow  <= wr_en && full;
            underflow <= rd_en && empty;
            wr_ack    <= wr_en && !full;

            if (wr_en && !full) begin
                memory[write_ptr_q] <= din;
                write_ptr_q <= write_ptr_q + 1'b1;
            end
            if (rd_en && !empty)
                read_ptr_q <= read_ptr_q + 1'b1;

            case ({wr_en && !full, rd_en && !empty})
                2'b10: count_q <= count_q + 1;
                2'b01: count_q <= count_q - 1;
                default: count_q <= count_q;
            endcase
        end
    end

    wire unused_inputs = &{1'b0, injectdbiterr, injectsbiterr, sleep,
                           FIFO_MEMORY_TYPE[0], USE_ADV_FEATURES[0],
                           DOUT_RESET_VALUE[0], ECC_MODE[0],
                           FIFO_READ_LATENCY[0], FULL_RESET_VALUE[0],
                           WAKEUP_TIME[0]};

endmodule
