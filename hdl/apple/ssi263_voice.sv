`timescale 1ns / 1ps

// One fixed SSI-263AP socket. Bus state comes from the native SC-02 digital
// core; the separate audio block consumes only that core's proved controls.
module ssi263_voice (
    input  logic               clk,
    input  logic               rstn,
    input  logic               apple_res,
    input  logic               card_enabled,
    input  logic               audio_tick,
    input  logic               xck_ce,

    // The selected write remains high for one fabric cycle. The native core
    // latches its held address and data on the following falling edge.
    input  logic               ssi_write_active,
    input  logic [2:0]         ssi_reg,
    input  logic [7:0]         ssi_wdata,
    output logic               ssi_d7,
    output logic               ar_drive_low,

    output logic signed [15:0] audio,

    // Existing top-level debug names retain their ABI. "done" is now the
    // response-boundary pulse and "enable" is the native A/R enable state.
    output logic               dbg_backend_done,
    output logic               dbg_enable_ints
);

    logic       powered_down;
    logic       phone_active;
    logic       ar_enabled;
    logic       response_boundary_ce;
    logic       voice_toggle;
    logic       noise_clock_ce;
    logic       filter_phase_ce;
    logic       filter_phase;
    logic       fricative;
    logic       voiced;
    logic       fric1_sw;
    logic       fric2_sw;
    logic       closure;
    logic [7:0] filter_frequency;
    logic [3:0] f1_code;
    logic [3:0] f2_code;
    logic [3:0] f2_res_code;
    logic [3:0] f3_code;
    logic [3:0] f4_code;
    logic [3:0] filter_amp_code;
    logic [3:0] voice_amp_code;
    logic [3:0] fric_amp_code;

    assign dbg_backend_done = response_boundary_ce;
    assign dbg_enable_ints = ar_enabled;

    ssi263_sc02_core #(
        .REVISION_AP(1'b1)
    ) core_i (
        .clk(clk),
        .rstn(rstn && card_enabled),
        .pd_rst_n(apple_res),
        .xck_ce(xck_ce),
        .div2(1'b1),
        .write_active(ssi_write_active),
        .write_reg(ssi_reg),
        .write_data(ssi_wdata),
        .d7_pending(ssi_d7),
        .ar_drive_low(ar_drive_low),
        .powered_down(powered_down),
        .phone_active(phone_active),
        .ar_enabled(ar_enabled),
        .response_phoneme(),
        .transitioned_pitch(),
        .phoneme(),
        .duration(),
        .rate(),
        .inflection(),
        .pitch_inflection(),
        .transitioned_inflection_state(),
        .articulation(),
        .amplitude(),
        .filter_frequency(filter_frequency),
        .effective_xck_ce(),
        .response_boundary_ce(response_boundary_ce),
        .voice_clock_ce(),
        .voice_toggle(voice_toggle),
        .pitch_period_ce(),
        .noise_clock_ce(noise_clock_ce),
        .filter_phase_ce(filter_phase_ce),
        .filter_phase(filter_phase),
        .selector(),
        .selector_phase(),
        .selector_step_ce(),
        .selector_rom_data(),
        .selector_flags(),
        .phone_fricative(),
        .phone_voiced(),
        .pw_0(),
        .pw_1(),
        .pw_2(),
        .pw_3(),
        .pw_5(),
        .fric1_sw(fric1_sw),
        .fric2_sw(fric2_sw),
        .fricative(fricative),
        .voiced(voiced),
        .closure(closure),
        .rate_clock_ce(),
        .rate_clock_div2_ce(),
        .articulation_step_ce(),
        .inflection_step_ce(),
        .parameter_write_ce(),
        .parameter_write_selector(),
        .f1_code(f1_code),
        .f2_code(f2_code),
        .f2_res_code(f2_res_code),
        .f3_code(f3_code),
        .f4_code(f4_code),
        .filter_amp_code(filter_amp_code),
        .voice_amp_code(voice_amp_code),
        .fric_amp_code(fric_amp_code)
    );

    ssi263_sc02_audio audio_i (
        .clk(clk),
        .rstn(rstn && card_enabled),
        .audio_tick(audio_tick),
        .phone_active(phone_active),
        .powered_down(powered_down),
        .fricative(fricative),
        .voiced(voiced),
        .noise_clock_ce(noise_clock_ce),
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
        .audio_sample(audio)
    );

endmodule
