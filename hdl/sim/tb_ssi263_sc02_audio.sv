`timescale 1ns / 1ps

module tb_ssi263_sc02_audio;

    logic clk = 1'b0;
    logic rstn = 1'b0;
    logic pd_rst_n = 1'b1;

    logic audio_tick = 1'b0;
    logic powered_down = 1'b0;
    logic pw_3 = 1'b0;
    logic noise_clock_ce = 1'b0;
    logic noise_shift_ce = 1'b0;
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
    logic phase_noise_shift_ce;
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
    integer phase_noise_edges = 0;
    integer phase_voice_loads;
    integer phase_voice_gap;
    integer phase_ticks_since_load;
    integer phase_u62_rises;
    integer phase_u62_gap;
    integer phase_ticks_since_u62;
    logic phase_seen;
    logic phase_prior_load_pending;
    logic phase_prior_voice_toggle;
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
    logic signed [24:0] held_c143_delta;
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
        .pd_rst_n(pd_rst_n),
        .audio_tick(audio_tick),
        .powered_down(powered_down),
        .pw_3(pw_3),
        .noise_clock_ce(noise_clock_ce),
        .noise_shift_ce(noise_shift_ce),
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
        .phone_active(),
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
        .noise_shift_ce(phase_noise_shift_ce),
        .filter_phase_ce(phase_filter_ce),
        .filter_phase(phase_filter_phase),
        .selector(),
        .selector_phase(),
        .selector_step_ce(),
        .selector_rom_data(),
        .selector_flags(),
        .pw_0(),
        .pw_1(),
        .pw_2(),
        .pw_3(phase_pw_3),
        .pw_5(),
        .fric1_sw(),
        .fric2_sw(),
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
        .pd_rst_n(1'b1),
        .audio_tick(audio_tick),
        .powered_down(1'b0),
        .pw_3(phase_noise_test_enable ? phase_pw_3 : 1'b0),
        .noise_clock_ce(phase_noise_clock_ce && phase_noise_test_enable),
        .noise_shift_ce(phase_noise_shift_ce && phase_noise_test_enable),
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
            pd_rst_n = 1'b1;
            audio_tick = 1'b0;
            powered_down = 1'b0;
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
            noise_shift_ce = 1'b0;
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

    task automatic pulse_noise_count;
        begin
            @(negedge clk);
            noise_clock_ce = 1'b1;
            @(negedge clk);
            noise_clock_ce = 1'b0;
        end
    endtask

    task automatic pulse_noise_shift;
        begin
            @(negedge clk);
            noise_shift_ce = 1'b1;
            @(negedge clk);
            noise_shift_ce = 1'b0;
        end
    endtask

    task automatic pulse_noise;
        begin
            pulse_noise_count();
            pulse_noise_shift();
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

    task automatic step_noise_count_reference;
        begin
            ref_count_next = (ref_count == 15) ? 1 : ref_count + 1;
            ref_count = ref_count_next;
        end
    endtask

    task automatic step_noise_shift_reference;
        begin
            ref_feedback = ((ref_count < 4) ? 1 : 0) ^
                           ((ref_d1 >> 3) & 1) ^
                           ((ref_d2 >> 4) & 1) ^
                           ((ref_d4 >> 3) & 1) ^
                           ((ref_d4 >> 4) & 1);
            ref_d1 = ((ref_d1 << 1) & 15) | ((ref_d3 >> 3) & 1);
            ref_d3 = ((ref_d3 << 1) & 15) | ((ref_d2 >> 4) & 1);
            ref_d2 = ((ref_d2 << 1) & 31) | ((ref_d4 >> 4) & 1);
            ref_d4 = ((ref_d4 << 1) & 31) | ref_feedback;
        end
    endtask

    task automatic step_noise_reference;
        begin
            step_noise_count_reference();
            step_noise_shift_reference();
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

        // Sheet 6 U51D takes U73 D3+4. U104C is PW3 AND U62./Q;
        // U163F, U51C, and U41D form the complete FRICATIVE equation. Sheets
        // 1/2 then bias both CMOS levels through 390 kohm and 1.8 kohm, so
        // high and low are the exact bipolar +/-3/653 source drive.
        fric_amp_code = 4'hF;
        voice_amp_code = 4'hF;
        force dut.noise_d3_q = 4'b0000;
        pw_3 = 1'b0;
        voice_toggle = 1'b0;
        #1;
        check(dut.noise_bit && dut.fric_drive == 18'sd301 &&
              dut.fric1_source == -24'sd310,
              "D3+4 low did not select the exact CMOS-high source");
        @(posedge clk);
        #1;
        force dut.noise_d3_q = 4'b1000;
        #1;
        check(!dut.noise_bit && dut.fric_drive == -18'sd301 &&
              dut.fric1_source == 24'sd310,
              "D3+4 high did not select the exact CMOS-low source");
        force dut.noise_d3_q = 4'b0000;
        pw_3 = 1'b1;
        voice_toggle = 1'b0;
        #1;
        check(!dut.noise_bit && dut.fric1_source == 24'sd310,
              "PW3 AND U62./Q did not select the CMOS-low source");
        voice_toggle = 1'b1;
        #1;
        check(!dut.noise_bit && dut.fric1_source == 24'sd310,
              "U62.Q high ignored the VOICE_AMP nonzero term");
        voice_amp_code = 4'h0;
        #1;
        check(dut.noise_bit && dut.fric1_source == -24'sd310,
              "VOICE_AMP_ZERO did not restore FRICATIVE");
        force dut.noise_d4_q = 5'b11111;
        #1;
        check(dut.noise_bit,
              "FRICATIVE followed D4+5 instead of D3+4");
        release dut.noise_d3_q;
        release dut.noise_d4_q;
        reset_all();

        // U157 is reset in Phi1 and regenerates -Csel/C133*x in every Phi0.
        // U152 has no phase reset and changes only at a real HCC edge, using
        // the FRIC_AMP bank selected at that edge. Combined rounding must use
        // the exact 3/653 level and 6/653 edge ratios, not staged gain math.
        force dut.noise_d3_q = 4'b1000;
        @(posedge clk);
        fric_amp_code = 4'hF;
        force dut.noise_d3_q = 4'b0000;
        #1;
        check(dut.fric_drive == 18'sd301 &&
              dut.fric_drive_delta == 19'sd602 &&
              dut.fric1_source == -24'sd310 &&
              dut.fric2_source == -24'sd624,
              "full-code HCC rise did not charge U157/U152 exactly");
        @(posedge clk);
        #1;
        check(dut.fric1_source == -24'sd310 &&
              dut.fric2_source_state_q == -24'sd624,
              "HCC rise did not establish both source nodes");
        fric_amp_code = 4'h1;
        #1;
        check(dut.fric1_source == -24'sd21 &&
              dut.fric2_source == -24'sd624,
              "U157/U152 did not differ on a steady-HCC gain change");
        force dut.noise_d3_q = 4'b1000;
        #1;
        check(dut.fric_drive == -18'sd301 &&
              dut.fric_drive_delta == -19'sd602 &&
              dut.fric1_source == 24'sd21 &&
              dut.fric2_source == -24'sd582,
              "code-one HCC fall used the wrong edge charge");
        @(posedge clk);
        force dut.noise_d3_q = 4'b0000;
        #1;
        check(dut.fric1_source == -24'sd21 &&
              dut.fric2_source == -24'sd624,
              "next HCC rise did not restore the retained source charge");
        release dut.noise_d3_q;
        reset_all();

        // A held-high HCC bit must regenerate U157 on every filter pair.  An
        // edge-only model would make the second Phi0 snapshot zero.
        fric_amp_code = 4'hF;
        fric1_sw = 1'b1;
        force dut.noise_d3_q = 4'b0000;
        pulse_filter(1'b0);
        check(dut.c143_source_plate_q == -24'sd310,
              "first high-HCC Phi0 did not charge C143 from U157");
        pulse_filter(1'b1);
        pulse_filter(1'b0);
        check(dut.c143_source_plate_q == -24'sd310 &&
              dut.c143_phi0_delta_q == 25'sd310,
              "held-high HCC did not recharge C143 on the next Phi0");
        release dut.noise_d3_q;
        reset_all();

        // Phi0 owns the tract input sample.  Changing every live source and
        // switch control after that edge must not alter the two source nodes,
        // route states, or FL_AMP bank that the next Phi1 run will consume.
        pw_3 = 1'b0;
        fric1_sw = 1'b1;
        fric2_sw = 1'b1;
        fric_amp_code = 4'hF;
        f1_code = 4'h1;
        f2_code = 4'h2;
        f2_res_code = 4'h3;
        f3_code = 4'h4;
        f4_code = 4'h5;
        force dut.noise_d3_q = 4'b0000;
        #1;
        filter_amp_code = 4'hA;
        held_fric1_source = dut.fric1_source;
        held_fric2_source = dut.fric2_source;
        pulse_filter(1'b0);
        held_c143_delta = dut.c143_phi0_delta_q;
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
        check(dut.c143_source_plate_q == held_fric1_source &&
              dut.c143_phi0_delta_q == held_c143_delta &&
              dut.fric2_source_phi0_q == held_fric2_source,
              "Phi0 source plates followed later live controls");
        check(dut.fric2_sw_phi0_q && dut.filter_amp_phi0_q == 4'hA,
              "Phi0 FRIC2 route or FL_AMP snapshot followed live controls");
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
        check(dut.c143_delta_hold_q == held_c143_delta &&
              dut.fric2_sw_phi0_q,
              "FRIC paths missed their separate phase boundaries");
        while (dut.engine_busy_q && dut.engine_stage_q != 4'd6)
            @(negedge clk);
        #1;
        check(dut.engine_busy_q &&
              dut.engine_coefficient_b == dut.filter_amp_gain(4'hA),
              "U146 engine did not consume the Phi0 FL_AMP snapshot");
        wait_engine_idle();
        release dut.noise_d3_q;
        reset_all();

        // Sheet 6 ties U60 /RST and CET high, CEP to /VOICED, and P0/P1 high.
        // U34 /PE therefore loads 3. Normal Phi1 clocks count to F; CEP then
        // holds F until the next U34 load.
        voice_amp_code = 4'hF;
        set_voice_toggle(1'b1);
        pulse_filter(1'b0);
        check(dut.pitch_sync1_q && !dut.pitch_sync2_q &&
              dut.voice_load_pending_q,
              "U61/U34 did not detect new Q1 with new /Q2");
        held_f1 = dut.f1_state_q;
        held_f1_input_history = dut.f1_input_history_q;
        @(negedge clk);
        filter_phase = 1'b1;
        filter_phase_ce = 1'b1;
        #1;
        check(dut.voice_count_q == 4'hF &&
              dut.voice_count_after_phi1 == 4'h3 &&
              !dut.u60_tc_after_phi1 &&
              dut.voice_source_after_phi1 == dut.voice_magnitude,
              "U34 /PE did not form the post-clock U60 load value 3");
        @(posedge clk);
        #1;
        check(dut.voice_count_q == 4'h3 &&
              dut.f1_input_q == dut.voice_magnitude && dut.engine_busy_q &&
              dut.engine_main_delta_q ==
                  $signed({held_f1[23], held_f1}) -
                  $signed({dut.voice_magnitude[23], dut.voice_magnitude}) &&
              dut.engine_side_delta_q ==
                  $signed({held_f1_input_history[23],
                            held_f1_input_history}) -
                  $signed({dut.voice_magnitude[23], dut.voice_magnitude}),
              "F->3 load did not feed magnitude to F1 on that Phi1");
        @(negedge clk);
        filter_phase_ce = 1'b0;
        wait_engine_idle();

        for (i = 4; i <= 14; i = i + 1) begin
            pulse_filter(1'b0);
            pulse_filter(1'b1);
            check(dut.voice_count_q == i[3:0] &&
                  !dut.u60_tc &&
                  dut.f1_input_q == dut.voice_magnitude,
                  "U60 normal 3..E recurrence changed");
        end

        // U60 and U118A act on the same Phi1 boundary. Use post-clock state:
        // E->F must select zero for the F1 operand on that edge.
        pulse_filter(1'b0);
        held_f1 = dut.f1_state_q;
        held_f1_input_history = dut.f1_input_history_q;
        @(negedge clk);
        filter_phase = 1'b1;
        filter_phase_ce = 1'b1;
        #1;
        check(dut.voice_count_q == 4'hE &&
              dut.voice_count_after_phi1 == 4'hF &&
              dut.u60_tc_after_phi1 &&
              dut.voice_source == dut.voice_magnitude &&
              dut.voice_source_after_phi1 == 24'sd0,
              "U60 E->F did not select the post-clock zero source");
        @(posedge clk);
        #1;
        check(dut.voice_count_q == 4'hF && dut.u60_tc &&
              dut.f1_input_q == 24'sd0 && dut.engine_busy_q &&
              dut.engine_main_delta_q ==
                  $signed({held_f1[23], held_f1}) &&
              dut.engine_side_delta_q ==
                  $signed({held_f1_input_history[23],
                            held_f1_input_history}),
              "F1 did not consume the terminal-selected zero on that edge");
        @(negedge clk);
        filter_phase_ce = 1'b0;
        wait_engine_idle();

        // /VOICED drives CEP low at F. With no load, the next Phi1 must hold
        // both the count and the zero F1 source.
        pulse_filter(1'b0);
        held_f1 = dut.f1_state_q;
        held_f1_input_history = dut.f1_input_history_q;
        @(negedge clk);
        filter_phase = 1'b1;
        filter_phase_ce = 1'b1;
        #1;
        check(dut.voice_count_after_phi1 == 4'hF &&
              dut.u60_tc_after_phi1 &&
              dut.voice_source_after_phi1 == 24'sd0,
              "U60 CEP did not hold the terminal F state");
        @(posedge clk);
        #1;
        check(dut.voice_count_q == 4'hF && dut.f1_input_q == 24'sd0 &&
              dut.engine_main_delta_q ==
                  $signed({held_f1[23], held_f1}) &&
              dut.engine_side_delta_q ==
                  $signed({held_f1_input_history[23],
                            held_f1_input_history}),
              "held F did not keep the same-edge F1 source at zero");
        @(negedge clk);
        filter_phase_ce = 1'b0;
        wait_engine_idle();

        // Clear the U61 history, then present a new U62 rise. The resulting
        // F->3 parallel load must restore magnitude on that same Phi1 edge.
        set_voice_toggle(1'b0);
        pulse_filter(1'b0);
        pulse_filter(1'b0);
        set_voice_toggle(1'b1);
        pulse_filter(1'b0);
        check(dut.voice_load_pending_q,
              "second U62 rise did not arm the U60 parallel load");
        held_f1 = dut.f1_state_q;
        held_f1_input_history = dut.f1_input_history_q;
        @(negedge clk);
        filter_phase = 1'b1;
        filter_phase_ce = 1'b1;
        #1;
        check(dut.voice_count_after_phi1 == 4'h3 &&
              dut.voice_source_after_phi1 == dut.voice_magnitude,
              "F->3 load did not select post-clock magnitude");
        @(posedge clk);
        #1;
        check(dut.voice_count_q == 4'h3 &&
              dut.f1_input_q == dut.voice_magnitude &&
              dut.engine_main_delta_q ==
                  $signed({held_f1[23], held_f1}) -
                  $signed({dut.voice_magnitude[23], dut.voice_magnitude}) &&
              dut.engine_side_delta_q ==
                  $signed({held_f1_input_history[23],
                            held_f1_input_history}) -
                  $signed({dut.voice_magnitude[23], dut.voice_magnitude}),
              "F->3 load did not restore the same-edge F1 operand");
        @(negedge clk);
        filter_phase_ce = 1'b0;
        wait_engine_idle();

        // CTL/PD drives U61 clear only. Make every cleared U61 state high,
        // then prove the same PD edge neither clears nor loads U60.
        set_voice_toggle(1'b0);
        pulse_filter(1'b0);
        pulse_filter(1'b0);
        set_voice_toggle(1'b1);
        pulse_filter(1'b0);
        pulse_filter(1'b0);
        check(dut.pitch_sync1_q && dut.pitch_sync2_q &&
              dut.voice_load_pending_q &&
              dut.voice_count_q == 4'h3,
              "PD/RST test did not establish the drawn U61/U60 state");
        @(negedge clk);
        pd_rst_n = 1'b0;
        @(posedge clk);
        #1;
        check(!dut.pitch_sync1_q && !dut.pitch_sync2_q &&
              !dut.voice_load_pending_q && dut.voice_count_q == 4'h3,
              "PD/RST did not clear only U61 while holding U60");
        @(negedge clk);
        pd_rst_n = 1'b1;

        // Exact HCC4006/U75 recurrence against an independent reference.
        reset_all();
        fric_amp_code = 4'hF;
        ref_d1 = 1;
        ref_d2 = 0;
        ref_d3 = 0;
        ref_d4 = 0;
        ref_count = 15;
        step_noise_count_reference();
        pulse_noise_count();
        check(dut.noise_count_q == 4'h1 &&
              dut.noise_d1_q == 4'h1 &&
              dut.noise_d2_q == 5'h00 &&
              dut.noise_d3_q == 4'h0 &&
              dut.noise_d4_q == 5'h00,
              "U41C rise did not advance only U75 from F to 1");
        step_noise_shift_reference();
        pulse_noise_shift();
        check(dut.noise_count_q == ref_count[3:0] &&
              dut.noise_d1_q == ref_d1[3:0] &&
              dut.noise_d2_q == ref_d2[4:0] &&
              dut.noise_d3_q == ref_d3[3:0] &&
              dut.noise_d4_q == ref_d4[4:0],
              "U41C fall did not shift only U73 with the new U75 force");
        for (i = 1; i < 8192; i = i + 1) begin
            step_noise_reference();
            pulse_noise();
            check(dut.noise_count_q == ref_count[3:0],
                  "U75 reference count mismatch");
            check(dut.noise_count_q >= 4'h1,
                  "U75 entered the forbidden zero count");
            check(dut.noise_d1_q == ref_d1[3:0],
                  "HCC4006 D1 mismatch");
            check(dut.noise_d2_q == ref_d2[4:0],
                  "HCC4006 D2 mismatch");
            check(dut.noise_d3_q == ref_d3[3:0],
                  "HCC4006 D3 mismatch");
            check(dut.noise_d4_q == ref_d4[4:0],
                  "HCC4006 D4 mismatch");
        end
        step_noise_reference();
        pulse_noise();
        check(dut.noise_count_q == ref_count[3:0] &&
              dut.noise_d1_q == ref_d1[3:0] &&
              dut.noise_d2_q == ref_d2[4:0] &&
              dut.noise_d3_q == ref_d3[3:0] &&
              dut.noise_d4_q == ref_d4[4:0],
              "next U41C rise/fall pair left the exact recurrence");
        powered_down = 1'b1;
        step_noise_reference();
        pulse_noise();
        check(dut.noise_count_q == ref_count[3:0] &&
              dut.noise_d4_q == ref_d4[4:0],
              "power-down incorrectly froze digital noise state");
        powered_down = 1'b0;

        // With PW3 high and U62.Q low, exact U104C/U85C holds U62 reset and
        // suppresses U41C. A phone name does not override those gates.
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
        check(phase_fric_amp_code != 4'h0,
              "native S phone left the FRIC_AMP bank at zero");
        previous_value = phase_noise_edges;
        repeat (128)
            pulse_phase_xck(phase_seen);
        check(phase_core.u104c && phase_core.u62_reset &&
              !phase_core.u41c_level &&
              phase_noise_edges == previous_value,
              "PW3/U62 reset phase did not suppress U41C exactly");

        // Isolate the exact prerequisites for a free U62: U68 at its binary up
        // terminal, PW3 low, and a nonzero voice bank. With I=FFF, U62 rises
        // every eight effective XCK ticks and U60 loads on the next Phi1.
        force phase_core.ampct_q = 4'd15;
        force phase_core.pw_3_q = 1'b0;
        force phase_core.voice_amp_code_q = 4'hF;
        #1;
        check(!phase_core.u62_reset,
              "exact U68 terminal prerequisites did not release U62");
        phase_audio_enable = 1'b1;
        phase_voice_loads = 0;
        phase_voice_gap = 0;
        phase_ticks_since_load = 0;
        phase_u62_rises = 0;
        phase_u62_gap = 0;
        phase_ticks_since_u62 = 0;
        phase_prior_load_pending = phase_audio.voice_load_pending_q;
        phase_prior_voice_toggle = phase_voice_toggle;
        for (i = 0; i < 64; i = i + 1) begin
            pulse_phase_xck(phase_seen);
            phase_ticks_since_load = phase_ticks_since_load + 1;
            phase_ticks_since_u62 = phase_ticks_since_u62 + 1;
            if (!phase_prior_voice_toggle && phase_voice_toggle) begin
                phase_u62_rises = phase_u62_rises + 1;
                if (phase_u62_rises == 2)
                    phase_u62_gap = phase_ticks_since_u62;
                phase_ticks_since_u62 = 0;
            end
            if (phase_prior_load_pending &&
                !phase_audio.voice_load_pending_q &&
                phase_audio.voice_count_q == 4'h3) begin
                phase_voice_loads = phase_voice_loads + 1;
                // Ignore the two-stage U61 synchronizer fill event.  The next
                // full core-driven rising-to-rising interval must be exact.
                if (phase_voice_loads == 3)
                    phase_voice_gap = phase_ticks_since_load;
                phase_ticks_since_load = 0;
            end
            phase_prior_load_pending = phase_audio.voice_load_pending_q;
            phase_prior_voice_toggle = phase_voice_toggle;
        end
        check(phase_u62_rises >= 2 && phase_u62_gap == 8,
              "I=FFF U62 rising interval was not eight XCK ticks");
        check(phase_voice_loads >= 3,
              "U61/U34 did not load U60 on repeated Phi1 boundaries");
        check(phase_voice_gap == 8,
              "I=FFF U60 excitation interval was not eight XCK ticks");
        release phase_core.ampct_q;
        release phase_core.pw_3_q;
        release phase_core.voice_amp_code_q;

        // A high phase starts exactly 34 registered engine clocks.  A phase
        // inside that window must set the sticky diagnostic, not restart it.
        reset_all();
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
        f1_code = 4'h8;
        pulse_filter(1'b0);
        force dut.voice_source_after_phi1 = 24'sd65536;
        pulse_filter(1'b1);
        release dut.voice_source_after_phi1;
        check(dut.f1_state_q == 24'sd4076 &&
              dut.f1_history_q == -24'sd30248 &&
              dut.f1_input_history_q == 24'sd65536,
              "F1 first charge step changed its rounded p/y result");
        force dut.voice_source_after_phi1 = 24'sd0;
        pulse_filter(1'b1);
        release dut.voice_source_after_phi1;
        check(dut.f1_state_q == 24'sd5918 &&
              dut.f1_history_q == -24'sd13666 &&
              dut.f1_input_history_q == 0,
              "F1 prior-input step changed its rounded p/y result");

        reset_all();
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

        // C143 keeps the U157-side plate behind U159D. Closed, a steady source
        // s produces exactly 0-s after the Phi1 reset. Open, the plate floats
        // and produces no delta. Reconnect contributes held-s once.
        reset_all();
        force dut.f2_input_q = 24'sd0;
        force dut.fric1_source = 24'sd65536;
        fric1_sw = 1'b1;
        pulse_filter(1'b0);
        check(dut.c143_source_plate_q == 24'sd65536 &&
              dut.c143_phi0_delta_q == -25'sd65536,
              "closed C143 did not accumulate exact 0-s charge");
        repeat (2) @(posedge clk);
        #1;
        check(dut.c143_source_plate_q == 24'sd65536 &&
              dut.c143_phi0_delta_q == -25'sd65536,
              "closed steady C143 source added more than 0-s");
        pulse_filter(1'b1);
        check(dut.c143_delta_hold_q == -25'sd65536 &&
              dut.c143_source_plate_q == 24'sd0 &&
              dut.f2_state_q == -24'sd9636 &&
              dut.f2_history_q == 0,
              "closed Phi1 did not snapshot C143 and reset its source plate");

        // Open U159D before Phi1. The reset cannot reach the held source
        // plate, even though the completed Phi0 delta still reaches F2.
        pulse_filter(1'b0);
        fric1_sw = 1'b0;
        pulse_filter(1'b1);
        check(dut.c143_source_plate_q == 24'sd65536 &&
              dut.c143_delta_hold_q == -25'sd65536,
              "open C143 source plate did not survive Phi1");

        // While open, a new U157 level must neither move the plate nor add
        // charge at the next boundary.
        force dut.fric1_source = -24'sd65536;
        pulse_filter(1'b0);
        repeat (2) @(posedge clk);
        #1;
        check(dut.c143_source_plate_q == 24'sd65536 &&
              dut.c143_phi0_delta_q == 25'sd0,
              "open C143 did not hold its plate with zero delta");
        pulse_filter(1'b1);
        check(dut.c143_source_plate_q == 24'sd65536 &&
              dut.c143_delta_hold_q == 25'sd0,
              "open Phi1 reset or injected C143 charge");

        // Reconnect in Phi0 with held=+65536 and s=-65536. The one retained
        // source-plate difference is +131072; closed Phi1 then resets it.
        pulse_filter(1'b0);
        fric1_sw = 1'b1;
        @(posedge clk);
        #1;
        check(dut.c143_source_plate_q == -24'sd65536 &&
              dut.c143_phi0_delta_q == 25'sd131072,
              "C143 reconnect did not inject exact held-s charge");
        pulse_filter(1'b1);
        check(dut.c143_delta_hold_q == 25'sd131072 &&
              dut.c143_source_plate_q == 24'sd0,
              "closed Phi1 did not snapshot and reset reconnected C143");
        release dut.f2_input_q;
        release dut.fric1_source;

        // C150 is always connected to U152.  C151 has an independent history
        // which holds while U159C is open and rejoins only on reconnect.
        reset_all();
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
        f2_code = 4'h8;
        f2_res_code = 4'h0;
        pulse_filter(1'b0);
        deposit_f2_impulse();
        repeat (10) pulse_filter(1'b1);
        energy_low = pair_magnitude(dut.f2_state_q, dut.f2_history_q);
        reset_all();
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

        // Sheets 1 and 2 have no decoded-phone switch after U148. Only the
        // chip power state may mute the held output at the PCM boundary.
        reset_all();
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
              "known U148 level did not reach the PCM boundary");
        powered_down = 1'b1;
        pulse_audio_tick();
        check(audio_sample == 0,
              "powerdown did not mute the PCM boundary");
        reset_all();

        // Exhaust every FILT divider code through the native core boundary.
        reset_all();
        write_phase_register(3'd1, 8'hFF);
        write_phase_register(3'd2, 8'h0F);
        write_phase_register(3'd0, 8'h80);
        write_phase_register(3'd3, 8'h00);
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
        // Hold the same exact U68 terminal prerequisites used above, then
        // stop on a fresh U62 rise so Phi sampling sees one real voice pulse.
        force phase_core.ampct_q = 4'd15;
        force phase_core.pw_3_q = 1'b0;
        force phase_core.voice_amp_code_q = 4'hF;
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
        release phase_core.ampct_q;
        release phase_core.pw_3_q;
        release phase_core.voice_amp_code_q;

        if (failures == 0)
            $display("SSI263 SC02 AUDIO PASS");
        else
            $display("SSI263 SC02 AUDIO FAIL: %0d checks", failures);
        $finish;
    end

endmodule
