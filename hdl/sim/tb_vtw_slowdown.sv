`timescale 1ns / 1ps
// Focused bench: the vTW per-region slowdown (physical TransWarp DIP
// block 2). The core runs a warp-speed loop that touches the speaker
// region ($C030) with many shadow-executed NOPs between touches. With the
// feature off the NOPs blaze at fabric speed (many core cycles per Apple
// cycle); with speaker slowdown armed, each $C030 access re-locks the core
// to 1 MHz for the configured window, so the loop paces one core cycle per
// Apple cycle. The bench measures that collapse and also checks the region
// decode (a non-enabled region does NOT slow the core).

module tb_vtw_slowdown;

    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 0;
    always #3.75 clk = ~clk;   // 133.333 MHz

    logic rstn = 0;

    // ---- Apple bus pins with motherboard-style weak pulls ----
    wire [7:0]  apple_data_pin;
    wire [15:0] apple_addr_pin;
    wire        apple_rw_pin;
    logic       phi0 = 0;
    wire        apple_inh_pin;
    wire        apple_res_pin;
    wire        apple_irq_pin;
    wire        apple_rdy_pin;
    wire        apple_dma_pin;
    wire        apple_nmi_pin;

    assign (weak0, weak1) apple_data_pin = 8'hFF;
    assign (weak0, weak1) apple_addr_pin = 16'hFFFF;
    assign (weak0, weak1) apple_rw_pin   = 1'b1;
    assign (weak0, weak1) apple_inh_pin  = 1'b1;
    assign (weak0, weak1) apple_res_pin  = 1'b1;
    assign (weak0, weak1) apple_irq_pin  = 1'b1;
    assign (weak0, weak1) apple_rdy_pin  = 1'b1;
    assign (weak0, weak1) apple_dma_pin  = 1'b1;
    assign (weak0, weak1) apple_nmi_pin  = 1'b1;

    always #490 phi0 = ~phi0;   // ~1.02 MHz

    logic res_drive_low = 1;
    assign apple_res_pin = res_drive_low ? 1'b0 : 1'bz;

    globals::AppleBus_read  ab_read;
    globals::AppleBus_write ab_write;
    globals::AppleBus_write vtw_ab_write;
    logic tini_oe_pin, tini_addr_dir_pin, tini_data_dir_pin;

    apple_bus_wrapper wrapper_i (
        .clk(clk), .rstn(rstn), .physical_bus_isolate(1'b0),
        .res_filtered_out(), .dbg_lost_cycle_count(), .dbg_clear(1'b0),
        .inh_allowed(1'b1), .gs_m2_qualify(1'b0), .m2sel_active_high(1'b0),
        .host_is_iiplus(1'b0),
        .iiplus_dma_refresh_active(1'b0),
        .apple_data_pin(apple_data_pin), .apple_addr_pin(apple_addr_pin),
        .apple_rw_pin(apple_rw_pin), .apple_phi0_pin(phi0),
        .apple_m2sel_pin(1'b0), .apple_m2b0_pin(1'b0),
        .apple_inh_pin(apple_inh_pin), .apple_res_pin(apple_res_pin),
        .apple_irq_pin(apple_irq_pin), .apple_rdy_pin(apple_rdy_pin),
        .apple_dma_pin(apple_dma_pin), .apple_nmi_pin(apple_nmi_pin),
        .tini_oe_pin(tini_oe_pin), .tini_5v_pin(1'b0),
        .tini_addr_dir_pin(tini_addr_dir_pin),
        .tini_data_dir_pin(tini_data_dir_pin),
        .ab_read(ab_read), .ab_write(ab_write)
    );

    apple_bus_write_arbiter #(.NUM_CLIENTS(1)) arbiter_i (
        .inh_allowed(1'b1), .client_writes({vtw_ab_write}), .ab_write(ab_write)
    );

    logic        enable = 0;
    logic        core_run = 0;
    logic [9:0]  sd_region_en = 10'd0;
    logic [15:0] sd_duration  = 16'd0;
    logic        disk2_write_timing_active = 0;
    logic        sh_en = 0;
    logic [17:0] sh_addr = '0;
    logic        sh_we = 0;
    logic [7:0]  sh_wdata = '0;
    logic [7:0]  sh_rdata;
    logic [31:0] cnt_core_cycles;
    logic [31:0] cnt_bus_cycles;
    logic        video_phase_1mhz;
    logic        disk2_active = 1'b0;
    logic        disk2_req_valid;
    logic [3:0]  disk2_req_addr;
    logic        disk2_resp_valid = 1'b0;
    logic [31:0] disk2_req_count = 32'd0;
    logic        disk2_cycle_tick;
    logic        disk2_native_cycle_active;
    logic        disk2_time_ready = 1'b1;

    always_ff @(posedge clk) begin
        if (!rstn) begin
            disk2_resp_valid <= 1'b0;
            disk2_req_count <= 32'd0;
        end else begin
            disk2_resp_valid <= disk2_req_valid;
            if (disk2_req_valid)
                disk2_req_count <= disk2_req_count + 32'd1;
        end
    end

    vtw_core_top dut (
        .clk(clk), .rstn(rstn), .enable(enable),
        .host_is_iiplus(1'b0), .virtual_motherboard(1'b0),
        .core_run(core_run),
        .pause(1'b0),
        .assert_apple_res(1'b0),
        .speed_mode(2'd0),        // WARP baseline
        .pace_divider(16'd0),
        .ignore_c074(1'b0),
        .irq_assert_in(1'b0),
        .data_drive_in(vtw_ab_write.wr_data_en),
        .data_drive_value_in(vtw_ab_write.wr_data),
        .dbg_clear(1'b0),
        .iiplus_buttons_zero(1'b0),
        .slow_region_en(sd_region_en),
        .slow_duration(sd_duration),
        .d2_active(disk2_active),
        .d2_req_valid(disk2_req_valid), .d2_req_addr(disk2_req_addr),
        .d2_req_ready(1'b1),
        .d2_resp_valid(disk2_resp_valid), .d2_resp_rdata(8'hA5),
        .d2_cycle_tick(disk2_cycle_tick),
        .d2_native_cycle_active(disk2_native_cycle_active),
        .d2_time_ready(disk2_time_ready),
        .d2_write_timing_active(disk2_write_timing_active),
        .ramworks_en(1'b0),
        .video_vbl(1'b0),
        .post_main_wide(1'b0),
        .overlay_capture_armed(1'b0),
        .overlay_capture_bank_aux(1'b0),
        .overlay_capture_base(16'd0),
        .overlay_capture_limit(16'd0),
        .video_mode_50hz(1'b0),
        .video_line(9'd0),
        .video_cycle(7'd0),
        .ab_read(ab_read), .ab_write(vtw_ab_write),
        .rw_req_valid(), .rw_req_rw(), .rw_req_addr(), .rw_req_wline(),
        .rw_req_ready(1'b1), .rw_resp_valid(1'b0), .rw_resp_rline(64'd0),
        .sp_active(1'b0),
        .sp_boot_suppress(1'b0),
        .sp_req_valid(), .sp_req_target(), .sp_req_addr(), .sp_req_rw(),
        .sp_req_wdata(), .sp_req_ready(1'b1), .sp_resp_valid(1'b0),
        .sp_resp_rdata(8'd0), .sp_sss_snapshot(),
        .sh_en(sh_en), .sh_addr(sh_addr), .sh_we(sh_we),
        .sh_wdata(sh_wdata), .sh_rdata(sh_rdata),
        .arm_req_valid(1'b0), .arm_req_addr('0), .arm_req_rw(1'b1),
        .arm_req_wdata('0), .arm_req_busy(), .arm_resp_valid(),
        .arm_resp_rdata(),
        .arm_post_we(1'b0), .arm_post_addr('0), .arm_post_wdata('0),
        .arm_post_ready(),
        .arm_rw_flush_req(1'b0), .arm_rw_hold_release(1'b0),
        .arm_rw_flush_done(), .arm_rw_hold_state(),
        .c074_state(), .bus_owned(),
        .video_phase_1mhz(video_phase_1mhz),
        .dbg_core_pc(), .cnt_core_cycles(cnt_core_cycles),
        .cnt_bus_cycles(cnt_bus_cycles), .cnt_posted_writes(),
        .post_fill(), .post_high_water(), .cnt_post_drops(),
        .cnt_invalid_routes(),
        .dbg_vsss(), .dbg_last_sync_addr(), .dbg_last_sync_data(),
        .dbg_last_sync_rw(), .dbg_irq_edges(),
        .dbg_cxxx_ring(), .dbg_c0_ring()
    );

    // Motherboard read model: $Cxxx -> $EE, RAM otherwise.
    logic [7:0] mb_rdata;
    logic [7:0] mb_ram [0:16'hFFFF];
    always_comb begin
        if (apple_addr_pin[15:12] == 4'hC) mb_rdata = 8'hEE;
        else mb_rdata = mb_ram[apple_addr_pin];
    end
    wire mb_drive_data = phi0 && (apple_rw_pin === 1'b1) && !tini_data_dir_pin;
    assign apple_data_pin = mb_drive_data ? mb_rdata : 8'hzz;

    // ---- Shadow load port ----
    task automatic sh_write(input logic [17:0] a, input logic [7:0] d);
        @(posedge clk);
        sh_en <= 1'b1; sh_we <= 1'b1; sh_addr <= a; sh_wdata <= d;
        @(posedge clk);
        sh_en <= 1'b0; sh_we <= 1'b0;
    endtask

    // ---- Program: a warp loop touching a chosen $C0xx region ----
    // reset vector -> $F000. The region address byte at $F001 is patched
    // per phase (speaker $C030 vs an unrelated $C05x switch).
    localparam logic [17:0] ROM_BASE = 18'h20000;
    localparam int NOPS = 12;

    task automatic load_program_op_abs(input logic [7:0] opcode,
                                       input logic [15:0] addr);
        int off;
        // $F000: absolute read/write of the selected region.
        sh_write(ROM_BASE + 18'h3000, opcode);
        sh_write(ROM_BASE + 18'h3001, addr[7:0]);
        sh_write(ROM_BASE + 18'h3002, addr[15:8]);
        off = 3;
        // NOPs
        for (int i = 0; i < NOPS; i++) begin
            sh_write(ROM_BASE + 18'h3000 + 18'(off), 8'hEA);
            off++;
        end
        // JMP $F000
        sh_write(ROM_BASE + 18'h3000 + 18'(off),     8'h4C); off++;
        sh_write(ROM_BASE + 18'h3000 + 18'(off),     8'h00); off++;
        sh_write(ROM_BASE + 18'h3000 + 18'(off),     8'hF0); off++;
        // reset vector -> $F000
        sh_write(ROM_BASE + 18'h3FFC, 8'h00);
        sh_write(ROM_BASE + 18'h3FFD, 8'hF0);
        // define zero page / stack
        for (int i = 0; i < 512; i++) sh_write(18'(i), 8'h00);
    endtask

    task automatic load_program_abs(input logic [15:0] addr);
        load_program_op_abs(8'h8D, addr);
    endtask

    task automatic load_program(input logic [7:0] region_lo);
        load_program_abs({8'hC0, region_lo});
    endtask

    // Full reset + reload + re-lock, for phases that need a different target.
    task automatic reboot_with(input logic [15:0] tgt,
                               input logic [9:0]  region_en,
                               input logic [15:0] duration);
        core_run = 0; enable = 0;
        rstn = 0; res_drive_low = 1;
        disk2_time_ready = 1'b1;
        sd_region_en = region_en; sd_duration = duration;
        repeat (20) @(posedge clk);
        rstn = 1;
        load_program_abs(tgt);
        enable = 1; #10us;
        res_drive_low = 0; #4us;
        core_run = 1;
    endtask

    task automatic reboot_op_with(input logic [7:0] opcode,
                                  input logic [15:0] tgt);
        core_run = 0; enable = 0;
        rstn = 0; res_drive_low = 1;
        disk2_time_ready = 1'b1;
        sd_region_en = 10'd0; sd_duration = 16'd0;
        repeat (20) @(posedge clk);
        rstn = 1;
        load_program_op_abs(opcode, tgt);
        enable = 1; #10us;
        res_drive_low = 0; #4us;
        core_run = 1;
    endtask

    int fails = 0;
    task automatic check(input bit cond, input string msg);
        if (!cond) begin
            fails++;
            $display("VTW SLOWDOWN FAIL: %s", msg);
        end
    endtask

    localparam logic [15:0] PIPELINE_DURATION = 16'h0037;
    int pipeline_accept_count = 0;
    int pipeline_apply_count = 0;
    logic pipeline_watch = 1'b0;

    /* Count the two sides of the slowdown update stage at their accepting
     * edges. The directed checks below keep the watch window short enough
     * that one CPU completion must yield exactly one counter update. */
    always @(posedge clk) begin
        if (pipeline_watch) begin
            if (dut.core_en && dut.sd_hit)
                pipeline_accept_count++;
            if (dut.slow_update_valid_q && dut.slow_update_hit_q &&
                ab_read.res)
                pipeline_apply_count++;
        end
    end

    task automatic check_slow_update_pipeline;
        int wait_cycles;

        // Start with an empty counter and accept one matching $C030 cycle.
        reboot_with(16'hC030, 10'b00_1000_0000, PIPELINE_DURATION);
        pipeline_accept_count = 0;
        pipeline_apply_count = 0;
        pipeline_watch = 1'b1;
        wait_cycles = 0;
        while (pipeline_accept_count == 0 && wait_cycles < 20000) begin
            @(posedge clk);
            #1ps;
            wait_cycles++;
        end
        check(wait_cycles < 20000,
              "timed out waiting for a matching slowdown cycle");
        check(pipeline_accept_count == 1,
              "the directed window accepted more than one slowdown hit");
        check(dut.slow_update_valid_q && dut.slow_update_hit_q,
              "accepted slowdown hit was not captured in the update stage");
        check(dut.slow_update_duration_q == PIPELINE_DURATION,
              "slowdown stage did not capture the accepted duration");
        check(dut.slow_cnt_q == 16'd0,
              "accepted slowdown hit changed the counter immediately");

        // Changing the live input now must not change the captured update.
        sd_duration = 16'h005A;
        @(posedge clk);
        #1ps;
        check(dut.slow_cnt_q == PIPELINE_DURATION,
              "next fabric edge did not load the captured duration");
        check(pipeline_accept_count == 1 && pipeline_apply_count == 1,
              "one accepted slowdown hit did not produce one update");
        @(posedge clk);
        #1ps;
        check(dut.slow_cnt_q == PIPELINE_DURATION,
              "one accepted slowdown hit updated the counter more than once");
        check(pipeline_accept_count == 1 && pipeline_apply_count == 1,
              "slowdown update stage duplicated an accepted hit");
        pipeline_watch = 1'b0;

        // Present an Apple reset between capture and apply. Force the DUT's
        // filtered reset input low for this one-edge test so the 2.1 us pin
        // filter does not hide the single-clock cancellation case.
        reboot_with(16'hC030, 10'b00_1000_0000, PIPELINE_DURATION);
        wait_cycles = 0;
        while (!(dut.slow_update_valid_q && dut.slow_update_hit_q) &&
               wait_cycles < 20000) begin
            @(posedge clk);
            #1ps;
            wait_cycles++;
        end
        check(wait_cycles < 20000,
              "timed out waiting for the reset-cancel slowdown hit");
        check(dut.slow_cnt_q == 16'd0,
              "reset-cancel test did not start with an empty counter");
        res_drive_low = 1'b1;
        force dut.ab_read.res = 1'b0;
        @(posedge clk);
        #1ps;
        check(dut.slow_cnt_q == 16'd0,
              "Apple reset did not cancel the pending slowdown update");
        check(!dut.slow_update_valid_q,
              "Apple reset did not clear the pending slowdown valid bit");
        release dut.ab_read.res;
        res_drive_low = 1'b0;
    endtask

    /* The registered normal tick must equal the prior edge's accepted
     * virtual cycle. This point-by-point check proves that the stage neither
     * merges nor loses a tick across the whole bench. */
    wire d2_normal_tick_accept =
        dut.core_en && disk2_active &&
        !dut.private_d2_q && !dut.sd_disk2_native;
    logic d2_tick_expected_q = 1'b0;
    int d2_accept_count = 0;
    int d2_tick_count = 0;
    int d2_private_done_count = 0;
    int d2_native_done_count = 0;
    int d2_native_active_count = 0;

    always @(posedge clk) begin
        if (!rstn) begin
            d2_tick_expected_q      <= 1'b0;
            d2_accept_count         <= 0;
            d2_tick_count           <= 0;
            d2_private_done_count   <= 0;
            d2_native_done_count    <= 0;
            d2_native_active_count  <= 0;
        end else begin
            if (disk2_cycle_tick !== d2_tick_expected_q) begin
                fails++;
                $display("VTW SLOWDOWN FAIL: staged Disk II tick mismatch (got=%0b expected=%0b)",
                         disk2_cycle_tick, d2_tick_expected_q);
            end
            d2_tick_expected_q <= d2_normal_tick_accept;
            if (d2_normal_tick_accept)
                d2_accept_count <= d2_accept_count + 1;
            if (disk2_cycle_tick)
                d2_tick_count <= d2_tick_count + 1;
            if (dut.core_en && disk2_active && dut.private_d2_q) begin
                d2_private_done_count <= d2_private_done_count + 1;
                if (disk2_cycle_tick) begin
                    fails++;
                    $display("VTW SLOWDOWN FAIL: private Disk II completion also emitted a normal tick");
                end
            end
            if (dut.core_en && disk2_active && dut.sd_disk2_native) begin
                d2_native_done_count <= d2_native_done_count + 1;
                if (disk2_cycle_tick) begin
                    fails++;
                    $display("VTW SLOWDOWN FAIL: native Disk II completion also emitted a normal tick");
                end
            end
            if (disk2_native_cycle_active)
                d2_native_active_count <= d2_native_active_count + 1;
        end
    end

    localparam int WINDOW = 200;
    int off_core, on_core, other_core;
    int iosel_on_core, iosel_off_core;
    int video_on_core, video_off_core;
    int disk2_on_core, disk2_off_core;
    int disk2_direct_before, disk2_bus_before;
    int d2_accept_before, d2_tick_before;
    int d2_private_done_before, d2_native_done_before;
    int d2_native_active_before;
    int d2_wait_cycles;

    initial begin
        // ---- 0. Slowdown bookkeeping pipeline ----
        check_slow_update_pipeline();

        // ---- 1. Baseline WARP: slowdown disabled ----
        // Preload the speaker program, then boot with the feature OFF.
        rstn = 0; res_drive_low = 1; enable = 0; core_run = 0;
        sd_region_en = 10'd0; sd_duration = 16'd0;
        repeat (20) @(posedge clk);
        rstn = 1;
        load_program(8'h30);      // STA $C030 loop
        enable = 1; #10us;
        res_drive_low = 0; #4us;
        core_run = 1;
        repeat (80) @(negedge phi0);
        begin
            int a = int'(cnt_core_cycles);
            repeat (WINDOW) @(negedge phi0);
            off_core = int'(cnt_core_cycles) - a;
        end
        // At warp the NOPs blaze: well over one core cycle per Apple cycle.
        check(off_core > 2 * WINDOW,
              $sformatf("warp baseline runs fast (%0d core / %0d apple)",
                        off_core, WINDOW));
        check(!video_phase_1mhz,
              "warp baseline leaves renderer phase lookahead off");

        // ---- 2. Floating-I/O slowdown ARMED: speaker access collapses
        //         the loop to ~1 MHz. ----
        sd_region_en = 10'b00_1000_0000;   // bit7 = $C019/$C030-$C05F
        sd_duration  = 16'd64;           // >= loop length in Apple cycles
        repeat (80) @(negedge phi0);     // let it re-lock
        begin
            int a = int'(cnt_core_cycles);
            repeat (WINDOW) @(negedge phi0);
            on_core = int'(cnt_core_cycles) - a;
        end
        // One core cycle per Apple cycle (+ small jitter).
        check(on_core <= WINDOW + WINDOW / 4,
              $sformatf("speaker slowdown paces ~1 MHz (%0d core / %0d apple)",
                        on_core, WINDOW));
        check(off_core >= 3 * on_core,
              $sformatf("slowdown clearly collapses the rate (off=%0d on=%0d)",
                        off_core, on_core));
        check(video_phase_1mhz,
              "1 MHz slowdown advertises renderer phase lookahead");

        // ---- 3. Region decode: enabling PADDLE/SLOTS must NOT slow a
        //         speaker-only loop (wrong-region enable is inert). ----
        sd_region_en = 10'b01_0000_0001;   // paddle + slot1, NOT speaker
        sd_duration  = 16'd64;
        repeat (80) @(negedge phi0);
        begin
            int a = int'(cnt_core_cycles);
            repeat (WINDOW) @(negedge phi0);
            other_core = int'(cnt_core_cycles) - a;
        end
        check(other_core > 2 * WINDOW,
              $sformatf("non-matching region enable stays warp (%0d)", other_core));
        check(!video_phase_1mhz,
              "non-matching slowdown leaves renderer phase lookahead off");

        // ---- 4. I/O-SELECT $C400 (Mockingboard/Phasor 6522 space), slot-4
        //         armed: the timer-space reads must self-slow to ~1 MHz.
        //         This is the address range A2Desktop's detection reads. ----
        reboot_with(16'hC400, 10'b00_0000_1000, 16'd64);   // slot 4 = bit3
        repeat (80) @(negedge phi0);
        begin
            int a = int'(cnt_core_cycles);
            repeat (WINDOW) @(negedge phi0);
            iosel_on_core = int'(cnt_core_cycles) - a;
        end
        check(iosel_on_core <= WINDOW + WINDOW / 4,
              $sformatf("slot-4 $C400 I/O-select paces ~1 MHz (%0d / %0d)",
                        iosel_on_core, WINDOW));

        // ---- 5. Same $C400 loop, slot-4 NOT armed: stays warp (proves the
        //         I/O-select slowdown is per-slot gated, not blanket). ----
        reboot_with(16'hC400, 10'd0, 16'd64);
        repeat (80) @(negedge phi0);
        begin
            int a = int'(cnt_core_cycles);
            repeat (WINDOW) @(negedge phi0);
            iosel_off_core = int'(cnt_core_cycles) - a;
        end
        check(iosel_off_core > 2 * WINDOW,
              $sformatf("$C400 with slot-4 off stays warp (%0d)", iosel_off_core));

        // ---- 6. Floating-I/O region ($C019/$C030-$C05F) armed: a $C05A loop
        //         self-slows to ~1 MHz (FLOATBUS vapor lock / raster sync). ----
        reboot_with(16'hC05A, 10'b00_1000_0000, 16'd64);   // bit7 = floating I/O
        repeat (80) @(negedge phi0);
        begin
            int a = int'(cnt_core_cycles);
            repeat (WINDOW) @(negedge phi0);
            video_on_core = int'(cnt_core_cycles) - a;
        end
        check(video_on_core <= WINDOW + WINDOW / 4,
              $sformatf("video region paces ~1 MHz (%0d / %0d)",
                        video_on_core, WINDOW));

        // ---- 7. Same $C05A loop, video NOT armed: stays warp. ----
        reboot_with(16'hC05A, 10'd0, 16'd64);
        repeat (80) @(negedge phi0);
        begin
            int a = int'(cnt_core_cycles);
            repeat (WINDOW) @(negedge phi0);
            video_off_core = int'(cnt_core_cycles) - a;
        end
        check(video_off_core > 2 * WINDOW,
              $sformatf("$C05A with video off stays warp (%0d)", video_off_core));

        // ---- 8. Disk II Q7 write-mode interlock: even with every optional
        //         slowdown disabled, writing holds all instructions at native
        //         speed. Leaving write mode restores Warp. ----
        reboot_with(16'hC030, 10'd0, 16'd0);
        disk2_write_timing_active = 1'b1;
        repeat (80) @(negedge phi0);
        begin
            int a = int'(cnt_core_cycles);
            repeat (WINDOW) @(negedge phi0);
            disk2_on_core = int'(cnt_core_cycles) - a;
        end
        check(disk2_on_core <= WINDOW + WINDOW / 4,
              $sformatf("Disk II write mode holds every cycle at ~1 MHz (%0d / %0d)",
                        disk2_on_core, WINDOW));
        check(video_phase_1mhz,
              "Disk II write mode advertises 1 MHz lookahead");

        disk2_write_timing_active = 1'b0;
        repeat (80) @(negedge phi0);
        begin
            int a = int'(cnt_core_cycles);
            repeat (WINDOW) @(negedge phi0);
            disk2_off_core = int'(cnt_core_cycles) - a;
        end
        check(disk2_off_core > 2 * WINDOW,
              $sformatf("leaving Disk II write mode restores Warp (%0d)", disk2_off_core));
        check(!video_phase_1mhz,
              "leaving the 1 MHz interlock disables renderer lookahead");

        // ---- 9. Normal Disk II virtual-time tick stage. First drain the
        //         stage into a readiness stall, then run a burst and prove
        //         exact accepted/output counts. A tick accepted just before
        //         a later stall must still drain once. ----
        disk2_active = 1'b1;
        reboot_with(16'hC030, 10'd0, 16'd0);
        while (!dut.core_en)
            @(negedge clk);
        disk2_time_ready = 1'b0;
        repeat (4) @(posedge clk);
        #1ps;
        check(d2_accept_count == d2_tick_count,
              "normal Disk II tick stage did not drain into readiness stall");
        d2_accept_before = d2_accept_count;
        d2_tick_before = d2_tick_count;
        repeat (16) @(posedge clk);
        #1ps;
        check(d2_accept_count == d2_accept_before &&
              d2_tick_count == d2_tick_before && !dut.core_en,
              "Disk II readiness stall admitted a virtual cycle or tick");

        @(negedge clk);
        disk2_time_ready = 1'b1;
        d2_wait_cycles = 0;
        while ((d2_accept_count - d2_accept_before) < 32 &&
               d2_wait_cycles < 20000) begin
            @(posedge clk);
            d2_wait_cycles++;
        end
        @(negedge clk);
        disk2_time_ready = 1'b0;
        repeat (4) @(posedge clk);
        #1ps;
        check((d2_accept_count - d2_accept_before) >= 32,
              $sformatf("normal Disk II tick burst accepted only %0d cycles (xstate=%0d core_res_n=%0b core_en=%0b pc=%04X)",
                        d2_accept_count - d2_accept_before,
                        dut.xstate_q, dut.core_res_n, dut.core_en,
                        dut.core_addr));
        check((d2_accept_count - d2_accept_before) ==
              (d2_tick_count - d2_tick_before),
              "normal Disk II tick burst did not keep exact pulse count");

        @(negedge clk);
        disk2_time_ready = 1'b1;
        while (!d2_normal_tick_accept)
            @(negedge clk);
        @(posedge clk);
        @(negedge clk);
        disk2_time_ready = 1'b0;
        d2_accept_before = d2_accept_count;
        d2_tick_before = d2_tick_count;
        repeat (2) @(posedge clk);
        #1ps;
        check(d2_accept_count == d2_accept_before &&
              d2_tick_count == d2_tick_before + 1,
              "readiness stall cancelled or duplicated an accepted tick");
        repeat (16) @(posedge clk);
        #1ps;
        check(d2_accept_count == d2_accept_before &&
              d2_tick_count == d2_tick_before + 1,
              "readiness stall emitted an extra delayed tick");
        disk2_time_ready = 1'b1;

        // Ending the core session between acceptance and consumption clears
        // the staged output. The card also leaves virtual-time mode then.
        while (!d2_normal_tick_accept)
            @(negedge clk);
        @(posedge clk);
        @(negedge clk);
        check(disk2_cycle_tick,
              "normal Disk II tick was not pending before reset");
        core_run = 1'b0;
        @(posedge clk);
        #1ps;
        check(!disk2_cycle_tick,
              "core-session reset did not clear the pending Disk II tick");

        // ---- 10. Disk II route split. Even reads use the private card port;
        //         writes and odd reads still emit real Apple bus cycles. ----
        reboot_op_with(8'hAD, 16'hC0EC); // LDA $C0EC
        repeat (80) @(negedge phi0);
        disk2_direct_before = int'(disk2_req_count);
        disk2_bus_before = int'(cnt_bus_cycles);
        d2_private_done_before = d2_private_done_count;
        repeat (80) @(negedge phi0);
        check(int'(disk2_req_count) > disk2_direct_before,
              "even Disk II reads did not use the private port");
        check(int'(cnt_bus_cycles) == disk2_bus_before,
              "even Disk II reads leaked onto the physical Apple bus");
        check(d2_private_done_count > d2_private_done_before,
              "even Disk II reads did not complete as private cycles");

        reboot_op_with(8'h8D, 16'hC0EC); // STA $C0EC
        repeat (80) @(negedge phi0);
        disk2_direct_before = int'(disk2_req_count);
        disk2_bus_before = int'(cnt_bus_cycles);
        d2_native_done_before = d2_native_done_count;
        d2_native_active_before = d2_native_active_count;
        repeat (80) @(negedge phi0);
        check(int'(disk2_req_count) == disk2_direct_before,
              "Disk II write used the private read port");
        check(int'(cnt_bus_cycles) > disk2_bus_before,
              "Disk II write did not use the physical 1 MHz bus path");
        check(d2_native_done_count > d2_native_done_before &&
              d2_native_active_count > d2_native_active_before,
              "Disk II write did not keep its native tick path");

        reboot_op_with(8'hAD, 16'hC0ED); // odd read needs floating bus
        repeat (80) @(negedge phi0);
        disk2_direct_before = int'(disk2_req_count);
        disk2_bus_before = int'(cnt_bus_cycles);
        d2_native_done_before = d2_native_done_count;
        d2_native_active_before = d2_native_active_count;
        repeat (80) @(negedge phi0);
        check(int'(disk2_req_count) == disk2_direct_before,
              "odd Disk II read used the private port");
        check(int'(cnt_bus_cycles) > disk2_bus_before,
              "odd Disk II read did not use the physical Apple bus");
        check(d2_native_done_count > d2_native_done_before &&
              d2_native_active_count > d2_native_active_before,
              "odd Disk II read did not keep its native tick path");

        if (fails == 0) $display("VTW SLOWDOWN PASS");
        else            $display("VTW SLOWDOWN FAILED: %0d checks", fails);
        $finish;
    end

    initial begin
        #5ms;
        $display("VTW SLOWDOWN FAIL: timeout");
        $finish;
    end

endmodule
