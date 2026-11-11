`timescale 1ns / 1ps

module tb_iigs_video_timing;
    import globals::*;
    logic clk = 0;
    always #5 clk = !clk;
    logic resetn = 0, enabled = 1;
    AppleBus_read ab_read = '0;
    SoftSwitchState sss;
    logic [7:0] c022, c029, c02b;
    logic [3:0] border;
    logic phase_locked, update_pulse;
    logic [8:0] line_in_frame;
    logic [6:0] cycle_in_line;

    apple_iigs_video_timing #(.CLOCK_TIMEOUT_BITS(4)) dut (.*);
    soft_switch_manager switches (
        .clk, .rstn(resetn), .ramworks_en(1'b1), .iigs_profile(enabled),
        .iigs_bootstrap_slot7(1'b0), .iigs_boot_mask(8'h00),
        .iigs_boot_mask_valid(1'b0), .iigs_native_video(enabled),
        .iigs_bank_latch(c029[0]), .ab_read, .sss,
        .iigs_live_mask(), .iigs_live_mask_valid(), .iigs_io_mask(), .iigs_rom_mask()
    );

    task automatic check(input logic ok, input string message);
        if (ok !== 1'b1) $fatal(1, "%s", message);
    endtask

    task automatic tick;
        ab_read.phi0_tick = 1;
        @(negedge clk);
        check(update_pulse, "PHI0 advances even without a qualified access");
        ab_read.phi0_tick = 0;
    endtask

    task automatic bus_access(input logic [15:0] addr, input logic rw,
                              input logic [7:0] data,
                              input logic m2sel = 0, input logic m2b0 = 0);
        ab_read.addr = addr;
        ab_read.addr_early = addr;
        ab_read.rw = rw;
        ab_read.rw_early = rw;
        ab_read.m2sel = m2sel;
        ab_read.m2b0 = m2b0;
        ab_read.cycle_valid = !m2sel;
        ab_read.data = data;
        tick();
        ab_read.addr_en = 1;
        ab_read.serve_en = 1;
        @(negedge clk);
        ab_read.addr_en = 0;
        ab_read.serve_en = 0;
        ab_read.data_en = 1;
        @(negedge clk);
        ab_read.data_en = 0;
    endtask

    task automatic bracket(input logic [8:0] vertical,
                           input logic [6:0] first_h, input logic [6:0] last_h);
        bus_access(16'hC02F, 1, {vertical[0], first_h});
        bus_access(16'hC02E, 1, vertical[8:1]);
        bus_access(16'hC02F, 1, {vertical[0], last_h});
    endtask

    task automatic memory_write(input logic [15:0] addr, input logic m2b0,
                                input logic [7:0] bank);
        bus_access(addr, 0, 8'h42, 0, m2b0);
        check(sss.addr_decode_late_en && sss.addr_decode_late == {bank, addr},
              $sformatf("incorrect native bank at %04x: %06x", addr, sss.addr_decode_late));
    endtask

    initial begin
        repeat (2) @(negedge clk);
        resetn = 1;
        ab_read.res = 1;
        bus_access(16'hC022, 0, 8'hAB);
        bus_access(16'hC034, 0, 8'h97);
        bus_access(16'hC029, 0, 8'hFF);
        bus_access(16'hC022, 0, 8'h99, 1);
        check(c022 == 8'hAB && border == 4'h7 && c029 == 8'hE1,
              "qualified native display controls");
        memory_write(16'h4000, 0, 0);
        memory_write(16'h4000, 1, 1);
        check(sss.addr_decode[23:16] == 8'h00, "M2B0 does not alter early decode");
        bus_access(16'hC029, 0, 8'h80);
        memory_write(16'h4000, 1, 0);
        bus_access(16'hC068, 0, 8'h10);
        bus_access(16'hC073, 0, 8'h07);
        memory_write(16'h4000, 0, 1);
        bus_access(16'hC001, 0, 8'h00);
        bus_access(16'hC054, 0, 8'h00);
        memory_write(16'h0400, 1, 0);
        bus_access(16'hC055, 0, 8'h00);
        memory_write(16'h0400, 0, 1);
        bus_access(16'hC057, 0, 8'h00);
        memory_write(16'h2000, 0, 1);
        bus_access(16'hC068, 0, 8'h80);
        memory_write(16'h0100, 0, 1);

        bracket(9'h100, 7'h7F, 7'h41);
        check(!phase_locked, "cross-line bracket with unchanged V parity is incoherent");
        bracket(9'h100, 7'h40, 7'h41);
        check(!phase_locked, "H samples must match elapsed PHI0 ticks");
        bus_access(16'hC02F, 1, 8'h40);
        bus_access(16'hC02E, 1, 8'h80);
        repeat (65) tick();
        bus_access(16'hC02F, 1, 8'h42);
        check(!phase_locked, "stale counter bracket cannot establish phase");
        bus_access(16'hC02F, 1, 8'hFE);
        bus_access(16'hC02E, 1, 8'h7F);
        bus_access(16'hC02F, 1, 8'h00);
        check(!phase_locked, "lock waits for the corrected PHI0 timestamp");
        tick();
        check(phase_locked && line_in_frame == 0 && cycle_in_line == 1,
              "NTSC rollover uses the first H parity when V precedes rollover");
        bus_access(16'hC02F, 1, 8'hFF);
        bus_access(16'hC02E, 1, 8'h80);
        bus_access(16'hC02F, 1, 8'h40);
        tick();
        check(phase_locked && line_in_frame == 0 && cycle_in_line == 2,
              "NTSC rollover uses the second H parity when V follows rollover");
        bracket(9'h100, 7'h00, 7'h41);
        tick();
        check(phase_locked && line_in_frame == 0 && cycle_in_line == 3,
              "H00 sequence aligns NTSC scanline zero");
        ab_read.res = 0;
        tick();
        check(phase_locked && cycle_in_line == 4, "warm reset preserves scanner phase");
        ab_read.res = 1;

        bracket(9'h0FF, 7'h7D, 7'h7F);
        tick();
        check(phase_locked && line_in_frame == 0 && cycle_in_line == 0,
              "NTSC last blanking line wraps to line zero");
        bus_access(16'hC02B, 0, 8'h10);
        check(!phase_locked && c02b[4], "PAL selection invalidates old phase");
        bracket(9'h0C8, 7'h58, 7'h5A);
        tick();
        check(phase_locked && line_in_frame == 256 && cycle_in_line == 28,
              "PAL blanking wrap starts at line 256");
        bracket(9'h0FF, 7'h7D, 7'h7F);
        tick();
        check(phase_locked && line_in_frame == 0 && cycle_in_line == 0,
              "PAL last blanking line wraps to line zero");
        repeat (20) @(negedge clk);
        check(!phase_locked, "clock loss invalidates phase");
        bracket(9'h1C0, 7'h40, 7'h42);
        tick();
        check(phase_locked && line_in_frame == 192 && cycle_in_line == 4,
              "clock recovery anchors GS VBL at line 192");
        $display("IIGS VIDEO TIMING PASS");
        $finish;
    end
endmodule
