`timescale 1ns / 1ps

// Focused regression for the one-entry vTW PC-trace event pipeline.
//
// The bench drives the trace event sources at their boundary inside
// vtw_core_top. This avoids running a program merely to create exact adjacent
// instruction events, while the checks still cover the production trace
// registers, clear/reset priority, freeze causes, and Apple RESET behavior.
module tb_vtw_pc_event_pipeline;

    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;

    logic rstn = 1'b0;
    logic dbg_clear = 1'b0;

    globals::AppleBus_read  ab_read;
    globals::AppleBus_write ab_write;

    logic [16*16-1:0] dbg_pc_trace;
    logic [31:0]      dbg_trace_status;

    vtw_core_top dut (
        .clk(clk),
        .rstn(rstn),
        .enable(1'b0),
        .host_is_iiplus(1'b0),
        .virtual_motherboard(1'b0),
        .core_run(1'b0),
        .pause(1'b0),
        .assert_apple_res(1'b0),
        .speed_mode(2'd0),
        .pace_divider(16'd0),
        .ignore_c074(1'b0),
        .slow_region_en(10'd0),
        .slow_duration(16'd0),
        .d2_active(1'b0),
        .d2_req_valid(),
        .d2_req_addr(),
        .d2_req_ready(1'b0),
        .d2_resp_valid(1'b0),
        .d2_resp_rdata(8'd0),
        .d2_cycle_tick(),
        .d2_native_cycle_active(),
        .d2_time_ready(1'b1),
        .d2_write_timing_active(1'b0),
        .ramworks_en(1'b0),
        .video_vbl(1'b0),
        .video_mode_50hz(1'b0),
        .video_line(9'd0),
        .video_cycle(7'd0),
        .post_main_wide(1'b0),
        .overlay_capture_armed(1'b0),
        .overlay_capture_bank_aux(1'b0),
        .overlay_capture_base(16'd0),
        .overlay_capture_limit(16'd0),
        .ab_read(ab_read),
        .ab_write(ab_write),
        .irq_assert_in(1'b0),
        .data_drive_in(1'b0),
        .data_drive_value_in(8'd0),
        .dbg_clear(dbg_clear),
        .iiplus_buttons_zero(1'b0),
        .rw_req_valid(),
        .rw_req_rw(),
        .rw_req_addr(),
        .rw_req_wline(),
        .rw_req_ready(1'b1),
        .rw_resp_valid(1'b0),
        .rw_resp_rline(64'd0),
        .sp_active(1'b0),
        .sp_boot_suppress(1'b0),
        .sp_req_valid(),
        .sp_req_target(),
        .sp_req_addr(),
        .sp_req_rw(),
        .sp_req_wdata(),
        .sp_req_ready(1'b1),
        .sp_resp_valid(1'b0),
        .sp_resp_rdata(8'd0),
        .sp_sss_snapshot(),
        .sh_en(1'b0),
        .sh_addr(18'd0),
        .sh_we(1'b0),
        .sh_wdata(8'd0),
        .sh_rdata(),
        .arm_req_valid(1'b0),
        .arm_req_addr(16'd0),
        .arm_req_rw(1'b1),
        .arm_req_wdata(8'd0),
        .arm_req_busy(),
        .arm_resp_valid(),
        .arm_resp_rdata(),
        .arm_post_we(1'b0),
        .arm_post_addr(16'd0),
        .arm_post_wdata(8'd0),
        .arm_post_ready(),
        .arm_rw_flush_req(1'b0),
        .arm_rw_hold_release(1'b0),
        .arm_rw_flush_done(),
        .arm_rw_hold_state(),
        .c074_state(),
        .bus_owned(),
        .video_phase_1mhz(),
        .dbg_core_pc(),
        .cnt_core_cycles(),
        .cnt_bus_cycles(),
        .cnt_posted_writes(),
        .post_fill(),
        .post_high_water(),
        .cnt_post_drops(),
        .cnt_invalid_routes(),
        .dbg_vsss(),
        .dbg_last_sync_addr(),
        .dbg_last_sync_data(),
        .dbg_last_sync_rw(),
        .dbg_irq_edges(),
        .dbg_cxxx_ring(),
        .dbg_c0_ring(),
        .dbg_sync_write_check(),
        .dbg_sync_write_addr(),
        .dbg_c000_context(),
        .dbg_c000_counts(),
        .dbg_pc_trace(dbg_pc_trace),
        .dbg_io_trace(),
        .dbg_trace_status(dbg_trace_status),
        .dbg_bus_faults()
    );

    // Boundary controls. A continuous force lets each task change the event
    // tuple before an edge without depending on the core's execution state.
    logic        event_valid = 1'b0;
    logic [15:0] event_pc = 16'd0;
    logic        event_bad_c000 = 1'b0;
    logic        event_intcxrom = 1'b0;

    int errors = 0;

    function automatic logic [15:0] trace_pc(input int index);
        trace_pc = dbg_pc_trace[index*16 +: 16];
    endfunction

    task automatic check(input bit condition, input string message);
        if (!condition) begin
            errors++;
            $display("FAIL: %s", message);
        end
    endtask

    task automatic set_event(
        input logic        valid,
        input logic [15:0] pc,
        input logic        bad_c000,
        input logic        intcxrom
    );
        @(negedge clk);
        event_valid = valid;
        event_pc = pc;
        event_bad_c000 = bad_c000;
        event_intcxrom = intcxrom;
    endtask

    task automatic event_edge(
        input logic        valid,
        input logic [15:0] pc,
        input logic        bad_c000,
        input logic        intcxrom
    );
        set_event(valid, pc, bad_c000, intcxrom);
        @(posedge clk);
        #1;
    endtask

    task automatic idle_edge;
        event_edge(1'b0, 16'd0, 1'b0, 1'b0);
    endtask

    task automatic clear_trace;
        set_event(1'b0, 16'd0, 1'b0, 1'b0);
        dbg_clear = 1'b1;
        @(posedge clk);
        #1;
        dbg_clear = 1'b0;
        check(dbg_pc_trace == '0 && dbg_trace_status == 32'd0,
              "dbg_clear did not clear trace and freeze state");
    endtask

    initial begin
        ab_read = '0;
        ab_read.res = 1'b1;
        ab_read.irq = 1'b1;
        ab_read.nmi = 1'b1;
        ab_read.rdy = 1'b1;
        ab_read.dma = 1'b1;

        force dut.core_en = event_valid;
        force dut.core_sync = event_valid;
        force dut.core_addr = event_pc;
        force dut.eng_bad_c000_pulse = event_bad_c000;
        force dut.vsss.sw_intcxrom = event_intcxrom;

        repeat (4) @(posedge clk);
        rstn = 1'b1;
        idle_edge();

        // Three adjacent instruction events must sustain one event per clock.
        // Each tuple appears exactly one edge later and remains newest-first.
        clear_trace();
        event_edge(1'b1, 16'h1000, 1'b0, 1'b0);
        check(trace_pc(0) == 16'd0,
              "first PC event bypassed the pending stage");
        event_edge(1'b1, 16'h1001, 1'b0, 1'b0);
        check(trace_pc(0) == 16'h1000 && trace_pc(1) == 16'd0,
              "second PC event did not drain the first event in order");
        event_edge(1'b1, 16'h1002, 1'b0, 1'b0);
        check(trace_pc(0) == 16'h1001 && trace_pc(1) == 16'h1000,
              "third PC event did not preserve adjacent-event order");
        idle_edge();
        check(trace_pc(0) == 16'h1002 &&
              trace_pc(1) == 16'h1001 &&
              trace_pc(2) == 16'h1000,
              "final adjacent PC event did not drain newest-first");

        // A trigger can arrive while the prior fetch still waits in the
        // one-entry pipe. The older PC must drain first, then C600 must become
        // the newest frozen entry on the following edge.
        clear_trace();
        event_edge(1'b1, 16'h6789, 1'b0, 1'b0);
        event_edge(1'b1, 16'hC600, 1'b0, 1'b1);
        check(dbg_trace_status[0] == 1'b0 &&
              trace_pc(0) == 16'h6789 && trace_pc(1) == 16'd0,
              "C600 overlap did not drain the older pending PC first");
        idle_edge();
        check(dbg_trace_status[0] == 1'b1 &&
              dbg_trace_status[2:1] == 2'd3 &&
              trace_pc(0) == 16'hC600 && trace_pc(1) == 16'h6789,
              "C600 overlap did not freeze with both PCs in order");

        // A $C600 fetch under INTCXROM must enter the history before the
        // self-test freeze takes effect one edge later.
        clear_trace();
        event_edge(1'b1, 16'h2345, 1'b0, 1'b0);
        idle_edge();
        event_edge(1'b1, 16'hC600, 1'b0, 1'b1);
        check(dbg_trace_status[0] == 1'b0 && trace_pc(0) == 16'h2345,
              "C600 self-test froze or shifted before its pending edge");
        idle_edge();
        check(dbg_trace_status[0] == 1'b1 &&
              dbg_trace_status[2:1] == 2'd3 &&
              trace_pc(0) == 16'hC600 && trace_pc(1) == 16'h2345,
              "C600 trigger PC was not recorded before reason-3 freeze");
        event_edge(1'b1, 16'h3456, 1'b0, 1'b0);
        idle_edge();
        check(trace_pc(0) == 16'hC600 && trace_pc(1) == 16'h2345,
              "PC history changed after self-test freeze");

        // A bad-$C000 event and self-test event on the same edge must retain
        // the trigger PC but select bad-$C000 reason 2.
        clear_trace();
        event_edge(1'b1, 16'hC600, 1'b1, 1'b1);
        check(dbg_trace_status[0] == 1'b0 && trace_pc(0) == 16'd0,
              "simultaneous freeze causes bypassed the pending stage");
        idle_edge();
        check(dbg_trace_status[0] == 1'b1 &&
              dbg_trace_status[2:1] == 2'd2 &&
              trace_pc(0) == 16'hC600,
              "bad-C000 did not win freeze priority with trigger PC intact");

        // A bad-C000 pulse need not coincide with a fetch. If an older PC is
        // pending, that real entry drains first; the pulse must not create a
        // zero/fake PC entry when its reason commits on the following edge.
        clear_trace();
        event_edge(1'b1, 16'h4567, 1'b0, 1'b0);
        event_edge(1'b0, 16'd0, 1'b1, 1'b0);
        check(dbg_trace_status[0] == 1'b0 &&
              trace_pc(0) == 16'h4567 && trace_pc(1) == 16'd0,
              "bad-C000 pulse did not drain the older PC without a fake entry");
        idle_edge();
        check(dbg_trace_status[0] == 1'b1 &&
              dbg_trace_status[2:1] == 2'd2 &&
              trace_pc(0) == 16'h4567 && trace_pc(1) == 16'd0,
              "bad-C000 no-fetch freeze changed the drained PC history");

        // Once a bad-C000 reason is pending, a C600 fetch on the next edge is
        // later than the fault. It must be rejected rather than replacing the
        // reason or entering the frozen history.
        clear_trace();
        event_edge(1'b1, 16'h5678, 1'b0, 1'b0);
        idle_edge();
        event_edge(1'b0, 16'd0, 1'b1, 1'b0);
        check(dbg_trace_status[0] == 1'b0 && trace_pc(0) == 16'h5678,
              "bad-C000 reason committed before its pending edge");
        event_edge(1'b1, 16'hC600, 1'b0, 1'b1);
        check(dbg_trace_status[0] == 1'b1 &&
              dbg_trace_status[2:1] == 2'd2 &&
              trace_pc(0) == 16'h5678 && trace_pc(1) == 16'd0,
              "C600 behind bad-C000 was not rejected with reason 2 retained");
        idle_edge();
        check(trace_pc(0) == 16'h5678 && trace_pc(1) == 16'd0,
              "rejected C600 appeared after bad-C000 freeze");

        // Clear and hard reset must cancel the reason as well as the PC when
        // a self-test or bad-C000 freeze is waiting in the event pipe.
        clear_trace();
        event_edge(1'b1, 16'hC600, 1'b0, 1'b1);
        check(dbg_trace_status[0] == 1'b0 && trace_pc(0) == 16'd0,
              "clear-cancel self-test trigger bypassed the pending stage");
        clear_trace();
        idle_edge();
        check(dbg_pc_trace == '0 && dbg_trace_status == 32'd0,
              "dbg_clear allowed a canceled freeze reason or PC to drain");

        event_edge(1'b0, 16'd0, 1'b1, 1'b0);
        check(dbg_trace_status[0] == 1'b0,
              "hard-reset cancel fault froze before its pending edge");
        set_event(1'b0, 16'd0, 1'b0, 1'b0);
        rstn = 1'b0;
        @(posedge clk);
        #1;
        rstn = 1'b1;
        idle_edge();
        check(dbg_pc_trace == '0 && dbg_trace_status == 32'd0,
              "hard reset allowed a canceled freeze reason to commit");

        // dbg_clear must cancel an event that has been captured but has not
        // yet drained into the public history.
        clear_trace();
        event_edge(1'b1, 16'hCA11, 1'b0, 1'b0);
        check(trace_pc(0) == 16'd0,
              "clear-cancel test event bypassed the pending stage");
        clear_trace();
        idle_edge();
        check(dbg_pc_trace == '0 && dbg_trace_status == 32'd0,
              "dbg_clear allowed a canceled pending PC event to drain");

        // Hard reset has the same cancellation rule as dbg_clear.
        event_edge(1'b1, 16'hBEEF, 1'b0, 1'b0);
        check(trace_pc(0) == 16'd0,
              "hard-reset test event bypassed the pending stage");
        set_event(1'b0, 16'd0, 1'b0, 1'b0);
        rstn = 1'b0;
        @(posedge clk);
        #1;
        rstn = 1'b1;
        idle_edge();
        check(dbg_pc_trace == '0 && dbg_trace_status == 32'd0,
              "hard reset allowed a canceled pending PC event to drain");

        // Apple RESET is not the fabric hard reset. A PC event accepted just
        // before RES# falls must drain and remain available for forensics.
        clear_trace();
        event_edge(1'b1, 16'h5A5A, 1'b0, 1'b0);
        check(trace_pc(0) == 16'd0,
              "Apple-RESET test event bypassed the pending stage");
        set_event(1'b0, 16'd0, 1'b0, 1'b0);
        ab_read.res = 1'b0;
        @(posedge clk);
        #1;
        check(trace_pc(0) == 16'h5A5A && dbg_trace_status[0] == 1'b0,
              "pending PC event did not drain across Apple RESET");
        ab_read.res = 1'b1;
        idle_edge();
        check(trace_pc(0) == 16'h5A5A,
              "Apple RESET release discarded retained PC history");

        release dut.core_en;
        release dut.core_sync;
        release dut.core_addr;
        release dut.eng_bad_c000_pulse;
        release dut.vsss.sw_intcxrom;

        if (errors != 0) begin
            $fatal(1, "VTW PC EVENT PIPELINE FAIL: %0d errors", errors);
        end
        $display("VTW PC EVENT PIPELINE PASS");
        $finish;
    end

endmodule
