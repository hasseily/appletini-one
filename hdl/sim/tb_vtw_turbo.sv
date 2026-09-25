`timescale 1ns / 1ps
// TURBO end-to-end execution and shadow coherency regression.
// Programs execute through the production CPU, shadow, wrapper and bus engine.
// Results are CPU writes and real motherboard side effects, not cache tags.

module tb_vtw_turbo;

    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;       // 133.333 MHz fabric clock

    logic phi0 = 1'b0;
    always #490 phi0 = ~phi0;      // about 1.02 MHz Apple clock

    logic rstn = 1'b0;
    logic host_is_iiplus = 1'b0;

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
        .clk(clk), .rstn(rstn), .physical_bus_isolate(1'b0),
        .res_filtered_out(), .dbg_lost_cycle_count(), .dbg_clear(1'b0),
        .dbg_bus_quality(), .dbg_tap_mismatch(), .dbg_strobe_anom(),
        .dbg_tap_last(), .dbg_ghost_write(),
        .inh_allowed(1'b1), .physical_slave_select_ok(1'b1),
        .physical_irq_allowed(1'b1), .gs_m2_qualify(1'b0),
        .m2sel_active_high(1'b0),
        .host_is_iiplus(host_is_iiplus),
        .iiplus_dma_refresh_active(1'b0),
        .apple_data_pin(apple_data_pin), .apple_addr_pin(apple_addr_pin),
        .apple_rw_pin(apple_rw_pin), .apple_phi0_pin(phi0),
        .apple_m2sel_pin(1'b0), .apple_m2b0_pin(1'b0),
        .apple_devsel_n_pin(1'b1),
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
    logic [7:0] mb_aux [0:16'hFFFF];
    logic mb_aux_write = 1'b0;
    logic mb_80store = 1'b0, mb_page2 = 1'b0, mb_hires = 1'b0;
    bit check_deferred_page_flip = 0;
    bit check_display_flip = 0;
    logic [15:0] checked_display_switch, checked_display_byte;
    integer checked_display_cycles = 0;
    bit check_classic_fallback_write = 0;
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
    logic        disk2_active = 1'b0;
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

    logic pause = 1'b0;
    logic arm_rw_flush_req = 1'b0;
    logic arm_rw_hold_release = 1'b0;
    logic arm_rw_flush_done;
    logic arm_rw_hold_state;
    logic arm_req_valid = 1'b0;
    logic arm_req_busy, arm_resp_valid;
    logic [7:0] arm_resp_rdata;
    integer arm_responses = 0;
    logic disk2_motor_active = 1'b0;
    logic [31:0] cnt_posted_writes;
    logic [31:0] cnt_post_drops;
    logic [31:0] cnt_invalid_routes;
    logic [9:0] post_fill;
    logic video_record_enable = 1'b0;
    logic video_record_valid, video_direct_active;
    logic [16:0] video_record_addr;
    logic [7:0] video_record_data;
    logic video_record_ready = 1'b1;
    logic post_main_wide = 1'b0;
    logic overlay_capture_armed = 1'b0;
    logic bus_owned;
    integer direct_video_writes = 0;
    logic [7:0] renderer_shadow [0:131071];
    bit check_video_banks = 0;
    logic ramworks_en = 1'b0;
    logic rw_req_valid, rw_req_rw, rw_req_ready = 1'b0;
    logic rw_resp_valid = 1'b0;
    logic [23:0] rw_req_addr;
    logic [63:0] rw_req_wline, rw_resp_rline;
    logic [7:0] psram_model [logic [23:0]];
    logic ramworks_busy = 1'b0;
    logic ramworks_read;
    logic [23:0] ramworks_address;
    logic [63:0] ramworks_data;
    integer ramworks_delay;
    integer ramworks_operations = 0;

    // A real request/response boundary with variable latency, including
    // dirty-line writeback. The host changes this storage only while held.
    always @(posedge clk) begin
        rw_req_ready <= 1'b0;
        rw_resp_valid <= 1'b0;
        if (!rstn) begin
            ramworks_busy <= 1'b0;
            ramworks_operations <= 0;
        end else if (rw_req_valid && !ramworks_busy) begin
            ramworks_busy <= 1'b1;
            ramworks_read <= rw_req_rw;
            ramworks_address <= {rw_req_addr[23:3], 3'b000};
            ramworks_data <= rw_req_wline;
            ramworks_delay <= 16 + (ramworks_operations % 5) * 7;
            ramworks_operations <= ramworks_operations + 1;
            rw_req_ready <= 1'b1;
        end else if (ramworks_busy) begin
            if (ramworks_delay != 0)
                ramworks_delay <= ramworks_delay - 1;
            else begin
                for (int i = 0; i < 8; i++) begin
                    if (ramworks_read)
                        rw_resp_rline[8*i +: 8] <=
                            psram_model.exists(ramworks_address + 24'(i)) ?
                            psram_model[ramworks_address + 24'(i)] : 8'hFF;
                    else
                        psram_model[ramworks_address + 24'(i)] = ramworks_data[8*i +: 8];
                end
                rw_resp_valid <= 1'b1;
                ramworks_busy <= 1'b0;
            end
        end
    end

    vtw_core_top dut (
        .clk(clk), .rstn(rstn), .enable(enable),
        .host_is_iiplus(host_is_iiplus), .virtual_motherboard(1'b0),
        .core_run(core_run),
        .pause(pause),
        .assert_apple_res(1'b0),
        .speed_mode(speed_mode), .pace_divider(pace_divider),
        .ignore_c074(1'b0),
        .irq_assert_in(1'b0),
        .data_drive_in(vtw_ab_write.wr_data_en),
        .data_drive_value_in(vtw_ab_write.wr_data),
        .dbg_clear(1'b0), .iiplus_buttons_zero(1'b0),
        .slow_region_en(10'd0), .slow_duration(16'd0),
        .d2_active(disk2_active), .d2_motor_active(disk2_motor_active),
        .d2_req_valid(disk2_req_valid), .d2_req_addr(disk2_req_addr),
        .d2_req_ready(disk2_req_ready),
        .d2_resp_valid(disk2_resp_valid),
        .d2_resp_rdata(disk2_resp_rdata),
        .d2_cycle_tick(disk2_cycle_tick),
        .d2_native_cycle_active(disk2_native_cycle_active),
        .d2_time_ready(disk2_time_ready),
        .d2_write_timing_active(disk2_write_timing_active),
        .ramworks_en(ramworks_en), .video_vbl(1'b0),
        .post_main_wide(post_main_wide),
        .video_record_enable(video_record_enable),
        .video_record_valid(video_record_valid),
        .video_record_addr(video_record_addr),
        .video_record_data(video_record_data),
        .video_record_ready(video_record_ready),
        .video_direct_active(video_direct_active),
        .overlay_capture_armed(overlay_capture_armed),
        .overlay_capture_bank_aux(1'b0),
        .overlay_capture_base(16'd0),
        .overlay_capture_limit(16'd0),
        .video_mode_50hz(1'b0), .video_line(9'd0),
        .video_cycle(7'd0),
        .ab_read(ab_read), .ab_write(vtw_ab_write),
        .rw_req_valid(rw_req_valid), .rw_req_rw(rw_req_rw),
        .rw_req_addr(rw_req_addr), .rw_req_wline(rw_req_wline),
        .rw_req_ready(rw_req_ready), .rw_resp_valid(rw_resp_valid),
        .rw_resp_rline(rw_resp_rline),
        .sp_active(1'b0), .sp_boot_suppress(1'b0),
        .sp_req_valid(), .sp_req_target(), .sp_req_addr(), .sp_req_rw(),
        .sp_req_wdata(), .sp_req_ready(1'b1), .sp_resp_valid(1'b0),
        .sp_resp_rdata(8'd0), .sp_sss_snapshot(),
        .sh_en(sh_en), .sh_addr(sh_addr), .sh_we(sh_we),
        .sh_wdata(sh_wdata), .sh_rdata(sh_rdata),
        .arm_req_valid(arm_req_valid), .arm_req_addr(16'hC020), .arm_req_rw(1'b1),
        .arm_req_wdata('0), .arm_req_busy(arm_req_busy), .arm_resp_valid(arm_resp_valid),
        .arm_resp_rdata(arm_resp_rdata),
        .arm_post_we(1'b0), .arm_post_addr('0), .arm_post_wdata('0),
        .arm_post_ready(),
        .arm_rw_flush_req(arm_rw_flush_req), .arm_rw_hold_release(arm_rw_hold_release),
        .arm_rw_flush_done(arm_rw_flush_done), .arm_rw_hold_state(arm_rw_hold_state),
        .c074_state(), .bus_owned(bus_owned),
        .video_phase_1mhz(video_phase_1mhz),
        .dbg_core_pc(), .cnt_core_cycles(cnt_core_cycles),
        .cnt_bus_cycles(cnt_bus_cycles), .cnt_posted_writes(cnt_posted_writes),
        .post_fill(post_fill), .post_high_water(), .cnt_post_drops(cnt_post_drops),
        .cnt_invalid_routes(cnt_invalid_routes),
        .dbg_vsss(), .dbg_last_sync_addr(dbg_last_sync_addr),
        .dbg_last_sync_data(dbg_last_sync_data),
        .dbg_last_sync_rw(dbg_last_sync_rw), .dbg_irq_edges(),
        .dbg_cxxx_ring(), .dbg_c0_ring(),
        .dbg_sync_write_check(), .dbg_sync_write_addr(),
        .dbg_c000_context(), .dbg_c000_counts(),
        .dbg_pc_trace(), .dbg_io_trace(), .dbg_trace_status(),
        .dbg_bus_faults()
    );

    localparam logic [17:0] ROM_BASE = 18'h20000;
    integer code_pos;
    integer fabric_cycles = 0;
    integer marker_count [0:15];
    logic [7:0] marker_value [0:15];
    integer marker_cycle [0:15];
    integer physical_io_reads = 0;
    integer physical_video_writes = 0;
    integer normal_ticks = 0;
    integer physical_wait_clocks = 0;
    logic physical_request_pending = 1'b0;
    logic [15:0] physical_request_address;

    task automatic check(input bit condition, input string message);
        if (!condition)
            $fatal(1, "VTW TURBO FAIL: %s", message);
    endtask

    // Match the old capture-time policy against the staged result. This
    // reads the live pre-access inputs, independently of the new snapshots.
    wire capture_policy_active;
    logic expected_policy_active = 1'b0;
    integer policy_checks = 0;
    vtw_video_policy capture_policy_i (
        .address(dut.capture_xl_decoded_d[16:0]),
        .sw_text(dut.vsss.sw_text), .sw_mixed(dut.vsss.sw_mixed),
        .sw_page2(dut.vsss.sw_page2), .sw_hires(dut.vsss.sw_hires),
        .sw_80store(dut.vsss.sw_80store), .sw_80col(dut.vsss.sw_80col),
        .post_main_wide(dut.post_main_wide_eff),
        .overlay_match(overlay_capture_armed),
        .mirror_active(capture_policy_active)
    );
    always @(posedge clk) begin
        if (!rstn)
            expected_policy_active = 1'b0;
        else if (dut.core_res_n) begin
            if (dut.xstate_q == dut.X_CAPTURE && dut.d2_time_ready &&
                !dut.video_barrier)
                expected_policy_active = capture_policy_active || host_is_iiplus;
            else if (dut.xstate_q == dut.X_ROUTE) begin
                #1ps;
                check(dut.cycle_video_mirror_active_q === expected_policy_active,
                      "staged policy changed the captured display/bank decision");
                policy_checks++;
            end
        end
    end

    // Observe only accepted CPU accesses. The write address and data must
    // agree across the separate cache lookup and CPU execution stages.
    always @(posedge clk) begin
        fabric_cycles <= fabric_cycles + 1;
        if (!rstn) begin
            for (int i = 0; i < 16; i++) begin
                marker_count[i] <= 0;
                marker_value[i] <= 0;
                marker_cycle[i] <= 0;
            end
            physical_io_reads <= 0;
            physical_video_writes <= 0;
            normal_ticks <= 0;
            physical_wait_clocks <= 0;
            physical_request_pending <= 1'b0;
            mb_aux_write <= 1'b0;
            mb_80store <= 1'b0;
            mb_page2 <= 1'b0;
            mb_hires <= 1'b0;
            direct_video_writes <= 0;
            arm_responses <= 0;
            checked_display_cycles <= 0;
        end else begin
            check((video_record_valid && video_record_ready) ===
                  (dut.video_coalescer_i.write_valid &&
                   dut.video_coalescer_i.write_ready),
                  "renderer and motherboard mirror accepted different records");
            if (video_record_valid)
                check(dut.xstate_q == dut.X_POST_STALL,
                      "direct video bypassed the registered admission stage");
            if (check_display_flip && dut.ssm_apply_pulse &&
                dut.core_addr == checked_display_switch)
                check(mb_ram[checked_display_byte] === 8'hA6,
                      "private display/card side effect preceded the video drain");
            if (check_deferred_page_flip && dut.ssm_apply_pulse &&
                dut.core_addr == 16'hC055)
                check(mb_ram[16'h08DF] === 8'h5E && mb_aux[16'h08DF] === 8'hA7,
                      "private PAGE2 changed before deferred banks drained");
            if (arm_resp_valid) begin
                arm_responses <= arm_responses + 1;
                check(!dut.video_sync_active && dut.video_all_drained &&
                      physical_io_reads == 1 && arm_resp_rdata === 8'hEE,
                      "mirror maintenance completed or corrupted an ARM bus request");
            end
            if (video_record_valid && video_record_ready) begin
                direct_video_writes <= direct_video_writes + 1;
                renderer_shadow[video_record_addr] <= video_record_data;
            end
            if (ab_read.data_en) begin
                if (check_display_flip && ab_read.addr == checked_display_switch) begin
                    checked_display_cycles <= checked_display_cycles + 1;
                    check(mb_ram[checked_display_byte] === 8'hA6,
                          "display switch reached the bus before its memory was current");
                end
                case (ab_read.addr)
                    16'hC054: mb_page2 <= 1'b0;
                    16'hC055: begin
                        if (check_deferred_page_flip)
                            check(mb_ram[16'h08DF] === 8'h5E &&
                                  mb_aux[16'h08DF] === 8'hA7,
                                  "PAGE2 changed before deferred banks drained");
                        mb_page2 <= 1'b1;
                    end
                    16'hC056: mb_hires <= 1'b0;
                    16'hC057: mb_hires <= 1'b1;
                    default: begin end
                endcase
            end
            if (ab_read.data_en && !ab_read.rw) begin
                if (ab_read.addr == 16'hC000) mb_80store <= 1'b0;
                if (ab_read.addr == 16'hC001) mb_80store <= 1'b1;
                if (ab_read.addr == 16'hC005) begin
                    if (check_video_banks)
                        for (int i = 0; i < 1024; i++)
                            check(mb_ram[16'h2000+i] === 8'hC3,
                                  "RAMWRT changed before MAIN mirror drained");
                    mb_aux_write <= 1'b1;
                end
                if (ab_read.addr == 16'hC004) begin
                    if (check_video_banks)
                        for (int i = 0; i < 512; i++)
                            check(mb_aux[16'h2000+i] === 8'hA5,
                                  "RAMWRT changed before AUX mirror drained");
                    mb_aux_write <= 1'b0;
                end
                if (ab_read.addr < 16'hC000) begin
                    if (check_classic_fallback_write && ab_read.addr == 16'h08E0)
                        check(!video_direct_active,
                              "classic fallback write was suppressed from physical capture");
                    if (video_record_enable &&
                        (speed_mode == 2'd3 || dut.video_mirror_pending))
                        check(video_direct_active,
                              "mirrored physical byte was not suppressed in capture");
                    if (mb_80store &&
                        ((ab_read.addr >= 16'h0400 && ab_read.addr < 16'h0800) ||
                         (mb_hires && ab_read.addr >= 16'h2000 &&
                          ab_read.addr < 16'h4000)) ? mb_page2 : mb_aux_write)
                        mb_aux[ab_read.addr] <= ab_read.data;
                    else
                        mb_ram[ab_read.addr] <= ab_read.data;
                end
            end
            if (dut.eng_req_valid && dut.eng_req_ready) begin
                check(!physical_request_pending,
                      "second physical request overlapped an unfinished request");
                physical_request_pending <= 1'b1;
                physical_request_address <= dut.core_addr;
            end
            if (physical_request_pending) begin
                physical_wait_clocks <= physical_wait_clocks + 1;
                check(!dut.core_en && dut.core_addr == physical_request_address,
                      "TURBO advanced the CPU before the physical bus response");
                if (dut.eng_resp_valid)
                    physical_request_pending <= 1'b0;
            end
            if (dut.core_en && !dut.core_rwb &&
                dut.core_addr[15:4] == 12'hA10) begin
                marker_count[dut.core_addr[3:0]] <=
                    marker_count[dut.core_addr[3:0]] + 1;
                marker_value[dut.core_addr[3:0]] <= dut.core_data_out;
                marker_cycle[dut.core_addr[3:0]] <= fabric_cycles;
            end
            if (ab_read.data_en && ab_read.rw &&
                ab_read.addr == 16'hC020)
                physical_io_reads <= physical_io_reads + 1;
            if (ab_read.data_en && !ab_read.rw &&
                ab_read.addr >= 16'h0400 && ab_read.addr < 16'h0410) begin
                check(ab_read.data === {4'h0, ab_read.addr[3:0]},
                      "posted video write changed its address/data pair");
                physical_video_writes <= physical_video_writes + 1;
                mb_ram[ab_read.addr] <= ab_read.data;
            end
            if (disk2_cycle_tick)
                normal_ticks <= normal_ticks + 1;
        end
    end

    task automatic sh_write(input logic [17:0] a, input logic [7:0] d);
        @(negedge clk);
        sh_en = 1'b1;
        sh_we = 1'b1;
        sh_addr = a;
        sh_wdata = d;
        @(negedge clk);
        sh_en = 1'b0;
        sh_we = 1'b0;
    endtask

    task automatic sh_check(input logic [17:0] a, input logic [7:0] d);
        @(negedge clk);
        sh_en = 1'b1;
        sh_we = 1'b0;
        sh_addr = a;
        repeat (2) @(posedge clk);
        #1ps;
        check(sh_rdata === d,
              $sformatf("shadow $%05X contained $%02X, expected $%02X",
                        a, sh_rdata, d));
        @(negedge clk);
        sh_en = 1'b0;
    endtask

    task automatic emit(input logic [7:0] value);
        sh_write(ROM_BASE + 18'h3000 + 18'(code_pos), value);
        code_pos++;
    endtask

    task automatic absolute(input logic [7:0] opcode,
                            input logic [15:0] address);
        emit(opcode);
        emit(address[7:0]);
        emit(address[15:8]);
    endtask

    task automatic immediate(input logic [7:0] opcode,
                             input logic [7:0] value);
        emit(opcode);
        emit(value);
    endtask

    task automatic halt_loop;
        absolute(8'h4C, 16'hF000 + 16'(code_pos));
    endtask

    task automatic begin_program(input logic [1:0] mode);
        @(negedge clk);
        core_run = 1'b0;
        enable = 1'b0;
        rstn = 1'b0;
        host_is_iiplus = 1'b0;
        res_drive_low = 1'b1;
        pause = 1'b0;
        arm_rw_flush_req = 1'b0;
        arm_rw_hold_release = 1'b0;
        arm_req_valid = 1'b0;
        disk2_active = 1'b0;
        disk2_motor_active = 1'b0;
        ramworks_en = 1'b0;
        video_record_enable = 1'b0;
        video_record_ready = 1'b1;
        post_main_wide = 1'b0;
        overlay_capture_armed = 1'b0;
        check_video_banks = 1'b0;
        check_deferred_page_flip = 1'b0;
        check_display_flip = 1'b0;
        check_classic_fallback_write = 1'b0;
        speed_mode = mode;
        pace_divider = 0;
        repeat (20) @(posedge clk);
        @(negedge clk);
        rstn = 1'b1;
        code_pos = 0;
        sh_write(ROM_BASE + 18'h3FFC, 8'h00);
        sh_write(ROM_BASE + 18'h3FFD, 8'hF0);
        sh_write(18'h0A100, 8'h00);
    endtask

    task automatic ramworks_program;
        integer n;
        integer core_before;
        logic [15:0] loop_address;
        begin_program(2'd3);
        ramworks_en = 1'b1;
        sh_write(18'h09000, 8'h11);
        absolute(8'hAD, 16'h9000);
        absolute(8'h8D, 16'hA10E);     // Warm the same virtual main address.
        immediate(8'hA9, 8'h02);
        absolute(8'h8D, 16'hC073);     // RamWorks register 2 -> physical bank 3.
        absolute(8'h8D, 16'hC005);
        immediate(8'hA9, 8'hA5);
        absolute(8'h8D, 16'h9000);
        immediate(8'hA9, 8'h5A);
        absolute(8'h8D, 16'h9008);     // Evict the first dirty line.
        immediate(8'hA9, 8'h05);
        absolute(8'h8D, 16'hC071);     // Other bank, same virtual address.
        immediate(8'hA9, 8'hC7);
        absolute(8'h8D, 16'h9000);
        absolute(8'h8D, 16'hC004);
        absolute(8'h8D, 16'hC003);
        absolute(8'hAD, 16'h9000);
        absolute(8'h8D, 16'hA100);
        immediate(8'hA9, 8'h02);
        absolute(8'h8D, 16'hC073);
        absolute(8'hAD, 16'h9000);
        absolute(8'h8D, 16'hA101);
        absolute(8'hAD, 16'h9008);
        absolute(8'h8D, 16'hA102);
        immediate(8'hA9, 8'h05);
        absolute(8'h8D, 16'hC073);
        absolute(8'hAD, 16'h9000);
        absolute(8'h8D, 16'hA103);
        absolute(8'h8D, 16'hC005);
        immediate(8'hA9, 8'hC8);
        absolute(8'h8D, 16'h9000);     // Leave the current line dirty.
        absolute(8'h8D, 16'hC004);
        loop_address = 16'hF000 + 16'(code_pos);
        absolute(8'hAD, 16'h9000);
        absolute(8'h8D, 16'hA104);
        absolute(8'h4C, loop_address);
        start_program();
        wait_marker(4, 3);
        check(marker_value[14] == 8'h11 && marker_value[0] == 8'hC7 &&
              marker_value[1] == 8'hA5 && marker_value[2] == 8'h5A &&
              marker_value[3] == 8'hC7 && marker_value[4] == 8'hC8,
              "TURBO RamWorks reads/writes confused main, lines or banks");
        @(negedge clk);
        arm_rw_flush_req = 1'b1;
        @(negedge clk);
        arm_rw_flush_req = 1'b0;
        while (!arm_rw_flush_done) begin
            @(posedge clk);
            #1ps;
        end
        check(arm_rw_hold_state && !ramworks_busy,
              "TURBO RamWorks flush completed before the line port drained");
        check(psram_model.exists(24'h039000) && psram_model[24'h039000] == 8'hA5 &&
              psram_model.exists(24'h039008) && psram_model[24'h039008] == 8'h5A &&
              psram_model.exists(24'h069000) && psram_model[24'h069000] == 8'hC8,
              "TURBO dirty-line flush did not persist the correct PSRAM bytes");
        core_before = int'(cnt_core_cycles);
        psram_model[24'h069000] = 8'hD2;
        repeat (40) @(posedge clk);
        #1ps;
        check(cnt_core_cycles == core_before,
              "TURBO ran while host DMA owned RamWorks storage");
        n = marker_count[4];
        @(negedge clk);
        arm_rw_hold_release = 1'b1;
        @(negedge clk);
        arm_rw_hold_release = 1'b0;
        wait_marker(4, n + 3);
        check(marker_value[4] == 8'hD2,
              "TURBO RamWorks resumed with pre-DMA cache contents");
        freeze_core();
        sh_check(18'h09000, 8'h11);
        check(ramworks_operations >= 9,
              "RamWorks test did not exercise line fills and dirty evictions");
    endtask

    task automatic start_program;
        enable = 1'b1;
        #10us;
        @(negedge clk);
        res_drive_low = 1'b0;
        #4us;
        @(negedge clk);
        core_run = 1'b1;
    endtask

    task automatic wait_marker(input integer index, input integer wanted,
                               input integer budget = 200000);
        integer timeout;
        timeout = 0;
        while (marker_count[index] < wanted && timeout < budget) begin
            @(posedge clk);
            #1ps;
            timeout++;
        end
        check(marker_count[index] >= wanted,
              $sformatf("marker %0d timed out at PC $%04X after %0d writes",
                        index, dut.core_addr, marker_count[index]));
    endtask

    task automatic freeze_core;
        @(negedge clk);
        pause = 1'b1;
        repeat (12) @(posedge clk);
        #1ps;
    endtask

    task automatic freeze_cached_read(input logic [15:0] address);
        integer timeout;
        timeout = 0;
        while (!pause && timeout < 200000) begin
            @(negedge clk);
            if (dut.xstate_q == dut.X_TURBO_DONE && dut.turbo_hit &&
                dut.core_rwb && dut.core_addr == address)
                pause = 1'b1;
            timeout++;
        end
        check(pause, "did not find a cached read waiting for its execution edge");
        repeat (12) @(posedge clk);
        #1ps;
        check(dut.xstate_q == dut.X_TURBO_DONE && dut.core_addr == address,
              "pause did not retain the pending cached read");
    endtask

    task automatic pending_write_abort(input integer reset_kind);
        integer timeout;
        logic [7:0] previous_value;
        begin_program(2'd3);
        sh_write(18'h09000, 8'hFF);
        immediate(8'hA2, 8'h00);      // LDX #0
        emit(8'h8A);                  // loop: TXA
        absolute(8'h8D, 16'h9000);
        emit(8'hE8);                  // INX
        absolute(8'h4C, 16'hF002);
        start_program();
        timeout = 0;
        while (!pause && timeout < 200000) begin
            @(negedge clk);
            if (dut.xstate_q == dut.X_TURBO_DONE && dut.turbo_hit && !dut.core_rwb &&
                dut.core_addr == 16'h9000 && dut.core_data_out != 8'h00)
                pause = 1'b1;
            timeout++;
        end
        check(pause, "did not find a pending cached write for session abort");
        previous_value = dut.core_data_out - 8'd1;
        sh_check(18'h09000, previous_value);
        @(negedge clk);
        case (reset_kind)
            0: enable = 1'b0;
            1: core_run = 1'b0;
            2: begin
                res_drive_low = 1'b1;
                // The wrapper qualifies RESET. Release pause immediately
                // after that fact changes, before core_res_n catches up.
                while (ab_read.res) begin
                    @(posedge clk);
                    #1ps;
                end
                @(negedge clk);
            end
        endcase
        pause = 1'b0;
        repeat (12) @(posedge clk);
        sh_check(18'h09000, previous_value);
        check(!dut.core_res_n,
              "session abort did not reset the CPU after pending cache write");
    endtask

    // Sixteen-byte copies repeatedly execute the same indexed instruction
    // stream. Markers delimit complete work, so omitted internal CPU cycles
    // cannot make the measurement claim speed without producing results.
    task automatic benchmark(input logic [1:0] mode,
                             input logic [15:0] source_address,
                             output integer cold_clocks,
                             output integer hot_clocks);
        integer started;
        integer warmed;
        begin_program(mode);
        for (int i = 0; i < 16; i++) begin
            sh_write({2'b00, source_address} + 18'(i), 8'(i * 7 + 3));
            sh_write(18'h0A000 + 18'(i), 0);
        end
        immediate(8'hA2, 8'h00);       // LDX #0
        absolute(8'hBD, source_address); // loop: LDA source,X
        absolute(8'h9D, 16'hA000);    // STA $A000,X
        emit(8'hE8);                 // INX
        immediate(8'hE0, 8'h10);     // CPX #16
        immediate(8'hD0, 8'hF5);     // BNE loop
        absolute(8'hEE, 16'hA100);    // INC completion marker
        absolute(8'h4C, 16'hF000);
        start_program();
        started = fabric_cycles;
        wait_marker(0, 1);
        cold_clocks = marker_cycle[0] - started;
        wait_marker(0, 3);
        warmed = marker_cycle[0];
        wait_marker(0, 11);
        hot_clocks = (marker_cycle[0] - warmed) / 8;
        freeze_core();
        for (int i = 0; i < 16; i++)
            sh_check(18'h0A000 + 18'(i), 8'(i * 7 + 3));
        check(cnt_invalid_routes == 0 && cnt_post_drops == 0,
              "RAM benchmark produced an invalid route or dropped write");
    endtask

    task automatic coherency_program;
        integer n;
        integer core_before;
        logic [15:0] pc_before;
        begin_program(2'd3);
        sh_write(18'h09000, 8'h19);
        absolute(8'hAD, 16'h9000);
        absolute(8'h8D, 16'hA100);
        absolute(8'h4C, 16'hF000);
        start_program();
        wait_marker(0, 4);
        check(marker_value[0] == 8'h19, "cold/hot RAM read value differed");
        freeze_cached_read(16'h9000);
        core_before = int'(cnt_core_cycles);
        pc_before = dut.core_addr;
        sh_write(18'h09000, 8'hE7);
        repeat (40) @(posedge clk);
        #1ps;
        check(cnt_core_cycles == core_before && dut.core_addr == pc_before,
              "pause did not freeze the CPU through a host shadow write");
        n = marker_count[0];
        @(negedge clk);
        pause = 1'b0;
        // This read was captured but had not reached the CPU. A host write
        // must reissue it, so even the first result after pause is current.
        wait_marker(0, n + 1);
        check(marker_value[0] == 8'hE7, "paused cached response consumed pre-ARM data");
        wait_marker(0, n + 3);
        check(marker_value[0] == 8'hE7, "ARM write left stale cached RAM");

        // The SmartPort flush/hold handshake must still freeze a TURBO
        // session while the host changes shadow memory through port B.
        freeze_cached_read(16'h9000);
        @(negedge clk);
        arm_rw_flush_req = 1'b1;
        pause = 1'b0;
        @(negedge clk);
        arm_rw_flush_req = 1'b0;
        while (!arm_rw_flush_done) begin
            @(posedge clk);
            #1ps;
        end
        check(arm_rw_hold_state, "flush completion did not hold TURBO");
        core_before = int'(cnt_core_cycles);
        sh_write(18'h09000, 8'h5B);
        repeat (30) @(posedge clk);
        #1ps;
        check(cnt_core_cycles == core_before, "CPU ran during host DMA hold");
        n = marker_count[0];
        @(negedge clk);
        arm_rw_hold_release = 1'b1;
        @(negedge clk);
        arm_rw_hold_release = 1'b0;
        wait_marker(0, n + 3);
        check(marker_value[0] == 8'h5B, "DMA hold release reused stale RAM");

        // Switch the configured mode while code and data are hot.
        for (int mode_index = 0; mode_index < 4; mode_index++) begin
            @(negedge clk);
            case (mode_index)
                0: speed_mode = 2'd2;
                1: speed_mode = 2'd0;
                2: speed_mode = 2'd3;
                3: speed_mode = 2'd2;
            endcase
            n = marker_count[0];
            wait_marker(0, n + 3);
            check(marker_value[0] == 8'h5B,
                  "live 1 MHz/MAX/TURBO transition changed program results");
        end
        @(negedge clk);
        speed_mode = 2'd3;
        res_drive_low = 1'b1;
        #5us;
        check(!dut.core_res_n, "Apple reset did not reset the TURBO CPU");
        sh_write(18'h09000, 8'hD4);
        n = marker_count[0];
        @(negedge clk);
        res_drive_low = 1'b0;
        wait_marker(0, n + 3);
        check(marker_value[0] == 8'hD4, "Apple reset resumed with stale cached data");
        freeze_core();
    endtask

    task automatic bank_program;
        begin_program(2'd3);
        sh_write(18'h09000, 8'h11);
        sh_write(18'h19000, 8'h22);
        sh_write(18'h00020, 8'h33);
        sh_write(18'h10020, 8'h44);
        absolute(8'hAD, 16'h9000);
        absolute(8'h8D, 16'hA100);
        absolute(8'h8D, 16'hC003);     // RAMRD on
        absolute(8'hAD, 16'h9000);
        absolute(8'h8D, 16'hA101);
        absolute(8'h8D, 16'hC002);     // RAMRD off
        absolute(8'hAD, 16'h9000);
        absolute(8'h8D, 16'hA102);
        immediate(8'hA5, 8'h20);
        absolute(8'h8D, 16'hA103);
        absolute(8'h8D, 16'hC009);     // ALTZP on
        immediate(8'hA5, 8'h20);
        absolute(8'h8D, 16'hA104);
        absolute(8'h8D, 16'hC008);     // ALTZP off
        immediate(8'hA5, 8'h20);
        absolute(8'h8D, 16'hA105);
        // Read and write maps differ: write aux, continue reading main.
        absolute(8'h8D, 16'hC005);     // RAMWRT on
        immediate(8'hA9, 8'h66);
        absolute(8'h8D, 16'h9000);
        absolute(8'h8D, 16'hC004);     // RAMWRT off
        absolute(8'hAD, 16'h9000);
        absolute(8'h8D, 16'hA106);
        absolute(8'h8D, 16'hC003);
        absolute(8'hAD, 16'h9000);
        absolute(8'h8D, 16'hA107);
        absolute(8'h8D, 16'hC002);
        immediate(8'hA9, 8'h77);
        absolute(8'h8D, 16'h9000);
        absolute(8'hAD, 16'h9000);
        absolute(8'h8D, 16'hA108);
        halt_loop();
        start_program();
        wait_marker(8, 1);
        freeze_core();
        check(marker_value[0] == 8'h11 && marker_value[1] == 8'h22 &&
              marker_value[2] == 8'h11, "RAMRD bank changes reused an old map/data");
        check(marker_value[3] == 8'h33 && marker_value[4] == 8'h44 &&
              marker_value[5] == 8'h33, "ALTZP bank changes reused an old map/data");
        check(marker_value[6] == 8'h11 && marker_value[7] == 8'h66 &&
              marker_value[8] == 8'h77, "independent read/write mapping lost a store");
        sh_check(18'h09000, 8'h77);
        sh_check(18'h19000, 8'h66);
    endtask

    task automatic selfmod_program;
        integer n;
        begin_program(2'd3);
        absolute(8'h4C, 16'h6000);
        // LDA #$11 / STA $A100 / LDA #$22 / STA $6001 / JMP $6000.
        sh_write(18'h06000, 8'hA9); sh_write(18'h06001, 8'h11);
        sh_write(18'h06002, 8'h8D); sh_write(18'h06003, 8'h00);
        sh_write(18'h06004, 8'hA1); sh_write(18'h06005, 8'hA9);
        sh_write(18'h06006, 8'h22); sh_write(18'h06007, 8'h8D);
        sh_write(18'h06008, 8'h01); sh_write(18'h06009, 8'h60);
        sh_write(18'h0600A, 8'h4C); sh_write(18'h0600B, 8'h00);
        sh_write(18'h0600C, 8'h60);
        start_program();
        wait_marker(0, 1);
        check(marker_value[0] == 8'h11, "initial RAM instruction operand was wrong");
        wait_marker(0, 4);
        check(marker_value[0] == 8'h22, "CPU self-modifying write left stale instruction data");
        freeze_core();
        sh_check(18'h06001, 8'h22);
        // Change a second instruction operand through the host as well.
        sh_write(18'h06006, 8'h83);
        n = marker_count[0];
        @(negedge clk);
        pause = 1'b0;
        wait_marker(0, n + 4);
        check(marker_value[0] == 8'h83, "ARM code write left a stale instruction operand");
        freeze_core();
    endtask

    task automatic physical_program;
        integer before_io;
        begin_program(2'd3);
        for (int i = 0; i < 4; i++) begin
            absolute(8'hAD, 16'hC020);
            absolute(8'h8D, 16'hA100 + 16'(i));
        end
        // Indexed writes must preserve every video byte in FIFO order.
        immediate(8'hA2, 8'h00);
        emit(8'h8A);                 // TXA
        absolute(8'h9D, 16'h0400);    // STA $0400,X
        emit(8'hE8);
        immediate(8'hE0, 8'h10);
        immediate(8'hD0, 8'hF7);
        immediate(8'hA9, 8'h99);
        absolute(8'h8D, 16'hA10F);
        halt_loop();
        start_program();
        wait_marker(15, 1);
        freeze_core();
        check(physical_io_reads == 4, "I/O read was cached, omitted or duplicated");
        check(physical_wait_clocks >= 100,
              "physical I/O did not incur real Apple bus waits");
        for (int i = 0; i < 4; i++)
            check(marker_count[i] == 1 && marker_value[i] == 8'hEE,
                  "physical I/O response did not reach its CPU load exactly once");
        // The producer may finish before the physical queue drains.
        repeat (5000) @(posedge clk);
        #1ps;
        check(physical_video_writes == 16 && post_fill == 0 &&
              cnt_posted_writes == 16 && cnt_post_drops == 0,
              $sformatf("posted writes lost/duplicated: physical=%0d queued=%0d count=%0d drops=%0d",
                        physical_video_writes, post_fill, cnt_posted_writes, cnt_post_drops));
        for (int i = 0; i < 16; i++) begin
            sh_check(18'h00400 + 18'(i), 8'(i));
            check(mb_ram[16'h0400 + i] === 8'(i),
                  "motherboard video copy did not match shadow");
        end
    endtask

    task automatic emit_video_fill(input logic [7:0] value, input integer pages);
        immediate(8'hA9, value);
        immediate(8'hA2, 8'h00);
        for (int page = 0; page < pages; page++)
            absolute(8'h9D, 16'h2000 + 16'(page * 256));
        emit(8'hE8);
        immediate(8'hD0, 8'(-(3 * pages + 3)));
    endtask

    task automatic emit_hgr_mode;
        absolute(8'h8D, 16'hC050);     // Graphics, full screen, HGR page 1.
        absolute(8'h8D, 16'hC052);
        absolute(8'h8D, 16'hC054);
        absolute(8'h8D, 16'hC057);
    endtask

    task automatic await_video_drain;
        integer guard;
        guard = 0;
        while (!dut.video_all_drained && guard < 400000) begin
            @(posedge clk); #1ps;
            guard++;
        end
        check(dut.video_all_drained && cnt_post_drops == 0,
              "TURBO motherboard mirror failed to drain without drops");
    endtask

    // Exercise the first direct write with an empty, ready coalescer. A
    // later write could hide an early hold acknowledgement behind old data.
    task automatic direct_video_admission(input integer kind);
        integer guard;
        integer stopped_cycles;
        logic [15:0] target;
        begin_program(2'd3);
        video_record_enable = 1'b1;
        target = kind >= 7 ? 16'h4000 : 16'h2000;
        post_main_wide = kind == 7;
        overlay_capture_armed = kind == 8;
        mb_ram[target] = 8'h11;
        sh_write({2'b00, target}, 8'h11);
        emit_hgr_mode();
        immediate(8'hA9, 8'h5A);
        absolute(8'h8D, target);
        absolute(8'h8D, 16'hA100);
        halt_loop();

        // Finish the coalescer clear before starting the CPU, so its first
        // video write measures the mandatory stage without startup stalls.
        enable = 1'b1;
        #10us;
        @(negedge clk); res_drive_low = 1'b0;
        guard = 0;
        while (!(ab_read.res && dut.video_coalesce_ready) && guard < 200000) begin
            @(negedge clk);
            guard++;
        end
        check(guard < 200000, "admission test coalescer failed to become ready");
        core_run = 1'b1;
        guard = 0;
        while (!(dut.xstate_q == dut.X_ROUTE &&
                 dut.cycle_addr_q == target && !dut.cycle_rw_q) && guard < 200000) begin
            @(negedge clk);
            guard++;
        end
        check(guard < 200000 && !dut.video_mirror_pending &&
              !video_record_valid && direct_video_writes == 0 &&
              dut.video_start_ready && dut.video_coalesce_ready,
              "first direct write did not enter an empty admission stage");
        if (kind == 1) begin
            arm_rw_flush_req = 1'b1;
            video_record_ready = 1'b0;
        end
        // These controls affect the policy but must use the capture edge,
        // even when they change immediately before its new route stage.
        if (kind == 7) post_main_wide = 1'b0;
        if (kind == 8) overlay_capture_armed = 1'b0;
        @(posedge clk); #1ps;
        check(dut.xstate_q == dut.X_POST_STALL && direct_video_writes == 0 &&
              dut.cycle_video_mirror_active_q,
              "direct write skipped its route stage or lost captured policy");
        @(negedge clk);
        arm_rw_flush_req = 1'b0;
        stopped_cycles = cnt_core_cycles;
        case (kind)
            1: begin
                repeat (12) begin
                    @(posedge clk); #1ps;
                    check(!arm_rw_flush_done && arm_rw_hold_state &&
                          !dut.video_mirror_pending && direct_video_writes == 0 &&
                          dut.xstate_q == dut.X_POST_STALL &&
                          dut.cycle_addr_q == target && dut.cycle_wdata_q == 8'h5A &&
                          video_record_addr == {1'b0, target} &&
                          video_record_data == 8'h5A,
                          "first-write hold completed early or changed the saved tuple");
                end
                @(negedge clk); video_record_ready = 1'b1;
            end
            2: pause = 1'b1;
            3: speed_mode = 2'd0;
            4: enable = 1'b0;
            5: core_run = 1'b0;
            6: begin
                // The wrapper filters physical reset; block acceptance
                // until reset reaches the DUT's qualified input.
                video_record_ready = 1'b0;
                res_drive_low = 1'b1;
                guard = 0;
                while (ab_read.res && guard < 200000) begin
                    @(negedge clk);
                    guard++;
                end
                check(!ab_read.res, "admission reset did not reach the core");
                video_record_ready = 1'b1;
            end
            default: begin end
        endcase
        @(posedge clk); #1ps;
        if (kind >= 4 && kind <= 6) begin
            repeat (16) @(posedge clk);
            #1ps;
            check(!video_record_valid && direct_video_writes == 0 &&
                  !dut.video_mirror_pending && marker_count[0] == 0 &&
                  mb_ram[target] === 8'h11,
                  "aborted admission emitted or completed an unaccepted write");
        end else begin
            check(direct_video_writes == (kind == 3 ? 0 : 1),
                  "ready direct write did not accept exactly one edge after route");
            if (kind == 1) begin
                check(!arm_rw_flush_done && arm_rw_hold_state,
                      "ARM hold acknowledged on the first video acceptance edge");
                guard = 0;
                while (!arm_rw_flush_done && guard < 200000) begin
                    @(posedge clk); #1ps;
                    check(cnt_core_cycles == stopped_cycles,
                          "CPU advanced while its first video write was held");
                    guard++;
                end
                check(arm_rw_flush_done && arm_rw_hold_state && dut.video_all_drained &&
                      mb_ram[target] === 8'h5A && direct_video_writes == 1,
                      "first-write hold completed before physical drain");
                @(negedge clk); arm_rw_hold_release = 1'b1;
                @(negedge clk); arm_rw_hold_release = 1'b0;
            end else if (kind == 2) begin
                repeat (20) @(posedge clk);
                #1ps;
                check(cnt_core_cycles == stopped_cycles && direct_video_writes == 1 &&
                      marker_count[0] == 0,
                      "pause lost the staged write or advanced the CPU");
                @(negedge clk); pause = 1'b0;
            end
            wait_marker(0, 1);
            await_video_drain();
            check(marker_count[0] == 1 && mb_ram[target] === 8'h5A &&
                  cnt_posted_writes == 1 &&
                  direct_video_writes == (kind == 3 ? 0 : 1),
                  "admission exit omitted, duplicated or misrouted the video write");
        end
        // X_ROUTE has already committed the shadow on every path,
        // including aborts before the separate renderer/mirror acceptance.
        sh_check({2'b00, target}, 8'h5A);
        $display("VTW TURBO DIRECT ADMISSION PASS: kind=%0d", kind);
    endtask

    task automatic direct_video_banks;
        integer stalled_count;
        begin_program(2'd3);
        video_record_enable = 1'b1;
        video_record_ready = 1'b0;
        check_video_banks = 1'b1;
        emit_hgr_mode();
        emit_video_fill(8'h5A, 4);
        absolute(8'h8D, 16'hA100);
        emit_video_fill(8'hC3, 4);
        absolute(8'h8D, 16'hA101);
        absolute(8'h8D, 16'hC005);
        emit_video_fill(8'hA5, 2);
        absolute(8'h8D, 16'hC004);
        absolute(8'h8D, 16'hA10F);
        halt_loop();
        start_program();
        wait (video_record_valid);
        repeat (40) @(posedge clk);
        check(direct_video_writes == 0 && cnt_posted_writes == 0,
              "renderer backpressure allowed a partial video commit");
        @(negedge clk); video_record_ready = 1'b1;
        wait_marker(0, 1);
        check(direct_video_writes == 1024 && cnt_posted_writes < 512,
              "1024 direct bytes did not outrun the physical posted queue");
        $display("VTW TURBO VIDEO: 1024 direct writes completed with %0d physical writes", cnt_posted_writes);
        @(negedge clk); video_record_ready = 1'b0;
        wait (video_record_valid);
        stalled_count = direct_video_writes;
        repeat (40) @(posedge clk);
        check(direct_video_writes == stalled_count,
              "stalled renderer accepted another video byte");
        @(negedge clk); video_record_ready = 1'b1;
        wait_marker(15, 1, 600000);
        await_video_drain();
        check(direct_video_writes == 2560,
              "direct video stream lost or duplicated repeated/banked writes");
        for (int i = 0; i < 1024; i++)
            check(renderer_shadow[17'h02000+i] === 8'hC3 &&
                  mb_ram[16'h2000+i] === 8'hC3,
                  "latest repeated MAIN video value was not preserved");
        for (int i = 0; i < 512; i++)
            check(renderer_shadow[17'h12000+i] === 8'hA5 &&
                  mb_aux[16'h2000+i] === 8'hA5,
                  "AUX direct record or motherboard bank mapping was wrong");
        $display("VTW TURBO VIDEO BANKS/BACKPRESSURE PASS");
    endtask

    task automatic direct_video_exit(input bit handback);
        integer stopped_cycles;
        begin_program(2'd3);
        video_record_enable = 1'b1;
        emit_hgr_mode();
        emit_video_fill(8'hB6, 4);
        absolute(8'h8D, 16'hA100);
        halt_loop();
        start_program();
        wait_marker(0, 1);
        check(dut.video_mirror_pending, "video exit test had no pending mirror");
        @(negedge clk);
        if (handback) enable = 1'b0;
        else speed_mode = 2'd0;
        repeat (20) @(posedge clk);
        stopped_cycles = cnt_core_cycles;
        repeat (150) begin
            @(posedge clk); #1ps;
            check(bus_owned && apple_dma_pin === 1'b0,
                  "physical bus released before video mirror drained");
            check(cnt_core_cycles == stopped_cycles,
                  "CPU advanced across pending video mode-exit barrier");
        end
        await_video_drain();
        for (int i = 0; i < 1024; i++)
            check(mb_ram[16'h2000+i] === 8'hB6,
                  "mode exit/handback left stale physical video RAM");
        repeat (1000) @(posedge clk);
        if (handback)
            check(!bus_owned && apple_dma_pin === 1'b1,
                  "clean video handback did not release DMA");
        else
            check(cnt_core_cycles > stopped_cycles,
                  "CPU did not resume after video speed-exit drain");
        $display("VTW TURBO VIDEO EXIT PASS: handback=%0d", handback);
    endtask

    task automatic direct_video_abort(input bit stop_core);
        begin_program(2'd3);
        video_record_enable = 1'b1;
        emit_hgr_mode();
        emit_video_fill(8'h5A, 1);
        absolute(8'h8D, 16'hA100);
        immediate(8'hA9, 8'hC3);
        absolute(8'h8D, 16'h2100);
        absolute(8'h8D, 16'hA10F);
        halt_loop();
        start_program();
        wait_marker(0, 1);
        @(negedge clk); video_record_ready = 1'b0;
        wait (video_record_valid);
        check(video_record_addr == 17'h02100 && direct_video_writes == 256,
              "abort test did not park the next direct byte");
        @(negedge clk);
        if (stop_core) core_run = 1'b0;
        else enable = 1'b0;
        video_record_ready = 1'b1;
        repeat (20) @(posedge clk);
        check(direct_video_writes == 256,
              "disabled core accepted a previously blocked direct byte");
        await_video_drain();
        for (int i = 0; i < 256; i++)
            check(mb_ram[16'h2000+i] === 8'h5A,
                  "abort lost an already accepted video byte");
        $display("VTW TURBO VIDEO ABORT PASS: core_run=%0d", stop_core);
    endtask

    task automatic deferred_video_case(input integer exit_kind);
        integer stopped_cycles;
        integer guard;
        begin_program(2'd3);
        video_record_enable = 1'b1;
        video_record_ready = 1'b0;
        mb_ram[16'h08DF] = 8'h11;
        mb_aux[16'h08DF] = 8'h22;
        renderer_shadow[17'h008DF] = 8'h11;
        renderer_shadow[17'h108DF] = 8'h22;
        emit_hgr_mode();
        absolute(8'h8D, 16'hC00D);     // 80-column and double-hires on.
        absolute(8'h8D, 16'hC05E);
        immediate(8'hA9, 8'h3C);
        absolute(8'h8D, 16'h08DF);
        absolute(8'h8D, 16'hC005);
        immediate(8'hA9, 8'hA7);
        absolute(8'h8D, 16'h08DF);
        absolute(8'hAD, 16'hC020);     // Ordinary I/O must leave these deferred.
        absolute(8'h8D, 16'hA100);
        absolute(8'h8D, 16'hC004);
        immediate(8'hA9, 8'h5E);
        absolute(8'h8D, 16'h08DF);     // Revisit MAIN after the AUX bank switch.
        absolute(8'hAD, 16'hC020);
        absolute(8'h8D, 16'hA101);
        if (exit_kind == 0) begin
            absolute(8'h8D, 16'hC055);
            absolute(8'h8D, 16'hC051); // Expose text page 2.
            absolute(8'h8D, 16'hA10F);
        end
        halt_loop();
        start_program();
        wait (video_record_valid);
        repeat (40) @(posedge clk);
        check(video_record_addr == 17'h008DF && direct_video_writes == 0 &&
              cnt_posted_writes == 0 && renderer_shadow[17'h008DF] === 8'h11,
              "inactive renderer backpressure allowed a partial write commit");
        @(negedge clk); video_record_ready = 1'b1;
        wait_marker(0, 1);
        check(renderer_shadow[17'h008DF] === 8'h3C &&
              renderer_shadow[17'h108DF] === 8'hA7,
              "inactive video write left the renderer stale or confused banks");
        check(mb_ram[16'h08DF] === 8'h11 && mb_aux[16'h08DF] === 8'h22 &&
              cnt_posted_writes == 0,
              "ordinary I/O or RAMWRT drained inactive DHGR text memory");
        wait_marker(1, 1);
        check(renderer_shadow[17'h008DF] === 8'h5E &&
              renderer_shadow[17'h108DF] === 8'hA7 && direct_video_writes == 3,
              "repeated inactive writes lost their bank or renderer record");
        check(mb_ram[16'h08DF] === 8'h11 && mb_aux[16'h08DF] === 8'h22 &&
              cnt_posted_writes == 0,
              "second ordinary I/O drained deferred video bytes");
        if (exit_kind == 0) begin
            check_deferred_page_flip = 1'b1;
            wait_marker(15, 1);
            check(mb_page2 && !mb_aux_write,
                  "display flip left a temporary mirror bank selected");
        end else begin
            @(negedge clk);
            case (exit_kind)
                1: speed_mode = 2'd0;
                2: enable = 1'b0;
                3: arm_rw_flush_req = 1'b1;
                4: core_run = 1'b0;
            endcase
            @(negedge clk);
            arm_rw_flush_req = 1'b0;
            if (exit_kind == 3) begin
                guard = 0;
                while (!arm_rw_flush_done && guard < 400000) begin
                    @(posedge clk); #1ps;
                    guard++;
                end
                check(arm_rw_flush_done && arm_rw_hold_state,
                      "ARM hold did not wait for both deferred video banks");
            end
            await_video_drain();
            check(!mb_page2 && !mb_aux_write,
                  "deferred flush failed to restore motherboard bank switches");
            if (exit_kind == 3) begin
                stopped_cycles = cnt_core_cycles;
                repeat (100) @(posedge clk);
                check(cnt_core_cycles == stopped_cycles,
                      "ARM video flush acknowledged without holding the CPU");
            end
        end
        check(mb_ram[16'h08DF] === 8'h5E && mb_aux[16'h08DF] === 8'hA7,
              "deferred full flush lost the latest MAIN/AUX values");
        if (exit_kind == 2) begin
            repeat (1000) @(posedge clk);
            check(!bus_owned && apple_dma_pin === 1'b1,
                  "deferred handback failed to release the physical CPU");
        end
        $display("VTW TURBO DEFERRED VIDEO PASS: exit=%0d", exit_kind);
    endtask

    task automatic deferred_80store_hold;
        integer guard;
        begin_program(2'd3);
        video_record_enable = 1'b1;
        mb_ram[16'h08DF] = 8'h11;
        mb_aux[16'h08DF] = 8'h22;
        mb_ram[16'h0480] = 8'h33;
        mb_aux[16'h0480] = 8'h44;
        emit_hgr_mode();
        absolute(8'h8D, 16'hC001);
        absolute(8'h8D, 16'hC055);
        immediate(8'hA9, 8'h3C);
        absolute(8'h8D, 16'h08DF);
        absolute(8'h8D, 16'hC005);
        immediate(8'hA9, 8'hA7);
        absolute(8'h8D, 16'h08DF);
        immediate(8'hA9, 8'hB6);
        absolute(8'h8D, 16'h0480);     // 80STORE/PAGE2 choose AUX independently.
        absolute(8'hAD, 16'hC020);
        absolute(8'h8D, 16'hA100);
        halt_loop();
        start_program();
        wait_marker(0, 1);
        check(mb_ram[16'h08DF] === 8'h11 && mb_aux[16'h08DF] === 8'h22 &&
              mb_aux[16'h0480] === 8'h44 && cnt_posted_writes == 0,
              "80STORE inactive writes drained before an ownership barrier");
        check(renderer_shadow[17'h008DF] === 8'h3C &&
              renderer_shadow[17'h108DF] === 8'hA7 &&
              renderer_shadow[17'h10480] === 8'hB6,
              "80STORE direct records did not retain their translated banks");
        @(negedge clk); arm_rw_flush_req = 1'b1;
        @(negedge clk); arm_rw_flush_req = 1'b0;
        guard = 0;
        while (!arm_rw_flush_done && guard < 400000) begin
            @(posedge clk); #1ps;
            guard++;
        end
        check(arm_rw_flush_done && arm_rw_hold_state &&
              mb_80store && mb_page2 && mb_aux_write &&
              dut.vsss.sw_page2 && dut.vsss.sw_ramwrt,
              "80STORE flush changed physical or virtual bank-switch state");
        check(mb_ram[16'h08DF] === 8'h3C && mb_aux[16'h08DF] === 8'hA7 &&
              mb_ram[16'h0480] === 8'h33 && mb_aux[16'h0480] === 8'hB6,
              "80STORE flush wrote deferred data into the wrong physical bank");
        $display("VTW TURBO DEFERRED 80STORE/ARM HOLD PASS");
    endtask

    task automatic deferred_display_flip(input integer kind);
        logic [15:0] address;
        logic [15:0] barrier_address;
        begin_program(2'd3);
        video_record_enable = 1'b1;
        case (kind)
            0: begin address = 16'h4000; barrier_address = 16'hC055; end
            1: begin address = 16'h0650; barrier_address = 16'hC053; end
            2: begin address = 16'h08DF; barrier_address = 16'hC700; end
        endcase
        mb_ram[address] = 8'h11;
        emit_hgr_mode();
        immediate(8'hA9, 8'hA6);
        absolute(8'h8D, address);
        absolute(8'hAD, 16'hC020);
        absolute(8'h8D, 16'hA100);
        absolute(kind == 2 ? 8'hAD : 8'h8D, barrier_address);
        absolute(8'h8D, 16'hA10F);
        halt_loop();
        start_program();
        wait_marker(0, 1);
        check(mb_ram[address] === 8'h11 &&
              renderer_shadow[{1'b0,address}] === 8'hA6 && cnt_posted_writes == 0,
              "inactive page/text write waited for the motherboard mirror");
        checked_display_switch = barrier_address;
        checked_display_byte = address;
        check_display_flip = 1'b1;
        wait_marker(15, 1);
        check(mb_ram[address] === 8'hA6 && checked_display_cycles == 1,
              "display or card access exposed stale deferred memory");
        $display("VTW TURBO DEFERRED DISPLAY/CARD BARRIER PASS: kind=%0d", kind);
    endtask

    task automatic classic_video_case(input logic [1:0] mode);
        begin_program(mode);
        video_record_enable = 1'b1;
        mb_ram[16'h08DF] = 8'h11;
        mb_aux[16'h08DF] = 8'h22;
        emit_hgr_mode();
        immediate(8'hA9, 8'h3C);
        absolute(8'h8D, 16'h08DF);
        absolute(8'h8D, 16'hC005);
        immediate(8'hA9, 8'hA7);
        absolute(8'h8D, 16'h08DF);
        absolute(8'h8D, 16'hC004);
        absolute(8'hAD, 16'hC020);
        absolute(8'h8D, 16'hA100);
        halt_loop();
        start_program();
        wait_marker(0, 1);
        check(mb_ram[16'h08DF] === 8'h3C && mb_aux[16'h08DF] === 8'hA7 &&
              direct_video_writes == 0 && cnt_posted_writes == 2,
              "classic speed mode gained TURBO-only deferred mirroring");
        $display("VTW CLASSIC VIDEO UNCHANGED PASS: speed=%0d", mode);
    endtask

    task automatic deferred_async_hold(input integer request_point);
        integer guard;
        integer stopped_cycles;
        bit found;
        begin_program(2'd3);
        video_record_enable = 1'b1;
        mb_ram[16'h08DF] = 8'h11;
        mb_aux[16'h08DF] = 8'h22;
        sh_write(18'h09000, 8'h66);
        emit_hgr_mode();
        immediate(8'hA9, 8'h3C);
        absolute(8'h8D, 16'h08DF);
        absolute(8'h8D, 16'hC005);
        immediate(8'hA9, 8'hA7);
        absolute(8'h8D, 16'h08DF);
        absolute(8'h8D, 16'hC004);
        absolute(8'h8D, 16'hA100);
        if (request_point == 3) begin
            absolute(8'hAD, 16'h9000); // Warm the exact address before parking a hit.
            absolute(8'h8D, 16'hA102);
        end
        if (request_point != 4) begin
            absolute(8'hAD, request_point < 2 || request_point == 5 ? 16'hC020 : 16'h9000);
            absolute(8'h8D, 16'hA101);
        end
        halt_loop();
        start_program();
        wait_marker(0, 1);
        check(mb_ram[16'h08DF] === 8'h11 && mb_aux[16'h08DF] === 8'h22,
              "asynchronous hold test had no deferred bank data");
        found = request_point == 4;
        guard = 0;
        while (!found && guard < 200000) begin
            @(negedge clk);
            case (request_point)
                0: found = dut.xstate_q == dut.X_BUS &&
                           dut.core_addr == 16'hC020 && !dut.req_inflight_q;
                1: found = dut.xstate_q == dut.X_BUS &&
                           dut.core_addr == 16'hC020 && dut.req_inflight_q;
                2: found = dut.xstate_q == dut.X_MEM_DONE &&
                           dut.core_addr == 16'h9000;
                3: found = dut.xstate_q == dut.X_TURBO_DONE && dut.turbo_hit &&
                           dut.core_addr == 16'h9000 && marker_count[2] == 1;
                5: found = dut.xstate_q == dut.X_VIDEO_WAIT &&
                           dut.cycle_addr_q == 16'hC020;
            endcase
            guard++;
        end
        check(found, $sformatf("did not reach ARM request point %0d", request_point));
        if (request_point == 4) @(negedge clk);
        pause = 1'b1;
        if (request_point == 4) begin
            core_run = 1'b0;
            arm_req_valid = 1'b1;
            @(posedge clk); #1ps;
            check(arm_req_busy, "ARM test request was not accepted into its latch");
        end else begin
            arm_rw_flush_req = 1'b1;
            if (request_point == 3) begin
                sh_en = 1'b1;
                sh_we = 1'b1;
                sh_addr = 18'h09000;
                sh_wdata = 8'hD2;
            end
        end
        @(negedge clk);
        arm_rw_flush_req = 1'b0;
        arm_req_valid = 1'b0;
        sh_en = 1'b0;
        sh_we = 1'b0;
        stopped_cycles = cnt_core_cycles;
        guard = 0;
        while ((request_point == 4 ? arm_responses == 0 : !arm_rw_flush_done) &&
               guard < 200000) begin
            @(posedge clk); #1ps;
            check(cnt_core_cycles == stopped_cycles,
                  "CPU advanced during an asynchronous video hold");
            guard++;
        end
        check(request_point == 4 ? arm_responses == 1 :
              (arm_rw_flush_done && arm_rw_hold_state),
              $sformatf("ARM request point %0d deadlocked: state=%0d pending=%0b engine=%0b/%0b video=%0b sync=%0b io=%0d response=%0d",
                        request_point, dut.xstate_q, arm_req_busy,
                        dut.eng_req_valid, dut.eng_req_ready,
                        dut.video_mirror_pending, dut.video_sync_active,
                        physical_io_reads, arm_responses));
        check(mb_ram[16'h08DF] === 8'h3C && mb_aux[16'h08DF] === 8'hA7 &&
              !mb_aux_write && !mb_page2 && !dut.vsss.sw_ramwrt &&
              !dut.vsss.sw_page2 && dut.video_all_drained,
              "asynchronous video flush lost data or failed to restore banking");
        if (request_point != 4) begin
            check(arm_responses == 0, "maintenance invented an ARM bus response");
            @(negedge clk);
            arm_rw_hold_release = 1'b1;
            pause = 1'b0;
            @(negedge clk); arm_rw_hold_release = 1'b0;
            wait_marker(1, 1);
            check(marker_count[1] == 1 && marker_value[1] ===
                  (request_point < 2 || request_point == 5 ? 8'hEE :
                   request_point == 3 ? 8'hD2 : 8'h66),
                  "held access lost its response or used pre-invalidation data");
            check(physical_io_reads == (request_point < 2 || request_point == 5 ? 1 : 0),
                  "asynchronous hold omitted or duplicated physical I/O");
        end
        $display("VTW TURBO ASYNC VIDEO HOLD PASS: point=%0d", request_point);
    endtask

    task automatic deferred_video_wait_exit(input bit stop_core);
        integer guard;
        begin_program(2'd3);
        video_record_enable = 1'b1;
        mb_ram[16'h08DF] = 8'h11;
        mb_aux[16'h08DF] = 8'h22;
        emit_hgr_mode();
        immediate(8'hA9, 8'h5E);
        absolute(8'h8D, 16'h08DF);
        absolute(8'h8D, 16'hC005);
        immediate(8'hA9, 8'hA7);
        absolute(8'h8D, 16'h08DF);
        absolute(8'h8D, 16'hC004);
        absolute(8'h8D, 16'hA100);
        absolute(8'h8D, 16'hC055);
        absolute(8'h8D, 16'hA101);
        halt_loop();
        start_program();
        wait_marker(0, 1);
        check_deferred_page_flip = 1'b1;
        guard = 0;
        while (!(dut.xstate_q == dut.X_VIDEO_WAIT &&
                 dut.cycle_addr_q == 16'hC055) && guard < 200000) begin
            @(negedge clk);
            guard++;
        end
        check(guard < 200000 && !dut.vsss.sw_page2 && !mb_page2 &&
              mb_ram[16'h08DF] === 8'h11 && mb_aux[16'h08DF] === 8'h22,
              "video wait did not precede private and physical PAGE2 side effects");
        if (stop_core) core_run = 1'b0;
        else speed_mode = 2'd0;
        await_video_drain();
        check(mb_ram[16'h08DF] === 8'h5E && mb_aux[16'h08DF] === 8'hA7 &&
              !mb_aux_write,
              "video wait exit lost deferred banks or failed to restore RAMWRT");
        if (stop_core) begin
            repeat (100) @(posedge clk);
            check(!dut.vsss.sw_page2 && !mb_page2 && marker_count[1] == 0,
                  "video wait abort performed an unaccepted PAGE2 access");
        end else begin
            wait_marker(1, 1);
            check(dut.vsss.sw_page2 && mb_page2,
                  "video wait speed exit lost the saved PAGE2 access");
        end
        $display("VTW TURBO VIDEO WAIT EXIT PASS: stop_core=%0d", stop_core);
    endtask

    task automatic deferred_bank_abort(input bit target_aux);
        integer guard;
        logic [15:0] bank_switch;
        begin_program(2'd3);
        video_record_enable = 1'b1;
        bank_switch = target_aux ? 16'hC005 : 16'hC004;
        mb_ram[16'h08DF] = 8'h11;
        mb_aux[16'h08DF] = 8'h22;
        emit_hgr_mode();
        immediate(8'hA9, 8'h3C);
        absolute(8'h8D, 16'h08DF);
        absolute(8'h8D, 16'hC005);
        immediate(8'hA9, 8'hA7);
        absolute(8'h8D, 16'h08DF);
        if (target_aux) absolute(8'h8D, 16'hC004);
        absolute(8'h8D, 16'hA100);
        absolute(8'h8D, bank_switch);
        halt_loop();
        start_program();
        wait_marker(0, 1);
        guard = 0;
        while (!(dut.xstate_q == dut.X_BUS && dut.core_addr == bank_switch &&
                 !dut.req_inflight_q) && guard < 200000) begin
            @(negedge clk);
            guard++;
        end
        check(guard < 200000 && mb_aux_write == !target_aux &&
              dut.vsss.sw_ramwrt == target_aux,
              "bank abort did not separate private and physical switch state");
        core_run = 1'b0;
        await_video_drain();
        check(mb_ram[16'h08DF] === 8'h3C && mb_aux[16'h08DF] === 8'hA7 &&
              mb_aux_write == !target_aux && !mb_page2,
              "aborted bank switch replayed bytes using unaccepted private state");
        $display("VTW TURBO DEFERRED BANK ABORT PASS: target_aux=%0d", target_aux);
    endtask

    task automatic iiplus_video_case;
        begin_program(2'd3);
        host_is_iiplus = 1'b1;
        video_record_enable = 1'b1;
        mb_ram[16'h08DF] = 8'h11;
        emit_hgr_mode();
        immediate(8'hA9, 8'h3C);
        absolute(8'h8D, 16'h08DF);
        absolute(8'hAD, 16'hC020);
        absolute(8'h8D, 16'hA100);
        halt_loop();
        start_program();
        wait_marker(0, 1);
        check(mb_ram[16'h08DF] === 8'h3C && cnt_posted_writes == 1 &&
              direct_video_writes == 1 && dut.video_all_drained,
              "II+ TURBO deferred an inactive byte despite its physical card mappings");
        $display("VTW TURBO II+ VIDEO IMMEDIATE PASS");
    endtask

    task automatic deferred_post_stall_exit(input integer kind);
        integer guard;
        begin_program(2'd3);
        video_record_enable = 1'b1;
        check_classic_fallback_write = kind != 2;
        mb_ram[16'h08DF] = 8'h11;
        mb_aux[16'h08DF] = 8'h44;
        mb_ram[16'h08E0] = 8'h22;
        emit_hgr_mode();
        if (kind == 2) absolute(8'h8D, 16'hC005);
        immediate(8'hA9, 8'h3C);
        absolute(8'h8D, 16'h08DF);
        if (kind == 2) absolute(8'h8D, 16'hC004);
        absolute(8'h8D, 16'hA100);
        immediate(8'hA9, 8'hA7);
        absolute(8'h8D, 16'h08E0);
        absolute(8'h8D, 16'hA101);
        halt_loop();
        start_program();
        wait_marker(0, 1);
        @(negedge clk); video_record_ready = 1'b0;
        guard = 0;
        while (!(video_record_valid && dut.xstate_q == dut.X_POST_STALL) &&
               guard < 200000) begin
            @(negedge clk);
            guard++;
        end
        check(guard < 200000 && video_record_addr == 17'h008E0 &&
              direct_video_writes == 1 && cnt_posted_writes == 0,
              "mode exit did not park an unaccepted video byte behind deferred data");
        if (kind == 1) video_record_enable = 1'b0;
        else speed_mode = 2'd0;
        if (kind == 2) begin
            // Re-enter after replay has actually selected AUX on the bus.
            // The waiting MAIN byte must not join that bank's scan.
            guard = 0;
            while (!(dut.video_sync_active && ab_read.data_en &&
                     !ab_read.rw && ab_read.addr == 16'hC005) && guard < 200000) begin
                @(posedge clk); #1ps;
                guard++;
            end
            check(guard < 200000, "re-entry test never reached AUX mirror steering");
            @(negedge clk);
            speed_mode = 2'd3;
            video_record_ready = 1'b1;
            while (dut.video_sync_active && guard < 400000) begin
                check(!video_record_valid && direct_video_writes == 1,
                      "TURBO re-entry accepted a byte during mirror bank restoration");
                @(posedge clk); #1ps;
                guard++;
            end
            check(!dut.video_sync_active && !mb_aux_write,
                  "TURBO re-entry prevented mirror bank restoration");
        end
        wait_marker(1, 1);
        if (kind == 2) begin
            check(direct_video_writes == 2 && mb_aux[16'h08DF] === 8'h3C &&
                  mb_ram[16'h08DF] === 8'h11 && mb_ram[16'h08E0] === 8'h22,
                  "TURBO re-entry lost, duplicated or misrouted the waiting record");
            @(negedge clk); core_run = 1'b0;
        end
        await_video_drain();
        check((kind == 2 ? mb_aux[16'h08DF] : mb_ram[16'h08DF]) === 8'h3C &&
              mb_ram[16'h08E0] === 8'hA7 &&
              direct_video_writes == (kind == 2 ? 2 : 1) && cnt_posted_writes == 2,
              "post-stall mode exit lost or duplicated a deferred/classic video write");
        sh_check(kind == 2 ? 18'h108DF : 18'h008DF, 8'h3C);
        sh_check(18'h008E0, 8'hA7);
        $display("VTW TURBO POST STALL EXIT/REENTRY PASS: kind=%0d", kind);
    endtask

    integer max_cold, max_hot, turbo_cold, turbo_hot;
    integer alt_max_cold, alt_max_hot, alt_turbo_cold, alt_turbo_hot;
    initial begin
        benchmark(2'd0, 16'h9000, max_cold, max_hot);
        benchmark(2'd3, 16'h9000, turbo_cold, turbo_hot);
        check(turbo_cold < max_cold, "cold TURBO program was not faster than MAX");
        check(turbo_hot * 3 < max_hot * 2,
              "hot TURBO copy failed to exceed 1.5 times MAX throughput");
        $display("VTW TURBO SPEED $9000: MAX cold/hot=%0d/%0d clocks; TURBO=%0d/%0d; hot ratio=%0.2f",
                 max_cold, max_hot, turbo_cold, turbo_hot,
                 real'(max_hot) / real'(turbo_hot));
        benchmark(2'd0, 16'h9080, alt_max_cold, alt_max_hot);
        benchmark(2'd3, 16'h9080, alt_turbo_cold, alt_turbo_hot);
        check(alt_turbo_cold < alt_max_cold &&
              alt_turbo_hot * 3 < alt_max_hot * 2,
              "alternate-layout TURBO copy did not exceed MAX throughput");
        $display("VTW TURBO SPEED $9080: MAX cold/hot=%0d/%0d clocks; TURBO=%0d/%0d; hot ratio=%0.2f",
                 alt_max_cold, alt_max_hot, alt_turbo_cold, alt_turbo_hot,
                 real'(alt_max_hot) / real'(alt_turbo_hot));
        coherency_program();
        bank_program();
        ramworks_program();
        selfmod_program();
        physical_program();
        for (int kind = 0; kind < 9; kind++)
            direct_video_admission(kind);
        direct_video_banks();
        direct_video_exit(1'b0);
        direct_video_exit(1'b1);
        direct_video_abort(1'b0);
        direct_video_abort(1'b1);
        for (int exit_kind = 0; exit_kind < 5; exit_kind++)
            deferred_video_case(exit_kind);
        deferred_80store_hold();
        for (int kind = 0; kind < 3; kind++)
            deferred_display_flip(kind);
        for (int mode = 0; mode < 3; mode++)
            classic_video_case(2'(mode));
        for (int point = 0; point < 6; point++)
            deferred_async_hold(point);
        deferred_video_wait_exit(1'b0);
        deferred_video_wait_exit(1'b1);
        deferred_bank_abort(1'b0);
        deferred_bank_abort(1'b1);
        iiplus_video_case();
        for (int kind = 0; kind < 3; kind++)
            deferred_post_stall_exit(kind);
        pending_write_abort(0);
        pending_write_abort(1);
        pending_write_abort(2);
        // Every case resets and reloads a formerly cached ROM address. The
        // final fresh benchmark also proves reset after I/O and posted work.
        benchmark(2'd3, 16'h9000, turbo_cold, turbo_hot);
        $display("VTW CAPTURE POLICY EQUIVALENCE PASS: %0d routed accesses", policy_checks);
        $display("VTW TURBO PASS");
        $finish;
    end

    initial begin
        #50ms;
        $fatal(1, "VTW TURBO FAIL: global timeout");
    end
endmodule
