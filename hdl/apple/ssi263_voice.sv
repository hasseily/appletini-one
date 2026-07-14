`timescale 1ns / 1ps

// Compatibility shell for the SSI263/SC-01 voice. The Apple-visible behavior
// lives in ssi263_bus_wrapper; audio generation uses the formant backend behind
// the same bus contract.
module ssi263_voice #(
    // AppleWin types: 0=empty, 1=SSI263P, 2=SSI263AP.
    parameter int unsigned SSI263_TYPE = 2,
    parameter bit HAS_SC01 = 1'b0
) (
    input  logic               clk,
    input  logic               rstn,
    input  logic               apple_res,
    input  logic               card_enabled,
    input  logic [2:0]         card_mode,
    input  logic               audio_tick,

    input  logic               ssi_write_strobe,
    input  logic [2:0]         ssi_reg,
    input  logic [7:0]         ssi_wdata,
    output logic               ssi_d7,

    input  logic               votrax_write_strobe,
    input  logic [7:0]         votrax_wdata,

    input  logic [7:0]         via_pcr,
    output logic [6:0]         via_ifr_set,
    output logic [6:0]         via_ifr_clr,

    output logic signed [15:0] audio,
    output logic               direct_irq
);

    ssi263_bus_wrapper #(
        .SSI263_TYPE(SSI263_TYPE),
        .HAS_SC01(HAS_SC01)
    ) bus_wrapper_i (
        .clk(clk),
        .rstn(rstn),
        .apple_res(apple_res),
        .card_enabled(card_enabled),
        .card_mode(card_mode),
        .audio_tick(audio_tick),
        .ssi_write_strobe(ssi_write_strobe),
        .ssi_reg(ssi_reg),
        .ssi_wdata(ssi_wdata),
        .ssi_d7(ssi_d7),
        .votrax_write_strobe(votrax_write_strobe),
        .votrax_wdata(votrax_wdata),
        .via_pcr(via_pcr),
        .via_ifr_set(via_ifr_set),
        .via_ifr_clr(via_ifr_clr),
        .audio(audio),
        .direct_irq(direct_irq)
    );

endmodule
