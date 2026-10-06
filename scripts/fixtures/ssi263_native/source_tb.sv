`timescale 1ns / 1ps

module source_tb (
    input logic clk, rstn, xck_ce,
    input logic [31:0] codes,
    input logic [2:0] selector,
    input logic pw3, pw3_known,
    input logic [11:0] inflection,
    input logic fric1, fric1_known, fric2, fric2_known,
    input logic filter_phase, filter_phase_edge, powered_down,
    output logic [31:0] event_codes,
    output logic signed [23:0] voice_drive,
    output logic signed [17:0] fric_drive,
    output logic event_phase, event_phase_edge, event_fric1, event_fric2,
    output logic event_output_open, ampct_zero, source_fault, source_valid, source_busy,
    output logic [3:0] debug_ampct, debug_voice_count,
    output logic [14:0] debug_voice_left,
    output logic [21:0] debug_noise,
    output logic [2:0] debug_flags
);
    ssi263_native_source dut (.*);
    assign debug_ampct = dut.q.ampct;
    assign debug_voice_count = dut.q.voice_count;
    assign debug_voice_left = dut.q.voice_left;
    assign debug_noise = {dut.q.noise_count, dut.q.noise_d4, dut.q.noise_d3,
                          dut.q.noise_d2, dut.q.noise_d1};
    assign debug_flags = {dut.q.load_pending, dut.q.noise_bit, dut.q.u62};
endmodule
