`timescale 1ns / 1ps

module tb_ssi263_sc02_audio;

    logic clk = 1'b0;
    logic rstn = 1'b0;

    logic audio_tick = 1'b0;
    logic phone_active = 1'b0;
    logic powered_down = 1'b0;
    logic fricative = 1'b0;
    logic voiced = 1'b0;
    logic pw_2 = 1'b0;
    logic pw_3 = 1'b0;
    logic noise_clock_ce = 1'b0;
    logic fric1_sw = 1'b1;
    logic fric2_sw = 1'b1;
    logic voice_toggle = 1'b0;
    logic filter_phase_ce = 1'b0;
    logic filter_phase = 1'b0;
    logic [7:0] filter_frequency = 8'hE9;
    logic [3:0] f1_code = 4'h0;
    logic [3:0] f2_code = 4'h0;
    logic [3:0] f2_res_code = 4'h0;
    logic [3:0] f3_code = 4'h0;
    logic [3:0] f4_code = 4'h0;
    logic [3:0] filter_amp_code = 4'h0;
    logic [3:0] voice_amp_code = 4'h0;
    logic [3:0] fric_amp_code = 4'h0;
    logic closure = 1'b0;
    logic signed [15:0] audio_sample;

    logic phase_xck_ce = 1'b0;
    logic phase_write_active = 1'b0;
    logic [2:0] phase_write_reg = 3'd0;
    logic [7:0] phase_write_data = 8'd0;
    logic [7:0] phase_filter_frequency;
    logic phase_d7_pending;
    logic phase_ar_drive_low;
    logic phase_filter_ce;
    logic phase_filter_phase;
    logic phase_closure;
    logic phase_voice_clock_ce;
    logic phase_voice_toggle;
    logic phase_noise_clock_ce;
    logic phase_phone_active;
    logic phase_fricative;
    logic phase_pw_2;
    logic phase_pw_3;
    logic [3:0] phase_fric_amp_code;
    logic phase_audio_enable = 1'b0;
    logic phase_noise_test_enable = 1'b0;
    logic signed [15:0] phase_audio_sample;

    integer failures = 0;
    integer i;
    integer previous_value;
    integer interval;
    integer energy_low;
    integer energy_high;
    integer engine_cycles;
    integer voice_signature;
    integer fric_signature;
    integer voice_peak;
    integer fric_peak;
    integer fric_hold_peak;
    integer fric_f5_peak;
    integer voice_hold_peak;
    integer voice_f1_peak;
    integer voice_f5_peak;
    integer phase_noise_edges = 0;
    integer phase_voice_loads;
    integer phase_voice_gap;
    integer phase_ticks_since_load;
    logic phase_seen;
    logic phase_prior_load_pending;
    logic signed [15:0] held_sample;
    logic signed [23:0] held_f1;
    logic signed [23:0] held_f1_charge;
    logic signed [23:0] held_f1_input_history;
    logic signed [23:0] held_f2;
    logic signed [23:0] held_f2_charge;
    logic signed [23:0] held_f3;
    logic signed [23:0] held_f3_charge;
    logic signed [23:0] held_f3_side_history;
    logic signed [23:0] held_f4;
    logic signed [23:0] held_f4_charge;
    logic signed [23:0] held_f5;
    logic signed [23:0] held_f5_charge;
    logic signed [23:0] held_reconstruction;
    logic signed [23:0] held_fric1_source;
    logic signed [23:0] held_fric2_source;
    logic signed [23:0] held_fric2_base_history;
    logic signed [23:0] held_fric2_sw_history;
    logic signed [23:0] held_output;

    integer ref_d1;
    integer ref_d2;
    integer ref_d3;
    integer ref_d4;
    integer ref_count;
    integer ref_count_next;
    integer ref_feedback;

    always #5 clk = ~clk;

    always_ff @(posedge clk) begin
        if (!rstn)
            phase_noise_edges <= 0;
        else if (phase_noise_clock_ce)
            phase_noise_edges <= phase_noise_edges + 1;
    end

    ssi263_sc02_audio dut (
        .clk(clk),
        .rstn(rstn),
        .audio_tick(audio_tick),
        .phone_active(phone_active),
        .powered_down(powered_down),
        .fricative(fricative),
        .voiced(voiced),
        .pw_2(pw_2),
        .pw_3(pw_3),
        .noise_clock_ce(noise_clock_ce),
        .fric1_sw(fric1_sw),
        .fric2_sw(fric2_sw),
        .voice_toggle(voice_toggle),
        .filter_phase_ce(filter_phase_ce),
        .filter_phase(filter_phase),
        .filter_frequency(filter_frequency),
        .f1_code(f1_code),
        .f2_code(f2_code),
        .f2_res_code(f2_res_code),
        .f3_code(f3_code),
        .f4_code(f4_code),
        .filter_amp_code(filter_amp_code),
        .voice_amp_code(voice_amp_code),
        .fric_amp_code(fric_amp_code),
        .closure(closure),
        .audio_sample(audio_sample)
    );

    // This core instance proves the complete 00-FF FILT divider.  A second
    // audio instance consumes that same phase boundary for the FF rate test.
    ssi263_sc02_core #(
        .REVISION_AP(1'b1),
        .ROM_FILE("ssi263_sc02_rom.mem")
    ) phase_core (
        .clk(clk),
        .rstn(rstn),
        .pd_rst_n(1'b1),
        .xck_ce(phase_xck_ce),
        .div2(1'b0),
        .write_active(phase_write_active),
        .write_reg(phase_write_reg),
        .write_data(phase_write_data),
        .d7_pending(phase_d7_pending),
        .ar_drive_low(phase_ar_drive_low),
        .powered_down(),
        .phone_active(phase_phone_active),
        .ar_enabled(),
        .response_phoneme(),
        .transitioned_pitch(),
        .phoneme(),
        .duration(),
        .rate(),
        .inflection(),
        .pitch_inflection(),
        .transitioned_inflection_state(),
        .articulation(),
        .amplitude(),
        .filter_frequency(phase_filter_frequency),
        .effective_xck_ce(),
        .response_boundary_ce(),
        .voice_clock_ce(phase_voice_clock_ce),
        .voice_toggle(phase_voice_toggle),
        .pitch_period_ce(),
        .noise_clock_ce(phase_noise_clock_ce),
        .filter_phase_ce(phase_filter_ce),
        .filter_phase(phase_filter_phase),
        .selector(),
        .selector_phase(),
        .selector_step_ce(),
        .selector_rom_data(),
        .selector_flags(),
        .phone_fricative(),
        .phone_voiced(),
        .pw_0(),
        .pw_1(),
        .pw_2(phase_pw_2),
        .pw_3(phase_pw_3),
        .pw_5(),
        .fric1_sw(),
        .fric2_sw(),
        .fricative(phase_fricative),
        .voiced(),
        .closure(phase_closure),
        .rate_clock_ce(),
        .rate_clock_div2_ce(),
        .articulation_step_ce(),
        .inflection_step_ce(),
        .parameter_write_ce(),
        .parameter_write_selector(),
        .f1_code(),
        .f2_code(),
        .f2_res_code(),
        .f3_code(),
        .f4_code(),
        .filter_amp_code(),
        .voice_amp_code(),
        .fric_amp_code(phase_fric_amp_code)
    );

    ssi263_sc02_audio phase_audio (
        .clk(clk),
        .rstn(rstn),
        .audio_tick(audio_tick),
        .phone_active(phase_noise_test_enable ? phase_phone_active : 1'b1),
        .powered_down(1'b0),
        .fricative(phase_noise_test_enable ? phase_fricative : 1'b0),
        .voiced(!phase_noise_test_enable),
        .pw_2(phase_noise_test_enable ? phase_pw_2 : 1'b0),
        .pw_3(phase_noise_test_enable ? phase_pw_3 : 1'b0),
        .noise_clock_ce(phase_noise_clock_ce && phase_noise_test_enable),
        .fric1_sw(1'b0),
        .fric2_sw(1'b0),
        .voice_toggle(phase_voice_toggle),
        .filter_phase_ce(phase_filter_ce && phase_audio_enable),
        .filter_phase(phase_filter_phase),
        .filter_frequency(phase_filter_frequency),
        .f1_code(4'hF),
        .f2_code(4'hF),
        .f2_res_code(4'hF),
        .f3_code(4'hF),
        .f4_code(4'hF),
        .filter_amp_code(4'hF),
        .voice_amp_code(4'hF),
        .fric_amp_code(
            phase_noise_test_enable ? phase_fric_amp_code : 4'h0
        ),
        .closure(phase_closure && phase_audio_enable),
        .audio_sample(phase_audio_sample)
    );

    function automatic integer pair_magnitude(
        input logic signed [23:0] state_i,
        input logic signed [23:0] state_q
    );
        integer magnitude_i;
        integer magnitude_q;
        begin
            magnitude_i = (state_i < 0) ? -state_i : state_i;
            magnitude_q = (state_q < 0) ? -state_q : state_q;
            pair_magnitude = magnitude_i + magnitude_q;
        end
    endfunction

    function automatic integer sample_magnitude(
        input logic signed [23:0] sample
    );
        begin
            sample_magnitude = (sample < 0) ? -sample : sample;
        end
    endfunction

    task automatic check(input logic condition, input string message);
        begin
            if (!condition) begin
                $display("SSI263 SC02 AUDIO FAIL: %s", message);
                failures = failures + 1;
            end
        end
    endtask

    task automatic reset_all;
        begin
            @(negedge clk);
            rstn = 1'b0;
            audio_tick = 1'b0;
            phone_active = 1'b0;
            powered_down = 1'b0;
            fricative = 1'b0;
            voiced = 1'b0;
            pw_2 = 1'b0;
            pw_3 = 1'b0;
            fric1_sw = 1'b0;
            fric2_sw = 1'b0;
            filter_frequency = 8'hE9;
            f1_code = 4'h0;
            f2_code = 4'h0;
            f2_res_code = 4'h0;
            f3_code = 4'h0;
            f4_code = 4'h0;
            filter_amp_code = 4'h0;
            voice_amp_code = 4'h0;
            fric_amp_code = 4'h0;
            voice_toggle = 1'b0;
            filter_phase_ce = 1'b0;
            noise_clock_ce = 1'b0;
            closure = 1'b0;
            phase_xck_ce = 1'b0;
            phase_write_active = 1'b0;
            phase_audio_enable = 1'b0;
            phase_noise_test_enable = 1'b0;
            repeat (4) @(negedge clk);
            rstn = 1'b1;
            repeat (2) @(negedge clk);
        end
    endtask

    task automatic wait_engine_idle;
        begin
            while (dut.engine_busy_q)
                @(negedge clk);
        end
    endtask

    task automatic set_voice_toggle(input logic new_value);
        begin
            @(negedge clk);
            voice_toggle = new_value;
        end
    endtask

    task automatic pulse_filter(input logic new_phase);
        begin
            wait_engine_idle();
            @(negedge clk);
            filter_phase = new_phase;
            filter_phase_ce = 1'b1;
            closure = !new_phase;
            @(negedge clk);
            filter_phase_ce = 1'b0;
            closure = 1'b0;
            if (new_phase)
                wait_engine_idle();
        end
    endtask

    task automatic pulse_filter_pair;
        begin
            pulse_filter(1'b0);
            pulse_filter(1'b1);
        end
    endtask

    task automatic pulse_noise;
        begin
            @(negedge clk);
            noise_clock_ce = 1'b1;
            @(negedge clk);
            noise_clock_ce = 1'b0;
        end
    endtask

    task automatic pulse_audio_tick;
        begin
            @(negedge clk);
            audio_tick = 1'b1;
            @(negedge clk);
            audio_tick = 1'b0;
            // The AC-coupling/DC-blocker path completes on two fabric clocks.
            repeat (3) @(negedge clk);
        end
    endtask

    task automatic write_phase_register(
        input logic [2:0] address,
        input logic [7:0] value
    );
        begin
            @(negedge clk);
            phase_write_reg = address;
            phase_write_data = value;
            phase_write_active = 1'b1;
            repeat (2) @(negedge clk);
            phase_write_active = 1'b0;
            repeat (2) @(negedge clk);
        end
    endtask

    task automatic write_phase_filter(input logic [7:0] value);
        begin
            write_phase_register(3'd4, value);
        end
    endtask

    task automatic pulse_phase_xck(output logic saw_phase);
        begin
            @(negedge clk);
            phase_xck_ce = 1'b1;
            @(posedge clk);
            #1 saw_phase = phase_filter_ce;
            @(negedge clk);
            phase_xck_ce = 1'b0;
            @(posedge clk);
            #1;
        end
    endtask

    task automatic pulse_phase_xck_slow(output logic saw_phase);
        begin
            pulse_phase_xck(saw_phase);
            // Even the fastest intended profile gives 133 fabric clocks
            // between phase CEs.  The registered engine takes 34 clocks.
            repeat (140) @(negedge clk);
        end
    endtask

    task automatic wait_phase_terminal;
        begin
            phase_seen = 1'b0;
            while (!phase_seen)
                pulse_phase_xck(phase_seen);
        end
    endtask

    task automatic step_noise_reference;
        begin
            ref_count_next = (ref_count == 15) ? 0 : ref_count + 1;
            ref_feedback = ((ref_count_next < 4) ? 1 : 0) ^
                           ((ref_d1 >> 3) & 1) ^
                           ((ref_d2 >> 4) & 1) ^
                           ((ref_d4 >> 3) & 1) ^
                           ((ref_d4 >> 4) & 1);
            ref_count = ref_count_next;
            ref_d1 = ((ref_d1 << 1) & 15) | ((ref_d3 >> 3) & 1);
            ref_d3 = ((ref_d3 << 1) & 15) | ((ref_d2 >> 4) & 1);
            ref_d2 = ((ref_d2 << 1) & 31) | ((ref_d4 >> 4) & 1);
            ref_d4 = ((ref_d4 << 1) & 31) | ref_feedback;
        end
    endtask

    task automatic deposit_f2_impulse;
        begin
            @(negedge clk);
            force dut.f2_state_q = 24'sd65536;
            force dut.f2_history_q = 24'sd0;
            force dut.f2_input_q = 24'sd0;
            @(negedge clk);
            release dut.f2_state_q;
            release dut.f2_history_q;
            release dut.f2_input_q;
        end
    endtask

    initial begin
        reset_all();

        // Exact source-derived switched-capacitor totals.
        check(dut.f1_capacitance(4'hF) == 13'd2450,
              "F1 capacitor total is not 2450 pF");
        check(dut.f2_capacitance(4'hF) == 13'd4260,
              "F2 capacitor total is not 4260 pF");
        check(dut.f2_res_capacitance(4'hF) == 13'd3320,
              "F2 resonance capacitor total is not 3320 pF");
        check(dut.f3_capacitance(4'hF) == 13'd3090,
              "F3 capacitor total is not 3090 pF");
        check(dut.f4_capacitance(4'hF) == 13'd3040,
              "F4 capacitor total is not 3040 pF");
        check(dut.voice_capacitance(4'hF) == 13'd3320,
              "voice capacitor total is not 3320 pF");
        check(dut.fric1_capacitance(4'hF) == 13'd4010,
              "U157/FRIC1 capacitor total is not 4010 pF");
        check(dut.fric2_capacitance(4'hF) == 13'd4042,
              "U152/FRIC2 capacitor total is not 4042 pF");
        check(dut.filter_amp_capacitance(4'hF) == 13'd1126,
              "filter amplitude capacitor total is not 1126 pF");
        check(dut.sat24_from48(48'sd8388608) == 24'sh7FFFFF,
              "positive internal saturation limit wrapped");
        check(dut.sat24_from48(-48'sd8388609) == -24'sd8388608,
              "negative internal saturation limit wrapped");
        check(dut.sat26_from27(27'sd33554432) == 26'sh1FFFFFF,
              "positive DC-state saturation limit wrapped");
        check(dut.sat26_from27(-27'sd33554433) == -26'sd33554432,
              "negative DC-state saturation limit wrapped");
        check(dut.sat16_from27(27'sd32768) == 16'sh7FFF,
              "positive PCM conversion wrapped");
        check(dut.sat16_from27(-27'sd32769) == -16'sd32768,
              "negative PCM conversion wrapped");

        // Exact Q14 endpoints guard the cap-ratio tables as well as their
        // direction.  The Python wrapper checks every entry against the
        // rounded ratios from sheets 1 and 2.
        check(dut.F1_ALPHA_Q14 == 17'sd16104 &&
              dut.F1_A_Q14 == 17'sd3781 &&
              dut.F1_G_Q14 == 17'sd3781,
              "fixed F1 charge ratios changed");
        check(dut.F2_FRIC_H_Q14 == 17'sd2409,
              "C143/F2 second-integrator ratio changed");
        check(dut.F3_ALPHA_Q14 == 17'sd15715 &&
              dut.F3_A_Q14 == 17'sd13040 &&
              dut.F3_G_Q14 == 17'sd6687,
              "fixed F3 charge ratios changed");
        check(dut.F4_ALPHA_Q14 == 17'sd15656 &&
              dut.F4_A_Q14 == 17'sd17112,
              "fixed F4 charge ratios changed");
        check(dut.F5_ALPHA_Q14 == 17'sd15154 &&
              dut.F5_A_Q14 == 17'sd20645 &&
              dut.F5_B_Q14 == 17'sd22320 &&
              dut.F5_FRIC_BASE_G_Q14 == 17'sd5051 &&
              dut.F5_FRIC_SW_G_Q14 == 17'sd16252,
              "fixed F5 charge ratios changed");
        check(dut.FILTER_OUTPUT_ALPHA_Q14 == 17'sd16086 &&
              dut.filter_amp_gain(4'h0) == 17'sd0 &&
              dut.filter_amp_gain(4'hF) == 17'sd6709,
              "C172/C173 or FL_AMP endpoint ratio changed");
        check(dut.f1_b_q14(4'h0) == 17'sd356 &&
              dut.f1_b_q14(4'hF) == 17'sd3847,
              "F1 b endpoint ratio changed");
        check(dut.f2_b_q14(4'h0) == 17'sd1205 &&
              dut.f2_b_q14(4'hF) == 17'sd11469,
              "F2 b endpoint ratio changed");
        check(dut.f2_alpha_q14(4'h0) == 17'sd15916 &&
              dut.f2_alpha_q14(4'hF) == 17'sd10796 &&
              dut.f2_a_q14(4'h0) == 17'sd11001 &&
              dut.f2_a_q14(4'hF) == 17'sd7462,
              "F2 loss endpoint ratios changed");
        check(dut.f3_b_q14(4'h0) == 17'sd2858 &&
              dut.f3_b_q14(4'hF) == 17'sd13630,
              "F3 b endpoint ratio changed");
        check(dut.f4_b_q14(4'h0) == 17'sd6363 &&
              dut.f4_b_q14(4'hF) == 17'sd17946,
              "F4 b endpoint ratio changed");

        // Every formant code raises its sampled input ratio.  F2 RES adds
        // the drawn cap bank, so both alpha and a fall as damping rises.
        for (i = 1; i < 16; i = i + 1) begin
            check(dut.f1_b_q14(i[3:0]) >
                  dut.f1_b_q14((i - 1) & 4'hF),
                  "an F1 code did not raise b");
            check(dut.f2_b_q14(i[3:0]) >
                  dut.f2_b_q14((i - 1) & 4'hF),
                  "an F2 code did not raise b");
            check(dut.f3_b_q14(i[3:0]) >
                  dut.f3_b_q14((i - 1) & 4'hF),
                  "an F3 code did not raise b");
            check(dut.f4_b_q14(i[3:0]) >
                  dut.f4_b_q14((i - 1) & 4'hF),
                  "an F4 code did not raise b");
            check(dut.f2_alpha_q14(i[3:0]) <
                  dut.f2_alpha_q14((i - 1) & 4'hF),
                  "an F2 resonance code did not lower alpha");
            check(dut.f2_a_q14(i[3:0]) <
                  dut.f2_a_q14((i - 1) & 4'hF),
                  "an F2 resonance code did not lower a");
        end

        // Sheet 6 takes FRICATIVE from inverted U73 D4+5 (pin 9), not the
        // adjacent D3+4 tap.  PW3 then forces the negative source while U62 Q
        // is low.  Prove both facts without advancing the HCC state.
        phone_active = 1'b1;
        fricative = 1'b1;
        fric_amp_code = 4'hF;
        force dut.noise_d3_q = 4'b1000;
        force dut.noise_d4_q = 5'b00000;
        pw_3 = 1'b0;
        voice_toggle = 1'b0;
        #1;
        check(dut.noise_bit && dut.fric1_source < 0 &&
              dut.fric2_source < 0 &&
              dut.fric1_source != dut.fric2_source,
              "FRICATIVE did not use inverted U73 D4+5");
        @(posedge clk);
        #1;
        force dut.noise_d3_q = 4'b0000;
        force dut.noise_d4_q = 5'b10000;
        #1;
        check(!dut.noise_bit && dut.fric1_source == 0 &&
              dut.fric2_source == 0,
              "FRICATIVE still followed the old U73 D3+4 tap");
        force dut.noise_d4_q = 5'b00000;
        pw_3 = 1'b1;
        voice_toggle = 1'b0;
        #1;
        check(!dut.noise_bit && dut.fric1_source == 0 &&
              dut.fric2_source == 0,
              "PW3/U62 low phase did not force the fricative source low");
        voice_toggle = 1'b1;
        #1;
        check(dut.noise_bit && dut.fric1_source < 0 &&
              dut.fric2_source < 0,
              "PW3/U62 high phase did not restore the HCC noise tap");
        fricative = 1'b0;
        #1;
        check(dut.fric1_source == 0 && dut.fric2_source == 0,
              "disabled fricative source ignored its zero guard");
        release dut.noise_d3_q;
        release dut.noise_d4_q;
        reset_all();

        // U157 is reset in Phi1 and regenerates -Csel/C133*x in every Phi0.
        // U152 has no phase reset and changes only at a real HCC edge, using
        // the FRIC_AMP bank selected at that edge.
        phone_active = 1'b1;
        fricative = 1'b1;
        fric_amp_code = 4'hF;
        force dut.noise_d4_q = 5'b00000;
        #1;
        check(dut.fric1_source == -24'sd67392 &&
              dut.fric2_source == -24'sd67920,
              "full-code HCC rise did not charge U157/U152 exactly");
        @(posedge clk);
        #1;
        check(dut.fric1_source == -24'sd67392 &&
              dut.fric2_source_state_q == -24'sd67920,
              "HCC rise did not establish both source nodes");
        fric_amp_code = 4'h1;
        #1;
        check(dut.fric1_source == -24'sd4544 &&
              dut.fric2_source == -24'sd67920,
              "U157/U152 did not differ on a steady-HCC gain change");
        force dut.noise_d4_q = 5'b10000;
        #1;
        check(dut.fric1_source == 24'sd0 &&
              dut.fric2_source == -24'sd63376,
              "code-one HCC fall used the wrong edge charge");
        @(posedge clk);
        force dut.noise_d4_q = 5'b00000;
        #1;
        check(dut.fric1_source == -24'sd4544 &&
              dut.fric2_source == -24'sd67920,
              "next HCC rise did not restore the retained source charge");
        release dut.noise_d4_q;
        reset_all();

        // A held-high HCC bit must regenerate U157 on every filter pair.  An
        // edge-only model would make the second Phi0 snapshot zero.
        phone_active = 1'b1;
        fricative = 1'b1;
        fric_amp_code = 4'hF;
        force dut.noise_d4_q = 5'b00000;
        pulse_filter(1'b0);
        check(dut.fric1_source_phi0_q == -24'sd67392,
              "first high-HCC Phi0 did not sample U157");
        pulse_filter(1'b1);
        pulse_filter(1'b0);
        check(dut.fric1_source_phi0_q == -24'sd67392,
              "held-high HCC did not regenerate U157 on the next Phi0");
        release dut.noise_d4_q;
        reset_all();

        // Phi0 owns the tract input sample.  Changing every live source and
        // switch control after that edge must not alter the two source nodes,
        // route states, or FL_AMP bank that the next Phi1 run will consume.
        phone_active = 1'b1;
        fricative = 1'b1;
        pw_2 = 1'b0;
        pw_3 = 1'b0;
        fric1_sw = 1'b1;
        fric2_sw = 1'b1;
        fric_amp_code = 4'hF;
        f1_code = 4'h1;
        f2_code = 4'h2;
        f2_res_code = 4'h3;
        f3_code = 4'h4;
        f4_code = 4'h5;
        force dut.noise_d4_q = 5'b00000;
        #1;
        filter_amp_code = 4'hA;
        held_fric1_source = dut.fric1_source;
        held_fric2_source = dut.fric2_source;
        pulse_filter(1'b0);
        phone_active = 1'b0;
        fricative = 1'b0;
        fric1_sw = 1'b0;
        fric2_sw = 1'b0;
        fric_amp_code = 4'h0;
        f1_code = 4'hF;
        f2_code = 4'hE;
        f2_res_code = 4'hD;
        f3_code = 4'hC;
        f4_code = 4'hB;
        filter_amp_code = 4'h1;
        repeat (4) @(posedge clk);
        #1;
        check(dut.fric1_source_phi0_q == held_fric1_source &&
              dut.fric2_source_phi0_q == held_fric2_source,
              "Phi0 source snapshots followed later live controls");
        check(dut.fric1_sw_phi0_q && dut.fric2_sw_phi0_q &&
              dut.filter_amp_phi0_q == 4'hA,
              "Phi0 route or FL_AMP snapshot followed later controls");
        check(dut.f1_code_phi0_q == 4'h1 &&
              dut.f2_code_phi0_q == 4'h2 &&
              dut.f2_res_code_phi0_q == 4'h3 &&
              dut.f3_code_phi0_q == 4'h4 &&
              dut.f4_code_phi0_q == 4'h5,
              "Phi0 formant snapshot followed later controls");
        check(dut.f3_input_q == dut.f2_state_q &&
              dut.f5_input_q == dut.f4_state_q,
              "FRIC source was merged into a serial formant input");
        @(negedge clk);
        filter_phase = 1'b1;
        filter_phase_ce = 1'b1;
        @(negedge clk);
        filter_phase_ce = 1'b0;
        while (dut.engine_busy_q && dut.engine_stage_q != 4'd6)
            @(negedge clk);
        #1;
        check(dut.engine_busy_q &&
              dut.engine_coefficient_b == dut.filter_amp_gain(4'hA),
              "U146 engine did not consume the Phi0 FL_AMP snapshot");
        wait_engine_idle();
        release dut.noise_d4_q;
        reset_all();

        // The programming guide says that B/D/P/T/K produce no output unless
        // another phone follows.  PW2=1, PW3=0 identifies that stop class.
        // Keep its held source silent; the following live phone supplies the
        // source while the normal tract transition still shapes the release.
        phone_active = 1'b1;
        fricative = 1'b1;
        pw_2 = 1'b1;
        pw_3 = 1'b0;
        fric1_sw = 1'b1;
        fric2_sw = 1'b0;
        fric_amp_code = 4'hF;
        filter_amp_code = 4'hF;
        f3_code = 4'h8;
        f4_code = 4'h8;
        repeat (4) @(negedge clk);
        check(dut.stop_class && !dut.source_fricative &&
              dut.fric1_source == 0 && dut.fric2_source == 0,
              "held P/T/K stop source was not silent");
        repeat (32) begin
            pulse_noise();
            pulse_filter_pair();
        end
        pulse_audio_tick();
        check(dut.f3_state_q == 0 && dut.f5_state_q == 0 &&
              audio_sample == 0,
              "held P/T/K stop produced a recurring exhaust");

        // P followed by HF: switch to the following phone's live source.  No
        // extra stop-burst state exists in the reconstructed circuit.
        pw_2 = 1'b0;
        pw_3 = 1'b1;
        fricative = 1'b1;
        fric_amp_code = 4'h8;
        voice_toggle = 1'b1;
        repeat (2) @(negedge clk);
        check(!dut.stop_class && dut.source_fricative,
              "following phone did not enable its live source");
        repeat (32) begin
            pulse_noise();
            pulse_filter_pair();
        end
        pulse_audio_tick();
        check(dut.f3_state_q != 0 && dut.f5_state_q != 0 &&
              audio_sample != 0,
              "P-to-HF transition produced no tract output");

        // The same stop class must suppress the voiced B/D source.  PW3=1
        // sustained fricatives such as S/F/SCH must never enter stop state.
        reset_all();
        phone_active = 1'b1;
        voiced = 1'b1;
        pw_2 = 1'b1;
        pw_3 = 1'b0;
        voice_amp_code = 4'hF;
        repeat (4) @(negedge clk);
        check(dut.stop_class && !dut.source_voiced &&
              dut.voice_source == 0,
              "held B/D stop leaked its voiced source");
        reset_all();
        phone_active = 1'b1;
        fricative = 1'b1;
        pw_2 = 1'b1;
        pw_3 = 1'b1;
        fric_amp_code = 4'hF;
        repeat (4) @(negedge clk);
        check(!dut.stop_class && dut.source_fricative,
              "sustained S/F/SCH was mistaken for a stop");
        reset_all();

        // U62/U61/U34/U60: a rising held U62 level loads zero. U60 then
        // free-runs modulo 16 and makes VOICED a one-phase TC pulse.
        phone_active = 1'b1;
        voiced = 1'b1;
        voice_amp_code = 4'hF;
        set_voice_toggle(1'b1);
        pulse_filter(1'b0);
        pulse_filter(1'b0);
        pulse_filter(1'b1);
        check(dut.voice_shape_q == 4'h0 && !dut.voiced_q,
              "rising pitch edge did not load U60");
        for (i = 1; i <= 14; i = i + 1) begin
            pulse_filter(1'b1);
            check(dut.voice_shape_q == i[3:0] && !dut.voiced_q,
                  "U60 ended its low phase early");
        end
        pulse_filter(1'b1);
        check(dut.voice_shape_q == 4'hF && dut.voiced_q,
              "U60 terminal-count pulse missing");
        set_voice_toggle(1'b0);
        pulse_filter(1'b0);
        pulse_filter(1'b0);
        pulse_filter(1'b1);
        check(dut.voice_shape_q == 4'h0 && !dut.voiced_q,
              "U60 did not wrap after terminal count");
        pulse_filter(1'b1);
        check(dut.voice_shape_q == 4'h1 && !dut.voiced_q,
              "U60 did not free-run after wrap");

        // Exact HCC4006/U75 recurrence against an independent reference.
        reset_all();
        phone_active = 1'b1;
        fricative = 1'b1;
        fric_amp_code = 4'hF;
        ref_d1 = 1;
        ref_d2 = 0;
        ref_d3 = 0;
        ref_d4 = 0;
        ref_count = 15;
        for (i = 0; i < 8192; i = i + 1) begin
            step_noise_reference();
            pulse_noise();
            check(dut.noise_count_q == ref_count[3:0],
                  "U75 reference count mismatch");
            check(dut.noise_d1_q == ref_d1[3:0],
                  "HCC4006 D1 mismatch");
            check(dut.noise_d2_q == ref_d2[4:0],
                  "HCC4006 D2 mismatch");
            check(dut.noise_d3_q == ref_d3[3:0],
                  "HCC4006 D3 mismatch");
            check(dut.noise_d4_q == ref_d4[4:0],
                  "HCC4006 D4 mismatch");
        end
        fricative = 1'b0;
        step_noise_reference();
        pulse_noise();
        check(dut.noise_count_q == ref_count[3:0] &&
              dut.noise_d1_q == ref_d1[3:0] &&
              dut.noise_d2_q == ref_d2[4:0] &&
              dut.noise_d3_q == ref_d3[3:0] &&
              dut.noise_d4_q == ref_d4[4:0],
              "ungated noise edge left the exact recurrence");
        powered_down = 1'b1;
        step_noise_reference();
        pulse_noise();
        check(dut.noise_count_q == ref_count[3:0] &&
              dut.noise_d4_q == ref_d4[4:0],
              "power-down incorrectly froze digital noise state");
        powered_down = 1'b0;

        // A ROM-held PW3 is a gate state, not an edge.  Run the native S
        // phone through the core long enough to prove recurring U41C edges
        // keep the attached U75/HCC4006 sequence moving.
        reset_all();
        phase_noise_test_enable = 1'b1;
        write_phase_register(3'd1, 8'hFF);
        write_phase_register(3'd2, 8'hFF);
        // DR=10 selects immediate pitch so I=FFF reaches U59 at once.
        write_phase_register(3'd0, 8'hB0);
        write_phase_register(3'd3, 8'h70);
        // Reset loads the first raw-voice interval for I=000.  Cross that
        // retained interval, then prove the direct I=FFF reload repeats.
        for (i = 0; i < 20000; i = i + 1)
            pulse_phase_xck(phase_seen);
        check(phase_pw_3,
              "native S phone did not hold the PW3 lower-ROM state");
        check(phase_fricative && phase_fric_amp_code != 4'h0,
              "native S phone did not enable its fricative source");
        check(phase_noise_edges > 16,
              "static PW3 produced only one U41C noise edge");
        check(phase_audio.noise_count_q != 4'hF ||
              phase_audio.noise_d1_q != 4'h1 ||
              phase_audio.noise_d2_q != 5'h00 ||
              phase_audio.noise_d3_q != 4'h0 ||
              phase_audio.noise_d4_q != 5'h00,
              "integrated static PW3 left HCC4006 at its seed");

        // The same core run has direct I=FFF.  U59 toggles U62 every four
        // XCK ticks, so two rising U62 transitions and U60 loads are eight
        // XCK ticks apart.  This guards against a doubled or halved pitch.
        phase_audio_enable = 1'b1;
        phase_voice_loads = 0;
        phase_voice_gap = 0;
        phase_ticks_since_load = 0;
        phase_prior_load_pending = phase_audio.voice_load_pending_q;
        for (i = 0; i < 64; i = i + 1) begin
            pulse_phase_xck(phase_seen);
            phase_ticks_since_load = phase_ticks_since_load + 1;
            if (phase_prior_load_pending &&
                !phase_audio.voice_load_pending_q &&
                phase_audio.voice_shape_q == 4'h0) begin
                phase_voice_loads = phase_voice_loads + 1;
                // Ignore the two-stage U61 synchronizer fill event.  The next
                // full core-driven rising-to-rising interval must be exact.
                if (phase_voice_loads == 3)
                    phase_voice_gap = phase_ticks_since_load;
                phase_ticks_since_load = 0;
            end
            phase_prior_load_pending = phase_audio.voice_load_pending_q;
        end
        check(phase_voice_loads >= 3,
              "integrated U62/U61/U60 did not make repeated excitation events");
        check(phase_voice_gap == 8,
              "I=FFF U60 excitation interval was not eight XCK ticks");

        // A high phase starts exactly 34 registered engine clocks.  A phase
        // inside that window must set the sticky diagnostic, not restart it.
        reset_all();
        phone_active = 1'b1;
        voiced = 1'b1;
        voice_amp_code = 4'hF;
        filter_amp_code = 4'hF;
        pulse_filter(1'b0);
        @(negedge clk);
        filter_phase = 1'b1;
        filter_phase_ce = 1'b1;
        @(negedge clk);
        filter_phase_ce = 1'b0;
        engine_cycles = 0;
        while (dut.engine_busy_q) begin
            @(negedge clk);
            engine_cycles = engine_cycles + 1;
        end
        check(engine_cycles == 34,
              "registered charge engine did not take 34 clocks");

        @(negedge clk);
        filter_phase_ce = 1'b1;
        @(negedge clk);
        filter_phase_ce = 1'b0;
        repeat (4) @(negedge clk);
        filter_phase = 1'b0;
        filter_phase_ce = 1'b1;
        @(negedge clk);
        filter_phase_ce = 1'b0;
        check(dut.engine_overrun_q,
              "phase collision did not set the scheduler diagnostic");
        wait_engine_idle();

        // F1 and F3 each use a drawn one-pair history path.  Drive one
        // coherent step into each side input, then remove it and prove the
        // retained prior sample participates in the next charge update.
        reset_all();
        phone_active = 1'b1;
        f1_code = 4'h8;
        pulse_filter(1'b0);
        force dut.f1_input_q = 24'sd65536;
        pulse_filter(1'b1);
        release dut.f1_input_q;
        check(dut.f1_state_q == 24'sd4076 &&
              dut.f1_history_q == -24'sd30248 &&
              dut.f1_input_history_q == 24'sd65536,
              "F1 first charge step changed its rounded p/y result");
        force dut.f1_input_q = 24'sd0;
        pulse_filter(1'b1);
        release dut.f1_input_q;
        check(dut.f1_state_q == 24'sd5918 &&
              dut.f1_history_q == -24'sd13666 &&
              dut.f1_input_history_q == 0,
              "F1 prior-input step changed its rounded p/y result");

        reset_all();
        phone_active = 1'b1;
        f3_code = 4'h8;
        pulse_filter(1'b0);
        force dut.f3_input_q = 24'sd0;
        force dut.f3_side_input_q = 24'sd65536;
        pulse_filter(1'b1);
        release dut.f3_input_q;
        release dut.f3_side_input_q;
        check(dut.f3_state_q == 24'sd13999 &&
              dut.f3_history_q == -24'sd26748 &&
              dut.f3_side_history_q == 24'sd65536,
              "F3 first side step changed its rounded p/y result");
        force dut.f3_side_input_q = 24'sd0;
        pulse_filter(1'b1);
        release dut.f3_side_input_q;
        check(dut.f3_state_q == 24'sd7596 &&
              dut.f3_history_q == 24'sd12234 &&
              dut.f3_side_history_q == 0,
              "F3 prior-side step changed its rounded p/y result");

        // C143 enters F2's second integrator.  It changes y2 directly, keeps
        // p2 at zero for an otherwise empty section, and holds charge while
        // U159D is open.
        reset_all();
        phone_active = 1'b1;
        force dut.f2_input_q = 24'sd0;
        force dut.fric1_source_phi0_q = 24'sd65536;
        force dut.fric1_sw_phi0_q = 1'b1;
        pulse_filter(1'b1);
        release dut.f2_input_q;
        release dut.fric1_source_phi0_q;
        release dut.fric1_sw_phi0_q;
        check(dut.f2_state_q == -24'sd9636 &&
              dut.f2_history_q == 0,
              "C143 did not add 1000/6800 at F2 y");
        @(negedge clk);
        force dut.f2_state_q = 24'sd0;
        force dut.f2_history_q = 24'sd0;
        @(negedge clk);
        release dut.f2_state_q;
        release dut.f2_history_q;
        force dut.fric1_source_phi0_q = -24'sd65536;
        force dut.fric1_sw_phi0_q = 1'b0;
        pulse_filter(1'b1);
        release dut.fric1_source_phi0_q;
        release dut.fric1_sw_phi0_q;
        check(dut.f2_state_q == 0 && dut.f2_history_q == 0,
              "open C143 injected into F2");
        force dut.fric1_source_phi0_q = 24'sd0;
        force dut.fric1_sw_phi0_q = 1'b1;
        pulse_filter(1'b1);
        release dut.fric1_source_phi0_q;
        release dut.fric1_sw_phi0_q;
        check(dut.f2_state_q == 0 && dut.f2_history_q == 0,
              "C143 invented a reconnect-history impulse");

        // C150 is always connected to U152.  C151 has an independent history
        // which holds while U159C is open and rejoins only on reconnect.
        reset_all();
        phone_active = 1'b1;
        force dut.f5_input_q = 24'sd0;
        force dut.fric2_source_phi0_q = 24'sd65536;
        force dut.fric2_sw_phi0_q = 1'b0;
        pulse_filter(1'b1);
        release dut.f5_input_q;
        release dut.fric2_source_phi0_q;
        release dut.fric2_sw_phi0_q;
        check(dut.f5_history_q == -24'sd20204 &&
              dut.f5_state_q == 24'sd27524 &&
              dut.fric2_base_history_q == 24'sd65536 &&
              dut.fric2_sw_history_q == 0,
              "switch-low FRIC2 did not retain the C150 path");

        reset_all();
        phone_active = 1'b1;
        force dut.f5_input_q = 24'sd0;
        force dut.fric2_source_phi0_q = 24'sd65536;
        force dut.fric2_sw_phi0_q = 1'b1;
        pulse_filter(1'b1);
        release dut.f5_input_q;
        release dut.fric2_source_phi0_q;
        release dut.fric2_sw_phi0_q;
        check(dut.f5_history_q == -24'sd85212 &&
              dut.f5_state_q == 24'sd116085 &&
              dut.fric2_base_history_q == 24'sd65536 &&
              dut.fric2_sw_history_q == 24'sd65536,
              "C150 and C151 did not sum before one F5 round");
        @(negedge clk);
        force dut.f5_state_q = 24'sd0;
        force dut.f5_history_q = 24'sd0;
        @(negedge clk);
        release dut.f5_state_q;
        release dut.f5_history_q;
        force dut.fric2_source_phi0_q = -24'sd65536;
        force dut.fric2_sw_phi0_q = 1'b0;
        pulse_filter(1'b1);
        release dut.fric2_source_phi0_q;
        release dut.fric2_sw_phi0_q;
        check(dut.fric2_base_history_q == -24'sd65536 &&
              dut.fric2_sw_history_q == 24'sd65536,
              "open C151 did not hold separately from live C150");
        @(negedge clk);
        force dut.f5_state_q = 24'sd0;
        force dut.f5_history_q = 24'sd0;
        @(negedge clk);
        release dut.f5_state_q;
        release dut.f5_history_q;
        force dut.fric2_source_phi0_q = 24'sd0;
        force dut.fric2_sw_phi0_q = 1'b1;
        pulse_filter(1'b1);
        release dut.fric2_source_phi0_q;
        release dut.fric2_sw_phi0_q;
        check(dut.f5_history_q == 24'sd44804 &&
              dut.f5_state_q == -24'sd61037 &&
              dut.fric2_base_history_q == 0 &&
              dut.fric2_sw_history_q == 0,
              "C151 reconnect did not preserve its independent history");

        // Run all five persistent tract sections through the registered lanes.
        reset_all();
        phone_active = 1'b1;
        voiced = 1'b1;
        voice_amp_code = 4'hF;
        filter_amp_code = 4'hF;
        f1_code = 4'h8;
        f2_code = 4'h8;
        f2_res_code = 4'h8;
        f3_code = 4'h8;
        f4_code = 4'h8;
        set_voice_toggle(1'b1);
        repeat (48) begin
            pulse_noise();
            pulse_filter_pair();
        end
        check(dut.f1_state_q != 0 && dut.f1_history_q != 0,
              "F1 charge section did not become active");
        check(dut.f2_state_q != 0 && dut.f2_history_q != 0,
              "F2 charge section did not become active");
        check(dut.f3_state_q != 0 && dut.f3_history_q != 0,
              "F3 charge section did not become active");
        check(dut.f4_state_q != 0 && dut.f4_history_q != 0,
              "F4 charge section did not become active");
        check(dut.f5_state_q != 0 && dut.f5_history_q != 0,
              "fixed F5 charge section did not become active");

        held_f1 = dut.f1_state_q;
        held_f1_charge = dut.f1_history_q;
        held_f1_input_history = dut.f1_input_history_q;
        held_f2 = dut.f2_state_q;
        held_f2_charge = dut.f2_history_q;
        held_f3 = dut.f3_state_q;
        held_f3_charge = dut.f3_history_q;
        held_f3_side_history = dut.f3_side_history_q;
        held_f4 = dut.f4_state_q;
        held_f4_charge = dut.f4_history_q;
        held_f5 = dut.f5_state_q;
        held_f5_charge = dut.f5_history_q;
        f1_code = 4'hF;
        f2_code = 4'h1;
        f2_res_code = 4'hF;
        f3_code = 4'h2;
        f4_code = 4'h4;
        @(negedge clk);
        check(dut.f1_state_q == held_f1 &&
              dut.f1_history_q == held_f1_charge &&
              dut.f1_input_history_q == held_f1_input_history &&
              dut.f2_state_q == held_f2 &&
              dut.f2_history_q == held_f2_charge &&
              dut.f3_state_q == held_f3 &&
              dut.f3_history_q == held_f3_charge &&
              dut.f3_side_history_q == held_f3_side_history &&
              dut.f4_state_q == held_f4 &&
              dut.f4_history_q == held_f4_charge &&
              dut.f5_state_q == held_f5 &&
              dut.f5_history_q == held_f5_charge,
              "a code change reset persistent y/p or side history state");
        pulse_filter_pair();
        check(dut.f1_state_q != held_f1 && dut.f2_state_q != held_f2 &&
              dut.f3_state_q != held_f3 && dut.f4_state_q != held_f4 &&
              dut.f5_state_q != held_f5,
              "one shared FILT phase did not update every section");
        check(!dut.engine_overrun_q,
              "resonator scheduler exceeded the phase gap");

        // C143 enters F2's second integrator.  It must leave F1 idle, then
        // cross the rest of F2 and the serial F3/F4/F5 tract.
        reset_all();
        phone_active = 1'b1;
        fricative = 1'b1;
        fric1_sw = 1'b1;
        fric2_sw = 1'b0;
        fric_amp_code = 4'hF;
        filter_amp_code = 4'hF;
        f3_code = 4'h8;
        f4_code = 4'h8;
        repeat (64) begin
            pulse_noise();
            pulse_filter_pair();
        end
        check(dut.f1_state_q == 0,
              "FRIC_1 leaked upstream of F2");
        check(dut.f2_state_q != 0 && dut.f3_state_q != 0 &&
              dut.f4_state_q != 0 &&
              dut.f5_state_q != 0,
              "FRIC_1 did not traverse F2 through F5");
        pulse_audio_tick();
        check(audio_sample != 0,
              "FRIC_1 tract injection did not reach reconstructed PCM");

        // C150/C151 enter F5's first integrator.  Only F5 may become active.
        reset_all();
        phone_active = 1'b1;
        fricative = 1'b1;
        fric1_sw = 1'b0;
        fric2_sw = 1'b1;
        fric_amp_code = 4'hF;
        filter_amp_code = 4'hF;
        repeat (64) begin
            pulse_noise();
            pulse_filter_pair();
        end
        check(dut.f1_state_q == 0 && dut.f2_state_q == 0 &&
              dut.f3_state_q == 0 && dut.f4_state_q == 0,
              "FRIC_2 leaked upstream of the F4/F5 injection node");
        check(dut.f5_state_q != 0,
              "FRIC_2 did not traverse fixed F5");
        pulse_audio_tick();
        check(audio_sample != 0,
              "FRIC_2 tract injection did not reach reconstructed PCM");

        // The F2 RES bank loads the first integrator.  Higher codes lower
        // alpha and a, so code F must damp an impulse faster than code 0.
        reset_all();
        phone_active = 1'b1;
        f2_code = 4'h8;
        f2_res_code = 4'h0;
        pulse_filter(1'b0);
        deposit_f2_impulse();
        repeat (10) pulse_filter(1'b1);
        energy_low = pair_magnitude(dut.f2_state_q, dut.f2_history_q);
        reset_all();
        phone_active = 1'b1;
        f2_code = 4'h8;
        f2_res_code = 4'hF;
        pulse_filter(1'b0);
        deposit_f2_impulse();
        repeat (10) pulse_filter(1'b1);
        energy_high = pair_magnitude(dut.f2_state_q, dut.f2_history_q);
        check(energy_high < energy_low,
              "F2 resonance code did not increase charge damping");

        // U146 must combine both wide products before one Q14 round.  These
        // vectors guard the C172 recurrence, FL_AMP scale, and delta sign.
        force dut.product_a_q = 42'sd16086000;  // 16086 * 1000
        force dut.product_b_q = -42'sd6709000; // 6709 * -1000
        #1;
        check(dut.engine_output_next == 24'sd572,
              "U146 full-code recurrence missed its exact one-round result");
        release dut.product_a_q;
        release dut.product_b_q;
        force dut.product_a_q = 42'sd16086000;
        force dut.product_b_q = 42'sd0;
        #1;
        check(dut.engine_output_next == 24'sd982,
              "U146 code-zero recurrence did not retain 2700/2750");
        release dut.product_a_q;
        release dut.product_b_q;

        // The registered timing split must preserve old F5 minus new F5.
        force dut.output_old_state_q = 24'sd1000;
        force dut.f5_state_q = -24'sd1000;
        force dut.engine_busy_q = 1'b1;
        force dut.engine_stage_q = 4'd9;
        @(posedge clk);
        #1;
        release dut.output_old_state_q;
        release dut.f5_state_q;
        release dut.engine_busy_q;
        release dut.engine_stage_q;
        #1;
        check(dut.output_sum_q == 25'sd2000,
              "U146 delta was not old F5 minus new F5");
        reset_all();

        // FL_AMP transfers the F5 change into U146.  Code zero removes the
        // new input term but C172 must retain 2700/2750 of the prior output.
        reset_all();
        phone_active = 1'b1;
        voiced = 1'b1;
        voice_amp_code = 4'hF;
        filter_amp_code = 4'hF;
        f1_code = 4'h8;
        f2_code = 4'h8;
        f2_res_code = 4'h8;
        f3_code = 4'h8;
        f4_code = 4'h8;
        set_voice_toggle(1'b1);
        repeat (48) pulse_filter_pair();
        check(dut.f5_state_q != 0 && dut.reconstruction_hold_q != 0,
              "full filter-amplitude code produced no tract output");
        held_output = dut.output_hold_q;
        filter_amp_code = 4'h0;
        pulse_filter_pair();
        check(dut.output_hold_q != 0 &&
              sample_magnitude(dut.output_hold_q) <
                  sample_magnitude(held_output) &&
              dut.reconstruction_hold_q == held_output,
              "C172 did not retain and decay U146 at FL_AMP zero");
        check(dut.f1_state_q != 0 && dut.f5_state_q != 0,
              "filter-amplitude code zero erased tract state");

        // U145D/CLOSURE copies the completed U146 hold into C100/U148.  Low
        // holds; it never discharges the internal or reconstructed node.
        reset_all();
        phone_active = 1'b1;
        voiced = 1'b1;
        voice_amp_code = 4'hF;
        filter_amp_code = 4'hF;
        f1_code = 4'h8;
        f2_code = 4'h8;
        f2_res_code = 4'h8;
        f3_code = 4'h8;
        f4_code = 4'h8;
        set_voice_toggle(1'b1);
        repeat (48) pulse_filter_pair();
        pulse_audio_tick();
        held_sample = audio_sample;
        check(held_sample != 0, "active filter produced no held output");
        repeat (4) pulse_filter_pair();
        check(audio_sample == held_sample,
              "sample changed without an audio tick");
        pulse_audio_tick();
        held_sample = audio_sample;
        held_output = dut.output_hold_q;
        closure = 1'b1;
        @(negedge clk);
        pulse_audio_tick();
        closure = 1'b0;
        check(dut.output_hold_q == held_output &&
              dut.reconstruction_hold_q == held_output,
              "closure did not copy U146 into the C100/U148 hold");
        held_reconstruction = dut.reconstruction_hold_q;
        repeat (4) @(negedge clk);
        check(dut.reconstruction_hold_q == held_reconstruction,
              "open closure switch did not hold C100/U148");
        check(dut.f1_state_q != 0 && dut.f4_state_q != 0,
              "closure erased persistent formant state");
        pulse_filter_pair();
        pulse_audio_tick();
        check(audio_sample != 0, "output did not resume after closure");

        // Sheets 1 and 2 have no phone-active switch after U148.  Ending a
        // phone stops new excitation but must not hard-mute the held output.
        reset_all();
        phone_active = 1'b1;
        @(negedge clk);
        force dut.output_hold_q = 24'sd65536;
        closure = 1'b1;
        @(posedge clk);
        #1;
        closure = 1'b0;
        release dut.output_hold_q;
        check(dut.reconstruction_hold_q == 24'sd65536,
              "closure did not seed the natural U148 tail test");
        pulse_audio_tick();
        check(audio_sample != 0,
              "known U148 level did not reach PCM while phone was active");
        phone_active = 1'b0;
        pulse_audio_tick();
        check(dut.dc_input == 24'sd65536 && audio_sample != 0,
              "phone end hard-muted the post-U148 output tail");
        powered_down = 1'b1;
        pulse_audio_tick();
        check(audio_sample == 0,
              "powerdown did not mute the PCM boundary");
        reset_all();

        // C381's digital high-pass equivalent must keep the full 25-bit
        // input difference until after its 255/256 leak.  Early 24-bit
        // saturation turns a full positive-to-negative edge into a small
        // false pulse.
        reset_all();
        phone_active = 1'b1;
        force dut.reconstruction_hold_q = 24'sh7FFFFF;
        pulse_audio_tick();
        check(dut.dc_delta_q == 25'sd8388607 &&
              dut.dc_output_q == 26'sd8355840,
              "DC blocker changed its positive full-scale step");
        force dut.reconstruction_hold_q = -24'sd8388608;
        pulse_audio_tick();
        check(dut.dc_delta_q == -25'sd16777215 &&
              dut.dc_output_q == -26'sd8388479,
              "DC blocker clipped a full-scale negative reversal early");
        check(audio_sample == -16'sd32768,
              "DC blocker reversal did not saturate only at PCM output");
        force dut.reconstruction_hold_q = 24'sh7FFFFF;
        pulse_audio_tick();
        check(dut.dc_delta_q == 25'sd16777215 &&
              dut.dc_output_q == 26'sd8355968,
              "DC blocker clipped a full-scale positive reversal early");
        release dut.reconstruction_hold_q;
        reset_all();

        // Exhaust every FILT divider code through the native core boundary.
        reset_all();
        for (i = 0; i < 256; i = i + 1) begin
            write_phase_filter(i[7:0]);
            wait_phase_terminal();
            interval = 0;
            phase_seen = 1'b0;
            while (!phase_seen) begin
                pulse_phase_xck(phase_seen);
                interval = interval + 1;
            end
            check(interval == (256 - i),
                  "FILT divider interval was not 256-FF");
        end

        // FF is maximum phase rate, never a mute shortcut.  Slow the test's
        // artificial XCK pulses enough to model the real fabric-clock gap.
        write_phase_filter(8'hFF);
        wait_phase_terminal();
        // U59 and U62 free-run while powered down.  Stop on a fresh rising
        // U62 level so the enabled Phi sampler sees one real voice pulse.
        while (phase_voice_toggle)
            pulse_phase_xck(phase_seen);
        while (!phase_voice_toggle)
            pulse_phase_xck(phase_seen);
        phase_audio_enable = 1'b1;
        repeat (96) begin
            pulse_phase_xck_slow(phase_seen);
            check(phase_seen, "FILT=FF missed an XCK phase event");
        end
        pulse_audio_tick();
        check(phase_audio.f1_state_q != 0 &&
              phase_audio.f5_state_q != 0,
              "FILT=FF did not update every voice section");
        check(phase_audio_sample != 0,
              "FILT=FF incorrectly muted the held output");
        check(!phase_audio.engine_overrun_q,
              "FF phase rate overran the scheduled engine");

        // Use exact rows from the supplied SC-02 ROM: phone 01 is voiced and
        // phone 30 (S) is fricative.  They must make distinct native output.
        reset_all();
        phone_active = 1'b1;
        f1_code = phase_core.rom_q[9'd8][7:4];
        f2_code = phase_core.rom_q[9'd9][7:4];
        f2_res_code = phase_core.rom_q[9'd10][7:4];
        f3_code = phase_core.rom_q[9'd11][7:4];
        f4_code = phase_core.rom_q[9'd11][7:4];
        voice_amp_code = phase_core.rom_q[9'd13][7:4];
        fric_amp_code = phase_core.rom_q[9'd14][7:4];
        filter_amp_code = 4'hF;
        voiced = !phase_core.rom_q[9'd8][0];
        fricative = !phase_core.rom_q[9'd9][0];
        pw_2 = phase_core.rom_q[9'd10][2];
        pw_3 = phase_core.rom_q[9'd10][1];
        fric1_sw = phase_core.rom_q[9'd10][3];
        fric2_sw = !phase_core.rom_q[9'd10][3];
        check(voiced && !fricative,
              "native phone 01 source flags were not voiced-only");
        repeat (64) pulse_filter_pair();
        voice_signature = 0;
        voice_peak = 0;
        voice_hold_peak = 0;
        voice_f1_peak = 0;
        voice_f5_peak = 0;
        for (i = 0; i < 24; i = i + 1) begin
            if ((i & 7) == 0)
                set_voice_toggle(!voice_toggle);
            pulse_filter_pair();
            pulse_audio_tick();
            voice_signature = voice_signature + audio_sample * (i + 1);
            if (sample_magnitude(audio_sample) > voice_peak)
                voice_peak = sample_magnitude(audio_sample);
            if (sample_magnitude(dut.output_hold_q) > voice_hold_peak)
                voice_hold_peak = sample_magnitude(dut.output_hold_q);
            if (sample_magnitude(dut.f1_state_q) > voice_f1_peak)
                voice_f1_peak = sample_magnitude(dut.f1_state_q);
            if (sample_magnitude(dut.f5_state_q) > voice_f5_peak)
                voice_f5_peak = sample_magnitude(dut.f5_state_q);
        end
        $display("SSI263 LEVEL voice peak=%0d hold=%0d f1=%0d f5=%0d",
                 voice_peak, voice_hold_peak, voice_f1_peak, voice_f5_peak);
        check(voice_signature != 0,
              "native voiced phone produced no output");
        check(voice_peak >= 1024 && voice_hold_peak >= 8192,
              "native voiced phone remained below a nontrivial PCM floor");
        check(voice_peak < 32767,
              "native voiced phone clipped its PCM output");

        reset_all();
        phone_active = 1'b1;
        f1_code = phase_core.rom_q[9'd384][7:4];
        f2_code = phase_core.rom_q[9'd385][7:4];
        f2_res_code = phase_core.rom_q[9'd386][7:4];
        f3_code = phase_core.rom_q[9'd387][7:4];
        f4_code = phase_core.rom_q[9'd387][7:4];
        voice_amp_code = phase_core.rom_q[9'd389][7:4];
        fric_amp_code = phase_core.rom_q[9'd390][7:4];
        filter_amp_code = 4'hF;
        voiced = !phase_core.rom_q[9'd384][0];
        fricative = !phase_core.rom_q[9'd385][0];
        pw_2 = phase_core.rom_q[9'd386][2];
        pw_3 = phase_core.rom_q[9'd386][1];
        fric1_sw = phase_core.rom_q[9'd386][3];
        fric2_sw = !phase_core.rom_q[9'd386][3];
        voice_toggle = 1'b1;
        check(!voiced && fricative,
              "native phone 30 source flags were not fricative-only");
        repeat (64) begin
            pulse_noise();
            pulse_filter_pair();
        end
        fric_signature = 0;
        fric_peak = 0;
        fric_hold_peak = 0;
        fric_f5_peak = 0;
        for (i = 0; i < 24; i = i + 1) begin
            pulse_noise();
            pulse_filter_pair();
            pulse_audio_tick();
            fric_signature = fric_signature + audio_sample * (i + 1);
            if (sample_magnitude(audio_sample) > fric_peak)
                fric_peak = sample_magnitude(audio_sample);
            if (sample_magnitude(dut.output_hold_q) > fric_hold_peak)
                fric_hold_peak = sample_magnitude(dut.output_hold_q);
            if (sample_magnitude(dut.f5_state_q) > fric_f5_peak)
                fric_f5_peak = sample_magnitude(dut.f5_state_q);
        end
        $display("SSI263 LEVEL fric peak=%0d hold=%0d f5=%0d",
                 fric_peak, fric_hold_peak, fric_f5_peak);
        check(fric_signature != 0,
              "native fricative phone produced no output");
        check(fric_signature != voice_signature,
              "native voiced and fricative outputs were not distinct");
        // This synthetic loop advances one HCC edge per reconstructed filter
        // pair, unlike the chip's true XCK/filter/audio cadence.  Use it only
        // as a nonzero and no-clip guard.  The integrated card-rate sweep owns
        // phone-specific RMS, occupancy, and voiced/fricative balance checks.
        check(fric_peak >= 256 && fric_peak < 32767,
              "native fricative phone missed its nonzero PCM range");
        check(!dut.engine_overrun_q,
              "scheduled engine overran during native phone tests");

        if (failures == 0)
            $display("SSI263 SC02 AUDIO PASS");
        else
            $display("SSI263 SC02 AUDIO FAIL: %0d checks", failures);
        $finish;
    end

endmodule
