`timescale 1ns / 1ps
// Focused bench: the vTW SmartPort short-circuit port. The accelerator
// reaches the card's ROM and DATA/CTRL/DPOP logic fabric-internally
// instead of over the 1 MHz bus. This proves the fast port drives the
// SAME FIFO/exec/IRQ state the bus path does, that ROM/OUT-head reads are
// side-effect-free, and that the bus path still works alongside it.

module tb_smartport_shortcut;

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

    // vTW short-circuit port.
    logic       vtw_valid = 0;
    logic [2:0] vtw_target = 0;
    logic [10:0] vtw_addr = 0;
    logic       vtw_rw = 1;
    logic [7:0] vtw_wdata = 0;
    logic       vtw_ready;
    logic       vtw_resp_valid;
    logic [7:0] vtw_resp_rdata;

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
        .vtw_valid(vtw_valid),
        .vtw_target(vtw_target),
        .vtw_addr(vtw_addr),
        .vtw_rw(vtw_rw),
        .vtw_wdata(vtw_wdata),
        .vtw_ready(vtw_ready),
        .vtw_resp_valid(vtw_resp_valid),
        .vtw_resp_rdata(vtw_resp_rdata)
    );

    // Fast-port targets (mirror vtw_core_top's SP_TGT_*).
    localparam logic [2:0] T_SLOT_ROM = 3'd0;
    localparam logic [2:0] T_C8_ROM   = 3'd1;
    localparam logic [2:0] T_DATA     = 3'd2;
    localparam logic [2:0] T_CTRL     = 3'd3;
    localparam logic [2:0] T_DPOP     = 3'd4;

    // AXI register indices / control bits (mirror smartport_service.c).
    localparam logic [7:0] R_STATUS   = 8'd0;
    localparam logic [7:0] R_IN_HEAD  = 8'd1;
    localparam logic [7:0] R_OUT_PUSH = 8'd2;
    localparam logic [7:0] R_CONTROL  = 8'd3;
    localparam int CTL_POP_IN    = 1;
    localparam int CTL_CLR_IN    = 2;
    localparam int CTL_CLR_OUT   = 4;
    localparam int CTL_SET_READY = 8;
    localparam int CTL_ACK_EXEC  = 16;

    int fails = 0;
    task automatic check(input bit cond, input string msg);
        if (!cond) begin
            fails++;
            $display("SP SHORTCUT FAIL: %s (t=%0t)", msg, $time);
        end
    endtask

    int irq_count = 0;
    always @(posedge clk) if (rstn && smartport_irq) irq_count++;

    // ------------------------------------------------------------------
    // vTW fast-port access: assert valid, fire on the first ready edge,
    // capture the response pulse. Single outstanding.
    // ------------------------------------------------------------------
    task automatic vtw_access(input logic [2:0] target,
                              input logic       rw,
                              input logic [10:0] addr,
                              input logic [7:0]  wdata,
                              output logic [7:0] rdata);
        @(posedge clk);
        vtw_valid  <= 1'b1;
        vtw_target <= target;
        vtw_rw     <= rw;
        vtw_addr   <= addr;
        vtw_wdata  <= wdata;
        // The card accepts on the first edge with vtw_ready high.
        @(posedge clk iff vtw_ready);
        vtw_valid <= 1'b0;
        @(posedge clk iff vtw_resp_valid);
        rdata = vtw_resp_rdata;
        repeat (2) @(posedge clk);
    endtask

    // Bus-path access (proves coexistence): one serve/data pulse pair.
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

    function automatic bit st_ready(input logic [31:0] st); return st[29]; endfunction
    function automatic bit st_exec(input logic [31:0] st);  return st[28]; endfunction
    function automatic int st_in(input logic [31:0] st);  return int'(st[10:0]); endfunction
    function automatic int st_out(input logic [31:0] st); return int'(st[26:16]); endfunction

    logic [7:0]  rd, rd2;
    logic [31:0] st;
    logic [7:0]  rom0, rom0b, rom1, c8a;

    initial begin
        ab_read = '0;
        ab_read.res = 1'b1;
        ab_read.rw  = 1'b1;
        ab_read.cycle_valid = 1'b1;
        as_if.awvalid = 1'b0;
        as_common = '0;
        // Slot 7 owns the C8 window; INTCXROM off (matches the vTW
        // classifier's precondition).
        sss = '0;
        sss.io_select = 8'h80;
        sss.slot_access = 1'b1;
        repeat (10) @(posedge clk);
        rstn = 1;
        repeat (10) @(posedge clk);

        // ---- 1. ROM reads are stable and side-effect-free ----
        vtw_access(T_SLOT_ROM, 1'b1, 11'h000, 8'h00, rom0);
        vtw_access(T_SLOT_ROM, 1'b1, 11'h000, 8'h00, rom0b);
        vtw_access(T_SLOT_ROM, 1'b1, 11'h001, 8'h00, rom1);
        vtw_access(T_C8_ROM,   1'b1, 11'h000, 8'h00, c8a);
        check(rom0 == rom0b, "slot ROM read is repeatable");
        axi_read(R_STATUS, st);
        check(st_in(st) == 0 && st_out(st) == 0,
              "ROM reads leave the FIFOs untouched");

        // ---- 2. Full command round trip entirely through the fast port ----
        vtw_access(T_DATA, 1'b0, 11'h7F0, 8'hA1, rd);   // frame byte 0
        vtw_access(T_DATA, 1'b0, 11'h7F0, 8'h11, rd);   // frame byte 1
        vtw_access(T_DATA, 1'b0, 11'h7F0, 8'h22, rd);   // frame byte 2
        axi_read(R_STATUS, st);
        check(st_in(st) == 3, "fast-port DATA writes reached the IN FIFO");

        irq_count = 0;
        vtw_access(T_CTRL, 1'b0, 11'h7F1, 8'h02, rd);   // EXECUTE
        axi_read(R_STATUS, st);
        check(st_exec(st), "fast-port CTRL write raised exec_pending");
        check(irq_count == 1, "fast-port EXECUTE pulsed the PS IRQ once");

        // PS drains the frame, answers with two bytes, sets ready.
        repeat (3) axi_write(R_CONTROL, CTL_POP_IN);
        axi_write(R_OUT_PUSH, 8'h5A);
        axi_write(R_OUT_PUSH, 8'h6B);
        axi_write(R_CONTROL, CTL_SET_READY | CTL_ACK_EXEC | CTL_CLR_IN);

        // Poll ready through the fast port.
        vtw_access(T_CTRL, 1'b1, 11'h7F1, 8'h00, rd);
        check(rd[7], "fast-port CTRL read returns ready");

        // ---- 3. DATA read is side-effect-free; DPOP advances ----
        vtw_access(T_DATA, 1'b1, 11'h7F0, 8'h00, rd);
        vtw_access(T_DATA, 1'b1, 11'h7F0, 8'h00, rd2);
        check(rd == 8'h5A && rd2 == 8'h5A,
              "repeated fast DATA read returns the same head (no pop)");
        vtw_access(T_DPOP, 1'b0, 11'h7F2, 8'h00, rd);   // pop
        vtw_access(T_DATA, 1'b1, 11'h7F0, 8'h00, rd);
        check(rd == 8'h6B, "fast DPOP advanced the OUT FIFO");
        // A DPOP *read* must not pop.
        vtw_access(T_DPOP, 1'b1, 11'h7F2, 8'h00, rd);
        vtw_access(T_DATA, 1'b1, 11'h7F0, 8'h00, rd);
        check(rd == 8'h6B, "fast DPOP read did not pop");
        vtw_access(T_DPOP, 1'b0, 11'h7F2, 8'h00, rd);   // pop last
        axi_read(R_STATUS, st);
        check(st_out(st) == 0 && !st_exec(st),
              "transport drained via the fast port");

        // ---- 4. Bus path still works after fast-port traffic ----
        apple_write(16'hCFF0, 8'h99);
        axi_read(R_STATUS, st);
        check(st_in(st) == 1, "bus-path DATA write still reaches the IN FIFO");

        if (fails == 0) $display("SP SHORTCUT PASS");
        else            $display("SP SHORTCUT FAILED: %0d checks", fails);
        $finish;
    end

    initial begin
        #2ms;
        $display("SP SHORTCUT FAIL: timeout");
        $finish;
    end

endmodule
