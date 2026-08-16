`timescale 1ns / 1ps

/* Focused late-frame restart test. The old fb_reader reset its outstanding
 * count at vblank while eight accepted reads could still return. Their RLAST
 * beats then wrapped the cleared count and their data could enter the freshly
 * reset FIFO. This bench holds eight reads in flight across vblank and proves
 * that the next base is not latched until every old response drains. */
module tb_fb_reader_restart;

    logic clk = 1'b0;
    logic pixel_clk = 1'b0;
    always #5 clk = ~clk;
    always #3 pixel_clk = ~pixel_clk;

    logic resetn = 1'b0;
    logic [31:0] base_addr_in = 32'h0;
    logic [31:0] last_latched_addr;
    logic vblank_latched_pulse;
    logic vblank_start = 1'b0;
    logic pixel_rd_en = 1'b0;
    logic [15:0] pixel_rgb565;
    logic axi_read_err;
    logic [2:0] dbg_state;
    logic [17:0] dbg_burst_count;
    logic [15:0] dbg_axi_err_count;
    logic [15:0] dbg_underrun_count;

    Axi3_read_if #(.ADDR_WIDTH(32), .DATA_WIDTH(64)) axi_read();

    fb_reader dut (
        .clk(clk),
        .resetn(resetn),
        .axi_read_if(axi_read),
        .base_addr_in(base_addr_in),
        .last_latched_addr(last_latched_addr),
        .vblank_latched_pulse(vblank_latched_pulse),
        .vblank_start(vblank_start),
        .pixel_clk(pixel_clk),
        .pixel_rd_en(pixel_rd_en),
        .pixel_rgb565(pixel_rgb565),
        .axi_read_err(axi_read_err),
        .dbg_state(dbg_state),
        .dbg_burst_count(dbg_burst_count),
        .dbg_axi_err_count(dbg_axi_err_count),
        .dbg_underrun_count(dbg_underrun_count)
    );

    int failures = 0;
    int latch_pulses = 0;

    task automatic check(input bit condition, input string message);
        if (!condition) begin
            $display("FAIL: %s", message);
            failures++;
        end
    endtask

    task automatic pulse_vblank;
        @(negedge clk);
        vblank_start = 1'b1;
        @(negedge clk);
        vblank_start = 1'b0;
    endtask

    task automatic send_old_rlast;
        @(negedge clk);
        axi_read.rdata = 64'hDEAD_0000_0000_0000;
        axi_read.rlast = 1'b1;
        axi_read.rvalid = 1'b1;
        @(negedge clk);
        axi_read.rlast = 1'b0;
        axi_read.rvalid = 1'b0;
    endtask

    always @(posedge clk) begin
        if (vblank_latched_pulse)
            latch_pulses <= latch_pulses + 1;
    end

    initial begin
        axi_read.arready = 1'b1;
        axi_read.rdata = 64'h0;
        axi_read.rresp = 2'b00;
        axi_read.rlast = 1'b0;
        axi_read.rvalid = 1'b0;

        repeat (8) @(posedge clk);
        resetn = 1'b1;
        repeat (3) @(posedge clk);

        base_addr_in = 32'h3E00_0000;
        pulse_vblank();
        wait (last_latched_addr == 32'h3E00_0000);
        wait (dbg_state == 3'd2);

        /* Accept seven ARs, then hold the eighth ARVALID pending. AXI masters
         * may not cancel that request when vblank arrives. */
        while (dut.outstanding != 4'd7)
            @(negedge clk);
        axi_read.arready = 1'b0;
        wait (axi_read.arvalid);
        repeat (2) @(posedge clk);
        check(dut.outstanding == 4'd7 && axi_read.arvalid,
              "eighth old-frame AR remains pending before vblank");
        check(latch_pulses == 1, "first framebuffer base latched once");

        base_addr_in = 32'h3E40_0000;
        pulse_vblank();
        wait (dbg_state == 3'd3);
        check(last_latched_addr == 32'h3E00_0000,
              "new base waits while old AXI reads remain outstanding");

        for (int burst = 0; burst < 7; ++burst)
            send_old_rlast();
        repeat (2) @(posedge clk);
        check(dbg_state == 3'd3,
              "reader stays in drain while an old ARVALID remains pending");
        check(dut.outstanding == 4'd0 && axi_read.arvalid,
              "responses drain to zero without cancelling pending ARVALID");
        check(last_latched_addr == 32'h3E00_0000 && latch_pulses == 1,
              "cancelled responses cannot commit the next frame early");

        /* Accept the pending old request. It becomes one more outstanding
         * response and must also finish before restart. */
        axi_read.arready = 1'b1;
        wait (dut.outstanding == 4'd1 && !axi_read.arvalid);
        send_old_rlast();
        wait (dbg_state == 3'd1);
        repeat (2) @(posedge clk);
        check(dut.outstanding == 4'd0,
              "drain reaches zero without wrapping the outstanding count");
        check(last_latched_addr == 32'h3E40_0000,
              "new base commits only after the old AXI frame drains");
        check(latch_pulses == 2,
              "late restart produces one new frame-latch pulse");

        wait (dbg_state == 3'd2);
        wait (axi_read.arvalid);
        check(axi_read.araddr == 32'h3E40_0000,
              "first post-drain burst starts at the new framebuffer base");

        if (failures == 0)
            $display("FB READER RESTART PASS");
        else
            $display("FB READER RESTART FAIL: %0d error(s)", failures);
        $finish;
    end

    initial begin
        #200000;
        $display("FB READER RESTART FAIL: timeout");
        $finish;
    end

endmodule

/* The restart test needs only the control behavior around the XPM blocks.
 * These small simulation stubs keep FIFO contents out of scope. */
module xpm_cdc_single #(
    parameter integer DEST_SYNC_FF = 2,
    parameter integer INIT_SYNC_FF = 0,
    parameter integer SIM_ASSERT_CHK = 0,
    parameter integer SRC_INPUT_REG = 0
) (
    input  logic src_clk,
    input  logic src_in,
    input  logic dest_clk,
    output logic dest_out
);
    always @(posedge dest_clk)
        dest_out <= src_in;
endmodule

module xpm_fifo_async #(
    parameter integer CDC_SYNC_STAGES = 2,
    parameter DOUT_RESET_VALUE = "0",
    parameter ECC_MODE = "no_ecc",
    parameter FIFO_MEMORY_TYPE = "block",
    parameter integer FIFO_READ_LATENCY = 1,
    parameter integer FIFO_WRITE_DEPTH = 8192,
    parameter integer FULL_RESET_VALUE = 0,
    parameter integer PROG_EMPTY_THRESH = 10,
    parameter integer PROG_FULL_THRESH = 7680,
    parameter integer RD_DATA_COUNT_WIDTH = 15,
    parameter integer READ_DATA_WIDTH = 16,
    parameter READ_MODE = "fwft",
    parameter integer RELATED_CLOCKS = 0,
    parameter integer SIM_ASSERT_CHK = 0,
    parameter USE_ADV_FEATURES = "0102",
    parameter integer WAKEUP_TIME = 0,
    parameter integer WRITE_DATA_WIDTH = 64,
    parameter integer WR_DATA_COUNT_WIDTH = 14
) (
    input  logic wr_clk,
    input  logic wr_en,
    input  logic [WRITE_DATA_WIDTH-1:0] din,
    output logic full,
    output logic prog_full,
    output logic [WR_DATA_COUNT_WIDTH-1:0] wr_data_count,
    output logic overflow,
    output logic wr_rst_busy,
    output logic almost_full,
    output logic wr_ack,
    input  logic rd_clk,
    input  logic rd_en,
    output logic [READ_DATA_WIDTH-1:0] dout,
    output logic empty,
    output logic prog_empty,
    output logic [RD_DATA_COUNT_WIDTH-1:0] rd_data_count,
    output logic underflow,
    output logic rd_rst_busy,
    output logic almost_empty,
    output logic data_valid,
    input  logic rst,
    input  logic sleep,
    input  logic injectsbiterr,
    input  logic injectdbiterr,
    output logic sbiterr,
    output logic dbiterr
);
    always_comb begin
        full = 1'b0;
        prog_full = 1'b0;
        wr_data_count = '0;
        overflow = 1'b0;
        wr_rst_busy = rst;
        almost_full = 1'b0;
        wr_ack = wr_en && !rst;
        dout = '0;
        empty = 1'b0;
        prog_empty = 1'b0;
        rd_data_count = '0;
        underflow = 1'b0;
        rd_rst_busy = rst;
        almost_empty = 1'b0;
        data_valid = rd_en && !rst;
        sbiterr = 1'b0;
        dbiterr = 1'b0;
    end
endmodule
