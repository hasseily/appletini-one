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
    logic card_enabled = 1'b1;
    logic disk2_timing_active = 1'b0;
    logic vtw_enabled = 1'b0;
    logic vtw_bus_owned = 1'b0;

    localparam logic [15:0] BASE = 16'hC0D0; // slot 5

    // AxiSimple register indices (match applicard_card.sv / applicard_regs.h)
    localparam logic [7:0] REG_STATUS  = 8'h00;
    localparam logic [7:0] REG_TO6502  = 8'h01;
    localparam logic [7:0] REG_CONTROL = 8'h02;
    localparam logic [7:0] REG_DEBUG   = 8'h03;
    localparam logic [7:0] REG_MODE    = 8'h04;
    localparam logic [7:0] REG_AD_STATUS = 8'h05;
    localparam logic [7:0] REG_AD_CONTROL = 8'h06;
    localparam logic [7:0] REG_AD_PORTS3 = 8'h0A;
    localparam logic [7:0] REG_BUS_COMMAND = 8'h0B;
    localparam logic [7:0] REG_BUS_STATUS = 8'h0C;

    applicard_card dut (
        .clk(clk),
        .rstn(rstn),
        .ab_read(ab_read),
        .card_enabled(card_enabled),
        .disk2_timing_active(disk2_timing_active),
        .vtw_enabled(vtw_enabled),
        .vtw_bus_owned(vtw_bus_owned),
        .slot_assign(3'h5),
        .as_common(as_common),
        .as_client(axi),
        .ab_write(ab_write)
    );

    // 6502 read: serve_en is only the early decode pulse. The response must
    // remain driven until the later data_en sample, not disappear one fabric
    // clock after serve_en.
    task automatic apple_read(input logic [15:0] addr, output logic [7:0] data);
        @(negedge clk);
        ab_read.addr = addr;
        ab_read.rw = 1'b1;
        ab_read.serve_en = 1'b1;
        @(negedge clk);
        ab_read.serve_en = 1'b0;
        if (!ab_write.wr_data_en)
            $fatal(1, "read $%04X: wr_data_en not asserted", addr);
        data = ab_write.wr_data;
        repeat (4) @(negedge clk);
        if (!ab_write.wr_data_en || ab_write.wr_data !== data)
            $fatal(1, "read $%04X: response was not held to data_en", addr);
        ab_read.data_en = 1'b1;
        @(negedge clk);
        ab_read.data_en = 1'b0;
        repeat (2) @(negedge clk);
        if (ab_write.wr_data_en)
            $fatal(1, "read $%04X: response remained driven after data_en", addr);
    endtask

    task automatic pulse_drive;
        @(negedge clk);
        ab_read.drive_en = 1'b1;
        @(negedge clk);
        ab_read.drive_en = 1'b0;
        repeat (2) @(negedge clk);
    endtask

    task automatic pulse_recovery;
        repeat (2) begin
            pulse_drive();
            if (ab_write.assert_dma)
                $fatal(1, "DMA asserted during mandatory CPU recovery");
        end
    endtask

    task automatic pulse_data(input logic [7:0] data);
        @(negedge clk);
        ab_read.data = data;
        ab_read.data_en = 1'b1;
        @(negedge clk);
        ab_read.data_en = 1'b0;
        repeat (2) @(negedge clk);
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

        // Select AD8088. Mode switching is a hard reset boundary and the
        // clean-room monitor starts ready (Apple port 0 bit 7 set).
        axi_write(REG_MODE, 32'h0000_0001);
        axi_read(REG_MODE, status);
        if (status[0] !== 1'b1) $fatal(1, "AD8088 mode did not latch");
        expect_read(BASE + 0, 8'h80, "AD8088 ready after mode switch");
        expect_read(BASE + 1, 8'hFF, "AD8088 ports 1-15 unreadable");

        // Parameter writes remain independently latched. A command write
        // clears ready and posts one sequence-numbered event to the PS.
        apple_write(BASE + 14, 8'h34);
        apple_write(BASE + 15, 8'h12);
        apple_write(BASE + 0, 8'hFC);
        expect_read(BASE + 0, 8'h00, "AD8088 busy after command");
        axi_read(REG_AD_STATUS, status);
        if (status[0] !== 1'b1 || status[23:16] !== 8'hFC ||
            status[11:8] !== 4'h0)
            $fatal(1, "AD8088 command event/status mismatch: %08X", status);
        seq = status[31:24];
        axi_read(REG_AD_PORTS3, status);
        if (status[31:16] !== 16'h1234)
            $fatal(1, "AD8088 Address B parameter mismatch: %08X", status);

        // A stale command ACK cannot consume a newer command.
        axi_write(REG_AD_CONTROL, {16'h0, seq + 8'd1, 8'h03});
        axi_read(REG_AD_STATUS, status);
        if (status[0] !== 1'b1) $fatal(1, "stale AD8088 ACK consumed command");
        axi_write(REG_AD_CONTROL, {16'h0, seq, 8'h03});
        expect_read(BASE + 0, 8'h80, "AD8088 ready after command finish");
        axi_read(REG_AD_STATUS, status);
        if (status[0] !== 1'b0) $fatal(1, "AD8088 ACK did not clear command");

        // A long monitor operation marks the card running+busy. An early
        // nonzero port-0 write must not replace the operation with a second
        // command. A zero remains the explicit abort path.
        axi_write(REG_AD_CONTROL, 32'h0000_000C); // running + flag clear
        apple_write(BASE + 0, 8'hFE);
        axi_read(REG_AD_STATUS, status);
        if (status[0] !== 1'b0 || status[3] !== 1'b0 || status[2] !== 1'b1)
            $fatal(1, "busy monitor accepted overlapping command: %08X",
                   status);
        apple_write(BASE + 0, 8'h00);
        axi_read(REG_AD_STATUS, status);
        if (status[3] !== 1'b1)
            $fatal(1, "busy monitor did not accept explicit abort");
        axi_write(REG_AD_CONTROL, 32'h0000_0040); // monitor reset

        // Running-program handshake: 8088 OUT 0 raises the Apple-visible
        // flag, and any Apple write acknowledges it without posting a PROM
        // command. Zero only stops a program when no signal is pending.
        axi_write(REG_AD_CONTROL, 32'h0000_000C); // running + flag clear
        axi_write(REG_AD_CONTROL, 32'h0000_0002); // OUT 0 signal
        expect_read(BASE + 0, 8'h80, "AD8088 user signal");
        apple_write(BASE + 0, 8'h55);
        expect_read(BASE + 0, 8'h00, "AD8088 signal ACK");
        axi_read(REG_AD_STATUS, status);
        if (status[0] !== 1'b0 || status[3] !== 1'b0)
            $fatal(1, "signal ACK incorrectly posted command/abort");
        axi_write(REG_AD_CONTROL, 32'h0000_0002); // another OUT 0 signal
        apple_write(BASE + 0, 8'h00);
        axi_read(REG_AD_STATUS, status);
        if (status[3] !== 1'b0 || status[1] !== 1'b0)
            $fatal(1, "zero did not acknowledge signalled AD8088 program");
        apple_write(BASE + 0, 8'h00);
        axi_read(REG_AD_STATUS, status);
        if (status[3] !== 1'b1)
            $fatal(1, "zero without a signal did not stop AD8088 program");
        axi_write(REG_AD_CONTROL, 32'h0000_0040); // monitor reset

        // One sparse Apple-memory read: the cut acquisition cycle parks a
        // few fabric clocks after /DMA, a second parked cycle follows, then
        // the requested cycle and a parked release cycle whose late data
        // strobe drops the drivers before /DMA returns at the next drive
        // point. No owned cycle floats.
        axi_write(REG_BUS_COMMAND, 32'h8100_1234);
        pulse_drive();
        if (!ab_write.assert_dma) $fatal(1, "bus read did not assert DMA");
        repeat (10) @(negedge clk);
        if (!ab_write.wr_addr_rw_en || ab_write.wr_addr !== 16'h0200 ||
            !ab_write.wr_rw)
            $fatal(1, "cut cycle did not park after the buffer-release delay");
        pulse_drive();
        if (!ab_write.wr_addr_rw_en || ab_write.wr_addr !== 16'h0200 ||
            !ab_write.wr_rw)
            $fatal(1, "bus read did not park during second DMA grace cycle");
        pulse_drive();
        if (!ab_write.wr_addr_rw_en || ab_write.wr_addr !== 16'h1234 ||
            !ab_write.wr_rw)
            $fatal(1, "bus read address/RW not driven");
        pulse_data(8'h5A);
        pulse_drive();
        if (!ab_write.wr_addr_rw_en || ab_write.wr_addr !== 16'h0200 ||
            !ab_write.wr_rw || !ab_write.assert_dma)
            $fatal(1, "bus read did not park before release");
        pulse_data(8'hFF);
        if (ab_write.wr_addr_rw_en || !ab_write.assert_dma)
            $fatal(1, "bus read did not release address before DMA");
        pulse_drive();
        if (ab_write.assert_dma)
            $fatal(1, "bus read did not release DMA at the next drive point");
        axi_read(REG_BUS_STATUS, status);
        if (!status[9] || status[10] || status[7:0] !== 8'h5A)
            $fatal(1, "bus read completion mismatch: %08X", status);
        pulse_recovery();

        // A motherboard-RAM write carries a dedicated timing request rather
        // than the ordinary late card-response enable. Pin-level timing is
        // checked in tb_applicard_bus_master.
        axi_write(REG_BUS_COMMAND, 32'h80A5_2345);
        pulse_drive();
        pulse_drive();
        pulse_drive();
        if (!ab_write.wr_addr_rw_en || ab_write.wr_rw ||
            !ab_write.wr_dma_data_en || ab_write.wr_data_en ||
            ab_write.wr_addr !== 16'h2345 || ab_write.wr_data !== 8'hA5)
            $fatal(1, "bus write did not present timed DMA payload");
        pulse_data(8'hA5);
        pulse_drive();
        if (!ab_write.wr_addr_rw_en || ab_write.wr_addr !== 16'h0200 ||
            !ab_write.wr_rw || ab_write.wr_dma_data_en ||
            !ab_write.assert_dma)
            $fatal(1, "bus write did not park before release");
        pulse_data(8'hFF);
        if (ab_write.wr_addr_rw_en || ab_write.wr_dma_data_en ||
            !ab_write.assert_dma)
            $fatal(1, "bus write did not release address before DMA");
        pulse_drive();
        if (ab_write.assert_dma)
            $fatal(1, "bus write did not release DMA at the next drive point");
        axi_read(REG_BUS_STATUS, status);
        if (!status[9] || status[10])
            $fatal(1, "bus write completion mismatch: %08X", status);

        // Disk II/vTW interlock rejects rather than beginning an unsafe DMA
        // access. The PS can defer the 8088 slice and retry later.
        disk2_timing_active = 1'b1;
        axi_write(REG_BUS_COMMAND, 32'h8100_5678);
        axi_read(REG_BUS_STATUS, status);
        if (!status[9] || !status[10] || !status[11])
            $fatal(1, "blocked bus command not rejected safely: %08X", status);
        disk2_timing_active = 1'b0;

        vtw_enabled = 1'b1;
        axi_write(REG_BUS_COMMAND, 32'h8100_5678);
        /* Drop the blocker BEFORE reading completion: the blocked verdict
         * must be latched per transaction, not sampled live, or a short
         * blocking pulse reads back as a hard error and kills a running
         * 8088 program. */
        vtw_enabled = 1'b0;
        axi_read(REG_BUS_STATUS, status);
        if (!status[9] || !status[10] || !status[11])
            $fatal(1, "enabled-vTW bus command not rejected safely: %08X",
                   status);
        pulse_recovery();

        // If a higher-priority owner appears during a bulk transfer, finish
        // the current byte and use the same guarded release path as cancel.
        axi_write(REG_BUS_COMMAND, 32'hC103_6000);
        pulse_drive();
        pulse_drive();
        pulse_drive();
        pulse_data(8'h6A);
        disk2_timing_active = 1'b1;
        pulse_drive();
        if (!ab_write.assert_dma || !ab_write.wr_addr_rw_en ||
            ab_write.wr_addr !== 16'h0200 || !ab_write.wr_rw)
            $fatal(1, "mid-bulk block skipped release park");
        pulse_data(8'hFF);
        if (!ab_write.assert_dma || ab_write.wr_addr_rw_en)
            $fatal(1, "mid-bulk block did not release address before DMA");
        pulse_drive();
        if (ab_write.assert_dma)
            $fatal(1, "mid-bulk block did not release DMA at the drive point");
        /* The Disk II window has already closed when the PS reads the
         * completion; the blocked classification must survive it. */
        disk2_timing_active = 1'b0;
        axi_read(REG_BUS_STATUS, status);
        if (!status[9] || !status[10] || status[8])
            $fatal(1, "mid-bulk block completion mismatch: %08X", status);
        if (!status[11])
            $fatal(1, "mid-bulk block lost its blocked verdict: %08X",
                   status);
        pulse_recovery();

        // A PS timeout must keep /DMA asserted until the guarded release
        // boundary. It may not drop ownership in the middle of a cycle.
        axi_write(REG_BUS_COMMAND, 32'h8100_9ABC);
        pulse_drive();
        if (!ab_write.assert_dma) $fatal(1, "cancel setup did not assert DMA");
        axi_write(REG_AD_CONTROL, 32'h0000_0080);
        if (!ab_write.assert_dma)
            $fatal(1, "bus cancel released DMA before its guard boundary");
        pulse_drive();
        if (!ab_write.assert_dma || !ab_write.wr_addr_rw_en ||
            ab_write.wr_addr !== 16'h0200)
            $fatal(1, "bus cancel skipped the parked release cycle");
        pulse_data(8'hFF);
        if (ab_write.wr_addr_rw_en || !ab_write.assert_dma)
            $fatal(1, "bus cancel did not release drivers before DMA");
        pulse_drive();
        if (ab_write.assert_dma || ab_write.wr_addr_rw_en ||
            ab_write.wr_data_en || ab_write.wr_dma_data_en)
            $fatal(1, "bus cancel did not release all Apple bus drives");
        axi_read(REG_BUS_STATUS, status);
        if (!status[9] || !status[10] || status[8])
            $fatal(1, "bus cancel completion mismatch: %08X", status);
        if (status[11])
            $fatal(1, "PS cancel wrongly classified as blocked: %08X",
                   status);

        $display("tb_applicard_card: all checks passed");
        $finish;
    end
endmodule
