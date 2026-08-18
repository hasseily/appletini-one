`timescale 1ns / 1ps

// Sampled Apple //e speaker path for ONE//e mode.
//
// The one-bit speaker is mapped to equal positive and negative levels. A
// slow DC estimate follows a held level, and the output subtracts that
// estimate. This removes steady DC while preserving C030 square waves.
module onee_speaker_audio #(
    parameter integer SPEAKER_AMPLITUDE = 12000,
    parameter integer DC_BLOCK_SHIFT    = 8
) (
    input  logic                       clk,
    input  logic                       resetn,
    input  logic                       enabled,
    input  logic                       speaker_level,
    input  logic                       audio_sample_tick,
    output logic signed [15:0]         audio_mono
);

    // Seventeen signed bits hold the full 16-bit PCM range plus its sign.
    // The wider temporary values also cover a full level-to-level step.
    logic signed [16:0] dc_estimate_q;
    logic signed [16:0] target_sample;
    logic signed [17:0] dc_error_wide;
    logic signed [17:0] dc_step_wide;
    logic signed [17:0] dc_next_wide;
    logic signed [18:0] highpass_wide;
    (* KEEP = "TRUE" *) logic signed [18:0] highpass_q;
    logic sample_pending_q;

    function automatic logic signed [15:0] saturate_pcm16(
        input logic signed [18:0] sample
    );
        begin
            // A signed value fits in 16 bits exactly when every bit above
            // bit 15 is a copy of bit 15. This avoids two wide compares on
            // the output of the DC-blocking arithmetic.
            if ((sample[18:15] == 4'b0000) ||
                (sample[18:15] == 4'b1111))
                saturate_pcm16 = sample[15:0];
            else if (sample[18])
                saturate_pcm16 = -16'sd32768;
            else
                saturate_pcm16 = 16'sh7FFF;
        end
    endfunction

    always_comb begin
        if (speaker_level)
            target_sample = SPEAKER_AMPLITUDE;
        else
            target_sample = -SPEAKER_AMPLITUDE;

        dc_error_wide = {target_sample[16], target_sample} -
                        {dc_estimate_q[16], dc_estimate_q};
        dc_step_wide = dc_error_wide >>> DC_BLOCK_SHIFT;

        // Arithmetic shift already gives a one-count negative step for a
        // small negative error. Add the matching positive minimum so either
        // static speaker level reaches exact zero output in finite time.
        if ((dc_error_wide > 18'sd0) && (dc_step_wide == 18'sd0))
            dc_step_wide = 18'sd1;

        dc_next_wide = {dc_estimate_q[16], dc_estimate_q} + dc_step_wide;
        // target - (estimate + step) is exactly (target - estimate) - step.
        // Use the saved error term so the estimate update and output filter do
        // not form a serial add/subtract chain on the 133 MHz sample path.
        highpass_wide = {dc_error_wide[17], dc_error_wide} -
                        {dc_step_wide[17], dc_step_wide};
    end

    always_ff @(posedge clk) begin
        if (!resetn) begin
            dc_estimate_q <= target_sample;
            highpass_q    <= '0;
            sample_pending_q <= 1'b0;
            audio_mono    <= 16'sd0;
        end else if (!enabled) begin
            // Track the muted level so enabling ONE//e does not add a click.
            dc_estimate_q <= target_sample;
            highpass_q    <= '0;
            sample_pending_q <= 1'b0;
            audio_mono    <= 16'sd0;
        end else begin
            // The top-level mixer samples the prior output on the same edge
            // as audio_sample_tick. Register the new high-pass result here
            // and publish it one fabric clock later, well before the next
            // audio tick. This preserves the DAC sample sequence while
            // splitting the DC-block arithmetic from the output clamp.
            sample_pending_q <= 1'b0;
            if (sample_pending_q)
                audio_mono <= saturate_pcm16(highpass_q);

            if (audio_sample_tick) begin
                dc_estimate_q   <= dc_next_wide[16:0];
                highpass_q      <= highpass_wide;
                sample_pending_q <= 1'b1;
            end
        end
    end

endmodule
