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
    logic [21:0] vtw_sss_snapshot = 22'h2AAAAA;
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
        .overlay_capture_drop(1'b0),
        .overlay_canvas_shr_active(1'b0),
        .overlay_devsel_enabled(),
        .overlay_capture_armed(),
        .overlay_capture_bank_aux(),
        .overlay_capture_base(),
        .overlay_capture_limit(),
        .vtw_valid(vtw_valid),
        .vtw_target(vtw_target),
        .vtw_addr(vtw_addr),
        .vtw_rw(vtw_rw),
        .vtw_wdata(vtw_wdata),
        .vtw_sss_snapshot(vtw_sss_snapshot),
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
    localparam logic [7:0] R_SSS      = 8'd4;
    localparam logic [7:0] R_OUT_PUSH4 = 8'd5;
    localparam logic [7:0] R_IN_HEAD4 = 8'd6;
    localparam int CTL_POP_IN    = 1;
    localparam int CTL_CLR_IN    = 2;
    localparam int CTL_CLR_OUT   = 4;
    localparam int CTL_SET_READY = 8;
    localparam int CTL_ACK_EXEC  = 16;
    localparam int CTL_SET_DIRECT = 32;
    localparam int CTL_POP_IN4    = 64;

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

    task automatic apple_read(input logic [15:0] addr,
                              output logic [7:0] d);
        @(posedge clk);
        ab_read.addr <= addr;
        ab_read.rw   <= 1'b1;
        @(posedge clk);
        ab_read.serve_en <= 1'b1;
        @(posedge clk);
        ab_read.serve_en <= 1'b0;
        @(posedge clk);
        #1;
        check(ab_write.wr_data_en,
              $sformatf("bus read %h produced no response", addr));
        d = ab_write.wr_data;
        ab_read.data_en <= 1'b1;
        @(posedge clk);
        ab_read.data_en <= 1'b0;
        repeat (2) @(posedge clk);
    endtask

    /* A DEVSEL reply must stay live from its address decode through the
     * wrapper's later data-drive window. A one-clock pulse can pass a unit
     * test but never reach the Apple bus. */
    task automatic apple_overlay_read(input logic [15:0] addr,
                                      input logic [7:0] expected);
        @(posedge clk);
        ab_read.addr <= addr;
        ab_read.rw   <= 1'b1;
        @(posedge clk);
        ab_read.serve_en <= 1'b1;
        @(posedge clk);
        ab_read.serve_en <= 1'b0;
        repeat (20) @(posedge clk);
        check(ab_write.wr_data_en,
              $sformatf("overlay holds read reply for %h", addr));
        check(ab_write.wr_data == expected,
              $sformatf("overlay read %h returns %h (got %h)",
                        addr, expected, ab_write.wr_data));
        ab_read.data_en <= 1'b1;
        @(posedge clk);
        ab_read.data_en <= 1'b0;
        repeat (2) @(posedge clk);
        check(!ab_write.wr_data_en,
              $sformatf("overlay releases read reply for %h", addr));
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
    function automatic bit st_exec_vtw(input logic [31:0] st); return st[31]; endfunction
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

        // The v1.0 MAGIC byte lives at slot-7 DEVSEL +$E.
        apple_overlay_read(16'hC0FE, 8'h4C);

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
        vtw_access(T_DATA, 1'b0, 11'h7F0, 8'h33, rd);
        vtw_access(T_DATA, 1'b0, 11'h7F0, 8'h44, rd);
        vtw_access(T_DATA, 1'b0, 11'h7F0, 8'h55, rd);
        vtw_access(T_DATA, 1'b0, 11'h7F0, 8'h66, rd);
        vtw_access(T_DATA, 1'b0, 11'h7F0, 8'h77, rd);
        axi_read(R_STATUS, st);
        check(st_in(st) == 8, "fast-port DATA writes reached the IN FIFO");

        irq_count = 0;
        vtw_access(T_CTRL, 1'b0, 11'h7F1, 8'h02, rd);   // EXECUTE
        axi_read(R_STATUS, st);
        check(st_exec(st), "fast-port CTRL write raised exec_pending");
        check(st_exec_vtw(st), "fast-port command is tagged as vTW");
        check(irq_count == 1, "fast-port EXECUTE pulsed the PS IRQ once");
        axi_read(R_SSS, st);
        check(st[21:0] == vtw_sss_snapshot,
              "fast-port command latched the vTW private switch state");

        // PS drains aligned words and posts one aligned response word. The
        // Apple still sees four bytes in least-significant-first order.
        axi_read(R_IN_HEAD4, st);
        check(st == 32'h332211A1, "packed IN head returns first four bytes");
        axi_write(R_CONTROL, CTL_POP_IN4);
        axi_read(R_IN_HEAD4, st);
        check(st == 32'h77665544, "packed IN pop prefetched the next word");
        axi_write(R_CONTROL, CTL_POP_IN4);
        axi_write(R_OUT_PUSH4, 32'h8D7C6B5A);
        axi_read(R_STATUS, st);
        check(!st[30] && st_out(st) == 4,
              "word response stored four bytes before READY");
        axi_write(R_CONTROL, CTL_SET_READY | CTL_ACK_EXEC | CTL_CLR_IN);

        // Poll ready through the fast port.
        vtw_access(T_CTRL, 1'b1, 11'h7F1, 8'h00, rd);
        check(rd[7] && rd[5],
              "fast-port CTRL read returns ready and private fast marker");

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
        vtw_access(T_DPOP, 1'b0, 11'h7F2, 8'h00, rd);
        vtw_access(T_DATA, 1'b1, 11'h7F0, 8'h00, rd);
        check(rd == 8'h7C, "packed response byte 2 is ordered");
        vtw_access(T_DPOP, 1'b0, 11'h7F2, 8'h00, rd);
        vtw_access(T_DATA, 1'b1, 11'h7F0, 8'h00, rd);
        check(rd == 8'h8D, "packed response byte 3 is ordered");
        vtw_access(T_DPOP, 1'b0, 11'h7F2, 8'h00, rd);   // pop last
        axi_read(R_STATUS, st);
        check(st_out(st) == 0 && !st_exec(st),
              "transport drained via the fast port");

        // ---- 3b. Word-boundary prefetch and scalar tail ----
        axi_write(R_OUT_PUSH4, 32'h04030201);
        axi_write(R_OUT_PUSH, 8'h05);
        for (int i = 1; i <= 5; i++) begin
            vtw_access(T_DATA, 1'b1, 11'h7F0, 8'h00, rd);
            check(rd == 8'(i),
                  $sformatf("response byte %0d crosses word boundary", i));
            vtw_access(T_DPOP, 1'b0, 11'h7F2, 8'h00, rd);
        end
        axi_read(R_STATUS, st);
        check(st_out(st) == 0, "word plus scalar tail drains exactly");

        // ---- 4. Direct block response contains only the result byte ----
        vtw_access(T_DATA, 1'b0, 11'h7F0, 8'h01, rd);
        vtw_access(T_CTRL, 1'b0, 11'h7F1, 8'h01, rd);
        axi_write(R_CONTROL, CTL_POP_IN);
        axi_write(R_OUT_PUSH, 8'h00);
        axi_write(R_CONTROL, CTL_SET_READY | CTL_SET_DIRECT |
                             CTL_ACK_EXEC | CTL_CLR_IN);
        vtw_access(T_CTRL, 1'b1, 11'h7F1, 8'h00, rd);
        check(rd[7:6] == 2'b11,
              "vTW CTRL reports ready plus completed direct copy");
        vtw_access(T_DATA, 1'b1, 11'h7F0, 8'h00, rd);
        check(rd == 8'h00, "direct response retains its result byte");
        vtw_access(T_DPOP, 1'b0, 11'h7F2, 8'h00, rd);
        axi_read(R_STATUS, st);
        check(st_out(st) == 0,
              "direct response has no hidden 512-byte FIFO payload");

        // Native DATA/DPOP also crosses a packed-word boundary. Each Apple
        // cycle leaves ample time for the registered BRAM word to catch up.
        axi_write(R_CONTROL, CTL_CLR_OUT);
        axi_write(R_OUT_PUSH4, 32'h14131211);
        axi_write(R_OUT_PUSH, 8'h15);
        for (int i = 1; i <= 5; i++) begin
            apple_read(16'hCFF0, rd);
            check(rd == 8'(8'h10 + i),
                  $sformatf("native response byte %0d crosses word boundary (got %02h)",
                            i, rd));
            apple_write(16'hCFF2, 8'h00);
        end
        axi_read(R_STATUS, st);
        check(st_out(st) == 0, "native word plus scalar tail drains exactly");

        // ---- 5. Bus path still works after fast-port traffic ----
        sss.sw_ramworks_bank = 7'h12;
        sss.sw_ramwrt = 1'b1;
        apple_write(16'hCFF0, 8'h99);
        axi_read(R_STATUS, st);
        check(st_in(st) == 1, "bus-path DATA write still reaches the IN FIFO");
        apple_write(16'hCFF1, 8'h01);
        axi_read(R_STATUS, st);
        check(st_exec(st) && !st_exec_vtw(st),
              "native command is not tagged for packed acceleration");
        axi_read(R_SSS, st);
        check(!st[21] && st[20:14] == 7'h12 && st[2],
              "native command keeps the motherboard switch snapshot");
        axi_write(R_CONTROL, CTL_POP_IN);
        axi_write(R_OUT_PUSH, 8'h00);
        axi_write(R_CONTROL, CTL_SET_READY | CTL_SET_DIRECT |
                             CTL_ACK_EXEC | CTL_CLR_IN);
        vtw_access(T_CTRL, 1'b1, 11'h7F1, 8'h00, rd);
        check(rd[7] && !rd[6],
              "native command cannot acquire the direct-copy flag");

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
