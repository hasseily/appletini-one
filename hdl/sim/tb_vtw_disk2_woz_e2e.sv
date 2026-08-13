`timescale 1ns / 1ps

// End-to-end vTW/raw-WOZ Disk II regression.
//
// The W65C02 executes a real motor-on read and repeated LDA $C0EC accesses.
// Requests pass through vtw_core_top's private Disk II port into the real
// disk2_card. Track bytes arrive through the card's DDR line interface. The
// physical motor-on cycle uses the production wrapper and 12-client arbiter.
module tb_vtw_disk2_woz_e2e;
    timeunit 1ns;
    timeprecision 1ps;

    localparam logic [17:0] ROM_BASE = 18'h20000;
    localparam logic [20:0] TRACK_BASE_LINE = 21'h0E0000;

    logic clk = 1'b0;
    always #3.75ns clk = ~clk;
    logic phi0 = 1'b0;
    always #490ns phi0 = ~phi0;
    logic rstn = 1'b0;

    wire [7:0]  apple_data_pin;
    wire [15:0] apple_addr_pin;
    wire        apple_rw_pin;
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
    logic res_drive_low = 1'b1;
    assign apple_res_pin = res_drive_low ? 1'b0 : 1'bz;

    globals::AppleBus_read ab_read;
    globals::AppleBus_write vtw_write;
    globals::AppleBus_write disk_write;
    globals::AppleBus_write [11:0] client_writes;
    globals::AppleBus_write ab_write;
    logic tini_oe_pin, tini_addr_dir_pin, tini_data_dir_pin;

    apple_bus_wrapper wrapper_i (
        .clk(clk), .rstn(rstn),
        .res_filtered_out(), .dbg_lost_cycle_count(), .dbg_clear(1'b0),
        .dbg_bus_quality(), .dbg_tap_mismatch(), .dbg_strobe_anom(),
        .dbg_tap_last(), .dbg_ghost_write(),
        .inh_allowed(1'b1), .gs_m2_qualify(1'b0),
        .m2sel_active_high(1'b0), .host_is_iiplus(1'b0),
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

    always_comb begin
        client_writes = '{default: '0};
        client_writes[11] = vtw_write;
        client_writes[3] = disk_write;
    end
    apple_bus_write_arbiter #(.NUM_CLIENTS(12), .FAST_DATA_CLIENT(2)) arbiter_i (
        .inh_allowed(1'b1), .client_writes(client_writes),
        .ab_write(ab_write)
    );

    logic [7:0] mb_rdata;
    logic [7:0] mb_ram [0:16'hFFFF];
    always_comb begin
        if (apple_addr_pin[15:12] == 4'hC)
            mb_rdata = 8'hEE;
        else
            mb_rdata = mb_ram[apple_addr_pin];
    end
    wire mb_drive_data = phi0 && (apple_rw_pin === 1'b1) &&
                         !tini_data_dir_pin;
    assign apple_data_pin = mb_drive_data ? mb_rdata : 8'hzz;

    globals::SoftSwitchState sss;
    globals::AxiSimple_common as_common;
    AxiSimple_if axi();
    logic [20:0] mc_line_addr;
    logic mc_rw;
    logic [63:0] mc_wdata;
    logic [7:0] mc_wstrb;
    logic mc_valid;
    logic mc_ready;
    logic [63:0] mc_rdata = 64'h0;
    logic mc_rvalid = 1'b0;
    logic mc_pending_q = 1'b0;
    logic [20:0] mc_pending_addr_q = '0;
    logic allow_mc_response = 1'b1;
    integer mc_read_count = 0;
    assign mc_ready = !mc_pending_q;

    always_ff @(posedge clk) begin
        mc_rvalid <= 1'b0;
        if (!rstn) begin
            mc_pending_q <= 1'b0;
            mc_pending_addr_q <= '0;
            mc_read_count <= 0;
        end else begin
            if (mc_valid && mc_ready && mc_rw) begin
                mc_pending_q <= 1'b1;
                mc_pending_addr_q <= mc_line_addr;
                mc_read_count <= mc_read_count + 1;
            end
            if (mc_valid && mc_ready && !mc_rw)
                $fatal(1, "FAIL: read-only WOZ test issued a DDR write");
            if (mc_pending_q && allow_mc_response) begin
                mc_rvalid <= 1'b1;
                if (mc_pending_addr_q == TRACK_BASE_LINE ||
                    mc_pending_addr_q == TRACK_BASE_LINE + 21'd1)
                    mc_rdata <= 64'hA5A5_A5A5_A5A5_A5A5;
                else
                    mc_rdata <= 64'h0;
                mc_pending_q <= 1'b0;
            end
        end
    end

    logic disk2_req_valid;
    logic [3:0] disk2_req_addr;
    logic disk2_req_ready;
    logic disk2_resp_valid;
    logic [7:0] disk2_resp_rdata;
    logic disk2_cycle_tick;
    logic disk2_native_cycle_active;
    logic disk2_time_ready;
    logic disk2_write_timing_active;

    disk2_card card_i (
        .clk(clk), .rstn(rstn), .ab_read(ab_read), .sss(sss),
        .slot_assign(3'd6), .as_common(as_common), .as_client(axi),
        .mc_line_addr(mc_line_addr), .mc_rw(mc_rw),
        .mc_wdata(mc_wdata), .mc_wstrb(mc_wstrb),
        .mc_valid(mc_valid), .mc_ready(mc_ready),
        .mc_rdata(mc_rdata), .mc_rvalid(mc_rvalid),
        .ab_write(disk_write), .vtw_active(1'b1),
        .vtw_req_valid(disk2_req_valid), .vtw_req_addr(disk2_req_addr),
        .vtw_req_ready(disk2_req_ready),
        .vtw_resp_valid(disk2_resp_valid),
        .vtw_resp_rdata(disk2_resp_rdata),
        .vtw_cycle_tick(disk2_cycle_tick),
        .vtw_native_cycle_active(disk2_native_cycle_active),
        .vtw_time_ready(disk2_time_ready),
        .vtw_write_timing_active(disk2_write_timing_active),
        .sound_spinning(), .sound_qtrack(), .sound_event(),
        .sound_seek_start_qtrack(), .sound_seek_distance()
    );

    logic enable = 1'b0;
    logic core_run = 1'b0;
    logic [1:0] speed_mode = 2'd0;
    logic [15:0] pace_divider = 16'd0;
    logic sh_en = 1'b0, sh_we = 1'b0;
    logic [17:0] sh_addr = '0;
    logic [7:0] sh_wdata = '0;
    logic [7:0] sh_rdata;
    logic [31:0] cnt_core_cycles;
    logic [31:0] cnt_bus_cycles;

    vtw_core_top core_i (
        .clk(clk), .rstn(rstn), .enable(enable),
        .host_is_iiplus(1'b0), .core_run(core_run),
        .assert_apple_res(1'b0), .speed_mode(speed_mode),
        .pace_divider(pace_divider), .ignore_c074(1'b0),
        .irq_assert_in(1'b0), .data_drive_in(vtw_write.wr_data_en),
        .data_drive_value_in(vtw_write.wr_data), .dbg_clear(1'b0),
        .iiplus_buttons_zero(1'b0),
        .slow_region_en(10'd0), .slow_duration(16'd0),
        .d2_active(1'b1), .d2_req_valid(disk2_req_valid),
        .d2_req_addr(disk2_req_addr), .d2_req_ready(disk2_req_ready),
        .d2_resp_valid(disk2_resp_valid),
        .d2_resp_rdata(disk2_resp_rdata),
        .d2_cycle_tick(disk2_cycle_tick),
        .d2_native_cycle_active(disk2_native_cycle_active),
        .d2_time_ready(disk2_time_ready),
        .d2_write_timing_active(disk2_write_timing_active),
        .ramworks_en(1'b0), .video_vbl(1'b0),
        .post_main_wide(1'b0), .overlay_capture_armed(1'b0),
        .overlay_capture_bank_aux(1'b0), .overlay_capture_base(16'd0),
        .overlay_capture_limit(16'd0), .video_mode_50hz(1'b0),
        .video_line(9'd0), .video_cycle(7'd0),
        .ab_read(ab_read), .ab_write(vtw_write),
        .rw_req_valid(), .rw_req_rw(), .rw_req_addr(), .rw_req_wline(),
        .rw_req_ready(1'b1), .rw_resp_valid(1'b0), .rw_resp_rline(64'd0),
        .sp_active(1'b0), .sp_boot_suppress(1'b0),
        .sp_req_valid(), .sp_req_target(), .sp_req_addr(), .sp_req_rw(),
        .sp_req_wdata(), .sp_req_ready(1'b1), .sp_resp_valid(1'b0),
        .sp_resp_rdata(8'd0), .sp_sss_snapshot(),
        .sh_en(sh_en), .sh_addr(sh_addr), .sh_we(sh_we),
        .sh_wdata(sh_wdata), .sh_rdata(sh_rdata),
        .arm_req_valid(1'b0), .arm_req_addr('0), .arm_req_rw(1'b1),
        .arm_req_wdata('0), .arm_req_busy(), .arm_resp_valid(),
        .arm_resp_rdata(), .arm_post_we(1'b0), .arm_post_addr('0),
        .arm_post_wdata('0), .arm_post_ready(),
        .arm_rw_flush_req(1'b0), .arm_rw_hold_release(1'b0),
        .arm_rw_flush_done(), .arm_rw_hold_state(),
        .c074_state(), .bus_owned(), .video_phase_1mhz(),
        .dbg_core_pc(), .cnt_core_cycles(cnt_core_cycles),
        .cnt_bus_cycles(cnt_bus_cycles), .cnt_posted_writes(),
        .post_fill(), .post_high_water(), .cnt_post_drops(),
        .cnt_invalid_routes(), .dbg_vsss(), .dbg_last_sync_addr(),
        .dbg_last_sync_data(), .dbg_last_sync_rw(), .dbg_irq_edges(),
        .dbg_cxxx_ring(), .dbg_c0_ring(), .dbg_sync_write_check(),
        .dbg_sync_write_addr(), .dbg_c000_context(), .dbg_c000_counts(),
        .dbg_pc_trace(), .dbg_io_trace(), .dbg_trace_status(),
        .dbg_bus_faults()
    );

    integer private_req_count = 0;
    integer private_a5_count = 0;
    integer selected_tick_count = 0;
    integer cpu_a5_store_count = 0;
    integer physical_motor_access_count = 0;
    bit physical_motor_effect_seen = 1'b0;
    always_ff @(posedge clk) begin
        if (!rstn) begin
            private_req_count <= 0;
            private_a5_count <= 0;
            selected_tick_count <= 0;
            cpu_a5_store_count <= 0;
        end else begin
            if (disk2_req_valid && disk2_req_ready) begin
                private_req_count <= private_req_count + 1;
                if (disk2_req_addr != 4'hC)
                    $fatal(1, "FAIL: private Disk II address was %X", disk2_req_addr);
            end
            if (disk2_resp_valid && disk2_resp_rdata == 8'hA5)
                private_a5_count <= private_a5_count + 1;
            // Count only selected ticks for which the real raw-track
            // sequencer is live. Reset-vector and motor-off CPU cycles also
            // reach disk_cycle_tick, but cannot advance media state.
            if (card_i.woz_stream_active)
                selected_tick_count <= selected_tick_count + 1;
            if (core_i.ssm_apply_pulse && !core_i.cycle_rw_q &&
                core_i.cycle_addr_q == 16'h0200 &&
                core_i.cycle_wdata_q == 8'hA5)
                cpu_a5_store_count <= cpu_a5_store_count + 1;
        end
    end

    // Observe the exact strobe consumed by disk2_card, then sample the card
    // latch after that same edge's nonblocking updates have taken effect.
    always @(posedge clk) begin
        if (!rstn) begin
            physical_motor_access_count = 0;
            physical_motor_effect_seen = 1'b0;
        end else if ((card_i.ab_io_read || card_i.ab_io_write) &&
                     card_i.io_idx == 4'h9) begin
            physical_motor_access_count++;
            check(card_i.ab_io_read,
                  "CPU motor-on instruction did not arrive as a physical read");
            #1ps;
            check(card_i.motor_on_q,
                  "physical C0E9 read did not set the real Disk II motor latch");
            physical_motor_effect_seen = 1'b1;
        end
    end

    task automatic check(input bit condition, input string message);
        if (!condition)
            $fatal(1, "FAIL: %s", message);
    endtask

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

    task automatic load_program;
        // LDA $C0E9; LDA $C0EC; STA $0200; JMP $F003.
        sh_write(ROM_BASE + 18'h3000, 8'hAD);
        sh_write(ROM_BASE + 18'h3001, 8'hE9);
        sh_write(ROM_BASE + 18'h3002, 8'hC0);
        sh_write(ROM_BASE + 18'h3003, 8'hAD);
        sh_write(ROM_BASE + 18'h3004, 8'hEC);
        sh_write(ROM_BASE + 18'h3005, 8'hC0);
        sh_write(ROM_BASE + 18'h3006, 8'h8D);
        sh_write(ROM_BASE + 18'h3007, 8'h00);
        sh_write(ROM_BASE + 18'h3008, 8'h02);
        sh_write(ROM_BASE + 18'h3009, 8'h4C);
        sh_write(ROM_BASE + 18'h300A, 8'h03);
        sh_write(ROM_BASE + 18'h300B, 8'hF0);
        sh_write(ROM_BASE + 18'h3FFC, 8'h00);
        sh_write(ROM_BASE + 18'h3FFD, 8'hF0);
        sh_write(18'h00200, 8'h00);
    endtask

    task automatic configure_track;
        axi_write(8'h10, 32'h0000_0001);
        axi_write(8'h07, 32'd16);
        axi_write(8'h0E, 32'd128);
        axi_write(8'h0F, 32'd0);
        axi_write(8'h15, 32'd32);
        axi_write(8'h20, 32'h0000_0000);
        axi_write(8'h06, 32'h0000_0005);
    endtask

    task automatic run_preset(input logic [1:0] mode,
                              input integer divider,
                              input string label,
                              input bit check_cache_stall);
        logic [7:0] stored;
        integer timeout;
        integer core_before;
        integer ticks_before;
        integer cells_before;
        integer accum_before;
        integer tick_delta;
        integer cell_delta;
        integer expected_cells;
        integer expected_accum;
        integer req_before;
        integer a5_before;
        integer store_before;

        if (check_cache_stall) begin
            @(negedge clk);
            core_run = 1'b0;
            enable = 1'b0;
            rstn = 1'b0;
            res_drive_low = 1'b1;
            allow_mc_response = 1'b0;
            speed_mode = mode;
            pace_divider = divider[15:0];
            repeat (20) @(posedge clk);
            @(negedge clk);
            rstn = 1'b1;
            load_program();
            sh_read(18'h00200, stored);
            check(stored == 8'h00,
                  $sformatf("%s result byte did not clear before boot", label));
            configure_track();
            @(negedge clk);
            enable = 1'b1;
            #10us;
            @(negedge clk);
            res_drive_low = 1'b0;
            #4us;
            @(negedge clk);
            core_run = 1'b1;

            timeout = 0;
            while ((disk2_time_ready || !mc_pending_q) && timeout < 200000) begin
                @(posedge clk);
                timeout++;
            end
            check(!disk2_time_ready && mc_pending_q,
                  "raw-track miss did not hold vTW time");
            check(core_run && enable,
                  "cache-stall check did not occur in an active session");
            core_before = int'(cnt_core_cycles);
            repeat (64) @(posedge clk);
            check(int'(cnt_core_cycles) == core_before,
                  "core advanced while the WOZ cache held time");
            allow_mc_response = 1'b1;
        end else begin
            @(negedge clk);
            speed_mode = mode;
            pace_divider = divider[15:0];
            repeat (8) @(posedge clk);
        end

        timeout = 0;
        while (!(card_i.active_drive_loaded && card_i.stream_line_hit_q) &&
               timeout < 200000) begin
            @(posedge clk);
            timeout++;
        end
        check(card_i.active_drive_loaded && card_i.stream_line_hit_q,
              "raw WOZ track did not enter the real card cache");
        if (check_cache_stall)
            check(physical_motor_access_count > 0 &&
                  physical_motor_effect_seen,
                  "CPU motor-on access did not reach the card through the physical bus");
        check(card_i.track_woz_q && card_i.track_bit_timing_q == 8'd32,
              "real card did not retain raw WOZ timing");
        @(negedge clk);
        ticks_before = selected_tick_count;
        cells_before = int'(card_i.stream_read_count_q);
        accum_before = int'(card_i.woz_bit_accum_q);
        req_before = private_req_count;
        a5_before = private_a5_count;
        store_before = cpu_a5_store_count;

        timeout = 0;
        while (((cpu_a5_store_count == store_before) ||
                ((selected_tick_count - ticks_before) < 40)) &&
               timeout < 1000000) begin
            repeat (20) @(posedge clk);
            timeout += 20;
        end
        check(cpu_a5_store_count > store_before,
              $sformatf("%s CPU did not consume raw WOZ byte A5", label));
        check(private_req_count > req_before && private_a5_count > a5_before,
              $sformatf("%s did not complete a real private WOZ response", label));

        @(negedge clk);
        tick_delta = selected_tick_count - ticks_before;
        cell_delta = int'(card_i.stream_read_count_q) - cells_before;
        expected_cells = (accum_before + tick_delta * 8) / 32;
        expected_accum = (accum_before + tick_delta * 8) % 32;
        check(cell_delta == expected_cells,
              $sformatf("%s selected ticks produced %0d cells, expected %0d from %0d ticks",
                        label, cell_delta, expected_cells, tick_delta));
        check(int'(card_i.woz_bit_accum_q) == expected_accum,
              $sformatf("%s WOZ accumulator was %0d, expected %0d",
                        label, card_i.woz_bit_accum_q, expected_accum));
        check(card_i.drive_bit_offset_q[0] ==
              ((cells_before + cell_delta) % 128),
              $sformatf("%s raw bit offset did not match exact cell count", label));
        $display("VTW DISK2 WOZ E2E: %-16s A5, ticks=%0d cells=%0d offset=%0d",
                 label, tick_delta, cell_delta, card_i.drive_bit_offset_q[0]);
    endtask

    initial begin
        sss = '0;
        sss.slot_access = 1'b1;
        as_common = '0;
        axi.awvalid = 1'b0;
        for (int i = 0; i < 65536; i++)
            mb_ram[i] = 8'h00;

        run_preset(2'd1, 2667, "0.05 MHz slug", 1'b1);
        run_preset(2'd2, 0,    "1 MHz",         1'b0);
        run_preset(2'd1, 51,   "2.6 MHz",       1'b0);
        run_preset(2'd1, 37,   "3.6 MHz",       1'b0);
        run_preset(2'd1, 19,   "7 MHz",         1'b0);
        run_preset(2'd1, 10,   "13 MHz",        1'b0);
        run_preset(2'd1, 5,    "26 MHz",        1'b0);
        run_preset(2'd0, 0,    "MAX",           1'b0);
        $display("VTW DISK2 WOZ E2E PASS");
        $finish;
    end

    initial begin
        #40ms;
        $fatal(1, "FAIL: vTW Disk II WOZ end-to-end timeout");
    end
endmodule
