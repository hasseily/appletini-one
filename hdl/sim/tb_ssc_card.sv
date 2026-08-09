`timescale 1ns / 1ps

// ssc_card behavioral checks: ROM serving (slot page + owned $C800 window),
// DEVSEL register map, printer FIFO push/pop/overflow, and decode
// disjointness with the Uthernet II half of slot 1.
module tb_ssc_card;
    logic clk = 1'b0;
    logic rstn = 1'b0;
    always #3.75 clk = ~clk;

    globals::AppleBus_read ab_read;
    globals::SoftSwitchState sss;
    globals::AppleBus_write ab_write;

    logic [11:0] tx_count;
    logic [7:0]  tx_head;
    logic        tx_head_valid;
    logic        tx_pop = 1'b0;
    logic        tx_clear = 1'b0;
    logic        tx_overflow;
    logic        tx_overflow_clear = 1'b0;
    logic [7:0]  acia_command;
    logic [7:0]  acia_control;

    ssc_card dut (
        .clk(clk),
        .rstn(rstn),
        .ab_read(ab_read),
        .sss(sss),
        .slot_assign(3'h1),
        .ab_write(ab_write),
        .tx_count(tx_count),
        .tx_head(tx_head),
        .tx_head_valid(tx_head_valid),
        .tx_pop(tx_pop),
        .tx_clear(tx_clear),
        .tx_overflow(tx_overflow),
        .tx_overflow_clear(tx_overflow_clear),
        .acia_command(acia_command),
        .acia_control(acia_control)
    );

    // One bus read as the card sees it. The card serves on a one-clock
    // delayed serve_en (registered BRAM), so sample two clocks after the
    // strobe, then finish the cycle with data_en so wr_data_en clears.
    logic [7:0] rd;
    logic rd_valid;
    task automatic bus_read(input logic [15:0] addr);
        @(negedge clk);
        ab_read.addr = addr;
        ab_read.rw = 1'b1;
        ab_read.serve_en = 1'b1;
        @(negedge clk);
        ab_read.serve_en = 1'b0;
        repeat (2) @(negedge clk);
        rd = ab_write.wr_data;
        rd_valid = ab_write.wr_data_en;
        ab_read.data_en = 1'b1;
        @(negedge clk);
        ab_read.data_en = 1'b0;
        @(negedge clk);
    endtask

    task automatic bus_write(input logic [15:0] addr, input logic [7:0] data);
        @(negedge clk);
        ab_read.addr = addr;
        ab_read.rw = 1'b0;
        ab_read.serve_en = 1'b1;
        @(negedge clk);
        ab_read.serve_en = 1'b0;
        @(negedge clk);
        ab_read.data = data;
        ab_read.data_en = 1'b1;
        @(negedge clk);
        ab_read.data_en = 1'b0;
        ab_read.rw = 1'b1;
        @(negedge clk);
    endtask

    task automatic pop_one;
        @(negedge clk);
        tx_pop = 1'b1;
        @(negedge clk);
        tx_pop = 1'b0;
        repeat (2) @(negedge clk);
    endtask

    task automatic expect_read(input logic [15:0] addr,
                               input logic [7:0] value,
                               input string what);
        bus_read(addr);
        if (!rd_valid) $fatal(1, "%s: no response at %04x", what, addr);
        if (rd !== value)
            $fatal(1, "%s: read %02x expected %02x", what, rd, value);
    endtask

    task automatic expect_no_response(input logic [15:0] addr,
                                      input string what);
        bus_read(addr);
        if (rd_valid) $fatal(1, "%s: unexpected response at %04x", what, addr);
    endtask

    initial begin
        ab_read = '0;
        sss = '0;
        ab_read.res = 1'b1;
        ab_read.cycle_valid = 1'b1;
        ab_read.rw = 1'b1;
        sss.slot_access = 1'b1;

        repeat (4) @(posedge clk);
        rstn = 1'b1;
        repeat (4) @(posedge clk);

        // Slot ROM: the real SSC entry and Pascal signature bytes.
        expect_read(16'hC100, 8'h2C, "slot ROM entry");
        expect_read(16'hC105, 8'h38, "Pascal signature C105");
        expect_read(16'hC107, 8'h18, "Pascal signature C107");
        expect_read(16'hC10B, 8'h01, "generic signature");
        expect_read(16'hC10C, 8'h31, "serial device signature");

        // DEVSEL: DIP switches, ACIA idle status, latch write/readback.
        expect_read(16'hC091, 8'hE2, "DIPSW1");
        expect_read(16'hC092, 8'h20, "DIPSW2");
        expect_read(16'hC099, 8'h10, "ACIA status");
        expect_read(16'hC098, 8'h00, "ACIA receive data");
        bus_write(16'hC09A, 8'h0B);
        bus_write(16'hC09B, 8'h1E);
        expect_read(16'hC09A, 8'h0B, "ACIA command latch");
        expect_read(16'hC09B, 8'h1E, "ACIA control latch");
        expect_read(16'hC09E, 8'h0B, "ACIA command alias C09E");
        expect_read(16'hC09D, 8'h10, "ACIA status alias C09D");
        if (acia_command !== 8'h0B || acia_control !== 8'h1E)
            $fatal(1, "ACIA latch taps wrong");

        // Programmed reset keeps only the parity bits of the command latch.
        bus_write(16'hC09A, 8'hFF);
        bus_write(16'hC099, 8'h00);
        expect_read(16'hC09A, 8'hE0, "ACIA programmed reset");

        // The Uthernet II half of the DEVSEL page must stay silent, and so
        // must the unused offsets.
        expect_no_response(16'hC094, "Uthernet mode reg");
        expect_no_response(16'hC095, "Uthernet addr hi");
        expect_no_response(16'hC096, "Uthernet addr lo");
        expect_no_response(16'hC097, "Uthernet data reg");
        expect_no_response(16'hC090, "unused C0n0");
        expect_no_response(16'hC093, "unused C0n3");

        // Printer bytes queue in order; DIP/latch writes do not.
        bus_write(16'hC098, 8'hC1);
        bus_write(16'hC09C, 8'hC2);       // TDR alias
        bus_write(16'hC091, 8'hAA);       // read-only, ignored
        repeat (2) @(posedge clk);
        if (tx_count !== 12'd2) $fatal(1, "tx_count=%0d expected 2", tx_count);
        if (!tx_head_valid || tx_head !== 8'hC1)
            $fatal(1, "tx head %02x valid %0b expected C1", tx_head, tx_head_valid);
        pop_one();
        if (tx_count !== 12'd1 || tx_head !== 8'hC2)
            $fatal(1, "after pop: count=%0d head=%02x", tx_count, tx_head);
        pop_one();
        if (tx_count !== 12'd0 || tx_head_valid)
            $fatal(1, "FIFO should be empty");

        // Fill to 2048, overflow is sticky, the extra byte is dropped.
        for (int i = 0; i < 2048; i++) begin
            bus_write(16'hC098, i[7:0]);
        end
        if (tx_count !== 12'd2048) $fatal(1, "FIFO fill count=%0d", tx_count);
        if (tx_overflow) $fatal(1, "overflow set before FIFO was full");
        bus_write(16'hC098, 8'h55);
        repeat (2) @(posedge clk);
        if (!tx_overflow) $fatal(1, "overflow not sticky after drop");
        if (tx_count !== 12'd2048) $fatal(1, "dropped byte changed count");
        @(negedge clk);
        tx_overflow_clear = 1'b1;
        @(negedge clk);
        tx_overflow_clear = 1'b0;
        @(negedge clk);
        if (tx_overflow) $fatal(1, "overflow clear failed");
        @(negedge clk);
        tx_clear = 1'b1;
        @(negedge clk);
        tx_clear = 1'b0;
        repeat (2) @(negedge clk);
        if (tx_count !== 12'd0) $fatal(1, "FIFO clear failed");

        // $C800 window: served only while this slot owns it, never $CFFF,
        // never under INTCXROM, and ROM-space writes push nothing.
        expect_no_response(16'hC800, "C8 without ownership");
        sss.io_select = 8'h02;
        expect_read(16'hC800, 8'h20, "C8 ROM first byte");
        expect_no_response(16'hCFFF, "CFFF release address");
        sss.sw_intcxrom = 1'b1;
        expect_no_response(16'hC800, "C8 under INTCXROM");
        sss.sw_intcxrom = 1'b0;
        bus_write(16'hCFF9, 8'h00);       // BENTER: ROM-space write
        bus_write(16'hC1F0, 8'h00);       // slot-ROM write
        repeat (2) @(posedge clk);
        if (tx_count !== 12'd0) $fatal(1, "ROM-space write reached the FIFO");
        sss.io_select = 8'h00;

        // Apple reset clears the ACIA latches but preserves queued bytes.
        bus_write(16'hC098, 8'h41);
        bus_write(16'hC09A, 8'h0B);
        @(negedge clk);
        ab_read.res = 1'b0;
        repeat (3) @(negedge clk);
        ab_read.res = 1'b1;
        repeat (2) @(negedge clk);
        if (acia_command !== 8'h00) $fatal(1, "reset kept the command latch");
        if (tx_count !== 12'd1) $fatal(1, "reset dropped queued print data");

        $display("SSC CARD PASS");
        $finish;
    end
endmodule
