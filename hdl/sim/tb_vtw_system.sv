`timescale 1ns / 1ps
// Full-stack virtual TransWarp bench: the real W65C02 core executing a real
// program out of vtw_shadow, mastering the Apple bus through vtw_bus_engine,
// the arbiter, and the pin-level apple_bus_wrapper, against a motherboard
// model (PHI0 generator, weak bus pulls, RAM write capture, $C000 keyboard).
//
// The scenario is the vTW boot flow from README_VIRTUAL_TRANSWARP.md:
//   1. TB-as-ARM loads a program into the shadow ROM region over port B.
//   2. Session enable -> /DMA asserted (PHI1-only transitions), parked
//      driving begins, Apple RES# released, core released.
//   3. The program: reads $C000 over the bus (sync cycle), writes ZP
//      (shadow-only, invisible), posts a video write, floods 768 more
//      posted writes through the 512-deep queue (backpressure), writes
//      $C074=1 (sync write cycle + 1 MHz lock), then loops on shadow.
// Checks: every posted write reaches motherboard RAM in order, the sync
// read data lands in shadow, address/R-W transitions stay inside early
// PHI1, /DMA transitions stay inside PHI1, no queue drops, no invalid
// routes, and the 1 MHz lock paces the core to one cycle per Apple cycle.

module tb_vtw_system;

    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 0;
    always #3.75 clk = ~clk;   // 133.333 MHz

    logic rstn = 0;

    // ------------------------------------------------------------------
    // Apple bus pins with motherboard-style weak pulls
    // ------------------------------------------------------------------
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

    // PHI0: ~1.02 MHz, 490 ns per half.
    always #490 phi0 = ~phi0;

    // Motherboard reset driver (open-drain style).
    logic res_drive_low = 1;

    // ------------------------------------------------------------------
    // DUT stack: wrapper + arbiter + vtw_core_top
    // ------------------------------------------------------------------
    globals::AppleBus_read  ab_read;
    globals::AppleBus_write ab_write;
    globals::AppleBus_write vtw_ab_write;

    /* Model the production board's dedicated A2CTRL.RESET transistor.
     * A2FPGA.RESET inside the wrapper is observation-only. */
    assign apple_res_pin =
        (res_drive_low || ab_write.assert_res) ? 1'b0 : 1'bz;

    logic tini_oe_pin, tini_addr_dir_pin, tini_data_dir_pin;

    apple_bus_wrapper wrapper_i (
        .clk(clk),
        .rstn(rstn),
        .res_filtered_out(),
        .dbg_lost_cycle_count(), .dbg_clear(1'b0),
        .inh_allowed(1'b1),
        .gs_m2_qualify(1'b0),
        .m2sel_active_high(1'b0),
        .host_is_iiplus(1'b0),
        .iiplus_dma_refresh_active(1'b0),
        .apple_data_pin(apple_data_pin),
        .apple_addr_pin(apple_addr_pin),
        .apple_rw_pin(apple_rw_pin),
        .apple_phi0_pin(phi0),
        .apple_m2sel_pin(1'b0),
        .apple_m2b0_pin(1'b0),
        .apple_inh_pin(apple_inh_pin),
        .apple_res_pin(apple_res_pin),
        .apple_irq_pin(apple_irq_pin),
        .apple_rdy_pin(apple_rdy_pin),
        .apple_dma_pin(apple_dma_pin),
        .apple_nmi_pin(apple_nmi_pin),
        .tini_oe_pin(tini_oe_pin),
        .tini_5v_pin(1'b0),
        .tini_addr_dir_pin(tini_addr_dir_pin),
        .tini_data_dir_pin(tini_data_dir_pin),
        .ab_read(ab_read),
        .ab_write(ab_write)
    );

    apple_bus_write_arbiter #(.NUM_CLIENTS(1)) arbiter_i (
        .inh_allowed(1'b1),
        .client_writes({vtw_ab_write}),
        .ab_write(ab_write)
    );

    logic        enable = 0;
    logic        core_run = 0;
    logic        tb_assert_res = 0;
    logic [1:0]  speed_mode = 2'd0;
    logic        ignore_c074 = 0;
    logic        sp_boot_suppress = 1;
    /* Per-region slowdown config (feature off by default so the existing
     * scenario is unchanged; the slowdown bench drives these). */
    logic [8:0]  sd_region_en = 9'd0;
    logic [15:0] sd_duration  = 16'd0;
    logic        video_mode_50hz = 1'b0;
    logic [8:0]  video_line = 9'd253;
    logic [6:0]  video_cycle = 7'd27;
    logic        overlay_capture_armed = 1'b0;
    logic        overlay_capture_bank_aux = 1'b0;
    logic [15:0] overlay_capture_base = 16'h6000;
    logic [15:0] overlay_capture_limit = 16'h6001;
    logic        sh_en = 0;
    logic [17:0] sh_addr = '0;
    logic        sh_we = 0;
    logic [7:0]  sh_wdata = '0;
    logic [7:0]  sh_rdata;
    logic [1:0]  c074_state;
    logic        bus_owned;
    logic [15:0] dbg_core_pc;
    logic [31:0] cnt_core_cycles;
    logic [31:0] cnt_bus_cycles;
    logic [31:0] cnt_posted_writes;
    logic [9:0]  post_fill;
    logic [9:0]  post_high_water;
    logic [31:0] cnt_post_drops;
    logic [31:0] cnt_invalid_routes;
    logic        arm_post_we = 1'b0;
    logic [15:0] arm_post_addr = 16'h0000;
    logic [7:0]  arm_post_wdata = 8'h00;
    logic        arm_post_ready;
    logic        arm_rw_flush_req = 1'b0;
    logic        arm_rw_hold_release = 1'b0;
    logic        arm_rw_flush_done;
    logic        arm_rw_hold_state;
    logic        sp_active = 1'b0;
    logic        sp_req_valid;
    logic [2:0]  sp_req_target;
    logic [10:0] sp_req_addr;
    logic        sp_req_rw;
    logic [7:0]  sp_req_wdata;
    logic        sp_resp_valid = 1'b0;
    logic [7:0]  sp_resp_rdata = 8'h00;

    /* PSRAM line-port model: sparse 8 MB byte memory behind an
     * admission-like handshake with varying latency (30..~120 clk),
     * emulating psram_simple's once-per-bus-cycle background window. */
    logic        rw_req_valid, rw_req_rw, rw_req_ready, rw_resp_valid;
    logic [23:0] rw_req_addr;
    logic [63:0] rw_req_wline, rw_resp_rline;
    logic [7:0]  psram_model [logic [23:0]];

    logic        rwm_busy_q = 0, rwm_rw_q;
    logic [23:0] rwm_addr_q;
    logic [63:0] rwm_wline_q;
    int          rwm_lat_q, rwm_ops = 0;
    int          rwm_latency_override = -1;
    always @(posedge clk) begin
        rw_req_ready  <= 1'b0;
        rw_resp_valid <= 1'b0;
        if (rw_req_valid && !rwm_busy_q) begin
            rwm_busy_q  <= 1'b1;
            rwm_rw_q    <= rw_req_rw;
            rwm_addr_q  <= {rw_req_addr[23:3], 3'b000};
            rwm_wline_q <= rw_req_wline;
            rwm_lat_q   <= (rwm_latency_override >= 0)
                         ? rwm_latency_override
                         : 30 + ((rwm_ops * 37) % 90);
            rwm_ops     <= rwm_ops + 1;
            rw_req_ready <= 1'b1;
        end
        else if (rwm_busy_q) begin
            if (rwm_lat_q != 0) begin
                rwm_lat_q <= rwm_lat_q - 1;
            end
            else begin
                if (rwm_rw_q) begin
                    for (int b = 0; b < 8; b++) begin
                        rw_resp_rline[8*b +: 8] <=
                            psram_model.exists(rwm_addr_q + 24'(b)) ?
                            psram_model[rwm_addr_q + 24'(b)] : 8'hFF;
                    end
                end
                else begin
                    for (int b = 0; b < 8; b++) begin
                        psram_model[rwm_addr_q + 24'(b)] = rwm_wline_q[8*b +: 8];
                    end
                end
                rw_resp_valid <= 1'b1;
                rwm_busy_q    <= 1'b0;
            end
        end
    end

    vtw_core_top dut (
        .clk(clk),
        .rstn(rstn),
        .enable(enable),
        .host_is_iiplus(1'b0),
        .core_run(core_run),
        .assert_apple_res(tb_assert_res),
        .speed_mode(speed_mode),  // starts with full-rate bursts
        .pace_divider(16'd0),
        .ignore_c074(ignore_c074),
        .irq_assert_in(1'b0),
        .data_drive_in(vtw_ab_write.wr_data_en),
        .data_drive_value_in(vtw_ab_write.wr_data),
        .dbg_clear(1'b0),
        .iiplus_buttons_zero(1'b0),
        .slow_region_en(sd_region_en),
        .slow_duration(sd_duration),
        .d2_active(1'b0),
        .d2_req_valid(), .d2_req_addr(), .d2_req_ready(1'b0),
        .d2_resp_valid(1'b0), .d2_resp_rdata(8'd0),
        .d2_cycle_tick(), .d2_native_cycle_active(),
        .d2_time_ready(1'b1), .d2_write_timing_active(1'b0),
        .ramworks_en(1'b1),
        .video_vbl(1'b0),
        .post_main_wide(1'b0),
        .overlay_capture_armed(overlay_capture_armed),
        .overlay_capture_bank_aux(overlay_capture_bank_aux),
        .overlay_capture_base(overlay_capture_base),
        .overlay_capture_limit(overlay_capture_limit),
        .video_mode_50hz(video_mode_50hz),
        .video_line(video_line),
        .video_cycle(video_cycle),
        .ab_read(ab_read),
        .ab_write(vtw_ab_write),
        .rw_req_valid(rw_req_valid),
        .rw_req_rw(rw_req_rw),
        .rw_req_addr(rw_req_addr),
        .rw_req_wline(rw_req_wline),
        .rw_req_ready(rw_req_ready),
        .rw_resp_valid(rw_resp_valid),
        .rw_resp_rline(rw_resp_rline),
        .sp_active(sp_active),
        .sp_boot_suppress(sp_boot_suppress),
        .sp_req_valid(sp_req_valid),
        .sp_req_target(sp_req_target),
        .sp_req_addr(sp_req_addr),
        .sp_req_rw(sp_req_rw),
        .sp_req_wdata(sp_req_wdata),
        .sp_req_ready(1'b1),
        .sp_resp_valid(sp_resp_valid),
        .sp_resp_rdata(sp_resp_rdata),
        .sp_sss_snapshot(),
        .sh_en(sh_en),
        .sh_addr(sh_addr),
        .sh_we(sh_we),
        .sh_wdata(sh_wdata),
        .sh_rdata(sh_rdata),
        .arm_req_valid(1'b0),
        .arm_req_addr('0),
        .arm_req_rw(1'b1),
        .arm_req_wdata('0),
        .arm_req_busy(),
        .arm_resp_valid(),
        .arm_resp_rdata(),
        .arm_post_we(arm_post_we),
        .arm_post_addr(arm_post_addr),
        .arm_post_wdata(arm_post_wdata),
        .arm_post_ready(arm_post_ready),
        .arm_rw_flush_req(arm_rw_flush_req),
        .arm_rw_hold_release(arm_rw_hold_release),
        .arm_rw_flush_done(arm_rw_flush_done),
        .arm_rw_hold_state(arm_rw_hold_state),
        .c074_state(c074_state),
        .bus_owned(bus_owned),
        .dbg_core_pc(dbg_core_pc),
        .cnt_core_cycles(cnt_core_cycles),
        .cnt_bus_cycles(cnt_bus_cycles),
        .cnt_posted_writes(cnt_posted_writes),
        .post_fill(post_fill),
        .post_high_water(post_high_water),
        .cnt_post_drops(cnt_post_drops),
        .cnt_invalid_routes(cnt_invalid_routes)
    );

    // ------------------------------------------------------------------
    // Motherboard model: read drive during PHI0-high, write capture at
    // the PHI0 fall, $C000 keyboard register.
    // ------------------------------------------------------------------
    logic [7:0] kbd_value = 8'hA7;
    logic [7:0] mb_ram [0:16'hFFFF];

    logic [7:0] mb_rdata;
    always_comb begin
        if (apple_addr_pin == 16'hC000) mb_rdata = kbd_value;
        else if (apple_addr_pin[15:12] == 4'hC) mb_rdata = 8'hEE; // I/O misc
        else mb_rdata = mb_ram[apple_addr_pin];
    end
    wire mb_drive_data = phi0 && (apple_rw_pin === 1'b1) && !tini_data_dir_pin;
    assign apple_data_pin = mb_drive_data ? mb_rdata : 8'hzz;

    /* Mirror apple_top's one-shot release condition for the focused cold
     * scan scenario in this bench. The core itself owns the deterministic
     * slot-7 no-card response; the integration latch clears when the real
     * bus cycle for slot 6 appears. */
    always_ff @(posedge clk) begin
        if (!rstn) begin
            sp_boot_suppress <= 1'b1;
        end else if (sp_boot_suppress && bus_owned &&
                     ab_read.serve_en && ab_read.rw &&
                     (ab_read.addr[15:8] == 8'hC6)) begin
            sp_boot_suppress <= 1'b0;
        end
    end

    typedef struct { logic [15:0] addr; logic [7:0] data; } wrec_t;
    wrec_t wrecs [$];
    int c074_bus_writes = 0;
    logic [7:0] last_c074_bus_data = 8'h00;

    always @(negedge phi0) begin
        if (apple_rw_pin === 1'b0) begin
            mb_ram[apple_addr_pin] <= apple_data_pin;
            wrecs.push_back('{apple_addr_pin, apple_data_pin});
            if (apple_addr_pin === 16'hC074) begin
                c074_bus_writes <= c074_bus_writes + 1;
                last_c074_bus_data <= apple_data_pin;
            end
        end
    end

    // ------------------------------------------------------------------
    // Contract monitors
    // ------------------------------------------------------------------
    int fails = 0;
    task automatic check(input bit cond, input string msg);
        if (!cond) begin
            fails++;
            $display("VTW SYSTEM FAIL: %s (t=%0t)", msg, $time);
        end
    endtask

    /* X_CAPTURE must save the same route tuple that the old X_ROUTE
     * combinational translator produced from the captured cycle and its
     * pre-access switch state. Count each route class so this assertion is
     * not satisfied by only the boot-ROM path. */
    int route_tuple_checks = 0;
    int route_bus_checks = 0;
    int route_rom_checks = 0;
    int route_main_checks = 0;
    int route_aux_checks = 0;
    int route_ramworks_checks = 0;

    always @(posedge clk) begin : route_tuple_monitor
        globals::apple_route_kind_e ref_route;
        logic [31:0] ref_decoded;
        logic        ref_shadow_valid;
        logic [17:0] ref_shadow_phys;

        if (rstn && dut.ssm_apply_pulse) begin
            globals::translate_apple_addr(
                dut.cycle_translate_state_q,
                dut.cycle_addr_q,
                dut.cycle_rw_q,
                ref_decoded,
                ref_route);
            vtw_shadow_pkg::vtw_shadow_map(
                ref_route,
                ref_decoded,
                ref_shadow_valid,
                ref_shadow_phys);

            check(dut.cycle_xl_decoded_q === ref_decoded &&
                  dut.cycle_xl_route_q === ref_route &&
                  dut.cycle_xl_shadow_valid_q === ref_shadow_valid &&
                  dut.cycle_xl_shadow_phys_q === ref_shadow_phys,
                  "X_CAPTURE saves the exact pre-access route tuple");
            route_tuple_checks++;

            unique case (ref_route)
                globals::APPLE_ROUTE_BUS: route_bus_checks++;
                globals::APPLE_ROUTE_ROM: route_rom_checks++;
                globals::APPLE_ROUTE_CACHE: begin
                    if (!ref_shadow_valid)
                        route_ramworks_checks++;
                    else if (ref_shadow_phys[16])
                        route_aux_checks++;
                    else
                        route_main_checks++;
                end
                default: ;
            endcase
        end
    end

    /* Private RamWorks switch phase checks. The manager must keep the old
     * bank through X_CAPTURE, apply the saved C071/C073 tuple at X_ROUTE,
     * and expose the new bank to the next captured access. */
    logic       bank_apply_pending = 1'b0;
    logic       bank_next_pending  = 1'b0;
    logic [6:0] bank_old_q          = 7'd0;
    logic [6:0] bank_new_q          = 7'd0;
    logic [15:0] bank_switch_addr_q = 16'h0000;
    int c071_capture_checks = 0;
    int c071_apply_checks   = 0;
    int c071_next_checks    = 0;
    int c073_capture_checks = 0;
    int c073_apply_checks   = 0;
    int c073_next_checks    = 0;

    always @(posedge clk) begin
        if (!rstn) begin
            bank_apply_pending = 1'b0;
            bank_next_pending  = 1'b0;
        end
        else if (dut.ssm_pulse && !dut.core_rwb &&
                 (dut.core_addr == 16'hC071 ||
                  dut.core_addr == 16'hC073)) begin
            check(!bank_apply_pending && !bank_next_pending,
                  "RamWorks switch capture has no prior phase pending");
            bank_old_q          = dut.vsss.sw_ramworks_bank;
            bank_new_q          = dut.core_data_out[6:0];
            bank_switch_addr_q  = dut.core_addr;
            bank_apply_pending  = 1'b1;
            if (dut.core_addr == 16'hC071)
                c071_capture_checks++;
            else
                c073_capture_checks++;
            #1ps;
            check(dut.vsss.sw_ramworks_bank == bank_old_q,
                  "RamWorks bank stays old at X_CAPTURE");
            check(dut.cycle_addr_q == bank_switch_addr_q &&
                  !dut.cycle_rw_q &&
                  dut.cycle_wdata_q[6:0] == bank_new_q,
                  "X_CAPTURE saves the complete RamWorks switch tuple");
            check(dut.cycle_translate_state_q.sw_ramworks_bank == bank_old_q,
                  "RamWorks switch access snapshots the pre-access bank");
        end
        else if (dut.ssm_apply_pulse && !dut.cycle_rw_q &&
                 (dut.cycle_addr_q == 16'hC071 ||
                  dut.cycle_addr_q == 16'hC073)) begin
            check(bank_apply_pending &&
                  dut.cycle_addr_q == bank_switch_addr_q &&
                  dut.cycle_wdata_q[6:0] == bank_new_q,
                  "X_ROUTE applies the pending RamWorks switch tuple");
            #1ps;
            check(dut.vsss.sw_ramworks_bank == bank_new_q,
                  "RamWorks bank changes at X_ROUTE");
            check(dut.cycle_translate_state_q.sw_ramworks_bank == bank_old_q,
                  "current RamWorks switch cycle keeps its old-bank snapshot");
            bank_apply_pending = 1'b0;
            bank_next_pending  = 1'b1;
            if (bank_switch_addr_q == 16'hC071)
                c071_apply_checks++;
            else
                c073_apply_checks++;
        end
        else if (bank_next_pending && dut.ssm_pulse) begin
            #1ps;
            check(dut.cycle_translate_state_q.sw_ramworks_bank == bank_new_q,
                  "next vTW access snapshots the new RamWorks bank");
            bank_next_pending = 1'b0;
            if (bank_switch_addr_q == 16'hC071)
                c071_next_checks++;
            else
                c073_next_checks++;
        end
    end

    /* Every accepted core or ARM post must emerge from the new boundary on
     * exactly the next fabric edge with the same tuple. This point-by-point
     * check catches loss, duplication, and address/data mixing. */
    logic        post_expected_valid_q = 1'b0;
    logic [15:0] post_expected_addr_q  = 16'h0000;
    logic [7:0]  post_expected_data_q  = 8'h00;
    int          post_accept_checks    = 0;
    int          post_emit_checks      = 0;

    always @(posedge clk) begin
        if (!rstn || !enable || !ab_read.res) begin
            check(!dut.eng_post_we,
                  "posted boundary emits no tuple across a clear boundary");
            post_expected_valid_q = 1'b0;
        end
        else begin
            check(dut.eng_post_we === post_expected_valid_q,
                  "posted boundary valid is delayed by exactly one clock");
            if (post_expected_valid_q) begin
                check(dut.eng_post_addr == post_expected_addr_q &&
                      dut.eng_post_wdata == post_expected_data_q,
                      "posted boundary keeps the accepted address/data tuple");
                post_emit_checks++;
            end

            post_expected_valid_q = dut.core_post_accept ||
                                    dut.arm_post_accept;
            if (dut.core_post_accept) begin
                post_expected_addr_q = dut.cycle_addr_q;
                post_expected_data_q = dut.cycle_wdata_q;
                post_accept_checks++;
            end
            else if (dut.arm_post_accept) begin
                post_expected_addr_q = arm_post_addr;
                post_expected_data_q = arm_post_wdata;
                post_accept_checks++;
            end
        end
    end

    logic monitors_armed = 0;
    realtime last_fall = 0;
    always @(negedge phi0) last_fall = $realtime;

    /* Address/R-W may change only in early PHI1 (the drive tap plus CDC and
     * majority-filter lag lands ~100-140 ns after the true fall). */
    always @(apple_addr_pin or apple_rw_pin) begin
        if (monitors_armed && tini_addr_dir_pin) begin
            check(phi0 === 1'b0 && ($realtime - last_fall) < 250.0,
                  $sformatf("addr/rw transition outside early PHI1 (addr=%h dt=%0t)",
                            apple_addr_pin, $realtime - last_fall));
        end
    end

    /* /DMA transitions only during PHI1 (Apple IIe Tech Note #2). */
    always @(apple_dma_pin) begin
        if (rstn) begin
            check(phi0 === 1'b0, "/DMA transition outside PHI1");
        end
    end

    // ------------------------------------------------------------------
    // TB-as-ARM shadow access
    // ------------------------------------------------------------------
    task automatic sh_write(input logic [17:0] a, input logic [7:0] d);
        @(posedge clk);
        sh_en <= 1'b1; sh_we <= 1'b1; sh_addr <= a; sh_wdata <= d;
        @(posedge clk);
        sh_en <= 1'b0; sh_we <= 1'b0;
    endtask

    task automatic sh_read(input logic [17:0] a, output logic [7:0] d);
        @(posedge clk);
        sh_en <= 1'b1; sh_we <= 1'b0; sh_addr <= a;
        @(posedge clk);
        sh_en <= 1'b0;
        @(posedge clk);
        d = sh_rdata;
    endtask

    task automatic arm_post_push(input logic [15:0] a,
                                 input logic [7:0] d);
        @(negedge clk);
        arm_post_addr  = a;
        arm_post_wdata = d;
        arm_post_we    = 1'b1;
        do @(posedge clk); while (!arm_post_ready);
        @(negedge clk);
        arm_post_we = 1'b0;
    endtask

    task automatic arm_post_burst(input logic [15:0] base,
                                  input logic [7:0] first_data,
                                  input int count);
        @(negedge clk);
        arm_post_we    = 1'b1;
        arm_post_addr  = base;
        arm_post_wdata = first_data;
        for (int i = 0; i < count; i++) begin
            do @(posedge clk); while (!arm_post_ready);
            @(negedge clk);
            if (i + 1 < count) begin
                arm_post_addr  = base + 16'(i + 1);
                arm_post_wdata = first_data + 8'(i + 1);
            end
            else begin
                arm_post_we = 1'b0;
            end
        end
    endtask

    // ------------------------------------------------------------------
    // Program (hand-assembled, see header). ROM region: CPU $F000 ->
    // shadow phys $23000; vectors at $23FFC/D.
    // ------------------------------------------------------------------
    localparam logic [17:0] ROM_BASE = 18'h20000;
    localparam byte PROGRAM [0:95] = '{
        8'hA2, 8'h00,               // F000 LDX #$00
        8'hAD, 8'h00, 8'hC0,        // F002 LDA $C000
        8'h85, 8'h10,               // F005 STA $10
        8'hA9, 8'h5A,               // F007 LDA #$5A
        8'h8D, 8'h00, 8'h04,        // F009 STA $0400
        8'hA9, 8'h11,               // F00C LDA #$11
        8'h9D, 8'h00, 8'h20,        // F00E STA $2000,X
        8'hE8,                      // F011 INX
        8'hD0, 8'hFA,               // F012 BNE $F00E
        8'hA2, 8'h00,               // F014 LDX #$00
        8'h9D, 8'h00, 8'h21,        // F016 STA $2100,X
        8'hE8,                      // F019 INX
        8'hD0, 8'hFA,               // F01A BNE $F016
        8'hA2, 8'h00,               // F01C LDX #$00
        8'h9D, 8'h00, 8'h22,        // F01E STA $2200,X
        8'hE8,                      // F021 INX
        8'hD0, 8'hFA,               // F022 BNE $F01E
        8'hA9, 8'h01,               // F024 LDA #$01
        8'h8D, 8'h74, 8'hC0,        // F026 STA $C074
        // ---- Banking discriminator: the //e boot-flow switch dance ----
        8'h8D, 8'h06, 8'hC0,        // F029 STA $C006  INTCXROM off
        // Disk II accelerated cold-scan gate: slot 7 is absent until the
        // scan reaches slot 6, then immediately becomes visible again.
        8'hAD, 8'h00, 8'hC7,        // F02C LDA $C700  -> internal no-card $FF
        8'h85, 8'h1A,               // F02F STA $1A
        8'hAD, 8'h00, 8'hC6,        // F031 LDA $C600  -> bus $EE, releases gate
        8'h85, 8'h1B,               // F034 STA $1B
        8'hAD, 8'h01, 8'hC7,        // F036 LDA $C701  -> bus (slot ROM, $EE)
        8'h85, 8'h12,               // F039 STA $12
        8'h8D, 8'h07, 8'hC0,        // F03B STA $C007  INTCXROM on
        8'hAD, 8'h01, 8'hC7,        // F03E LDA $C701  -> shadow internal ($88)
        8'h85, 8'h13,               // F041 STA $13
        8'h8D, 8'h06, 8'hC0,        // F043 STA $C006  INTCXROM off again
        8'hAD, 8'h01, 8'hC7,        // F046 LDA $C701  -> bus again ($EE)
        8'h85, 8'h14,               // F049 STA $14
        8'hAD, 8'h10, 8'hC3,        // F04B LDA $C310  -> internal slot-3 ROM
        8'h85, 8'h15,               // F04E STA $15    ($33; sets INTC8ROM)
        8'hAD, 8'h00, 8'hC8,        // F050 LDA $C800  -> shadow internal C8 ($C8)
        8'h85, 8'h16,               // F053 STA $16
        8'hAD, 8'hFF, 8'hCF,        // F055 LDA $CFFF  releases INTC8ROM
        8'hAD, 8'h00, 8'hC8,        // F058 LDA $C800  -> bus now ($EE)
        8'h85, 8'h17,               // F05B STA $17
        /* The LC test must run from RAM: switching LC RAM in makes this
         * very ROM code disappear from the address space (as on a real
         * //e -- the first run of this bench proved it the hard way). */
        8'h4C, 8'h00, 8'h03         // F05D JMP $0300
    };

    localparam byte LC_PROG [0:96] = '{
        8'hAD, 8'h83, 8'hC0,        // 0300 LDA $C083  LC RAM read (1st)
        8'hAD, 8'h83, 8'hC0,        // 0303 LDA $C083  LC RAM read+write (2nd)
        8'hAD, 8'h22, 8'hD0,        // 0306 LDA $D022  -> shadow LC RAM ($42)
        8'h85, 8'h18,               // 0309 STA $18
        8'hAD, 8'h82, 8'hC0,        // 030B LDA $C082  LC ROM
        8'hAD, 8'h22, 8'hD0,        // 030E LDA $D022  -> shadow ROM copy ($99)
        8'h85, 8'h19,               // 0311 STA $19
        // ---- Aux video write-through + bank-steer flush ordering ----
        8'h8D, 8'h01, 8'hC0,        // 0313 STA $C001  80STORE on (flushes)
        8'h8D, 8'h55, 8'hC0,        // 0316 STA $C055  PAGE2 on -> aux window
        8'hA9, 8'h77,               // 0319 LDA #$77
        8'h8D, 8'h27, 8'h04,        // 031B STA $0427  aux write -> posted
        8'h8D, 8'h54, 8'hC0,        // 031E STA $C054  PAGE2 off (must flush)
        8'hA9, 8'h78,               // 0321 LDA #$78
        8'h8D, 8'h28, 8'h04,        // 0323 STA $0428  main write -> posted
        8'h8D, 8'h00, 8'hC0,        // 0326 STA $C000  80STORE off
        8'h4C, 8'h80, 8'h01,        // 0329 JMP $0180  RamWorks segment
        /* FLOATBUS sanity probes (RamWorks segment jumps here). At raw line
         * 253/cycle 27, the two-cycle rewind selects scanner cycle 25 and
         * main $37F8. Every read in $C030-$C05F must return that byte while
         * still executing its physical speaker/video/annunciator effect. */
        8'h2C, 8'h57, 8'hC0,        // 032C BIT $C057  HIRES on
        8'h2C, 8'h50, 8'hC0,        // 032F BIT $C050  TEXT off
        8'hAD, 8'h30, 8'hC0,        // 0332 LDA $C030  speaker + floating bus
        8'h85, 8'h12,               // 0335 STA $12
        8'hAD, 8'h47, 8'hC0,        // 0337 LDA $C047  undriven motherboard I/O
        8'h85, 8'h24,               // 033A STA $24
        8'hAD, 8'h4F, 8'hC0,        // 033C LDA $C04F  undriven motherboard I/O
        8'h85, 8'h25,               // 033F STA $25
        8'hAD, 8'h50, 8'hC0,        // 0341 LDA $C050  graphics switch
        8'h85, 8'h26,               // 0344 STA $26
        8'hAD, 8'h57, 8'hC0,        // 0346 LDA $C057  graphics switch
        8'h85, 8'h27,               // 0349 STA $27
        8'hAD, 8'h58, 8'hC0,        // 034B LDA $C058  annunciator
        8'h85, 8'h28,               // 034E STA $28
        8'hAD, 8'h5A, 8'hC0,        // 0350 LDA $C05A  FLOATBUS ZIPLOCK read
        8'h85, 8'h29,               // 0353 STA $29
        8'h4C, 8'h58, 8'h03,        // 0355 JMP $0358  timing loop
        /* Timing loop for the cycle-exact 1 MHz check: LDA abs (4) +
         * INC zp (5) + NOP (2) + JMP abs (3) = 14 cycles, identical on
         * NMOS and 65C02. Consecutive $C000 sync reads must be exactly
         * 14 Apple cycles apart under the lock. */
        8'hAD, 8'h00, 8'hC0,        // 0358 LDA $C000  sync bus read
        8'hE6, 8'h11,               // 035B INC $11
        8'hEA,                      // 035D NOP
        8'h4C, 8'h58, 8'h03         // 035E JMP $0358
    };

    /* RamWorks exercise. Lives in the stack page ($0180): zp/stack routing
     * ignores RAMRD/RAMWRT (ALTZP off), so this code stays fetchable while
     * the data switches point at aux -- main-RAM code would vanish mid-
     * segment the instant RAMRD turns on. Covers write-allocate misses,
     * a cache-hit write, flush-on-eviction chains, bank isolation via
     * $C071/$C073, and read-back through fills; results land in main ZP $20-$23
     * and the flushed lines in the TB's PSRAM model. */
    localparam byte RW_PROG [0:79] = '{
        8'hA9, 8'h02,               // 0180 LDA #$02
        8'h8D, 8'h73, 8'hC0,        // 0182 STA $C073  bank 2 (decode bank 3)
        8'h8D, 8'h05, 8'hC0,        // 0185 STA $C005  RAMWRT on
        8'hA9, 8'hA5,               // 0188 LDA #$A5
        8'h8D, 8'h00, 8'h18,        // 018A STA $1800  miss: fill + patch
        8'hA9, 8'h77,               // 018D LDA #$77
        8'h8D, 8'h03, 8'h18,        // 018F STA $1803  cache-hit write
        8'hA9, 8'h5A,               // 0192 LDA #$5A
        8'h8D, 8'h08, 8'h18,        // 0194 STA $1808  next line: flush + fill
        8'hA9, 8'h05,               // 0197 LDA #$05
        8'h8D, 8'h71, 8'hC0,        // 0199 STA $C071  bank 5 (decode bank 6)
        8'hA9, 8'h3C,               // 019C LDA #$3C
        8'h8D, 8'h00, 8'h18,        // 019E STA $1800  other bank, same address
        8'h8D, 8'h04, 8'hC0,        // 01A1 STA $C004  RAMWRT off
        8'h8D, 8'h03, 8'hC0,        // 01A4 STA $C003  RAMRD on
        8'hA9, 8'h02,               // 01A7 LDA #$02
        8'h8D, 8'h73, 8'hC0,        // 01A9 STA $C073  bank 2
        8'hAD, 8'h00, 8'h18,        // 01AC LDA $1800  expect $A5
        8'h85, 8'h20,               // 01AF STA $20    (zp: main regardless)
        8'hAD, 8'h03, 8'h18,        // 01B1 LDA $1803  expect $77
        8'h85, 8'h21,               // 01B4 STA $21
        8'hAD, 8'h08, 8'h18,        // 01B6 LDA $1808  expect $5A
        8'h85, 8'h22,               // 01B9 STA $22
        8'hA9, 8'h05,               // 01BB LDA #$05
        8'h8D, 8'h73, 8'hC0,        // 01BD STA $C073  bank 5
        8'hAD, 8'h00, 8'h18,        // 01C0 LDA $1800  expect $3C
        8'h85, 8'h23,               // 01C3 STA $23
        8'h8D, 8'h02, 8'hC0,        // 01C5 STA $C002  RAMRD off
        8'hA9, 8'h00,               // 01C8 LDA #$00
        8'h8D, 8'h73, 8'hC0,        // 01CA STA $C073  bank 0
        8'h4C, 8'h2C, 8'h03         // 01CD JMP $032C  floating-read probe
    };

    task automatic load_program();
        // Program at ROM offset $3000 ($F000 - $C000).
        for (int i = 0; i < 96; i++) begin
            sh_write(ROM_BASE + 18'h3000 + 18'(i), PROGRAM[i]);
        end
        // LC + aux test routine in shadow main RAM at $0300.
        for (int i = 0; i < 97; i++) begin
            sh_write(18'h00300 + 18'(i), LC_PROG[i]);
        end
        // Reset vector -> $F000.
        sh_write(ROM_BASE + 18'h3FFC, 8'h00);
        sh_write(ROM_BASE + 18'h3FFD, 8'hF0);
        // Zero ZP + stack so reset-sequence and loop reads are defined.
        for (int i = 0; i < 512; i++) begin
            sh_write(18'(i), 8'h00);
        end
        // RamWorks segment in the stack page (after the zero sweep).
        for (int i = 0; i < 80; i++) begin
            sh_write(18'h00180 + 18'(i), RW_PROG[i]);
        end
        // Discriminator landmarks in the shadow's "internal ROM" copy and
        // memory banks (values the bus model never returns; it serves $EE
        // for all non-$C000 I/O-space reads).
        sh_write(ROM_BASE + 18'h0701, 8'h88);  // internal $C701
        sh_write(ROM_BASE + 18'h0310, 8'h33);  // internal $C310
        sh_write(ROM_BASE + 18'h0800, 8'hC8);  // internal $C800
        sh_write(ROM_BASE + 18'h1022, 8'h99);  // ROM $D022
        sh_write(ROM_BASE + 18'h0FFF, 8'h00);  // internal $CFFF (release read)
        sh_write(18'h0D022, 8'h42);            // main-bank LC RAM $D022 (bank 2)
        /* Address-sensitive scanner landmarks. Only $37F8 is correct for
         * HGR page 1 at raw line 253/cycle 27 after the two-cycle rewind;
         * adjacent cycles and the text-page address carry different values
         * so a phase/mode error cannot pass. */
        sh_write(18'h037F7, 8'hE1);
        sh_write(18'h037F8, 8'hD7);
        sh_write(18'h037F9, 8'hE2);
        sh_write(18'h007F8, 8'hE3);
    endtask

    // ------------------------------------------------------------------
    // Scenario
    // ------------------------------------------------------------------
    int flood_seen;
    int c074_seen;
    int c006_seen;
    int c007_seen;
    int c071_seen;
    int c073_seen;
    int idx_aux, idx_main, idx_c054;
    logic [7:0] rd;
    int cyc_a, cyc_b;
    int arm_rec_base, arm_post_before, c074_override_base;

    /* Cycle-exactness probe: Apple-cycle stamps of every $C000 sync read
     * (parked $Cxxx replays are sanitized to $FFFF, so each appearance is
     * one real sync cycle). */
    int apple_cyc_cnt;
    int c6_bus_reads = 0;
    int c7_bus_reads = 0;
    bit collect_c000 = 0;
    int c000_stamps[$];
    always @(negedge phi0) begin
        apple_cyc_cnt++;
        if (monitors_armed && apple_rw_pin === 1'b1) begin
            if (apple_addr_pin[15:8] == 8'hC6) c6_bus_reads++;
            if (apple_addr_pin[15:8] == 8'hC7) c7_bus_reads++;
        end
        if (collect_c000 && apple_addr_pin === 16'hC000 &&
            apple_rw_pin === 1'b1) begin
            c000_stamps.push_back(apple_cyc_cnt);
        end
    end

    initial begin
        repeat (20) @(posedge clk);
        rstn = 1;

        load_program();

        /* Session start with the Apple still in reset: the engine must NOT
         * take the bus while RES# is low -- the motherboard MMU/IOU only
         * process a reset under stock bus conditions (no DMA master). */
        enable = 1;
        #10us;
        check(apple_dma_pin === 1'b1, "no takeover while Apple RES# is held");

        // Release the Apple: the takeover proceeds.
        res_drive_low = 0;
        fork : dma_wait
            begin
                wait (apple_dma_pin === 1'b0);
                disable dma_wait;
            end
            begin
                /* 2.1 us RES# filter + 80 stock cycles (~78 us) + arm and
                 * grace: the takeover lands around 85 us after release. */
                #150us;
                check(0, "/DMA never asserted");
                disable dma_wait;
            end
        join
        #5us;
        check(bus_owned, "parked driver active before core release");
        check(tini_addr_dir_pin, "address transceiver driving (never floats)");
        monitors_armed = 1;

        core_run = 1;

        // Wait for the whole program to retire on the bus: 788 write
        // records (1 + 768 + 2 posted; sync writes $C074, $C006 x2,
        // $C007, $C001, $C055, $C054, $C000, then the RamWorks segment's
        // $C071 x1, $C073 x4, and $C005/$C004/$C003/$C002).
        fork : prog_wait
            begin
                wait (wrecs.size() >= 788);
                disable prog_wait;
            end
            begin
                #5ms;
                check(0, $sformatf("program timeout: %0d bus writes seen",
                                   wrecs.size()));
                disable prog_wait;
            end
        join

        check(wrecs.size() >= 788, "all bus writes arrived");
        if (wrecs.size() >= 788) begin
            // First record: the $0400 posted write.
            check(wrecs[0].addr == 16'h0400 && wrecs[0].data == 8'h5A,
                  "posted $0400 write first and intact");
            // Flood records in exact order; sync writes (speed register,
            // soft switches) may overtake the queue tail (decoupled
            // engine, like the real card) so they can appear anywhere.
            flood_seen = 0;
            c074_seen  = 0;
            c006_seen  = 0;
            c007_seen  = 0;
            c071_seen  = 0;
            c073_seen  = 0;
            for (int i = 1; i < wrecs.size(); i++) begin
                if (wrecs[i].addr == 16'hC074) begin
                    check(wrecs[i].data == 8'h01, "$C074 write data");
                    c074_seen++;
                end
                else if (wrecs[i].addr == 16'hC006) begin
                    c006_seen++;
                end
                else if (wrecs[i].addr == 16'hC007) begin
                    c007_seen++;
                end
                else if (wrecs[i].addr == 16'hC001 ||
                         wrecs[i].addr == 16'hC000 ||
                         wrecs[i].addr == 16'hC055) begin
                    // Aux-test steering switches; ordering checked below.
                end
                else if (wrecs[i].addr == 16'hC054) begin
                    idx_c054 = i;
                end
                else if (wrecs[i].addr == 16'h0427) begin
                    check(wrecs[i].data == 8'h77, "aux posted write data");
                    idx_aux = i;
                end
                else if (wrecs[i].addr == 16'h0428) begin
                    check(wrecs[i].data == 8'h78, "main posted write data");
                    idx_main = i;
                end
                else if (wrecs[i].addr == 16'hC071) begin
                    c071_seen++;
                end
                else if (wrecs[i].addr == 16'hC073) begin
                    c073_seen++;
                end
                else if (wrecs[i].addr == 16'hC005 ||
                         wrecs[i].addr == 16'hC004 ||
                         wrecs[i].addr == 16'hC003 ||
                         wrecs[i].addr == 16'hC002) begin
                    // RamWorks segment bank/switch writes (in the 788).
                end
                else begin
                    automatic logic [15:0] expect_addr =
                        (flood_seen < 256) ? 16'h2000 + 16'(flood_seen) :
                        (flood_seen < 512) ? 16'h2100 + 16'(flood_seen - 256) :
                                             16'h2200 + 16'(flood_seen - 512);
                    if (flood_seen < 768) begin
                        check(wrecs[i].addr == expect_addr &&
                              wrecs[i].data == 8'h11,
                              $sformatf("flood write %0d in order (got %h=%h)",
                                        flood_seen, wrecs[i].addr, wrecs[i].data));
                    end
                    flood_seen++;
                end
            end
            check(flood_seen == 768, "flood write count");
            check(c074_seen == 1, "$C074 written exactly once");
            check(c006_seen == 2 && c007_seen == 1,
                  "INTCXROM switch writes reached the real MMU");
            check(c071_seen == 1 && c073_seen == 4,
                  "both RamWorks bank-select addresses reached the real MMU");
            // Aux write-through: the aux-window write reaches the bus, and
            // the bank-steer flush keeps it ahead of the PAGE2 change.
            check(idx_aux > 0 && idx_c054 > 0 && idx_main > 0,
                  "aux/main posted writes and PAGE2 switch all on the bus");
            check(idx_aux < idx_c054 && idx_c054 < idx_main,
                  $sformatf("bank-steer flush ordering (aux=%0d c054=%0d main=%0d)",
                            idx_aux, idx_c054, idx_main));
        end

        // Sync read result landed in shadow ZP.
        sh_read(18'h00010, rd);
        check(rd == 8'hA7, $sformatf("sync $C000 read -> ZP $10 (got %h)", rd));

        // Disk II cold-boot selection: the first accelerated slot-7 probe
        // is consumed as an empty slot, the slot-6 probe releases the
        // one-shot, and later slot-7 traffic reaches the bus normally.
        sh_read(18'h0001A, rd);
        check(rd == 8'hFF, $sformatf("hidden cold-scan $C700 -> $FF (got %h)", rd));
        sh_read(18'h0001B, rd);
        check(rd == 8'hEE, $sformatf("Disk II $C600 probe reached bus (got %h)", rd));
        check(!sp_boot_suppress, "slot 7 restored after the slot-6 probe");
        check(c6_bus_reads == 1 && c7_bus_reads == 2,
              $sformatf("cold scan hides only first slot-7 probe (C6=%0d C7=%0d)",
                        c6_bus_reads, c7_bus_reads));

        // Banking discriminator: private INTCXROM / INTC8ROM / LC tracking
        // must route exactly like a stock //e MMU. Bus-served reads return
        // the model's $EE; shadow-served reads return the preloaded marks.
        sh_read(18'h00012, rd);
        check(rd == 8'hEE, $sformatf("INTCXROM off: $C701 from the bus (got %h)", rd));
        sh_read(18'h00013, rd);
        check(rd == 8'h88, $sformatf("INTCXROM on: $C701 from shadow internal ROM (got %h)", rd));
        sh_read(18'h00014, rd);
        check(rd == 8'hEE, $sformatf("INTCXROM off again: $C701 from the bus (got %h)", rd));
        sh_read(18'h00015, rd);
        check(rd == 8'h33, $sformatf("$C310 from shadow slot-3 internal ROM (got %h)", rd));
        sh_read(18'h00016, rd);
        check(rd == 8'hC8, $sformatf("INTC8ROM claim: $C800 from shadow (got %h)", rd));
        sh_read(18'h00017, rd);
        check(rd == 8'hEE, $sformatf("$CFFF release: $C800 from the bus (got %h)", rd));
        sh_read(18'h00018, rd);
        check(rd == 8'h42, $sformatf("LC RAM read: $D022 from shadow RAM (got %h)", rd));
        sh_read(18'h00019, rd);
        check(rd == 8'h99, $sformatf("LC ROM read: $D022 from shadow ROM copy (got %h)", rd));

        // Aux write landed in the vTW's own aux shadow too.
        sh_read(18'h10427, rd);
        check(rd == 8'h77, $sformatf("aux write in shadow aux bank (got %h)", rd));

        /* RamWorks banks from PSRAM through the line cache: bank isolation
         * ($1800 differs across banks 2 and 5), cache-hit read-back,
         * write-allocate fills, and the flush chains that pushed every
         * dirty line out to the model. */
        sh_read(18'h00020, rd);
        check(rd == 8'hA5, $sformatf("RamWorks bank 2 $1800 (got %h)", rd));
        sh_read(18'h00021, rd);
        check(rd == 8'h77, $sformatf("RamWorks cache-hit $1803 (got %h)", rd));
        sh_read(18'h00022, rd);
        check(rd == 8'h5A, $sformatf("RamWorks line-cross $1808 (got %h)", rd));
        sh_read(18'h00023, rd);
        check(rd == 8'h3C, $sformatf("RamWorks bank 5 $1800 (got %h)", rd));
        check(psram_model.exists(24'h031800) && psram_model[24'h031800] == 8'hA5,
              "bank 2 $1800 flushed to PSRAM");
        check(psram_model.exists(24'h031803) && psram_model[24'h031803] == 8'h77,
              "bank 2 $1803 flushed to PSRAM");
        check(psram_model.exists(24'h031808) && psram_model[24'h031808] == 8'h5A,
              "bank 2 $1808 flushed to PSRAM");
        check(psram_model.exists(24'h061800) && psram_model[24'h061800] == 8'h3C,
              "bank 5 $1800 flushed to PSRAM");

        /* Floating-bus substitution: FLOATBUS's six sanity addresses plus
         * its $C05A ZIPLOCK capture address must return main shadow $37F8
         * ($D7), selected two cycles behind the raw scanner coordinates and
         * from pre-access video state. Adjacent-cycle and text-page locations
         * contain different markers, proving address and BEAMPOS phase. */
        // Seven 4-cycle reads plus their stores and setup take roughly
        // 64 native Apple cycles; leave enough room for the whole sequence.
        #100us;
        sh_read(18'h00012, rd);
        check(rd == 8'hD7,
              $sformatf("floating $C030 read returns scanner $37F8 byte (got %h)",
                        rd));
        sh_read(18'h00024, rd);
        check(rd == 8'hD7,
              $sformatf("floating $C047 read returns scanner $37F8 byte (got %h)",
                        rd));
        sh_read(18'h00025, rd);
        check(rd == 8'hD7,
              $sformatf("floating $C04F read returns scanner $37F8 byte (got %h)",
                        rd));
        sh_read(18'h00026, rd);
        check(rd == 8'hD7,
              $sformatf("floating $C050 read returns scanner $37F8 byte (got %h)",
                        rd));
        sh_read(18'h00027, rd);
        check(rd == 8'hD7,
              $sformatf("floating $C057 read returns scanner $37F8 byte (got %h)",
                        rd));
        sh_read(18'h00028, rd);
        check(rd == 8'hD7,
              $sformatf("floating $C058 read returns scanner $37F8 byte (got %h)",
                        rd));
        sh_read(18'h00029, rd);
        check(rd == 8'hD7,
              $sformatf("floating $C05A read returns scanner $37F8 byte (got %h)",
                        rd));
        check(c071_capture_checks == 1 && c071_apply_checks == 1 &&
              c071_next_checks == 1,
              "C071 changes at X_ROUTE and reaches the next access");
        check(c073_capture_checks == 4 && c073_apply_checks == 4 &&
              c073_next_checks == 4,
              "C073 changes at X_ROUTE and reaches each next access");
        check(!bank_apply_pending && !bank_next_pending,
              "RamWorks switch phase monitor is idle after the program");
        check(route_tuple_checks > 100 &&
              route_bus_checks > 0 && route_rom_checks > 0 &&
              route_main_checks > 0 && route_aux_checks > 0 &&
              route_ramworks_checks > 0,
              "captured route tuple covers bus, ROM, main, aux, and RamWorks");

        // Engine health.
        check(cnt_post_drops == 32'd0, "no posted-queue drops");
        check(cnt_invalid_routes == 32'd0, "no invalid routes");
        check(post_high_water >= 10'd400, "queue backpressure exercised");
        check(c074_state == 2'd1, "$C074 state mirrors the write");

        // 1 MHz lock: exactly one core cycle per Apple cycle (+/- jitter).
        wait (post_fill == '0);

        /* CPU0 SmartPort fast path: inject video copies through the same
         * ordered queue while the core runs. The handshake must preserve
         * every address and byte and must not count a drop. */
        arm_rec_base = wrecs.size();
        arm_post_before = int'(cnt_posted_writes);
        arm_post_burst(16'h6000, 8'hD0, 8);
        fork : arm_post_wait
            begin
                wait (cnt_posted_writes == arm_post_before + 32'd8);
                disable arm_post_wait;
            end
            begin
                #100us;
                check(0, $sformatf(
                    "CPU0 post timeout: before=%0d now=%0d fill=%0d ready=%b",
                    arm_post_before, cnt_posted_writes, post_fill,
                    arm_post_ready));
                disable arm_post_wait;
            end
        join
        check(wrecs.size() >= arm_rec_base + 8,
              "CPU0 injected all eight physical writes");
        if (wrecs.size() >= arm_rec_base + 8) begin
            for (int i = 0; i < 8; i++) begin
                check(wrecs[arm_rec_base + i].addr == 16'h6000 + 16'(i) &&
                      wrecs[arm_rec_base + i].data == 8'hD0 + 8'(i),
                      $sformatf("CPU0 post %0d kept address/data", i));
            end
        end
        check(cnt_post_drops == 32'd0, "CPU0 post injection has no drops");

        /* An armed main-RAM text buffer must become a vTW write-through
         * window even though $6000 is ordinary program RAM. The following
         * DEVSEL command must wait behind that posted byte. */
        begin
            int overlay_rec_base = wrecs.size();
            overlay_capture_armed = 1'b1;
            sh_write(18'h00500, 8'hA9); // LDA #$5A
            sh_write(18'h00501, 8'h5A);
            sh_write(18'h00502, 8'h8D); // STA $6000
            sh_write(18'h00503, 8'h00);
            sh_write(18'h00504, 8'h60);
            sh_write(18'h00505, 8'hA9); // LDA #$02
            sh_write(18'h00506, 8'h02);
            sh_write(18'h00507, 8'h8D); // STA $C0F3 (SHOW)
            sh_write(18'h00508, 8'hF3);
            sh_write(18'h00509, 8'hC0);
            sh_write(18'h0050A, 8'h4C); // wait at $050A
            sh_write(18'h0050B, 8'h0A);
            sh_write(18'h0050C, 8'h05);
            // The live loop is LDA $C000 at $0358. Its low byte is already 0.
            sh_write(18'h0035A, 8'h05);
            sh_write(18'h00358, 8'h4C); // JMP $0500

            /* Hold one ARM post valid on the exact core-post edge. The core
             * tuple must win this capture; the held ARM tuple follows on the
             * next accepted edge. Both must drain before the DEVSEL command. */
            do @(negedge clk); while (!dut.core_post_accept);
            arm_post_addr  = 16'h6FFD;
            arm_post_wdata = 8'hA6;
            arm_post_we    = 1'b1;
            check(!arm_post_ready,
                  "core posted write has priority over a simultaneous ARM post");
            @(posedge clk);
            #1ps;
            check(dut.post_stage_valid_q &&
                  dut.post_stage_addr_q == 16'h6000 &&
                  dut.post_stage_wdata_q == 8'h5A,
                  "core tuple occupies the posted boundary after collision");
            do @(negedge clk); while (!arm_post_ready);
            @(posedge clk);
            #1ps;
            check(dut.post_stage_valid_q &&
                  dut.post_stage_addr_q == 16'h6FFD &&
                  dut.post_stage_wdata_q == 8'hA6,
                  "held ARM tuple follows the core tuple without mixing");
            @(negedge clk);
            arm_post_we = 1'b0;

            fork : overlay_post_wait
                begin
                    wait (wrecs.size() >= overlay_rec_base + 3);
                    disable overlay_post_wait;
                end
                begin
                    #200us;
                    check(0, $sformatf(
                        "vTW overlay timeout pc=%h state=%0d cycle=%h fill=%0d records=%0d",
                        dbg_core_pc, dut.xstate_q, dut.cycle_addr_q,
                        post_fill, wrecs.size()));
                    disable overlay_post_wait;
                end
            join
            check(wrecs.size() >= overlay_rec_base + 3,
                  "vTW emitted both queued RAM writes and DEVSEL command");
            if (wrecs.size() >= overlay_rec_base + 3) begin
                check(wrecs[overlay_rec_base].addr == 16'h6000 &&
                      wrecs[overlay_rec_base].data == 8'h5A,
                      "vTW posts armed main-RAM overlay byte");
                check(wrecs[overlay_rec_base + 1].addr == 16'h6FFD &&
                      wrecs[overlay_rec_base + 1].data == 8'hA6,
                      "ARM post follows the simultaneous core post");
                check(wrecs[overlay_rec_base + 2].addr == 16'hC0F3 &&
                      wrecs[overlay_rec_base + 2].data == 8'h02,
                      "vTW orders SHOW after overlay bytes");
            end
            // Restore the live loop, then let the wait loop return to it.
            sh_write(18'h0035A, 8'hC0);
            sh_write(18'h00358, 8'hAD);
            sh_write(18'h0050B, 8'h5A);
            sh_write(18'h0050C, 8'h03);
            overlay_capture_armed = 1'b0;
            repeat (20) @(posedge clk);
        end

        /* Hold the core on one internal SmartPort access, seed a dirty
         * RamWorks cache line, and issue the same flush request CPU0 uses
         * before a 512-byte PS-DMA copy. The dirty line must reach PSRAM and
         * the cache must be invalid before the completion pulse. */
        sp_active = 1'b1;
        sh_write(18'h0035A, 8'hC7);  // loop LDA $C000 -> LDA $C700
        fork : sp_wait_enter
            begin
                wait (sp_req_valid);
                check(sp_req_target == 3'd0 && sp_req_rw,
                      "SmartPort test access reaches slot-ROM read target");
                /* Ownership may change once X_ROUTE has accepted the
                 * access. The issued request must keep its captured target
                 * until the next handshake edge. */
                sp_active = 1'b0;
                #1ps;
                check(sp_req_valid && sp_req_target == 3'd0,
                      "SmartPort issue keeps its captured target");
                sp_active = 1'b1;
                @(posedge clk);
                disable sp_wait_enter;
            end
            begin
                #100us;
                check(0, "SmartPort wait-state entry timeout");
                disable sp_wait_enter;
            end
        join
        @(negedge clk);
        dut.rwc_valid_q = 1'b1;
        dut.rwc_dirty_q = 1'b1;
        dut.rwc_line_q  = 21'h04300;  // PSRAM $021800
        dut.rwc_data_q  = 64'h8877_6655_4433_2211;
        arm_rw_flush_req <= 1'b1;
        @(posedge clk);
        arm_rw_flush_req <= 1'b0;
        fork : rw_flush_wait
            begin
                wait (arm_rw_flush_done);
                disable rw_flush_wait;
            end
            begin
                #100us;
                check(0, "RamWorks cache flush timeout");
                disable rw_flush_wait;
            end
        join
        check(psram_model.exists(24'h021800) &&
              psram_model[24'h021800] == 8'h11 &&
              psram_model[24'h021807] == 8'h88,
              "CPU0 flush writes the complete dirty RamWorks line");
        check(!dut.rwc_valid_q && !dut.rwc_dirty_q,
              "CPU0 flush invalidates the RamWorks cache");
        check(arm_rw_hold_state,
              "flush completion leaves the core frozen for the DMA window");
        /* Un-park the fast-port access: the pending SmartPort response may
         * arrive, but the frozen core must not complete a single cycle
         * until CPU0 releases the hold (this is the DMA window). */
        sh_write(18'h0035A, 8'hC0);
        sp_resp_rdata = 8'h00;
        sp_resp_valid = 1'b1;
        @(posedge clk);
        sp_resp_valid = 1'b0;
        sp_active = 1'b0;
        cyc_a = int'(cnt_core_cycles);
        #20us;
        cyc_b = int'(cnt_core_cycles);
        check(cyc_b == cyc_a,
              $sformatf("held core completes no cycles (%0d -> %0d)",
                        cyc_a, cyc_b));
        arm_rw_hold_release <= 1'b1;
        @(posedge clk);
        arm_rw_hold_release <= 1'b0;
        #20us;
        cyc_b = int'(cnt_core_cycles);
        check(cyc_b > cyc_a, "released core resumes execution");

        /* A timeout may release a request before it becomes safe to touch
         * the cache. It must cancel the pending command, pulse DONE so the
         * 0x9C BUSY latch closes, and never fire later. */
        @(negedge clk);
        dut.rwc_valid_q = 1'b1;
        dut.rwc_dirty_q = 1'b1;
        dut.rwc_line_q  = 21'h04310;
        dut.rwc_data_q  = 64'hA8A7_A6A5_A4A3_A2A1;
        force dut.rw_flush_unsafe = 1'b1;
        arm_rw_flush_req <= 1'b1;
        @(posedge clk);
        arm_rw_flush_req <= 1'b0;
        repeat (4) @(posedge clk);
        check(arm_rw_hold_state && dut.rw_flush_pending_q,
              "unsafe flush stays pending with the core held");
        /* Make the cache safe on the release edge itself. Old pending state
         * must not win NBA priority and launch this dirty writeback. */
        @(negedge clk);
        force dut.rw_flush_unsafe = 1'b0;
        arm_rw_hold_release = 1'b1;
        @(posedge clk);
        @(negedge clk);
        arm_rw_hold_release = 1'b0;
        @(posedge clk);
        check(arm_rw_flush_done && !arm_rw_hold_state &&
              !dut.rw_flush_pending_q && !dut.rw_flush_active_q &&
              !dut.rw_req_valid_q,
              $sformatf("release cancels pending: done=%b held=%b pend=%b active=%b req=%b",
                        arm_rw_flush_done, arm_rw_hold_state,
                        dut.rw_flush_pending_q, dut.rw_flush_active_q,
                        dut.rw_req_valid_q));
        release dut.rw_flush_unsafe;
        repeat (8) @(posedge clk);
        check(!dut.rw_flush_active_q && !arm_rw_hold_state,
              $sformatf("cancelled flush stays dead: held=%b active=%b req=%b inflight=%b",
                        arm_rw_hold_state, dut.rw_flush_active_q,
                        dut.rw_req_valid_q, dut.rw_inflight_q));

        /* Once a dirty-line write reaches PSRAM it cannot be cancelled.
         * An early RELEASE must stay deferred until the response returns. */
        @(negedge clk);
        dut.rwc_valid_q = 1'b1;
        dut.rwc_dirty_q = 1'b1;
        dut.rwc_line_q  = 21'h04320;
        dut.rwc_data_q  = 64'hFEDC_BA98_7654_3210;
        rwm_latency_override = 200;
        arm_rw_flush_req <= 1'b1;
        @(posedge clk);
        arm_rw_flush_req <= 1'b0;
        fork : rw_active_wait
            begin
                wait (dut.rw_flush_active_q && dut.rw_inflight_q);
                disable rw_active_wait;
            end
            begin
                #100us;
                check(0, "active flush acceptance timeout");
                disable rw_active_wait;
            end
        join
        arm_rw_hold_release <= 1'b1;
        @(posedge clk);
        arm_rw_hold_release <= 1'b0;
        repeat (20) @(posedge clk);
        check(arm_rw_hold_state && dut.rw_release_pending_q,
              "release stays deferred while dirty writeback is active");
        fork : rw_deferred_wait
            begin
                wait (arm_rw_flush_done);
                disable rw_deferred_wait;
            end
            begin
                #100us;
                check(0, "deferred release completion timeout");
                disable rw_deferred_wait;
            end
        join
        check(!arm_rw_hold_state && !dut.rw_release_pending_q &&
              psram_model.exists(24'h021900) &&
              psram_model[24'h021900] == 8'h10 &&
              psram_model[24'h021907] == 8'hFE,
              $sformatf("active release drains: held=%b relpend=%b exists=%b first=%02x last=%02x",
                        arm_rw_hold_state, dut.rw_release_pending_q,
                        psram_model.exists(24'h021900),
                        psram_model.exists(24'h021900) ? psram_model[24'h021900] : 8'h00,
                        psram_model.exists(24'h021907) ? psram_model[24'h021907] : 8'h00));
        rwm_latency_override = -1;

        repeat (5) @(negedge phi0);
        cyc_a = int'(cnt_core_cycles);
        repeat (100) @(negedge phi0);
        cyc_b = int'(cnt_core_cycles);
        check(cyc_b - cyc_a >= 97 && cyc_b - cyc_a <= 103,
              $sformatf("1 MHz lock: %0d core cycles per 100 Apple cycles",
                        cyc_b - cyc_a));

        /* Cycle-exactness through I/O: the running 14-cycle loop does one
         * $C000 sync read per iteration. Under the lock, consecutive reads
         * must be EXACTLY 14 Apple cycles apart, every iteration -- any
         * fixed excess or jitter here is raster-program drift on hardware. */
        collect_c000 = 1;
        repeat (60 * 14) @(negedge phi0);
        collect_c000 = 0;
        check(c000_stamps.size() >= 40, "collected loop sync-read stamps");
        begin
            int delta0 = 0;
            bit uniform = 1;
            for (int i = 1; i < c000_stamps.size(); i++) begin
                int d = c000_stamps[i] - c000_stamps[i-1];
                if (i == 1) delta0 = d;
                else if (d != delta0) uniform = 0;
            end
            check(uniform, "locked-mode loop spacing is uniform");
            check(delta0 == 14,
                  $sformatf("locked-mode I/O loop cycle-exact: %0d per iteration (stock 14)",
                            delta0));
        end

        /* Enabling the override live must clear a value latched before the
         * setting changed. Keep the rest of this bench at 1 MHz through the
         * configured mode after dropping the override again. */
        ignore_c074 = 1'b1;
        repeat (4) @(posedge clk);
        check(c074_state == 2'd0,
              "$C074 override clears an already-latched 1 MHz state");
        speed_mode = 2'd2;
        ignore_c074 = 1'b0;
        repeat (4) @(posedge clk);

        /* A hold that CPU0 never releases must clear on Apple RES#. The
         * cache is clean here, so the flush completes immediately and
         * only the frozen-core state carries into the reset. */
        arm_rw_flush_req <= 1'b1;
        @(posedge clk);
        arm_rw_flush_req <= 1'b0;
        fork : rw_hold_rearm
            begin
                wait (arm_rw_flush_done);
                disable rw_hold_rearm;
            end
            begin
                #100us;
                check(0, "auto-release re-arm flush timeout");
                disable rw_hold_rearm;
            end
        join
        check(arm_rw_hold_state, "core re-frozen ahead of the machine reset");

        /* Takeover machine reset: the vTW pulls RES# open-collector, and
         * the engine must respond to its OWN reset exactly as it does to a
         * keyboard reset -- full bus release (stock reset window for the
         * MMU/IOU), then automatic re-take at release. */
        tb_assert_res = 1;
        #8us;
        check(apple_res_pin === 1'b0, "vTW asserts Apple RES#");
        check(apple_dma_pin === 1'b1, "/DMA released during the machine reset");
        check(!arm_rw_hold_state, "Apple RES# auto-releases the core hold");
        /* A CPU0 request that arrives after RES# fell must also close. This
         * covers the request/reset race that would otherwise leave 0x9C
         * BUSY set even though the hold auto-released. */
        arm_rw_flush_req <= 1'b1;
        @(posedge clk);
        arm_rw_flush_req <= 1'b0;
        @(posedge clk);
        check(arm_rw_flush_done && !arm_rw_hold_state &&
              !dut.rw_flush_pending_q && !dut.rw_flush_active_q,
              "flush request during Apple RES# completes without a hold");
        tb_assert_res = 0;
        /* Filter release + the 80-cycle motherboard stock run, then the
         * automatic re-take. */
        #120us;
        check(apple_res_pin === 1'b1, "vTW releases Apple RES#");
        check(apple_dma_pin === 1'b0, "bus re-taken after the reset");

        /* Last, restart with the program's immediate value changed from
         * $C074=$01 to $03. The override must keep the new value from
         * changing the latch while the physical write remains visible. No
         * later timing check depends on this deliberate restart. */
        core_run = 1'b0;
        wait (post_fill == '0);
        repeat (4) @(posedge clk);
        ignore_c074 = 1'b1;
        speed_mode = 2'd0;
        repeat (4) @(posedge clk);
        check(c074_state == 2'd0, "$C074 state clear before ignored write");
        sh_write(ROM_BASE + 18'h03025, 8'h03);
        c074_override_base = c074_bus_writes;
        core_run = 1'b1;
        fork : c074_override_wait
            begin
                wait (c074_bus_writes > c074_override_base);
                disable c074_override_wait;
            end
            begin
                #5ms;
                check(0, "$C074 override physical-write timeout");
                disable c074_override_wait;
            end
        join
        check(last_c074_bus_data == 8'h03,
              "$C074=$03 write still reaches the physical bus while ignored");
        check(c074_state == 2'd0,
              "$C074 override ignores off-until-reset value 3");

        /* Cancel one accepted-but-not-yet-emitted ARM tuple with hard reset.
         * It must neither reach the engine nor appear on the Apple bus. */
        begin
            int reset_rec_base;
            int reset_accept_base;
            int reset_emit_base;
            do @(negedge clk); while (!arm_post_ready ||
                                      dut.post_stage_valid_q);
            reset_accept_base = post_accept_checks;
            reset_emit_base = post_emit_checks;
            arm_post_addr  = 16'h6FFE;
            arm_post_wdata = 8'hE7;
            arm_post_we    = 1'b1;
            @(posedge clk);
            #1ps;
            check(dut.post_stage_valid_q &&
                  dut.post_stage_addr_q == 16'h6FFE &&
                  dut.post_stage_wdata_q == 8'hE7,
                  "reset test captures one pending ARM post");
            reset_rec_base = wrecs.size();
            rstn = 1'b0;
            arm_post_we = 1'b0;
            repeat (4) @(posedge clk);
            #1ps;
            check(!dut.post_stage_valid_q && !dut.eng_post_we,
                  "hard reset clears the pending posted tuple");
            for (int i = reset_rec_base; i < wrecs.size(); i++) begin
                check(wrecs[i].addr != 16'h6FFE,
                      "reset-cancelled posted tuple never reaches the bus");
            end
            check(post_accept_checks == reset_accept_base + 1 &&
                  post_emit_checks == reset_emit_base,
                  "reset cancels exactly one accepted tuple before emission");
        end

        if (fails == 0) $display("VTW SYSTEM PASS");
        else            $display("VTW SYSTEM FAILED: %0d checks", fails);
        $finish;
    end

    initial begin
        #20ms;
        $display("VTW SYSTEM FAIL: global timeout");
        $finish;
    end

endmodule
