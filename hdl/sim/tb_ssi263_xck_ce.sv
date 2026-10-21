`timescale 1ns / 1ps

module tb_ssi263_xck_ce;

    logic clk = 1'b0;
    logic rstn = 1'b0;
    logic q3_raw = 1'b0;
    logic q3_ideal = 1'b0;
    logic standalone = 1'b0;
    logic phi0 = 1'b0;
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
    integer handoff_cases = 0;
    integer pending_handoff_cases = 0;

    // The production fabric runs at 133.333 MHz. Physical Q3 is about 2 MHz.
    always #3.75ns clk = ~clk;

    ssi263_xck_ce dut (
        .clk(clk),
        .rstn(rstn),
        .q3_raw(q3_raw),
        .standalone(standalone),
        .phi0(phi0),
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
        // Sample registered outputs after the DUT's nonblocking updates.
        #1ns;
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

    task automatic clock_ticks(input integer count);
        begin
            repeat (count) begin
                @(posedge clk);
                #2ns;
            end
        end
    endtask

    task automatic prepare_source(input bit source, input bit phi_level);
        begin
            @(negedge clk);
            rstn = 1'b0;
            standalone = source;
            phi0 = phi_level;
            q3_raw = 1'b0;
            q3_ideal = 1'b0;
            clock_ticks(4);
            if (xck_ce !== 1'b0) begin
                failures = failures + 1;
                $display("SSI263 XCK FAIL: reset left enable set source=%0d", source);
            end
            rate_enable_seen = 1'b0;
            div2_enable_seen = 1'b0;
            div2_phase = 1'b0;
            div2_parity = 1'b0;
            @(negedge clk);
            rstn = 1'b1;
            // Standalone entry has a full cooldown; a held PHI0 level is
            // not an edge, including reset released while PHI0 is high.
            clock_ticks(80);
        end
    endtask

    task automatic wait_for_enable(input string label_text);
        integer first_count;
        integer timeout_clocks;
        begin
            first_count = enable_pulses;
            timeout_clocks = 0;
            while (enable_pulses == first_count && timeout_clocks < 100) begin
                clock_ticks(1);
                timeout_clocks = timeout_clocks + 1;
            end
            if (enable_pulses != first_count + 1) begin
                failures = failures + 1;
                $display("SSI263 XCK FAIL: %s missing enable", label_text);
            end
        end
    endtask

    task automatic run_standalone_cadence(input bit pal, input bit noisy_q3);
        integer first_count;
        integer edge_index;
        integer gap;
        integer cycle_index;
        integer last_edge_cycle;
        integer last_pair_cycle;
        integer current_edge_cycle;
        begin
            prepare_source(1'b1, 1'b0);
            first_count = enable_pulses;
            last_edge_cycle = -1;
            last_pair_cycle = -1;
            for (edge_index = 0; edge_index < 32; edge_index = edge_index + 1) begin
                gap = (pal && (edge_index % 2 != 0)) ? 66 : 65;
                for (cycle_index = 0; cycle_index < gap;
                     cycle_index = cycle_index + 1) begin
                    @(negedge clk);
                    // Accepted physical rises at a different rate must not
                    // leak into standalone. With this disabled there is no
                    // external Q3 activity at all.
                    q3_raw = noisy_q3 ? ((fabric_cycles % 38) < 19) : 1'b0;
                end
                phi0 = !phi0;
                clock_ticks(1);
                current_edge_cycle = fabric_cycles;
                if (xck_ce !== 1'b1 || enable_pulses != first_count + edge_index + 1) begin
                    failures = failures + 1;
                    $display("SSI263 XCK FAIL: standalone cadence PAL=%0d noisy_Q3=%0d edge=%0d",
                             pal, noisy_q3, edge_index);
                end
                if (last_edge_cycle >= 0 && current_edge_cycle - last_edge_cycle != gap) begin
                    failures = failures + 1;
                    $display("SSI263 XCK FAIL: standalone edge gap=%0d expected=%0d",
                             current_edge_cycle - last_edge_cycle, gap);
                end
                if (edge_index % 2 == 0) begin
                    if (last_pair_cycle >= 0 && current_edge_cycle - last_pair_cycle != (pal ? 131 : 130)) begin
                        failures = failures + 1;
                        $display("SSI263 XCK FAIL: standalone DIV2 cadence PAL=%0d gap=%0d",
                                 pal, current_edge_cycle - last_pair_cycle);
                    end
                    last_pair_cycle = current_edge_cycle;
                end
                last_edge_cycle = current_edge_cycle;
            end
            q3_raw = 1'b0;
            clock_ticks(80);
            check_count("standalone held PHI0 after cadence", first_count, 32);
            $display("SSI263 XCK STANDALONE: PAL=%0d noisy_Q3=%0d edges=32 DIV2_clocks=%0d",
                     pal, noisy_q3, pal ? 131 : 130);
        end
    endtask

    task automatic run_source_handoff(
        input bit old_source,
        input bit old_phi,
        input bit new_phi,
        input bit request_pending,
        input integer age
    );
        integer first_count;
        integer transition_cycle;
        integer timeout_clocks;
        integer cycle_index;
        integer prior_rate_errors;
        integer prior_div2_errors;
        bit had_pending;
        begin
            prepare_source(old_source, old_phi);
            prior_rate_errors = rate_spacing_errors;
            prior_div2_errors = div2_spacing_errors;
            @(negedge clk);
            if (old_source) phi0 = !phi0;
            else q3_raw = 1'b1;
            wait_for_enable("old handoff source");
            if (request_pending) begin
                if (old_source) begin
                    @(negedge clk);
                    phi0 = !phi0;
                    clock_ticks(1);
                end else begin
                    // A second qualified rise before the 62-clock limit
                    // queues physical work. Do not force internal state.
                    @(negedge clk);
                    q3_raw = 1'b0;
                    clock_ticks(16);
                    @(negedge clk);
                    q3_raw = 1'b1;
                    timeout_clocks = 0;
                    while (!dut.rise_pending_q && timeout_clocks < 30) begin
                        clock_ticks(1);
                        timeout_clocks = timeout_clocks + 1;
                    end
                end
                if (!dut.rise_pending_q) begin
                    failures = failures + 1;
                    $display("SSI263 XCK FAIL: unable to queue handoff source=%0d", old_source);
                end
            end
            clock_ticks(age);
            had_pending = dut.rise_pending_q;
            first_count = enable_pulses;
            @(negedge clk);
            standalone = !old_source;
            phi0 = new_phi;
            q3_raw = 1'b0;
            transition_cycle = fabric_cycles + 1;
            clock_ticks(1);
            if (xck_ce !== 1'b0 || dut.rise_pending_q !== 1'b0) begin
                failures = failures + 1;
                $display("SSI263 XCK FAIL: transition retained work source=%0d age=%0d", old_source, age);
            end
            // Neither the joined-bus PHI0 level change nor an old queued
            // rise can invent a tick. Exercise the unselected source too.
            for (cycle_index = 0; cycle_index < 90; cycle_index = cycle_index + 1) begin
                @(negedge clk);
                if (old_source) phi0 = !phi0;
                else q3_raw = ((cycle_index % 30) < 15);
                clock_ticks(1);
            end
            check_count("inactive new source after handoff", first_count, 0);
            // A real new-source edge resumes clocking after the holdoff.
            @(negedge clk);
            if (old_source) q3_raw = 1'b1;
            else phi0 = !phi0;
            wait_for_enable("new handoff source");
            if (fabric_cycles - transition_cycle < 62 ||
                rate_spacing_errors != prior_rate_errors ||
                div2_spacing_errors != prior_div2_errors) begin
                failures = failures + 1;
                $display("SSI263 XCK FAIL: unsafe handoff source=%0d age=%0d pending=%0d", old_source, age, had_pending);
            end
            handoff_cases = handoff_cases + 1;
            if (had_pending) pending_handoff_cases = pending_handoff_cases + 1;
        end
    endtask

    task automatic run_handoff_holdoff(input bit old_source, input bit new_phi);
        integer first_count;
        integer transition_cycle;
        integer held_cycle;
        begin
            prepare_source(old_source, !new_phi);
            @(negedge clk);
            if (old_source) phi0 = !phi0;
            else q3_raw = 1'b1;
            wait_for_enable("handoff holdoff old source");
            first_count = enable_pulses;
            @(negedge clk);
            standalone = !old_source;
            phi0 = new_phi;
            q3_raw = 1'b0;
            transition_cycle = fabric_cycles + 1;
            clock_ticks(1);
            @(negedge clk);
            if (old_source) q3_raw = 1'b1;
            else phi0 = !phi0;
            // The new edge arrives during cooldown and must wait. Every
            // intermediate clock is checked, including the last blocked one.
            for (held_cycle = 1; held_cycle <= 61; held_cycle = held_cycle + 1) begin
                clock_ticks(1);
                check_count("new source full transition holdoff", first_count, 0);
            end
            clock_ticks(1);
            check_count("new source resumes at transition +62", first_count, 1);
            if (fabric_cycles - transition_cycle != 62 || xck_ce !== 1'b1) begin
                failures = failures + 1;
                $display("SSI263 XCK FAIL: transition holdoff source=%0d gap=%0d", old_source, fabric_cycles - transition_cycle);
            end
        end
    endtask

    task automatic run_standalone_reset;
        integer first_count;
        begin
            prepare_source(1'b1, 1'b1);
            first_count = enable_pulses;
            clock_ticks(80);
            check_count("standalone held high reset release", first_count, 0);
            @(negedge clk);
            phi0 = 1'b0;
            wait_for_enable("standalone falling edge after reset");
            @(negedge clk);
            phi0 = 1'b1;
            clock_ticks(1);
            if (!dut.rise_pending_q) begin
                failures = failures + 1;
                $display("SSI263 XCK FAIL: reset case did not queue edge");
            end
            @(negedge clk);
            rstn = 1'b0;
            q3_raw = 1'b1;
            clock_ticks(4);
            first_count = enable_pulses;
            @(negedge clk);
            rstn = 1'b1;
            clock_ticks(100);
            check_count("reset flushes pending with PHI0 and Q3 held high", first_count, 0);
            @(negedge clk);
            phi0 = 1'b0;
            wait_for_enable("standalone restarts after pending reset");
            // An inactive physical source cannot use the free-running
            // standalone level changes, in either polarity.
            prepare_source(1'b0, 1'b0);
            first_count = enable_pulses;
            repeat (12) begin
                @(negedge clk);
                phi0 = !phi0;
                clock_ticks(65);
            end
            check_count("physical mode ignores PHI0 without Q3", first_count, 0);
        end
    endtask

    task automatic sweep_source_handoffs;
        integer source;
        integer old_level;
        integer new_level;
        integer pending_request;
        integer phase;
        begin
            check_rate_limits = 1'b1;
            for (source = 0; source < 2; source = source + 1) begin
                for (old_level = 0; old_level < 2; old_level = old_level + 1) begin
                    for (new_level = 0; new_level < 2; new_level = new_level + 1) begin
                        for (pending_request = 0; pending_request < 2;
                             pending_request = pending_request + 1) begin
                            // Every fabric phase of a complete PAL native
                            // cycle also covers every cooldown value and all
                            // NTSC phases, with pending and completed work.
                            for (phase = 0; phase < 131; phase = phase + 1) begin
                                run_source_handoff(source != 0, old_level != 0,
                                                   new_level != 0,
                                                   pending_request != 0, phase);
                            end
                        end
                    end
                end
                run_handoff_holdoff(source != 0, 1'b0);
                run_handoff_holdoff(source != 0, 1'b1);
            end
            check_rate_limits = 1'b0;
            if (pending_handoff_cases == 0 || rate_spacing_errors != 0 || div2_spacing_errors != 0) begin
                failures = failures + 1;
                $display("SSI263 XCK FAIL: handoff coverage pending=%0d raw_errors=%0d DIV2_errors=%0d",
                         pending_handoff_cases, rate_spacing_errors, div2_spacing_errors);
            end
            $display("SSI263 XCK HANDOFF SWEEP: cases=%0d pending=%0d min_raw_clocks=%0d min_DIV2_clocks=%0d",
                     handoff_cases, pending_handoff_cases, minimum_raw_gap, minimum_div2_gap);
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

        run_standalone_cadence(1'b0, 1'b0);
        run_standalone_cadence(1'b1, 1'b0);
        run_standalone_cadence(1'b0, 1'b1);
        run_standalone_cadence(1'b1, 1'b1);
        run_standalone_reset();
        sweep_source_handoffs();

        if (failures == 0) begin
            $display("SSI263 XCK CE PASS ideal_rises=%0d reset_restarts=%0d raw_rises=%0d enables=%0d",
                     ideal_rises, reset_high_restarts, raw_rises, enable_pulses);
        end else begin
            $fatal(1, "SSI263 XCK CE FAIL count=%0d", failures);
        end
        $finish;
    end

endmodule
