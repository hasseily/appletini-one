`timescale 1ns / 1ps

module tb_ssi263_filter_finalize;

    localparam int unsigned STATE_FILTER_ACCUM = 4;
    localparam int unsigned STATE_FILTER_FINALIZE = 22;
    localparam int unsigned PIPELINE_LATENCY =
`ifdef SSI263_HAS_FILTER_FINALIZE
        151;
`else
        144;
`endif
    localparam int unsigned TICK_CYCLES = 192;
    localparam int unsigned SEQUENCE_SAMPLES = 12;

    logic clk = 1'b0;
    logic rstn = 1'b0;
    logic card_enabled = 1'b1;
    logic warm_reset = 1'b0;
    logic audio_tick = 1'b0;
    logic xck_ce = 1'b0;
    logic start = 1'b0;
    logic [5:0] start_phoneme = 6'h24;
    logic [5:0] start_sc01_phone = 6'h0E;
    // Keep this pipeline golden-vector bench on the unchanged SC-01 path.
    // Native SSI ROM and timing have their own focused checks.
    logic start_votrax = 1'b1;
    logic [1:0] current_function = 2'd0;
    logic [7:0] duration_phoneme = 8'h24;
    logic [7:0] inflection = 8'h52;
    logic [7:0] rate_inflection = 8'hB8;
    logic [7:0] ctrl_art_amp = 8'h0F;
    logic [7:0] filter_freq = 8'hE6;
    logic phoneme_done;
    logic response_done;
    logic signed [15:0] audio;

    // Filled from the uncut pipeline. The finalize state may change fabric
    // latency, but it must not change any sample value or its order.
    logic signed [15:0] expected_audio [0:SEQUENCE_SAMPLES-1] = '{
        16'sd0, 16'sd0, 16'sd0, 16'sd0,
        16'sd2, 16'sd12, 16'sd30, 16'sd64,
        16'sd132, 16'sd244, 16'sd416, 16'sd676
    };

    integer checks = 0;
    integer fabric_cycle = 0;
    integer last_tick_cycle = -1;
    integer output_commits = 0;

    always #5 clk = ~clk;

    always @(posedge clk) begin
        fabric_cycle = fabric_cycle + 1;
        if (rstn && card_enabled && !warm_reset && !audio_tick &&
            dut.synth_state_q == dut.SYNTH_OUT) begin
            output_commits = output_commits + 1;
        end
    end

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
                $display("SSI263 FILTER FINALIZE FAIL: %s", message);
                $fatal(1);
            end
        end
    endtask

    function automatic logic [2:0] last_tap(input integer stage);
        begin
            case (stage)
                2:       last_tap = 3'd4;
                6:       last_tap = 3'd1;
                default: last_tap = 3'd6;
            endcase
        end
    endfunction

    task automatic select_stage(input integer stage);
        begin
            case (stage)
                0: dut.filter_stage_q = dut.FILTER_F1;
                1: dut.filter_stage_q = dut.FILTER_F2;
                2: dut.filter_stage_q = dut.FILTER_FN;
                3: dut.filter_stage_q = dut.FILTER_F2N;
                4: dut.filter_stage_q = dut.FILTER_F3;
                5: dut.filter_stage_q = dut.FILTER_F4;
                6: dut.filter_stage_q = dut.FILTER_FX;
                default: $fatal(1, "bad filter stage %0d", stage);
            endcase
        end
    endtask

    task automatic pulse_reset;
        begin
            @(negedge clk);
            rstn = 1'b0;
            card_enabled = 1'b1;
            warm_reset = 1'b0;
            audio_tick = 1'b0;
            start = 1'b0;
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

    task automatic seed_filter_context;
        begin
            dut.synth_voice_input_q = 24'sd201;
            dut.synth_noise_input_q = 24'sd202;
            dut.synth_f2n_in_q = 24'sd203;
            dut.synth_vn_q = 24'sd204;
            dut.synth_mixed_q = 24'sd205;
            dut.synth_closed_q = 24'sd206;

            dut.synth_f1_q = 24'sd1011;
            dut.synth_f2_q = 24'sd1012;
            dut.synth_fn_q = 24'sd1013;
            dut.synth_f2n_q = 24'sd1014;
            dut.synth_f3_q = 24'sd1015;
            dut.synth_f4_q = 24'sd1016;
            dut.synth_fx_q = 24'sd1017;

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
        end
    endtask

    task automatic set_accum_state(
        input integer stage,
        input logic [2:0] tap,
        input logic signed [55:0] accum,
        input logic signed [55:0] product
    );
        begin
            select_stage(stage);
            dut.mac_tap_q = tap;
            dut.mac_accum_q = accum;
            dut.mac_product_q = product;
            dut.synth_state_q = dut.SYNTH_FILTER_ACCUM;
        end
    endtask

    task automatic expect_not_finalized(input integer stage);
        begin
            case (stage)
                0: require(dut.synth_f1_q == 24'sd1011 &&
                           dut.f1_y1_q == 24'sd21,
                           "F1 finalized one cycle early");
                1: require(dut.synth_f2_q == 24'sd1012 &&
                           dut.f2_y1_q == 24'sd41,
                           "F2 finalized one cycle early");
                2: require(dut.synth_fn_q == 24'sd1013 &&
                           dut.fn_y1_q == 24'sd61,
                           "FN finalized one cycle early");
                3: require(dut.synth_f2n_q == 24'sd1014 &&
                           dut.f2n_y1_q == 24'sd81,
                           "F2N finalized one cycle early");
                4: require(dut.synth_f3_q == 24'sd1015 &&
                           dut.f3_y1_q == 24'sd101,
                           "F3 finalized one cycle early");
                5: require(dut.synth_f4_q == 24'sd1016 &&
                           dut.f4_y1_q == 24'sd121,
                           "F4 finalized one cycle early");
                6: require(dut.synth_fx_q == 24'sd1017 &&
                           dut.fx_y1_q == 24'sd131,
                           "FX finalized one cycle early");
            endcase
        end
    endtask

    task automatic expect_finalized(
        input integer stage,
        input logic signed [23:0] value
    );
        begin
            require(dut.mac_accum_q == 56'sd0 && dut.mac_tap_q == 3'd0,
                    "finalize did not clear the MAC cursor");
            case (stage)
                0: begin
                    require(dut.synth_f1_q == value &&
                            dut.f1_x1_q == 24'sd201 &&
                            dut.f1_x2_q == 24'sd11 &&
                            dut.f1_x3_q == 24'sd12 &&
                            dut.f1_y1_q == value &&
                            dut.f1_y2_q == 24'sd21 &&
                            dut.f1_y3_q == 24'sd22,
                            "F1 result or history mismatch");
                    require(dut.filter_stage_q == dut.FILTER_F2 &&
                            dut.synth_state_q == dut.SYNTH_FILTER_PREP,
                            "F1 next state mismatch");
                end
                1: begin
                    require(dut.synth_f2_q == value &&
                            dut.f2_x1_q == 24'sd1011 &&
                            dut.f2_x2_q == 24'sd31 &&
                            dut.f2_x3_q == 24'sd32 &&
                            dut.f2_y1_q == value &&
                            dut.f2_y2_q == 24'sd41 &&
                            dut.f2_y3_q == 24'sd42,
                            "F2 result or history mismatch");
                    require(dut.filter_stage_q == dut.FILTER_FN &&
                            dut.synth_state_q == dut.SYNTH_FILTER_PREP,
                            "F2 next state mismatch");
                end
                2: begin
                    require(dut.synth_fn_q == value &&
                            dut.fn_x1_q == 24'sd202 &&
                            dut.fn_x2_q == 24'sd51 &&
                            dut.fn_y1_q == value &&
                            dut.fn_y2_q == 24'sd61,
                            "FN result or history mismatch");
                    require(dut.filter_stage_q == dut.FILTER_FN &&
                            dut.synth_state_q == dut.SYNTH_F2N_SCALE,
                            "FN next state mismatch");
                end
                3: begin
                    require(dut.synth_f2n_q == value &&
                            dut.f2n_x1_q == 24'sd203 &&
                            dut.f2n_x2_q == 24'sd71 &&
                            dut.f2n_x3_q == 24'sd72 &&
                            dut.f2n_y1_q == value &&
                            dut.f2n_y2_q == 24'sd81 &&
                            dut.f2n_y3_q == 24'sd82,
                            "F2N result or history mismatch");
                    require(dut.filter_stage_q == dut.FILTER_F2N &&
                            dut.synth_state_q == dut.SYNTH_F2N_MIX,
                            "F2N next state mismatch");
                end
                4: begin
                    require(dut.synth_f3_q == value &&
                            dut.f3_x1_q == 24'sd204 &&
                            dut.f3_x2_q == 24'sd91 &&
                            dut.f3_x3_q == 24'sd92 &&
                            dut.f3_y1_q == value &&
                            dut.f3_y2_q == 24'sd101 &&
                            dut.f3_y3_q == 24'sd102,
                            "F3 result or history mismatch");
                    require(dut.filter_stage_q == dut.FILTER_F3 &&
                            dut.synth_state_q == dut.SYNTH_NOISE_GAIN,
                            "F3 next state mismatch");
                end
                5: begin
                    require(dut.synth_f4_q == value &&
                            dut.f4_x1_q == 24'sd205 &&
                            dut.f4_x2_q == 24'sd111 &&
                            dut.f4_x3_q == 24'sd112 &&
                            dut.f4_y1_q == value &&
                            dut.f4_y2_q == 24'sd121 &&
                            dut.f4_y3_q == 24'sd122,
                            "F4 result or history mismatch");
                    require(dut.filter_stage_q == dut.FILTER_F4 &&
                            dut.synth_state_q == dut.SYNTH_CLOSURE,
                            "F4 next state mismatch");
                end
                6: begin
                    require(dut.synth_fx_q == value && dut.fx_y1_q == value,
                            "FX result or history mismatch");
                    require(dut.filter_stage_q == dut.FILTER_FX &&
                            dut.synth_state_q == dut.SYNTH_SCALE,
                            "FX next state mismatch");
                end
            endcase
        end
    endtask

    task automatic check_non_last(input integer stage);
        logic [2:0] tap;
        begin
            pulse_reset();
            @(negedge clk);
            seed_filter_context();
            tap = last_tap(stage) - 3'd1;
            set_accum_state(stage, tap, 56'sd1000, -56'sd125);
            @(posedge clk);
            #1;
            require(dut.synth_state_q == dut.SYNTH_FILTER_PREP &&
                    dut.mac_tap_q == tap + 3'd1 &&
                    dut.mac_accum_q == 56'sd875,
                    "non-last ACCUM changed its cadence or sum");
            expect_not_finalized(stage);
        end
    endtask

    task automatic check_last(
        input integer stage,
        input logic signed [55:0] accum,
        input logic signed [55:0] product,
        input logic signed [23:0] expected
    );
        logic signed [55:0] sum;
        begin
            pulse_reset();
            @(negedge clk);
            seed_filter_context();
            sum = accum + product;
            set_accum_state(stage, last_tap(stage), accum, product);
            @(posedge clk);
            #1;
