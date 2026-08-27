`timescale 1ns / 1ps
// Focused bench: apple_video_standard_detect flip hysteresis.
//
// Pulses arrive one per Apple cycle. A PAL line is 65 pulses over 8533
// clocks (131.3 clocks per pulse), NTSC 8493 (130.7). The bench uses 132
// clocks per pulse for PAL (8580 per line) and 130 for NTSC (8450), both
// well clear of the 8513 threshold, and checks:
//   1. the first complete line sets valid and the verdict at once;
//   2. one line with an extra pulse (a phantom PHI0 edge) does not flip;
//   3. FLIP_LINES-1 consecutive lines of the other standard do not flip,
//      and a single agreeing line resets the run;
//   4. FLIP_LINES consecutive lines do flip, in both directions.

module tb_apple_video_standard_detect;

    timeunit 1ns;
    timeprecision 1ps;

    localparam int PAL_PULSE_CLKS  = 132;
    localparam int NTSC_PULSE_CLKS = 130;
    localparam int FLIP_LINES      = 64;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;

    logic resetn = 1'b0;
    logic apple_bus_pulse = 1'b0;
    logic video_mode_50hz;
    logic video_mode_valid;

    apple_video_standard_detect #(
        .FLIP_LINES(7'(FLIP_LINES))
    ) dut (
        .clk(clk),
        .resetn(resetn),
        .apple_bus_pulse(apple_bus_pulse),
        .video_mode_50hz(video_mode_50hz),
        .video_mode_valid(video_mode_valid)
    );

    task automatic check(input logic condition, input string message);
        if (condition !== 1'b1)
            $fatal(1, "%s", message);
    endtask

    // One pulse, then idle to fill the requested cycle length.
    task automatic pulse(input int clks);
        @(posedge clk);
        apple_bus_pulse <= 1'b1;
        @(posedge clk);
        apple_bus_pulse <= 1'b0;
        repeat (clks - 2) @(posedge clk);
    endtask

    task automatic lines(input int n, input int clks_per_pulse);
        repeat (n) repeat (65) pulse(clks_per_pulse);
    endtask

    // Track every change of the verdict while valid.
    int unsigned flips;
    logic mode_prev;
    always @(posedge clk) begin
        if (video_mode_valid && video_mode_50hz != mode_prev) flips++;
        mode_prev <= video_mode_50hz;
    end

    initial begin
        flips = 0;
        repeat (4) @(posedge clk);
        resetn = 1'b1;
        repeat (4) @(posedge clk);
        check(video_mode_valid === 1'b0, "valid must start clear");

        // 1. The first complete line decides at once (PAL). The window
        //    that starts at reset is partial (64 intervals) and is only
        //    used to arm; the verdict lands on the second pulse-64 event.
        lines(1, PAL_PULSE_CLKS);
        pulse(PAL_PULSE_CLKS);
        @(posedge clk);
        check(video_mode_valid === 1'b0,
              "the partial reset window must not produce a verdict");
        lines(1, PAL_PULSE_CLKS);
        @(posedge clk);
        check(video_mode_valid === 1'b1, "first complete line must set valid");
        check(video_mode_50hz === 1'b1, "first complete PAL line must read 50 Hz");
        flips = 0;

        // 2. One phantom pulse in a line: 66 pulses in 65 real cycles'
        //    time -> that window measures short (NTSC). Must not flip.
        lines(4, PAL_PULSE_CLKS);
        repeat (64) pulse(PAL_PULSE_CLKS - 2);
        pulse(2);                       // the phantom edge
        pulse(PAL_PULSE_CLKS - 2);
        lines(4, PAL_PULSE_CLKS);
        check(video_mode_50hz === 1'b1 && flips == 0,
              "a single phantom-edge line flipped the verdict");

        // 3. FLIP_LINES-1 NTSC lines: no flip. One PAL line resets the
        //    run, so another FLIP_LINES-1 NTSC lines still do not flip.
        lines(FLIP_LINES - 1, NTSC_PULSE_CLKS);
        lines(1, PAL_PULSE_CLKS);
        check(video_mode_50hz === 1'b1 && flips == 0,
              $sformatf("%0d NTSC lines flipped the verdict early", FLIP_LINES - 1));
        lines(FLIP_LINES - 1, NTSC_PULSE_CLKS);
        lines(1, PAL_PULSE_CLKS);
        check(video_mode_50hz === 1'b1 && flips == 0,
              "an agreeing line did not reset the flip run");

        // 4a. FLIP_LINES NTSC lines: flip to 60 Hz, exactly once.
        lines(FLIP_LINES, NTSC_PULSE_CLKS);
        pulse(NTSC_PULSE_CLKS);
        @(posedge clk);
        check(video_mode_50hz === 1'b0,
              $sformatf("%0d NTSC lines must flip to 60 Hz", FLIP_LINES));
        check(flips == 1, $sformatf("flip count %0d != 1", flips));

        // 4b. And back: FLIP_LINES PAL lines flip to 50 Hz.
        lines(FLIP_LINES - 1, PAL_PULSE_CLKS);
        check(video_mode_50hz === 1'b0, "flip back happened early");
        lines(1, PAL_PULSE_CLKS);
        pulse(PAL_PULSE_CLKS);
        @(posedge clk);
        check(video_mode_50hz === 1'b1, "did not flip back to 50 Hz");
        check(flips == 2, $sformatf("flip count %0d != 2", flips));

        $display("APPLE VIDEO STANDARD DETECT PASS");
        $finish;
    end

    initial begin
        #20_000_000;
        $fatal(1, "APPLE VIDEO STANDARD DETECT TIMEOUT");
    end

endmodule
