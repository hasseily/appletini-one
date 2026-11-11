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
    //    phi0_tick), so it phases the counter's NEXT tick. The tick advances
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

    /* Wrap on >= not ==: if the standard verdict steps 50 -> 60 Hz while
     * the counter sits in 262..311, an equality wrap would run the frame
     * on to 511 before it came back to 0. */
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
                if (current_line_in_frame >= line_max)
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

module apple_iigs_video_timing #(
    parameter int CLOCK_TIMEOUT_BITS = 10
) (
    input  logic clk,
    input  logic resetn,
    input  logic enabled,
    input  globals::AppleBus_read ab_read,
    output logic [7:0] c022,
    output logic [7:0] c029,
    output logic [7:0] c02b,
    output logic [3:0] border,
    output logic phase_locked,
    output logic update_pulse,
    output logic [8:0] line_in_frame,
    output logic [6:0] cycle_in_line
);
    wire access = ab_read.res && ab_read.data_en && ab_read.cycle_valid &&
                  !ab_read.m2sel;
    wire horizontal_read = access && ab_read.rw && ab_read.addr == 16'hC02F;
    wire vertical_read = access && ab_read.rw && ab_read.addr == 16'hC02E;
    wire standard_change = access && !ab_read.rw &&
        ab_read.addr == 16'hC02B && ab_read.data[4] != c02b[4];
    wire [8:0] line_max = c02b[4] ? 9'd311 : 9'd261;
    wire [8:0] vertical_wrap = c02b[4] ? 9'h0C8 : 9'h0FA;

    logic [CLOCK_TIMEOUT_BITS-1:0] quiet_clocks_q;
    logic [8:0] next_line_q;
    logic [6:0] next_cycle_q;
    logic phase_seed_q;
    logic first_h_valid_q, high_v_valid_q;
    logic [6:0] first_h_q, elapsed_ticks_q, vertical_read_ticks_q;
    logic first_low_v_q;
    logic [7:0] high_v_q;

    // TN 39. H counts 00,40..7F. The two H samples must agree with
    // the elapsed PHI0 ticks. If the bracket crosses a scanline, the tick
    // at which V was read determines which H sample supplied its low bit.
    wire [6:0] horizontal_raw = ab_read.data[6:0];
    wire horizontal_valid = horizontal_raw == 7'd0 || horizontal_raw[6];
    wire [6:0] horizontal_count = horizontal_raw == 7'd0 ? 7'd0 :
                                  {1'b0, horizontal_raw[5:0]} + 7'd1;
    wire [7:0] unwrapped_h = {1'b0, first_h_q} + {1'b0, elapsed_ticks_q};
    wire crossed_line = unwrapped_h >= 8'd65;
    wire [7:0] expected_h = crossed_line ? unwrapped_h - 8'd65 : unwrapped_h;
    wire vertical_on_next_line =
        ({1'b0, first_h_q} + {1'b0, vertical_read_ticks_q}) >= 8'd65;

    /* Line number for one value of the V low bit (bit 0 of the 9-bit
     * vertical count, which comes from one of the two H samples). */
    function automatic logic [8:0] measure_line(input logic [7:0] high_v,
                                                input logic low_v,
                                                input logic crossed,
                                                input logic v_next,
                                                input logic [8:0] wrap,
                                                input logic [8:0] last);
        logic [8:0] raw, sampled;
        raw = {high_v, low_v};
        sampled = raw[8] ? {1'b0, raw[7:0]} : 9'd256 + raw - wrap;
        return (crossed && !v_next) ?
            (sampled >= last ? 9'd0 : sampled + 9'd1) : sampled;
    endfunction

    /* Registered precompute of every term that uses only state. The state
     * changes on PHI0 ticks and qualified data strobes. A C02F data strobe
     * comes at least two clocks after either (about 30 on the bus), so
     * these registers have settled when horizontal_read uses them. Index
     * [v] selects the V low bit. This keeps the 9-bit line arithmetic off
     * the data-strobe path. */
    logic       pre_state_ok_q, pre_crossed_q, pre_v_next_q;
    logic       pre_h_zero_q, pre_h_range_q;
    logic [5:0] pre_h_minus1_q;
    logic       pre_c_zero_q, pre_c_range_q;
    logic [5:0] pre_c_minus1_q;
    logic [1:0][8:0] pre_line_q, pre_line_wrap_q;
    logic [1:0] pre_vraw_ok_q, pre_line_match_q;
    always_ff @(posedge clk) begin
        pre_state_ok_q <= first_h_valid_q && high_v_valid_q &&
            elapsed_ticks_q < 7'd65 && vertical_read_ticks_q != 7'd0 &&
            vertical_read_ticks_q < elapsed_ticks_q;
        pre_crossed_q  <= crossed_line;
        pre_v_next_q   <= vertical_on_next_line;
        pre_h_zero_q   <= expected_h == 8'd0;
        pre_h_range_q  <= expected_h >= 8'd1 && expected_h <= 8'd64;
        pre_h_minus1_q <= 6'(expected_h - 8'd1);
        pre_c_zero_q   <= cycle_in_line == 7'd0;
        pre_c_range_q  <= cycle_in_line >= 7'd1 && cycle_in_line <= 7'd64;
        pre_c_minus1_q <= 6'(cycle_in_line - 7'd1);
        for (int v = 0; v < 2; v++) begin
            pre_vraw_ok_q[v] <= {high_v_q, 1'(v)} >= vertical_wrap;
            pre_line_q[v] <= measure_line(high_v_q, 1'(v), crossed_line,
                vertical_on_next_line, vertical_wrap, line_max);
            pre_line_wrap_q[v] <= (measure_line(high_v_q, 1'(v),
                    crossed_line, vertical_on_next_line, vertical_wrap,
                    line_max) >= line_max) ? 9'd0 :
                measure_line(high_v_q, 1'(v), crossed_line,
                    vertical_on_next_line, vertical_wrap, line_max) + 9'd1;
            pre_line_match_q[v] <= line_in_frame == measure_line(high_v_q,
                1'(v), crossed_line, vertical_on_next_line, vertical_wrap,
                line_max);
        end
    end

    // Data-strobe terms. A valid H sample is 00 or 40..7F, so its count
    // matches a precomputed value c in 1..64 when raw[5:0] == c - 1.
    wire vertical_low = pre_v_next_q ? ab_read.data[7] : first_low_v_q;
    wire h_matches = pre_h_zero_q ? (horizontal_raw == 7'd0) :
        (pre_h_range_q && horizontal_raw[6] &&
         horizontal_raw[5:0] == pre_h_minus1_q);
    wire c_matches = pre_c_zero_q ? (horizontal_raw == 7'd0) :
        (pre_c_range_q && horizontal_raw[6] &&
         horizontal_raw[5:0] == pre_c_minus1_q);
    wire phase_sample = horizontal_read && horizontal_valid &&
        pre_state_ok_q && h_matches &&
        (first_low_v_q ^ ab_read.data[7]) == pre_crossed_q &&
        pre_vraw_ok_q[vertical_low];

    always_ff @(posedge clk) begin
        if (!resetn || !enabled) begin
            c022   <= 8'hF0;
            c029   <= 8'h01;
            c02b   <= 8'h00;
            border <= 4'h0;
        end else if (access && !ab_read.rw) begin
            case (ab_read.addr)
                16'hC022: c022   <= ab_read.data;
                16'hC029: c029   <= ab_read.data & 8'hE1;
                16'hC02B: c02b   <= ab_read.data;
                16'hC034: border <= ab_read.data[3:0];
                default: ;
            endcase
        end
    end

    always_ff @(posedge clk) begin
        if (!resetn || !enabled) begin
            quiet_clocks_q  <= '0;
            next_line_q     <= 9'd0;
            next_cycle_q    <= 7'd0;
            line_in_frame   <= 9'd0;
            cycle_in_line   <= 7'd0;
            update_pulse    <= 1'b0;
            phase_locked    <= 1'b0;
            phase_seed_q    <= 1'b0;
            first_h_valid_q <= 1'b0;
            high_v_valid_q  <= 1'b0;
            first_h_q       <= 7'd0;
            elapsed_ticks_q <= 7'd0;
            vertical_read_ticks_q <= 7'd0;
            first_low_v_q   <= 1'b0;
            high_v_q        <= 8'd0;
        end else begin
            update_pulse <= ab_read.phi0_tick;
            if (ab_read.phi0_tick) begin
                quiet_clocks_q <= '0;
                line_in_frame <= next_line_q;
                cycle_in_line <= next_cycle_q;
                next_cycle_q <= (next_cycle_q == 7'd64) ?
                    7'd0 : next_cycle_q + 7'd1;
                if (next_cycle_q == 7'd64)
                    next_line_q <= (next_line_q >= line_max) ?
                        9'd0 : next_line_q + 9'd1;
                if (elapsed_ticks_q < 7'd65)
                    elapsed_ticks_q <= elapsed_ticks_q + 7'd1;
                if (phase_seed_q) begin
                    phase_locked <= 1'b1;
                    phase_seed_q <= 1'b0;
                end
            end else if (!(&quiet_clocks_q)) begin
                quiet_clocks_q <= quiet_clocks_q + 1'b1;
            end

            if (standard_change || (&quiet_clocks_q)) begin
                phase_locked    <= 1'b0;
                phase_seed_q    <= 1'b0;
                first_h_valid_q <= 1'b0;
                high_v_valid_q  <= 1'b0;
            end else if (!ab_read.res) begin
                first_h_valid_q <= 1'b0;
                high_v_valid_q  <= 1'b0;
            end else if (horizontal_read) begin
                first_h_valid_q <= horizontal_valid;
                first_h_q       <= horizontal_count;
                first_low_v_q   <= ab_read.data[7];
                high_v_valid_q  <= 1'b0;
                elapsed_ticks_q <= 7'd0;
                if (phase_sample) begin
                    // Publish the lock with the first corrected timestamp.
                    // A mismatched prior phase is no longer authoritative.
                    phase_seed_q <= 1'b1;
                    if (!pre_line_match_q[vertical_low] || !c_matches)
                        phase_locked <= 1'b0;
                    next_cycle_q <= horizontal_count == 7'd64 ?
                        7'd0 : horizontal_count + 7'd1;
                    next_line_q <= horizontal_count != 7'd64 ?
                        pre_line_q[vertical_low] : pre_line_wrap_q[vertical_low];
                end
            end else if (vertical_read && first_h_valid_q) begin
                high_v_q       <= ab_read.data;
                high_v_valid_q <= 1'b1;
                vertical_read_ticks_q <= elapsed_ticks_q;
            end
        end
    end
endmodule