`ifdef SSI263_HAS_FILTER_FINALIZE
            require(dut.synth_state_q == STATE_FILTER_FINALIZE &&
                    dut.mac_accum_q == sum && dut.mac_tap_q == 3'd0,
                    "last ACCUM did not capture the final sum");
            expect_not_finalized(stage);
            @(posedge clk);
            #1;
`endif
            expect_finalized(stage, expected);
        end
    endtask

    task automatic arm_pending_finalize;
        begin
            pulse_reset();
            @(negedge clk);
            seed_filter_context();
            set_accum_state(0, 3'd6,
                            56'sd3276800000, 56'sd768606208);
`ifdef SSI263_HAS_FILTER_FINALIZE
            @(posedge clk);
            #1;
            require(dut.synth_state_q == STATE_FILTER_FINALIZE,
                    "could not arm the finalize state");
            @(negedge clk);
`endif
        end
    endtask

    task automatic check_finalize_cancellations;
        begin
            arm_pending_finalize();
            warm_reset = 1'b1;
            @(posedge clk);
            #1;
            require(dut.synth_state_q == dut.SYNTH_IDLE &&
                    dut.active_valid_q == 1'b0 &&
                    dut.mac_accum_q == 56'sd0 && audio == 16'sd0,
                    "warm reset did not cancel pending finalize");
            @(negedge clk);
            warm_reset = 1'b0;

            arm_pending_finalize();
            card_enabled = 1'b0;
            @(posedge clk);
            #1;
            require(dut.synth_state_q == dut.SYNTH_IDLE &&
                    dut.active_valid_q == 1'b0 &&
                    dut.mac_accum_q == 56'sd0 && audio == 16'sd0,
                    "card disable did not cancel pending finalize");
            @(negedge clk);
            card_enabled = 1'b1;

            arm_pending_finalize();
            start_votrax = 1'b1;
            start = 1'b1;
            @(posedge clk);
            #1;
            require(dut.synth_state_q == dut.SYNTH_IDLE &&
                    dut.active_valid_q == 1'b1 && dut.is_votrax_q == 1'b1 &&
                    dut.mac_accum_q == 56'sd0 &&
                    dut.f1_y1_q == 24'sd21,
                    "new start did not preempt pending finalize");
            @(negedge clk);
            start = 1'b0;
            start_votrax = 1'b0;

            arm_pending_finalize();
            audio_tick = 1'b1;
            @(posedge clk);
            #1;
            require(dut.synth_state_q == dut.SYNTH_EXCITE &&
                    dut.synth_f1_q == 24'sd1011 &&
                    dut.f1_y1_q == 24'sd21,
                    "audio tick did not preempt pending finalize");
            @(negedge clk);
            audio_tick = 1'b0;
            @(posedge clk);
            #1;
            require(dut.synth_state_q == dut.SYNTH_FILTER_PREP &&
                    dut.mac_accum_q == 56'sd0 && dut.mac_tap_q == 3'd0,
                    "replacement sample did not restart the MAC");
        end
    endtask

    task automatic force_sequence_inputs;
        begin
            force dut.active_valid_q = 1'b1;
            force dut.is_votrax_q = 1'b0;
            force dut.ticks_q = 5'd1;
            force dut.pitch_q = 10'd16;
            force dut.pitch_gate_q = 1'b1;
            force dut.cur_noise_q = 1'b1;
            force dut.closure_q = 5'd0;
            force dut.rom_phone_q = 6'h05;
            force dut.rom_fa_q = 4'd5;
            force dut.rom_va_q = 4'd12;
            force dut.rom_cld_q = 4'd0;
            force dut.rom_vd_q = 4'd0;
            force dut.rom_closure_q = 1'b0;
            force dut.filt_fa_q = 4'd5;
            force dut.filt_fc_q = 4'd7;
            force dut.filt_va_q = 4'd12;
            force dut.filt_f1_q = 4'd5;
            force dut.filt_f2_q = 5'd8;
            force dut.filt_f2q_q = 4'd4;
            force dut.filt_f3_q = 4'd6;
        end
    endtask

    task automatic release_sequence_inputs;
        begin
            release dut.active_valid_q;
            release dut.is_votrax_q;
            release dut.ticks_q;
            release dut.pitch_q;
            release dut.pitch_gate_q;
            release dut.cur_noise_q;
            release dut.closure_q;
            release dut.rom_phone_q;
            release dut.rom_fa_q;
            release dut.rom_va_q;
            release dut.rom_cld_q;
            release dut.rom_vd_q;
            release dut.rom_closure_q;
            release dut.filt_fa_q;
            release dut.filt_fc_q;
            release dut.filt_va_q;
            release dut.filt_f1_q;
            release dut.filt_f2_q;
            release dut.filt_f2q_q;
            release dut.filt_f3_q;
        end
    endtask

    task automatic run_sequence_sample(input integer index);
        integer latency;
        begin
            @(negedge clk);
            require(dut.synth_state_q == dut.SYNTH_IDLE,
                    "sample tick arrived while the pipeline was busy");
            audio_tick = 1'b1;
            @(posedge clk);
            #1;
            require(dut.synth_state_q == dut.SYNTH_EXCITE,
                    "sample tick did not start the pipeline");
            if (last_tick_cycle >= 0) begin
                require(fabric_cycle - last_tick_cycle == TICK_CYCLES,
                        "audio tick cadence changed");
            end
            last_tick_cycle = fabric_cycle;
            @(negedge clk);
            audio_tick = 1'b0;

            latency = 0;
            while (dut.synth_state_q != dut.SYNTH_IDLE) begin
                @(posedge clk);
                #1;
                latency = latency + 1;
                require(latency <= PIPELINE_LATENCY,
                        "synth pipeline failed to finish before its bound");
            end
            require(latency == PIPELINE_LATENCY,
                    "synth pipeline latency mismatch");
