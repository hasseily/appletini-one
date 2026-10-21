`timescale 1ns / 1ps

// The real virtual bus and Mockingboard, including both native SSI engines.
// Physical Q3 stays stopped while ONE//e supplies its own Apple bus cadence.
module tb_onee_ssi263_clock;
    `define PRIMARY card_i.ssi263_primary_i.bus_wrapper_i
    `define SECONDARY card_i.ssi263_secondary_i.bus_wrapper_i
    `define PENGINE `PRIMARY.native_engine_i
    `define SENGINE `SECONDARY.native_engine_i

    logic clk = 0, rstn = 0, standalone = 0, pal = 0, q3 = 0;
    logic req_valid = 0, req_ready, req_rw = 1, resp_valid;
    logic [15:0] req_addr = 16'hFFFF;
    logic [7:0] req_data = 0;
    logic audio_tick = 0;
    logic signed [15:0] audio_l, audio_r;
    globals::AppleBus_read bus;
    globals::AppleBus_write card_write;
    globals::SoftSwitchState sss;
    integer fabric_cycle = 0, audio_div = 0;
    integer checks = 0, transfers = 0, xck_count = 0, effective_count = 0;
    integer minimum_xck = 1000000, minimum_effective = 1000000;
    integer last_xck = -1, last_effective = -1;
    integer primary_samples = 0, secondary_samples = 0, mixed_samples = 0;
    integer standard_cases = 0, transition_cases = 0, pending_cases = 0;
    logic previous_xck = 0;
    logic primary_progress = 0, secondary_progress = 0;
    logic primary_done = 0, secondary_done = 0;

    always #3.75 clk = ~clk;
    always @(negedge clk) begin
        if (!rstn) begin
            audio_div = 0;
            audio_tick = 0;
        end else begin
            audio_div = (audio_div + 1) % 2778;
            audio_tick = (audio_div == 0);
        end
    end

    apple_virtual_bus virtual_bus_i (
        .clk(clk), .resetn(rstn), .video_mode_50hz(pal),
        .res_n_in(1'b1), .irq_n_in(1'b1), .nmi_n_in(1'b1),
        .rdy_n_in(1'b1), .dma_n_in(1'b1), .inh_n_in(1'b1),
        .req_valid(req_valid), .req_ready(req_ready), .req_addr(req_addr),
        .req_rw(req_rw), .req_wdata(req_data), .resp_valid(resp_valid),
        .resp_rdata(), .floating_bus_data(8'hFF), .ab_write(card_write),
        .ab_read(bus), .data_phase_data()
    );
    soft_switch_manager soft_switch_i (
        .clk(clk), .rstn(rstn), .ramworks_en(1'b0), .ab_read(bus), .sss(sss)
    );
    mockingboard card_i (
        .clk(clk), .rstn(rstn), .apple_q3_raw(q3), .standalone(standalone),
        .ab_read(bus), .sss(sss), .slot_assign(3'd4),
        .pan(48'h888888888888), .ssi_pan(8'hF0),
        .audio_control(32'h10000000), .audio_sample_tick(audio_tick),
        .ab_write(card_write), .audio_l(audio_l), .audio_r(audio_r),
        .dbg_ssi_irq(), .dbg_ssi_backend_done(), .dbg_ssi_enable_ints()
    );

    task automatic require(input logic condition, input string message);
        checks = checks + 1;
        if (condition !== 1'b1)
            $fatal(1, "ONEE SSI263 CLOCK FAIL: %s time=%0t cycle=%0d standalone=%0d PAL=%0d",
                message, $time, fabric_cycle, standalone, pal);
    endtask

    always @(posedge clk) begin
        fabric_cycle = fabric_cycle + 1;
        #1;
        if (!rstn) begin
            last_xck = -1;
            last_effective = -1;
            previous_xck = 0;
            primary_samples = 0;
            secondary_samples = 0;
            mixed_samples = 0;
            primary_progress = 0;
            secondary_progress = 0;
            primary_done = 0;
            secondary_done = 0;
            require(!card_i.ssi_xck_ce && !card_i.ssi_xck_ce_i.rise_pending_q,
                "reset must discard output and pending SSI clocks");
        end else begin
            require(!$isunknown({card_i.ssi_xck_ce, `PRIMARY.native_fault,
                `SECONDARY.native_fault, card_i.ssi0_direct_irq, card_i.ssi1_direct_irq}),
                "SSI clock, scheduling and completion outputs must be known");
            require(!`PRIMARY.native_fault && !`SECONDARY.native_fault,
                "a native SSI engine missed its work interval");
            if (card_i.ssi_xck_ce) begin
                xck_count = xck_count + 1;
                require(!previous_xck, "SSI XCK must last one fabric clock");
                if (last_xck >= 0) begin
                    require(fabric_cycle - last_xck >= 62, "source change compressed XCK below 62 clocks");
                    if (fabric_cycle - last_xck < minimum_xck) minimum_xck = fabric_cycle - last_xck;
                end
                last_xck = fabric_cycle;
            end
            previous_xck = card_i.ssi_xck_ce;
            if (`PENGINE.effective_ce) begin
                effective_count = effective_count + 1;
                if (last_effective >= 0) begin
                    require(fabric_cycle - last_effective >= 124, "DIV2 interval fell below 124 clocks");
                    if (fabric_cycle - last_effective < minimum_effective)
                        minimum_effective = fabric_cycle - last_effective;
                end
                last_effective = fabric_cycle;
            end
            if (`PENGINE.duration_phase != 0) primary_progress = 1;
            if (`SENGINE.duration_phase != 0) secondary_progress = 1;
            if (`PRIMARY.backend_done) primary_done = 1;
            if (`SECONDARY.backend_done) secondary_done = 1;
            if (card_i.ssi1_audio_valid && card_i.ssi1_audio != 0) primary_samples = primary_samples + 1;
            if (card_i.ssi0_audio_valid && card_i.ssi0_audio != 0) secondary_samples = secondary_samples + 1;
            if (audio_tick && (audio_l != 0 || audio_r != 0)) mixed_samples = mixed_samples + 1;
        end
    end

    task automatic reset_card(input logic enabled, input logic standard, input logic q3_level);
        @(negedge clk);
        rstn = 0;
        standalone = enabled;
        pal = standard;
        q3 = q3_level;
        req_valid = 0;
        repeat (8) @(negedge clk);
        rstn = 1;
        repeat (160) @(negedge clk);
        require(card_i.phasor_mode_q == 0, "card reset must select Mockingboard mode");
    endtask

    task automatic access_bus(input logic [15:0] address, input logic [7:0] value,
        input logic reading);
        integer guard;
        @(negedge clk);
        req_addr = address;
        req_data = value;
        req_rw = reading;
        req_valid = 1;
        guard = 0;
        do begin
            @(posedge clk);
            guard = guard + 1;
            if (guard > 400) $fatal(1, "ONEE SSI263 CLOCK FAIL: virtual request timeout");
        end while (!req_ready);
        @(negedge clk);
        req_valid = 0;
        guard = 0;
        do begin
            @(negedge clk);
            guard = guard + 1;
            if (guard > 400) $fatal(1, "ONEE SSI263 CLOCK FAIL: virtual response timeout");
        end while (!resp_valid);
        repeat (8) @(negedge clk);
        transfers = transfers + 1;
    endtask

    task automatic program_both_voices;
        access_bus(16'hC0CD, 0, 1);
        require(card_i.phasor_mode_q == 5, "virtual bus must select native Phasor mode");
        access_bus(16'hC463, 8'hFF, 0);
        access_bus(16'hC460, 8'h80, 0);
        access_bus(16'hC461, 8'hEC, 0);
        access_bus(16'hC462, 8'hF8, 0);
        access_bus(16'hC464, 8'h80, 0);
        access_bus(16'hC463, 8'h7F, 0);
        access_bus(16'hC460, 8'hCE, 0); // AH, fastest duration/rate; both sockets.
        require(`PRIMARY.duration_phoneme_q == 8'hCE && `SECONDARY.duration_phoneme_q == 8'hCE &&
            `PRIMARY.rate_inflection_q == 8'hF8 && `SECONDARY.rate_inflection_q == 8'hF8,
            "real virtual-bus writes must reach both SSI register sets");
        require(`PRIMARY.current_enable_ints_q && `SECONDARY.current_enable_ints_q,
            "both voices must enable phoneme completion IRQs");
    endtask

    task automatic check_stopped_physical;
        integer before_xck;
        logic [14:0] primary_left, secondary_left;
        reset_card(0, 0, 0);
        program_both_voices();
        before_xck = xck_count;
        primary_left = `PENGINE.controller_i.debug_duration_left;
        secondary_left = `SENGINE.controller_i.debug_duration_left;
        repeat (3000) @(negedge clk);
        require(xck_count == before_xck && !primary_done && !secondary_done &&
            `PENGINE.controller_i.debug_duration_left == primary_left &&
            `SENGINE.controller_i.debug_duration_left == secondary_left,
            "physical mode must not invent speech clocks when Q3 stays low");
        q3 = 1;
        repeat (100) @(negedge clk); // The one real rising edge may qualify once.
        before_xck = xck_count;
        primary_left = `PENGINE.controller_i.debug_duration_left;
        secondary_left = `SENGINE.controller_i.debug_duration_left;
        repeat (3000) @(negedge clk);
        require(xck_count == before_xck && !primary_done && !secondary_done &&
            `PENGINE.controller_i.debug_duration_left == primary_left &&
            `SENGINE.controller_i.debug_duration_left == secondary_left,
            "physical mode must not invent speech clocks when Q3 stays high");
        $display("ONEE SSI263 CLOCK STOPPED physical_low=PASS physical_high=PASS");
    endtask

    task automatic check_standard(input logic standard);
        integer before_xck, before_effective, period, elapsed;
        reset_card(1, standard, standard); // NTSC Q3 low; PAL Q3 high.
        program_both_voices();
        period = standard ? 131 : 130;
        // No CPU requests during this window: idle bus cycles must still clock speech.
        before_xck = xck_count;
        before_effective = effective_count;
        repeat (64 * period) @(negedge clk);
        require(xck_count - before_xck == 128 && effective_count - before_effective == 64,
            "standalone XCK and DIV2 rate must follow idle virtual bus cadence");
        elapsed = 0;
        while (!(primary_done && secondary_done && card_i.ssi0_direct_irq && card_i.ssi1_direct_irq &&
                 primary_samples > 0 && secondary_samples > 0 && mixed_samples > 0) &&
               elapsed < 6000000) begin
            @(negedge clk);
            elapsed = elapsed + 1;
        end
        repeat (8) @(negedge clk);
        $display("ONEE SSI263 CLOCK AUDIO PAL=%0d elapsed=%0d primary=%0d secondary=%0d mixed=%0d codes=%h/%h",
            standard, elapsed, primary_samples, secondary_samples, mixed_samples,
            `PENGINE.control_codes, `SENGINE.control_codes);
        require(primary_progress && secondary_progress, "both native SSI duration engines must advance");
        require(primary_samples > 0 && secondary_samples > 0 && mixed_samples > 0,
            "both SSI voices and the card audio mix must produce nonzero audio");
        require(primary_done && secondary_done && card_i.ssi0_direct_irq && card_i.ssi1_direct_irq &&
            card_write.assert_irq && !bus.irq, "both completions must reach the real virtual IRQ line");
        require(!req_valid, "speech proof must run without a held CPU request");
        standard_cases = standard_cases + 1;
        $display("ONEE SSI263 CLOCK STANDARD PAL=%0d period=%0d elapsed=%0d primary_audio=%0d secondary_audio=%0d mixed_audio=%0d",
            standard, period, elapsed, primary_samples, secondary_samples, mixed_samples);
    endtask

    task automatic make_pending_clock;
        integer guard;
        @(negedge clk);
        standalone = 0;
        q3 = 0;
        repeat (90) @(negedge clk);
        q3 = 1;
        guard = 0;
        do begin
            @(negedge clk);
            guard = guard + 1;
            if (guard > 100) $fatal(1, "ONEE SSI263 CLOCK FAIL: physical XCK missing");
        end while (!card_i.ssi_xck_ce);
        q3 = 0;
        repeat (14) @(negedge clk);
        q3 = 1;
        guard = 0;
        do begin
            @(negedge clk);
            guard = guard + 1;
            if (guard > 30) $fatal(1, "ONEE SSI263 CLOCK FAIL: early physical edge did not pend");
        end while (!card_i.ssi_xck_ce_i.rise_pending_q);
        pending_cases = pending_cases + 1;
    endtask

    task automatic check_transitions;
        integer before_xck, change_cycle;
        // Exercise every virtual-bus phase with a source change and nearby Q3 edge.
        for (integer offset = 0; offset < 131; offset = offset + 1) begin
            @(negedge clk);
            standalone = 1;
            q3 = 0;
            repeat (131 + offset) @(negedge clk);
            standalone = 0;
            q3 = 1;
            repeat (17 + offset % 40) @(negedge clk);
            standalone = 1;
            repeat (150) @(negedge clk);
            transition_cases = transition_cases + 1;
        end
        make_pending_clock();
        before_xck = xck_count;
        change_cycle = fabric_cycle;
        standalone = 1;
        repeat (61) @(negedge clk);
        require(xck_count == before_xck, "source change must flush the old pending clock and retain the guard");
        repeat (150) @(negedge clk);
        require(xck_count > before_xck && fabric_cycle - change_cycle > 62,
            "new virtual source must resume after its safe gap");
        make_pending_clock();
        rstn = 0;
        repeat (8) @(negedge clk);
        standalone = 1;
        rstn = 1;
        repeat (300) @(negedge clk);
        require(!card_i.ssi_xck_ce_i.rise_pending_q || card_i.ssi_xck_ce_i.spacing_q != 0,
            "reset must not preserve an old pending edge");
        require(minimum_xck >= 62 && minimum_effective >= 124,
            "all source transitions must respect native engine service intervals");
    endtask

    initial begin
        check_stopped_physical();
        check_standard(0);
        check_standard(1);
        check_transitions();
        $display("ONEE SSI263 CLOCK PASS checks=%0d standards=%0d transitions=%0d pending=%0d transfers=%0d min_xck=%0d min_div2=%0d",
            checks, standard_cases, transition_cases, pending_cases, transfers, minimum_xck, minimum_effective);
        $finish;
    end
    initial begin
        #150000000;
        $fatal(1, "ONEE SSI263 CLOCK FAIL: bounded timeout");
    end
endmodule
