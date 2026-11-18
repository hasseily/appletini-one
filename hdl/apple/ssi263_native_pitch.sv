`timescale 1ns / 1ps

// SSI-263 pitch word and transitioned inflection, isolated from sound
// generation. The glide follows the SC-02 prototype (sheet 3: U27, U21A,
// U66, U65/U64):
//   - SEL1 rises once per 64 effective XCK.
//   - U27 divides SEL1 by 16-RATE and U21A halves it, so RATECLK has a
//     period of 128*(16-RATE) XCK.
//   - U66 divides RATECLK rises by 8-I[5:3].
//   - Each U66 output moves the 8-bit counter I[10:3] one count toward
//     {I[10:6],000}. I11 and I[2:0] always act at once.
// That is one count of the 12-bit pitch word per 16*(8-I[5:3])*(16-RATE)
// XCK, the rate measured on a real chip. U65/U64 have no reset; the first
// transitioned phone adopts its target once (a model policy).
module ssi263_native_pitch (
    input  logic        clk,
    input  logic        rstn,
    input  logic        warm_reset,
    input  logic        mode_latch,
    input  logic        write_strobe,
    input  logic [2:0]  write_reg,
    input  logic [7:0]  write_data,
    input  logic        xck_ce,     // one pulse per effective SSI XCK
    output logic [11:0] inflection_for_tick,
    output logic [11:0] active_inflection,
    output logic [1:0]  current_function
);
    logic [7:0] duration_q, duration_d, inflect_q, inflect_d;
    logic [7:0] rate_q, rate_d, control_q, control_d;
    logic [11:0] active_q, live_i;
    logic [1:0] function_q, function_d;
    logic seeded_q, seeded_d, start_phone;
    logic [5:0] sel1_q, sel1_d;
    logic [4:0] rate_left_q, rate_left_d;
    logic [3:0] move_left_q, move_left_d;
    logic rateclk_q, rateclk_d;
    logic [7:0] glide_q, glide_d, glide_target;

    always_comb begin
        duration_d = duration_q;
        inflect_d = inflect_q;
        rate_d = rate_q;
        control_d = control_q;
        function_d = function_q;
        seeded_d = seeded_q;
        sel1_d = sel1_q;
        rate_left_d = rate_left_q;
        move_left_d = move_left_q;
        rateclk_d = rateclk_q;
        glide_d = glide_q;
        start_phone = 1'b0;
        // SSI263P keeps running through Apple reset, which relatches its mode.
        if (mode_latch && duration_q[7:6] != 0)
            function_d = duration_q[7:6];
        if (write_strobe) begin
            case (write_reg)
                0: begin
                    duration_d = write_data;
                    start_phone = !control_q[7];
                end
                1: inflect_d = write_data;
                2: rate_d = write_data;
                3: begin
                    control_d = write_data;
                    if (control_q[7] && !write_data[7]) begin
                        if (duration_q[7:6] != 0)
                            function_d = duration_q[7:6];
                        start_phone = 1'b1;
                    end
                end
                default: begin end
            endcase
        end
        live_i = {rate_d[3], inflect_d, rate_d[2:0]};
        glide_target = {inflect_d[7:3], 3'b000};
        if (start_phone && function_d == 3) begin
            if (!seeded_q) glide_d = glide_target;
            seeded_d = 1'b1;
        end
        // A write precedes a coincident XCK, as in the host event loop.
        // The counters reload from the registers only at their terminal
        // counts, as the prototype's presettable counters do.
        if (xck_ce) begin
            if (sel1_q != 6'd63) sel1_d = sel1_q + 6'd1;
            else begin
                sel1_d = '0;
                if (rate_left_q != 5'd1) rate_left_d = rate_left_q - 5'd1;
                else begin
                    rate_left_d = 5'd16 - {1'b0, rate_d[7:4]};
                    rateclk_d = !rateclk_q;
                    if (!rateclk_q) begin
                        if (move_left_q != 4'd1) move_left_d = move_left_q - 4'd1;
                        else begin
                            move_left_d = 4'd8 - {1'b0, inflect_d[2:0]};
                            if (glide_d < glide_target) glide_d = glide_d + 8'd1;
                            else if (glide_d > glide_target) glide_d = glide_d - 8'd1;
                        end
                    end
                end
            end
        end
        // The source tick on this XCK sees the moved count.
        inflection_for_tick = function_d == 3 ?
                              {live_i[11], glide_d, live_i[2:0]} : live_i;
    end
    assign active_inflection = active_q;
    assign current_function = function_q;

    always_ff @(posedge clk) begin
        if (!rstn) begin
            duration_q <= 8'hc0;
            inflect_q <= 0;
            rate_q <= 0;
            control_q <= 8'h80;
            active_q <= 0;
            function_q <= 0;
            seeded_q <= 0;
            sel1_q <= 0;
            rate_left_q <= 5'd16;
            move_left_q <= 4'd8;
            rateclk_q <= 0;
            glide_q <= 0;
        end else if (warm_reset) begin
            // AP PD/RST keeps the glide count, its dividers and the
            // first-use seed, as the previous bus backend did.
            control_q <= 8'h80;
        end else begin
            duration_q <= duration_d;
            inflect_q <= inflect_d;
            rate_q <= rate_d;
            control_q <= control_d;
            active_q <= inflection_for_tick;
            function_q <= function_d;
            seeded_q <= seeded_d;
            sel1_q <= sel1_d;
            rate_left_q <= rate_left_d;
            move_left_q <= move_left_d;
            rateclk_q <= rateclk_d;
            glide_q <= glide_d;
        end
    end
endmodule
