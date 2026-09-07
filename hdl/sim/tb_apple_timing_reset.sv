`timescale 1ns / 1ps

module tb_apple_timing_reset;
    logic clk = 1'b0;
    always #3.75 clk = ~clk;
    logic [3:0] rstn = '0;
    logic phi0_running = 1'b1;
    logic phi0 = 1'b0;
    always #490 if (phi0_running) phi0 = ~phi0;
    logic aux_clock_running = 1'b1;
    logic aux_clock = 1'b0;
    always #100 if (aux_clock_running) aux_clock = ~aux_clock;
    logic res_n = 1'b0;
    logic host_50hz = 1'b0;
    logic onee_enable_effective = 1'b0;
    wire onee_activity_quiet;
    logic boot_command = 1'b0;
    globals::AppleBus_read physical_ab_read;
    globals::AppleBus_read virtual_ab_read = '0;
    wire globals::AppleBus_read ab_read = onee_enable_effective ?
        virtual_ab_read : physical_ab_read;
    globals::AppleBus_write ab_write = '0;
    tri [7:0] apple_data_pin;
    tri [15:0] apple_addr_pin;
    tri apple_rw_pin, apple_inh_pin, apple_res_pin;
    tri apple_irq_pin, apple_rdy_pin, apple_dma_pin, apple_nmi_pin;
    assign apple_res_pin = res_n ? 1'bz : 1'b0;
    assign (weak0, weak1) apple_data_pin = 8'hFF;
    assign (weak0, weak1) apple_addr_pin = 16'hFFFF;
    assign (weak0, weak1) apple_rw_pin = 1'b1;
    pullup (apple_inh_pin);
    pullup (apple_res_pin);
    pullup (apple_irq_pin);
    pullup (apple_rdy_pin);
    pullup (apple_dma_pin);
    pullup (apple_nmi_pin);

    apple_bus_wrapper bus_i (
        .clk(clk), .rstn(rstn[1]), .physical_bus_isolate(1'b0),
        .dbg_clear(1'b0), .inh_allowed(1'b1),
        .physical_slave_select_ok(1'b1), .physical_irq_allowed(1'b1),
        .gs_m2_qualify(1'b0), .m2sel_active_high(1'b0),
        .host_is_iiplus(1'b0), .iiplus_dma_refresh_active(1'b0),
        .apple_data_pin(apple_data_pin), .apple_addr_pin(apple_addr_pin),
        .apple_rw_pin(apple_rw_pin), .apple_phi0_pin(phi0),
        .apple_m2sel_pin(1'b0), .apple_m2b0_pin(1'b0),
        .apple_devsel_n_pin(1'b1), .apple_inh_pin(apple_inh_pin),
        .apple_res_pin(apple_res_pin), .apple_irq_pin(apple_irq_pin),
        .apple_rdy_pin(apple_rdy_pin), .apple_dma_pin(apple_dma_pin),
        .apple_nmi_pin(apple_nmi_pin), .tini_5v_pin(1'b0),
        .ab_read(physical_ab_read), .ab_write(ab_write)
    );

    apple_timing_reset_harness dut (
        .clk(clk), .rstn(rstn), .ab_read(ab_read),
        .onee_enable_effective(onee_enable_effective),
        .onee_activity_quiet(onee_activity_quiet),
        .onee_video_50hz_active(host_50hz), .host_50hz(host_50hz),
        .boot_command(boot_command)
    );

    onee_mode_safety_guard activity_i (
        .clk(clk), .resetn(rstn[1]), .manual_enable_request(1'b0),
        .apple_power_present_raw(1'b0), .apple_phi0_raw(phi0),
        .apple_7m_raw(aux_clock), .apple_q3_raw(1'b0),
        .apple_m2sel_raw(1'b0), .apple_m2b0_raw(1'b0),
        .apple_devsel_n_raw(1'b1), .apple_reset_n_raw(apple_res_pin),
        .apple_inh_n_raw(apple_inh_pin), .apple_irq_n_raw(apple_irq_pin),
        .apple_nmi_n_raw(apple_nmi_pin), .apple_rdy_n_raw(apple_rdy_pin),
        .apple_dma_n_raw(apple_dma_pin),
        .apple_activity_quiet(onee_activity_quiet)
    );

    task automatic check(input logic condition, input string message);
        if (condition !== 1'b1) $fatal(1, "%s", message);
    endtask

    // Independent PHI0-strobe count. Compare every fabric clock, including
    // clocks without a bus strobe, so a reset-induced update cannot hide.
    bit tracking = 1'b0;
    int next_line, next_cycle, shown_line, shown_cycle;
    int ticks, reset_ticks, vbl_edges;
    logic expected_update;
    always @(posedge clk) begin
        if (tracking) begin
            expected_update = physical_ab_read.sss_en;
            if (expected_update) begin
                shown_line = next_line;
                shown_cycle = next_cycle;
                ticks++;
                if (!physical_ab_read.res) reset_ticks++;
                if (next_cycle == 64) begin
                    next_cycle = 0;
                    next_line = (next_line == (host_50hz ? 311 : 261)) ?
                        0 : next_line + 1;
                end else next_cycle++;
            end
            #1;
            check(dut.update_pulse === expected_update,
                  "scanner update without a PHI0 strobe (or lost strobe)");
            check(dut.line_in_frame == shown_line &&
                  dut.cycle_in_line == shown_cycle,
                  $sformatf("scanner phase changed: got %0d:%0d expected %0d:%0d",
                            dut.line_in_frame, dut.cycle_in_line,
                            shown_line, shown_cycle));
            check(dut.apple_vblank_start_pulse ===
                  (expected_update && shown_line == 192 && shown_cycle == 0),
                  "VBL heartbeat changed phase");
            if (dut.apple_vblank_start_pulse) vbl_edges++;
        end
    end

    task automatic start_tracking(input int line_num, input int cycle_num);
        next_line = line_num;
        next_cycle = cycle_num;
        shown_line = line_num;
        shown_cycle = cycle_num;
        tracking = 1'b1;
    endtask

    task automatic send_command;
        @(negedge clk);
        boot_command = 1'b1;
        @(negedge clk);
        boot_command = 1'b0;
    endtask

    task automatic calibrate;
        @(negedge clk);
        tracking = 1'b0;
        send_command();
        check(dut.line_in_frame == 192 && dut.cycle_in_line == 15,
              "first boot-ROM command did not calibrate the scanner");
        check(dut.apple_vblank_lock_seen_q,
              "first boot-ROM command did not latch the phase lock");
        start_tracking(192, 15);
    endtask

    task automatic wait_position(input int line_num, input int cycle_num);
        do @(negedge clk);
        while (dut.line_in_frame != line_num || dut.cycle_in_line != cycle_num);
    endtask

    task automatic physical_reset(input int cycles_held, input int offset_ns);
        # (offset_ns);
        res_n = 1'b0;
        repeat (cycles_held) @(negedge phi0);
        #137;
        res_n = 1'b1;
        repeat (10) @(negedge phi0);
        check(physical_ab_read.res, "filtered RES# did not release");
        check(dut.apple_vblank_lock_seen_q,
              "physical CTRL-RESET discarded the VBL phase lock");
        send_command();
        repeat (3) @(negedge phi0);
    endtask

    initial begin
        ticks = 0;
        reset_ticks = 0;
        vbl_edges = 0;
        for (int standard = 0; standard < 2; standard++) begin
            @(negedge clk);
            tracking = 1'b0;
            rstn = '0;
            res_n = 1'b0;
            host_50hz = (standard == 1);
            repeat (8) @(negedge clk);
            rstn = '1;
            start_tracking(0, 0);
            repeat (13) @(negedge phi0);
            res_n = 1'b1;
            repeat (12) @(negedge phi0);
            check(!dut.apple_vblank_lock_seen_q,
                  "physical reset consumed the first calibration");
            calibrate();
            repeat (101) @(negedge phi0);
            send_command();
            wait_position(10, 20);
            physical_reset(17, 43);
            wait_position(191, 62);
            physical_reset(15, 271);
            wait_position(host_50hz ? 311 : 261, 62);
            physical_reset(19, 617);
            physical_reset((host_50hz ? 312 : 262) * 65 + 111, 89);

            // Stop PHI0, drain its pending strobe, then toggle both RES#
            // edges. Neither edge may advance or reseed the scanner.
            @(negedge phi0);
            phi0_running = 1'b0;
            #5000;
            res_n = 1'b0;
            #10000;
            res_n = 1'b1;
            #10000;
            send_command();
            #1000;
            phi0_running = 1'b1;
            repeat (20) @(negedge phi0);
        end
        check(ticks > 70000 && reset_ticks > 35000 && vbl_edges >= 4,
              "reset/frame coverage did not run");

        // Host power-off stops all slot clocks. The existing activity guard
        // then permits calibration on the next boot, without a phase reset.
        @(negedge phi0);
        phi0_running = 1'b0;
        aux_clock_running = 1'b0;
        #5000;
        @(negedge clk);
        check(onee_activity_quiet && !dut.apple_vblank_lock_seen_q,
              "host clock loss did not rearm calibration");
        aux_clock_running = 1'b1;
        phi0_running = 1'b1;
        repeat (20) @(negedge phi0);
        calibrate();

        // Keep the existing stand-alone reset-release contract and allow a
        // new physical phase lock when returning from the virtual clock.
        @(negedge clk);
        tracking = 1'b0;
        onee_enable_effective = 1'b1;
        virtual_ab_read.res = 1'b0;
        repeat (5) @(negedge clk);
        check(!dut.apple_vblank_lock_seen_q,
              "virtual reset did not rearm calibration");
        virtual_ab_read.res = 1'b1;
        repeat (3) @(negedge clk);
        check(dut.line_in_frame == 0 && dut.cycle_in_line == 0,
              "ONE//e reset release lost its frame-zero seed");
        send_command();
        check(dut.apple_vblank_lock_seen_q,
              "ONE//e first command did not calibrate");
        virtual_ab_read.res = 1'b0;
        repeat (4) @(negedge clk);
        virtual_ab_read.res = 1'b1;
        repeat (4) @(negedge clk);
        check(!dut.apple_vblank_lock_seen_q && dut.line_in_frame == 0 &&
              dut.cycle_in_line == 0, "ONE//e warm reset changed behavior");
        send_command();
        onee_enable_effective = 1'b0;
        repeat (4) @(negedge clk);
        check(!dut.apple_vblank_lock_seen_q,
              "return to physical PHI0 did not rearm calibration");
        calibrate();
        repeat (20) @(negedge phi0);
        $display("APPLE TIMING RESET PASS ticks=%0d reset_ticks=%0d VBL=%0d",
                 ticks, reset_ticks, vbl_edges);
        $finish;
    end

    initial begin
        #300_000_000;
        $fatal(1, "APPLE TIMING RESET TIMEOUT");
    end
endmodule
