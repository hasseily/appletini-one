`timescale 1ns / 1ps

import ssi263_formant_pkg::*;

module tb_ssi263_hybrid_core;

    logic clk = 1'b0;
    logic rstn = 1'b0;
    logic reset = 1'b0;
    logic audio_tick = 1'b0;
    logic xck_ce = 1'b0;

    logic       start = 1'b0;
    logic [5:0] start_phone = 6'd0;
    logic       start_votrax = 1'b0;
    logic [1:0] current_function = 2'd2;
    logic [7:0] duration_phoneme = 8'h80;
    logic [7:0] inflection = 8'd0;
    logic [7:0] rate_inflection = 8'd0;
    logic [2:0] articulation = 3'd5;

    logic       phoneme_done;
    logic       response_done;
    logic [4:0] ticks;
    logic [9:0] pitch;
    logic       pitch_noise_gate;
    logic [4:0] closure_age;
    logic       noise_bit;
    logic [3:0] rom_fa;
    logic [3:0] rom_fc;
    logic [3:0] rom_va;
    logic [3:0] rom_f1;
    logic [3:0] rom_f2;
    logic [3:0] rom_f2q;
    logic [3:0] rom_f3;
    logic [3:0] rom_cld;
    logic [3:0] rom_vd;
    logic [6:0] rom_duration;
    logic       rom_closure;
    logic       rom_silence;
    logic [5:0] rom_phone;
    logic [7:0] cur_fa;
    logic [7:0] cur_fc;
    logic [7:0] cur_va;
    logic [7:0] cur_f1;
    logic [7:0] cur_f2;
    logic [7:0] cur_f2q;
    logic [7:0] cur_f3;
    logic [3:0] filt_fa;
    logic [3:0] filt_fc;
    logic [3:0] filt_va;
    logic [3:0] filt_f1;
    logic [4:0] filt_f2;
    logic [3:0] filt_f2q;
    logic [3:0] filt_f3;

    always #5 clk = ~clk;

    sc01a_digital_core dut (
        .clk(clk),
        .rstn(rstn),
        .reset(reset),
        .audio_tick(audio_tick),
        .xck_ce(xck_ce),
        .start(start),
        .start_phone(start_phone),
        .start_votrax(start_votrax),
        .current_function(current_function),
        .duration_phoneme(duration_phoneme),
        .inflection(inflection),
        .rate_inflection(rate_inflection),
        .articulation(articulation),
        .phoneme_done(phoneme_done),
        .response_done(response_done),
        .ticks(ticks),
        .pitch(pitch),
        .pitch_noise_gate(pitch_noise_gate),
        .closure_age(closure_age),
        .noise_bit(noise_bit),
        .rom_fa(rom_fa),
        .rom_fc(rom_fc),
        .rom_va(rom_va),
        .rom_f1(rom_f1),
        .rom_f2(rom_f2),
        .rom_f2q(rom_f2q),
        .rom_f3(rom_f3),
        .rom_cld(rom_cld),
        .rom_vd(rom_vd),
        .rom_duration(rom_duration),
        .rom_closure(rom_closure),
        .rom_silence(rom_silence),
        .rom_phone(rom_phone),
        .cur_fa(cur_fa),
        .cur_fc(cur_fc),
        .cur_va(cur_va),
        .cur_f1(cur_f1),
        .cur_f2(cur_f2),
        .cur_f2q(cur_f2q),
        .cur_f3(cur_f3),
        .filt_fa(filt_fa),
        .filt_fc(filt_fc),
        .filt_va(filt_va),
        .filt_f1(filt_f1),
        .filt_f2(filt_f2),
        .filt_f2q(filt_f2q),
        .filt_f3(filt_f3)
    );

    task automatic require(input logic condition, input string message);
        begin
            if (condition !== 1'b1) begin
                $display("SSI263 HYBRID CORE FAIL: %s", message);
                $fatal(1);
            end
        end
    endtask

    task automatic cold_reset;
        begin
            start = 1'b0;
            xck_ce = 1'b0;
            audio_tick = 1'b0;
            reset = 1'b0;
            rstn = 1'b0;
            repeat (4) @(posedge clk);
            @(negedge clk);
            rstn = 1'b1;
            repeat (2) @(posedge clk);
            @(negedge clk);
        end
    endtask

    task automatic set_inflection_word(input logic [11:0] value,
                                        input logic [3:0] rate);
        begin
            inflection = value[10:3];
            rate_inflection = {rate, value[11], value[2:0]};
        end
    endtask

    function automatic logic [9:0] expected_ssi_pitch_limit(
        input logic [11:0] value
    );
        logic [12:0] span;
        logic [15:0] scaled;
        logic [9:0] period;
        begin
            span = 13'd4096 - {1'b0, value};
            scaled = {3'd0, span} * 16'd5;
            period = scaled[14:5];
            expected_ssi_pitch_limit =
                (period == 10'd0) ? 10'd1 : period;
        end
    endfunction

    task automatic start_core_phone(input logic [5:0] phone,
                                    input logic votrax);
        begin
            start_phone = phone;
            start_votrax = votrax;
            start = 1'b1;
            @(posedge clk);
            #1;
            @(negedge clk);
            start = 1'b0;
        end
    endtask

    // Call at a falling edge. effective is the DIV2-qualified value sampled
    // by the core at the following rising edge.
    task automatic pulse_raw_xck(output logic effective);
        begin
            effective = dut.ssi_div2_phase_q;
            xck_ce = 1'b1;
            @(posedge clk);
            #1;
            @(negedge clk);
            xck_ce = 1'b0;
        end
    endtask

    task automatic test_native_fallback_phone;
        begin
            cold_reset();
            current_function = 2'd2;
            duration_phoneme = 8'h80 | 8'h31;
            set_inflection_word(12'hA20, 4'hF);
            start_core_phone(6'h31, 1'b0);

            // SSI phone 31 (J) maps to SC-01 STOP in the old phone map. Its
            // native row is F1=2, F2=B, F2Q=0, F3=E, VA=2, FA=F.
            require(rom_f1 == 4'h2 && rom_f2 == 4'hB &&
                    rom_f2q == 4'h0 && rom_f3 == 4'hE,
                    "phone 31 did not load its native formant targets");
            require(rom_va == 4'h2 && rom_fa == 4'hF,
                    "phone 31 lost its native voice/fricative targets");
            require(!rom_silence && !rom_closure,
                    "phone 31 inherited silence or closure from SC-01 STOP");
            require(!dut.is_votrax_q,
                    "native phone entered the Votrax target path");
        end
    endtask

    task automatic test_exact_div2_duration;
        integer raw_pulses;
        integer effective_pulses;
        logic effective;
        begin
            cold_reset();
            current_function = 2'd3;
            duration_phoneme = 8'hC0 | 8'h31; // D=3
            set_inflection_word(12'hA20, 4'hF); // R=15
            start_core_phone(6'h31, 1'b0);

            effective_pulses = 0;
            for (raw_pulses = 1; raw_pulses < 8192;
                 raw_pulses = raw_pulses + 1) begin
                pulse_raw_xck(effective);
                if (effective) begin
                    effective_pulses = effective_pulses + 1;
                end
                require(!phoneme_done,
                        "D=3/R=15 DONE asserted before raw XCK pulse 8192");
                require(!response_done,
                        "phoneme response asserted before phone completion");
            end

            pulse_raw_xck(effective);
            if (effective) begin
                effective_pulses = effective_pulses + 1;
            end
            require(phoneme_done,
                    "D=3/R=15 did not finish on raw XCK pulse 8192");
            require(response_done,
                    "phoneme mode did not respond with phone completion");
            require(effective_pulses == 4096 && ticks == 5'd0,
                    "D=3/R=15 did not consume 4096 effective XCK edges");

            // With no replacement phone, the SSI repeats the current phone
            // and produces another response after a full phone interval.
            for (raw_pulses = 1; raw_pulses < 8192;
                 raw_pulses = raw_pulses + 1) begin
                pulse_raw_xck(effective);
                require(!phoneme_done && !response_done,
                        "repeating phone responded before its next boundary");
            end
            pulse_raw_xck(effective);
            require(phoneme_done && response_done,
                    "phone did not repeat without a replacement write");
        end
    endtask

    task automatic test_frame_response_is_not_phone_end;
        integer raw_pulses;
        integer effective_pulses;
        integer responses;
        integer phone_boundaries;
        logic effective;
        begin
            cold_reset();
            current_function = 2'd1;
            duration_phoneme = 8'h40 | 8'h31; // frame mode, D=1
            set_inflection_word(12'hA20, 4'hF); // R=15
            start_core_phone(6'h31, 1'b0);

            effective_pulses = 0;
            responses = 0;
            phone_boundaries = 0;
            for (raw_pulses = 1; raw_pulses <= 32768;
                 raw_pulses = raw_pulses + 1) begin
                pulse_raw_xck(effective);
                if (effective) begin
                    effective_pulses = effective_pulses + 1;
                end
                if (response_done) begin
                    responses = responses + 1;
                    require(effective_pulses == responses * 4096,
                            "frame response used phoneme-duration timing");
                    if (responses < 3 || responses == 4) begin
                        require(!phoneme_done && ticks != 5'h10,
                                "frame response ended the active phone");
                    end
                end
                if (phoneme_done) begin
                    phone_boundaries = phone_boundaries + 1;
                end
            end

            require(responses == 4,
                    "frame responses stopped at the first phone boundary");
            require(phone_boundaries == 1 && ticks != 5'h10,
                    "frame-mode phone did not wrap and remain active");
        end
    endtask

    task automatic test_disabled_mode_has_no_response;
        integer raw_pulses;
        logic effective;
        begin
            cold_reset();
            current_function = 2'd0;
            duration_phoneme = 8'h00 | 8'h31; // D=0, response disabled
            set_inflection_word(12'hA20, 4'hF); // R=15
            start_core_phone(6'h31, 1'b0);

            for (raw_pulses = 1; raw_pulses <= 32768;
                 raw_pulses = raw_pulses + 1) begin
                pulse_raw_xck(effective);
                require(!response_done,
                        "disabled response mode asserted A/R completion");
            end
            require(phoneme_done,
                    "disabled response mode did not finish internal audio timing");
            require(ticks == 5'd0 && dut.ssi_duration_active_q,
                    "disabled response mode stopped instead of repeating the phone");
        end
    endtask

    task automatic test_all_rate_and_duration_reloads;
        integer mode;
        integer rate;
        integer expected_slot;
        integer expected_duration_slot;
        begin
            for (mode = 1; mode <= 3; mode = mode + 1) begin
                for (rate = 0; rate <= 15; rate = rate + 1) begin
                    cold_reset();
                    current_function = mode[1:0];
                    duration_phoneme = (mode << 6) | 8'h0B;
                    set_inflection_word(12'hA20, rate[3:0]);
                    start_core_phone(6'h0B, 1'b0);
                    expected_slot = 256 * (16 - rate) - 1;
                    expected_duration_slot =
                        (4 - mode) * 256 * (16 - rate) - 1;
                    require(dut.ssi_response_subticks_left_q == expected_slot,
                            "RATE did not set the SSI response-slot reload");
                    require(dut.ssi_durclk_ticks_left_q == expected_duration_slot,
                            "DR/R did not set the SSI phone-duration reload");
                end
            end
        end
    endtask

    task automatic test_all_ssi_start_pitch_limits;
        integer value;
        logic [11:0] target;
        begin
            cold_reset();
            current_function = 2'd2;
            duration_phoneme = 8'h80 | 8'h0B;
            for (value = 0; value < 4096; value = value + 1) begin
                target = value[11:0];
                set_inflection_word(target, 4'h0);
                start_core_phone(6'h0B, 1'b0);
                require(dut.target_inflection_q == target &&
                        dut.active_inflection_q == target,
                        "non-transitioned start changed the inflection word");
                require(dut.pitch_limit_q == expected_ssi_pitch_limit(target),
                        "SSI start pitch limit changed during refactor");
            end
        end
    endtask

    task automatic test_rate_change_waits_for_slot_reload;
        integer raw_pulses;
        logic effective;
        begin
            cold_reset();
            current_function = 2'd1;
            duration_phoneme = 8'h40 | 8'h31;
            set_inflection_word(12'hA20, 4'hF);
            start_core_phone(6'h31, 1'b0);

            // Finish one 256-edge R=15 slot, then consume half of the next.
            for (raw_pulses = 0; raw_pulses < 512;
                 raw_pulses = raw_pulses + 1) begin
                pulse_raw_xck(effective);
            end
            require(dut.ssi_response_slot_q == 4'd1 &&
                    dut.ssi_response_subticks_left_q == 12'd255,
                    "first response slot did not reload at R=15");
            for (raw_pulses = 0; raw_pulses < 256;
                 raw_pulses = raw_pulses + 1) begin
                pulse_raw_xck(effective);
            end
            require(dut.ssi_response_subticks_left_q == 12'd127,
                    "partial response slot count is wrong");

            set_inflection_word(12'hA20, 4'hE);
            require(dut.ssi_response_subticks_left_q == 12'd127,
                    "RATE write rewrote the response slot in progress");
            for (raw_pulses = 0; raw_pulses < 256;
                 raw_pulses = raw_pulses + 1) begin
                pulse_raw_xck(effective);
            end
            require(dut.ssi_response_slot_q == 4'd2 &&
                    dut.ssi_response_subticks_left_q == 12'd511,
                    "new RATE did not apply at the next response-slot reload");
        end
    endtask

    task automatic full_interval_after_idle(input integer idle_pulses);
        integer index;
        integer raw_pulses;
        integer effective_pulses;
        logic effective;
        begin
            // The duration engine is idle after the prior DONE. Raw XCK and
            // DIV2 keep their physical phase during this CTL-like wait.
            for (index = 0; index < idle_pulses; index = index + 1) begin
                pulse_raw_xck(effective);
            end

            current_function = 2'd3;
            duration_phoneme = 8'hC0 | 8'h31;
            set_inflection_word(12'hA20, 4'hF);
            start_core_phone(6'h31, 1'b0);

            raw_pulses = 0;
            effective_pulses = 0;
            while (!phoneme_done && raw_pulses < 8193) begin
                pulse_raw_xck(effective);
                raw_pulses = raw_pulses + 1;
                if (effective) begin
                    effective_pulses = effective_pulses + 1;
                end
                if (effective_pulses < 4096) begin
                    require(!phoneme_done,
                            "restart DONE asserted before a full interval");
                end
            end

            require(phoneme_done,
                    "restart did not finish after a full interval");
            require(effective_pulses == 4096,
                    "idle DIV2 phase changed the restarted phone duration");
            require(raw_pulses == 8191 || raw_pulses == 8192,
                    "restart raw XCK count was outside the two DIV2 phases");
        end
    endtask

    task automatic test_idle_restart_duration;
        begin
            cold_reset();
            current_function = 2'd3;
            duration_phoneme = 8'hC0 | 8'h0B;
            set_inflection_word(12'hA20, 4'hF);
            start_core_phone(6'h0B, 1'b0);

            // First reach an idle state, then cover both DIV2 phases and waits
            // much longer than one internal duration slot.
            full_interval_after_idle(8192);
            full_interval_after_idle(0);
            full_interval_after_idle(1);
            full_interval_after_idle(17);
            full_interval_after_idle(4097);
        end
    endtask

    task automatic test_transitioned_inflection_seed;
        logic [11:0] first_target;
        logic [11:0] second_target;
        logic [11:0] expected_active;
        begin
            cold_reset();
            current_function = 2'd3;
            duration_phoneme = 8'hC0 | 8'h0B;

            first_target = 12'hA5C;
            set_inflection_word(first_target, 4'hF);
            start_core_phone(6'h0B, 1'b0);
            require(dut.transitioned_inflection_seeded_q,
                    "first transitioned phone did not set the one-time seed");
            require(dut.target_inflection_q == first_target &&
                    dut.active_inflection_q == first_target,
                    "first transitioned phone did not seed from its target");

            second_target = 12'h6E3;
            set_inflection_word(second_target, 4'hF);
            start_core_phone(6'h0C, 1'b0);
            expected_active = {second_target[11], first_target[10:6],
                               second_target[5:0]};
            require(dut.transitioned_inflection_seeded_q,
                    "later transitioned phone cleared the one-time seed");
            require(dut.target_inflection_q == second_target,
                    "later transitioned phone did not latch its new target");
            require(dut.active_inflection_q == expected_active &&
                    dut.active_inflection_q != second_target,
                    "later transitioned phone incorrectly reseeded pitch");
        end
    endtask

    task automatic test_votrax_sc01_targets;
        logic [5:0] phone;
        begin
            cold_reset();
            phone = 6'h25;
            current_function = 2'd0;
            duration_phoneme = 8'd0;
            set_inflection_word(12'h000, 4'd0);
            start_core_phone(phone, 1'b1);

            require(dut.is_votrax_q && rom_phone == phone,
                    "Votrax phone did not stay on the SC-01 path");
            require(rom_f1 == sc01a_f1(phone) &&
                    rom_f2 == sc01a_f2(phone) &&
                    rom_f2q == sc01a_f2q(phone) &&
                    rom_f3 == sc01a_f3(phone),
                    "Votrax formants no longer match the SC-01 ROM");
            require(rom_va == sc01a_va(phone) &&
                    rom_fa == sc01a_fa(phone) &&
                    rom_fc == sc01a_fc(phone),
                    "Votrax source targets no longer match the SC-01 ROM");
            require(rom_cld == sc01a_cld(phone) &&
                    rom_vd == sc01a_vd(phone) &&
                    rom_duration == sc01a_duration(phone) &&
                    rom_closure == sc01a_closure(phone) &&
                    rom_silence == sc01a_pause(phone),
                    "Votrax timing traits no longer match the SC-01 ROM");
        end
    endtask

    initial begin
        test_native_fallback_phone();
        test_exact_div2_duration();
        test_frame_response_is_not_phone_end();
        test_disabled_mode_has_no_response();
        test_all_rate_and_duration_reloads();
        test_all_ssi_start_pitch_limits();
        test_rate_change_waits_for_slot_reload();
        test_idle_restart_duration();
        test_transitioned_inflection_seed();
        test_votrax_sc01_targets();

        $display("SSI263 HYBRID CORE PASS");
        $finish;
    end

endmodule
