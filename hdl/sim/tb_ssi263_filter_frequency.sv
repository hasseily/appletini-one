`timescale 1ns / 1ps

module tb_ssi263_filter_frequency;
    localparam int TICK_CYCLES = 384;
    logic clk = 1'b0;
    logic rstn = 1'b0;
    logic card_enabled = 1'b1;
    logic warm_reset = 1'b0;
    logic audio_tick = 1'b0;
    logic xck_ce = 1'b0;
    logic start = 1'b0;
    logic start_votrax = 1'b0;
    logic [7:0] filter_freq = 8'h80;
    logic [7:0] ctrl_art_amp = 8'h7B;
    logic signed [15:0] audio, reference_audio;
    logic phoneme_done, response_done, reference_done, reference_response;
    integer checks = 0;
    integer core_checks = 0;
    integer frame_count = 0;
    integer output_count = 0;
    integer presence_count = 0;
    integer stage_count [0:6];
    integer cycle_count = 0;
    integer frame_start = 0;
    integer max_latency = 0;
    integer response_count = 0;
    integer done_count = 0;
    integer xck_div = 0;
    integer fd;
    integer spectral_fd;
    integer steady_fd;
    logic fixture = 1'b0;
    logic signed [15:0] fixture_source = 16'sd0;

    always #5 clk = ~clk;
    always @(negedge clk) begin
        xck_div = (xck_div + 1) % 4;
        xck_ce = (xck_div == 0);
    end

    ssi263_formant_backend dut (
        .clk(clk), .rstn(rstn), .card_enabled(card_enabled),
        .warm_reset(warm_reset), .audio_tick(audio_tick), .xck_ce(xck_ce),
        .start(start), .start_phoneme(6'h2D), .start_sc01_phone(6'h0E),
        .start_votrax(start_votrax), .current_function(2'd2),
        .duration_phoneme(8'hAD), .inflection(8'h52),
        .rate_inflection(8'hFB), .ctrl_art_amp(ctrl_art_amp),
        .filter_freq(filter_freq), .phoneme_done(phoneme_done),
        .response_done(response_done), .audio(audio)
    );
    ssi263_formant_backend reference_dut (
        .clk(clk), .rstn(rstn), .card_enabled(card_enabled),
        .warm_reset(warm_reset), .audio_tick(audio_tick), .xck_ce(xck_ce),
        .start(start), .start_phoneme(6'h2D), .start_sc01_phone(6'h0E),
        .start_votrax(start_votrax), .current_function(2'd2),
        .duration_phoneme(8'hAD), .inflection(8'h52),
        .rate_inflection(8'hFB), .ctrl_art_amp(ctrl_art_amp),
        .filter_freq(8'h80), .phoneme_done(reference_done),
        .response_done(reference_response), .audio(reference_audio)
    );

    task automatic require(input logic condition, input string message);
        checks = checks + 1;
        if (condition !== 1'b1) begin
            $display("SSI263 FILTER FREQUENCY FAIL: %s", message);
            $fatal(1);
        end
    endtask

    // User contract: half rate at 0, legacy rate at 128, one Q8 unit per code.
    function automatic integer expected_step(input integer ff);
        return 256 + (ff - 128);
    endfunction

    always @(posedge clk) begin
        cycle_count = cycle_count + 1;
        if (rstn && card_enabled && !warm_reset && !start) begin
            if (!audio_tick && dut.synth_state_q == dut.SYNTH_FILTER_FINALIZE)
                stage_count[dut.filter_stage_q] = stage_count[dut.filter_stage_q] + 1;
            if (!audio_tick && dut.synth_state_q == dut.SYNTH_PRESENCE)
                presence_count = presence_count + 1;
            if (!audio_tick && dut.synth_state_q == dut.SYNTH_OUT) begin
                output_count = output_count + 1;
                if (cycle_count - frame_start > max_latency)
                    max_latency = cycle_count - frame_start;
            end
        end
        #1;
        if (rstn && card_enabled && !warm_reset && !fixture) begin
            core_checks = core_checks + 1;
            // Compare actual source, articulation, pitch and completion state
            // every fabric cycle, including while the second pass runs.
            if ({dut.core_ticks, dut.core_pitch, dut.core_pitch_noise_gate,
                 dut.core_noise_bit, dut.core_closure_age, dut.core_cur_f1,
                 dut.core_cur_f2, dut.core_cur_f3, dut.core_cur_fa,
                 dut.core_cur_fc, dut.core_cur_va, dut.core_cur_f2q,
                 phoneme_done, response_done} !==
                {reference_dut.core_ticks, reference_dut.core_pitch,
                 reference_dut.core_pitch_noise_gate, reference_dut.core_noise_bit,
                 reference_dut.core_closure_age, reference_dut.core_cur_f1,
                 reference_dut.core_cur_f2, reference_dut.core_cur_f3,
                 reference_dut.core_cur_fa, reference_dut.core_cur_fc,
                 reference_dut.core_cur_va, reference_dut.core_cur_f2q,
                 reference_done, reference_response})
                $fatal(1, "FF changed source/control/timing state");
            if (response_done) response_count = response_count + 1;
            if (phoneme_done) done_count = done_count + 1;
        end
    end

    task automatic reset_and_start(input logic votrax, input logic [7:0] ff);
        @(negedge clk);
        rstn = 1'b0;
        start = 1'b0;
        audio_tick = 1'b0;
        filter_freq = ff;
        start_votrax = votrax;
        repeat (3) @(negedge clk);
        rstn = 1'b1;
        repeat (3) @(negedge clk);
        require(audio == 16'sd0 && dut.filter_phase_q == 0 &&
                !dut.filter_repeat_q, "reset did not clear audio/rate phase");
        start = 1'b1;
        @(negedge clk);
        start = 1'b0;
        repeat (3) @(negedge clk);
    endtask

    task automatic frame(input logic hot_write, input logic [7:0] new_ff);
        integer jobs, phase, old_audio;
        integer step;
        @(negedge clk);
        step = expected_step(filter_freq);
        require(dut.filter_step_q == step, "rate differs from full-range linear contract");
        if (start_votrax || step == 256) begin
            jobs = 1;
            phase = 0;
        end else begin
            phase = dut.filter_phase_q + step;
            jobs = phase / 256;
            phase = phase % 256;
        end
        for (integer stage = 0; stage < 7; stage = stage + 1)
            stage_count[stage] = 0;
        output_count = 0;
        presence_count = 0;
        frame_start = cycle_count;
        old_audio = $signed(audio);
        audio_tick = 1'b1;
        @(negedge clk);
        audio_tick = 1'b0;
        for (integer cycle = 1; cycle < TICK_CYCLES; cycle = cycle + 1) begin
            if (hot_write && cycle == 50) filter_freq = new_ff;
            @(negedge clk);
        end
        require(dut.synth_state_q == dut.SYNTH_IDLE, "sample missed its completion budget");
        require(output_count == 1 && presence_count == 1,
                "output shaping/envelope must run once per output sample");
        require(dut.filter_phase_q == phase, "fractional phase or hot-write latch mismatch");
        for (integer stage = 0; stage < 7; stage = stage + 1)
            require(stage_count[stage] == jobs, "tract stage pass count mismatch");
        require(!$isunknown(audio), "unknown signed audio result");
        require(($signed(audio) - old_audio <= 3000) &&
                (old_audio - $signed(audio) <= 3000), "signed slew bound exceeded");
        if (start_votrax)
            require(audio === reference_audio, "FF altered an SC-01 sample");
        $fwrite(fd, "%0d,%0d,%0d,%0d,%0d,%0d\n", frame_count,
                filter_freq, jobs, $signed(audio), $signed(reference_audio),
                dut.filter_phase_q);
        frame_count = frame_count + 1;
    endtask

    initial begin
        fd = $fopen("frames.csv", "w");
        spectral_fd = $fopen("f1_impulses.csv", "w");
        steady_fd = $fopen("steady_audio.csv", "w");
        require(fd != 0 && spectral_fd != 0 && steady_fd != 0,
                "cannot open test sample files");
        reset_and_start(1'b0, 8'h80);
        for (integer i = 0; i < 512; i = i + 1) begin
            frame(1'b0, 0);
            require(audio === reference_audio, "128 changed the neutral transfer");
        end
        // Exhaustive FF decode with uninterrupted speech and fractional phase.
        for (integer ff = 0; ff < 256; ff = ff + 1) begin
            filter_freq = ff;
            repeat (3) @(negedge clk);
            for (integer i = 0; i < 16; i = i + 1) frame(1'b0, 0);
        end
        // Accepted in the middle of both one- and two-pass samples; the current
        // sample retains its scheduled pass count, the next sees the new FF.
        for (integer i = 0; i < 64; i = i + 1) begin
            frame(1'b1, 8'h00);
            frame(1'b1, 8'hFF);
        end
        require(response_count > 0 && done_count > 0,
                "completion/pulse comparison did not observe real events");
        reset_and_start(1'b1, 8'h80);
        for (integer ff = 0; ff < 256; ff = ff + 1) begin
            filter_freq = ff;
            repeat (3) @(negedge clk);
            frame(1'b0, 0);
        end

        // Capture enough of the actual native voiced phone to include its
        // attack, with identical reset/core timing at each filter setting.
        for (integer setting = 0; setting < 3; setting = setting + 1) begin
            reset_and_start(1'b0, setting == 0 ? 8'h00 :
                                    setting == 1 ? 8'h80 : 8'hFF);
            for (integer i = 0; i < 2048; i = i + 1) begin
                frame(1'b0, 0);
                $fwrite(steady_fd, "%0d,%0d,%0d,%0d\n", setting, i,
                        $signed(audio), $signed(reference_audio));
            end
        end

        // Isolate the actual F1 recurrence for an impulse-response measurement.
        // It still traverses all production MAC/finalize/history/rate logic.
        // Coefficients stay fixed while only FF changes.
        fixture = 1'b1;
        force dut.filt_f1_q = 4'd7;
        force dut.synth_voice_gain_q = 4'd15;
        force dut.synth_voice_source_q = fixture_source;
        for (integer setting = 0; setting < 3; setting = setting + 1) begin
            reset_and_start(1'b0, setting == 0 ? 8'h00 :
                                    setting == 1 ? 8'h80 : 8'hFF);
            fixture_source = 16'sd0;
            // Put the impulse on a frame that advances the low-rate tract.
            frame(1'b0, 0);
            for (integer i = 0; i < 1024; i = i + 1) begin
                fixture_source = (i == 0) ? 16'sd16384 : 16'sd0;
                frame(1'b0, 0);
                $fwrite(spectral_fd, "%0d,%0d,%0d\n", setting, i,
                        $signed(dut.synth_f1_q));
            end
        end
        release dut.filt_f1_q;
        release dut.synth_voice_gain_q;
        release dut.synth_voice_source_q;
        fixture = 1'b0;

        reset_and_start(1'b0, 8'hFF);
        frame(1'b0, 0);
        @(negedge clk);
        warm_reset = 1'b1;
        @(negedge clk);
        require(audio == 0 && dut.filter_phase_q == 0 &&
                !dut.filter_repeat_q, "warm reset left live rate/audio state");
        warm_reset = 1'b0;
        @(negedge clk);
        card_enabled = 1'b0;
        @(negedge clk);
        require(audio == 0 && dut.filter_phase_q == 0,
                "card disable left live rate/audio state");
        $fclose(fd);
        $fclose(spectral_fd);
        $fclose(steady_fd);
        $display("SSI263 FILTER FREQUENCY PASS checks=%0d core_checks=%0d frames=%0d max_latency=%0d responses=%0d done=%0d",
                 checks, core_checks, frame_count, max_latency, response_count, done_count);
        $finish;
    end
endmodule
