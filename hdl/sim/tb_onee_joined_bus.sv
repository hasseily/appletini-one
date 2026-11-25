`timescale 1ns / 1ps

// Joined ONE//e motherboard, vTW, slot-bus, and arbiter regression.
module tb_onee_joined_bus;

    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;

    logic resetn = 1'b0;
    logic onee_enabled = 1'b0;
    logic slot6_enabled = 1'b0;
    logic disk2_boot_selected = 1'b1;
    wire card_slot6_bus_enable = slot6_enabled && onee_enabled;
    wire configured_boot_target_disk2 = disk2_boot_selected;
    wire boot_target_disk2 =
        configured_boot_target_disk2 && slot6_enabled;
    /* Model the exact VTW_CTRL register word that apple_top holds outside the
     * virtual Apple reset domain. The joined test drives the core from the
     * same fields as apple_top: enable/core-run in [1:0], speed in [3:2],
     * pause in bit 8, and the divided-mode pace value in [31:16]. */
    logic [31:0] vtw_ctrl_q = 32'h0000_0000;
    wire vtw_enable = vtw_ctrl_q[0];
    wire core_run = vtw_ctrl_q[1];
    logic onee_menu_audio_mute = 1'b0;
    always_ff @(posedge clk) begin
        if (!resetn)
            onee_menu_audio_mute <= 1'b0;
        else
            onee_menu_audio_mute <= onee_enabled && vtw_ctrl_q[8];
    end

    globals::AppleBus_read  ab_read;
    globals::AppleBus_read  card_ab_read;
    globals::AppleBus_read  card_probe_ab_read;
    globals::AppleBus_read  softswitch_ab_read;
    globals::AppleBus_read  disk_ab_read;
    globals::AppleBus_write motherboard_write;
    globals::AppleBus_write vtw_write;
    globals::AppleBus_write disk_write;
    globals::AppleBus_write slot7_write_q;
    globals::AppleBus_write inh_test_write = '0;
    globals::AppleBus_write [12:0] client_writes;
    globals::AppleBus_write merged_write;
    globals::AppleBus_write physical_write;
    globals::SoftSwitchState sss;
    globals::SoftSwitchState disk_sss;
    logic card_probe_active = 1'b0;

    logic req_valid = 1'b0;
    logic req_ready;
    logic resp_valid;
    logic [7:0] resp_rdata;

    logic        input_ps_wr_en = 1'b0;
    logic [7:0]  input_ps_addr = 8'h00;
    logic [31:0] input_ps_wdata = 32'h0000_0000;
    wire         onee_warm_reset_request;
    wire         onee_warm_reset_ack;
    wire         onee_virtual_res_n;

    onee_input_bridge input_bridge_i (
        .clk                    (clk),
        .resetn                 (resetn),
        .enabled                (onee_enabled),
        .ps_wr_en               (input_ps_wr_en),
        .ps_addr                (input_ps_addr),
        .ps_wdata               (input_ps_wdata),
        .ps_read_addr           (8'h5F),
        .ps_rdata               (),
        .keyboard_event_valid   (),
        .keyboard_event_ready   (1'b0),
        .keyboard_event_code    (),
        .keyboard_any_down      (),
        .keyboard_modifiers     (),
        .pushbuttons             (),
        .paddle_values          (),
        .warm_reset_request     (onee_warm_reset_request),
        .warm_reset_ack         (onee_warm_reset_ack)
    );

    onee_warm_reset_ctrl #(
        .MIN_NATIVE_CYCLES(8)
    ) warm_reset_i (
        .clk                    (clk),
        .resetn                 (resetn),
        .enabled                (onee_enabled),
        .request                (onee_warm_reset_request),
        .native_cycle_tick      (ab_read.data_en),
        .virtual_res_n          (onee_virtual_res_n),
        .acknowledge            (onee_warm_reset_ack),
        .active                 ()
    );

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
        .res_n_in(onee_virtual_res_n),
        .irq_n_in(1'b1),
        .nmi_n_in(1'b1),
        .rdy_n_in(1'b1),
        .dma_n_in(1'b1),
        .inh_n_in(1'b1),
        .req_valid(req_valid),
        .req_ready(req_ready),
        .req_addr(16'hFFFF),
        .req_rw(1'b1),
        .req_wdata(8'h00),
        .resp_valid(resp_valid),
        .resp_rdata(resp_rdata),
        .floating_bus_data(8'hFF),
        .ab_write(merged_write),
        .ab_read(ab_read)
    );

    logic keyboard_event_valid = 1'b0;
    wire keyboard_event_ready;
    logic [6:0] keyboard_event_code = 7'h00;
    logic keyboard_any_down = 1'b1;
    logic [2:0] pushbuttons = 3'b001;
    wire [7:0] keyboard_latch;
    wire keyboard_strobe;
    wire cassette_out;
    wire speaker;

    onee_motherboard_io motherboard_i (
        .clk(clk),
        .resetn(resetn),
        .enabled(onee_enabled),
        .ab_read(ab_read),
        .sss(sss),
        .softswitch_ab_read(softswitch_ab_read),
        .ab_write(motherboard_write),
        .floating_bus_data(8'hFF),
        .video_vblank(1'b0),
        .keyboard_event_valid(keyboard_event_valid),
        .keyboard_event_ready(keyboard_event_ready),
        .keyboard_event_code(keyboard_event_code),
        .keyboard_any_down(keyboard_any_down),
        .keyboard_modifiers_in(3'b000),
        .keyboard_modifiers_state(),
        .keyboard_latch(keyboard_latch),
        .keyboard_strobe(keyboard_strobe),
        .pushbuttons(pushbuttons),
        .cassette_in(1'b0),
        .paddle_values(32'h8080_8080),
        .cassette_out(cassette_out),
        .speaker(speaker),
        .utility_strobe_pulse(),
        .annunciators(),
        .ioudis(),
        .paddle_active(),
        .paddle_trigger_pulse()
    );

    soft_switch_manager manager_i (
        .iigs_profile(1'b0),
        .iigs_bootstrap_slot7(1'b0),
        .iigs_boot_mask(8'h00),
        .iigs_boot_mask_valid(1'b0),
        .iigs_native_video(1'b0), .iigs_bank_latch(1'b0),
        .clk(clk),
        .rstn(resetn),
        .ramworks_en(1'b0), .ramworks_16mb(1'b0),
        .ab_read(softswitch_ab_read),
        .sss(sss)
    );

    logic slot7_hidden;
    onee_cold_slot_scan cold_scan_i (
        .clk(clk),
        .resetn(resetn),
        .enabled(onee_enabled),
        .manual_enable_request(onee_enabled),
        .boot_target_disk2(boot_target_disk2),
        .warm_reset_active(1'b0),
        .ab_read(ab_read),
        .session_boot_target_disk2(),
        .slot7_hidden(slot7_hidden)
    );

    /* The SmartPort card, with both its ports. apple_top gives the bus port
     * and the private vTW port the same slot-7 visibility. private_paths
     * selects the build under test: 0 keeps every Cxxx access on the virtual
     * bus (the Disk II acceleration setting off), 1 lets the core use the
     * private SmartPort and Disk II ports. */
    logic private_paths = 1'b0;
    wire slot7_visible = onee_enabled && !slot7_hidden;
    wire sp_fast_active = private_paths && slot7_visible;
    globals::AppleBus_read sp_ab_read;
    globals::AxiSimple_common sp_as_common;
    AxiSimple_if sp_axi();
    logic        sp_req_valid;
    logic [2:0]  sp_req_target;
    logic [10:0] sp_req_addr;
    logic        sp_req_rw;
    logic [7:0]  sp_req_wdata;
    logic        sp_req_ready;
    logic        sp_resp_valid;
    logic [7:0]  sp_resp_rdata;
    logic [22:0] sp_sss_snapshot;

    smartport_card sp_i (
        .clk(clk),
        .rstn(resetn),
        .ab_read(sp_ab_read),
        .apple_bus_visible(slot7_visible),
        .overlay_bus_visible(1'b0),
        .sss(disk_sss),
        .slot_assign(3'd7),
        .as_common(sp_as_common),
        .as_client(sp_axi),
        .ab_write(slot7_write_q),
        .smartport_irq(),
        .overlay_capture_drop(1'b0),
        .overlay_canvas_shr_active(1'b0),
        .overlay_devsel_enabled(),
        .overlay_capture_armed(),
        .overlay_capture_bank_aux(),
        .overlay_capture_base(),
        .overlay_capture_limit(),
        .vtw_valid(sp_req_valid),
        .vtw_target(sp_req_target),
        .vtw_addr(sp_req_addr),
        .vtw_rw(sp_req_rw),
        .vtw_wdata(sp_req_wdata),
        .vtw_sss_snapshot(sp_sss_snapshot),
        .vtw_ready(sp_req_ready),
        .vtw_resp_valid(sp_resp_valid),
        .vtw_resp_rdata(sp_resp_rdata)
    );

    function automatic globals::AppleBus_read gate_ab(
        input globals::AppleBus_read value,
        input logic enabled
    );
        globals::AppleBus_read gated;
        gated = value;
        if (!enabled) begin
            gated.addr_en  = 1'b0;
            gated.phi0_tick = 1'b0;
            gated.sss_en   = 1'b0;
            gated.serve_en = 1'b0;
            gated.data_en  = 1'b0;
        end
        return gated;
    endfunction

    wire saved_disk2_handoff = 1'b0;
    wire disk2_visible =
        (onee_enabled && slot6_enabled) ||
        (card_slot6_bus_enable && saved_disk2_handoff);
    always_comb begin
        card_ab_read = card_probe_active ? card_probe_ab_read : ab_read;
        disk_ab_read = gate_ab(card_ab_read, disk2_visible);
        sp_ab_read = gate_ab(card_ab_read, slot7_visible);
        disk_sss = sss;
        if (card_probe_active)
            disk_sss.slot_access = 1'b1;
    end

    globals::AxiSimple_common disk_as_common;
    AxiSimple_if disk_axi();
    logic [20:0] disk_mc_line_addr;
    logic disk_mc_rw;
    logic [63:0] disk_mc_wdata;
    logic [7:0] disk_mc_wstrb;
    logic disk_mc_valid;

    // Same terms as apple_top's vtw_disk2_active for a ONE//e session.
    logic vtw_bus_owned;
    wire d2_fast_active = private_paths && core_run && vtw_bus_owned &&
                          card_slot6_bus_enable && !vtw_ctrl_q[7];
    logic       d2_req_valid;
    logic [3:0] d2_req_addr;
    logic       d2_req_ready;
    logic       d2_resp_valid;
    logic [7:0] d2_resp_rdata;
    logic       d2_cycle_tick;
    logic [3:0] d2_cycle_ticks;
    logic       d2_native_cycle_active;
    logic       d2_time_ready;
    logic       d2_write_timing_active;
    logic       d2_spinning;

    disk2_card disk_i (
        .clk(clk),
        .rstn(resetn),
        .ab_read(disk_ab_read),
        .rom_serve_en(1'b0),
        .sss(disk_sss),
        .slot_assign(3'd6),
        .as_common(disk_as_common),
        .as_client(disk_axi),
        .mc_line_addr(disk_mc_line_addr),
        .mc_rw(disk_mc_rw),
        .mc_wdata(disk_mc_wdata),
        .mc_wstrb(disk_mc_wstrb),
        .mc_valid(disk_mc_valid),
        .mc_ready(1'b1),
        .mc_rdata(64'h0),
        .mc_rvalid(1'b0),
        .ab_write(disk_write),
        .vtw_active(d2_fast_active),
        .vtw_req_valid(d2_req_valid),
        .vtw_req_addr(d2_req_addr),
        .vtw_req_ready(d2_req_ready),
        .vtw_resp_valid(d2_resp_valid),
        .vtw_resp_rdata(d2_resp_rdata),
        .vtw_cycle_tick(d2_cycle_tick), .vtw_cycle_ticks(d2_cycle_ticks),
        .vtw_native_cycle_active(d2_native_cycle_active),
        .vtw_time_ready(d2_time_ready),
        .vtw_write_timing_active(d2_write_timing_active),
        .sound_spinning(d2_spinning),
        .sound_qtrack(),
        .sound_event(),
        .sound_seek_start_qtrack(),
        .sound_seek_distance()
    );

    always_comb begin
        client_writes = '{default: '0};
        client_writes[12] = motherboard_write;
        client_writes[11] = vtw_write;
        client_writes[10] = inh_test_write;
        client_writes[3]  = disk_write;
        client_writes[2]  = slot7_write_q;
    end

    apple_bus_write_arbiter #(
        .NUM_CLIENTS(13),
        .FAST_DATA_CLIENT(2),
        .FAST_ADDR_CLIENT(11)
    ) arbiter_i (
        .inh_allowed(onee_enabled),
        .client_writes(client_writes),
        .ab_write(merged_write)
    );

    // This is the exact physical-wrapper boundary used by apple_top.
    assign physical_write = onee_enabled ? '0 : merged_write;

    logic sh_en = 1'b0;
    logic [17:0] sh_addr = '0;
    logic sh_we = 1'b0;
    logic [7:0] sh_wdata = '0;
    logic [7:0] sh_rdata;
    logic [15:0] dbg_core_pc;
    logic [31:0] vtw_core_cycles;

    vtw_core_top core_i (
        .clk(clk),
        .rstn(resetn),
        .enable(vtw_enable),
        .host_is_iiplus(1'b0),
        .virtual_motherboard(1'b1),
        .core_run(core_run),
        .pause(vtw_ctrl_q[8]),
        .assert_apple_res(1'b0),
        .speed_mode(vtw_ctrl_q[3:2]),
        .pace_divider(vtw_ctrl_q[31:16]),
        .ignore_c074(1'b0),
        .slow_region_en(10'd0),
        .slow_duration(16'd0),
        .d2_active(d2_fast_active),
        .d2_motor_active(d2_spinning),
        .d2_req_valid(d2_req_valid),
        .d2_req_addr(d2_req_addr),
        .d2_req_ready(d2_req_ready),
        .d2_resp_valid(d2_resp_valid),
        .d2_resp_rdata(d2_resp_rdata),
        .d2_cycle_tick(d2_cycle_tick),
        .d2_cycle_ticks(d2_cycle_ticks),
        .d2_native_cycle_active(d2_native_cycle_active),
        .d2_time_ready(d2_time_ready),
        .d2_write_timing_active(d2_write_timing_active),
        .ramworks_en(1'b0), .ramworks_16mb(1'b0),
        .video_vbl(1'b0),
        .video_mode_50hz(1'b0),
        .video_line(9'd10),
        .video_cycle(7'd20),
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
        // This would bypass C061-C063 on a physical II+. ONE//e must ignore it.
        .iiplus_buttons_zero(1'b1),
        .usb_joystick_active(1'b0), .usb_joystick_buttons(3'd0),
        .usb_joystick_paddles(32'h80808080),
        .rw_req_valid(),
        .rw_req_rw(),
        .rw_req_addr(),
        .rw_req_wline(),
        .rw_req_ready(1'b1),
        .rw_resp_valid(1'b0),
        .rw_resp_rline(64'h0),
        .sp_active(sp_fast_active),
        .sp_boot_suppress(1'b0),
        .sp_req_valid(sp_req_valid),
        .sp_req_target(sp_req_target),
        .sp_req_addr(sp_req_addr),
        .sp_req_rw(sp_req_rw),
        .sp_req_wdata(sp_req_wdata),
        .sp_req_ready(sp_req_ready),
        .sp_resp_valid(sp_resp_valid),
        .sp_resp_rdata(sp_resp_rdata),
        .sp_sss_snapshot(sp_sss_snapshot),
        .sh_en(sh_en),
        .sh_addr(sh_addr),
        .sh_we(sh_we),
        .sh_wdata(sh_wdata),
        .sh_rdata(sh_rdata),
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
        .bus_owned(vtw_bus_owned),
        .video_phase_1mhz(),
        .dbg_core_pc(dbg_core_pc),
        .cnt_core_cycles(vtw_core_cycles),
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
        .dbg_pc_trace(),
        .dbg_io_trace(),
        .dbg_trace_status(),
        .dbg_bus_faults()
    );

    localparam logic [17:0] ROM_BASE = 18'h20000;
    localparam byte PROGRAM [0:72] = '{
        8'hAD, 8'h00, 8'hC0, 8'h85, 8'h00, // C000 key with strobe
        8'hAD, 8'h10, 8'hC0, 8'h85, 8'h01, // C010 key + clear
        8'hAD, 8'h00, 8'hC0, 8'h85, 8'h02, // C000 after clear
        8'hAD, 8'h61, 8'hC0, 8'h85, 8'h03, // Open Apple + scanner low
        8'hAD, 8'h62, 8'hC0, 8'h85, 8'h04, // Closed Apple + scanner low
        8'hAD, 8'h11, 8'hC0, 8'h85, 8'h05, // status + scanner low
        8'hAD, 8'h20, 8'hC0, 8'h85, 8'h06, // cassette, unclaimed
        8'hAD, 8'h30, 8'hC0, 8'h85, 8'h07, // speaker, unclaimed
        8'hAD, 8'h00, 8'hC7, 8'h85, 8'h08, // hidden slot 7
        8'hAD, 8'h00, 8'hC6, 8'h85, 8'h09, // forced Disk II ROM
        8'hAD, 8'h00, 8'hC7, 8'h85, 8'h0A, // visible SmartPort slot ROM
        8'hAD, 8'h01, 8'hC8, 8'h85, 8'h0B, // visible SmartPort C8 ROM
        8'hAD, 8'hE9, 8'hC0, 8'h85, 8'h0C, // Disk II motor-on side effect
        8'hAD, 8'hEE, 8'hC0, 8'h85, 8'h0D, // Disk II even read response
        8'h4C, 8'h46, 8'hF0                    // done loop
    };

    // The first bytes of the SmartPort ROMs: $C700 and $C801.
    localparam logic [7:0] SP_ROM_C700 = 8'hC9;
    localparam logic [7:0] SP_ROM_C801 = 8'h20;

    /* The program for the private ports, at $F100. It ends in the wait of
     * the SmartPort ROM for a reply, which never comes in this bench. */
    localparam byte FAST_PROGRAM [0:45] = '{
        8'hAD, 8'h00, 8'hC6, 8'h85, 8'h10, // Disk II ROM: ends the cold scan
        8'hAD, 8'h00, 8'hC7, 8'h85, 8'h11, // SmartPort slot ROM
        8'hAD, 8'h01, 8'hC8, 8'h85, 8'h12, // SmartPort C8 ROM
        8'hAD, 8'hF1, 8'hCF, 8'h85, 8'h13, // SmartPort CTRL
        8'hA9, 8'h5A,
        8'h8D, 8'hF0, 8'hCF,               // one command byte
        8'h8D, 8'hF1, 8'hCF,               // EXECUTE
        8'hAD, 8'hE9, 8'hC0,               // Disk II motor on: odd, bus
        8'hAD, 8'hEE, 8'hC0, 8'h85, 8'h14, // Disk II even read: private
        8'hE6, 8'h15,                      // count this run
        8'hAD, 8'hF1, 8'hCF,               // F126: wait for the reply
        8'h10, 8'hFB,
        8'h4C, 8'h2B, 8'hF1
    };

    task automatic check(input logic condition, input string message);
        if (condition !== 1'b1)
            $fatal(1, "%s", message);
    endtask

    task automatic sh_write(input logic [17:0] addr,
                            input logic [7:0] value);
        @(posedge clk);
        sh_en    <= 1'b1;
        sh_we    <= 1'b1;
        sh_addr  <= addr;
        sh_wdata <= value;
        @(posedge clk);
        sh_en <= 1'b0;
        sh_we <= 1'b0;
    endtask

    task automatic sh_read(input logic [17:0] addr,
                           output logic [7:0] value);
        @(posedge clk);
        sh_en   <= 1'b1;
        sh_we   <= 1'b0;
        sh_addr <= addr;
        @(posedge clk);
        sh_en <= 1'b0;
        @(posedge clk);
        value = sh_rdata;
    endtask

    task automatic push_key(input logic [6:0] code);
        wait (keyboard_event_ready);
        @(negedge clk);
        keyboard_event_code  = code;
        keyboard_event_valid = 1'b1;
        @(posedge clk);
        @(negedge clk);
        keyboard_event_valid = 1'b0;
    endtask

    task automatic write_vtw_ctrl(input logic [31:0] value);
        @(negedge clk);
        vtw_ctrl_q = value;
        @(posedge clk);
        #1;
    endtask

    task automatic write_onee_control(input logic [31:0] value);
        @(negedge clk);
        input_ps_addr  = 8'h5F;
        input_ps_wdata = value;
        input_ps_wr_en = 1'b1;
        @(posedge clk);
        #1;
        input_ps_wr_en = 1'b0;
    endtask

    task automatic probe_card_read(
        input  logic [15:0] addr,
        output logic        disk_drives,
        output logic        slot7_drives,
        output logic [7:0]  slot7_data
    );
        @(negedge clk);
        card_probe_ab_read = '0;
        card_probe_ab_read.res = 1'b1;
        card_probe_ab_read.rw = 1'b1;
        card_probe_ab_read.cycle_valid = 1'b1;
        card_probe_ab_read.addr = addr;
        card_probe_ab_read.serve_en = 1'b1;
        card_probe_active = 1'b1;
        @(posedge clk);
        #1;
        disk_drives = disk_write.wr_data_en;
        // The SmartPort card registers its ROM read: one clock more.
        @(posedge clk);
        #1;
        slot7_drives = slot7_write_q.wr_data_en;
        slot7_data = slot7_write_q.wr_data;

        @(negedge clk);
        card_probe_ab_read.serve_en = 1'b0;
        card_probe_ab_read.data_en = 1'b1;
        @(posedge clk);
        #1;
        card_probe_active = 1'b0;
        card_probe_ab_read = '0;
    endtask

    int physical_nonzero_cycles = 0;
    int private_smartport_cycles = 0;
    int private_disk2_cycles = 0;
    int bus_slot7_cycles = 0;
    int bus_disk2_even_reads = 0;
    always @(posedge clk) begin
        if (onee_enabled && physical_write !== '0)
            physical_nonzero_cycles++;
        if (onee_enabled && sp_req_valid && sp_req_ready)
            private_smartport_cycles++;
        if (onee_enabled && d2_req_valid && d2_req_ready)
            private_disk2_cycles++;
        // Virtual bus cycles that the private ports must take.
        if (sp_fast_active && ab_read.addr_en && ab_read.cycle_valid &&
            (ab_read.addr[15:8] == 8'hC7 ||
             (ab_read.addr[15:11] == 5'b11001 &&
              ab_read.addr != 16'hCFFF)))
            bus_slot7_cycles++;
        if (d2_fast_active && ab_read.addr_en && ab_read.cycle_valid &&
            ab_read.rw && ab_read.addr[15:4] == 12'hC0E && !ab_read.addr[0])
            bus_disk2_even_reads++;
    end

    logic [15:0] scan_pos;
    logic [15:0] scan_addr;
    logic [7:0] result [0:13];
    logic [31:0] warm_reset_ctrl_before;
    logic [31:0] paused_core_cycles;
    logic [15:0] paused_core_pc;
    int warm_reset_fabric_clks;
    int warm_reset_native_ticks;
    logic probe_disk_drives;
    logic probe_slot7_drives;
    logic [7:0] probe_slot7_data;

    initial begin
        sp_as_common = '0;
        sp_axi.awvalid = 1'b0;
        disk_as_common = '0;
        disk_axi.awvalid = 1'b0;
        card_probe_ab_read = '0;

        repeat (8) @(posedge clk);
        resetn = 1'b1;

        // Load the core image while port B owns the shadow.
        for (int i = 0; i < 73; i++)
            sh_write(ROM_BASE + 18'h3000 + 18'(i), PROGRAM[i]);
        sh_write(ROM_BASE + 18'h3FFC, 8'h00);
        sh_write(ROM_BASE + 18'h3FFD, 8'hF0);
        for (int i = 0; i < 16; i++)
            sh_write(18'(i), 8'h00);

        scan_pos = vtw_pkg::vtw_video_position_rewind(
            1'b0, 9'd10, 7'd20, 2'd2);
        scan_addr = vtw_pkg::vtw_scanner_address(
            1'b0, scan_pos[15:7], scan_pos[6:0],
            1'b1, 1'b0, 1'b0, 1'b0, 1'b0);
        sh_write({2'b00, scan_addr}, 8'h2D);

        // With Slot 6 off, ONE//e must select SmartPort and keep every Disk II
        // ROM and I/O response silent.
        onee_enabled = 1'b1;
        repeat (3) @(posedge clk);
        check(!slot6_enabled && !card_slot6_bus_enable &&
              !boot_target_disk2 && !disk2_visible,
              "Slot 6 OFF did not remove the ONE//e Disk II target");
        check(!slot7_hidden,
              "Slot 6 OFF hid the ONE//e SmartPort boot ROM");
        probe_card_read(16'hC600, probe_disk_drives,
                        probe_slot7_drives, probe_slot7_data);
        check(!probe_disk_drives,
              "Slot 6 OFF still served the Disk II ROM");
        probe_card_read(16'hC0E9, probe_disk_drives,
                        probe_slot7_drives, probe_slot7_data);
        check(!probe_disk_drives && !disk_i.motor_on_q,
              "Slot 6 OFF still accepted Disk II I/O");
        probe_card_read(16'hC700, probe_disk_drives,
                        probe_slot7_drives, probe_slot7_data);
        check(!probe_disk_drives && probe_slot7_drives &&
              probe_slot7_data == SP_ROM_C700,
              "Slot 6 OFF did not expose the SmartPort slot ROM");

        // Re-enter ONE//e with Slot 6 on. The same resolved target now hides
        // SmartPort until the core reaches the Disk II ROM.
        @(negedge clk);
        onee_enabled = 1'b0;
        repeat (3) @(posedge clk);
        @(negedge clk);
        slot6_enabled = 1'b1;
        onee_enabled = 1'b1;
        repeat (3) @(posedge clk);
        check(card_slot6_bus_enable && boot_target_disk2 && disk2_visible,
              "Slot 6 ON did not expose the ONE//e Disk II target");
        check(slot7_hidden, "ONE//e entry did not hide slot 7");
        push_key(7'h41);
        check(keyboard_latch == 8'hC1,
              "keyboard event did not reach the motherboard latch");

        // Internal INH/data serving must pass the arbiter even though the
        // saved physical machine mode is unknown, while the physical record
        // remains fully masked.
        inh_test_write = '0;
        inh_test_write.assert_inh = 1'b1;
        inh_test_write.wr_data = 8'hA6;
        inh_test_write.wr_data_en = 1'b1;
        #1;
        check(merged_write.assert_inh && merged_write.wr_data_en &&
              merged_write.wr_data == 8'hA6 && !ab_read.inh,
              "ONE//e internal INH serve was blocked");
        check(physical_write == '0,
              "internal INH escaped into the physical write record");
        inh_test_write = '0;

        // ONE//e run word: enable, divided mode, forced native Disk II path,
        // and a 37-clock (~3.6 MHz) pace. Keep the core held for bus acquire.
        write_vtw_ctrl(32'h0025_0085);
        fork : wait_for_bus
            begin
                wait (vtw_bus_owned);
                disable wait_for_bus;
            end
            begin
                #20us;
                $fatal(1, "vTW did not acquire the synthetic bus");
            end
        join
        write_vtw_ctrl(32'h0025_0087);

        fork : wait_for_program
            begin
                wait (dbg_core_pc == 16'hF046);
                repeat (4) @(posedge clk);
                disable wait_for_program;
            end
            begin
                #100us;
                $fatal(1, "ONE//e joined program timeout at PC=%04X state=%0d",
                       dbg_core_pc, core_i.xstate_q);
            end
        join

        /* The config menu pause keeps ENABLE, CORE_RUN, speed, and the CPU
         * reset domain live. The current access may drain, but no completed
         * 65C02 edge may occur after the pause has settled. Clearing only
         * bit 8 must resume the same machine without a reset or ROM reload. */
        write_vtw_ctrl(32'h0025_0187);
        repeat (256) @(posedge clk);
        check(onee_menu_audio_mute,
              "ONE//e menu pause did not assert emulation audio mute");
        paused_core_cycles = vtw_core_cycles;
        paused_core_pc = dbg_core_pc;
        repeat (128) begin
            @(posedge clk);
            check(vtw_core_cycles == paused_core_cycles &&
                  dbg_core_pc == paused_core_pc &&
                  !core_i.core_en,
                  "ONE//e pause allowed the virtual CPU to advance");
            check(core_i.core_res_n && core_run && vtw_ctrl_q[8] &&
                  vtw_ctrl_q[3:2] == 2'd1 &&
                  vtw_ctrl_q[31:16] == 16'd37,
                  "ONE//e pause reset the core or changed its speed");
        end
        write_vtw_ctrl(32'h0025_0087);
        #1;
        check(!onee_menu_audio_mute,
              "ONE//e menu resume did not release emulation audio mute");
        fork : wait_for_pause_resume
            begin
                wait (vtw_core_cycles != paused_core_cycles);
                disable wait_for_pause_resume;
            end
            begin
                #20us;
                $fatal(1, "ONE//e core did not resume after menu pause");
            end
        join
        check(core_i.core_res_n && core_run && !vtw_ctrl_q[8],
              "ONE//e resume changed run/reset state");

        write_vtw_ctrl(32'h0025_0085);
        repeat (5) @(posedge clk);
        for (int i = 0; i < 14; i++)
            sh_read(18'(i), result[i]);

        check(result[0] == 8'hC1, "$C000 did not return full key/strobe");
        check(result[1] == 8'hC1, "$C010 did not return key/down state");
        check(result[2] == 8'h41 && !keyboard_strobe,
              "$C010 did not clear the key strobe");
        check(result[3] == 8'hAD,
              "Open Apple did not merge bit 7 over scanner low bits");
        check(result[4] == 8'h2D,
              "Closed Apple did not merge bit 7 over scanner low bits");
        check(result[5] == 8'hC1,
              $sformatf("C011 status did not keep the last key code: %02X",
                        result[5]));
        check(result[6] == 8'h2D && cassette_out,
              "unclaimed cassette read lost scanner data or side effect");
        check(result[7] == 8'h2D && speaker,
              "unclaimed speaker read lost scanner data or side effect");
        check(result[8] == 8'h2D,
              "hidden slot 7 did not return scanner fallback");
        check(result[9] == 8'hA2,
              "slot 6 ROM was not visible with Slot 6 enabled");
        check(result[10] == SP_ROM_C700 && result[11] == SP_ROM_C801,
              "SmartPort C7/C8 did not use the synthetic slot bus");
        check(result[12] == 8'h2D && disk_i.motor_on_q,
              "Disk II I/O side effect did not traverse the synthetic bus");
        check(result[13] == 8'h80,
              "Disk II even I/O read did not return the card response");
        check(!slot7_hidden,
              "$C600 probe did not release the slot-7 cold-scan hold");
        check(private_smartport_cycles == 0 && private_disk2_cycles == 0,
              "a private port was used while the private paths were off");
        check(physical_nonzero_cycles == 0 && physical_write == '0,
              "ONE//e virtual traffic reached the physical write record");

        /* Re-run the core, then exercise the bridge's private $5F bit-0 reset
         * primitive. The real input bridge holds the request, the real
         * warm-reset controller counts virtual native cycles, and
         * apple_virtual_bus drives ab_read.res low. Ctrl+Alt+Delete uses the
         * ordered PS cold-reboot path; this lower-level fallback must still
         * leave VTW_CTRL and both speed fields unchanged. */
        write_vtw_ctrl(32'h0025_0087);
        warm_reset_ctrl_before = vtw_ctrl_q;
        check(vtw_ctrl_q[3:2] == 2'd1 &&
              vtw_ctrl_q[31:16] == 16'd37 &&
              core_i.eff_mode == 2'd1,
              "divided VTW_CTRL was not active before warm reset");
        write_onee_control(32'h0000_0001);
        warm_reset_fabric_clks = 0;
        while (ab_read.res && warm_reset_fabric_clks < 16) begin
            @(posedge clk);
            #1;
            warm_reset_fabric_clks++;
            check(vtw_ctrl_q == warm_reset_ctrl_before,
                  "warm-reset request changed VTW_CTRL before RESET fell");
        end
        check(onee_warm_reset_request && !ab_read.res,
              "ONE//e control write did not start the virtual reset");

        warm_reset_fabric_clks = 0;
        warm_reset_native_ticks = 0;
        while (!ab_read.res && warm_reset_fabric_clks < 512) begin
            @(posedge clk);
            #1;
            warm_reset_fabric_clks++;
            if (ab_read.data_en)
                warm_reset_native_ticks++;
            check(vtw_ctrl_q == warm_reset_ctrl_before &&
                  vtw_ctrl_q[3:2] == 2'd1 &&
                  vtw_ctrl_q[31:16] == 16'd37,
                  "warm reset changed VTW_CTRL speed fields");
        end
        check(ab_read.res && warm_reset_native_ticks >= 8,
              "virtual reset did not finish after eight native cycles");
        check(!onee_warm_reset_request &&
              vtw_ctrl_q == warm_reset_ctrl_before &&
              vtw_ctrl_q[3:2] == 2'd1 &&
              vtw_ctrl_q[31:16] == 16'd37,
              "warm-reset release changed divided VTW_CTRL");
        warm_reset_fabric_clks = 0;
        while (!core_i.core_en && warm_reset_fabric_clks < 512) begin
            @(posedge clk);
            #1;
            warm_reset_fabric_clks++;
        end
        check(core_i.core_en && core_i.eff_mode == 2'd1,
              "core did not resume at divided speed after warm reset");

        /* The private SmartPort and Disk II ports in ONE//e, at TURBO. A
         * warm reset starts the second program; the cold scan then hides
         * slot 7 again until the program reads the Disk II ROM. */
        write_vtw_ctrl(32'h0025_0085);
        repeat (64) @(posedge clk);
        for (int i = 0; i < 46; i++)
            sh_write(ROM_BASE + 18'h3100 + 18'(i), FAST_PROGRAM[i]);
        sh_write(ROM_BASE + 18'h3FFC, 8'h00);
        sh_write(ROM_BASE + 18'h3FFD, 8'hF1);
        for (int i = 16; i < 22; i++)
            sh_write(18'(i), 8'h00);
        @(negedge clk);
        private_paths = 1'b1;
        write_vtw_ctrl(32'h0025_000F);
        run_fast_program(8'd1);

        // A menu pause during the SmartPort wait stops the CPU and no more.
        write_vtw_ctrl(32'h0025_010F);
        repeat (256) @(posedge clk);
        paused_core_cycles = vtw_core_cycles;
        paused_core_pc = dbg_core_pc;
        repeat (128) begin
            @(posedge clk);
            check(vtw_core_cycles == paused_core_cycles &&
                  dbg_core_pc == paused_core_pc && !core_i.core_en,
                  "pause in the SmartPort wait let the CPU advance");
        end
        check(sp_i.exec_pending_q && sp_i.exec_vtw_q,
              "pause changed the pending SmartPort command");
        write_vtw_ctrl(32'h0025_000F);
        fork : wait_for_fast_resume
            begin
                wait (vtw_core_cycles != paused_core_cycles);
                disable wait_for_fast_resume;
            end
            begin
                #20us;
                $fatal(1, "core did not resume in the SmartPort wait");
            end
        join

        // A warm reset during the SmartPort wait runs the program again.
        run_fast_program(8'd2);

        $display("ONEE JOINED BUS PASS");
        $finish;
    end

    /* Warm reset, then wait for the program to reach its SmartPort wait and
     * check what it read. */
    task automatic run_fast_program(input logic [7:0] run);
        logic [7:0] fast [0:5];
        int sp_before;
        int d2_before;
        sp_before = private_smartport_cycles;
        d2_before = private_disk2_cycles;
        bus_slot7_cycles = 0;
        bus_disk2_even_reads = 0;
        write_onee_control(32'h0000_0001);
        fork : wait_for_fast_program
            begin
                wait (!ab_read.res);
                wait (ab_read.res);
                wait (dbg_core_pc >= 16'hF126 && dbg_core_pc <= 16'hF12A &&
                      sp_i.exec_pending_q);
                repeat (64) @(posedge clk);
                disable wait_for_fast_program;
            end
            begin
                #400us;
                $fatal(1, "private-port program timeout: PC=%04X state=%0d hidden=%0b",
                       dbg_core_pc, core_i.xstate_q, slot7_hidden);
            end
        join
        write_vtw_ctrl(32'h0025_000D);
        repeat (64) @(posedge clk);
        for (int i = 0; i < 6; i++)
            sh_read(18'(16 + i), fast[i]);
        write_vtw_ctrl(32'h0025_000F);
        $display("fast run %0d: %02X %02X %02X %02X %02X %02X sp=%0d d2=%0d bus7=%0d busd2=%0d",
                 run, fast[0], fast[1], fast[2], fast[3], fast[4], fast[5],
                 private_smartport_cycles - sp_before,
                 private_disk2_cycles - d2_before,
                 bus_slot7_cycles, bus_disk2_even_reads);
        check(fast[5] == run, "the private-port program did not run once");
        check(fast[0] == 8'hA2 && !slot7_hidden,
              "the Disk II ROM read did not end the cold scan");
        check(fast[1] == SP_ROM_C700 && fast[2] == SP_ROM_C801,
              "SmartPort ROM bytes were wrong on the private port");
        check(fast[3] == 8'h20,
              "SmartPort CTRL did not show the vTW flag in ONE//e");
        check(sp_i.exec_pending_q && sp_i.exec_vtw_q &&
              sp_i.in_count_q == 11'd1,
              "EXECUTE did not arrive as a vTW command with its byte");
        check(private_smartport_cycles - sp_before >= 6 &&
              bus_slot7_cycles == 0,
              "SmartPort accesses did not use the private port");
        check(disk_i.motor_on_q && private_disk2_cycles - d2_before == 1 &&
              bus_disk2_even_reads == 0,
              "the Disk II even read did not use the private port");
        check(fast[4] == 8'h80,
              "the private Disk II read returned the wrong byte");
        check(physical_nonzero_cycles == 0,
              "ONE//e traffic reached the physical write record");
    endtask

    initial begin
        #2ms;
        $fatal(1, "ONEE JOINED BUS TIMEOUT");
    end

endmodule
