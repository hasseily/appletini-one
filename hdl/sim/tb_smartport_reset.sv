`timescale 1ns / 1ps
// Focused bench: Apple RES# must abort any in-flight SmartPort transaction
// (FIFOs, ready, exec-pending, control latch) while leaving the card
// serviceable -- the stale-transport state that killed the SmartPort after
// warm resets under the virtual TransWarp. Exercises RES# during
// partial-input, execution-pending, and unread-response states, models a
// PS-side command finishing AFTER the reset (stale push + the service's
// reset sweep), and proves a full round trip works after each abort.

module tb_smartport_reset;

    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 0;
    always #3.75 clk = ~clk;   // 133.333 MHz

    logic rstn = 0;

    globals::AppleBus_read   ab_read;
    globals::SoftSwitchState sss;
    globals::AppleBus_write  ab_write;
    globals::AxiSimple_common as_common;
    AxiSimple_if as_if();
    logic smartport_irq;

    smartport_card dut (
        .clk(clk),
        .rstn(rstn),
        .ab_read(ab_read),
        .sss(sss),
        .slot_assign(3'h7),
        .as_common(as_common),
        .as_client(as_if),
        .ab_write(ab_write),
        .smartport_irq(smartport_irq),
        .vtw_valid(1'b0),
        .vtw_target(3'd0),
        .vtw_addr(11'd0),
        .vtw_rw(1'b1),
        .vtw_wdata(8'd0),
        .vtw_sss_snapshot(22'd0),
        .vtw_ready(),
        .vtw_resp_valid(),
        .vtw_resp_rdata()
    );

    // Card registers in the slot-7 C8 window.
    localparam logic [15:0] A_DATA = 16'hCFF0;
    localparam logic [15:0] A_CTRL = 16'hCFF1;
    localparam logic [15:0] A_POP  = 16'hCFF2;

    // AXI register indices / control bits (mirrors smartport_service.c).
    localparam logic [7:0] R_STATUS   = 8'd0;
    localparam logic [7:0] R_IN_HEAD  = 8'd1;
    localparam logic [7:0] R_OUT_PUSH = 8'd2;
    localparam logic [7:0] R_CONTROL  = 8'd3;
    localparam logic [7:0] R_OUT_PUSH4 = 8'd5;
    localparam int CTL_POP_IN    = 1;
    localparam int CTL_CLR_IN    = 2;
    localparam int CTL_CLR_OUT   = 4;
    localparam int CTL_SET_READY = 8;
    localparam int CTL_ACK_EXEC  = 16;

    int fails = 0;
    task automatic check(input bit cond, input string msg);
        if (!cond) begin
            fails++;
            $display("SP RESET FAIL: %s (t=%0t)", msg, $time);
        end
    endtask

    // ------------------------------------------------------------------
    // Apple-bus emulation: one serve_en/data_en pulse pair per access,
    // like the wrapper emits for real (or vTW-driven) cycles.
    // ------------------------------------------------------------------
    task automatic apple_idle();
        ab_read           = '0;
        ab_read.res       = 1'b1;
        ab_read.rw        = 1'b1;
        ab_read.cycle_valid = 1'b1;
    endtask

    task automatic apple_write(input logic [15:0] addr, input logic [7:0] d);
        @(posedge clk);
        ab_read.addr <= addr;
        ab_read.rw   <= 1'b0;
        ab_read.data <= d;
        @(posedge clk);
        ab_read.serve_en <= 1'b1;
        @(posedge clk);
        ab_read.serve_en <= 1'b0;
        ab_read.data_en  <= 1'b1;
        @(posedge clk);
        ab_read.data_en <= 1'b0;
        ab_read.rw      <= 1'b1;
        repeat (4) @(posedge clk);
    endtask

    task automatic apple_read(input logic [15:0] addr, output logic [7:0] d);
        @(posedge clk);
        ab_read.addr <= addr;
        ab_read.rw   <= 1'b1;
        @(posedge clk);
        ab_read.serve_en <= 1'b1;
        @(posedge clk);
        ab_read.serve_en <= 1'b0;
        // Serving keys on the delayed strobe; data registers one clock on.
        repeat (2) @(posedge clk);
        check(ab_write.wr_data_en, $sformatf("card serves read of %h", addr));
        d = ab_write.wr_data;
        repeat (3) @(posedge clk);
    endtask

    task automatic apple_reset_pulse();
        @(posedge clk);
        ab_read.res <= 1'b0;
        repeat (20) @(posedge clk);
        ab_read.res <= 1'b1;
        repeat (4) @(posedge clk);
    endtask

    // ------------------------------------------------------------------
    // PS (AXI) emulation
    // ------------------------------------------------------------------
    task automatic axi_write(input logic [7:0] a, input logic [31:0] d);
        @(posedge clk);
        as_common.awaddr <= a;
        as_common.wdata  <= d;
        as_common.wstrb  <= 4'hF;
        as_if.awvalid    <= 1'b1;
        @(posedge clk);
        as_if.awvalid <= 1'b0;
        repeat (2) @(posedge clk);
    endtask

    task automatic axi_read(input logic [7:0] a, output logic [31:0] d);
        @(posedge clk);
        as_common.araddr <= a;
        repeat (2) @(posedge clk);
        d = as_if.rdata;
    endtask

    // status decode helpers
    function automatic bit st_ready(input logic [31:0] st);
        return st[29];
    endfunction
    function automatic bit st_exec(input logic [31:0] st);
        return st[28];
    endfunction
    function automatic int st_in(input logic [31:0] st);
        return int'(st[10:0]);
    endfunction
    function automatic int st_out(input logic [31:0] st);
        return int'(st[26:16]);
    endfunction

    /* Full command round trip, PS side played inline: Apple queues a
     * 3-byte frame + CTRL, PS drains and answers with two bytes, Apple
     * polls ready and pops both. Proves the transport is healthy. */
    task automatic round_trip(input logic [7:0] tag, input string label);
        logic [7:0]  rd;
        logic [31:0] st;

        apple_write(A_DATA, tag);
        apple_write(A_DATA, 8'h01);
        apple_write(A_DATA, 8'h02);
        apple_write(A_CTRL, 8'h02);           // EXECUTE (SmartPort family)
        axi_read(R_STATUS, st);
        check(st_exec(st) && st_in(st) == 3,
              $sformatf("%s: frame queued + exec pending", label));
        // PS drains the frame.
        repeat (3) begin
            logic [31:0] head;
            axi_read(R_IN_HEAD, head);
            check(head[8], $sformatf("%s: in byte present", label));
            axi_write(R_CONTROL, CTL_POP_IN);
        end
        // PS answers: status byte + payload byte, then ready+ack.
        axi_write(R_OUT_PUSH, {24'h0, tag});
        axi_write(R_OUT_PUSH, 8'h5A);
        axi_write(R_CONTROL, CTL_SET_READY | CTL_ACK_EXEC | CTL_CLR_IN);
        // Apple polls ready, pops both bytes.
        apple_read(A_CTRL, rd);
        check(rd[7], $sformatf("%s: ready seen", label));
        apple_read(A_DATA, rd);
        check(rd == tag, $sformatf("%s: response byte 0 (got %h)", label, rd));
        apple_write(A_POP, 8'h00);
        apple_read(A_DATA, rd);
        check(rd == 8'h5A, $sformatf("%s: response byte 1 (got %h)", label, rd));
        apple_write(A_POP, 8'h00);
        axi_read(R_STATUS, st);
        check(st_out(st) == 0 && !st_exec(st),
              $sformatf("%s: transport drained", label));
    endtask

    logic [7:0]  rd;
    logic [31:0] st;

    initial begin
        apple_idle();
        as_if.awvalid = 1'b0;
        as_common = '0;
        // Slot 7 owns the C8 window; INTCXROM off.
        sss = '0;
        sss.io_select = 8'h80;
        sss.slot_access = 1'b1;
        repeat (10) @(posedge clk);
        rstn = 1;
        repeat (10) @(posedge clk);

        // ---- 1. Baseline round trip ----
        round_trip(8'hA1, "baseline");

        // Cross a 32-bit word boundary through the physical Apple-bus
        // pipeline, including a scalar tail in the following RAM word.
        axi_write(R_CONTROL, CTL_CLR_OUT);
        axi_write(R_OUT_PUSH4, 32'h44332211);
        axi_write(R_OUT_PUSH, 8'h55);
        axi_write(R_CONTROL, CTL_SET_READY);
        for (int i = 0; i < 5; i++) begin
            apple_read(A_DATA, rd);
            check(rd == (8'h11 * 8'(i + 1)),
                  $sformatf("physical word-buffer byte %0d (got %h)", i, rd));
            apple_write(A_POP, 8'h00);
        end
        axi_read(R_STATUS, st);
        check(st_out(st) == 0,
              "physical word plus scalar tail drains exactly");

        // ---- 2. RES# during a partial input frame ----
        apple_write(A_DATA, 8'hB0);
        apple_write(A_DATA, 8'hB1);
        axi_read(R_STATUS, st);
        check(st_in(st) == 2, "partial frame queued");
        apple_reset_pulse();
        axi_read(R_STATUS, st);
        check(st_in(st) == 0 && !st_exec(st) && !st_ready(st),
              "RES# clears partial input frame");
        round_trip(8'hB2, "after partial-frame reset");

        // ---- 3. RES# with execution pending (PS never drained) ----
        apple_write(A_DATA, 8'hC0);
        apple_write(A_CTRL, 8'h02);
        axi_read(R_STATUS, st);
        check(st_exec(st), "exec pending before reset");
        apple_reset_pulse();
        axi_read(R_STATUS, st);
        check(!st_exec(st) && st_in(st) == 0,
              "RES# clears exec-pending + frame");
        /* PS-side command was mid-execution and completes AFTER the
         * release: its pushes land in the cleared FIFO as orphans... */
        axi_write(R_OUT_PUSH, 8'hEE);
        axi_write(R_OUT_PUSH, 8'hEE);
        axi_write(R_CONTROL, CTL_SET_READY | CTL_ACK_EXEC);
        axi_read(R_STATUS, st);
        check(st_out(st) == 2, "stale post-reset push lands (as it would)");
        /* ...and the service's reset sweep removes them. */
        axi_write(R_CONTROL, CTL_CLR_IN | CTL_CLR_OUT | CTL_ACK_EXEC);
        axi_read(R_STATUS, st);
        check(st_out(st) == 0 && !st_exec(st),
              "PS reset sweep clears stale response");
        round_trip(8'hC1, "after stale-execution sweep");

        // ---- 4. RES# with an unread response waiting ----
        apple_write(A_DATA, 8'hD0);
        apple_write(A_CTRL, 8'h02);
        axi_write(R_CONTROL, CTL_POP_IN);
        axi_write(R_OUT_PUSH, 8'hD1);
        axi_write(R_OUT_PUSH, 8'hD2);
        axi_write(R_CONTROL, CTL_SET_READY | CTL_ACK_EXEC | CTL_CLR_IN);
        apple_read(A_CTRL, rd);
        check(rd[7], "response ready before reset");
        apple_reset_pulse();
        axi_read(R_STATUS, st);
        check(st_out(st) == 0, "RES# discards unread response");
        apple_read(A_CTRL, rd);
        check(!rd[7], "ready cleared by RES#");
        round_trip(8'hD3, "after unread-response reset");

        if (fails == 0) $display("SP RESET PASS");
        else            $display("SP RESET FAILED: %0d checks", fails);
        $finish;
    end

    initial begin
        #2ms;
        $display("SP RESET FAIL: timeout");
        $finish;
    end

endmodule
