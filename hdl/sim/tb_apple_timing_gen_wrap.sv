`timescale 1ns / 1ps
// Focused bench: apple_timing_gen frame wrap when the standard verdict
// moves under a running counter.
//
// The physical 50/60 Hz verdict can step 50 -> 60 Hz while the scanner sits
// on a line above 261. The wrap must then happen on the very next line, not
// after the 9-bit counter runs on to 511. The bench also checks the two
// steady-state frame lengths (312 / 262 lines) and the reverse step.

module tb_apple_timing_gen_wrap;

    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;

    logic       resetn = 1'b0;
    logic       apple_bus_pulse = 1'b0;
    logic       video_mode_50hz = 1'b1;
    logic       update_pulse;
    logic [8:0] line_in_frame;
    logic [6:0] cycle_in_line;

    apple_timing_gen dut (
        .clk(clk),
        .resetn(resetn),
        .apple_bus_pulse(apple_bus_pulse),
        .video_mode_50hz(video_mode_50hz),
        .update_pulse(update_pulse),
        .line_in_frame(line_in_frame),
        .cycle_in_line(cycle_in_line),
        .set_frame_zero_pulse(1'b0),
        .set_vblank_start_pulse(1'b0)
    );

    task automatic check(input logic condition, input string message);
        if (condition !== 1'b1)
            $fatal(1, "%s", message);
    endtask

    // One Apple cycle: a single-clock bus pulse, then idle.
    task automatic cycle();
        @(posedge clk);
        apple_bus_pulse <= 1'b1;
        @(posedge clk);
        apple_bus_pulse <= 1'b0;
        @(posedge clk);
    endtask

    task automatic line();
        repeat (65) cycle();
    endtask

    // Run whole lines until the presented line wraps to 0; return the
    // highest line number seen on the way. Each line() leaves the presented
    // line at its cycle 64 while the internal counter already holds the
    // next line, so a verdict change lands on the following line boundary.
    task automatic run_to_wrap(output int unsigned max_line,
                               output int unsigned lines_run);
        int unsigned prev;
        max_line = line_in_frame;
        lines_run = 0;
        do begin
            prev = line_in_frame;
            line();
            lines_run++;
            if (line_in_frame > max_line) max_line = line_in_frame;
        end while (!(line_in_frame == 0 && prev != 0));
    endtask

    int unsigned max_line, lines_run;

    initial begin
        repeat (4) @(posedge clk);
        resetn = 1'b1;
        repeat (4) @(posedge clk);

        // Steady PAL frame: 312 lines, top line 311. One line() first so
        // the presented line 0 is the counter's own, not the reset value.
        line();
        run_to_wrap(max_line, lines_run);
        check(max_line == 311, $sformatf("PAL top line %0d != 311", max_line));
        check(lines_run == 312, $sformatf("PAL frame %0d lines != 312", lines_run));

        // Step 50 -> 60 Hz while the counter sits on line 300.
        repeat (300) line();
        check(line_in_frame == 300, $sformatf("setup: line %0d != 300", line_in_frame));
        video_mode_50hz = 1'b0;
        run_to_wrap(max_line, lines_run);
        // Line 301 was already loaded; it must be the last one before 0.
        check(max_line == 301,
              $sformatf("60 Hz step: counter ran on to line %0d (must wrap at 301)",
                        max_line));
        check(lines_run == 2, $sformatf("60 Hz step: wrap took %0d lines != 2",
                                        lines_run));

        // Steady NTSC frame: 262 lines, top line 261.
        run_to_wrap(max_line, lines_run);
        check(max_line == 261, $sformatf("NTSC top line %0d != 261", max_line));
        check(lines_run == 262, $sformatf("NTSC frame %0d lines != 262", lines_run));

        // Reverse step 60 -> 50 Hz mid-frame just extends the frame.
        repeat (100) line();
        video_mode_50hz = 1'b1;
        run_to_wrap(max_line, lines_run);
        check(max_line == 311, $sformatf("50 Hz step: top line %0d != 311", max_line));

        $display("APPLE TIMING GEN WRAP PASS");
        $finish;
    end

    initial begin
        #5_000_000;
        $fatal(1, "APPLE TIMING GEN WRAP TIMEOUT");
    end

endmodule
