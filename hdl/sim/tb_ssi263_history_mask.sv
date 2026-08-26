`timescale 1ns / 1ps

import ssi263_formant_pkg::*;

module tb_ssi263_history_mask;

    logic clk = 1'b0;
    logic rstn = 1'b0;
    logic card_enabled = 1'b1;
    logic warm_reset = 1'b0;
    logic audio_tick = 1'b0;
    logic xck_ce = 1'b0;
    logic start = 1'b0;
    logic [5:0] start_phoneme = 6'h2D;
    logic [5:0] start_sc01_phone = 6'h3F;
    logic start_votrax = 1'b0;
    logic [1:0] current_function = 2'd0;
    logic [7:0] duration_phoneme = 8'h2D;
    logic [7:0] inflection = 8'h52;
    logic [7:0] rate_inflection = 8'hB8;
    logic [7:0] ctrl_art_amp = 8'h7B;
    logic [7:0] filter_freq = 8'hE6;
    logic phoneme_done;
    logic response_done;
    logic signed [15:0] audio;

    integer checks = 0;
    integer stage;
    integer tap;

    always #5 clk = ~clk;

    ssi263_formant_backend dut (
        .clk(clk),
        .rstn(rstn),
        .card_enabled(card_enabled),
        .warm_reset(warm_reset),
        .audio_tick(audio_tick),
        .xck_ce(xck_ce),
        .start(start),
        .start_phoneme(start_phoneme),
        .start_sc01_phone(start_sc01_phone),
        .start_votrax(start_votrax),
        .current_function(current_function),
        .duration_phoneme(duration_phoneme),
        .inflection(inflection),
        .rate_inflection(rate_inflection),
        .ctrl_art_amp(ctrl_art_amp),
        .filter_freq(filter_freq),
        .phoneme_done(phoneme_done),
        .response_done(response_done),
        .audio(audio)
    );

    task automatic require(input logic condition, input string message);
        begin
            checks = checks + 1;
            if (condition !== 1'b1) begin
                $display("SSI263 HISTORY MASK FAIL: %s", message);
                $fatal(1);
            end
        end
    endtask

    task automatic pulse_reset;
        begin
            @(negedge clk);
            rstn = 1'b0;
            start = 1'b0;
            audio_tick = 1'b0;
            @(posedge clk);
            #1;
            @(negedge clk);
            rstn = 1'b1;
            @(posedge clk);
            #1;
            require(dut.synth_state_q == dut.SYNTH_IDLE,
                    "reset did not leave the pipeline idle");
        end
    endtask

    task automatic seed_stale_history;
        begin
            @(negedge clk);
            dut.f1_x1_q = 24'sd11;
            dut.f1_x2_q = 24'sd12;
            dut.f1_x3_q = 24'sd13;
            dut.f1_y1_q = 24'sd21;
            dut.f1_y2_q = 24'sd22;
            dut.f1_y3_q = 24'sd23;
            dut.f2_x1_q = 24'sd31;
            dut.f2_x2_q = 24'sd32;
            dut.f2_x3_q = 24'sd33;
            dut.f2_y1_q = 24'sd41;
            dut.f2_y2_q = 24'sd42;
            dut.f2_y3_q = 24'sd43;
            dut.fn_x1_q = 24'sd51;
            dut.fn_x2_q = 24'sd52;
            dut.fn_y1_q = 24'sd61;
            dut.fn_y2_q = 24'sd62;
            dut.f2n_x1_q = 24'sd71;
            dut.f2n_x2_q = 24'sd72;
            dut.f2n_x3_q = 24'sd73;
            dut.f2n_y1_q = 24'sd81;
            dut.f2n_y2_q = 24'sd82;
            dut.f2n_y3_q = 24'sd83;
            dut.f3_x1_q = 24'sd91;
            dut.f3_x2_q = 24'sd92;
            dut.f3_x3_q = 24'sd93;
            dut.f3_y1_q = 24'sd101;
            dut.f3_y2_q = 24'sd102;
            dut.f3_y3_q = 24'sd103;
            dut.f4_x1_q = 24'sd111;
            dut.f4_x2_q = 24'sd112;
            dut.f4_x3_q = 24'sd113;
            dut.f4_y1_q = 24'sd121;
            dut.f4_y2_q = 24'sd122;
            dut.f4_y3_q = 24'sd123;
            dut.fx_y1_q = 24'sd131;
            dut.presence_low_q = 24'sd141;

            dut.f1_history_valid_q = 3'b111;
            dut.f2_history_valid_q = 3'b111;
            dut.f2n_history_valid_q = 3'b111;
            dut.f3_history_valid_q = 3'b111;
            dut.f4_history_valid_q = 3'b111;
            dut.fn_history_valid_q = 2'b11;
            dut.fx_history_valid_q = 1'b1;
            dut.presence_history_valid_q = 1'b1;
        end
    endtask

    task automatic start_ssi_phone;
        begin
            @(negedge clk);
            start_votrax = 1'b0;
            start = 1'b1;
            @(posedge clk);
            #1;
            start = 1'b0;
        end
    endtask

    task automatic seed_live_inputs;
        begin
            @(negedge clk);
            dut.synth_voice_input_q = 24'sd201;
            dut.synth_f1_q = 24'sd202;
            dut.synth_noise_input_q = 24'sd203;
            dut.synth_f2n_in_q = 24'sd204;
            dut.synth_vn_q = 24'sd205;
            dut.synth_mixed_q = 24'sd206;
            dut.synth_closed_q = 24'sd207;
        end
    endtask

    task automatic select_stage(input integer selected_stage);
        begin
            case (selected_stage)
                0: dut.filter_stage_q = dut.FILTER_F1;
                1: dut.filter_stage_q = dut.FILTER_F2;
                2: dut.filter_stage_q = dut.FILTER_FN;
                3: dut.filter_stage_q = dut.FILTER_F2N;
                4: dut.filter_stage_q = dut.FILTER_F3;
                5: dut.filter_stage_q = dut.FILTER_F4;
                6: dut.filter_stage_q = dut.FILTER_FX;
                default: $fatal(1, "bad filter stage %0d", selected_stage);
            endcase
        end
    endtask

    function automatic logic [2:0] last_tap(input integer selected_stage);
        begin
            case (selected_stage)
                2:       last_tap = 3'd4;
                6:       last_tap = 3'd1;
                default: last_tap = 3'd6;
            endcase
        end
    endfunction

    function automatic logic signed [23:0] live_input(input integer selected_stage);
        begin
            case (selected_stage)
                0:       live_input = 24'sd201;
                1:       live_input = 24'sd202;
                2:       live_input = 24'sd203;
                3:       live_input = 24'sd204;
                4:       live_input = 24'sd205;
                5:       live_input = 24'sd206;
                6:       live_input = 24'sd207;
                default: live_input = 24'sd0;
            endcase
        end
    endfunction

    task automatic expect_prep_sample(
        input integer selected_stage,
        input logic [2:0] selected_tap,
        input logic signed [23:0] expected,
        input string message
    );
        begin
            @(negedge clk);
            select_stage(selected_stage);
            dut.mac_tap_q = selected_tap;
            dut.synth_state_q = dut.SYNTH_FILTER_PREP;
            @(posedge clk);
            #1;
            require(dut.mac_sample_q == expected, message);
        end
    endtask

    task automatic finalize_f1(
        input logic signed [23:0] new_input,
        input logic signed [23:0] new_output
    );
        begin
            @(negedge clk);
            dut.filter_stage_q = dut.FILTER_F1;
            dut.synth_voice_input_q = new_input;
            dut.mac_accum_q =
                {{32{new_output[23]}}, new_output} <<< SC01_COEFF_FRAC_BITS;
            dut.synth_state_q = dut.SYNTH_FILTER_FINALIZE;
            @(posedge clk);
            #1;
        end
    endtask

    task automatic finalize_fx(input logic signed [23:0] new_output);
        begin
            @(negedge clk);
            dut.filter_stage_q = dut.FILTER_FX;
            dut.mac_accum_q =
                {{32{new_output[23]}}, new_output} <<< SC01_COEFF_FRAC_BITS;
            dut.synth_state_q = dut.SYNTH_FILTER_FINALIZE;
            @(posedge clk);
            #1;
        end
    endtask

    initial begin
        logic signed [23:0] expected_presence;
        logic signed [23:0] expected_presence_low;

        pulse_reset();
        seed_stale_history();
        start_ssi_phone();

        require(dut.f1_x1_q == 24'sd11 && dut.f2_y3_q == 24'sd43 &&
                dut.fn_y2_q == 24'sd62 && dut.f2n_x3_q == 24'sd73 &&
                dut.f3_y3_q == 24'sd103 && dut.f4_x2_q == 24'sd112 &&
                dut.fx_y1_q == 24'sd131 && dut.presence_low_q == 24'sd141,
                "SSI start changed stored history instead of masking it");
        require(dut.f1_history_valid_q == 3'b000 &&
                dut.f2_history_valid_q == 3'b000 &&
                dut.f2n_history_valid_q == 3'b000 &&
                dut.f3_history_valid_q == 3'b000 &&
                dut.f4_history_valid_q == 3'b000 &&
                dut.fn_history_valid_q == 2'b00 &&
                !dut.fx_history_valid_q &&
                !dut.presence_history_valid_q,
                "SSI start did not invalidate every delay line");

        seed_live_inputs();
        for (stage = 0; stage < 7; stage = stage + 1) begin
            expect_prep_sample(stage, 3'd0, live_input(stage),
                               "tap zero was masked");
            for (tap = 1; tap <= last_tap(stage); tap = tap + 1) begin
                expect_prep_sample(stage, tap[2:0], 24'sd0,
                                   "stale history tap was not masked");
            end
        end

        // A three-position delay line must expose exactly the entries that a
        // physically zeroed line would contain after each new-phone sample.
        @(negedge clk);
        dut.f1_x1_q = 24'sd711;
        dut.f1_x2_q = 24'sd712;
        dut.f1_x3_q = 24'sd713;
        dut.f1_y1_q = 24'sd721;
        dut.f1_y2_q = 24'sd722;
        dut.f1_y3_q = 24'sd723;
        dut.f1_history_valid_q = 3'b000;

        finalize_f1(24'sd301, 24'sd401);
        require(dut.f1_history_valid_q == 3'b001,
                "first F1 sample did not validate the first tap");
        expect_prep_sample(0, 3'd1, 24'sd301,
                           "first new F1 input tap is not visible");
        expect_prep_sample(0, 3'd2, 24'sd0,
                           "second F1 input tap became valid too early");
        expect_prep_sample(0, 3'd4, 24'sd401,
                           "first new F1 output tap is not visible");
        expect_prep_sample(0, 3'd5, 24'sd0,
                           "second F1 output tap became valid too early");

        finalize_f1(24'sd302, 24'sd402);
        require(dut.f1_history_valid_q == 3'b011,
                "second F1 sample did not validate the second tap");
        expect_prep_sample(0, 3'd2, 24'sd301,
                           "second F1 input tap does not match zeroed history");
        expect_prep_sample(0, 3'd3, 24'sd0,
                           "third F1 input tap became valid too early");
        expect_prep_sample(0, 3'd5, 24'sd401,
                           "second F1 output tap does not match zeroed history");
        expect_prep_sample(0, 3'd6, 24'sd0,
                           "third F1 output tap became valid too early");

        finalize_f1(24'sd303, 24'sd403);
        require(dut.f1_history_valid_q == 3'b111,
                "third F1 sample did not validate the full delay line");
        expect_prep_sample(0, 3'd3, 24'sd301,
                           "third F1 input tap does not match zeroed history");
        expect_prep_sample(0, 3'd6, 24'sd401,
                           "third F1 output tap does not match zeroed history");

        // FX has one stored feedback tap.  It must remain hidden for one
        // sample, then expose only the new-phone output.
        @(negedge clk);
        dut.fx_y1_q = 24'sd913;
        dut.fx_history_valid_q = 1'b0;
        expect_prep_sample(6, 3'd1, 24'sd0,
                           "stale FX feedback was not masked");
        finalize_fx(24'sd501);
        require(dut.fx_history_valid_q && dut.fx_y1_q == 24'sd501,
                "FX did not validate its new feedback sample");
        expect_prep_sample(6, 3'd1, 24'sd501,
                           "new FX feedback did not become visible");

        // Presence is also a one-state recursive filter.  Its first update
        // after a phone start must use zero, not the retained old-phone state.
        @(negedge clk);
        dut.presence_low_q = 24'sd1021;
        dut.presence_history_valid_q = 1'b0;
        dut.synth_scaled_q = 24'sd2048;
        expected_presence = dut.presence_boost_from24(24'sd2048, 24'sd0);
        expected_presence_low = dut.presence_low_next_from24(24'sd2048, 24'sd0);
        dut.synth_state_q = dut.SYNTH_PRESENCE;
        @(posedge clk);
        #1;
        require(dut.synth_presence_q == expected_presence &&
                dut.presence_low_q == expected_presence_low &&
                dut.presence_history_valid_q,
                "presence filter did not start from masked zero history");

        $display("SSI263 HISTORY MASK PASS (%0d checks)", checks);
        $finish;
    end

endmodule
