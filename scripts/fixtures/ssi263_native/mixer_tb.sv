`timescale 1ns / 1ps

module mixer_tb (
    input logic clk, rstn, audio_tick,
    input logic signed [15:0] sample0, sample1,
    input logic sample0_valid, sample1_valid,
    input logic signed [4:0] volume_db,
    input logic [7:0] pan,
    input logic [11:0] ay_l, ay_r,
    output logic signed [17:0] speech_l, speech_r,
    output logic signed [15:0] mixed_l, mixed_r,
    output logic mix_valid,
    output logic [15:0] gains [0:3]
);
    // The test runner extracts these exact production functions from
    // mockingboard.sv; the test does not copy its own version of the mixer.
    `include "mockingboard_mix_functions.svh"
    ssi263_stereo_mixer dut (
        .clk(clk), .rstn(rstn), .audio_tick(audio_tick),
        .sample0(sample0), .sample1(sample1), .sample0_valid(sample0_valid), .sample1_valid(sample1_valid),
        .volume_db(volume_db), .pan(pan), .speech_l(speech_l), .speech_r(speech_r), .mix_valid(mix_valid)
    );
    always_comb begin
        mixed_l=mix_speech(mix4_to_wide(ay_l),speech_l);
        mixed_r=mix_speech(mix4_to_wide(ay_r),speech_r);
        for (integer k=0;k<4;k=k+1) gains[k]=dut.gain_q[k];
    end
endmodule
