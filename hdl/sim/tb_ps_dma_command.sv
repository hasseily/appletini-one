`timescale 1ns / 1ps

// Register-protocol test for ps_dma_command. The engine drain rules have a
// separate bench; this one checks that CPU0 can tell busy, done, and aborted
// apart and that an abort request stays asserted until the engine acks it.
module tb_ps_dma_command;

    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;

    logic rstn = 1'b0;
    globals::AxiSimple_common as_common;
    AxiSimple_if as_if();

    logic [23:0] dma_req_mc_addr;
    logic [31:0] dma_req_ddr_addr;
    logic [15:0] dma_req_length;
    logic dma_req_rw;
    logic dma_req_valid;
    logic dma_req_abort;
    logic dma_req_ready = 1'b0;
    logic dma_req_done = 1'b0;
    logic dma_req_abort_done = 1'b0;

    int fails = 0;

    ps_dma_command dut (
        .clk(clk),
        .rstn(rstn),
        .as_common(as_common),
        .as_client(as_if),
        .dma_req_mc_addr(dma_req_mc_addr),
        .dma_req_ddr_addr(dma_req_ddr_addr),
        .dma_req_length(dma_req_length),
        .dma_req_rw(dma_req_rw),
        .dma_req_valid(dma_req_valid),
        .dma_req_abort(dma_req_abort),
        .dma_req_ready(dma_req_ready),
        .dma_req_done(dma_req_done),
        .dma_req_abort_done(dma_req_abort_done)
    );

    task automatic check(input bit condition, input string message);
        if (!condition) begin
            $display("FAIL: %s", message);
            fails++;
        end
    endtask

    task automatic reg_write(input logic [7:0] addr,
                             input logic [31:0] data);
        @(negedge clk);
        as_common.awaddr = addr;
        as_common.wdata = data;
        as_common.wstrb = 4'hF;
        as_if.awvalid = 1'b1;
        @(negedge clk);
        as_if.awvalid = 1'b0;
    endtask

    task automatic reg_read(input logic [7:0] addr,
                            output logic [31:0] data);
        @(negedge clk);
        as_common.araddr = addr;
        @(posedge clk);
        #1ps;
        data = as_if.rdata;
        @(negedge clk);
        as_common.araddr = 8'hFF;
        @(posedge clk);
    endtask

    logic [31:0] status;
    initial begin
        as_common = '0;
        as_common.araddr = 8'hFF;
        as_if.awvalid = 1'b0;
        repeat (8) @(posedge clk);
        rstn = 1'b1;
        repeat (4) @(posedge clk);

        reg_write(8'h00, 32'h00123456);
        reg_write(8'h01, 32'h30001000);
        reg_write(8'h02, 32'h80000200);
        check(dma_req_valid && dma_req_rw && dma_req_length == 16'h0200 &&
              dma_req_mc_addr == 24'h123456 &&
              dma_req_ddr_addr == 32'h30001000,
              "command write publishes the complete DMA request");
        reg_read(8'h03, status);
        check(status[1] && !status[0] && !status[2],
              "new command reports busy only");

        @(negedge clk);
        dma_req_ready = 1'b1;
        @(negedge clk);
        dma_req_ready = 1'b0;
        check(!dma_req_valid, "engine acceptance clears request valid");
        @(negedge clk);
        dma_req_done = 1'b1;
        @(negedge clk);
        dma_req_done = 1'b0;
        reg_read(8'h03, status);
        check(status[0] && !status[1] && !status[2],
              $sformatf("normal completion reports done: status=%08x", status));
        reg_read(8'h03, status);
        check(!status[0] && !status[2],
              "reading STATUS clears the normal completion latch");

        reg_write(8'h02, 32'h80000200);
        @(negedge clk);
        dma_req_ready = 1'b1;
        @(negedge clk);
        dma_req_ready = 1'b0;
        reg_write(8'h04, 32'h00000001);
        repeat (5) @(posedge clk);
        check(dma_req_abort,
              "abort request stays high until the engine drains");
        reg_read(8'h03, status);
        check(status[1] && !status[0] && !status[2],
              "draining abort remains busy and incomplete");
        @(negedge clk);
        dma_req_abort_done = 1'b1;
        @(negedge clk);
        dma_req_abort_done = 1'b0;
        check(!dma_req_abort, "abort ack clears the held request");
        reg_read(8'h03, status);
        check(!status[1] && status[2] && !status[0],
              $sformatf("drained abort reports aborted: status=%08x", status));
        reg_read(8'h03, status);
        check(!status[2], "reading STATUS clears the aborted latch");

        if (fails == 0) $display("PS DMA COMMAND PASS");
        else            $display("PS DMA COMMAND FAILED: %0d checks", fails);
        $finish;
    end

    initial begin
        #1ms;
        $display("PS DMA COMMAND FAIL: global timeout");
        $finish;
    end

endmodule
