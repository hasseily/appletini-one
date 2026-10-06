`timescale 1ns / 1ps

// Established SSI bus response counters, separated from the old sound core.
// Q3 divides by two here. Mode 1 requests once per 16 response slots;
// modes 2/3 request once per 16 duration slots. RATE is live at each reload.
// The audio controller has its own duration phase for envelopes; this small
// counter preserves the Apple-visible timing and its registered bus latency.
module ssi263_response_timing (
    input  logic clk,
    input  logic rstn,
    input  logic warm_reset,
    input  logic xck_ce,
    input  logic start,
    input  logic start_compat,
    input  logic [1:0] current_function,
    input  logic [7:0] duration_phoneme,
    input  logic [7:0] rate_inflection,
    output logic response_done,
    output logic phoneme_done
);
    logic div2_q, active_q, compat_q, response_raw_q;
    logic [13:0] duration_left_q;
    logic [11:0] response_left_q;
    logic [3:0] duration_phase_q, response_phase_q;

    function automatic logic [13:0] ssi_durclk_ticks_minus_one;
        logic [5:0] duration_high;
        begin
            // (4-D)*(16-R)*256-1 has eight constant low bits. The six
            // high bits depend on only D[1:0] and R[3:0], so use one
            // truth table instead of a multiply/subtract carry path.
            // Stay combinational: an accepted RATE write must still feed
            // a duration-slot reload on that same fabric edge.
            case ({duration_phoneme[7:6], rate_inflection[7:4]})
                // D=0: 4 duration units.
                6'h00: duration_high = 6'd63;
                6'h01: duration_high = 6'd59;
                6'h02: duration_high = 6'd55;
                6'h03: duration_high = 6'd51;
                6'h04: duration_high = 6'd47;
                6'h05: duration_high = 6'd43;
                6'h06: duration_high = 6'd39;
                6'h07: duration_high = 6'd35;
                6'h08: duration_high = 6'd31;
                6'h09: duration_high = 6'd27;
                6'h0A: duration_high = 6'd23;
                6'h0B: duration_high = 6'd19;
                6'h0C: duration_high = 6'd15;
                6'h0D: duration_high = 6'd11;
                6'h0E: duration_high = 6'd7;
                6'h0F: duration_high = 6'd3;
                // D=1: 3 duration units.
                6'h10: duration_high = 6'd47;
                6'h11: duration_high = 6'd44;
                6'h12: duration_high = 6'd41;
                6'h13: duration_high = 6'd38;
                6'h14: duration_high = 6'd35;
                6'h15: duration_high = 6'd32;
                6'h16: duration_high = 6'd29;
                6'h17: duration_high = 6'd26;
                6'h18: duration_high = 6'd23;
                6'h19: duration_high = 6'd20;
                6'h1A: duration_high = 6'd17;
                6'h1B: duration_high = 6'd14;
                6'h1C: duration_high = 6'd11;
                6'h1D: duration_high = 6'd8;
                6'h1E: duration_high = 6'd5;
                6'h1F: duration_high = 6'd2;
                // D=2: 2 duration units.
                6'h20: duration_high = 6'd31;
                6'h21: duration_high = 6'd29;
                6'h22: duration_high = 6'd27;
                6'h23: duration_high = 6'd25;
                6'h24: duration_high = 6'd23;
                6'h25: duration_high = 6'd21;
                6'h26: duration_high = 6'd19;
                6'h27: duration_high = 6'd17;
                6'h28: duration_high = 6'd15;
                6'h29: duration_high = 6'd13;
                6'h2A: duration_high = 6'd11;
                6'h2B: duration_high = 6'd9;
                6'h2C: duration_high = 6'd7;
                6'h2D: duration_high = 6'd5;
                6'h2E: duration_high = 6'd3;
                6'h2F: duration_high = 6'd1;
                // D=3: 1 duration unit.
                6'h30: duration_high = 6'd15;
                6'h31: duration_high = 6'd14;
                6'h32: duration_high = 6'd13;
                6'h33: duration_high = 6'd12;
                6'h34: duration_high = 6'd11;
                6'h35: duration_high = 6'd10;
                6'h36: duration_high = 6'd9;
                6'h37: duration_high = 6'd8;
                6'h38: duration_high = 6'd7;
                6'h39: duration_high = 6'd6;
                6'h3A: duration_high = 6'd5;
                6'h3B: duration_high = 6'd4;
                6'h3C: duration_high = 6'd3;
                6'h3D: duration_high = 6'd2;
                6'h3E: duration_high = 6'd1;
                6'h3F: duration_high = 6'd0;
                default: duration_high = 'x;
            endcase
            ssi_durclk_ticks_minus_one = {duration_high, 8'hFF};
        end
    endfunction

    always_ff @(posedge clk) begin
        if (!rstn || warm_reset) begin
            div2_q <= 0;
            active_q <= 0;
            compat_q <= 0;
            duration_left_q <= 0;
            response_left_q <= 0;
            duration_phase_q <= 0;
            response_phase_q <= 0;
            response_raw_q <= 0;
            response_done <= 0;
            phoneme_done <= 0;
        end else begin
            if (xck_ce) div2_q <= !div2_q;
            response_raw_q <= 0;
            response_done <= response_raw_q;
            phoneme_done <= 0;
            if (start) begin
                active_q <= 1;
                compat_q <= start_compat;
                duration_left_q <= ssi_durclk_ticks_minus_one();
                response_left_q <= {~rate_inflection[7:4], 8'hff};
                duration_phase_q <= 0;
                response_phase_q <= 0;
                response_done <= 0;
            end else if (xck_ce && div2_q && active_q) begin
                if (response_left_q == 0) begin
                    response_left_q <= {~rate_inflection[7:4], 8'hff};
                    response_phase_q <= response_phase_q + 4'd1;
                    if (response_phase_q == 15 && current_function == 1 && !compat_q)
                        response_raw_q <= 1;
                end else response_left_q <= response_left_q - 12'd1;
                if (duration_left_q == 0) begin
                    duration_left_q <= ssi_durclk_ticks_minus_one();
                    duration_phase_q <= duration_phase_q + 4'd1;
                    if (duration_phase_q == 15) begin
                        phoneme_done <= 1;
                        if (current_function == 2 || current_function == 3 || compat_q)
                            response_raw_q <= 1;
                        // Optional Votrax address compatibility plays one native
                        // SSI phone; it does not reproduce SC-01 ROM duration.
                        if (compat_q) active_q <= 0;
                    end
                end else duration_left_q <= duration_left_q - 14'd1;
            end
        end
    end
endmodule