`ifdef SSI263_DUMP_SEQUENCE
            $display("SSI263_SEQUENCE[%0d]=%0d", index, $signed(audio));
`else
            require(audio == expected_audio[index],
                    "synth sample sequence mismatch");
`endif
        end
    endtask

    task automatic check_exact_sequence;
        integer index;
        begin
            pulse_reset();
            force_sequence_inputs();
            output_commits = 0;
            last_tick_cycle = -1;
            for (index = 0; index < SEQUENCE_SAMPLES; index = index + 1) begin
                run_sequence_sample(index);
                if (index + 1 < SEQUENCE_SAMPLES) begin
                    repeat (TICK_CYCLES - PIPELINE_LATENCY - 1) @(posedge clk);
                end
            end
            require(output_commits == SEQUENCE_SAMPLES,
                    "pipeline did not commit exactly one output per tick");
            release_sequence_inputs();
        end
    endtask

    initial begin
        integer stage;

        repeat (3) @(posedge clk);
        for (stage = 0; stage < 7; stage = stage + 1) begin
            check_non_last(stage);
        end

        for (stage = 0; stage < 7; stage = stage + 1) begin
            check_last(stage,
                       56'sd3276800000,
                       56'sd768606208,
                       24'sd123456);
        end

        // Exact signed 24-bit saturation limits and one step beyond each.
        check_last(6, 56'sd274877874176, 56'sd0, 24'sd8388607);
        check_last(6, 56'sd274877906944, 56'sd0, 24'sd8388607);
        check_last(6, -56'sd274877906944, 56'sd0, -24'sd8388608);
        check_last(6, -56'sd274877939712, 56'sd0, -24'sd8388608);

        check_finalize_cancellations();
        check_exact_sequence();

        $display("SSI263 FILTER FINALIZE PASS checks=%0d latency=%0d samples=%0d",
                 checks, PIPELINE_LATENCY, SEQUENCE_SAMPLES);
        $finish;
    end

endmodule
