`timescale 1ns / 1ps

// ONE//e native scanner and renderer-input regression.
//
// The bench joins the same blocks used by apple_top in stand-alone mode:
// the free-running virtual Apple bus, the native scanner counter, the vTW
// soft CPU, the shared soft-switch tracker, and the normal cycle-capture
// FIFO.  It proves that the virtual bus supplies the complete NTSC cadence
// and that accelerated screen writes and video switches reach the renderer
// input as ordinary Apple-cycle records.
module tb_onee_video_path;

    timeunit 1ns;
    timeprecision 1ps;

    import apple_cycle_capture_pkg::*;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;

    logic resetn = 1'b0;
    logic set_frame_zero = 1'b0;
    logic frame_en = 1'b0;
    wire video_vblank;

    globals::AppleBus_read  ab_read;
    globals::AppleBus_read  softswitch_ab_read;
    globals::AppleBus_write motherboard_write;
    globals::AppleBus_write vtw_write;
    globals::AppleBus_write [1:0] client_writes;
    globals::AppleBus_write merged_write;
    globals::SoftSwitchState sss;

    logic req_ready;
    logic resp_valid;
    logic [7:0] resp_rdata;

    // Short phases keep the full-frame cadence check quick. Leave three
    // clocks between drive_en and addr_en so the production vTW posted-write
    // FETCH/LOAD pipeline can settle inside the tuple resolve window.
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
        .floating_bus_data(8'hFF),
        .ab_write(merged_write),
        .ab_read(ab_read)
    );

    onee_motherboard_io motherboard_i (
        .clk(clk),
        .resetn(resetn),
        .enabled(1'b1),
        .ab_read(ab_read),
        .sss(sss),
        .softswitch_ab_read(softswitch_ab_read),
        .ab_write(motherboard_write),
        .floating_bus_data(8'hFF),
        .video_vblank(video_vblank),
        .keyboard_event_valid(1'b0),
        .keyboard_event_ready(),
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

    always_comb begin
        client_writes = '{default: '0};
        client_writes[0] = motherboard_write;
        client_writes[1] = vtw_write;
    end

    apple_bus_write_arbiter #(
        .NUM_CLIENTS(2)
    ) arbiter_i (
        .inh_allowed(1'b1),
        .client_writes(client_writes),
        .ab_write(merged_write)
    );

    logic update_pulse;
    logic [8:0] line_in_frame;
    logic [6:0] cycle_in_line;
    assign video_vblank = (line_in_frame >= 9'd192);

    apple_timing_gen timing_i (
        .clk(clk),
        .resetn(resetn),
        .apple_bus_pulse(ab_read.sss_en),
        .video_mode_50hz(1'b0),
        .update_pulse(update_pulse),
        .line_in_frame(line_in_frame),
        .cycle_in_line(cycle_in_line),
        .set_frame_zero_pulse(set_frame_zero),
        .set_vblank_start_pulse(1'b0)
    );

    soft_switch_manager manager_i (
        .clk(clk),
        .rstn(resetn),
        .ramworks_en(1'b0),
        .ab_read(softswitch_ab_read),
        .sss(sss)
    );

    AppleCycleRecord capture_data;
    logic capture_rd_en = 1'b0;
    logic capture_empty;
    logic capture_drop;

    apple_cycle_capture capture_i (
        .clk(clk),
        .resetn(resetn),
        .soft_reset(!ab_read.res),
        .ab_read(ab_read),
        .sss(sss),
        .line_in_frame(line_in_frame),
        .cycle_in_line(cycle_in_line),
        .frame_en(frame_en),
        .overlay_devsel_enabled(1'b0),
        .overlay_capture_armed(1'b0),
        .overlay_capture_bank_aux(1'b0),
        .overlay_capture_base(16'h0000),
        .overlay_capture_limit(16'h0000),
        .cycle_capture_data(capture_data),
        .cycle_capture_rd_en(capture_rd_en),
        .cycle_capture_empty(capture_empty),
        .capture_drop_sticky(capture_drop),
        .capture_drop_ack(1'b0),
        .overlay_capture_drop_sticky(),
        .shr_capture_active()
    );

    logic vtw_enable = 1'b0;
    logic core_run = 1'b0;
    logic sh_en = 1'b0;
    logic [17:0] sh_addr = '0;
    logic sh_we = 1'b0;
    logic [7:0] sh_wdata = '0;
    logic [7:0] sh_rdata;
    logic [15:0] dbg_core_pc;
    logic bus_owned;
    logic [31:0] posted_writes;

    vtw_core_top core_i (
        .clk(clk),
        .rstn(resetn),
        .enable(vtw_enable),
        .host_is_iiplus(1'b0),
        .virtual_motherboard(1'b1),
        .core_run(core_run),
        .pause(1'b0),
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
        .irq_assert_in(1'b0),
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
        .bus_owned(bus_owned),
        .video_phase_1mhz(),
        .dbg_core_pc(dbg_core_pc),
        .cnt_core_cycles(),
        .cnt_bus_cycles(),
        .cnt_posted_writes(posted_writes),
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
    localparam byte PROGRAM [0:30] = '{
        8'hAD, 8'h50, 8'hC0,             // LDA $C050: graphics
        8'hAD, 8'h57, 8'hC0,             // LDA $C057: hi-res
        8'h8D, 8'h0C, 8'hC0,             // STA $C00C: 80COL off
        8'hAD, 8'h5F, 8'hC0,             // LDA $C05F: HGR, AN3 on
        8'hA9, 8'hA5,                    // LDA #$A5
        8'h8D, 8'h00, 8'h20,             // STA $2000
        8'h8D, 8'h0D, 8'hC0,             // STA $C00D: 80COL on
        8'hAD, 8'h5E, 8'hC0,             // LDA $C05E: normal //e DHGR
                                             // (no //c-only IOUDIS write)
        8'hA9, 8'h5A,                    // LDA #$5A
        8'h8D, 8'h00, 8'h04,             // STA $0400
        8'h4C, 8'h1C, 8'hF0              // JMP $F01C
    };

    task automatic check(input logic condition, input string message);
        if (condition !== 1'b1)
            $fatal(1, "ONEE VIDEO FAIL: %s", message);
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

    task automatic wait_native_tick;
        begin
            do begin
                @(posedge clk);
            end while (!ab_read.sss_en);
            #1;
            check(update_pulse, "native bus tick did not update scanner");
        end
    endtask

    task automatic pulse_frame_zero;
        begin
            @(negedge clk);
            set_frame_zero = 1'b1;
            @(posedge clk);
            #1;
            check(update_pulse && line_in_frame == 9'd0 &&
                  cycle_in_line == 7'd0,
                  "frame-zero pulse did not seed line 0 cycle 0");
            @(negedge clk);
            set_frame_zero = 1'b0;
        end
    endtask

    task automatic pop_record(output AppleCycleRecord record_out);
        int guard;
        begin
            guard = 0;
            while (capture_empty && guard < 16) begin
                @(posedge clk);
                #1;
                guard++;
            end
            check(!capture_empty, "timed out waiting for capture record");
            record_out = capture_data;
            @(negedge clk);
            capture_rd_en = 1'b1;
            @(posedge clk);
            #1;
            @(negedge clk);
            capture_rd_en = 1'b0;
        end
    endtask

    int native_index;
    int line_transitions;
    logic [8:0] previous_line;
    logic saw_vblank_start;
    logic saw_frame_wrap;
    logic saw_text_write;
    logic saw_hires_write;
    logic saw_hgr_state;
    logic saw_dhgr_state;
    AppleCycleRecord record;
    int records_read;

    initial begin
        repeat (8) @(posedge clk);
        resetn = 1'b1;

        // A released hard reset starts at line 0/cycle 0 on the first native
        // bus tick. Check every scanner position through one whole NTSC frame.
        previous_line = 9'd0;
        line_transitions = 0;
        saw_vblank_start = 1'b0;
        saw_frame_wrap = 1'b0;
        for (native_index = 0; native_index <= (65 * 262); native_index++) begin
            wait_native_tick();
            if (native_index < (65 * 262)) begin
                check(line_in_frame == 9'(native_index / 65),
                      $sformatf("line mismatch at native tick %0d", native_index));
                check(cycle_in_line == 7'(native_index % 65),
                      $sformatf("cycle mismatch at native tick %0d", native_index));
                check(video_vblank == (native_index >= (65 * 192)),
                      $sformatf("VBL mismatch at native tick %0d", native_index));
                if (line_in_frame != previous_line) begin
                    line_transitions++;
                    previous_line = line_in_frame;
                end
                if (line_in_frame == 9'd192 && cycle_in_line == 7'd0)
                    saw_vblank_start = 1'b1;
            end else begin
                check(line_in_frame == 9'd0 && cycle_in_line == 7'd0,
                      "NTSC frame did not wrap after 262 lines");
                check(!video_vblank, "VBL remained set after NTSC frame wrap");
                saw_frame_wrap = 1'b1;
            end
        end
        check(line_transitions == 261,
              "scanner did not traverse exactly 262 NTSC lines");
        check(saw_vblank_start, "VBL did not start at line 192 cycle 0");
        check(saw_frame_wrap, "scanner did not return to frame start");

        // The top-level reset-release hook uses this input. It must restart
        // the scanner at frame zero even after the free-running bus advances.
        repeat (37) wait_native_tick();
        check(line_in_frame != 9'd0 || cycle_in_line != 7'd0,
              "scanner did not advance before frame-zero test");
        pulse_frame_zero();
        wait_native_tick();
        check(line_in_frame == 9'd0 && cycle_in_line == 7'd0,
              "first native tick after frame-zero was not frame start");

        // Load and run a small 65C02 program. Its screen writes use vTW's
        // posted queue; its C05x accesses use the shared virtual bus.
        for (int i = 0; i < 31; i++)
            sh_write(ROM_BASE + 18'h3000 + 18'(i), PROGRAM[i]);
        sh_write(ROM_BASE + 18'h3FFC, 8'h00);
        sh_write(ROM_BASE + 18'h3FFD, 8'hF0);
        sh_write(18'h00400, 8'h00);
        sh_write(18'h02000, 8'h00);

        pulse_frame_zero();
        frame_en = 1'b1;
        vtw_enable = 1'b1;
        fork : wait_for_bus
            begin
                wait (bus_owned);
                disable wait_for_bus;
            end
            begin
                #20us;
                $fatal(1, "ONEE VIDEO FAIL: vTW did not acquire virtual bus");
            end
        join
        core_run = 1'b1;

        fork : wait_for_program
            begin
                wait (dbg_core_pc == 16'hF01C && posted_writes >= 32'd2);
                repeat (8) wait_native_tick();
                disable wait_for_program;
            end
            begin
                #200us;
                $fatal(1,
                       "ONEE VIDEO FAIL: program timeout PC=%04X posts=%0d",
                       dbg_core_pc, posted_writes);
            end
        join

        core_run = 1'b0;
        frame_en = 1'b0;
        repeat (8) @(posedge clk);

        check(!sss.sw_text && sss.sw_hires && sss.sw_80col &&
              sss.sw_dhires,
              "normal //e DHGR switches did not reach shared state");
        check(!capture_drop, "capture FIFO dropped ONE//e video records");

        saw_text_write = 1'b0;
        saw_hires_write = 1'b0;
        saw_hgr_state = 1'b0;
        saw_dhgr_state = 1'b0;
        records_read = 0;
        while (!capture_empty && records_read < 1024) begin
            pop_record(record);
            records_read++;

            if (record.record_kind == RECORD_KIND_LEGACY &&
                record.addr_decode_en &&
                record.addr_decode == 24'h000400 &&
                record.data == 8'h5A)
                saw_text_write = 1'b1;

            if (record.record_kind == RECORD_KIND_LEGACY &&
                record.addr_decode_en &&
                record.addr_decode == 24'h002000 &&
                record.data == 8'hA5)
                saw_hires_write = 1'b1;

            if (record.frame_en && !record.sw_text && record.sw_hires &&
                !record.sw_80col && !record.sw_dhires)
                saw_hgr_state = 1'b1;

            if (record.frame_en && !record.sw_text && record.sw_hires &&
                record.sw_80col && record.sw_dhires)
                saw_dhgr_state = 1'b1;

            if (record.frame_en) begin
                check(record.line_in_frame < 9'd262,
                      "capture record carried an invalid NTSC line");
                check(record.cycle_in_line < 7'd65,
                      "capture record carried an invalid native cycle");
            end
        end

        check(records_read < 1024, "capture FIFO did not drain");
        check(saw_text_write,
              "vTW $0400 write did not reach normal cycle capture");
        check(saw_hires_write,
              "vTW $2000 write did not reach normal cycle capture");
        check(saw_hgr_state,
              "normal HGR switch state did not reach renderer metadata");
        check(saw_dhgr_state,
              "C00D/C05E without IOUDIS did not reach DHGR metadata");

        $display("ONEE VIDEO PATH PASS");
        $finish;
    end

    initial begin
        #5ms;
        $fatal(1, "ONEE VIDEO FAIL: timeout");
    end

endmodule
