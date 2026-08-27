`timescale 1ns / 1ps
/* Physical host video standard (PAL 50 Hz / NTSC 60 Hz) from the PHI0 line
 * period.
 *
 * Every apple_bus_pulse is one PHI0 cycle (the wrapper's sss_en strobe).
 * 65 pulses span one scan line: 8533 fabric clocks on a PAL host
 * (912 dots / 14.25 MHz = 64.000 us) and 8493 on NTSC (912 / 14.31818 MHz
 * = 63.695 us) at 133.333 MHz, so the threshold sits 20 clocks (150 ns)
 * from each. One extra or missing PHI0 edge inside a window moves the
 * count by a whole cycle (~131 clocks) and flips that line's measurement.
 *
 * The host's standard is a physical constant. The first verdict is taken
 * from the first complete line (boot behavior unchanged). After that the
 * verdict changes only after FLIP_LINES consecutive lines that all measure
 * the other standard (~4 ms at 64). video_timing_gen latches the verdict
 * at every HDMI frame end: a one-line flip that landed there switched
 * H_TOTAL 2640 <-> 2200 and dropped monitor sync (seen on a PAL //e under
 * vTW, where bus-master drive ringing beat the PHI0 majority filter). */
module apple_video_standard_detect #(
    parameter logic [15:0] LINE_PERIOD_50HZ_THRESHOLD = 16'd8513,
    parameter logic [6:0]  FLIP_LINES = 7'd64
) (
    input  logic clk,
    input  logic resetn,            // synchronous, active low
    input  logic apple_bus_pulse,   // one strobe per PHI0 cycle
    output logic video_mode_50hz,   // verdict, meaningful when valid
    output logic video_mode_valid   // set by the first complete line
);

    logic [6:0]  cycle_in_line_q;
    logic [15:0] line_clk_count_q;
    logic [6:0]  flip_lines_q;
    logic        window_armed_q;

    wire line_measured_50hz =
        ((line_clk_count_q + 16'd1) >= LINE_PERIOD_50HZ_THRESHOLD);

    always_ff @(posedge clk) begin
        if (!resetn) begin
            cycle_in_line_q  <= 7'd0;
            line_clk_count_q <= 16'd0;
            flip_lines_q     <= 7'd0;
            window_armed_q   <= 1'b0;
            video_mode_50hz  <= 1'b0;
            video_mode_valid <= 1'b0;
        end else begin
            line_clk_count_q <= line_clk_count_q + 16'd1;

            if (apple_bus_pulse) begin
                if (cycle_in_line_q == 7'd64) begin
                    cycle_in_line_q  <= 7'd0;
                    line_clk_count_q <= 16'd0;
                    /* The window that starts at reset is partial: it holds
                     * 64 pulse intervals plus the reset-to-first-pulse
                     * offset, and reads short (60 Hz) on a PAL host. Only
                     * a window that began on a pulse-64 event is measured. */
                    if (!window_armed_q) begin
                        window_armed_q <= 1'b1;
                    end else if (!video_mode_valid) begin
                        video_mode_valid <= 1'b1;
                        video_mode_50hz <= line_measured_50hz;
                        flip_lines_q    <= 7'd0;
                    end else if (line_measured_50hz == video_mode_50hz) begin
                        flip_lines_q    <= 7'd0;
                    end else if (flip_lines_q == (FLIP_LINES - 7'd1)) begin
                        video_mode_50hz <= line_measured_50hz;
                        flip_lines_q    <= 7'd0;
                    end else begin
                        flip_lines_q    <= flip_lines_q + 7'd1;
                    end
                end else begin
                    cycle_in_line_q <= cycle_in_line_q + 7'd1;
                end
            end
        end
    end

endmodule
