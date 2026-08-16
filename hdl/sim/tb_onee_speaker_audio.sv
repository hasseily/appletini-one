`timescale 1ns / 1ps

module tb_onee_speaker_audio;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;

    logic resetn = 1'b0;
    logic enabled = 1'b0;
    logic speaker_level = 1'b0;
    logic audio_sample_tick = 1'b0;
    wire signed [15:0] audio_mono;
    wire signed [15:0] bounded_audio_mono;

    onee_speaker_audio #(
        .SPEAKER_AMPLITUDE(12000),
        .DC_BLOCK_SHIFT(3)
    ) dut (
        .clk(clk),
        .resetn(resetn),
        .enabled(enabled),
        .speaker_level(speaker_level),
        .audio_sample_tick(audio_sample_tick),
        .audio_mono(audio_mono)
    );

    // A second instance drives the filter beyond signed 16-bit range so the
    // bench proves saturation instead of wraparound.
    onee_speaker_audio #(
        .SPEAKER_AMPLITUDE(30000),
        .DC_BLOCK_SHIFT(3)
    ) bounds_dut (
        .clk(clk),
        .resetn(resetn),
        .enabled(enabled),
        .speaker_level(speaker_level),
        .audio_sample_tick(audio_sample_tick),
        .audio_mono(bounded_audio_mono)
    );

    task automatic check(input logic condition, input string message);
        if (condition !== 1'b1)
            $fatal(1, "%s", message);
    endtask

    task automatic sample_tick;
        audio_sample_tick = 1'b1;
        @(posedge clk);
        #1;
        audio_sample_tick = 1'b0;
    endtask

    integer sample_index;
    integer signed sample_sum;
    integer signed positive_peak;
    integer signed negative_peak;
    logic signed [15:0] held_sample;

    initial begin
        // Reset tracks the current static level while forcing a zero sample.
        enabled = 1'b1;
        speaker_level = 1'b1;
        repeat (4) @(posedge clk);
        check(audio_mono == 16'sd0 && bounded_audio_mono == 16'sd0,
              "reset did not force speaker silence");

        resetn = 1'b1;
        sample_tick();
        check(audio_mono == 16'sd0 && bounded_audio_mono == 16'sd0,
              "reset release added a static-level click");

        // Muted mode stays silent and tracks the input level. Enabling a
        // level which was already present must therefore stay silent.
        enabled = 1'b0;
        speaker_level = 1'b0;
        sample_tick();
        speaker_level = 1'b1;
        sample_tick();
        check(audio_mono == 16'sd0 && bounded_audio_mono == 16'sd0,
              "disabled speaker path was not silent");
        enabled = 1'b1;
        sample_tick();
        check(audio_mono == 16'sd0,
              "enabling a tracked static level caused a click");

        // A high-to-low edge gives a negative sample, then a held zero level
        // decays to exact silence. The wide test instance must clamp low.
        speaker_level = 1'b0;
        sample_tick();
        check(audio_mono < 16'sd0,
              "speaker falling edge did not give a negative response");
        check(bounded_audio_mono == -16'sd32768,
              "negative over-range response did not saturate");
        held_sample = audio_mono;
        repeat (4) @(posedge clk);
        #1;
        check(audio_mono == held_sample,
              "speaker sample changed without audio_sample_tick");
        repeat (256) sample_tick();
        check(audio_mono == 16'sd0,
              "static zero speaker level did not decay to silence");

        // A low-to-high edge gives the matching positive response. A held one
        // level must also decay to exact silence, and the wide path clamps.
        speaker_level = 1'b1;
        sample_tick();
        check(audio_mono > 16'sd0,
              "speaker rising edge did not give a positive response");
        check(bounded_audio_mono == 16'sd32767,
              "positive over-range response did not saturate");
        repeat (256) sample_tick();
        check(audio_mono == 16'sd0,
              "static one speaker level did not decay to silence");

        // Warm the blocker with a square wave, then measure complete pairs.
        // A settled C030-like stream must have strong samples on both sides
        // of zero and little sum bias.
        for (sample_index = 0; sample_index < 64;
             sample_index = sample_index + 1) begin
            speaker_level = ~speaker_level;
            sample_tick();
        end

        sample_sum = 0;
        positive_peak = 0;
        negative_peak = 0;
        for (sample_index = 0; sample_index < 128;
             sample_index = sample_index + 1) begin
            speaker_level = ~speaker_level;
            sample_tick();
            sample_sum = sample_sum + $signed(audio_mono);
            if ($signed(audio_mono) > positive_peak)
                positive_peak = $signed(audio_mono);
            if ($signed(audio_mono) < negative_peak)
                negative_peak = $signed(audio_mono);
            check(($signed(audio_mono) <= 32767) &&
                  ($signed(audio_mono) >= -32768),
                  "speaker output left signed 16-bit bounds");
        end
        check(positive_peak > 8000 && negative_peak < -8000,
              "sustained square wave lost positive or negative response");
        check((sample_sum < 512) && (sample_sum > -512),
              "sustained square wave was not centered around zero");

        enabled = 1'b0;
        @(posedge clk);
        #1;
        check(audio_mono == 16'sd0 && bounded_audio_mono == 16'sd0,
              "disable did not force immediate silence");

        enabled = 1'b1;
        resetn = 1'b0;
        @(posedge clk);
        #1;
        check(audio_mono == 16'sd0 && bounded_audio_mono == 16'sd0,
              "reset after activity did not force silence");

        $display("ONEE SPEAKER AUDIO PASS");
        $finish;
    end

    initial begin
        #500000;
        $fatal(1, "ONEE SPEAKER AUDIO TIMEOUT");
    end

endmodule
