`timescale 1ns / 1ps

// The reference module comes from git 3101934, with only its module name
// changed by test_ssi263_filter_neutral.py. Never use the new backend twice.
module tb_ssi263_filter_neutral;
    localparam integer FRAME_CYCLES = 384;
    logic clk = 1'b0;
    logic rstn = 1'b0;
    logic card_enabled = 1'b1;
    logic warm_reset = 1'b0;
    logic audio_tick = 1'b0;
    logic xck_ce = 1'b0;
    logic start = 1'b0;
    logic start_votrax = 1'b0;
    logic [5:0] start_phoneme = 6'h2D;
    logic [5:0] start_sc01_phone = 6'h0E;
    logic [1:0] current_function = 2'd2;
    logic [7:0] duration_phoneme = 8'hED;
    logic [7:0] inflection = 8'h52;
    logic [7:0] rate_inflection = 8'hFB;
    logic [7:0] ctrl_art_amp = 8'h7B;
    logic [7:0] filter_freq = 8'd128;
    logic phoneme_done, response_done, reference_done, reference_response;
    logic signed [15:0] audio, reference_audio;
    logic compare_audio = 1'b1;
    integer cycles = 0;
    integer state_checks = 0;
    integer exact_checks = 0;
    integer frames = 0;
    integer nonzero_frames = 0;
    integer responses = 0;
    integer completions = 0;
    integer xck_div = 0;
    integer waveform;

    always #5 clk = ~clk;
    // Preserve the 1.024 MHz raw XCK / 48 kHz DAC ratio while allowing the
    // production MAC to finish in a shorter simulated fabric-clock interval.
    always @(negedge clk) begin
        xck_div = (xck_div + 1) % 18;
        xck_ce = (xck_div == 0);
    end

    ssi263_formant_backend dut (
        .clk(clk), .rstn(rstn), .card_enabled(card_enabled),
        .warm_reset(warm_reset), .audio_tick(audio_tick), .xck_ce(xck_ce),
        .start(start), .start_phoneme(start_phoneme),
        .start_sc01_phone(start_sc01_phone), .start_votrax(start_votrax),
        .current_function(current_function), .duration_phoneme(duration_phoneme),
        .inflection(inflection), .rate_inflection(rate_inflection),
        .ctrl_art_amp(ctrl_art_amp), .filter_freq(filter_freq),
        .phoneme_done(phoneme_done), .response_done(response_done), .audio(audio)
    );
    ssi263_formant_backend_f122 reference_dut (
        .clk(clk), .rstn(rstn), .card_enabled(card_enabled),
        .warm_reset(warm_reset), .audio_tick(audio_tick), .xck_ce(xck_ce),
        .start(start), .start_phoneme(start_phoneme),
        .start_sc01_phone(start_sc01_phone), .start_votrax(start_votrax),
        .current_function(current_function), .duration_phoneme(duration_phoneme),
        .inflection(inflection), .rate_inflection(rate_inflection),
        .ctrl_art_amp(ctrl_art_amp), .filter_freq(filter_freq),
        .phoneme_done(reference_done), .response_done(reference_response),
        .audio(reference_audio)
    );

    task automatic require(input logic condition, input string message);
        if (condition !== 1'b1)
            $fatal(1, "SSI263 FILTER NEUTRAL FAIL: %s cycle=%0d frame=%0d",
                   message, cycles, frames);
    endtask

    always @(posedge clk) begin
        cycles = cycles + 1;
        #1;
        if (rstn) begin
            state_checks = state_checks + 1;
            require({dut.core_ticks, dut.core_pitch, dut.core_pitch_noise_gate,
                     dut.core_noise_bit, dut.core_closure_age, dut.core_cur_f1,
                     dut.core_cur_f2, dut.core_cur_f3, dut.core_cur_fa,
                     dut.core_cur_fc, dut.core_cur_va, dut.core_cur_f2q,
                     dut.digital_core_i.ssi_durclk_ticks_left_q,
                     dut.digital_core_i.ssi_duration_frame_q,
                     dut.digital_core_i.ssi_response_subticks_left_q,
                     dut.digital_core_i.ssi_response_slot_q,
                     phoneme_done, response_done} ===
                    {reference_dut.core_ticks, reference_dut.core_pitch,
                     reference_dut.core_pitch_noise_gate,
                     reference_dut.core_noise_bit, reference_dut.core_closure_age,
                     reference_dut.core_cur_f1, reference_dut.core_cur_f2,
                     reference_dut.core_cur_f3, reference_dut.core_cur_fa,
                     reference_dut.core_cur_fc, reference_dut.core_cur_va,
                     reference_dut.core_cur_f2q,
                     reference_dut.digital_core_i.ssi_durclk_ticks_left_q,
                     reference_dut.digital_core_i.ssi_duration_frame_q,
                     reference_dut.digital_core_i.ssi_response_subticks_left_q,
                     reference_dut.digital_core_i.ssi_response_slot_q,
                     reference_done, reference_response},
                    "source, duration or response cadence differs from F1.2.2");
            if (compare_audio) begin
                exact_checks = exact_checks + 1;
                require(audio === reference_audio,
                        "audio differs from the actual F1.2.2 backend");
                require({dut.synth_state_q, dut.filter_stage_q, dut.mac_tap_q,
                         dut.active_valid_q, dut.is_votrax_q} ===
                        {reference_dut.synth_state_q, reference_dut.filter_stage_q,
                         reference_dut.mac_tap_q, reference_dut.active_valid_q,
                         reference_dut.is_votrax_q},
                        "neutral synthesis cadence differs from F1.2.2");
            end
            if (response_done) responses = responses + 1;
            if (phoneme_done) completions = completions + 1;
        end
    end

    task automatic reset_and_start(input logic votrax, input logic [5:0] phone,
                                    input logic [7:0] ff);
        @(negedge clk);
        rstn = 1'b0;
        start = 1'b0;
        audio_tick = 1'b0;
        card_enabled = 1'b1;
        warm_reset = 1'b0;
        start_votrax = votrax;
        start_phoneme = phone;
        start_sc01_phone = phone;
        duration_phoneme = {2'b11, phone};
        filter_freq = ff;
        ctrl_art_amp = 8'h7B;
        inflection = 8'h52;
        rate_inflection = 8'hFB;
        repeat (4) @(negedge clk);
        rstn = 1'b1;
        repeat (4) @(negedge clk);
        require(audio == 0 && reference_audio == 0, "power reset is not quiet");
        start = 1'b1;
        @(negedge clk);
        start = 1'b0;
        repeat (4) @(negedge clk);
    endtask

    task automatic frame;
        @(negedge clk);
        audio_tick = 1'b1;
        @(negedge clk);
        audio_tick = 1'b0;
        repeat (FRAME_CYCLES - 1) @(negedge clk);
        require(dut.synth_state_q == dut.SYNTH_IDLE &&
                reference_dut.synth_state_q == reference_dut.SYNTH_IDLE,
                "synthesis missed the sample deadline");
        require(!$isunknown(audio), "unknown audio");
        if (audio != 0) nonzero_frames = nonzero_frames + 1;
        $fwrite(waveform, "%0d,%0d,%0d,%0d,%0d\n", frames, filter_freq,
                start_votrax, $signed(audio), $signed(reference_audio));
        frames = frames + 1;
    endtask

    initial begin
        integer before_nonzero;
        logic [5:0] phones [0:11];
        logic [7:0] neutral;
        phones = '{6'h00, 6'h01, 6'h02, 6'h0E, 6'h10, 6'h16,
                   6'h20, 6'h24, 6'h2D, 6'h32, 6'h39, 6'h3F};
        neutral = $test$plusargs("wrong_neutral") ? 8'd127 : 8'd128;
        waveform = $fopen("neutral_waveform.csv", "w");
        require(waveform != 0, "cannot open waveform");
        // Voiced, fricative, closure and pause mappings, including real DONE
        // and response events. No internal excitation/coefficient forcing.
        for (integer p = 0; p < 12; p = p + 1) begin
            reset_and_start(1'b0, phones[p], neutral);
            repeat (1024) frame();
        end
        require(responses > 0 && completions > 0,
                "duration/response comparisons did not observe real events");
        // A long retained vowel, with accepted control changes and repeated
        // starts. Exact agreement includes every intermediate fabric cycle.
        reset_and_start(1'b0, 6'h2D, neutral);
        before_nonzero = nonzero_frames;
        for (integer i = 0; i < 8192; i = i + 1) begin
            if ((i % 512) == 0) begin
                @(negedge clk);
                inflection = 8'h40 + ((i / 512) * 3);
                rate_inflection = 8'hAB + ((i / 512) & 3);
                ctrl_art_amp = 8'h78 | ((i / 512) & 7);
                start = 1'b1;
                @(negedge clk);
                start = 1'b0;
            end
            frame();
        end
        require(nonzero_frames - before_nonzero > 512,
                "long waveform comparison was silent");
        // Existing CTL/amplitude mute behavior remains identical.
        ctrl_art_amp = 8'h80;
        repeat (512) frame();
        ctrl_art_amp = 8'h70;
        repeat (512) frame();
        @(negedge clk);
        warm_reset = 1'b1;
        @(negedge clk);
        require(audio == 0 && reference_audio == 0, "warm reset is not quiet");
        warm_reset = 1'b0;
        repeat (8) frame();
        @(negedge clk);
        card_enabled = 1'b0;
        @(negedge clk);
        require(audio == 0 && reference_audio == 0, "card disable is not quiet");
        // SC-01 ignores FF; compare legacy audio even at range endpoints.
        for (integer setting = 0; setting < 3; setting = setting + 1) begin
            reset_and_start(1'b1, 6'h0E, setting == 0 ? 0 :
                                        setting == 1 ? 128 : 255);
            repeat (1024) frame();
        end
        // FF0 and FF255 must not mute native speech. Source/DONE/response
        // comparisons continue, but different tract output is expected.
        compare_audio = 1'b0;
        for (integer setting = 0; setting < 2; setting = setting + 1) begin
            reset_and_start(1'b0, 6'h2D, setting == 0 ? 0 : 255);
            before_nonzero = nonzero_frames;
            repeat (2048) frame();
            require(nonzero_frames > before_nonzero, "FF endpoint muted speech");
        end
        $fclose(waveform);
        $display("SSI263 FILTER NEUTRAL PASS cycles=%0d state_checks=%0d exact_checks=%0d frames=%0d nonzero=%0d responses=%0d done=%0d",
                 cycles, state_checks, exact_checks, frames, nonzero_frames,
                 responses, completions);
        $finish;
    end
endmodule
