`timescale 1ns / 1ps
module tb_video_output_modes;
    import video_pkg::*;
    logic clk = 0;
    always #5 clk = ~clk;
    logic resetn = 0;
    logic [3:0] mode = 4;
    logic pal = 0;
    wire hsync, vsync, de, vblank;
    wire [11:0] h, v;
    video_timing_gen timing (
        .clk_pixel(clk), .rst_n(resetn), .output_mode(mode),
        .mode_1080p50(pal), .genlock_vblank_start(1'b0),
        .hsync(hsync), .vsync(vsync), .de(de),
        .h_count(h), .v_count(v), .vblank_start(vblank)
    );
    int widths[VIDEO_MODE_COUNT] = '{1024, 1200, 1280, 1680, 1920, 1360};
    int heights[VIDEO_MODE_COUNT] = '{768, 800, 1024, 1050, 1080, 768};
    int ht[VIDEO_MODE_COUNT] = '{1344, 1360, 1688, 1840, 2200, 1792};
    int vt60[VIDEO_MODE_COUNT] = '{806, 828, 1066, 1080, 1125, 795};
    int vt50[VIDEO_MODE_COUNT] = '{967, 993, 1280, 1293, 1125, 954};
    int hs[VIDEO_MODE_COUNT] = '{136, 32, 112, 32, 44, 112};
    int vs[VIDEO_MODE_COUNT] = '{6, 10, 3, 6, 5, 6};
    bit hp[VIDEO_MODE_COUNT] = '{0, 1, 1, 1, 1, 1};
    bit vp[VIDEO_MODE_COUNT] = '{0, 0, 1, 0, 1, 1};
    int pixels, cycles, hsync_cycles, vsync_cycles, vblanks;
    int total_h, total_v;

    initial begin
        for (int m = 0; m < VIDEO_MODE_COUNT; m++) begin
            for (int standard = 0; standard < 2; standard++) begin
                @(negedge clk);
                resetn = 0;
                mode = 4'(m);
                pal = 1'(standard);
                repeat (8) @(negedge clk);
                resetn = 1;
                do @(negedge clk); while (h != 0 || v != 0);
                pixels = 0; cycles = 0; hsync_cycles = 0;
                vsync_cycles = 0; vblanks = 0;
                total_h = m == 4 && standard ? 2640 : ht[m];
                total_v = standard ? vt50[m] : vt60[m];
                do begin
                    pixels += int'(de);
                    hsync_cycles += int'(hsync == hp[m]);
                    vsync_cycles += int'(vsync == vp[m]);
                    vblanks += int'(vblank);
                    cycles++;
                    @(negedge clk);
                end while (h != 0 || v != 0);
                if (pixels != widths[m] * heights[m] || cycles != total_h * total_v ||
                    hsync_cycles != hs[m] * total_v ||
                    vsync_cycles != vs[m] * total_h || vblanks != 1)
                    $fatal(1, "mode %0d PAL %0d pixels=%0d cycles=%0d hs=%0d vs=%0d vb=%0d",
                        m, standard, pixels, cycles, hsync_cycles, vsync_cycles, vblanks);
                if (video_frame_bursts(4'(m)) * 128 < widths[m] * heights[m] * 2 ||
                    video_frame_bursts(4'(m)) * 128 - widths[m] * heights[m] * 2 >= 128)
                    $fatal(1, "invalid final-burst rounding");
            end
        end
        $display("VIDEO OUTPUT TIMING PASS");
        $finish;
    end
    initial begin
        #300000000;
        $fatal(1, "timing test timeout");
    end
endmodule

module tb_video_mode_control;
    import video_pkg::*;
    logic clk = 0;
    always #5 clk = ~clk;
    logic resetn = 0, request = 0;
    logic [3:0] requested_mode = 4;
    logic [31:0] requested_base = 32'h3E400000;
    logic quiescent = 0, frame_latched = 0, locked = 1;
    wire [3:0] active_mode;
    wire [31:0] committed_base;
    wire commit, busy, error, pause_reader, hold_video, clock_resetn;
    wire [10:0] awaddr;
    wire awvalid, wvalid, bready;
    wire [31:0] wdata;
    logic awready = 0, wready = 0, bvalid = 0;
    logic [1:0] bresp = 0;
    video_mode_control #(.TIMEOUT_CYCLES(512)) dut (
        .clk(clk), .resetn(resetn), .request(request),
        .requested_mode(requested_mode), .requested_base(requested_base),
        .reader_quiescent(quiescent), .frame_latched(frame_latched),
        .clock_locked(locked), .active_mode(active_mode),
        .committed_base(committed_base), .commit(commit), .busy(busy),
        .error(error), .reader_pause(pause_reader), .video_hold(hold_video),
        .clock_resetn(clock_resetn), .clock_awaddr(awaddr), .clock_awvalid(awvalid),
        .clock_awready(awready), .clock_wdata(wdata), .clock_wvalid(wvalid),
        .clock_wready(wready), .clock_bresp(bresp), .clock_bvalid(bvalid),
        .clock_bready(bready)
    );
    int cycle = 0, writes = 0, lock_timer = 0, register_index = 0;
    int reset_events = 0;
    logic aw_seen = 0, w_seen = 0, stall_clock = 0, stall_aw = 0;
    logic inject_bresp_error = 0;
    logic [10:0] saved_addr;
    logic [31:0] saved_data;
    logic [31:0] staged_feedback, staged_output;
    // Like the real MMCM, the running DRP configuration survives reset.
    logic [31:0] running_feedback = video_clock_feedback(VIDEO_DEFAULT_MODE);
    logic [31:0] running_output = video_clock_output(VIDEO_DEFAULT_MODE);
    real expected_clock[VIDEO_MODE_COUNT] = '{65.0, 67.5, 108.0, 119.0, 148.5, 85.5};
    real running_clock;
    always @(negedge clock_resetn) reset_events++;
    always @(negedge clk) if (commit) begin
        running_clock = 150.0 * (real'(running_feedback[15:8]) +
            real'(running_feedback[25:16]) / 1000.0) /
            real'(running_feedback[7:0]) /
            (real'(running_output[7:0]) + real'(running_output[17:8]) / 1000.0);
        if (running_clock < expected_clock[active_mode] - 0.001 ||
            running_clock > expected_clock[active_mode] + 0.001)
            $fatal(1, "committed mode %0d at wrong pixel clock %f", active_mode, running_clock);
    end
    always @(negedge clk) begin
        cycle++;
        awready = !stall_clock && !stall_aw && cycle % 3 == 0;
        wready = !stall_clock && cycle % 3 == 1;
    end
    always @(posedge clk) begin
        if (!clock_resetn) begin
            aw_seen <= 0; w_seen <= 0; bvalid <= 0;
            locked <= 0;
            lock_timer <= 8;
            register_index <= 0;
            bresp <= 0;
        end else begin
            if (awvalid && awready) begin aw_seen <= 1; saved_addr <= awaddr; end
            if (wvalid && wready) begin w_seen <= 1; saved_data <= wdata; end
            if (bvalid && bready) bvalid <= 0;
            if (aw_seen && w_seen && !bvalid) begin
                if (saved_addr != 11'h200 + register_index * 4)
                    $fatal(1, "clock register ordering/duplicate write");
                if (saved_addr == 11'h200) staged_feedback <= saved_data;
                if (saved_addr == 11'h208) staged_output <= saved_data;
                if (saved_addr == 11'h25c) begin
                    if (saved_data != 3) $fatal(1, "clock LOAD value");
                    locked <= 0;
                    lock_timer <= 12;
                    running_feedback <= staged_feedback;
                    running_output <= staged_output;
                end
                writes++;
                register_index <= register_index == 23 ? 0 : register_index + 1;
                bresp <= inject_bresp_error ? 2'b10 : 2'b00;
                bvalid <= 1; aw_seen <= 0; w_seen <= 0;
            end
            if (lock_timer != 0) begin
                lock_timer <= lock_timer - 1;
                if (lock_timer == 1) locked <= 1;
            end
        end
    end
    task automatic start_mode(input logic [3:0] id);
        @(negedge clk); requested_mode = id; request = 1;
        @(negedge clk); request = 0;
    endtask
    task automatic finish_frame;
        wait (!hold_video);
        repeat (5) @(negedge clk);
        frame_latched = 1;
        @(negedge clk); frame_latched = 0;
        wait (!busy);
    endtask
    int failure_writes, failure_resets;
    initial begin
        repeat (4) @(negedge clk);
        resetn = 1;
        wait (locked);
        for (int m = 0; m < VIDEO_MODE_COUNT; m++) begin
            quiescent = 0;
            start_mode(4'(m));
            repeat (12) @(negedge clk);
            if (!busy || !pause_reader || !hold_video || awvalid)
                $fatal(1, "must drain before touching clock");
            quiescent = 1;
            wait (commit);
            if (active_mode != m || committed_base != requested_base || !locked)
                $fatal(1, "mode/frame commit requires relock");
            finish_frame();
            if (error || writes != (m + 1) * 24) $fatal(1, "mode apply failed");
        end
        start_mode(15);
        repeat (4) @(negedge clk);
        if (busy || !error || active_mode != VIDEO_MODE_COUNT - 1)
            $fatal(1, "invalid mode changed scanout");
        stall_clock = 1;
        start_mode(0);
        wait (!clock_resetn);
        stall_clock = 0;
        wait (commit);
        if (active_mode != VIDEO_DEFAULT_MODE || !error)
            $fatal(1, "timeout did not recover default clock");
        finish_frame();
        // Fail after the low pixel clock is already running. Reset alone
        // cannot restore 1080p: recovery must write the default dividers.
        start_mode(0);
        wait (commit);
        wait (!commit);
        wait (!clock_resetn);
        wait (commit);
        if (active_mode != VIDEO_DEFAULT_MODE || !error ||
            running_feedback != video_clock_feedback(VIDEO_DEFAULT_MODE) ||
            running_output != video_clock_output(VIDEO_DEFAULT_MODE))
            $fatal(1, "late failure did not reprogram the default clock");
        finish_frame();
        // Both the requested config and its fallback return SLVERR.
        // Permit one reset/retry, then stop in held FAILED without looping.
        failure_writes = writes;
        failure_resets = reset_events;
        inject_bresp_error = 1;
        start_mode(2);
        wait (busy);
        wait (!busy);
        if (!error || !hold_video || !pause_reader ||
            writes - failure_writes != 2 || reset_events - failure_resets != 1)
            $fatal(1, "repeated BRESP error did not terminate after one fallback");
        repeat (1100) @(negedge clk);
        if (busy || !hold_video || writes - failure_writes != 2 ||
            reset_events - failure_resets != 1)
            $fatal(1, "terminal fallback failure retried without a new request");
        inject_bresp_error = 0;
        start_mode(2);
        wait (commit);
        if (active_mode != 2 || error)
            $fatal(1, "new request cannot recover from held failure");
        finish_frame();
        stall_aw = 1;
        start_mode(0);
        wait (w_seen && awvalid && !wvalid);
        wait (!busy);
        if (!error || !hold_video || !w_seen || !awvalid)
            $fatal(1, "half-accepted clock write did not reach held timeout");
        start_mode(1);
        wait (!clock_resetn);
        stall_aw = 0;
        wait (commit);
        if (active_mode != 1 || committed_base != requested_base || error)
            $fatal(1, "stale AXI half-write leaked into new request");
        finish_frame();
        $display("VIDEO MODE CONTROL PASS");
        $finish;
    end
    initial begin
        #300000;
        $fatal(1, "mode controller test timeout");
    end
endmodule
