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
        .clk(clk), .rstn(rstn),
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
    logic        disk2_timing_active = 0;
    logic        sh_en = 0;
    logic [17:0] sh_addr = '0;
    logic        sh_we = 0;
    logic [7:0]  sh_wdata = '0;
    logic [7:0]  sh_rdata;
    logic [31:0] cnt_core_cycles;
    logic        video_phase_1mhz;

    vtw_core_top dut (
        .clk(clk), .rstn(rstn), .enable(enable),
        .host_is_iiplus(1'b0), .core_run(core_run),
        .assert_apple_res(1'b0),
        .speed_mode(2'd0),        // WARP baseline
        .pace_divider(16'd0),
        .irq_assert_in(1'b0),
        .data_drive_in(vtw_ab_write.wr_data_en),
        .data_drive_value_in(vtw_ab_write.wr_data),
        .dbg_clear(1'b0),
        .iiplus_buttons_zero(1'b0),
        .slow_region_en(sd_region_en),
        .slow_duration(sd_duration),
        .disk2_timing_active(disk2_timing_active),
        .ramworks_en(1'b0),
        .video_vbl(1'b0),
        .post_main_wide(1'b0),
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
        .sp_resp_rdata(8'd0),
        .sh_en(sh_en), .sh_addr(sh_addr), .sh_we(sh_we),
        .sh_wdata(sh_wdata), .sh_rdata(sh_rdata),
        .arm_req_valid(1'b0), .arm_req_addr('0), .arm_req_rw(1'b1),
        .arm_req_wdata('0), .arm_req_busy(), .arm_resp_valid(),
        .arm_resp_rdata(),
        .c074_state(), .bus_owned(),
        .video_phase_1mhz(video_phase_1mhz),
        .dbg_core_pc(), .cnt_core_cycles(cnt_core_cycles),
        .cnt_bus_cycles(), .cnt_posted_writes(),
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

    task automatic load_program_abs(input logic [15:0] addr);
        int off;
        // $F000: STA addr (region access)
        sh_write(ROM_BASE + 18'h3000, 8'h8D);
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

    task automatic load_program(input logic [7:0] region_lo);
        load_program_abs({8'hC0, region_lo});
    endtask

    // Full reset + reload + re-lock, for phases that need a different target.
    task automatic reboot_with(input logic [15:0] tgt,
                               input logic [9:0]  region_en,
                               input logic [15:0] duration);
        core_run = 0; enable = 0;
        rstn = 0; res_drive_low = 1;
        sd_region_en = region_en; sd_duration = duration;
        repeat (20) @(posedge clk);
        rstn = 1;
        load_program_abs(tgt);
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

    localparam int WINDOW = 200;
    int off_core, on_core, other_core;
    int iosel_on_core, iosel_off_core;
    int video_on_core, video_off_core;
    int disk2_on_core, disk2_off_core;

    initial begin
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

        // ---- 8. Disk II active-drive interlock: even with every optional
        //         slowdown disabled, a spinning drive holds all instructions
        //         at native speed. Releasing the drive restores Warp. ----
        reboot_with(16'hC030, 10'd0, 16'd0);
        disk2_timing_active = 1'b1;
        repeat (80) @(negedge phi0);
        begin
            int a = int'(cnt_core_cycles);
            repeat (WINDOW) @(negedge phi0);
            disk2_on_core = int'(cnt_core_cycles) - a;
        end
        check(disk2_on_core <= WINDOW + WINDOW / 4,
              $sformatf("active Disk II holds every cycle at ~1 MHz (%0d / %0d)",
                        disk2_on_core, WINDOW));
        check(video_phase_1mhz,
              "Disk II's mandatory 1 MHz interlock advertises lookahead");

        disk2_timing_active = 1'b0;
        repeat (80) @(negedge phi0);
        begin
            int a = int'(cnt_core_cycles);
            repeat (WINDOW) @(negedge phi0);
            disk2_off_core = int'(cnt_core_cycles) - a;
        end
        check(disk2_off_core > 2 * WINDOW,
              $sformatf("stopped Disk II restores Warp (%0d)", disk2_off_core));
        check(!video_phase_1mhz,
              "leaving the 1 MHz interlock disables renderer lookahead");

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
