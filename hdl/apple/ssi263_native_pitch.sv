`timescale 1ns / 1ps

// Retained listening-model pitch policy, isolated from sound generation.
// The 20 kHz glide cadence and step table are provisional SSI behavior;
// they do not reproduce the prototype's RATE-coupled inflection circuit.
module ssi263_native_pitch (
    input  logic        clk,
    input  logic        rstn,
    input  logic        warm_reset,
    input  logic        mode_latch,
    input  logic        write_strobe,
    input  logic [2:0]  write_reg,
    input  logic [7:0]  write_data,
    input  logic        audio_tick, // one pulse per 48 kHz output sample
    output logic [11:0] inflection_for_tick,
    output logic [11:0] active_inflection,
    output logic [1:0]  current_function
);
    logic [7:0] duration_q, duration_d, inflect_q, inflect_d;
    logic [7:0] rate_q, rate_d, control_q, control_d;
    logic [11:0] active_q, active_d, live_i;
    logic [1:0] function_q, function_d;
    logic seeded_q, seeded_d, start_phone;
    logic [3:0] cadence_q, cadence_d;
    logic [4:0] active_field, target_field, step_size, next_field;

    always_comb begin
        duration_d = duration_q;
        inflect_d = inflect_q;
        rate_d = rate_q;
        control_d = control_q;
        function_d = function_q;
        seeded_d = seeded_q;
        active_d = active_q;
        cadence_d = cadence_q;
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
        if (start_phone) begin
            active_d = live_i;
            if (function_d == 3) begin
                if (seeded_q) active_d[10:6] = active_q[10:6];
                seeded_d = 1'b1;
            end
        end
        // A coincident source tick sees writes before the sample advances
        // the glide, as in the frozen host event loop.
        inflection_for_tick = function_d == 3 ? active_d : live_i;
        active_field = active_d[10:6];
        target_field = live_i[10:6];
        case (live_i[5:3])
            0: step_size = 1;
            1: step_size = 2;
            2: step_size = 3;
            3: step_size = 4;
            4: step_size = 6;
            5: step_size = 8;
            6: step_size = 12;
            default: step_size = 16;
        endcase
        next_field = target_field;
        if (target_field > active_field && target_field - active_field > step_size)
            next_field = active_field + step_size;
        else if (active_field > target_field && active_field - target_field > step_size)
            next_field = active_field - step_size;
        if (audio_tick) begin
            // 5/12 of the 48 kHz samples: exact 20 kHz average, no division.
            if (cadence_q >= 7) begin
                cadence_d = cadence_q - 7;
                active_d = live_i;
                if (function_d == 3) active_d[10:6] = next_field;
            end else cadence_d = cadence_q + 5;
        end
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
            cadence_q <= 0;
        end else if (warm_reset) begin
            // The retained SSI transitioned-pitch policy keeps its value and
            // first-use seed through AP PD/RST, as the previous bus backend did.
            control_q <= 8'h80;
            cadence_q <= 0;
        end else begin
            duration_q <= duration_d;
            inflect_q <= inflect_d;
            rate_q <= rate_d;
            control_q <= control_d;
            active_q <= active_d;
            function_q <= function_d;
            seeded_q <= seeded_d;
            cadence_q <= cadence_d;
        end
    end
endmodule
