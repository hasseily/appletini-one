`timescale 1ns / 1ps

// Self-checking testbench for applicard_card (PCPI Appli-Card front end).
// Exercises every 6502-visible register semantic in README_APPLICARD.md
// §1.1 plus the AxiSimple STATUS/TO6502/CONTROL contract used by
// applicard_service.c.
module tb_applicard_card;
    logic clk = 1'b0;
    logic rstn = 1'b0;
    always #5 clk = ~clk;

    globals::AppleBus_read ab_read;
    globals::AppleBus_write ab_write;
    globals::AxiSimple_common as_common;
    AxiSimple_if axi();

    localparam logic [15:0] BASE = 16'hC0D0; // slot 5

    // AxiSimple register indices (match applicard_card.sv / applicard_regs.h)
    localparam logic [7:0] REG_STATUS  = 8'h00;
    localparam logic [7:0] REG_TO6502  = 8'h01;
    localparam logic [7:0] REG_CONTROL = 8'h02;
    localparam logic [7:0] REG_DEBUG   = 8'h03;

    applicard_card dut (
        .clk(clk),
        .rstn(rstn),
        .ab_read(ab_read),
        .slot_assign(3'h5),
        .as_common(as_common),
        .as_client(axi),
        .ab_write(ab_write)
    );

    // 6502 read: pulse sss_en for one clk with addr/rw set; the card
    // registers its response, so sample ab_write on the following edge.
    task automatic apple_read(input logic [15:0] addr, output logic [7:0] data);
        @(negedge clk);
        ab_read.addr = addr;
        ab_read.rw = 1'b1;
        ab_read.sss_en = 1'b1;
        @(negedge clk);
        ab_read.sss_en = 1'b0;
        if (!ab_write.wr_data_en)
            $fatal(1, "read $%04X: wr_data_en not asserted", addr);
        data = ab_write.wr_data;
    endtask

    task automatic apple_write(input logic [15:0] addr, input logic [7:0] data);
        @(negedge clk);
        ab_read.addr = addr;
        ab_read.rw = 1'b0;
        ab_read.data = data;
        ab_read.data_en = 1'b1;
        @(negedge clk);
        ab_read.data_en = 1'b0;
    endtask

    task automatic axi_write(input logic [7:0] idx, input logic [31:0] value);
        @(negedge clk);
        as_common.awaddr = idx;
        as_common.wdata = value;
        as_common.wstrb = 4'hF;
        axi.awvalid = 1'b1;
        @(negedge clk);
        axi.awvalid = 1'b0;
    endtask

    // araddr is registered inside the card (axidouble OPT_REGISTERED
    // contract), so allow one clk before sampling rdata.
    task automatic axi_read(input logic [7:0] idx, output logic [31:0] value);
        @(negedge clk);
        as_common.araddr = idx;
        @(negedge clk);
        @(negedge clk);
        value = axi.rdata;
    endtask

    task automatic expect_read(input logic [15:0] addr, input logic [7:0] want,
                               input string what);
        logic [7:0] got;
        apple_read(addr, got);
        if (got !== want)
            $fatal(1, "%s: read $%04X got %02X want %02X", what, addr, got, want);
    endtask

    logic [7:0]  rd;
    logic [31:0] status;
    logic [7:0]  seq;

    initial begin
        ab_read = '0;
        ab_read.res = 1'b1;
        ab_read.cycle_valid = 1'b1;
        as_common = '0;
        axi.awvalid = 1'b0;

        repeat (3) @(posedge clk);
        rstn = 1'b1;
        repeat (2) @(posedge clk);

        // Idle state: no flags, unused offsets float high.
        expect_read(BASE + 2, 8'h00, "F_Z80 idle");
        expect_read(BASE + 3, 8'h00, "F_6502 idle");
        expect_read(BASE + 4, 8'hFF, "unused offset");
        expect_read(BASE + 8, 8'hFF, "unused offset high");

        // 6502 -> Z80: write latches data, sets F_Z80, bumps seq.
        apple_write(BASE + 1, 8'hA5);
        expect_read(BASE + 1, 8'hA5, "TOZ80 readback");
        expect_read(BASE + 2, 8'h80, "F_Z80 after write");
        axi_read(REG_STATUS, status);
        if (status[0] !== 1'b1) $fatal(1, "STATUS.F_Z80 not set");
        if (status[23:16] !== 8'hA5) $fatal(1, "STATUS data != A5");
        seq = status[15:8];

        // Stale ACK (wrong seq) must not consume.
        axi_write(REG_CONTROL, {16'h0, seq + 8'd1, 8'h01});
        axi_read(REG_STATUS, status);
        if (status[0] !== 1'b1) $fatal(1, "stale ACK consumed TOZ80");

        // Correct ACK consumes.
        axi_write(REG_CONTROL, {16'h0, seq, 8'h01});
        axi_read(REG_STATUS, status);
        if (status[0] !== 1'b0) $fatal(1, "ACK did not clear F_Z80");
        expect_read(BASE + 2, 8'h00, "F_Z80 after ACK");
        expect_read(BASE + 1, 8'hA5, "TOZ80 readback survives ACK");

        // Z80 -> 6502: PS deposit sets F_6502; 6502 read consumes.
        axi_write(REG_TO6502, 32'h0000_005A);
        expect_read(BASE + 3, 8'h80, "F_6502 after deposit");
        expect_read(BASE + 0, 8'h5A, "TO6502 data");
        expect_read(BASE + 3, 8'h00, "F_6502 consumed by read");
        axi_read(REG_STATUS, status);
        if (status[1] !== 1'b0) $fatal(1, "STATUS.F_6502 not cleared");

        // $C0D5 (read) resets: latches/flags cleared in-cycle, RESET_REQ sticky.
        apple_write(BASE + 1, 8'h11);
        axi_write(REG_TO6502, 32'h0000_0022);
        apple_read(BASE + 5, rd);
        expect_read(BASE + 1, 8'h00, "TOZ80 cleared by reset");
        expect_read(BASE + 0, 8'h00, "TO6502 cleared by reset");
        expect_read(BASE + 2, 8'h00, "F_Z80 cleared by reset");
        expect_read(BASE + 3, 8'h00, "F_6502 cleared by reset");
        axi_read(REG_STATUS, status);
        if (status[2] !== 1'b1) $fatal(1, "RESET_REQ not set by $C0D5 read");
        axi_write(REG_CONTROL, 32'h0000_0002);
        axi_read(REG_STATUS, status);
        if (status[2] !== 1'b0) $fatal(1, "RESET_REQ not cleared by CONTROL");

        // $C0D5 as a write triggers the same reset.
        apple_write(BASE + 1, 8'h33);
        apple_write(BASE + 5, 8'h00);
        expect_read(BASE + 2, 8'h00, "F_Z80 cleared by reset write");
        axi_read(REG_STATUS, status);
        if (status[2] !== 1'b1) $fatal(1, "RESET_REQ not set by $C0D5 write");
        axi_write(REG_CONTROL, 32'h0000_0002);

        // $C0D7 latches NMI_REQ (read or write), CONTROL bit2 clears it.
        apple_read(BASE + 7, rd);
        axi_read(REG_STATUS, status);
        if (status[3] !== 1'b1) $fatal(1, "NMI_REQ not set by $C0D7 read");
        axi_write(REG_CONTROL, 32'h0000_0004);
        axi_read(REG_STATUS, status);
        if (status[3] !== 1'b0) $fatal(1, "NMI_REQ not cleared");
        apple_write(BASE + 7, 8'h00);
        axi_read(REG_STATUS, status);
        if (status[3] !== 1'b1) $fatal(1, "NMI_REQ not set by $C0D7 write");
        axi_write(REG_CONTROL, 32'h0000_0004);

        // A stale ACK from before a $C0D5 reset must not consume a byte
        // written after it (seq bumped by the reset).
        apple_write(BASE + 1, 8'h44);
        axi_read(REG_STATUS, status);
        seq = status[15:8];
        apple_read(BASE + 5, rd);          // reset bumps seq, clears F_Z80
        axi_write(REG_CONTROL, 32'h0000_0002);
        apple_write(BASE + 1, 8'h55);      // fresh byte, new seq
        axi_write(REG_CONTROL, {16'h0, seq, 8'h01}); // pre-reset seq: stale
        axi_read(REG_STATUS, status);
        if (status[0] !== 1'b1) $fatal(1, "stale pre-reset ACK consumed byte");
        axi_write(REG_CONTROL, {16'h0, status[15:8], 8'h01});

        // Another slot's I/O space is ignored.
        apple_write(16'hC0C1, 8'hEE);      // slot 4 window
        axi_read(REG_STATUS, status);
        if (status[0] !== 1'b0) $fatal(1, "foreign slot write latched");

        // Apple bus reset raises RESET_REQ and clears everything.
        axi_write(REG_TO6502, 32'h0000_0077);
        @(negedge clk);
        ab_read.res = 1'b0;
        repeat (3) @(negedge clk);
        ab_read.res = 1'b1;
        repeat (2) @(negedge clk);
        axi_read(REG_STATUS, status);
        if (status[2] !== 1'b1) $fatal(1, "RESET_REQ not set by bus reset");
        if (status[1] !== 1'b0) $fatal(1, "F_6502 not cleared by bus reset");
        expect_read(BASE + 3, 8'h00, "F_6502 after bus reset");
        axi_write(REG_CONTROL, 32'h0000_0002);

        // DEBUG register reflects both latches.
        apple_write(BASE + 1, 8'hC3);
        axi_write(REG_TO6502, 32'h0000_003C);
        axi_read(REG_DEBUG, status);
        if (status[15:8] !== 8'hC3) $fatal(1, "DEBUG.TOZ80 mismatch");
        if (status[7:0] !== 8'h3C) $fatal(1, "DEBUG.TO6502 mismatch");
        if (status[16] !== 1'b1 || status[17] !== 1'b1)
            $fatal(1, "DEBUG flags mismatch");

        $display("tb_applicard_card: all checks passed");
        $finish;
    end
endmodule
