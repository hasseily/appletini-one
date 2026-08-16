`timescale 1ns / 1ps

// Real-ROM ONE//e Disk II boot proof.
//
// The runner converts the firmware's embedded Enhanced //e CPU ROM array to
// onee_enhanced_cpu_rom.mem.  This bench boots that exact image on the vTW
// core over the isolated virtual motherboard bus. The production slot-6 ROM
// then reads a nibblized DOS or ProDOS track through disk2_card's shared AppleBus
// port. The private vTW Disk II shortcut stays off, as it does in ONE//e.
module tb_onee_disk2_boot;

    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;

    logic resetn = 1'b0;
    logic vtw_enable = 1'b0;
    logic core_run = 1'b0;

    globals::AppleBus_read ab_read;
    globals::AppleBus_read softswitch_ab_read;
    globals::AppleBus_write motherboard_write;
    globals::AppleBus_write disk2_write;
    globals::AppleBus_write vtw_write;
    globals::AppleBus_write [12:0] client_writes;
    globals::AppleBus_write merged_write;
    globals::SoftSwitchState sss;

    logic req_ready;
    logic resp_valid;
    logic [7:0] resp_rdata;

    // Use the production default cadence. A shortened virtual cycle makes
    // the free-running Disk II rotation advance too quickly between decoder
    // reads and cannot prove a production boot.
    apple_virtual_bus virtual_bus_i (
        .clk(clk),
        .resetn(resetn),
        .res_n_in(1'b1),
        .irq_n_in(1'b1),
        .nmi_n_in(1'b1),
        .rdy_n_in(1'b1),
        .dma_n_in(1'b1),
        .inh_n_in(1'b1),
        .req_valid(1'b0),
        .req_ready(req_ready),
        .req_addr(16'hFFFF),
        .req_rw(1'b1),
        .req_wdata(8'h00),
        .resp_valid(resp_valid),
        .resp_rdata(resp_rdata),
        .floating_bus_data(8'h00),
        .ab_write(merged_write),
        .ab_read(ab_read)
    );

    logic update_pulse;
    logic [8:0] line_in_frame;
    logic [6:0] cycle_in_line;
    wire video_vblank = (line_in_frame >= 9'd192);

    apple_timing_gen timing_i (
        .clk(clk),
        .resetn(resetn),
        .apple_bus_pulse(ab_read.sss_en),
        .video_mode_50hz(1'b0),
        .update_pulse(update_pulse),
        .line_in_frame(line_in_frame),
        .cycle_in_line(cycle_in_line),
        .set_frame_zero_pulse(1'b0),
        .set_vblank_start_pulse(1'b0)
    );

    logic keyboard_event_ready;
    onee_motherboard_io motherboard_i (
        .clk(clk),
        .resetn(resetn),
        .enabled(1'b1),
        .ab_read(ab_read),
        .sss(sss),
        .softswitch_ab_read(softswitch_ab_read),
        .ab_write(motherboard_write),
        .floating_bus_data(8'h00),
        .video_vblank(video_vblank),
        .keyboard_event_valid(1'b0),
        .keyboard_event_ready(keyboard_event_ready),
        .keyboard_event_code(7'h00),
        .keyboard_any_down(1'b0),
        .keyboard_modifiers_in(3'b000),
        .keyboard_modifiers_state(),
        .keyboard_latch(),
        .keyboard_strobe(),
        .pushbuttons(3'b000),
        .cassette_in(1'b0),
        .paddle_values(32'h8080_8080),
        .cassette_out(),
        .speaker(),
        .utility_strobe_pulse(),
        .annunciators(),
        .ioudis(),
        .paddle_active(),
        .paddle_trigger_pulse()
    );

    soft_switch_manager manager_i (
        .clk(clk),
        .rstn(resetn),
        .ramworks_en(1'b0),
        .ab_read(softswitch_ab_read),
        .sss(sss)
    );

    logic slot7_hidden;
    onee_cold_slot_scan cold_scan_i (
        .clk(clk),
        .resetn(resetn),
        .enabled(1'b1),
        .ab_read(ab_read),
        .slot7_hidden(slot7_hidden)
    );

    always_comb begin
        client_writes = '{default: '0};
        client_writes[3] = disk2_write;
        client_writes[11] = vtw_write;
        client_writes[12] = motherboard_write;
    end

    apple_bus_write_arbiter #(
        .NUM_CLIENTS(13),
        .FAST_DATA_CLIENT(2),
        .FAST_ADDR_CLIENT(11)
    ) arbiter_i (
        .inh_allowed(1'b1),
        .client_writes(client_writes),
        .ab_write(merged_write)
    );

    localparam logic [20:0] TRACK_BASE_LINE = 21'h0E0000;
    localparam int unsigned TRACK_STREAM_BYTES = 8192;

    globals::AxiSimple_common as_common;
    AxiSimple_if disk2_axi();
    logic [20:0] mc_line_addr;
    logic mc_rw;
    logic [63:0] mc_wdata;
    logic [7:0] mc_wstrb;
    logic mc_valid;
    logic mc_ready;
    logic [63:0] mc_rdata = 64'h0;
    logic mc_rvalid = 1'b0;
    logic [7:0] track_mem [0:TRACK_STREAM_BYTES-1];
    logic [7:0] expected_boot [0:3];
    logic mc_pending_q = 1'b0;
    logic [20:0] mc_pending_line_q = 21'd0;
    integer mc_read_count = 0;

    initial begin
        for (int i = 0; i < TRACK_STREAM_BYTES; ++i)
            track_mem[i] = 8'hFF;
        $readmemh("onee_disk_track0.mem", track_mem);
        $readmemh("onee_expected_boot.mem", expected_boot);
    end

    assign mc_ready = !mc_pending_q;
    always_ff @(posedge clk) begin
        mc_rvalid <= 1'b0;
        if (!resetn) begin
            mc_pending_q <= 1'b0;
            mc_pending_line_q <= 21'd0;
            mc_read_count <= 0;
        end else begin
            if (mc_valid && mc_ready) begin
                if (!mc_rw)
                    $fatal(1, "ONEE DISK BOOT FAIL: read-only boot wrote staged track");
                mc_pending_q <= 1'b1;
                mc_pending_line_q <= mc_line_addr;
                mc_read_count <= mc_read_count + 1;
            end
            if (mc_pending_q) begin
                automatic integer byte_base =
                    (integer'(mc_pending_line_q) - integer'(TRACK_BASE_LINE)) * 8;
                mc_rvalid <= 1'b1;
                for (int lane = 0; lane < 8; ++lane) begin
                    if (byte_base + lane >= 0 &&
                        byte_base + lane < TRACK_STREAM_BYTES)
                        mc_rdata[lane * 8 +: 8] <= track_mem[byte_base + lane];
                    else
                        mc_rdata[lane * 8 +: 8] <= 8'hFF;
                end
                mc_pending_q <= 1'b0;
            end
        end
    end

    disk2_card disk2_i (
        .clk(clk), .rstn(resetn), .ab_read(ab_read), .rom_serve_en(1'b0),
        .sss(sss), .slot_assign(3'd6), .as_common(as_common),
        .as_client(disk2_axi), .mc_line_addr(mc_line_addr), .mc_rw(mc_rw),
        .mc_wdata(mc_wdata), .mc_wstrb(mc_wstrb), .mc_valid(mc_valid),
        .mc_ready(mc_ready), .mc_rdata(mc_rdata), .mc_rvalid(mc_rvalid),
        .ab_write(disk2_write),
        .vtw_active(1'b0), .vtw_req_valid(1'b0), .vtw_req_addr(4'h0),
        .vtw_req_ready(), .vtw_resp_valid(), .vtw_resp_rdata(),
        .vtw_cycle_tick(1'b0), .vtw_native_cycle_active(1'b0),
        .vtw_time_ready(), .vtw_write_timing_active(),
        .sound_spinning(), .sound_qtrack(), .sound_event(),
        .sound_seek_start_qtrack(), .sound_seek_distance()
    );

    logic [15:0] dbg_core_pc;
    logic bus_owned;
    logic [31:0] core_cycles;
    logic [31:0] bus_cycles;

    vtw_core_top core_i (
        .clk(clk),
        .rstn(resetn),
        .enable(vtw_enable),
        .host_is_iiplus(1'b0),
        .virtual_motherboard(1'b1),
        .core_run(core_run),
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
        .d2_resp_rdata(8'h00),
        .d2_cycle_tick(),
        .d2_native_cycle_active(),
        .d2_time_ready(1'b1),
        .d2_write_timing_active(1'b0),
        .ramworks_en(1'b0),
        .video_vbl(video_vblank),
        .video_mode_50hz(1'b0),
        .video_line(line_in_frame),
        .video_cycle(cycle_in_line),
        .post_main_wide(1'b0),
        .overlay_capture_armed(1'b0),
        .overlay_capture_bank_aux(1'b0),
        .overlay_capture_base(16'h0000),
        .overlay_capture_limit(16'h0000),
        .ab_read(ab_read),
        .ab_write(vtw_write),
        .irq_assert_in(merged_write.assert_irq),
        .data_drive_in(merged_write.wr_data_en),
        .data_drive_value_in(merged_write.wr_data),
        .dbg_clear(1'b0),
        .iiplus_buttons_zero(1'b0),
        .rw_req_valid(),
        .rw_req_rw(),
        .rw_req_addr(),
        .rw_req_wline(),
        .rw_req_ready(1'b1),
        .rw_resp_valid(1'b0),
        .rw_resp_rline(64'h0),
        .sp_active(1'b0),
        .sp_boot_suppress(1'b0),
        .sp_req_valid(),
        .sp_req_target(),
        .sp_req_addr(),
        .sp_req_rw(),
        .sp_req_wdata(),
        .sp_req_ready(1'b0),
        .sp_resp_valid(1'b0),
        .sp_resp_rdata(8'h00),
        .sp_sss_snapshot(),
        .sh_en(1'b0),
        .sh_addr(18'h00000),
        .sh_we(1'b0),
        .sh_wdata(8'h00),
        .sh_rdata(),
        .arm_req_valid(1'b0),
        .arm_req_addr(16'h0000),
        .arm_req_rw(1'b1),
        .arm_req_wdata(8'h00),
        .arm_req_busy(),
        .arm_resp_valid(),
        .arm_resp_rdata(),
        .arm_post_we(1'b0),
        .arm_post_addr(16'h0000),
        .arm_post_wdata(8'h00),
        .arm_post_ready(),
        .arm_rw_flush_req(1'b0),
        .arm_rw_hold_release(1'b0),
        .arm_rw_flush_done(),
        .arm_rw_hold_state(),
        .c074_state(),
        .bus_owned(bus_owned),
        .video_phase_1mhz(),
        .dbg_core_pc(dbg_core_pc),
        .cnt_core_cycles(core_cycles),
        .cnt_bus_cycles(bus_cycles),
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
        .dbg_pc_trace(),
        .dbg_io_trace(),
        .dbg_trace_status(),
        .dbg_bus_faults()
    );

    logic saw_reset_entry = 1'b0;
    logic saw_c600 = 1'b0;
    logic saw_disk_entry = 1'b0;
    integer slot_rom_reads = 0;
    integer disk_io_reads = 0;
    integer card_d5_reads = 0;
    integer disk_data_reads = 0;
    integer bus_data_mismatches = 0;
    logic [7:0] served_disk_byte_q = 8'h00;

    always @(posedge clk) begin
        if (core_run && dbg_core_pc == 16'hFA62)
            saw_reset_entry <= 1'b1;
        if (core_run && dbg_core_pc == 16'hC600)
            saw_c600 <= 1'b1;
        if (core_run && dbg_core_pc == 16'h0801)
            saw_disk_entry <= 1'b1;
        if (disk2_i.ab_rom_read)
            slot_rom_reads <= slot_rom_reads + 1;
        if (disk2_i.ab_io_read)
            disk_io_reads <= disk_io_reads + 1;

        // Capture the exact byte offered by disk2_card at serve_en. Only one
        // virtual CPU request can be in flight.
        if (disk2_i.ab_io_read && ab_read.addr[3:0] == 4'hC) begin
            served_disk_byte_q <= disk2_i.disk_read_byte;
            if (disk2_i.disk_read_byte == 8'hD5)
                card_d5_reads <= card_d5_reads + 1;
        end

        // Compare the card byte with the value returned by vtw_bus_engine.
        if (core_i.eng_resp_valid && core_i.cycle_rw_q &&
            core_i.cycle_addr_q == 16'hC0EC) begin
            disk_data_reads <= disk_data_reads + 1;
            if (core_i.eng_resp_rdata != served_disk_byte_q)
                bus_data_mismatches <= bus_data_mismatches + 1;
        end
    end

    task automatic check(input logic condition, input string message);
        if (condition !== 1'b1)
            $fatal(1, "ONEE DISK BOOT FAIL: %s", message);
    endtask

    task automatic axi_write(input logic [7:0] reg_addr,
                             input logic [31:0] value);
        @(negedge clk);
        as_common.awaddr = reg_addr;
        as_common.wdata = value;
        as_common.wstrb = 4'hF;
        disk2_axi.awvalid = 1'b1;
        @(negedge clk);
        disk2_axi.awvalid = 1'b0;
        repeat (2) @(negedge clk);
    endtask

    initial begin
        // This file comes from ps_sources/frontend/apple2e_cpu_rom_data.c,
        // not a reduced test program. Direct hierarchy keeps test setup from
        // spending 16K two-clock ARM writes before the first reset fetch.
        $readmemh("onee_enhanced_cpu_rom.mem", core_i.shadow_i.mem_rom);
        for (int i = 0; i < 65536; i++) begin
            core_i.shadow_i.mem_main[i] = 8'h00;
            core_i.shadow_i.mem_aux[i]  = 8'h00;
        end

        #1;
        check(core_i.shadow_i.mem_rom[14'h3FFC] == 8'h62 &&
              core_i.shadow_i.mem_rom[14'h3FFD] == 8'hFA,
              "embedded ROM reset vector is not $FA62");
        check(core_i.shadow_i.mem_rom[14'h3A62] == 8'hD8 &&
              core_i.shadow_i.mem_rom[14'h3A63] == 8'h20 &&
              core_i.shadow_i.mem_rom[14'h3A64] == 8'h84 &&
              core_i.shadow_i.mem_rom[14'h3A65] == 8'hFE,
              "embedded ROM reset-entry signature changed");

        as_common = '0;
        disk2_axi.awvalid = 1'b0;

        repeat (8) @(posedge clk);
        resetn = 1'b1;

        // Match disk2_service's standard-track commit order.
        axi_write(8'h10, 32'h0000_0003);
        axi_write(8'h02, 32'h0070_0000);
        axi_write(8'h07, 32'd6384);
        axi_write(8'h06, 32'h0000_0001);
        repeat (64) @(posedge clk);
        check(disk2_i.active_drive_loaded,
              "track 0 did not commit to disk2_card");

        vtw_enable = 1'b1;

        fork : wait_for_bus
            begin
                wait (bus_owned);
                disable wait_for_bus;
            end
            begin
                #2ms;
                $fatal(1, "ONEE DISK BOOT FAIL: vTW did not acquire virtual bus");
            end
        join

        core_run = 1'b1;

        fork : wait_for_disk_entry
            begin
                wait (saw_disk_entry);
                repeat (8) @(posedge clk);
                disable wait_for_disk_entry;
            end
            begin
                #260ms;
                $fatal(1,
                       "ONEE DISK BOOT FAIL: no $0801 PC=%04X core=%0d bus=%0d io=%0d pos=%0d d5=%0d reads=%0d mismatch=%0d",
                       dbg_core_pc, core_cycles, bus_cycles, disk_io_reads,
                       disk2_i.drive_stream_pos_q[0], card_d5_reads,
                       disk_data_reads, bus_data_mismatches);
            end
        join

        check(saw_reset_entry, "real ROM did not execute its $FA62 reset entry");
        check(saw_c600, "production slot-6 ROM did not execute at $C600");
        check(saw_disk_entry, "CPU did not enter disk-loaded code at $0801");
        check(slot_rom_reads > 100 && disk_io_reads > 100 &&
              disk_data_reads > 100 && mc_read_count > 10 &&
              card_d5_reads > 0,
              "boot did not exercise the full slot-ROM/card/staging path");
        check(bus_data_mismatches == 0,
              "disk2_card and vTW bus-engine response bytes differed");
        check(core_i.shadow_i.mem_main[16'h0800] == expected_boot[0] &&
              core_i.shadow_i.mem_main[16'h0801] == expected_boot[1] &&
              core_i.shadow_i.mem_main[16'h0802] == expected_boot[2] &&
              core_i.shadow_i.mem_main[16'h0803] == expected_boot[3],
              "selected boot-sector signature was not loaded at $0800");

        $display("ONEE DISK2 BOOT PASS slot=%0d io=%0d reads=%0d ddr=%0d d5=%0d",
                 slot_rom_reads, disk_io_reads, disk_data_reads,
                 mc_read_count, card_d5_reads);
        $finish;
    end

    initial begin
        #261ms;
        $fatal(1, "ONEE DISK BOOT FAIL: global timeout");
    end

endmodule
