`timescale 1ns / 1ps

// Focused test for the vTW Disk II read path. Writes remain physical; the
// private port may only complete a read after the requested track line is in
// the PL cache, and Q7 write mode asks the vTW to keep the CPU at 1 MHz.
module tb_disk2_vtw_read;
    logic clk = 1'b0;
    logic rstn = 1'b0;
    always #3.75 clk = ~clk;

    globals::AppleBus_read ab_read;
    globals::SoftSwitchState sss;
    globals::AppleBus_write ab_write;
    globals::AxiSimple_common as_common;
    AxiSimple_if axi();

    logic [20:0] mc_line_addr;
    logic mc_rw;
    logic [63:0] mc_wdata;
    logic [7:0] mc_wstrb;
    logic mc_valid;
    logic mc_ready = 1'b1;
    logic [63:0] mc_rdata = 64'hA8A7_A6A5_A4A3_A2A1;
    logic mc_rvalid = 1'b0;
    logic mc_read_pending_q = 1'b0;

    logic vtw_req_valid = 1'b0;
    logic rom_serve_en = 1'b0;
    logic [3:0] vtw_req_addr = 4'h0;
    logic vtw_req_ready;
    logic vtw_resp_valid;
    logic [7:0] vtw_resp_rdata;
    logic vtw_tick_extra = 1'b0;
    wire vtw_cycle_tick = vtw_tick_extra;
    logic vtw_native_cycle_active = 1'b0;
    logic vtw_time_ready;
    logic vtw_write_timing_active;
    int disk_tick_count = 0;

    disk2_card dut (
        .clk(clk), .rstn(rstn),
        .ab_read(ab_read), .rom_serve_en(rom_serve_en),
        .sss(sss), .slot_assign(3'd6),
        .as_common(as_common), .as_client(axi),
        .mc_line_addr(mc_line_addr), .mc_rw(mc_rw),
        .mc_wdata(mc_wdata), .mc_wstrb(mc_wstrb),
        .mc_valid(mc_valid), .mc_ready(mc_ready),
        .mc_rdata(mc_rdata), .mc_rvalid(mc_rvalid),
        .ab_write(ab_write),
        .vtw_active(1'b1),
        .vtw_req_valid(vtw_req_valid), .vtw_req_addr(vtw_req_addr),
        .vtw_req_ready(vtw_req_ready),
        .vtw_resp_valid(vtw_resp_valid), .vtw_resp_rdata(vtw_resp_rdata),
        .vtw_cycle_tick(vtw_cycle_tick),
        .vtw_native_cycle_active(vtw_native_cycle_active),
        .vtw_time_ready(vtw_time_ready),
        .vtw_write_timing_active(vtw_write_timing_active),
        .sound_spinning(), .sound_qtrack(), .sound_event(),
        .sound_seek_start_qtrack(), .sound_seek_distance()
    );

    // Return each line one clock after the card's read request is accepted.
    always_ff @(posedge clk) begin
        if (!rstn) begin
            mc_read_pending_q <= 1'b0;
            mc_rvalid <= 1'b0;
            disk_tick_count <= 0;
        end else begin
            mc_rvalid <= mc_read_pending_q;
            mc_read_pending_q <= mc_valid && mc_rw && mc_ready;
            if (dut.disk_cycle_tick)
                disk_tick_count <= disk_tick_count + 1;
            if (vtw_cycle_tick && dut.vtw_io_read)
                $fatal(1, "FAIL: normal and private Disk II ticks overlapped");
        end
    end

    always @(negedge clk) begin
        if (rstn && dut.vtw_drive_select_q !== dut.drive_select_q)
            $fatal(1, "FAIL: vTW drive selector diverged from Disk II state");
        if (rstn && dut.vtw_track_bit_count_q !== dut.track_bit_count_q)
            $fatal(1, "FAIL: vTW bit count diverged from Disk II state");
    end

    task automatic check(input bit cond, input string msg);
        if (!cond)
            $fatal(1, "FAIL: %s", msg);
    endtask

    task automatic axi_write(input logic [7:0] reg_addr,
                             input logic [31:0] value);
        @(negedge clk);
        as_common.awaddr = reg_addr;
        as_common.wdata = value;
        as_common.wstrb = 4'hF;
        axi.awvalid = 1'b1;
        @(negedge clk);
        axi.awvalid = 1'b0;
        repeat (2) @(negedge clk);
    endtask

    task automatic physical_write(input logic [3:0] io_addr,
                                  input logic [7:0] value);
        @(negedge clk);
        ab_read.addr = {12'hC0E, io_addr};
        ab_read.rw = 1'b0;
        ab_read.data = value;
        ab_read.serve_en = 1'b1;
        ab_read.data_en = 1'b0;
        @(negedge clk);
        ab_read.serve_en = 1'b0;
        ab_read.data_en = 1'b1;
        @(negedge clk);
        ab_read.data_en = 1'b0;
        ab_read.rw = 1'b1;
        repeat (2) @(negedge clk);
    endtask

    task automatic physical_tick;
        @(negedge clk);
        ab_read.sss_en = 1'b1;
        @(negedge clk);
        ab_read.sss_en = 1'b0;
    endtask

    task automatic virtual_ticks(input int count);
        repeat (count) begin
            @(negedge clk);
            vtw_tick_extra = 1'b1;
            @(negedge clk);
            vtw_tick_extra = 1'b0;
        end
    endtask

    task automatic direct_read(input logic [3:0] io_addr,
                               output logic [7:0] value);
        int timeout;
        @(negedge clk);
        vtw_req_addr = io_addr;
        vtw_req_valid = 1'b1;
        timeout = 0;
        while (!vtw_req_ready && timeout < 100) begin
            @(negedge clk);
            timeout++;
        end
        check(vtw_req_ready, "private read never became ready");
        @(negedge clk);
        vtw_req_valid = 1'b0;
        timeout = 0;
        while (!vtw_resp_valid && timeout < 10) begin
            @(negedge clk);
            timeout++;
        end
        check(vtw_resp_valid, "private read produced no response");
        value = vtw_resp_rdata;
    endtask

    logic [7:0] value;
    int ticks_before;
    initial begin
        ab_read = '0;
        ab_read.res = 1'b1;
        ab_read.rw = 1'b1;
        ab_read.cycle_valid = 1'b1;
        sss = '0;
        as_common = '0;
        axi.awvalid = 1'b0;

        repeat (5) @(posedge clk);
        rstn = 1'b1;
        repeat (4) @(posedge clk);

        // Exercise both non-reset updates of the vTW-local count copy before
        // the rest of this standard-track test leaves it at zero.
        axi_write(8'h0E, 32'd128);
        check(dut.track_bit_count_q == 17'd128 &&
              dut.vtw_track_bit_count_q == 17'd128,
              "vTW bit count copy did not follow a nonzero AXI write");
        axi_write(8'h0E, 32'd0);
        check(dut.track_bit_count_q == 17'd0 &&
              dut.vtw_track_bit_count_q == 17'd0,
              "vTW bit count copy did not follow a zero AXI write");

        // The live handoff bypass may serve only the first slot-ROM read. It
        // must not turn a C0Ex access into a controller event while normal
        // service remains blocked by the registered top-level gate.
        @(negedge clk);
        ab_read.addr = 16'hC600;
        ab_read.rw = 1'b1;
        ab_read.serve_en = 1'b0;
        sss.slot_access = 1'b1;
        rom_serve_en = 1'b1;
        @(posedge clk);
        #1;
        check(ab_write.wr_data_en,
              "live handoff bypass did not serve the C600 slot ROM");
        @(negedge clk);
        ab_read.addr = 16'hC0EC;
        sss.slot_access = 1'b0;
        @(posedge clk);
        #1;
        check(!ab_write.wr_data_en && dut.io_access_count_q == 32'd0,
              "live handoff bypass escaped the slot-ROM response path");
        @(negedge clk);
        rom_serve_en = 1'b0;
        repeat (2) @(negedge clk);

        // Drive 1 has standard media. Track 0 is staged, but its first DDR
        // line has not reached the local cache yet.
        mc_ready = 1'b0;
        axi_write(8'h10, 32'h0000_0001); // D1_INFO: media present
        axi_write(8'h07, 32'd16);        // TRACK_LENGTH
        axi_write(8'h06, 32'h0000_0001);// TRACK_INFO: drive 1, qtrack 0

        // vTW sees rotation through one register. The local motor state must
        // change first; the outbound speed-control view follows next clock.
        @(negedge clk);
        ab_read.addr = 16'hC0E9;
        ab_read.rw = 1'b0;
        ab_read.serve_en = 1'b1;
        ab_read.data_en = 1'b0;
        @(negedge clk);
        ab_read.serve_en = 1'b0;
        ab_read.data_en = 1'b1;
        @(posedge clk);
        #1;
        check(dut.motor_on_q && !dut.vtw_drive_spinning_q,
              "vTW rotation view changed on the motor-on edge");
        @(posedge clk);
        #1;
        check(dut.vtw_drive_spinning_q,
              "vTW rotation view did not follow motor-on in one clock");
        @(negedge clk);
        ab_read.data_en = 1'b0;
        ab_read.rw = 1'b1;
        repeat (2) @(negedge clk);

        // Both drive-select copies must move together, and readiness must use
        // the selected drive's own media/cache state.
        physical_write(4'hB, 8'h00);
        check(dut.drive_select_q && dut.vtw_drive_select_q,
              "DRIVE2 did not update both selector copies");
        check(vtw_time_ready,
              "empty DRIVE2 incorrectly held virtual time");
        physical_write(4'hA, 8'h00);
        check(!dut.drive_select_q && !dut.vtw_drive_select_q,
              "DRIVE1 did not update both selector copies");

        @(negedge clk);
        check(!vtw_time_ready,
              "vTW time ran before the active track line was cached");
        mc_ready = 1'b1;

        // Keep the read asserted across the miss. It must not complete with
        // stale data; cache fill makes it ready and returns the first byte.
        ticks_before = disk_tick_count;
        direct_read(4'hC, value);
        check(value == 8'hA1,
              $sformatf("first private nibble was %02X, expected A1", value));
        check(disk_tick_count == ticks_before + 1,
              "first private read did not add exactly one execution tick");

        // A full read resets the partial-nibble gap. After enough virtual
        // CPU cycles, the next read returns the next staged byte.
        ticks_before = disk_tick_count;
        virtual_ticks(8);
        check(disk_tick_count == ticks_before + 8,
              "normal vTW pulse train did not add exactly eight ticks");
        ticks_before = disk_tick_count;
        direct_read(4'hC, value);
        check(value == 8'hA2,
              $sformatf("second private nibble was %02X, expected A2", value));
        check(disk_tick_count == ticks_before + 1,
              "second private read did not add exactly one execution tick");

        // A native Disk II cycle uses only the physical sss_en pulse. A
        // virtual pulse presented at the same time source selection is native
        // must not advance the card.
        vtw_native_cycle_active = 1'b1;
        ticks_before = disk_tick_count;
        virtual_ticks(1);
        check(disk_tick_count == ticks_before,
              "native mode consumed a normal virtual tick");
        physical_tick();
        check(disk_tick_count == ticks_before + 1,
              "native mode did not consume exactly one physical tick");
        vtw_native_cycle_active = 1'b0;

        // Enter and leave Q7 write mode through physical 1 MHz writes. The
        // card must request a whole-core native-speed hold while Q7 is set.
        physical_write(4'hF, 8'h5A);
        check(vtw_write_timing_active,
              "Q7 write mode did not request the 1 MHz interlock");
        ticks_before = disk_tick_count;
        virtual_ticks(1);
        check(disk_tick_count == ticks_before,
              "Q7 write mode consumed a normal virtual tick");
        physical_tick();
        check(disk_tick_count == ticks_before + 1,
              "Q7 write mode did not consume exactly one physical tick");
        physical_write(4'hE, 8'h00);
        check(!vtw_write_timing_active,
              "leaving Q7 write mode did not release the 1 MHz interlock");

        // Reset after request acceptance but before private execution. No
        // pending private tick or response may survive the reset edge.
        @(negedge clk);
        vtw_req_addr = 4'hC;
        vtw_req_valid = 1'b1;
        while (!vtw_req_ready)
            @(negedge clk);
        @(posedge clk);
        @(negedge clk);
        vtw_req_valid = 1'b0;
        rstn = 1'b0;
        @(posedge clk);
        #1;
        check(!dut.vtw_req_pending_q && !dut.vtw_io_read && !vtw_resp_valid,
              "reset left a private Disk II tick or response pending");

        $display("DISK2 VTW READ PASS");
        $finish;
    end
endmodule
