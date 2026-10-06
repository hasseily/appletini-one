`timescale 1ns / 1ps

module tb_ssi263_clock_core;
    localparam integer LANES = 32;
    logic clk = 1'b0;
    logic rstn = 1'b0;
    logic xck_ce = 1'b0;
    logic div2 = 1'b0;
    logic pitch_run = 1'b0;
    logic pitch_reload = 1'b0;
    logic filter_run = 1'b0;
    logic filter_reload = 1'b0;
    logic [11:0] inflection [0:LANES-1];
    logic [7:0] filter_frequency [0:LANES-1];
    wire [LANES-1:0] effective_ce;
    wire [LANES-1:0] pitch_ce;
    wire [LANES-1:0] filter_ce;
    logic [15:0] pitch_periods [0:4095];
    logic [9:0] filter_periods [0:255];
    integer cases = 0;

    always #5 clk = ~clk;

    for (genvar lane = 0; lane < LANES; lane = lane + 1) begin : g_clock
        ssi263_clock_core dut (
            .clk(clk), .rstn(rstn), .xck_ce(xck_ce), .div2(div2),
            .pitch_run(pitch_run), .pitch_reload(pitch_reload),
            .immediate_inflection(inflection[lane]),
            .filter_run(filter_run), .filter_reload(filter_reload),
            .filter_frequency(filter_frequency[lane]),
            .effective_xck_ce(effective_ce[lane]),
            .pitch_ce(pitch_ce[lane]), .filter_ce(filter_ce[lane])
        );
    end

    task automatic cycle(input bit raw_enable);
        @(negedge clk);
        xck_ce = raw_enable;
        @(posedge clk);
        #1;
    endtask

    task automatic check(input bit valid, input string label);
        if (!valid)
            $fatal(1, "SSI263 CLOCK FAIL: %s at %0t", label, $time);
    endtask

    task automatic restart(input bit divide_by_two);
        rstn = 1'b0;
        pitch_run = 1'b0;
        filter_run = 1'b0;
        pitch_reload = 1'b0;
        filter_reload = 1'b0;
        div2 = divide_by_two;
        cycle(0);
        check({pitch_ce, filter_ce, effective_ce} === '0, "reset outputs");
        rstn = 1'b1;
        cycle(0);
    endtask

    task automatic all_targets(input integer pitch, input integer filter);
        for (integer lane = 0; lane < LANES; lane = lane + 1) begin
            inflection[lane] = pitch;
            filter_frequency[lane] = filter;
        end
    endtask

    // Every inflection word: two complete periods, no hierarchical force or
    // inspection of the DUT's arithmetic/counters. Parallel lanes reduce time.
    task automatic exhaustive_pitch;
        integer period;
        for (integer batch = 0; batch < 4096; batch = batch + LANES) begin
            for (integer lane = 0; lane < LANES; lane = lane + 1) begin
                inflection[lane] = batch + lane;
                filter_frequency[lane] = 255;
            end
            restart(0);
            pitch_run = 1'b1;
            for (integer tick = 1; tick <= 2 * pitch_periods[batch]; tick = tick + 1) begin
                cycle(1);
                check(effective_ce === {LANES{1'b1}}, "DIV2 bypass");
                check(filter_ce === '0, "disabled filter");
                for (integer lane = 0; lane < LANES; lane = lane + 1) begin
                    period = pitch_periods[batch + lane];
                    if (pitch_ce[lane] !== ((tick % period) == 0))
                        $fatal(1, "pitch I=%0d tick=%0d period=%0d got=%0b",
                               batch + lane, tick, period, pitch_ce[lane]);
                end
            end
            cases = cases + LANES;
        end
        $display("SSI263 CLOCK: all 4096 immediate inflections passed");
    endtask

    task automatic exhaustive_filter;
        integer effective_tick;
        integer period;
        for (integer mode = 0; mode < 2; mode = mode + 1) begin
            for (integer batch = 0; batch < 256; batch = batch + LANES) begin
                for (integer lane = 0; lane < LANES; lane = lane + 1) begin
                    inflection[lane] = 0;
                    filter_frequency[lane] = batch + lane;
                end
                restart(mode != 0);
                filter_run = 1'b1;
                for (integer tick = 1; tick <= 2 * (mode + 1) * filter_periods[batch];
                     tick = tick + 1) begin
                    cycle(1);
                    effective_tick = (mode == 0 || (tick % 2) == 0);
                    check(effective_ce === {LANES{effective_tick != 0}}, "DIV2 cadence");
                    check(pitch_ce === '0, "disabled pitch");
                    for (integer lane = 0; lane < LANES; lane = lane + 1) begin
                        period = filter_periods[batch + lane] * (mode + 1);
                        if (filter_ce[lane] !== ((tick % period) == 0))
                            $fatal(1, "filter FF=%0d DIV2=%0d tick=%0d got=%0b",
                                   batch + lane, mode, tick, filter_ce[lane]);
                    end
                end
                cases = cases + LANES;
            end
        end
        $display("SSI263 CLOCK: all 256 FF values, DIV2 off/on, passed");
    endtask

    task automatic div2_pitch_boundaries;
        integer period;
        for (integer lane = 0; lane < LANES; lane = lane + 1) begin
            case (lane % 4)
                0: inflection[lane] = 0;
                1: inflection[lane] = 1;
                2: inflection[lane] = 4094;
                3: inflection[lane] = 4095;
            endcase
            filter_frequency[lane] = 0;
        end
        restart(1);
        pitch_run = 1'b1;
        for (integer tick = 1; tick <= 131072; tick = tick + 1) begin
            cycle(1);
            check(effective_ce === {LANES{(tick % 2) == 0}}, "pitch DIV2 cadence");
            check(filter_ce === '0, "DIV2 disabled filter");
            for (integer lane = 0; lane < LANES; lane = lane + 1) begin
                period = pitch_periods[inflection[lane]] * 2;
                if (pitch_ce[lane] !== ((tick % period) == 0))
                    $fatal(1, "DIV2 pitch I=%0d tick=%0d period=%0d got=%0b",
                           inflection[lane], tick, period, pitch_ce[lane]);
            end
        end
        cases = cases + 4;
        $display("SSI263 CLOCK: DIV2 pitch endpoint and neighbor values passed");
    endtask

    // Physical regional raw-XCK cadence at a 100 MHz simulation fabric. The
    // independent reference counts source events, not elapsed DAC samples.
    task automatic regional_cadence(input integer raw_hz);
        longint accumulator;
        integer raw_count;
        integer effective_count;
        integer pitch_events;
        integer filter_events;
        bit raw_tick;
        bit effective_tick;
        all_targets(4000, 231);
        restart(1);
        pitch_run = 1'b1;
        filter_run = 1'b1;
        accumulator = 0;
        raw_count = 0;
        effective_count = 0;
        pitch_events = 0;
        filter_events = 0;
        for (integer tick = 0; tick < 1000000; tick = tick + 1) begin
            accumulator = accumulator + raw_hz;
            raw_tick = accumulator >= 100000000;
            if (raw_tick) begin
                accumulator = accumulator - 100000000;
                raw_count = raw_count + 1;
            end
            effective_tick = raw_tick && ((raw_count % 2) == 0);
            if (effective_tick)
                effective_count = effective_count + 1;
            cycle(raw_tick);
            check(effective_ce === {LANES{effective_tick}}, "regional effective XCK");
            check(pitch_ce === {LANES{effective_tick &&
                  ((effective_count % pitch_periods[4000]) == 0)}}, "regional pitch");
            check(filter_ce === {LANES{effective_tick &&
                  ((effective_count % filter_periods[231]) == 0)}}, "regional filter");
            if (pitch_ce[0]) pitch_events = pitch_events + 1;
            if (filter_ce[0]) filter_events = filter_events + 1;
        end
        check(raw_count == raw_hz / 100, "regional raw count");
        check(pitch_events == effective_count / pitch_periods[4000], "regional pitch count");
        check(filter_events == effective_count / filter_periods[231], "regional filter count");
        $display("SSI263 CLOCK: raw_hz=%0d raw=%0d effective=%0d pitch=%0d filter=%0d",
                 raw_hz, raw_count, effective_count, pitch_events, filter_events);
        cases = cases + 1;
    endtask

    task automatic write_and_enable_policy;
        // FF changes cannot restart or retune pitch. An input change alone
        // does not restart the pending filter cycle under this caller policy.
        all_targets(4095, 254); // Pitch period 8, filter period 4.
        restart(0);
        pitch_run = 1'b1;
        filter_run = 1'b1;
        for (integer tick = 1; tick <= 24; tick = tick + 1) begin
            if (tick == 3) all_targets(4095, 255);
            cycle(1);
            check(pitch_ce === {LANES{(tick % 8) == 0}}, "FF write leaves pitch alone");
            check(filter_ce === {LANES{tick >= 4 && (tick % 2) == 0}}, "live FF boundary");
        end

        // Explicit pitch reload excludes coincident XCK and leaves filter's
        // two-tick cycle alone, including at I=4095 (shortest pitch period).
        for (integer tick = 1; tick <= 17; tick = tick + 1) begin
            pitch_reload = tick == 1;
            cycle(1);
            check(pitch_ce === {LANES{tick == 9 || tick == 17}}, "pitch reload precedence");
            check(filter_ce === {LANES{(tick % 2) == 0}}, "pitch reload leaves FF alone");
        end
        pitch_reload = 1'b0;

        // With no XCK events neither period may advance. Reload is synchronous
        // to fabric clk and does not require an XCK event.
        filter_reload = 1'b1;
        pitch_reload = 1'b1;
        all_targets(4094, 253); // Pitch 16, filter 6.
        cycle(0);
        filter_reload = 1'b0;
        pitch_reload = 1'b0;
        repeat (19) begin
            cycle(0);
            check({pitch_ce, filter_ce, effective_ce} === '0, "idle fabric cycle");
        end
        for (integer tick = 1; tick <= 32; tick = tick + 1) begin
            cycle(1);
            check(pitch_ce === {LANES{(tick % 16) == 0}}, "XCK-gap pitch");
            check(filter_ce === {LANES{(tick % 6) == 0}}, "XCK-gap filter");
            cycle(0);
            check({pitch_ce, filter_ce, effective_ce} === '0, "single-cycle enable");
        end

        // A new pitch word also waits only for the current full cycle, not an
        // extra one. The caller may request a different phase with reload.
        pitch_reload = 1'b1;
        cycle(0);
        pitch_reload = 1'b0;
        for (integer tick = 1; tick <= 32; tick = tick + 1) begin
            if (tick == 5) all_targets(4095, 253);
            cycle(1);
            check(pitch_ce === {LANES{tick == 16 || tick == 24 || tick == 32}},
                  "live inflection boundary");
        end

        // Disabled dividers preload their targets even as XCK keeps running.
        pitch_run = 1'b0;
        filter_run = 1'b0;
        repeat (5) begin
            cycle(1);
            check({pitch_ce, filter_ce} === '0, "run off suppresses both outputs");
        end
        pitch_run = 1'b1;
        filter_run = 1'b1;
        for (integer tick = 1; tick <= 8; tick = tick + 1) begin
            cycle(1);
            check(pitch_ce === {LANES{tick == 8}}, "run on preloaded pitch");
            check(filter_ce === {LANES{tick == 6}}, "run on preloaded filter");
        end
        cases = cases + 7;
        $display("SSI263 CLOCK: independent writes, reload, gaps and run controls passed");
    endtask

    initial begin
        $readmemh("pitch_periods.mem", pitch_periods);
        $readmemh("filter_periods.mem", filter_periods);
        all_targets(0, 0);
        exhaustive_pitch();
        exhaustive_filter();
        div2_pitch_boundaries();
        regional_cadence(2031250); // PAL effective XCK = 1,015,625 Hz.
        regional_cadence(2040968); // NTSC effective XCK = 1,020,484 Hz.
        write_and_enable_policy();
        $display("SSI263 CLOCK CORE PASS cases=%0d", cases);
        $finish;
    end
endmodule
