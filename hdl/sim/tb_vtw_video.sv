`timescale 1ns / 1ps
// Focused bench: the vTW accelerated-//e video paths.
//
//  1. Super Hi-Res posted-write window. SHR's framebuffer, SCB ($9D00) and
//     palette ($9E00) live in AUX $2000-$9FFF. The vTW must echo those
//     writes to the Apple bus (posted writes) so the capture/renderer sees
//     them. The bench runs warp loops that hammer one address and measures
//     the posted-write rate:
//        - AUX $9D00  -> posts  (in the extended aux window)
//        - AUX $B000  -> no post (above the $9FFF ceiling)
//        - MAIN $9D00 -> no post (main window still ends at $5FFF)
//     The last two prove the window stays bank-aware (main RAM above $5FFF
//     is not spuriously echoed to the bus).
//
//  2. Synthesized //e status reads. After RAMRD on ($C003), reading RDRAMRD
//     ($C013) must return bit7=1 without a bus cycle; the program stores the
//     byte to main $0300, read back through the shadow port. $C019 RDVBLBAR
//     is checked against the one-cycle-lagged native video position.

module tb_vtw_video;

    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 0;
    always #3.75 clk = ~clk;   // 133.333 MHz

    logic rstn = 0;

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
        .host_is_iiplus(1'b0), .iiplus_data_tap(6'd52),
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
    logic [1:0]  speed_mode = 0;
    logic        video_vbl = 0;
    logic [8:0]  video_line = 9'd0;
    logic [6:0]  video_cycle = 7'd10;
    logic        sh_en = 0;
    logic [17:0] sh_addr = '0;
    logic        sh_we = 0;
    logic [7:0]  sh_wdata = '0;
    logic [7:0]  sh_rdata;
    logic [31:0] cnt_core_cycles;
    logic [31:0] cnt_posted_writes;
    logic [15:0] dbg_core_pc;
    logic [7:0]  rb;

    vtw_core_top dut (
        .clk(clk), .rstn(rstn), .enable(enable), .core_run(core_run),
        .assert_apple_res(1'b0),
        .speed_mode(speed_mode),
        .pace_divider(16'd0),
        .slow_region_en(10'd0),
        .slow_duration(16'd0),
        .disk2_timing_active(1'b0),
        .ramworks_en(1'b0),
        .video_vbl(video_vbl),
        .post_main_wide(1'b0),
        .video_mode_50hz(1'b0),
        .video_line(video_line),
        .video_cycle(video_cycle),
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
        .dbg_core_pc(dbg_core_pc), .cnt_core_cycles(cnt_core_cycles),
        .cnt_bus_cycles(), .cnt_posted_writes(cnt_posted_writes),
        .post_fill(), .post_high_water(), .cnt_post_drops(),
        .cnt_invalid_routes(),
        .dbg_vsss(), .dbg_last_sync_addr(), .dbg_last_sync_data(),
        .dbg_last_sync_rw(), .dbg_irq_edges(),
        .dbg_cxxx_ring(), .dbg_c0_ring()
    );

    // Motherboard read model: $Cxxx -> $EE, RAM otherwise. Posted writes go
    // onto the bus but are not captured here; cnt_posted_writes observes them.
    logic [7:0] mb_rdata;
    logic [7:0] mb_ram [0:16'hFFFF];
    always_comb begin
        if (apple_addr_pin[15:12] == 4'hC) mb_rdata = 8'hEE;
        else mb_rdata = mb_ram[apple_addr_pin];
    end
    wire mb_drive_data = phi0 && (apple_rw_pin === 1'b1) && !tini_data_dir_pin;
    assign apple_data_pin = mb_drive_data ? mb_rdata : 8'hzz;

    localparam logic [17:0] ROM_BASE = 18'h20000;   // shadow ROM = $C000
    localparam logic [17:0] PROG     = 18'h23000;    // shadow $F000

    task automatic sh_write(input logic [17:0] a, input logic [7:0] d);
        @(posedge clk);
        sh_en <= 1'b1; sh_we <= 1'b1; sh_addr <= a; sh_wdata <= d;
        @(posedge clk);
        sh_en <= 1'b0; sh_we <= 1'b0;
    endtask

    // Registered port-B read: hold b_en for two edges, then sample.
    task automatic sh_read(input logic [17:0] a, output logic [7:0] d);
        @(posedge clk);
        sh_en <= 1'b1; sh_we <= 1'b0; sh_addr <= a;
        @(posedge clk);   // address latched; registers load this edge
        @(posedge clk);   // b_rdata now reflects mem[a]
        d = sh_rdata;
        sh_en <= 1'b0;
    endtask

    // Hammer loop: STA <switch> once, then repeatedly STA <target> with a
    // few NOPs, jumping back into the store (keeping the switch set).
    task automatic load_hammer(input logic [7:0] switch_lo,
                               input logic [15:0] tgt);
        logic [17:0] p;
        p = PROG;
        sh_write(p,        8'h8D); sh_write(p+18'd1, switch_lo); sh_write(p+18'd2, 8'hC0);
        sh_write(p+18'd3,  8'h8D); sh_write(p+18'd4, tgt[7:0]);  sh_write(p+18'd5, tgt[15:8]);
        sh_write(p+18'd6,  8'hEA); sh_write(p+18'd7, 8'hEA);
        sh_write(p+18'd8,  8'hEA); sh_write(p+18'd9, 8'hEA);
        sh_write(p+18'd10, 8'h4C); sh_write(p+18'd11, 8'h03); sh_write(p+18'd12, 8'hF0);
        sh_write(ROM_BASE + 18'h3FFC, 8'h00);
        sh_write(ROM_BASE + 18'h3FFD, 8'hF0);
        for (int i = 0; i < 512; i++) sh_write(18'(i), 8'h00);
    endtask

    // Status probe: STA $C0<switch>; LDA $C0<status>; STA $0300; spin.
    task automatic load_status_probe(input logic [7:0] switch_lo,
                                     input logic [7:0] status_lo);
        logic [17:0] p;
        p = PROG;
        sh_write(p,        8'h8D); sh_write(p+18'd1, switch_lo); sh_write(p+18'd2, 8'hC0);
        sh_write(p+18'd3,  8'hAD); sh_write(p+18'd4, status_lo); sh_write(p+18'd5, 8'hC0);
        sh_write(p+18'd6,  8'h8D); sh_write(p+18'd7, 8'h00);     sh_write(p+18'd8, 8'h03);
        sh_write(p+18'd9,  8'h4C); sh_write(p+18'd10, 8'h09);    sh_write(p+18'd11, 8'hF0);
        sh_write(ROM_BASE + 18'h3FFC, 8'h00);
        sh_write(ROM_BASE + 18'h3FFD, 8'hF0);
        for (int i = 0; i < 512; i++) sh_write(18'(i), 8'h00);
    endtask

    int fails = 0;
    task automatic check(input bit cond, input string msg);
        if (!cond) begin
            fails++;
            $display("VTW VIDEO FAIL: %s", msg);
        end
    endtask

    task automatic boot();
        rstn = 0; res_drive_low = 1; enable = 0; core_run = 0;
        repeat (20) @(posedge clk);
        rstn = 1;
    endtask

    task automatic go();
        enable = 1; #10us;
        res_drive_low = 0; #4us;
        core_run = 1;
    endtask

    task automatic check_vbl_boundary(
        input logic [8:0] before_line,
        input logic [6:0] before_cycle,
        input logic before_vbl,
        input logic [8:0] sample_line,
        input logic [6:0] sample_cycle,
        input logic sample_vbl,
        input logic [7:0] expected,
        input string label
    );
        video_line = before_line;
        video_cycle = before_cycle;
        video_vbl = before_vbl;
        speed_mode = 2'd2;
        boot();
        load_status_probe(8'h02, 8'h19);
        go();

        // X_ROUTE has taken the early fallback snapshot. Advance the raw
        // scanner position at the next native tick, before this read's
        // data_en. RDVBLBAR must retain the preceding cycle's level.
        wait (dut.status_vbl_data_phase_q &&
              !dut.status_vbl_sampled_q);
        wait (ab_read.sss_en);
        video_line = sample_line;
        video_cycle = sample_cycle;
        video_vbl = sample_vbl;

        repeat (20) @(negedge phi0);
        sh_read(18'h00300, rb);
        check(rb == expected,
              $sformatf("%s ($%02h, want $%02h)", label, rb, expected));
    endtask

    localparam int WINDOW = 300;   // Apple cycles per measurement
    int posts_aux_shr, posts_aux_hi, posts_main_shr;

    task automatic measure_posts(output int delta);
        int a;
        repeat (120) @(negedge phi0);          // settle / re-lock
        a = int'(cnt_posted_writes);
        repeat (WINDOW) @(negedge phi0);
        delta = int'(cnt_posted_writes) - a;
    endtask

    initial begin
        // ---- 1a. AUX $9D00 (SHR SCB): must post ----
        boot();  load_hammer(8'h05, 16'h9D00);  go();
        measure_posts(posts_aux_shr);
        check(posts_aux_shr > WINDOW / 4,
              $sformatf("aux $9D00 (SHR) posts to bus (%0d over %0d)",
                        posts_aux_shr, WINDOW));

        // ---- 1b. AUX $B000 (above $9FFF): must NOT post ----
        boot();  load_hammer(8'h05, 16'hB000);  go();
        measure_posts(posts_aux_hi);
        check(posts_aux_hi < WINDOW / 20,
              $sformatf("aux $B000 (above SHR) not posted (%0d)", posts_aux_hi));

        // ---- 1c. MAIN $9D00 (RAMWRT off, above $5FFF): must NOT post ----
        boot();  load_hammer(8'h04, 16'h9D00);  go();
        measure_posts(posts_main_shr);
        check(posts_main_shr < WINDOW / 20,
              $sformatf("main $9D00 (above main window) not posted (%0d)",
                        posts_main_shr));

        check(posts_aux_shr >= 5 * (posts_aux_hi + 1),
              $sformatf("SHR aux window clearly distinct (shr=%0d hi=%0d)",
                        posts_aux_shr, posts_aux_hi));

        // ---- 2. Status read: RAMRD on -> RDRAMRD ($C013) bit7 = 1 ----
        boot();  load_status_probe(8'h03, 8'h13);  go();
        repeat (200) @(negedge phi0);
        sh_read(18'h00300, rb);
        check(rb == 8'h80,
              $sformatf("RDRAMRD reflects RAMRD=1 ($%02h, want $80)", rb));

        // ---- 3. $C019 RDVBLBAR in active display -> bit7=1 ----
        video_line = 9'd100;
        video_cycle = 7'd10;
        video_vbl = 0;
        boot();  load_status_probe(8'h02, 8'h19);  go();
        repeat (200) @(negedge phi0);
        sh_read(18'h00300, rb);
        check(rb == 8'h80,
              $sformatf("RDVBLBAR bit7=1 in active display ($%02h)", rb));

        // ---- 4. $C019 during vertical blank -> bit7=0 ----
        video_line = 9'd200;
        video_cycle = 7'd10;
        video_vbl = 1;
        boot();  load_status_probe(8'h02, 8'h19);  go();
        repeat (200) @(negedge phi0);
        sh_read(18'h00300, rb);
        check(rb == 8'h00,
              $sformatf("RDVBLBAR bit7=0 during vblank ($%02h)", rb));

        // ---- 5. Exact 1 MHz $C019 lags each raw VBL edge by one cycle ----
        check_vbl_boundary(
            9'd191, 7'd64, 1'b0, 9'd192, 7'd0, 1'b1, 8'h80,
            "RDVBLBAR remains active at raw VBL-start cycle zero");
        check_vbl_boundary(
            9'd192, 7'd0, 1'b1, 9'd192, 7'd1, 1'b1, 8'h00,
            "RDVBLBAR enters blanking at raw VBL-start cycle one");
        check_vbl_boundary(
            9'd261, 7'd64, 1'b1, 9'd0, 7'd0, 1'b0, 8'h00,
            "RDVBLBAR remains blank at raw frame-start cycle zero");
        check_vbl_boundary(
            9'd0, 7'd0, 1'b0, 9'd0, 7'd1, 1'b0, 8'h80,
            "RDVBLBAR returns active at raw frame-start cycle one");

        if (fails == 0) $display("VTW VIDEO PASS");
        else            $display("VTW VIDEO FAILED: %0d checks", fails);
        $finish;
    end

    initial begin
        #14ms;
        $display("VTW VIDEO FAIL: timeout");
        $finish;
    end

endmodule
