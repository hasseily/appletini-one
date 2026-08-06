`timescale 1ns / 1ps

// Focused abort tests for apple_dma_engine. A timeout may stop a SmartPort
// DDR-to-PSRAM copy, but the core may resume only after every accepted AXI
// beat and PSRAM operation has drained.
module tb_apple_dma_abort;

    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;

    logic rstn = 1'b0;
    logic [23:0] req_mc_addr = 24'h020000;
    logic [31:0] req_ddr_addr = 32'h30000000;
    logic [9:0] req_length = 10'd0;
    logic req_rw = 1'b1;
    logic req_valid = 1'b0;
    logic req_abort = 1'b0;
    logic req_ready;
    logic req_done;
    logic req_abort_done;

    logic [20:0] dma_line_addr;
    logic dma_rw;
    logic [63:0] dma_wdata;
    logic dma_valid;
    logic dma_ready = 1'b0;
    logic [63:0] dma_rdata = 64'd0;
    logic dma_rvalid = 1'b0;

    Axi3_read_if #(.ADDR_WIDTH(32), .DATA_WIDTH(64)) axi_read();
    Axi3_write_if #(.ADDR_WIDTH(32), .DATA_WIDTH(64)) axi_write();

    int fails = 0;
    int psram_write_accepts = 0;

    apple_dma_engine #(.LENGTH_W(10)) dut (
        .clk(clk),
        .rstn(rstn),
        .req_mc_addr(req_mc_addr),
        .req_ddr_addr(req_ddr_addr),
        .req_length(req_length),
        .req_rw(req_rw),
        .req_valid(req_valid),
        .req_abort(req_abort),
        .req_ready(req_ready),
        .req_done(req_done),
        .req_abort_done(req_abort_done),
        .dma_line_addr(dma_line_addr),
        .dma_rw(dma_rw),
        .dma_wdata(dma_wdata),
        .dma_valid(dma_valid),
        .dma_ready(dma_ready),
        .dma_rdata(dma_rdata),
        .dma_rvalid(dma_rvalid),
        .axi_hp1_read(axi_read),
        .axi_hp1_write(axi_write)
    );

    task automatic check(input bit condition, input string message);
        if (!condition) begin
            $display("FAIL: %s", message);
            fails++;
        end
    endtask

    task automatic start_write(input logic [9:0] length);
        wait (req_ready);
        @(negedge clk);
        req_length = length;
        req_valid = 1'b1;
        @(negedge clk);
        req_valid = 1'b0;
    endtask

    task automatic send_read_beat(input logic [63:0] data);
        wait (axi_read.rready);
        @(negedge clk);
        axi_read.rdata = data;
        axi_read.rvalid = 1'b1;
        @(negedge clk);
        axi_read.rvalid = 1'b0;
    endtask

    always @(posedge clk) begin
        if (dma_valid && dma_ready && !dma_rw)
            psram_write_accepts <= psram_write_accepts + 1;
    end

    initial begin
        axi_read.arready = 1'b1;
        axi_read.rdata = 64'd0;
        axi_read.rresp = 2'b00;
        axi_read.rlast = 1'b0;
        axi_read.rvalid = 1'b0;
        axi_write.awready = 1'b1;
        axi_write.wready = 1'b1;
        axi_write.bresp = 2'b00;
        axi_write.bvalid = 1'b0;

        repeat (8) @(posedge clk);
        rstn = 1'b1;
        repeat (4) @(posedge clk);

        // Abort after an eight-beat AXI read burst has been accepted. The
        // engine must discard and drain all eight beats before it responds.
        start_write(10'd64);
        wait (axi_read.arvalid && axi_read.arready);
        @(negedge clk);
        check(axi_read.arlen == 4'd7, "64-byte request issues eight AXI beats");
        req_abort = 1'b1;
        repeat (2) @(posedge clk);
        for (int beat = 0; beat < 8; beat++) begin
            send_read_beat(64'h1000 + 64'(beat));
            if (beat != 7)
                check(!req_abort_done,
                      $sformatf("abort waits after AXI beat %0d", beat + 1));
        end
        wait (req_abort_done);
        check(psram_write_accepts == 0,
              "AXI-drain abort launches no PSRAM write");
        @(negedge clk);
        req_abort = 1'b0;
        wait (req_ready);

        // Let one PSRAM write get accepted, then abort before its response.
        // The abort must wait for dma_rvalid and must not launch another op.
        psram_write_accepts = 0;
        dma_ready = 1'b1;
        start_write(10'd8);
        wait (axi_read.arvalid && axi_read.arready);
        send_read_beat(64'h8877665544332211);
        wait (dma_valid && !dma_rw && dma_ready);
        @(negedge clk);
        req_abort = 1'b1;
        repeat (10) begin
            @(posedge clk);
            check(!req_abort_done,
                  "abort waits for accepted PSRAM write response");
        end
        check(psram_write_accepts == 1,
              "only the accepted PSRAM write reached the memory port");
        @(negedge clk);
        dma_rvalid = 1'b1;
        @(negedge clk);
        dma_rvalid = 1'b0;
        wait (req_abort_done);
        check(psram_write_accepts == 1,
              "PSRAM drain abort launches no later write");
        @(negedge clk);
        req_abort = 1'b0;
        wait (req_ready);

        if (fails == 0) $display("APPLE DMA ABORT PASS");
        else            $display("APPLE DMA ABORT FAILED: %0d checks", fails);
        $finish;
    end

    initial begin
        #2ms;
        $display("APPLE DMA ABORT FAIL: global timeout");
        $finish;
    end

endmodule
