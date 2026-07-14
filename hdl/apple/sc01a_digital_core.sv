`timescale 1ns / 1ps

import ssi263_formant_pkg::*;

// SC-01A digital control core for the SSI263 formant backend.
//
// The structure here is derived from Olivier Galibert's vsim SC-01A digital
// simulation, which is based on the chip schematics/die work. This module keeps
// Apple-visible SSI263/SC-01 behavior out of the digital core; the bus wrapper
// remains the D7/IRQ/A/R timing authority.
module sc01a_digital_core (
    input  logic       clk,
    input  logic       rstn,
    input  logic       reset,
    input  logic       audio_tick,

    input  logic       start,
    input  logic [5:0] start_phone,
    input  logic       start_votrax,
    input  logic [1:0] current_function,
    input  logic [7:0] duration_phoneme,
    input  logic [7:0] inflection,
    input  logic [7:0] rate_inflection,
    input  logic [2:0] articulation,

    output logic       phoneme_done,

    output logic [4:0] ticks,
    output logic [9:0] pitch,
    output logic       pitch_noise_gate,
    output logic [4:0] closure_age,
    output logic       noise_bit,

    output logic [3:0] rom_fa,
    output logic [3:0] rom_fc,
    output logic [3:0] rom_va,
    output logic [3:0] rom_f1,
    output logic [3:0] rom_f2,
    output logic [3:0] rom_f2q,
    output logic [3:0] rom_f3,
    output logic [3:0] rom_cld,
    output logic [3:0] rom_vd,
    output logic [6:0] rom_duration,
    output logic       rom_closure,
    output logic       rom_silence,
    output logic [5:0] rom_phone,

    output logic [7:0] cur_fa,
    output logic [7:0] cur_fc,
    output logic [7:0] cur_va,
    output logic [7:0] cur_f1,
    output logic [7:0] cur_f2,
    output logic [7:0] cur_f2q,
    output logic [7:0] cur_f3,

    output logic [3:0] filt_fa,
    output logic [3:0] filt_fc,
    output logic [3:0] filt_va,
    output logic [3:0] filt_f1,
    output logic [4:0] filt_f2,
    output logic [3:0] filt_f2q,
    output logic [3:0] filt_f3
);

    localparam logic [16:0] SC01_CONTROL_UPDATE_RATE = 17'd20000;
    // SSI263 pitch is controlled by the 12-bit INFLECT/RATEINF word via a true
    // f0 = XCK/(8*(4096-I)) law (see ssi263_pitch_period_limit). Votrax mode
    // uses the SC-01 F1/XOR pitch path. The glottal pulse
    // occupies the first 72 phase counts of each period and the closed phase
    // fills the remainder; above ~278 Hz the period drops under 72 and the pulse
    // tail truncates -- the natural high-pitch behaviour.

    // Latched at phoneme start: 1 = Votrax/SC-01 direct, 0 = SSI263.
    logic is_votrax_q;

    logic [15:0] chip_accum_q;
    logic [8:0]  phonetick_q;
    logic [5:0]  update_counter_q;
    logic [1:0]  duration_mod4_q;
    logic        closure_active_q;
    logic        filter_commit_pending_q;
    logic [4:0]  rate_accum_q;
    logic        control_update_pending_q;
    logic [5:0]  control_speed_step_q;
    logic [9:0]  pitch_limit_q;
    logic [11:0] target_inflection_q;
    logic [11:0] active_inflection_q;

    // Galibert's MAME model uses the SC-01A 15-bit NXOR noise register.
    logic [14:0] noise_q;

    function automatic logic [2:0] articulation_shift;
        begin
            case (articulation)
                3'd0,
                3'd1:    articulation_shift = 3'd5;
                3'd2,
                3'd3:    articulation_shift = 3'd4;
                3'd4,
                3'd5:    articulation_shift = 3'd3;
                3'd6:    articulation_shift = 3'd2;
                default: articulation_shift = 3'd1;
            endcase
        end
    endfunction

    function automatic logic [7:0] interpolate8(input logic [7:0] reg_value,
                                                input logic [3:0] target);
        logic [2:0] shift;
        logic [8:0] current_value;
        logic [8:0] target_value;
        logic [8:0] delta;
        logic [8:0] step;
        logic [8:0] next_value;
        begin
            shift = articulation_shift();
            current_value = {1'b0, reg_value};
            target_value = {1'b0, target, 4'd0};

            if (target_value >= current_value) begin
                delta = target_value - current_value;
                step = delta >> shift;
                if (delta != 9'd0 && step == 9'd0) begin
                    step = 9'd1;
                end
                next_value = current_value + step;
                if (next_value[8]) begin
                    next_value = 9'h0FF;
                end
            end else begin
                delta = current_value - target_value;
                step = delta >> shift;
                if (delta != 9'd0 && step == 9'd0) begin
                    step = 9'd1;
                end
                next_value = (step > current_value) ? 9'd0 : (current_value - step);
            end

            interpolate8 = next_value[7:0];
        end
    endfunction

    function automatic logic duration_one_skip_mode;
        duration_one_skip_mode = (current_function != 2'd1) &&
                                 (duration_phoneme[7:6] == 2'd1);
    endfunction

    function automatic logic [2:0] duration_speed_step;
        logic [1:0] dur;
        begin
            dur = (current_function == 2'd1) ? 2'd3 : duration_phoneme[7:6];
            case (dur)
                2'd2:    duration_speed_step = 3'd2;
                2'd3:    duration_speed_step = 3'd4;
                default: duration_speed_step = 3'd1;
            endcase

            if (duration_one_skip_mode() && duration_mod4_q == 2'd2) begin
                duration_speed_step = 3'd2;
            end
        end
    endfunction

    function automatic logic [4:0] rate_period_units;
        logic [3:0] rate;
        begin
            rate = rate_inflection[7:4];
            rate_period_units = (rate == 4'd0) ? 5'd16 :
                (5'd16 - {1'b0, rate});
        end
    endfunction

    task automatic rate_divmod_units(input logic [5:0] numerator,
                                     input logic [4:0] period,
                                     output logic [5:0] quotient,
                                     output logic [4:0] remainder);
        begin
            case (period)
                5'd1: begin
                    quotient = numerator;
                    remainder = 5'd0;
                end
                5'd2: begin
                    quotient = {1'b0, numerator[5:1]};
                    remainder = {4'd0, numerator[0]};
                end
                5'd3: begin
                    quotient = numerator / 6'd3;
                    remainder = numerator % 6'd3;
                end
                5'd4: begin
                    quotient = {2'd0, numerator[5:2]};
                    remainder = {3'd0, numerator[1:0]};
                end
                5'd5: begin
                    quotient = numerator / 6'd5;
                    remainder = numerator % 6'd5;
                end
                5'd6: begin
                    quotient = numerator / 6'd6;
                    remainder = numerator % 6'd6;
                end
                5'd7: begin
                    quotient = numerator / 6'd7;
                    remainder = numerator % 6'd7;
                end
                5'd8: begin
                    quotient = {3'd0, numerator[5:3]};
                    remainder = {2'd0, numerator[2:0]};
                end
                5'd9: begin
                    quotient = numerator / 6'd9;
                    remainder = numerator % 6'd9;
                end
                5'd10: begin
                    quotient = numerator / 6'd10;
                    remainder = numerator % 6'd10;
                end
                5'd11: begin
                    quotient = numerator / 6'd11;
                    remainder = numerator % 6'd11;
                end
                5'd12: begin
                    quotient = numerator / 6'd12;
                    remainder = numerator % 6'd12;
                end
                5'd13: begin
                    quotient = numerator / 6'd13;
                    remainder = numerator % 6'd13;
                end
                5'd14: begin
                    quotient = numerator / 6'd14;
                    remainder = numerator % 6'd14;
                end
                5'd15: begin
                    quotient = numerator / 6'd15;
                    remainder = numerator % 6'd15;
                end
                default: begin
                    quotient = {3'd0, numerator[5:4]};
                    remainder = {1'd0, numerator[3:0]};
                end
            endcase
        end
    endtask

    task automatic rate_scaled_speed_step(input logic [2:0] base_step,
                                          output logic [5:0] scaled_step);
        logic [5:0] numerator;
        logic [4:0] period;
        logic [5:0] quotient;
        logic [5:0] remainder;
        begin
            period = rate_period_units();
            numerator = {1'b0, rate_accum_q} + ({3'd0, base_step} * 6'd6);
            rate_divmod_units(numerator, period, quotient, remainder);
            scaled_step = quotient;
            rate_accum_q <= remainder[4:0];
        end
    endtask

    function automatic logic [11:0] ssi263_inflection12;
        begin
            ssi263_inflection12 = {rate_inflection[3],
                                   inflection,
                                   rate_inflection[2:0]};
        end
    endfunction

    function automatic logic transitioned_inflection_mode(input logic votrax);
        begin
            transitioned_inflection_mode = !votrax && (current_function == 2'd3);
        end
    endfunction

    function automatic logic [4:0] inflection_slope_step(input logic [2:0] slope);
        begin
            case (slope)
                3'd0:    inflection_slope_step = 5'd1;
                3'd1:    inflection_slope_step = 5'd2;
                3'd2:    inflection_slope_step = 5'd3;
                3'd3:    inflection_slope_step = 5'd4;
                3'd4:    inflection_slope_step = 5'd6;
                3'd5:    inflection_slope_step = 5'd8;
                3'd6:    inflection_slope_step = 5'd12;
                default: inflection_slope_step = 5'd16;
            endcase
        end
    endfunction

    task automatic advance_inflection;
        logic [11:0] live_inflection;
        logic [11:0] next_inflection;
        logic [4:0]  active_target;
        logic [4:0]  target_target;
        logic [4:0]  step;
        logic [4:0]  delta;
        begin
            live_inflection = ssi263_inflection12();
            target_inflection_q <= live_inflection;
            next_inflection = live_inflection;
            if (transitioned_inflection_mode(is_votrax_q)) begin
                active_target = active_inflection_q[10:6];
                target_target = live_inflection[10:6];
                step = inflection_slope_step(live_inflection[5:3]);

                if (active_target < target_target) begin
                    delta = target_target - active_target;
                    next_inflection[10:6] =
                        active_target + ((delta < step) ? delta : step);
                end else if (active_target > target_target) begin
                    delta = active_target - target_target;
                    next_inflection[10:6] =
                        active_target - ((delta < step) ? delta : step);
                end
            end

            active_inflection_q <= next_inflection;
        end
    endtask

    // SSI263 pitch period, faithful to the data sheet:
    //   f0 = XCK / (8 * (4096 - I))  ->  period (in update ticks) proportional
    //   to (4096 - I), with NO fixed offset. The glottal counter advances at
    //   SC01_CONTROL_UPDATE_RATE = 20 kHz, so for the assumed XCK = 1 MHz:
    //     period = (4096 - I) * (20000 * 8 / 1e6) = (4096 - I) * 0.16
    //   5/32 = 0.15625 is the synthesizable approximation: 90.9 Hz at the
    //   nominal I = 0xA80 and 31.3 Hz at I = 0 (data sheet: 90 / 30.5 Hz).
    function automatic logic [9:0] ssi263_pitch_period_limit(input logic [11:0] infl);
        logic [12:0] span;     // 4096 - I, range 1..4096
        logic [15:0] scaled;   // span * 5, range 5..20480
        logic [9:0]  period;
        begin
            span   = 13'd4096 - {1'b0, infl};
            scaled = {3'd0, span} * 16'd5;
            period = scaled[14:5];               // >> 5  (max 20480>>5 = 640)
            ssi263_pitch_period_limit = (period == 10'd0) ? 10'd1 : period;
        end
    endfunction

    function automatic logic [9:0] pitch_period_limit;
        logic [11:0] infl;
        logic [7:0] base_limit;
        logic [8:0] period_ext;
        begin
            // SSI263 inflection is a control around the SC-01 voice source.
            // Keep it in this digital/audio core, but leave register timing in
            // the bus wrapper.
            infl = is_votrax_q ? ssi263_inflection12() : active_inflection_q;
            if (!is_votrax_q) begin
                pitch_period_limit = ssi263_pitch_period_limit(infl);
            end else begin
                // SC-01/Votrax native pitch (Galibert/MAME model).
                base_limit = 8'hE0 ^ {infl[11:10], 5'd0};
                period_ext = {1'b0, (base_limit ^ {filt_f1, 1'b0})} + 9'd2;
                pitch_period_limit = period_ext[8] ? 10'd255 :
                                                     {2'd0, period_ext[7:0]};
            end
        end
    endfunction

    function automatic logic [9:0] pitch_period_limit_for(input logic votrax,
                                                          input logic [11:0] infl);
        logic [7:0] base_limit;
        logic [8:0] period_ext;
        begin
            if (!votrax) begin
                pitch_period_limit_for = ssi263_pitch_period_limit(infl);
            end else begin
                base_limit = 8'hE0 ^ {infl[11:10], 5'd0};
                period_ext = {1'b0, (base_limit ^ {filt_f1, 1'b0})} + 9'd2;
                pitch_period_limit_for = period_ext[8] ? 10'd255 :
                                                       {2'd0, period_ext[7:0]};
            end
        end
    endfunction

    function automatic logic [14:0] sc01_noise_next(input logic [14:0] state,
                                                    input logic cur_noise);
        logic        inp;
        begin
            inp = cur_noise && (state != 15'h7FFF);
            sc01_noise_next = {state[13:0], inp};
        end
    endfunction

    function automatic logic sc01_noise_out(input logic [14:0] state);
        begin
            sc01_noise_out = ~(^state[14:13]);
        end
    endfunction

    task automatic commit_filters;
        begin
            filt_fa <= cur_fa[7:4];
            filt_fc <= cur_fc[7:4];
            filt_va <= cur_va[7:4];
            filt_f1 <= cur_f1[7:4];
            filt_f2 <= cur_f2[7:3];
            filt_f2q <= cur_f2q[7:4];
            filt_f3 <= cur_f3[7:4];
        end
    endtask

    task automatic load_phone(input logic [5:0] phone);
        logic [3:0] next_fa;
        logic [3:0] next_fc;
        logic [3:0] next_va;
        logic [3:0] next_f1;
        logic [3:0] next_f2;
        logic [3:0] next_f2q;
        logic [3:0] next_f3;
        logic [3:0] next_cld;
        logic       next_closure;
        logic [11:0] next_inflection;
        begin
            next_f1 = sc01a_f1(phone);
            next_va = sc01a_va(phone);
            next_f2 = sc01a_f2(phone);
            next_fc = sc01a_fc(phone);
            next_f2q = sc01a_f2q(phone);
            next_f3 = sc01a_f3(phone);
            next_fa = sc01a_fa(phone);
            next_cld = sc01a_cld(phone);
            next_closure = sc01a_closure(phone);

            phonetick_q <= 9'd0;
            ticks <= 5'd0;
            filter_commit_pending_q <= 1'b0;
            rate_accum_q <= 5'd0;
            control_update_pending_q <= 1'b0;
            control_speed_step_q <= 6'd0;

            rom_f1 <= next_f1;
            rom_va <= next_va;
            rom_f2 <= next_f2;
            rom_fc <= next_fc;
            rom_f2q <= next_f2q;
            rom_f3 <= next_f3;
            rom_fa <= next_fa;
            rom_cld <= next_cld;
            rom_vd <= sc01a_vd(phone);
            rom_closure <= next_closure;
            rom_duration <= sc01a_duration(phone);
            rom_silence <= sc01a_pause(phone);
            rom_phone <= phone;
            is_votrax_q <= start_votrax;
            next_inflection = ssi263_inflection12();
            target_inflection_q <= next_inflection;
            if (start_votrax || !transitioned_inflection_mode(start_votrax)) begin
                active_inflection_q <= next_inflection;
                pitch_limit_q <= pitch_period_limit_for(start_votrax, next_inflection);
            end else begin
                active_inflection_q <= {next_inflection[11],
                                        active_inflection_q[10:6],
                                        next_inflection[5:0]};
                pitch_limit_q <= pitch_period_limit_for(1'b0,
                                                        {next_inflection[11],
                                                         active_inflection_q[10:6],
                                                         next_inflection[5:0]});
            end

            if (next_cld == 4'd0) begin
                closure_active_q <= next_closure;
            end
        end
    endtask

    task automatic advance_control(input logic [5:0] speed_step);
        logic [5:0] update_next;
        logic       tick_625;
        logic       tick_208;
        logic [10:0] phonetick_next;
        logic [10:0] tick_limit_ext;
        logic [10:0] phonetick_wrapped;
        logic [4:0] ticks_next;
        logic [8:0] tick_limit;
        begin
            if (ticks != 5'h10) begin
                tick_limit = {rom_duration, 2'b00} | 9'd1;
                tick_limit_ext = {2'b0, tick_limit};
                phonetick_next = {2'b0, phonetick_q} + {5'd0, speed_step};
                phonetick_wrapped = phonetick_next - tick_limit_ext;
                if (phonetick_next >= tick_limit_ext) begin
                    phonetick_q <= phonetick_wrapped[8:0];
                    ticks_next = ticks + 5'd1;
                    ticks <= ticks_next;
                    if (ticks_next == 5'h10) begin
                        phoneme_done <= 1'b1;
                    end
                    if (ticks_next == {1'b0, rom_cld}) begin
                        closure_active_q <= rom_closure;
                    end
                end else begin
                    phonetick_q <= phonetick_next[8:0];
                end
            end

            update_next = (update_counter_q == 6'd47) ? 6'd0 : (update_counter_q + 6'd1);
            update_counter_q <= update_next;
            tick_625 = (update_next[3:0] == 4'd0);
            tick_208 = (update_next == 6'h28);

            // Same die behavior Galibert exposes through the right-timing and
            // parameter SRAM path: formants and VA/FA update on different
            // cadences, with FC tied to the formant cadence.
            if (tick_208 && (!rom_silence || !(filt_fa != 4'd0 || filt_va != 4'd0))) begin
                cur_fc <= interpolate8(cur_fc, rom_fc);
                cur_f1 <= interpolate8(cur_f1, rom_f1);
                cur_f2 <= interpolate8(cur_f2, rom_f2);
                cur_f2q <= interpolate8(cur_f2q, rom_f2q);
                cur_f3 <= interpolate8(cur_f3, rom_f3);
            end

            if (tick_625) begin
                if (ticks[3:0] >= rom_vd) begin
                    cur_fa <= interpolate8(cur_fa, rom_fa);
                end
                if (ticks[3:0] >= rom_cld) begin
                    cur_va <= interpolate8(cur_va, rom_va);
                end
            end

            if (!closure_active_q && (filt_fa != 4'd0 || filt_va != 4'd0)) begin
                closure_age <= 5'd0;
            end else if (closure_age != 5'd28) begin
                closure_age <= closure_age + 5'd1;
            end

            if (duration_one_skip_mode()) begin
                duration_mod4_q <= duration_mod4_q + 2'd1;
            end else begin
                duration_mod4_q <= 2'd0;
            end
        end
    endtask

    task automatic advance_pitch_noise;
        logic [10:0] pitch_next;
        logic [10:0] pitch_limit_ext;
        logic [10:0] pitch_wrapped;
        logic [9:0]  pitch_commit;
        logic [9:0]  pitch_limit;
        logic [14:0] next_noise;
        begin
            pitch_limit = pitch_limit_q;
            pitch_limit_ext = {1'b0, pitch_limit};
            pitch_next = {1'b0, pitch} + 11'd1;
            pitch_wrapped = pitch_next - pitch_limit_ext;
            pitch_commit = (pitch_next >= pitch_limit_ext) ?
                           pitch_wrapped[9:0] :
                           pitch_next[9:0];
            pitch <= pitch_commit;

            // Noise gate: Votrax keeps the SC-01/MAME pitch[6] AM (matches the
            // reference model); SSI263 uses a period-stable ~50%-duty gate (the
            // closed half of the glottal period) so a widened period does not
            // chop the noise into multiple bursts per cycle.
            pitch_noise_gate <= is_votrax_q ?
                                pitch_commit[6] :
                                (pitch_commit >= {1'b0, pitch_limit[9:1]});

            if ((pitch_commit & 10'h3F9) == 10'h008) begin
                filter_commit_pending_q <= 1'b1;
            end

            next_noise = sc01_noise_next(noise_q, noise_bit);
            noise_q <= next_noise;
            noise_bit <= sc01_noise_out(next_noise);
        end
    endtask

    task automatic reset_state;
        begin
            chip_accum_q <= 16'd0;
            phonetick_q <= 9'd0;
            update_counter_q <= 6'd0;
            duration_mod4_q <= 2'd0;
            closure_active_q <= 1'b1;
            filter_commit_pending_q <= 1'b0;
            is_votrax_q <= 1'b0;
            rate_accum_q <= 5'd0;
            control_update_pending_q <= 1'b0;
            control_speed_step_q <= 6'd0;
            pitch_limit_q <= 10'd255;
            target_inflection_q <= 12'd0;
            active_inflection_q <= 12'd0;
            noise_q <= 15'd0;

            phoneme_done <= 1'b0;
            ticks <= 5'd0;
            pitch <= 10'd0;
            pitch_noise_gate <= 1'b0;
            closure_age <= 5'd0;
            noise_bit <= 1'b0;

            rom_fa <= 4'd0;
            rom_fc <= 4'd0;
            rom_va <= 4'd0;
            rom_f1 <= 4'd7;
            rom_f2 <= 4'd9;
            rom_f2q <= 4'd4;
            rom_f3 <= 4'hC;
            rom_cld <= 4'd1;
            rom_vd <= 4'd1;
            rom_duration <= 7'h0F;
            rom_closure <= 1'b1;
            rom_silence <= 1'b0;
            rom_phone <= 6'h3F;

            cur_fa <= 8'd0;
            cur_fc <= 8'd0;
            cur_va <= 8'd0;
            cur_f1 <= 8'd0;
            cur_f2 <= 8'd0;
            cur_f2q <= 8'd0;
            cur_f3 <= 8'd0;

            filt_fa <= 4'd0;
            filt_fc <= 4'd0;
            filt_va <= 4'd0;
            filt_f1 <= 4'd0;
            filt_f2 <= 5'd0;
            filt_f2q <= 4'd0;
            filt_f3 <= 4'd0;
        end
    endtask

    always_ff @(posedge clk) begin
        logic [16:0] chip_accum_next;
        logic [2:0] base_speed_step;
        logic [5:0] speed_step;

        if (!rstn || reset) begin
            reset_state();
        end else begin
            phoneme_done <= 1'b0;

            if (filter_commit_pending_q) begin
                commit_filters();
                filter_commit_pending_q <= 1'b0;
            end

            if (start) begin
                load_phone(start_phone);
                duration_mod4_q <= 2'd0;
            end else if (control_update_pending_q) begin
                control_update_pending_q <= 1'b0;
                if (control_speed_step_q != 6'd0) begin
                    advance_control(control_speed_step_q);
                end
                advance_pitch_noise();
            end else if (audio_tick) begin
                chip_accum_next = {1'b0, chip_accum_q} + SC01_CONTROL_UPDATE_RATE;
                if (chip_accum_next >= 17'd48000) begin
                    chip_accum_q <= chip_accum_next - 17'd48000;
                    advance_inflection();
                    pitch_limit_q <= pitch_period_limit();
                    base_speed_step = duration_speed_step();
                    if (is_votrax_q) begin
                        speed_step = 6'd1;
                    end else begin
                        rate_scaled_speed_step(base_speed_step, speed_step);
                    end
                    control_speed_step_q <= speed_step;
                    control_update_pending_q <= 1'b1;
                end else begin
                    chip_accum_q <= chip_accum_next[15:0];
                end
            end
        end
    end

endmodule
