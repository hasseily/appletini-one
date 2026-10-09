`timescale 1ns / 1ps

module tb_ssi263_xck_ce;

    logic clk = 1'b0;
    logic rstn = 1'b0;
    logic q3_raw = 1'b0;
    logic q3_ideal = 1'b0;
    logic xck_ce;

    integer raw_rises = 0;
    integer ideal_rises = 0;
    integer reset_high_restarts = 0;
    integer enable_pulses = 0;
    integer spacing_errors = 0;
    integer failures = 0;
    logic xck_ce_q = 1'b0;
    bit check_spacing = 1'b0;
    bit first_enable = 1'b1;
    realtime expected_period;
    realtime last_enable_time;
    realtime enable_interval;
    integer fabric_cycles = 0;
    integer last_rate_enable_cycle = 0;
    integer last_div2_enable_cycle = 0;
    integer rate_spacing_errors = 0;
    integer div2_spacing_errors = 0;
    integer minimum_raw_gap = 1000000;
    integer minimum_div2_gap = 1000000;
    bit check_rate_limits = 1'b0;
    bit rate_enable_seen = 1'b0;
    bit div2_enable_seen = 1'b0;
    bit div2_phase = 1'b0;
    bit div2_parity = 1'b0;

    // The production fabric runs at 133.333 MHz. Physical Q3 is about 2 MHz.
    always #3.75ns clk = ~clk;

    ssi263_xck_ce dut (
        .clk(clk),
        .rstn(rstn),
        .q3_raw(q3_raw),
        .xck_ce(xck_ce)
    );

    always @(posedge q3_raw) begin
        if (rstn)
            raw_rises = raw_rises + 1;
    end

    always @(posedge q3_ideal) begin
        if (rstn)
            ideal_rises = ideal_rises + 1;
    end

    always @(posedge clk) begin
        fabric_cycles = fabric_cycles + 1;
        if (!rstn) begin
            xck_ce_q = 1'b0;
        end else begin
            if ($isunknown(xck_ce)) begin
                failures = failures + 1;
                $display("SSI263 XCK FAIL: unknown enable");
            end
            if (xck_ce) begin
                enable_pulses = enable_pulses + 1;
                if (xck_ce_q) begin
                    failures = failures + 1;
                    $display("SSI263 XCK FAIL: enable lasted over one clock");
                end
                if (check_spacing && !first_enable) begin
                    enable_interval = $realtime - last_enable_time;
                    // Input edges need not align with the fabric clock.
                    if (enable_interval < expected_period - 7.501ns ||
                        enable_interval > expected_period + 7.501ns)
                        spacing_errors = spacing_errors + 1;
                end
                if (check_rate_limits) begin
                    if (rate_enable_seen) begin
                        if (fabric_cycles - last_rate_enable_cycle < 62)
                            rate_spacing_errors = rate_spacing_errors + 1;
                        if (fabric_cycles - last_rate_enable_cycle < minimum_raw_gap)
                            minimum_raw_gap = fabric_cycles - last_rate_enable_cycle;
                    end
                    last_rate_enable_cycle = fabric_cycles;
                    rate_enable_seen = 1'b1;
                    // SSI DIV2 can start on either raw-enable parity.
                    if (div2_phase == div2_parity) begin
                        if (div2_enable_seen) begin
                            if (fabric_cycles - last_div2_enable_cycle < 124)
                                div2_spacing_errors = div2_spacing_errors + 1;
                            if (fabric_cycles - last_div2_enable_cycle < minimum_div2_gap)
                                minimum_div2_gap = fabric_cycles - last_div2_enable_cycle;
                        end
                        last_div2_enable_cycle = fabric_cycles;
                        div2_enable_seen = 1'b1;
                    end
                    div2_phase = !div2_phase;
                end
                first_enable = 1'b0;
                last_enable_time = $realtime;
            end
            xck_ce_q = xck_ce;
        end
    end

    task automatic check_count(
        input string label_text,
        input integer first_count,
        input integer expected_count
    );
        begin
            if (enable_pulses - first_count != expected_count) begin
                failures = failures + 1;
                $display("SSI263 XCK FAIL: %s enables=%0d expected=%0d",
                         label_text, enable_pulses - first_count,
                         expected_count);
            end
        end
    endtask

    task automatic sweep_ringing_offsets;
        integer parity;
        integer width_ticks;
        integer offset_ticks;
        integer tick;
        integer first_count;
        integer first_ideal;
        integer first_rate_errors;
        integer first_div2_errors;
        integer sweep_cases;
        bit ringing_active;
        string label_text;
        begin
            sweep_cases = 0;
            for (parity = 0; parity < 2; parity = parity + 1) begin
                for (width_ticks = 1; width_ticks <= 6;
                     width_ticks = width_ticks + 1) begin
                    for (offset_ticks = 0; offset_ticks < 65;
                         offset_ticks = offset_ticks + 1) begin
                        @(negedge clk);
                        first_count = enable_pulses;
                        first_ideal = ideal_rises;
                        first_rate_errors = rate_spacing_errors;
                        first_div2_errors = div2_spacing_errors;
                        rate_enable_seen = 1'b0;
                        div2_enable_seen = 1'b0;
                        div2_phase = 1'b0;
                        div2_parity = (parity != 0);
                        check_rate_limits = 1'b1;
                        // Six Q3 periods: clean/noisy/clean/noisy/clean/clean.
                        // A late glitch may continue into the next period.
                        // This also checks that late qualification cannot
                        // compress the following SSI engine service interval.
                        for (tick = 0; tick < 6 * 65; tick = tick + 1) begin
                            q3_ideal = ((tick % 65) < 37);
                            ringing_active =
                                ((tick >= 65 + offset_ticks) &&
                                 (tick < 65 + offset_ticks + width_ticks)) ||
                                ((tick >= 195 + offset_ticks) &&
                                 (tick < 195 + offset_ticks + width_ticks));
                            q3_raw = q3_ideal ^ ringing_active;
                            @(negedge clk);
                        end
                        q3_ideal = 1'b0;
                        q3_raw = 1'b0;
                        repeat (20) @(negedge clk);
                        check_rate_limits = 1'b0;
                        label_text = $sformatf("offset sweep parity=%0d width=%0d offset=%0d",
                                              parity, width_ticks, offset_ticks);
                        check_count(label_text, first_count, 6);
                        if (ideal_rises - first_ideal != 6) begin
                            failures = failures + 1;
                            $display("SSI263 XCK FAIL: %s invalid ideal reference",
                                     label_text);
                        end
                        if (rate_spacing_errors != first_rate_errors ||
                            div2_spacing_errors != first_div2_errors) begin
                            failures = failures + 1;
                            $display("SSI263 XCK FAIL: %s short raw=%0d DIV2=%0d",
                                     label_text,
                                     rate_spacing_errors - first_rate_errors,
                                     div2_spacing_errors - first_div2_errors);
                        end
                        sweep_cases = sweep_cases + 1;
                    end
                end
            end
            $display("SSI263 XCK SWEEP: cases=%0d min_raw_clocks=%0d min_DIV2_clocks=%0d",
                     sweep_cases, minimum_raw_gap, minimum_div2_gap);
        end
    endtask

    task automatic drive_phase(
        input bit level,
        input realtime phase_time,
        input integer ringing
    );
        realtime excursion;
        begin
            q3_ideal = level;
            q3_raw = level;
            if (ringing == 0) begin
                #(phase_time);
            end else if (ringing == 4) begin
                // Two opposite-level excursions inside one physical phase.
                // Each true level lasts at least 65 ns before this burst and
                // 70 ns after it, including the shortest 200 ns low phase.
                #65ns;
                q3_raw = !level;
                #25ns;
                q3_raw = level;
                #10ns;
                q3_raw = !level;
                #30ns;
                q3_raw = level;
                #(phase_time - 130ns);
            end else begin
                case (ringing)
                    1: excursion = 25ns;
                    2: excursion = 30ns;
                    default: excursion = 45ns;
                endcase
                #65ns;
                q3_raw = !level;
                #(excursion);
                q3_raw = level;
                #(phase_time - 65ns - excursion);
            end
        end
    endtask

    task automatic run_q3_case(
        input string label_text,
        input realtime high_time,
        input realtime low_time,
        input integer periods,
        input integer ringing
    );
        integer first_raw;
        integer first_ideal;
        integer first_count;
        integer first_spacing;
        integer period_index;
        begin
            #503.2ns;
            first_raw = raw_rises;
            first_ideal = ideal_rises;
            first_count = enable_pulses;
            first_spacing = spacing_errors;
            expected_period = high_time + low_time;
            first_enable = 1'b1;
            check_spacing = 1'b1;
            for (period_index = 0; period_index < periods;
                 period_index = period_index + 1) begin
                drive_phase(1'b1, high_time, ringing);
                drive_phase(1'b0, low_time, ringing);
            end
            #150ns;
            check_spacing = 1'b0;
            check_count(label_text, first_count, periods);
            if (ideal_rises - first_ideal != periods) begin
                failures = failures + 1;
                $display("SSI263 XCK FAIL: %s invalid ideal reference",
                         label_text);
            end
            if (spacing_errors != first_spacing) begin
                failures = failures + 1;
                $display("SSI263 XCK FAIL: %s bad output intervals=%0d",
                         label_text, spacing_errors - first_spacing);
            end
            $display("SSI263 XCK CASE: %s ideal=%0d raw=%0d enables=%0d",
                     label_text, ideal_rises - first_ideal,
                     raw_rises - first_raw, enable_pulses - first_count);
        end
    endtask

    integer held_count;

    initial begin
        repeat (8) @(posedge clk);
        #1ns;
        if (xck_ce !== 1'b0) begin
            failures = failures + 1;
            $display("SSI263 XCK FAIL: reset did not clear enable");
        end
        @(negedge clk);
        rstn = 1'b1;
        held_count = enable_pulses;
        #2us;
        check_count("held low after reset", held_count, 0);

        q3_ideal = 1'b1;
        q3_raw = 1'b1;
        #2us;
        check_count("held high", held_count, 1);

        @(negedge clk);
        rstn = 1'b0;
        repeat (8) @(posedge clk);
        #1ns;
        if (xck_ce !== 1'b0) begin
            failures = failures + 1;
            $display("SSI263 XCK FAIL: reset with Q3 high left enable set");
        end
        held_count = enable_pulses;
        @(negedge clk);
        rstn = 1'b1;
        reset_high_restarts = reset_high_restarts + 1;
        #2us;
        check_count("held high after reset", held_count, 1);
        held_count = enable_pulses;
        q3_ideal = 1'b0;
        q3_raw = 1'b0;
        #2us;
        check_count("held low after falling edge", held_count, 0);

        // Nominal IIe NTSC and PAL periods, plus the shortest IIgs phases.
        // All phase times exceed the filter interval by a wide margin.
        run_q3_case("clean NTSC", 279.365ns, 209.524ns, 64, 0);
        run_q3_case("clean PAL", 281.675ns, 211.256ns, 64, 0);
        run_q3_case("clean shortest phases", 270ns, 200ns, 64, 0);
        run_q3_case("25ns ringing", 279.365ns, 209.524ns, 12, 1);
        run_q3_case("30ns ringing", 281.675ns, 211.256ns, 12, 2);
        run_q3_case("45ns ringing", 270ns, 200ns, 12, 3);
        run_q3_case("25/30ns ringing bursts", 270ns, 200ns, 12, 4);
        sweep_ringing_offsets();
        check_count("total including reset with Q3 high", 0,
                    ideal_rises + reset_high_restarts);

        if (failures == 0) begin
            $display("SSI263 XCK CE PASS ideal_rises=%0d reset_restarts=%0d raw_rises=%0d enables=%0d",
                     ideal_rises, reset_high_restarts, raw_rises, enable_pulses);
        end else begin
            $fatal(1, "SSI263 XCK CE FAIL count=%0d", failures);
        end
        $finish;
    end

endmodule
