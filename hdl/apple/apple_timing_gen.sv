`timescale 1ns / 1ps

module apple_timing_gen (
    input  logic        clk,
    input  logic        resetn,
    input logic apple_bus_pulse,
    input logic video_mode_50hz,
    output logic update_pulse,
    output logic [8:0] line_in_frame,
    output logic [6:0] cycle_in_line,
    input logic set_frame_zero_pulse,
    input logic set_vblank_start_pulse
);

    localparam logic [8:0] VBL_START_LINE = 9'd192;
    // Raw scanner calibration lock, as 14 + 1 = 15:
    //
    //  * cycle 14 -- the ROM's first CMD_VBL_START write lands here on the
    //    VBL-start line: RDVBLBAR sampled at the last active cycle, then
    //    and + beq + lda# + ldy(abs $CA00) + sta(abs,Y) = 15 CPU cycles.
    //
    //  * +1 (-> 15): the seed fires during cycle 14 (after that cycle's
    //    sss_en), so it phases the counter's NEXT tick. sss_en advances
    //    cycle_in_line early in the Apple cycle (tap 26) and the capture
    //    samples it later at data_en (tap 59), so it reads the current
    //    cycle's value. At 15 the counter is aligned: a soft-switch write is
    //    tagged at its own true Apple cycle.
    //
    // Keep this counter in raw motherboard scanner phase. Consumers such as
    // synthesized $C019, MouseCard VBL, capture timestamps, and the software
    // renderer all use this common cycle convention. Folding a display
    // pipeline estimate into this seed makes the hardware-visible timing
    // consumers early and breaks the renderer's frame phasing.
    //
    // Confirmed with cycle-accurate mid-scanline mode-switch demos.
    localparam logic [6:0] VBL_LOCK_CYCLE = 7'd15;

    wire [8:0] line_max = video_mode_50hz ? 9'd311 : 9'd261;
    logic [8:0] current_line_in_frame;
    logic [6:0] current_cycle_in_line;

    always_ff @(posedge clk or negedge resetn) begin
        if (!resetn) begin
            current_cycle_in_line <= 7'd0;
            current_line_in_frame <= 9'd0;
            cycle_in_line         <= 7'd0;
            line_in_frame         <= 9'd0;
            update_pulse          <= 1'b0;
        end else if (set_frame_zero_pulse) begin
            current_cycle_in_line <= 7'd0;
            current_line_in_frame <= 9'd0;
            cycle_in_line         <= 7'd0;
            line_in_frame         <= 9'd0;
            update_pulse          <= 1'b1;
        end else if (set_vblank_start_pulse) begin
            current_cycle_in_line <= VBL_LOCK_CYCLE;
            current_line_in_frame <= VBL_START_LINE;
            cycle_in_line         <= VBL_LOCK_CYCLE;
            line_in_frame         <= VBL_START_LINE;
            update_pulse          <= 1'b1;
        end else if (apple_bus_pulse) begin
            // Present the current Apple cycle/line on this pulse, then
            // advance the internal counters for the next Apple cycle.
            cycle_in_line <= current_cycle_in_line;
            line_in_frame <= current_line_in_frame;
            if (current_cycle_in_line == 7'd64) begin
                current_cycle_in_line <= 7'd0;
                if (current_line_in_frame == line_max)
                    current_line_in_frame <= 9'd0;
                else
                    current_line_in_frame <= current_line_in_frame + 9'd1;
            end else begin
                current_cycle_in_line <= current_cycle_in_line + 7'd1;
            end
            update_pulse <= 1'b1;
        end else begin
            update_pulse <= 1'b0;
        end
    end

endmodule
