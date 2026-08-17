`timescale 1ns / 1ps

// Real-ROM ONE//e cold-reset boot-target proof.
//
// The runner converts the firmware's embedded Enhanced //e CPU ROM array to
// onee_enhanced_cpu_rom.mem. This bench boots that exact image on the vTW
// core over the isolated virtual motherboard bus. Production slot-ROM bytes
// prove that the ROM enters $C700 for SmartPort and $C600 for Disk II.
module tb_onee_rom_cold_boot #(
    parameter bit BOOT_TARGET_DISK2 = 1'b1
);

    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;

    logic resetn = 1'b0;
    logic vtw_enable = 1'b0;
    logic core_run = 1'b0;
    logic private_reset_assert = 1'b0;

    globals::AppleBus_read ab_read;
    globals::AppleBus_read softswitch_ab_read;
    globals::AppleBus_write motherboard_write;
    globals::AppleBus_write slot6_write_q = '0;
    globals::AppleBus_write slot7_write_q = '0;
    globals::AppleBus_write vtw_write;
    globals::AppleBus_write [3:0] client_writes;
    globals::AppleBus_write merged_write;
    globals::SoftSwitchState sss;

    logic req_ready;
    logic resp_valid;
    logic [7:0] resp_rdata;

    apple_virtual_bus #(
        .CYCLE_CLKS(16),
        .PHI0_RISE_CLK(8),
        .DRIVE_CLK(1),
        .ADDR_CLK(5),
        .SSS_CLK(6),
        .SERVE_CLK(9),
        .DATA_CLK(14)
    ) virtual_bus_i (
        .clk(clk),
        .resetn(resetn),
        .video_mode_50hz(1'b0),
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
        .manual_enable_request(1'b1),
        .boot_target_disk2(BOOT_TARGET_DISK2),
        .ab_read(ab_read),
        .session_boot_target_disk2(),
        .slot7_hidden(slot7_hidden)
    );

    logic [7:0] slot6_rom [0:255];
    logic [7:0] slot7_rom [0:255];

    initial begin
        $readmemh("disk2_slot6.mem", slot6_rom);
        $readmemh("smartport_a2retronet_style_c700.mem", slot7_rom);
    end

    // Use the production ROM bytes while keeping this test focused on the
    // reset scan and entry choice. The full Disk II boot bench covers the
    // slot-6 ROM's controller path; SmartPort protocol service remains a
    // separate end-to-end test.
    always_ff @(posedge clk) begin
        if (!resetn) begin
            slot6_write_q <= '0;
            slot7_write_q <= '0;
        end else begin
            if (ab_read.serve_en && ab_read.rw &&
                (ab_read.addr[15:8] == 8'hC6)) begin
                slot6_write_q.wr_data    <= slot6_rom[ab_read.addr[7:0]];
                slot6_write_q.wr_data_en <= 1'b1;
            end else if (ab_read.data_en) begin
                slot6_write_q.wr_data    <= 8'h00;
                slot6_write_q.wr_data_en <= 1'b0;
            end

            if (ab_read.serve_en && ab_read.rw && !slot7_hidden &&
                (ab_read.addr[15:8] == 8'hC7)) begin
                slot7_write_q.wr_data    <= slot7_rom[ab_read.addr[7:0]];
                slot7_write_q.wr_data_en <= 1'b1;
            end else if (ab_read.data_en) begin
                slot7_write_q.wr_data    <= 8'h00;
                slot7_write_q.wr_data_en <= 1'b0;
            end
        end
    end

    always_comb begin
        client_writes[0] = motherboard_write;
        client_writes[1] = vtw_write;
        client_writes[2] = slot6_write_q;
        client_writes[3] = slot7_write_q;
    end

    apple_bus_write_arbiter #(
        .NUM_CLIENTS(4),
        .FAST_DATA_CLIENT(1),
        .FAST_ADDR_CLIENT(1)
    ) arbiter_i (
        .inh_allowed(1'b1),
        .client_writes(client_writes),
        .ab_write(merged_write)
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
        .pause(1'b0),
        .assert_apple_res(private_reset_assert),
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
    logic saw_slot7_probe_while_hidden = 1'b0;
    logic saw_slot7_probe_while_visible = 1'b0;
    logic saw_slot6_probe = 1'b0;
    logic saw_slot7_entry = 1'b0;
    logic saw_slot6_entry = 1'b0;
    logic saw_warm_vector = 1'b0;
    logic clear_observation = 1'b0;

    always @(posedge clk) begin
        if (clear_observation) begin
            saw_reset_entry <= 1'b0;
            saw_slot7_probe_while_hidden <= 1'b0;
            saw_slot7_probe_while_visible <= 1'b0;
            saw_slot6_probe <= 1'b0;
            saw_slot7_entry <= 1'b0;
            saw_slot6_entry <= 1'b0;
            saw_warm_vector <= 1'b0;
        end else begin
            if (core_run && dbg_core_pc == 16'hFA62)
                saw_reset_entry <= 1'b1;

            if (saw_reset_entry && ab_read.serve_en && ab_read.rw &&
                ab_read.addr[15:8] == 8'hC7 && slot7_hidden)
                saw_slot7_probe_while_hidden <= 1'b1;

            if (saw_reset_entry && ab_read.serve_en && ab_read.rw &&
                ab_read.addr[15:8] == 8'hC7 && !slot7_hidden)
                saw_slot7_probe_while_visible <= 1'b1;

            if (saw_reset_entry && ab_read.serve_en && ab_read.rw &&
                ab_read.addr[15:8] == 8'hC6 && slot7_hidden)
                saw_slot6_probe <= 1'b1;

            if (saw_reset_entry && core_run && dbg_core_pc == 16'hC700)
                saw_slot7_entry <= 1'b1;
            if (saw_reset_entry && core_run && dbg_core_pc == 16'hC600)
                saw_slot6_entry <= 1'b1;
            if (saw_reset_entry && core_run && dbg_core_pc == 16'h0400)
                saw_warm_vector <= 1'b1;
        end
    end

    task automatic check(input logic condition, input string message);
        if (condition !== 1'b1)
            $fatal(1, "ONEE ROM BOOT FAIL: %s", message);
    endtask

    task automatic check_selected_path(input string phase);
        check(saw_reset_entry,
              $sformatf("%s did not execute the $FA62 reset entry", phase));
        if (BOOT_TARGET_DISK2) begin
            check(saw_slot7_probe_while_hidden,
                  $sformatf("%s did not hide the first slot-7 probe", phase));
            check(!saw_slot7_probe_while_visible,
                  $sformatf("%s exposed slot 7 before entering $C600", phase));
            check(saw_slot6_probe && saw_slot6_entry,
                  $sformatf("%s did not probe and enter $C600", phase));
            check(!saw_slot7_entry,
                  $sformatf("%s entered the SmartPort slot ROM", phase));
            check(!slot7_hidden,
                  $sformatf("%s did not release slot 7 at $C600", phase));
        end else begin
            check(!saw_slot7_probe_while_hidden,
                  $sformatf("%s hid a SmartPort slot-7 probe", phase));
            check(saw_slot7_probe_while_visible && saw_slot7_entry,
                  $sformatf("%s did not probe and enter $C700", phase));
            check(!saw_slot6_probe && !saw_slot6_entry,
                  $sformatf("%s fell through to Disk II", phase));
            check(!slot7_hidden,
                  $sformatf("%s did not keep slot 7 visible", phase));
        end
    endtask

    task automatic clear_boot_observation;
        begin
            clear_observation = 1'b1;
            @(posedge clk);
            #1;
            clear_observation = 1'b0;
        end
    endtask

    task automatic pulse_private_reset(input logic force_cold);
        begin
            // Exercise the same vTW Apple-RES request which firmware drives
            // through CARD_CTRL_VTW_CTRL, including its merged virtual-bus
            // path, instead of forcing the base motherboard reset input.
            private_reset_assert = 1'b1;
            repeat (8 * 16) @(posedge clk);
            if (force_cold) begin
                // This is the exact pair cleared by CPU0 before it releases
                // Ctrl+Alt+Delete. It defeats a valid stale warm vector.
                core_i.shadow_i.mem_main[16'h03F3] = 8'h00;
                core_i.shadow_i.mem_main[16'h03F4] = 8'h00;
            end
            repeat (16) @(posedge clk);
            private_reset_assert = 1'b0;
        end
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

        repeat (8) @(posedge clk);
        resetn = 1'b1;
        vtw_enable = 1'b1;

        fork : wait_for_bus
            begin
                wait (bus_owned);
                disable wait_for_bus;
            end
            begin
                #20us;
                $fatal(1, "ONEE ROM BOOT FAIL: vTW did not acquire virtual bus");
            end
        join

        check(slot7_hidden == BOOT_TARGET_DISK2,
              "initial slot-7 visibility did not match the boot target");
        core_run = 1'b1;

        fork : wait_for_boot_entry
            begin
                if (BOOT_TARGET_DISK2)
                    wait (saw_slot6_entry);
                else
                    wait (saw_slot7_entry);
                repeat (4) @(posedge clk);
                disable wait_for_boot_entry;
            end
            begin
                #12ms;
                $fatal(1,
                       "ONEE ROM BOOT FAIL: no selected card entry target=%s PC=%04X core=%0d bus=%0d line=%0d cycle=%0d",
                       BOOT_TARGET_DISK2 ? "Disk II" : "SmartPort",
                       dbg_core_pc, core_cycles, bus_cycles,
                       line_in_frame, cycle_in_line);
            end
        join

        check_selected_path("cold reset");
        check(core_cycles > 32'd100,
              "cold-reset smoke did not execute a meaningful ROM path");
        check(bus_cycles > 32'd10,
              "cold-reset smoke did not traverse the virtual Apple bus");

        if (!BOOT_TARGET_DISK2) begin
            // A valid warm signature and vector prove why a bare RES# pulse
            // does not reboot SmartPort: the real ROM jumps back into stale
            // software and never scans $C700.
            core_i.shadow_i.mem_main[16'h03F2] = 8'h00;
            core_i.shadow_i.mem_main[16'h03F3] = 8'h04;
            core_i.shadow_i.mem_main[16'h03F4] = 8'hA1;
            core_i.shadow_i.mem_main[16'h0400] = 8'h4C;
            core_i.shadow_i.mem_main[16'h0401] = 8'h00;
            core_i.shadow_i.mem_main[16'h0402] = 8'h04;

            clear_boot_observation();
            pulse_private_reset(1'b0);
            fork : wait_for_warm_vector
                begin
                    wait (saw_warm_vector);
                    disable wait_for_warm_vector;
                end
                begin
                    #4ms;
                    $fatal(1,
                           "ONEE ROM BOOT FAIL: valid warm signature did not take stale $0400 vector PC=%04X",
                           dbg_core_pc);
                end
            join
            check(!saw_slot7_entry,
                  "bare warm reset unexpectedly rescanned SmartPort");

            // Ctrl+Alt+Delete clears the signature while RES# is held. The
            // same Enhanced //e ROM must show its reset path and enter the
            // production SmartPort slot ROM again.
            clear_boot_observation();
            pulse_private_reset(1'b1);
            fork : wait_for_smartport_reboot
                begin
                    wait (saw_slot7_entry);
                    repeat (4) @(posedge clk);
                    disable wait_for_smartport_reboot;
                end
                begin
                    #12ms;
                    $fatal(1,
                           "ONEE ROM BOOT FAIL: cold reboot did not re-enter SmartPort PC=%04X",
                           dbg_core_pc);
                end
            join
            check_selected_path("Ctrl+Alt+Delete cold reboot");
        end

        if (BOOT_TARGET_DISK2)
            $display("ONEE ROM DISK2 BOOT PATH PASS");
        else
            $display("ONEE ROM SMARTPORT BOOT PATH PASS");
        $finish;
    end

    initial begin
        #35ms;
        $fatal(1, "ONEE ROM BOOT FAIL: timeout");
    end

endmodule

module tb_onee_rom_cold_boot_disk2;
    tb_onee_rom_cold_boot #(.BOOT_TARGET_DISK2(1'b1)) bench_i();
endmodule

module tb_onee_rom_cold_boot_smartport;
    tb_onee_rom_cold_boot #(.BOOT_TARGET_DISK2(1'b0)) bench_i();
endmodule
