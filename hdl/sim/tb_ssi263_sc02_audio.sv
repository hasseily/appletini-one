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
    logic fric1_sw = 1'b0;
    logic fric2_sw = 1'b0;
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

    integer failures = 0;
    integer expected_value;
    integer expected_first;
    integer expected_second;
    integer held_value;
    integer i;
    integer checkpoint = 0;
    integer divider_cases = 0;
    integer divider_boundary_cases = 0;
    integer divider_random_cases = 0;
    integer reciprocal_constant_cases = 0;
    integer reciprocal_bound_cases = 0;
    integer correction_input_cases = 0;
    integer numerator_alignment_cases = 0;
    logic [11:0] numerator_stages_seen = 12'h000;
    integer divider_denominator;
    integer random_index;
    longint signed divider_random_state;
    logic signed [47:0] divider_test_numerator;
    logic [13:0] divider_test_denominator;

    always #5 clk = ~clk;

    // With all four U119 capacitors selected, a POT3 step of 65141 produces
    // exactly +65536 at U116: round(3320*65141/3300). The exact unit vector
    // makes the mid-Phi1 C128/C127 causal result independent of a trim guess.
    ssi263_sc02_audio #(
        .VOICE_TRIM_U116_STEP_Q16(18'sd65141)
    ) dut (
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

    function automatic integer round_divide_ref(
        input integer numerator,
        input integer denominator
    );
        begin
            if (numerator < 0)
                round_divide_ref =
                    (numerator - (denominator / 2)) / denominator;
            else
                round_divide_ref =
                    (numerator + (denominator / 2)) / denominator;
        end
    endfunction

    function automatic logic signed [23:0] divider_ref48(
        input logic signed [47:0] numerator,
        input integer denominator
    );
        logic signed [63:0] rounded;
        logic signed [63:0] quotient;
        begin
            rounded = numerator;
            if (rounded < 0)
                rounded = rounded - (denominator / 2);
            else
                rounded = rounded + (denominator / 2);
            quotient = rounded / denominator;
            if (quotient > 64'sd8388607)
                divider_ref48 = 24'sh7FFFFF;
            else if (quotient < -64'sd8388608)
                divider_ref48 = 24'sh800000;
            else
                divider_ref48 = quotient[23:0];
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

    task automatic wait_engine_idle;
        integer guard;
        begin
            guard = 0;
            while (dut.engine_busy_q || dut.fifo_count_q != 0 ||
                   dut.numerator_input_valid_q || dut.products_valid_q ||
                   dut.numerator_valid_q ||
                   dut.reciprocal_input_valid_q ||
                   dut.reciprocal_product_valid_q ||
                   dut.correction_input_valid_q || dut.correction_valid_q ||
                   dut.divider_result_valid_q) begin
                @(negedge clk);
                guard = guard + 1;
                if (dut.numerator_input_valid_q) begin
                    if (dut.numerator_operand0_q !== dut.numerator_operand0 ||
                        dut.numerator_operand1_q !== dut.numerator_operand1 ||
                        dut.numerator_operand2_q !== dut.numerator_operand2 ||
                        dut.numerator_operand3_q !== dut.numerator_operand3 ||
                        dut.numerator_operand4_q !== dut.numerator_operand4 ||
                        dut.numerator_operand5_q !== dut.numerator_operand5 ||
                        dut.numerator_coefficient0_q !==
                            dut.numerator_coefficient0 ||
                        dut.numerator_coefficient1_q !==
                            dut.numerator_coefficient1 ||
                        dut.numerator_coefficient2_q !==
                            dut.numerator_coefficient2 ||
                        dut.numerator_coefficient3_q !==
                            dut.numerator_coefficient3 ||
                        dut.numerator_coefficient4_q !==
                            dut.numerator_coefficient4 ||
                        dut.numerator_coefficient5_q !==
                            dut.numerator_coefficient5 ||
                        dut.numerator_input_denominator_q !==
                            dut.numerator_denominator) begin
                        $display("SSI263 SC02 AUDIO FAIL: pre-numerator register misaligned checkpoint=%0d stage=%0d",
                                 checkpoint, dut.engine_stage_q);
                        failures = failures + 1;
                    end
                    numerator_alignment_cases = numerator_alignment_cases + 1;
                    if (dut.engine_stage_q < 12)
                        numerator_stages_seen[dut.engine_stage_q] = 1'b1;
                end
                if (guard == 5000) begin
                    $display("SSI263 SC02 AUDIO FAIL: wait timeout checkpoint=%0d busy=%b fifo=%0d stage=%0d pipe=%b%b%b%b%b%b%b%b",
                             checkpoint, dut.engine_busy_q, dut.fifo_count_q,
                             dut.engine_stage_q,
                             dut.numerator_input_valid_q,
                             dut.products_valid_q,
                             dut.numerator_valid_q,
                             dut.reciprocal_input_valid_q,
                             dut.reciprocal_product_valid_q,
                             dut.correction_input_valid_q,
                             dut.correction_valid_q,
                             dut.divider_result_valid_q);
                    $finish;
                end
            end
        end
    endtask

    task automatic check_reciprocal_constant(input integer denominator);
        longint signed power_of_two;
        longint signed expected_multiplier;
        longint signed remainder;
        longint signed maximum_input;
        begin
            wait_engine_idle();
            @(negedge clk);
            divider_test_denominator = denominator[13:0];
            force dut.engine_div_denominator_q = divider_test_denominator;
            #1;

            power_of_two = 64'sd1 << 37;
            expected_multiplier = power_of_two / denominator;
            remainder = power_of_two - denominator * expected_multiplier;
            maximum_input = denominator * 64'sd8388608 - 1;
            if (!dut.reciprocal_denominator_valid ||
                $unsigned(dut.reciprocal_multiplier) != expected_multiplier) begin
                $display("SSI263 SC02 AUDIO FAIL: reciprocal constant d=%0d got=%0d expected=%0d",
                         denominator, $unsigned(dut.reciprocal_multiplier),
                         expected_multiplier);
                failures = failures + 1;
            end
            if (remainder < 0 || remainder >= denominator ||
                maximum_input * remainder >= denominator * power_of_two) begin
                $display("SSI263 SC02 AUDIO FAIL: reciprocal proof bound d=%0d M=%0d r=%0d",
                         denominator, expected_multiplier, remainder);
                failures = failures + 1;
            end
            reciprocal_constant_cases = reciprocal_constant_cases + 1;

            release dut.engine_div_denominator_q;
            @(negedge clk);
        end
    endtask

    // Drive the production reciprocal-product, q0, correction-product, and
    // signed-result registers. Stage 12 has no charge-node write, so each
    // vector tests only the divider and cannot alter the charge fixtures.
    task automatic check_divider48(
        input logic signed [47:0] numerator,
        input integer denominator,
        input string label_text
    );
        logic signed [23:0] expected;
        logic proof_seen;
        logic correction_input_seen;
        logic correction_seen;
        integer guard;
        longint signed rounded_value;
        longint signed absolute_value;
        longint signed quotient_value;
        longint signed q0_value;
        longint signed expected_multiplier;
        begin
            wait_engine_idle();
            @(negedge clk);
            divider_test_numerator = numerator;
            divider_test_denominator = denominator[13:0];
            force dut.engine_div_numerator_q = divider_test_numerator;
            force dut.engine_div_denominator_q = divider_test_denominator;
            force dut.numerator_valid_q = 1'b1;
            force dut.engine_stage_q = 4'd12;
            force dut.engine_busy_q = 1'b1;
            #1;

            rounded_value = numerator;
            if (rounded_value < 0)
                rounded_value = rounded_value - (denominator / 2);
            else
                rounded_value = rounded_value + (denominator / 2);
            absolute_value = (rounded_value < 0) ?
                -rounded_value : rounded_value;
            quotient_value = absolute_value / denominator;
            expected_multiplier = (64'sd1 << 37) / denominator;
            proof_seen = 1'b0;
            correction_input_seen = 1'b0;
            correction_seen = 1'b0;
            guard = 0;
            while (!dut.engine_step_valid && guard < 20) begin
                @(negedge clk);
                guard = guard + 1;
                if (dut.reciprocal_product_valid_q &&
                    absolute_value < denominator * 64'sd8388608) begin
                    q0_value = $unsigned(dut.reciprocal_q0);
                    if (!dut.reciprocal_denominator_valid_q ||
                        dut.reciprocal_denominator_q != denominator ||
                        $unsigned(dut.reciprocal_product_q) !=
                            absolute_value * expected_multiplier ||
                        !((q0_value == quotient_value) ||
                          (q0_value + 1 == quotient_value))) begin
                        $display("SSI263 SC02 AUDIO FAIL: reciprocal q/q-1 bound %s n=%0d d=%0d q0=%0d q=%0d",
                                 label_text, numerator, denominator,
                                 q0_value, quotient_value);
                        failures = failures + 1;
                    end
                    proof_seen = 1'b1;
                    reciprocal_bound_cases = reciprocal_bound_cases + 1;
                end
                if (dut.correction_input_valid_q) begin
                    q0_value = $unsigned(dut.correction_input_q0_q);
                    if ($unsigned(dut.correction_operand_q) != q0_value + 1 ||
                        dut.correction_input_denominator_q != denominator ||
                        $unsigned(dut.correction_input_absolute_q) !=
                            absolute_value ||
                        dut.correction_input_negative_q !=
                            (rounded_value < 0) ||
                        dut.correction_input_zero_q != (numerator == 0) ||
                        dut.correction_input_overflow_q !=
                            (absolute_value >= denominator * 64'sd8388608) ||
                        !dut.correction_input_denominator_valid_q) begin
                        $display("SSI263 SC02 AUDIO FAIL: pre-correction register %s n=%0d d=%0d q0=%0d",
                                 label_text, numerator, denominator, q0_value);
                        failures = failures + 1;
                    end
                    correction_input_seen = 1'b1;
                    correction_input_cases = correction_input_cases + 1;
                end
                if (dut.correction_valid_q &&
                    absolute_value < denominator * 64'sd8388608) begin
                    q0_value = $unsigned(dut.correction_q0_q);
                    if ($unsigned(dut.correction_product_q) !=
                            (q0_value + 1) * denominator ||
                        ((q0_value + 1 == quotient_value) !=
                         (dut.correction_product_q <= absolute_value))) begin
                        $display("SSI263 SC02 AUDIO FAIL: reciprocal correction %s n=%0d d=%0d q0=%0d q=%0d",
                                 label_text, numerator, denominator,
                                 q0_value, quotient_value);
                        failures = failures + 1;
                    end
                    correction_seen = 1'b1;
                end
            end
            if (!dut.engine_step_valid) begin
                $display("SSI263 SC02 AUDIO FAIL: divider timeout %s n=%0d d=%0d",
                         label_text, numerator, denominator);
                $finish;
            end
            if (absolute_value < denominator * 64'sd8388608 &&
                (!proof_seen || !correction_seen)) begin
                $display("SSI263 SC02 AUDIO FAIL: reciprocal proof stages not observed %s n=%0d d=%0d",
                         label_text, numerator, denominator);
                failures = failures + 1;
            end
            if (!correction_input_seen) begin
                $display("SSI263 SC02 AUDIO FAIL: pre-correction stage not observed %s n=%0d d=%0d",
                         label_text, numerator, denominator);
                failures = failures + 1;
            end

            expected = divider_ref48(numerator, denominator);
            if ($signed(dut.engine_step_result) !== expected) begin
                $display("SSI263 SC02 AUDIO FAIL: divider %s n=%0d d=%0d got=%0d expected=%0d",
                         label_text, numerator, denominator,
                         $signed(dut.engine_step_result), expected);
                failures = failures + 1;
            end
            divider_cases = divider_cases + 1;

            // Keep the forced engine active for the result edge so the real
            // divider clears its busy state before the next vector.
            @(posedge clk);
            #1;
            release dut.numerator_valid_q;
            release dut.engine_busy_q;
            release dut.engine_stage_q;
            release dut.engine_div_denominator_q;
            release dut.engine_div_numerator_q;
            @(negedge clk);
        end
    endtask

    task automatic run_divider_denominator(input integer denominator);
        longint signed half_value;
        longint signed edge_value;
        longint signed random_value;
        longint signed random_quotient;
        longint signed random_remainder;
        integer boundary_start;
        integer random_start;
        begin
            check_reciprocal_constant(denominator);
            boundary_start = divider_cases;
            half_value = denominator / 2;
            check_divider48(48'sd0, denominator, "zero");
            check_divider48(half_value - 1, denominator, "positive below half");
            check_divider48(half_value, denominator, "positive half");
            check_divider48(half_value + 1, denominator, "positive above half");
            check_divider48(-(half_value - 1), denominator, "negative below half");
            check_divider48(-half_value, denominator, "negative half");
            check_divider48(-(half_value + 1), denominator, "negative above half");
            check_divider48(3 * denominator + half_value - 1,
                            denominator, "positive quotient and remainder");
            check_divider48(-(3 * denominator + half_value - 1),
                            denominator, "negative quotient and remainder");

            divider_random_state =
                (divider_random_state * 64'sd48271) % 64'sd2147483647;
            random_value = divider_random_state;
            check_divider48(random_value, denominator, "seeded positive random");
            divider_random_state =
                (divider_random_state * 64'sd48271) % 64'sd2147483647;
            random_value = -divider_random_state;
            check_divider48(random_value, denominator, "seeded negative random");

            edge_value = 64'sd8388607 * denominator;
            check_divider48(edge_value, denominator, "positive maximum");
            check_divider48(edge_value + half_value - 1,
                            denominator, "positive saturation below edge");
            check_divider48(edge_value + half_value,
                            denominator, "positive saturation edge");
            check_divider48(-(64'sd8388608 * denominator),
                            denominator, "negative minimum");
            check_divider48(-(64'sd8388609 * denominator),
                            denominator, "negative saturation");
            divider_boundary_cases = divider_boundary_cases +
                divider_cases - boundary_start;

            random_start = divider_cases;
            for (random_index = 0; random_index < 128;
                 random_index = random_index + 1) begin
                divider_random_state =
                    (divider_random_state * 64'sd48271) % 64'sd2147483647;
                random_quotient = divider_random_state % 64'sd8000000;
                divider_random_state =
                    (divider_random_state * 64'sd48271) % 64'sd2147483647;
                random_remainder = divider_random_state % denominator;
                random_value = random_quotient * denominator +
                    random_remainder - half_value;
                if (random_value == 0)
                    random_value = 1;
                if (random_index[0])
                    random_value = -random_value;
                check_divider48(random_value, denominator,
                                "deterministic randomized in-range");
            end
            divider_random_cases = divider_random_cases +
                divider_cases - random_start;
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
            noise_clock_ce = 1'b0;
            noise_shift_ce = 1'b0;
            fric1_sw = 1'b0;
            fric2_sw = 1'b0;
            voice_toggle = 1'b0;
            filter_phase_ce = 1'b0;
            filter_phase = 1'b0;
            filter_frequency = 8'hE9;
            f1_code = 4'h0;
            f2_code = 4'h0;
            f2_res_code = 4'h0;
            f3_code = 4'h0;
            f4_code = 4'h0;
            filter_amp_code = 4'h0;
            voice_amp_code = 4'h0;
            fric_amp_code = 4'h0;
            closure = 1'b0;
            repeat (4) @(negedge clk);
            rstn = 1'b1;
            repeat (2) @(negedge clk);
        end
    endtask

    task automatic phase_event(input logic new_phase);
        begin
            wait_engine_idle();
            @(negedge clk);
            filter_phase = new_phase;
            filter_phase_ce = 1'b1;
            closure = !new_phase;
            @(posedge clk);
            #1;
            @(negedge clk);
            filter_phase_ce = 1'b0;
            closure = 1'b0;
            wait_engine_idle();
        end
    endtask

    task automatic phase_pair;
        begin
            phase_event(1'b0);
            phase_event(1'b1);
        end
    endtask

    task automatic source_clock;
        begin
            @(posedge clk);
            #1;
            wait_engine_idle();
        end
    endtask

    task automatic pulse_noise;
        begin
            @(negedge clk);
            noise_clock_ce = 1'b1;
            @(negedge clk);
            noise_clock_ce = 1'b0;
            noise_shift_ce = 1'b1;
            @(negedge clk);
            noise_shift_ce = 1'b0;
        end
    endtask

    initial begin
        reset_all();

        // Check every selected bank against the capacitor values drawn on
        // sheets 1 and 2. cap_sum4 exposes pF, not an acoustic coefficient.
        check(dut.cap_sum4(4'h1,160,330,660,1300) == 14'd160 &&
              dut.cap_sum4(4'h2,160,330,660,1300) == 14'd330 &&
              dut.cap_sum4(4'h4,160,330,660,1300) == 14'd660 &&
              dut.cap_sum4(4'h8,160,330,660,1300) == 14'd1300 &&
              dut.cap_sum4(4'hF,160,330,660,1300) == 14'd2450,
              "F1 capacitor bank does not match C120/C119/C118/C117");
        check(dut.cap_sum4(4'h1,280,560,1120,2300) == 14'd280 &&
              dut.cap_sum4(4'h2,280,560,1120,2300) == 14'd560 &&
              dut.cap_sum4(4'h4,280,560,1120,2300) == 14'd1120 &&
              dut.cap_sum4(4'h8,280,560,1120,2300) == 14'd2300 &&
              dut.cap_sum4(4'hF,280,560,1120,2300) == 14'd4260,
              "F2 frequency capacitor bank is wrong");
        check(dut.cap_sum4(4'h1,220,430,870,1800) == 14'd220 &&
              dut.cap_sum4(4'h2,220,430,870,1800) == 14'd430 &&
              dut.cap_sum4(4'h4,220,430,870,1800) == 14'd870 &&
              dut.cap_sum4(4'h8,220,430,870,1800) == 14'd1800 &&
              dut.cap_sum4(4'hF,220,430,870,1800) == 14'd3320,
              "F2 resonance capacitor bank is wrong");
        check(dut.cap_sum4(4'h1,210,420,820,1640) == 14'd210 &&
              dut.cap_sum4(4'h2,210,420,820,1640) == 14'd420 &&
              dut.cap_sum4(4'h4,210,420,820,1640) == 14'd820 &&
              dut.cap_sum4(4'h8,210,420,820,1640) == 14'd1640 &&
              dut.cap_sum4(4'hF,210,420,820,1640) == 14'd3090,
              "F3 capacitor bank is wrong");
        check(dut.cap_sum4(4'h1,200,400,820,1620) == 14'd200 &&
              dut.cap_sum4(4'h2,200,400,820,1620) == 14'd400 &&
              dut.cap_sum4(4'h4,200,400,820,1620) == 14'd820 &&
              dut.cap_sum4(4'h8,200,400,820,1620) == 14'd1620 &&
              dut.cap_sum4(4'hF,200,400,820,1620) == 14'd3040,
              "F4 capacitor bank is wrong");
        check(dut.cap_sum4(4'h1,220,430,870,1800) == 14'd220 &&
              dut.cap_sum4(4'h2,220,430,870,1800) == 14'd430 &&
              dut.cap_sum4(4'h4,220,430,870,1800) == 14'd870 &&
              dut.cap_sum4(4'h8,220,430,870,1800) == 14'd1800 &&
              dut.cap_sum4(4'hF,220,430,870,1800) == 14'd3320,
              "U116 source capacitor bank is wrong");
        check(dut.cap_sum4(4'h1,270,512,1068,2160) == 14'd270 &&
              dut.cap_sum4(4'h2,270,512,1068,2160) == 14'd512 &&
              dut.cap_sum4(4'h4,270,512,1068,2160) == 14'd1068 &&
              dut.cap_sum4(4'h8,270,512,1068,2160) == 14'd2160 &&
              dut.cap_sum4(4'hF,270,512,1068,2160) == 14'd4010,
              "U157 source capacitor bank is wrong");
        check(dut.cap_sum4(4'h1,270,530,1082,2160) == 14'd270 &&
              dut.cap_sum4(4'h2,270,530,1082,2160) == 14'd530 &&
              dut.cap_sum4(4'h4,270,530,1082,2160) == 14'd1082 &&
              dut.cap_sum4(4'h8,270,530,1082,2160) == 14'd2160 &&
              dut.cap_sum4(4'hF,270,530,1082,2160) == 14'd4042,
              "U152 source capacitor bank is wrong");
        check(dut.cap_sum4(4'h1,76,150,300,600) == 14'd76 &&
              dut.cap_sum4(4'h2,76,150,300,600) == 14'd150 &&
              dut.cap_sum4(4'h4,76,150,300,600) == 14'd300 &&
              dut.cap_sum4(4'h8,76,150,300,600) == 14'd600 &&
              dut.cap_sum4(4'hF,76,150,300,600) == 14'd1126,
              "U146 amplitude capacitor bank is wrong");

        check(dut.sat24_from48(48'sd8388608) == 24'sh7FFFFF &&
              dut.sat24_from48(-48'sd8388609) == -24'sd8388608,
              "24-bit charge saturation wrapped");

        // The finite-denominator reciprocal divider must match exact signed
        // round-to-nearest arithmetic at every live capacitance total.
        checkpoint = 0;
        reset_all();
        divider_random_state = 64'sd104729;
        run_divider_denominator(2750);
        run_divider_denominator(3300);
        run_divider_denominator(3450);
        run_divider_denominator(3730);
        run_divider_denominator(3900);
        run_divider_denominator(4300);
        run_divider_denominator(4500);
        run_divider_denominator(4700);
        run_divider_denominator(4900);
        run_divider_denominator(6800);
        run_divider_denominator(11500);
        run_divider_denominator(11700);
        for (i = 0; i < 16; i = i + 1) begin
            divider_denominator = 7000 +
                dut.cap_sum4(i, 220, 430, 870, 1800);
            run_divider_denominator(divider_denominator);
        end
        check(divider_boundary_cases == 448,
              "reciprocal divider lost one of 448 boundary vectors");
        check(divider_random_cases == 3584 && divider_cases == 4032,
              "reciprocal divider did not cover 128 random values per denominator");
        check(reciprocal_constant_cases == 28,
              "reciprocal constants did not cover every live denominator");
        check(reciprocal_bound_cases == 3948,
              "reciprocal q/q-1 proof did not cover every in-range vector");
        check(correction_input_cases == 4032,
              "pre-correction register was not observed for every vector");

        // U60 and U118A act on the same Phi1 edge. A 14->15 count must pick
        // AGND for that U116 transfer, not retain one extra voiced event.
        checkpoint = 1;
        reset_all();
        voice_amp_code = 4'hF;
        phase_event(1'b0);
        @(negedge clk);
        force dut.voice_count_q = 4'hE;
        #1;
        release dut.voice_count_q;
        filter_phase = 1'b1;
        filter_phase_ce = 1'b1;
        #1;
        check(dut.voice_count_after_phi1 == 4'hF &&
              dut.voice_target_now == 0,
              "U60 14->15 did not select same-edge AGND");
        @(posedge clk);
        #1;
        @(negedge clk);
        filter_phase_ce = 1'b0;
        wait_engine_idle();
        check(dut.voice_count_q == 4'hF && dut.voice_source_state_q == 0,
              "U116 transferred a stale voiced level on U60 14->15");

        // The inverse race is a parallel load from held F to B. U118A must
        // select the physical source on that same Phi1 edge.
        phase_event(1'b0);
        @(negedge clk);
        force dut.voice_count_q = 4'hF;
        force dut.voice_load_pending_q = 1'b1;
        #1;
        release dut.voice_count_q;
        release dut.voice_load_pending_q;
        filter_phase = 1'b1;
        filter_phase_ce = 1'b1;
        #1;
        expected_value = -round_divide_ref(3320 * -65141, 3300);
        check(dut.voice_count_after_phi1 == 4'hB &&
              dut.voice_target_now == -24'sd65141,
              "U60 F->B did not select the same-edge source charge");
        @(posedge clk);
        #1;
        @(negedge clk);
        filter_phase_ce = 1'b0;
        wait_engine_idle();
        check(dut.voice_count_q == 4'hB &&
              dut.voice_source_state_q == expected_value,
              "U116 missed the same-edge U60 F->B source transfer");

        // U116: each selected U119 capacitor keeps its own source-side plate.
        // C205=3300 pF is the feedback capacitor. This test's exact virtual
        // POT3 fixture drives U201 to -65141.
        checkpoint = 2;
        reset_all();
        force dut.voice_count_q = 4'h3;
        voice_amp_code = 4'h1;
        phase_event(1'b0);
        phase_event(1'b1);
        expected_value = -round_divide_ref(220 * -65141, 3300);
        check(dut.voice_source_state_q == expected_value &&
              dut.voice_plate0_q == -24'sd65141 &&
              dut.voice_plate1_q == 0 &&
              dut.voice_plate2_q == 0 &&
              dut.voice_plate3_q == 0,
              "U116 bit 0 did not transfer C201 charge independently");

        // Open C201 in Phi0. Its plate must retain -65141. Reconnect it with
        // the drive at AGND and check the equal and opposite retained charge.
        voice_amp_code = 4'h0;
        phase_event(1'b0);
        check(dut.voice_source_state_q == 0 &&
              dut.voice_plate0_q == -24'sd65141,
              "open U116 plate lost charge during Phi0 reset");
        powered_down = 1'b1;
        phase_event(1'b1);
        @(negedge clk);
        voice_amp_code = 4'h1;
        source_clock();
        expected_value = -round_divide_ref(220 * 65141, 3300);
        check(dut.voice_source_state_q == expected_value &&
              dut.voice_plate0_q == 0,
              "U116 reconnect did not use the retained C201 plate");
        release dut.voice_count_q;

        // A simultaneous 7->8 code transition must operate four switches.
        // It must not use one aggregate code-step table. Bits 0..2 retain
        // their target plates while bit 3 contributes its own new charge.
        checkpoint = 3;
        reset_all();
        force dut.voice_count_q = 4'h3;
        voice_amp_code = 4'h7;
        phase_event(1'b0);
        phase_event(1'b1);
        expected_first = -round_divide_ref(1520 * -65141, 3300);
        check(dut.voice_source_state_q == expected_first,
              "U116 code 7 did not sum three physical capacitors");
        @(negedge clk);
        voice_amp_code = 4'h8;
        source_clock();
        expected_second = expected_first -
            round_divide_ref(1800 * -65141, 3300);
        check(dut.voice_source_state_q == expected_second &&
              dut.voice_plate0_q == -24'sd65141 &&
              dut.voice_plate1_q == -24'sd65141 &&
              dut.voice_plate2_q == -24'sd65141 &&
              dut.voice_plate3_q == -24'sd65141,
              "simultaneous U116 7->8 transition lost retained plate charge");
        held_value = dut.voice_source_state_q;
        @(negedge clk);
        voice_amp_code = 4'h7;
        source_clock();
        check(dut.voice_source_state_q == held_value,
              "disconnecting U116 bit 3 invented a charge transfer");
        release dut.voice_count_q;

        // U157 has the opposite phase: Phi1 resets selected U158 plates and
        // Phi0 transfers the bipolar divider drive into C133=3900 pF.
        checkpoint = 4;
        reset_all();
        force dut.noise_d3_q = 4'h0;
        fric_amp_code = 4'h1;
        phase_event(1'b1);
        phase_event(1'b0);
        expected_value = -round_divide_ref(270 * 301, 3900);
        check(dut.fric1_source_state_q == expected_value &&
              dut.fric1_plate0_q == 24'sd301,
              "U157 bit 0 did not transfer its physical charge");

        // Open C132 for the reset, invert the divider, then reconnect. The
        // full +301 to -301 plate step must appear once and only once.
        fric_amp_code = 4'h0;
        phase_event(1'b1);
        check(dut.fric1_source_state_q == 0 &&
              dut.fric1_plate0_q == 24'sd301,
              "open U157 plate lost charge during Phi1 reset");
        force dut.noise_d3_q = 4'h8;
        phase_event(1'b0);
        @(negedge clk);
        fric_amp_code = 4'h1;
        source_clock();
        expected_value = -round_divide_ref(270 * -602, 3900);
        check(dut.fric1_source_state_q == expected_value &&
              dut.fric1_plate0_q == -24'sd301,
              "U157 reconnect did not transfer its retained plate step");
        release dut.noise_d3_q;

        // The FL_AMP bank also keeps one plate per switch. Phi0 precharges a
        // selected plate to raw F5 y. An open plate must keep that voltage
        // through later y changes and both phases, then reconnect only once.
        checkpoint = 5;
        reset_all();
        force dut.f5_state_q = 24'sd4096;
        filter_amp_code = 4'h1;
        source_clock();
        check(dut.filter_plate0_q == 24'sd4096 &&
              dut.filter_plate1_q == 0 && dut.filter_plate2_q == 0 &&
              dut.filter_plate3_q == 0,
              "FL_AMP bit 0 did not precharge to F5 during Phi0");
        @(negedge clk);
        filter_amp_code = 4'h0;
        source_clock();
        check(dut.filter_plate0_q == 24'sd4096,
              "open FL_AMP plate lost its precharged voltage");
        release dut.f5_state_q;
        force dut.f5_state_q = -24'sd2048;
        source_clock();
        check(dut.filter_plate0_q == 24'sd4096,
              "open FL_AMP plate followed a Phi0 F5 change");
        phase_event(1'b1);
        check(dut.filter_plate0_q == 24'sd4096,
              "open FL_AMP plate changed at the Phi1 boundary");
        @(negedge clk);
        filter_amp_code = 4'h1;
        source_clock();
        check(dut.filter_plate0_q == -24'sd2048,
              "active-Phi1 FL_AMP reconnect did not capture F5");
        @(negedge clk);
        filter_amp_code = 4'h0;
        source_clock();
        release dut.f5_state_q;
        force dut.f5_state_q = 24'sd8192;
        source_clock();
        check(dut.filter_plate0_q == -24'sd2048,
              "open FL_AMP plate followed a later Phi1 F5 change");
        phase_event(1'b0);
        check(dut.filter_plate0_q == -24'sd2048,
              "open FL_AMP plate changed at the Phi0 boundary");
        @(negedge clk);
        filter_amp_code = 4'h1;
        source_clock();
        check(dut.filter_plate0_q == 24'sd8192,
              "reselected FL_AMP plate did not precharge in Phi0");
        release dut.f5_state_q;

        // U154/C139-C142 feedback shaper, independent of U152 input drive.
        // Phi0: s += 12/13*z, then z -= 19/13*delta_s.
        checkpoint = 6;
        reset_all();
        force dut.fric2_source_state_q = 24'sd0;
        force dut.fric2_shape_state_q = 24'sd1300;
        #1;
        release dut.fric2_source_state_q;
        release dut.fric2_shape_state_q;
        phase_event(1'b0);
        check(dut.fric2_source_state_q == 24'sd1200 &&
              dut.fric2_shape_state_q == -24'sd454,
              "U154 Phi0 12/13 and 19/13 recurrence is wrong");

        // Phi1 entry leaves s fixed and applies z -= 12/13*s.
        reset_all();
        force dut.fric2_source_state_q = 24'sd1300;
        force dut.fric2_shape_state_q = 24'sd0;
        #1;
        release dut.fric2_source_state_q;
        release dut.fric2_shape_state_q;
        phase_event(1'b1);
        check(dut.fric2_source_state_q == 24'sd1300 &&
              dut.fric2_shape_state_q == -24'sd1200,
              "U154 Phi1 12/13 transfer is wrong");

        // One full-bank HCC edge gives e=-Csel/3900*delta_d. C140 makes the
        // U154 response -19/13*e in Phi0 and C140+C141 make it -31/13*e in
        // Phi1. These checks use the pF values, not expected sound samples.
        checkpoint = 7;
        reset_all();
        force dut.noise_d3_q = 4'h8;
        source_clock();
        @(negedge clk);
        fric_amp_code = 4'hF;
        source_clock();
        expected_first = -round_divide_ref(4042 * 602, 3900);
        force dut.noise_d3_q = 4'h0;
        source_clock();
        expected_second = round_divide_ref(-5700 * expected_first, 3900);
        check(dut.fric2_source_state_q == expected_first &&
              dut.fric2_shape_state_q == expected_second,
              "U152/U154 Phi0 HCC edge recurrence is wrong");
        release dut.noise_d3_q;

        reset_all();
        force dut.noise_d3_q = 4'h8;
        source_clock();
        @(negedge clk);
        fric_amp_code = 4'hF;
        source_clock();
        phase_event(1'b1);
        force dut.noise_d3_q = 4'h0;
        source_clock();
        expected_second = round_divide_ref(-9300 * expected_first, 3900);
        check(dut.fric2_source_state_q == expected_first &&
              dut.fric2_shape_state_q == expected_second,
              "U152/U154 Phi1 HCC edge recurrence is wrong");
        release dut.noise_d3_q;

        // C150 and C151 see only changes of U152 while Phi1 is active. A
        // steady absolute U152 level must not be transferred once per phase.
        checkpoint = 8;
        reset_all();
        force dut.noise_d3_q = 4'h8;
        source_clock();
        @(negedge clk);
        fric_amp_code = 4'hF;
        fric2_sw = 1'b1;
        source_clock(); // U159C closes and precharges on the grounded bus.
        phase_event(1'b1);
        force dut.noise_d3_q = 4'h0;
        source_clock();
        expected_value = -round_divide_ref(4042 * 602, 3900);
        check(dut.c150_phi1_delta_q == expected_value &&
              dut.c151_phi1_delta_q == expected_value &&
              dut.c151_source_plate_q == dut.fric2_source_state_q,
              "C150/C151 did not capture the live Phi1 U152 edge");
        held_value = dut.c150_phi1_delta_q;
        source_clock();
        check(dut.c150_phi1_delta_q == held_value &&
              dut.c151_phi1_delta_q == held_value,
              "steady U152 level caused an absolute C150/C151 transfer");
        phase_event(1'b0);
        check(dut.c150_phi1_delta_hold_q == 0 &&
              dut.c151_phi1_delta_hold_q == 0 &&
              dut.c150_phi1_delta_q == 0 &&
              dut.c151_phi1_delta_q == 0,
              "Phi0 created a C150/C151 transfer");
        force dut.noise_d3_q = 4'h8;
        source_clock();
        check(dut.c150_phi1_delta_q == 0 &&
              dut.c151_phi1_delta_q == 0,
              "a Phi0 U152 edge leaked into C150/C151");
        release dut.noise_d3_q;

        // With U159C open, the always-connected C150 path remains and C151
        // contributes no live delta.
        reset_all();
        force dut.noise_d3_q = 4'h8;
        source_clock();
        @(negedge clk);
        fric_amp_code = 4'hF;
        fric2_sw = 1'b0;
        source_clock();
        phase_event(1'b1);
        force dut.noise_d3_q = 4'h0;
        source_clock();
        expected_value = -round_divide_ref(4042 * 602, 3900);
        check(dut.c150_phi1_delta_q == expected_value &&
              dut.c151_phi1_delta_q == 0 &&
              dut.c151_source_plate_q == 0,
              "open U159C did not isolate C151 from a Phi1 U152 edge");
        release dut.noise_d3_q;

        // C128 is discharged in Phi0. The same steady U116 output must enter
        // F1 on every Phi1 as an absolute -2700*x term, never as x[n-1]-x.
        checkpoint = 9;
        reset_all();
        voice_amp_code = 4'hF;
        force dut.voice_count_q = 4'h3;
        phase_event(1'b0);
        phase_event(1'b1);
        check(dut.voice_source_state_q == 24'sd65536 &&
              dut.engine_side_delta_q == -25'sd65536,
              "first C128 transfer was not the absolute U116 level");
        phase_event(1'b0);
        phase_event(1'b1);
        check(dut.voice_source_state_q == 24'sd65536 &&
              dut.engine_side_delta_q == -25'sd65536,
              "repeated steady C128 transfer collapsed to zero");
        release dut.voice_count_q;

        // A U116 switch event may occur while Phi1 is already active. With
        // zero p/y and code-zero F1/F3 banks, x:0->65536 must first update F1
        // through C205+C128, then pass that same rounded y1 change through
        // C127 into F3. A prior-cycle snapshot would leave F3 at zero here.
        checkpoint = 10;
        reset_all();
        f1_code = 4'h0;
        f3_code = 4'h0;
        voice_amp_code = 4'h0;
        force dut.voice_count_q = 4'h3;
        phase_event(1'b0);
        phase_event(1'b1);
        check(dut.f1_state_q == 0 && dut.f1_history_q == 0 &&
              dut.f3_state_q == 0 && dut.f3_history_q == 0,
              "mid-Phi1 fixture did not begin with zero F1/F3 charge");
        @(negedge clk);
        voice_amp_code = 4'hF;
        source_clock();
        check(dut.voice_source_state_q == 24'sd65536,
              "exact U116 fixture did not produce a +65536 source step");
        wait_engine_idle();
        check(dut.f1_history_q == -24'sd30247 &&
              dut.f1_state_q == 24'sd658,
              "mid-Phi1 C205/C128 event did not update F1 exactly");
        check(dut.f3_history_q == -24'sd269 &&
              dut.f3_state_q == 24'sd47,
              "C127 did not receive the same-event rounded F1 change");
        release dut.voice_count_q;

        // Keep one broad run as a topology smoke test. It only checks that
        // schematic charge reaches every serial formant and U146; it does not
        // prescribe a pitch, volume, waveform, or tuned sample value.
        checkpoint = 11;
        reset_all();
        force dut.voice_count_q = 4'h3;
        voice_amp_code = 4'hF;
        filter_amp_code = 4'hF;
        f1_code = 4'h8;
        f2_code = 4'h8;
        f2_res_code = 4'h8;
        f3_code = 4'h8;
        f4_code = 4'h8;
        repeat (32) begin
            pulse_noise();
            phase_pair();
        end
        check(dut.f1_state_q != 0 && dut.f2_state_q != 0 &&
              dut.f3_state_q != 0 && dut.f4_state_q != 0 &&
              dut.f5_state_q != 0,
              "U116 charge did not cross the five serial formants");
        check(dut.output_hold_q != 0 && !dut.engine_overrun_q,
              "U146 produced no held output or the engine overran");
        release dut.voice_count_q;

        // PD/RST clears U61 only. It must not reset U60 or the HCC4006/U75
        // recurrence. This guards the digital source gates used above.
        reset_all();
        force dut.voice_count_q = 4'h3;
        @(negedge clk);
        pd_rst_n = 1'b0;
        @(posedge clk);
        #1;
        check(dut.voice_count_q == 4'h3 &&
              !dut.pitch_sync1_q && !dut.pitch_sync2_q &&
              !dut.voice_load_pending_q,
              "PD/RST changed U60 or failed to clear U61");
        release dut.voice_count_q;
        pd_rst_n = 1'b1;

        check(numerator_alignment_cases != 0 &&
              numerator_stages_seen == 12'hFFF,
              "pre-numerator register alignment did not cover all engine stages");

        if (failures == 0)
            $display("SSI263 SC02 AUDIO PASS: %0d divider vectors, %0d reciprocal constants",
                     divider_cases, reciprocal_constant_cases);
        else
            $display("SSI263 SC02 AUDIO FAIL: %0d checks", failures);
        $finish;
    end

endmodule
