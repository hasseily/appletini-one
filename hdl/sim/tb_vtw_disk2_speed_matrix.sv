`timescale 1ns / 1ps
// Focused vTW Disk II speed and route matrix.
//
// This bench keeps the production RTL unchanged. It runs one live vTW
// session through all seven speed-ladder presets plus the 0.05 MHz UI slug
// override, proves the registered normal-time tick edge by edge, and then
// checks the Disk II route split at each speed: even reads use the private
// card port, while odd reads and all writes use the physical Apple bus. It
// also checks the Q7 write-mode interlock and both Disk II readiness gates.

module tb_vtw_disk2_speed_matrix;

    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;       // 133.333 MHz fabric clock

    logic phi0 = 1'b0;
    always #490 phi0 = ~phi0;      // about 1.02 MHz Apple clock

    logic rstn = 1'b0;

    // ---- Apple bus pins and motherboard pulls ----
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

    globals::AppleBus_read  ab_read;
    globals::AppleBus_write ab_write;
    globals::AppleBus_write vtw_ab_write;
    logic tini_oe_pin;
    logic tini_addr_dir_pin;
    logic tini_data_dir_pin;

    apple_bus_wrapper wrapper_i (
        .clk(clk), .rstn(rstn),
        .res_filtered_out(), .dbg_lost_cycle_count(), .dbg_clear(1'b0),
        .dbg_bus_quality(), .dbg_tap_mismatch(), .dbg_strobe_anom(),
        .dbg_tap_last(), .dbg_ghost_write(),
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
        .inh_allowed(1'b1),
        .client_writes({vtw_ab_write}),
        .ab_write(ab_write)
    );

    // Motherboard read model. Native Disk II reads get a stable floating
    // value; all other reads get RAM data.
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

    // ---- vTW controls and private Disk II response model ----
    logic        enable = 1'b0;
    logic        core_run = 1'b0;
    logic [1:0]  speed_mode = 2'd0;
    logic [15:0] pace_divider = 16'd0;
    logic        disk2_active = 1'b1;
    logic        disk2_req_valid;
    logic [3:0]  disk2_req_addr;
    logic        disk2_req_ready = 1'b1;
    logic        disk2_resp_valid = 1'b0;
    logic [7:0]  disk2_resp_rdata = 8'hA5;
    logic        disk2_cycle_tick;
    logic        disk2_native_cycle_active;
    logic        disk2_time_ready = 1'b1;
    logic        disk2_write_timing_active = 1'b0;

    logic        sh_en = 1'b0;
    logic [17:0] sh_addr = '0;
    logic        sh_we = 1'b0;
    logic [7:0]  sh_wdata = '0;
    logic [7:0]  sh_rdata;

    logic [31:0] cnt_core_cycles;
    logic [31:0] cnt_bus_cycles;
    logic        video_phase_1mhz;
    logic [15:0] dbg_last_sync_addr;
    logic [7:0]  dbg_last_sync_data;
    logic        dbg_last_sync_rw;

    logic        expected_native_check_en = 1'b0;
    logic [15:0] expected_native_addr = 16'hFFFF;
    logic        expected_native_rw = 1'b1;

    integer private_req_count = 0;
    always_ff @(posedge clk) begin
        if (!rstn) begin
            disk2_resp_valid <= 1'b0;
            private_req_count <= 0;
        end
        else begin
            disk2_resp_valid <= disk2_req_valid && disk2_req_ready;
            if (disk2_req_valid && disk2_req_ready) begin
                if (disk2_req_addr !== 4'hC)
                    $fatal(1,
                           "VTW DISK2 SPEED FAIL: private handshake used switch $%01X",
                           disk2_req_addr);
                private_req_count <= private_req_count + 1;
            end
        end
    end

    vtw_core_top dut (
        .clk(clk), .rstn(rstn), .enable(enable),
        .host_is_iiplus(1'b0), .core_run(core_run),
        .assert_apple_res(1'b0),
        .speed_mode(speed_mode), .pace_divider(pace_divider),
        .ignore_c074(1'b0),
        .irq_assert_in(1'b0),
        .data_drive_in(vtw_ab_write.wr_data_en),
        .data_drive_value_in(vtw_ab_write.wr_data),
        .dbg_clear(1'b0), .iiplus_buttons_zero(1'b0),
        .slow_region_en(10'd0), .slow_duration(16'd0),
        .d2_active(disk2_active),
        .d2_req_valid(disk2_req_valid), .d2_req_addr(disk2_req_addr),
        .d2_req_ready(disk2_req_ready),
        .d2_resp_valid(disk2_resp_valid),
        .d2_resp_rdata(disk2_resp_rdata),
        .d2_cycle_tick(disk2_cycle_tick),
        .d2_native_cycle_active(disk2_native_cycle_active),
        .d2_time_ready(disk2_time_ready),
        .d2_write_timing_active(disk2_write_timing_active),
        .ramworks_en(1'b0), .video_vbl(1'b0),
        .post_main_wide(1'b0),
        .overlay_capture_armed(1'b0),
        .overlay_capture_bank_aux(1'b0),
        .overlay_capture_base(16'd0),
        .overlay_capture_limit(16'd0),
        .video_mode_50hz(1'b0), .video_line(9'd0),
        .video_cycle(7'd0),
        .ab_read(ab_read), .ab_write(vtw_ab_write),
        .rw_req_valid(), .rw_req_rw(), .rw_req_addr(), .rw_req_wline(),
        .rw_req_ready(1'b1), .rw_resp_valid(1'b0),
        .rw_resp_rline(64'd0),
        .sp_active(1'b0), .sp_boot_suppress(1'b0),
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
        .dbg_vsss(), .dbg_last_sync_addr(dbg_last_sync_addr),
        .dbg_last_sync_data(dbg_last_sync_data),
        .dbg_last_sync_rw(dbg_last_sync_rw), .dbg_irq_edges(),
        .dbg_cxxx_ring(), .dbg_c0_ring(),
        .dbg_sync_write_check(), .dbg_sync_write_addr(),
        .dbg_c000_context(), .dbg_c000_counts(),
        .dbg_pc_trace(), .dbg_io_trace(), .dbg_trace_status(),
        .dbg_bus_faults()
    );

    // ---- Program loading ----
    localparam logic [17:0] ROM_BASE = 18'h20000;
    localparam int ROUTE_NOPS = 8;

    task automatic sh_write(input logic [17:0] a,
                            input logic [7:0] d);
        @(negedge clk);
        sh_en = 1'b1;
        sh_we = 1'b1;
        sh_addr = a;
        sh_wdata = d;
        @(negedge clk);
        sh_en = 1'b0;
        sh_we = 1'b0;
    endtask

    task automatic load_normal_program;
        int off;
        off = 0;
        for (int i = 0; i < 12; i++) begin
            sh_write(ROM_BASE + 18'h3000 + 18'(off), 8'hEA); // NOP
            off++;
        end
        sh_write(ROM_BASE + 18'h3000 + 18'(off), 8'h4C); off++;
        sh_write(ROM_BASE + 18'h3000 + 18'(off), 8'h00); off++;
        sh_write(ROM_BASE + 18'h3000 + 18'(off), 8'hF0); off++;
        sh_write(ROM_BASE + 18'h3FFC, 8'h00);
        sh_write(ROM_BASE + 18'h3FFD, 8'hF0);
        for (int i = 0; i < 512; i++)
            sh_write(18'(i), 8'h00);
    endtask

    task automatic load_route_program(input logic [7:0] opcode,
                                      input logic [15:0] addr);
        int off;
        sh_write(ROM_BASE + 18'h3000, opcode);
        sh_write(ROM_BASE + 18'h3001, addr[7:0]);
        sh_write(ROM_BASE + 18'h3002, addr[15:8]);
        off = 3;
        for (int i = 0; i < ROUTE_NOPS; i++) begin
            sh_write(ROM_BASE + 18'h3000 + 18'(off), 8'hEA);
            off++;
        end
        sh_write(ROM_BASE + 18'h3000 + 18'(off), 8'h4C); off++;
        sh_write(ROM_BASE + 18'h3000 + 18'(off), 8'h00); off++;
        sh_write(ROM_BASE + 18'h3000 + 18'(off), 8'hF0); off++;
        sh_write(ROM_BASE + 18'h3FFC, 8'h00);
        sh_write(ROM_BASE + 18'h3FFD, 8'hF0);
        for (int i = 0; i < 512; i++)
            sh_write(18'(i), 8'h00);
    endtask

    task automatic begin_reboot;
        @(negedge clk);
        core_run = 1'b0;
        enable = 1'b0;
        rstn = 1'b0;
        res_drive_low = 1'b1;
        disk2_time_ready = 1'b1;
        disk2_req_ready = 1'b1;
        disk2_write_timing_active = 1'b0;
        repeat (20) @(posedge clk);
        @(negedge clk);
        rstn = 1'b1;
    endtask

    task automatic finish_reboot;
        enable = 1'b1;
        #10us;
        @(negedge clk);
        res_drive_low = 1'b0;
        #4us;
        @(negedge clk);
        core_run = 1'b1;
    endtask

    task automatic reboot_normal;
        begin_reboot();
        load_normal_program();
        finish_reboot();
    endtask

    task automatic reboot_route(input logic [7:0] opcode,
                                input logic [15:0] addr);
        begin_reboot();
        expected_native_check_en = 1'b1;
        expected_native_addr = addr;
        expected_native_rw = (opcode == 8'hAD);
        load_route_program(opcode, addr);
        finish_reboot();
    endtask

    // ---- Edge-by-edge invariants ----
    integer fabric_cycle = 0;
    integer normal_accept_count = 0;
    integer normal_tick_count = 0;
    integer private_select_count = 0;
    integer native_select_count = 0;
    integer logical_select_count = 0;
    integer native_window_count = 0;
    logic   expected_normal_tick_q = 1'b0;
    logic   native_window_q = 1'b0;

    wire normal_tick_accept =
        dut.core_en && disk2_active &&
        !dut.private_d2_q && !dut.sd_disk2_native;

    task automatic check(input bit condition, input string message);
        if (!condition)
            $fatal(1, "VTW DISK2 SPEED FAIL: %s", message);
    endtask

    always @(posedge clk) begin
        fabric_cycle <= fabric_cycle + 1;
        if (!rstn) begin
            expected_normal_tick_q <= 1'b0;
            normal_accept_count <= 0;
            normal_tick_count <= 0;
            private_select_count <= 0;
            native_select_count <= 0;
            logical_select_count <= 0;
            native_window_count <= 0;
            native_window_q <= 1'b0;
        end
        else begin
            // The normal virtual-time pulse must be the exact prior edge's
            // accepted normal cycle at every speed and around live changes.
            if (disk2_cycle_tick !== expected_normal_tick_q) begin
                $fatal(1,
                       "VTW DISK2 SPEED FAIL: tick edge mismatch at fabric cycle %0d (got=%0b expected=%0b)",
                       fabric_cycle, disk2_cycle_tick,
                       expected_normal_tick_q);
            end
            expected_normal_tick_q <= normal_tick_accept;
            if (normal_tick_accept)
                normal_accept_count <= normal_accept_count + 1;
            if (disk2_cycle_tick)
                normal_tick_count <= normal_tick_count + 1;

            // Every completed logical cycle while Disk II virtual time is
            // active must choose exactly one source: staged normal time,
            // direct private-card time, or native Apple-bus time.
            if (dut.core_en && disk2_active) begin
                logical_select_count <= logical_select_count + 1;
                unique case ({dut.private_d2_q, dut.sd_disk2_native})
                    2'b00: begin
                        if (!normal_tick_accept) begin
                            $fatal(1,
                                   "VTW DISK2 SPEED FAIL: normal logical cycle lacked a tick selection");
                        end
                    end
                    2'b10: private_select_count <= private_select_count + 1;
                    2'b01: native_select_count <= native_select_count + 1;
                    default: begin
                        $fatal(1,
                               "VTW DISK2 SPEED FAIL: private and native tick sources selected together");
                    end
                endcase
            end

            native_window_q <= disk2_native_cycle_active;
            if (disk2_native_cycle_active && !native_window_q)
                native_window_count <= native_window_count + 1;
        end
    end

    // Once the bus engine owns a native Disk II cycle, its registered tuple
    // must match the CPU access exactly. The parked cycle is excluded by the
    // $C0Ex address check; completion is checked again through the debug
    // record in check_native_preset.
    always @(posedge clk) begin
        #1ps;
        if (rstn && expected_native_check_en &&
            disk2_native_cycle_active &&
            vtw_ab_write.wr_addr_rw_en &&
            vtw_ab_write.wr_addr[15:4] == 12'hC0E) begin
            check(vtw_ab_write.wr_addr === expected_native_addr,
                  $sformatf("native bus address was $%04X, expected $%04X",
                            vtw_ab_write.wr_addr, expected_native_addr));
            check(vtw_ab_write.wr_rw === expected_native_rw,
                  "native bus R/W field did not match the CPU operation");
            check(vtw_ab_write.wr_data_en === !expected_native_rw,
                  "native bus data-enable field did not match R/W");
        end
    end

    // ---- Speed measurements ----
    localparam int SPEED_SAMPLES = 16;

    task automatic wait_normal_accept;
        integer before_count;
        before_count = normal_accept_count;
        do begin
            @(posedge clk);
            #1ps;
        end while (normal_accept_count == before_count);
    endtask

    task automatic set_preset(input logic [1:0] mode,
                              input integer divider);
        @(negedge clk);
        speed_mode = mode;
        pace_divider = divider[15:0];
    endtask

    task automatic measure_normal_speed(input logic [1:0] mode,
                                        input integer divider,
                                        input string label,
                                        output integer average_gap);
        integer previous_cycle;
        integer gap;
        integer min_gap;
        integer max_gap;
        integer sum_gap;
        integer accept_before;
        integer tick_before;

        set_preset(mode, divider);
        repeat (5) wait_normal_accept();
        // Drain the final warm-up acceptance from the one-edge tick stage
        // before taking the baseline counts.
        @(posedge clk);
        #1ps;
        previous_cycle = -1;
        min_gap = 32'h7fffffff;
        max_gap = 0;
        sum_gap = 0;
        accept_before = normal_accept_count;
        tick_before = normal_tick_count;
        for (int i = 0; i < SPEED_SAMPLES; i++) begin
            wait_normal_accept();
            if (previous_cycle >= 0) begin
                gap = fabric_cycle - previous_cycle;
                if (gap < min_gap) min_gap = gap;
                if (gap > max_gap) max_gap = gap;
                sum_gap += gap;
            end
            previous_cycle = fabric_cycle;
        end
        average_gap = sum_gap / (SPEED_SAMPLES - 1);
        repeat (3) @(posedge clk);
        #1ps;
        check((normal_accept_count - accept_before) ==
              (normal_tick_count - tick_before),
              $sformatf("%s lost or duplicated a normal Disk II tick", label));
        if (mode == 2'd0) begin
            check(min_gap == 4 && max_gap == 4,
                  $sformatf("%s did not complete shadow cycles every four fabric clocks (min=%0d max=%0d)",
                            label, min_gap, max_gap));
        end
        else if (mode == 2'd1) begin
            check(min_gap >= divider && max_gap <= divider + 2,
                  $sformatf("%s did not honor divider %0d (min=%0d max=%0d)",
                            label, divider, min_gap, max_gap));
        end
        else begin
            check(min_gap == 130 && max_gap == 131,
                  $sformatf("%s was not paced by the native Apple data tick (min=%0d max=%0d)",
                            label, min_gap, max_gap));
        end
        $display("VTW DISK2 SPEED: %-22s gap min/avg/max = %0d/%0d/%0d",
                 label, min_gap, average_gap, max_gap);
    endtask

    task automatic measure_q7_speed(input logic [1:0] mode,
                                    input integer divider,
                                    input string label);
        integer previous_cycle;
        integer gap;
        integer min_gap;
        integer max_gap;

        set_preset(mode, divider);
        repeat (3) wait_normal_accept();
        previous_cycle = -1;
        min_gap = 32'h7fffffff;
        max_gap = 0;
        for (int i = 0; i < 8; i++) begin
            wait_normal_accept();
            if (previous_cycle >= 0) begin
                gap = fabric_cycle - previous_cycle;
                if (gap < min_gap) min_gap = gap;
                if (gap > max_gap) max_gap = gap;
            end
            previous_cycle = fabric_cycle;
        end
        #1ps;
        check(min_gap == 130 && max_gap == 131,
              $sformatf("Q7 did not force %s to native pace (min=%0d max=%0d)",
                        label, min_gap, max_gap));
        check(video_phase_1mhz,
              $sformatf("Q7 did not report 1 MHz phase at %s", label));
    endtask

    task automatic wait_private_delta(input integer before_count,
                                      input integer wanted,
                                      input string label);
        integer timeout;
        timeout = 0;
        while ((private_req_count - before_count) < wanted &&
               timeout < 100000) begin
            @(posedge clk);
            #1ps;
            timeout++;
        end
        check((private_req_count - before_count) >= wanted,
              $sformatf("%s did not issue %0d private requests", label, wanted));
    endtask

    task automatic wait_bus_delta(input integer before_count,
                                  input integer wanted,
                                  input string label);
        integer timeout;
        timeout = 0;
        while ((int'(cnt_bus_cycles) - before_count) < wanted &&
               timeout < 100000) begin
            @(posedge clk);
            #1ps;
            timeout++;
        end
        check((int'(cnt_bus_cycles) - before_count) >= wanted,
              $sformatf("%s did not issue %0d physical bus cycles", label,
                        wanted));
    endtask

    task automatic check_private_preset(input logic [1:0] mode,
                                        input integer divider,
                                        input string label);
        integer req_before;
        integer bus_before;
        integer private_before;
        set_preset(mode, divider);
        req_before = private_req_count;
        bus_before = int'(cnt_bus_cycles);
        private_before = private_select_count;
        wait_private_delta(req_before, 2, label);
        repeat (4) begin
            @(posedge clk);
            #1ps;
        end
        check(int'(cnt_bus_cycles) == bus_before,
              $sformatf("%s even reads leaked onto the Apple bus", label));
        check(private_select_count > private_before,
              $sformatf("%s even reads did not select private time", label));
        check(disk2_req_addr == 4'hC || !disk2_req_valid,
              $sformatf("%s changed the private switch address", label));
    endtask

    task automatic check_native_preset(input logic [1:0] mode,
                                       input integer divider,
                                       input string label);
        integer req_before;
        integer bus_before;
        integer native_before;
        integer window_before;
        set_preset(mode, divider);
        req_before = private_req_count;
        bus_before = int'(cnt_bus_cycles);
        native_before = native_select_count;
        window_before = native_window_count;
        wait_bus_delta(bus_before, 2, label);
        repeat (4) begin
            @(posedge clk);
            #1ps;
        end
        check(dbg_last_sync_addr === expected_native_addr,
              $sformatf("%s completed address $%04X, expected $%04X",
                        label, dbg_last_sync_addr, expected_native_addr));
        check(dbg_last_sync_rw === expected_native_rw,
              $sformatf("%s completed with R/W=%0b, expected %0b",
                        label, dbg_last_sync_rw, expected_native_rw));
        check(private_req_count == req_before,
              $sformatf("%s used the private read port", label));
        check(native_select_count > native_before,
              $sformatf("%s did not select native time", label));
        check(native_window_count > window_before,
              $sformatf("%s did not expose a native Disk II bus window", label));
    endtask

    task automatic run_private_matrix;
        check_private_preset(2'd1, 2667, "even read / 0.05 MHz slug");
        check_private_preset(2'd2, 0,  "even read / 1 MHz");
        check_private_preset(2'd1, 51, "even read / 2.6 MHz");
        check_private_preset(2'd1, 37, "even read / 3.6 MHz");
        check_private_preset(2'd1, 19, "even read / 7 MHz");
        check_private_preset(2'd1, 10, "even read / 13 MHz");
        check_private_preset(2'd1, 5,  "even read / 26 MHz");
        check_private_preset(2'd0, 0,  "even read / MAX");
    endtask

    task automatic run_native_matrix(input string operation);
        check_native_preset(2'd1, 2667,
                            $sformatf("%s / 0.05 MHz slug", operation));
        check_native_preset(2'd2, 0,  $sformatf("%s / 1 MHz", operation));
        check_native_preset(2'd1, 51, $sformatf("%s / 2.6 MHz", operation));
        check_native_preset(2'd1, 37, $sformatf("%s / 3.6 MHz", operation));
        check_native_preset(2'd1, 19, $sformatf("%s / 7 MHz", operation));
        check_native_preset(2'd1, 10, $sformatf("%s / 13 MHz", operation));
        check_native_preset(2'd1, 5,  $sformatf("%s / 26 MHz", operation));
        check_native_preset(2'd0, 0,  $sformatf("%s / MAX", operation));
    endtask

    integer avg_slug;
    integer avg_1mhz;
    integer avg_2m6;
    integer avg_3m6;
    integer avg_7m;
    integer avg_13m;
    integer avg_26m;
    integer avg_max;

    initial begin
        // 1. One live session changes through all seven ladder presets and
        // the UI's divided-mode 0.05 MHz slug override.
        reboot_normal();
        measure_normal_speed(2'd1, 2667, "0.05 MHz slug", avg_slug);
        measure_normal_speed(2'd2, 0,  "1 MHz", avg_1mhz);
        measure_normal_speed(2'd1, 51, "2.6 MHz", avg_2m6);
        measure_normal_speed(2'd1, 37, "3.6 MHz", avg_3m6);
        measure_normal_speed(2'd1, 19, "7 MHz", avg_7m);
        measure_normal_speed(2'd1, 10, "13 MHz", avg_13m);
        measure_normal_speed(2'd1, 5,  "26 MHz", avg_26m);
        measure_normal_speed(2'd0, 0,  "MAX", avg_max);
        check(avg_slug > avg_1mhz &&
              avg_1mhz > avg_2m6 && avg_2m6 > avg_3m6 &&
              avg_3m6 > avg_7m && avg_7m > avg_13m &&
              avg_13m > avg_26m && avg_26m > avg_max,
              "live preset changes did not produce the full ordered speed ladder");

        // 2. A pending normal tick drains once when time readiness falls.
        begin
            integer accept_before;
            integer tick_before;
            // Find a high completion enable before its accepting edge. This
            // avoids a testbench/RTL race at posedge and guarantees that the
            // tick stage contains one entry when readiness falls.
            while (!normal_tick_accept)
                @(negedge clk);
            @(posedge clk);
            @(negedge clk);
            accept_before = normal_accept_count;
            tick_before = normal_tick_count;
            check(disk2_cycle_tick,
                  "time-readiness test did not arm a pending tick");
            disk2_time_ready = 1'b0;
            speed_mode = 2'd1;
            pace_divider = 16'd51; // change mode while frozen
            repeat (4) @(posedge clk);
            #1ps;
            check(normal_accept_count == accept_before,
                  "time-readiness stall admitted a logical cycle");
            check(normal_tick_count == tick_before + 1,
                  $sformatf("time-readiness stall lost or duplicated the pending tick (accept=%0d tick before=%0d now=%0d)",
                            accept_before, tick_before, normal_tick_count));
            repeat (20) @(posedge clk);
            #1ps;
            check(normal_accept_count == accept_before &&
                  normal_tick_count == tick_before + 1,
                  $sformatf("time-readiness stall emitted an extra tick (accept before=%0d now=%0d tick before=%0d now=%0d)",
                            accept_before, normal_accept_count,
                            tick_before, normal_tick_count));
            @(negedge clk);
            disk2_time_ready = 1'b1;
            measure_normal_speed(2'd1, 51, "2.6 MHz after stall", avg_2m6);
        end

        // 3. Q7 write mode forces native pace for the whole ladder and the
        // slug override.
        @(negedge clk);
        disk2_write_timing_active = 1'b1;
        while (!dut.bus_owned)
            @(posedge clk);
        repeat (2) @(posedge clk);
        measure_q7_speed(2'd1, 2667, "0.05 MHz slug");
        measure_q7_speed(2'd2, 0,  "1 MHz");
        measure_q7_speed(2'd1, 51, "2.6 MHz");
        measure_q7_speed(2'd1, 37, "3.6 MHz");
        measure_q7_speed(2'd1, 19, "7 MHz");
        measure_q7_speed(2'd1, 10, "13 MHz");
        measure_q7_speed(2'd1, 5,  "26 MHz");
        measure_q7_speed(2'd0, 0,  "MAX");
        @(negedge clk);
        disk2_write_timing_active = 1'b0;

        // 4. Even reads stay on the private port at all ladder presets and
        // the slug override.
        reboot_route(8'hAD, 16'hC0EC); // LDA $C0EC
        run_private_matrix();

        // Hold private READY low: valid and address must remain stable, no
        // private request may be accepted, and the core cannot complete it.
        @(negedge clk);
        disk2_req_ready = 1'b0;
        while (!disk2_req_valid) begin
            @(posedge clk);
            #1ps;
        end
        begin
            integer req_before;
            integer private_before;
            integer core_before;
            req_before = private_req_count;
            private_before = private_select_count;
            core_before = int'(cnt_core_cycles);
            repeat (24) begin
                @(posedge clk);
                #1ps;
                check(disk2_req_valid && disk2_req_addr == 4'hC,
                      "private READY stall did not hold valid/address");
            end
            check(private_req_count == req_before,
                  "private READY stall accepted a request");
            check(private_select_count == private_before &&
                  int'(cnt_core_cycles) == core_before,
                  "private READY stall completed a logical cycle");
            @(negedge clk);
            disk2_req_ready = 1'b1;
            wait_private_delta(req_before, 1,
                               "private READY release");
            repeat (20) begin
                @(posedge clk);
                #1ps;
            end
            check(private_select_count == private_before + 1,
                  "private READY release did not complete exactly once");
        end

        // 5. Writes and odd reads stay on the physical 1 MHz path at all
        // configured speeds, including the slug override.
        reboot_route(8'h8D, 16'hC0EC); // STA $C0EC
        run_native_matrix("write");
        reboot_route(8'hAD, 16'hC0ED); // LDA $C0ED
        run_native_matrix("odd read");

        // 6. Q7 also removes the even-read private shortcut.
        reboot_route(8'hAD, 16'hC0EC);
        @(negedge clk);
        speed_mode = 2'd0;
        pace_divider = 16'd0;
        disk2_write_timing_active = 1'b1;
        check_native_preset(2'd1, 2667, "Q7 even read / 0.05 MHz slug");
        check_native_preset(2'd0, 0, "Q7 even read / MAX");
        check(video_phase_1mhz,
              "Q7 even-read route did not report the 1 MHz interlock");

        // Let the one-stage normal tick monitor drain before final counts.
        @(negedge clk);
        core_run = 1'b0;
        repeat (3) @(posedge clk);
        #1ps;
        check(normal_accept_count == normal_tick_count,
              "final normal tick count did not drain exactly");
        check(logical_select_count == normal_accept_count +
              private_select_count + native_select_count,
              "logical cycles did not have exactly one Disk II time source");

        $display("VTW DISK2 SPEED MATRIX PASS");
        $finish;
    end

    initial begin
        #25ms;
        $fatal(1, "VTW DISK2 SPEED MATRIX FAIL: timeout");
    end

endmodule
