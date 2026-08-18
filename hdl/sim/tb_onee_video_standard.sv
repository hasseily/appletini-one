`timescale 1ns / 1ps

module tb_onee_video_standard;

    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;

    logic resetn = 1'b0;
    logic enabled = 1'b0;
    logic virtual_res_n = 1'b1;
    logic write_valid = 1'b0;
    logic write_50hz = 1'b0;
    logic desired_50hz;
    logic active_50hz;
    logic set_frame_zero_pulse = 1'b0;
    logic update_pulse;
    logic [8:0] line_in_frame;
    logic [6:0] cycle_in_line;
    wire vblank = (line_in_frame >= 9'd192);

    onee_video_standard_ctrl standard_i (
        .clk(clk),
        .resetn(resetn),
        .enabled(enabled),
        .virtual_res_n(virtual_res_n),
        .write_valid(write_valid),
        .write_50hz(write_50hz),
        .desired_50hz(desired_50hz),
        .active_50hz(active_50hz)
    );

    // One scanner update per test clock keeps exact 262/312-line frame
    // checks quick; apple_virtual_bus has its own production-cadence bench.
    apple_timing_gen timing_i (
        .clk(clk),
        .resetn(resetn),
        .apple_bus_pulse(1'b1),
        .video_mode_50hz(active_50hz),
        .update_pulse(update_pulse),
        .line_in_frame(line_in_frame),
        .cycle_in_line(cycle_in_line),
        .set_frame_zero_pulse(set_frame_zero_pulse),
        .set_vblank_start_pulse(1'b0)
    );

    task automatic check(input logic condition, input string message);
        if (condition !== 1'b1)
            $fatal(1, "%s", message);
    endtask

    task automatic write_standard(input logic value);
        write_50hz <= value;
        write_valid <= 1'b1;
        @(posedge clk);
        #1;
        write_valid <= 1'b0;
    endtask

    task automatic check_frame(input int expected_lines);
        int lines_seen;
        logic [8:0] previous_line;
        logic saw_vblank_start;

        set_frame_zero_pulse <= 1'b1;
        @(posedge clk);
        #1;
        set_frame_zero_pulse <= 1'b0;
        check(line_in_frame == 9'd0 && cycle_in_line == 7'd0,
              "frame-zero pulse did not reset the scanner");

        lines_seen = 1;
        previous_line = line_in_frame;
        saw_vblank_start = 1'b0;
        forever begin
            @(posedge clk);
            #1;
            check(vblank == (line_in_frame >= 9'd192),
                  "VBL did not follow line 192");
            if (line_in_frame != previous_line) begin
                if (line_in_frame == 9'd192) begin
                    check(!saw_vblank_start && vblank,
                          "VBL did not begin once at line 192");
                    saw_vblank_start = 1'b1;
                end
                if (line_in_frame == 9'd0) begin
                    check(lines_seen == expected_lines,
                          $sformatf("scanner frame had %0d lines, expected %0d",
                                    lines_seen, expected_lines));
                    check(saw_vblank_start,
                          "scanner frame never reached VBL line 192");
                    check(!vblank, "VBL did not clear at frame wrap");
                    break;
                end
                lines_seen++;
                previous_line = line_in_frame;
            end
        end
    endtask

    initial begin
        repeat (5) @(posedge clk);
        resetn <= 1'b1;
        @(posedge clk);
        #1;
        check(!desired_50hz && !active_50hz,
              "ONE//e video standard did not default to NTSC");

        enabled <= 1'b1;
        write_standard(1'b1);
        check(desired_50hz && !active_50hz,
              "live PAL request was not held pending");
        check_frame(262);

        virtual_res_n <= 1'b0;
        @(posedge clk);
        #1;
        check(desired_50hz && active_50hz,
              "private reset did not apply pending PAL");
        virtual_res_n <= 1'b1;
        check_frame(312);

        write_standard(1'b0);
        check(!desired_50hz && active_50hz,
              "live NTSC request changed active PAL cadence");
        check_frame(312);

        enabled <= 1'b0;
        @(posedge clk);
        #1;
        check(!desired_50hz && !active_50hz,
              "ONE//e restart boundary did not apply pending NTSC");
        check_frame(262);

        $display("ONEE VIDEO STANDARD PASS");
        $finish;
    end

    initial begin
        #1000000;
        $fatal(1, "ONEE VIDEO STANDARD TIMEOUT");
    end

endmodule
