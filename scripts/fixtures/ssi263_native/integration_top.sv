`timescale 1ns / 1ps
module integration_top #(
    parameter integer CHIP_TYPE = 2,
    parameter bit COMPAT = 1
)(
    input logic clk, rstn, apple_res, card_enabled, xck_ce, audio_tick,
    input logic write_strobe,
    input logic [2:0] write_reg, card_mode,
    input logic [7:0] write_data, via_pcr,
    input logic compat_write,
    input logic [7:0] compat_data,
    output logic signed [15:0] audio,
    output logic audio_valid, d7, irq, done, ints, fault, busy, tick_done,
    output logic [6:0] ifr_set, ifr_clr,
    output logic [39:0] registers, native_registers,
    output logic [1:0] mode, pitch_mode,
    output logic [11:0] pitch_active, response_left,
    output logic [13:0] duration_left,
    output logic [3:0] response_phase, duration_phase,
    output logic div2, response_raw, backend_start,
    output logic native_valid,
    output logic signed [15:0] native_audio,
    output logic [31:0] codes
);
    ssi263_voice #(.SSI263_TYPE(CHIP_TYPE), .HAS_SC01(COMPAT)) dut (
        .clk(clk), .rstn(rstn), .apple_res(apple_res), .card_enabled(card_enabled),
        .card_mode(card_mode), .audio_tick(audio_tick), .xck_ce(xck_ce),
        .ssi_write_strobe(write_strobe), .ssi_reg(write_reg), .ssi_wdata(write_data),
        .ssi_d7(d7), .votrax_write_strobe(compat_write), .votrax_wdata(compat_data),
        .via_pcr(via_pcr), .via_ifr_set(ifr_set), .via_ifr_clr(ifr_clr),
        .audio(audio), .audio_valid(audio_valid), .direct_irq(irq),
        .dbg_backend_done(done), .dbg_enable_ints(ints)
    );
    assign native_valid = dut.bus_wrapper_i.native_valid;
    assign native_audio = dut.bus_wrapper_i.native_audio;
    assign registers = {dut.bus_wrapper_i.filter_freq_q, dut.bus_wrapper_i.ctrl_art_amp_q,
        dut.bus_wrapper_i.rate_inflection_q, dut.bus_wrapper_i.inflection_q,
        dut.bus_wrapper_i.duration_phoneme_q};
    assign native_registers = dut.bus_wrapper_i.native_engine_i.controller_i.debug_registers;
    assign mode = dut.bus_wrapper_i.current_function_q;
    assign pitch_mode = dut.bus_wrapper_i.native_engine_i.pitch_i.current_function;
    assign pitch_active = dut.bus_wrapper_i.native_engine_i.pitch_i.active_inflection;
    assign fault = dut.bus_wrapper_i.native_fault;
    assign busy = dut.bus_wrapper_i.native_engine_i.busy;
    assign tick_done = dut.bus_wrapper_i.native_engine_i.tick_done;
    assign codes = dut.bus_wrapper_i.native_engine_i.control_codes;
    assign response_left = dut.bus_wrapper_i.response_timing_i.response_left_q;
    assign duration_left = dut.bus_wrapper_i.response_timing_i.duration_left_q;
    assign response_phase = dut.bus_wrapper_i.response_timing_i.response_phase_q;
    assign duration_phase = dut.bus_wrapper_i.response_timing_i.duration_phase_q;
    assign div2 = dut.bus_wrapper_i.response_timing_i.div2_q;
    assign response_raw = dut.bus_wrapper_i.response_timing_i.response_raw_q;
    assign backend_start = dut.bus_wrapper_i.backend_start_q;
endmodule
