`timescale 1ns / 1ps

// Native SSI audio engine. All logic runs on the fabric clock. The bus wrapper
// owns Apple ACK/D7/IRQ; warm_reset applies its retained-register AP policy.
module ssi263_native_engine #(
    parameter ROM_FILE = "ssi263_sc02_rom.mem",
    parameter bit DIV2 = 1'b1,
    parameter integer ART_REFERENCE_RATE = 8,
    parameter integer VOICE_TRIM_Q16 = 2048,
    parameter integer OUTPUT_GAIN = 8,
    parameter logic [7:0] RESET_FILTER_FREQUENCY = 8'hff
) (
    input  logic               clk,
    input  logic               rstn,
    input  logic               warm_reset,
    input  logic               mode_latch,
    input  logic               xck_ce, // Q3 pulse; DIV2=0 accepts effective XCK
    input  logic               write_strobe,
    input  logic [2:0]         write_reg,
    input  logic [7:0]         write_data,
    input  logic               audio_tick,
    output logic signed [15:0] audio,
    output logic               audio_valid,
    output logic               busy,
    output logic               fault,
    output logic               tick_done,
    output logic [3:0]         duration_phase
);
    logic div2_q, effective_ce, source_ce_q, source_valid, source_busy;
    logic [11:0] pitch_for_tick, pitch_q;
    logic [31:0] control_codes, analog_codes;
    logic [2:0] selector;
    logic phase, phase_edge, powered_down, pw3, pw3_known;
    logic fric1, fric1_known, fric2, fric2_known, ampct_zero;
    logic event_phase, event_phase_edge, event_fric1, event_fric2, event_output_open;
    logic signed [23:0] voice_drive, reconstruction;
    logic signed [17:0] fric_drive;
    logic signed [15:0] tract_sample;
    logic tract_ready, tract_busy, tract_done, tract_fault, source_fault;
    logic sample_pending_q, schedule_fault_q;
    logic datapath_rstn;

    assign datapath_rstn = rstn && !warm_reset;
    assign effective_ce = xck_ce && (!DIV2 || div2_q) && !warm_reset;
    assign busy = source_ce_q || source_busy || source_valid || tract_busy;
    assign tick_done = tract_done;
    assign fault = schedule_fault_q || source_fault || tract_fault;

    ssi263_native_pitch pitch_i (
        .clk(clk), .rstn(rstn), .warm_reset(warm_reset), .mode_latch(mode_latch),
        .write_strobe(write_strobe),
        .write_reg(write_reg), .write_data(write_data), .audio_tick(audio_tick),
        .inflection_for_tick(pitch_for_tick),
        .active_inflection(), .current_function()
    );
    ssi263_native_controller #(
        .ROM_FILE(ROM_FILE), .ART_REFERENCE_RATE(ART_REFERENCE_RATE),
        .RESET_FILTER_FREQUENCY(RESET_FILTER_FREQUENCY)
    ) controller_i (
        .clk(clk), .rstn(rstn), .warm_reset(warm_reset), .xck_ce(effective_ce),
        .write_strobe(write_strobe), .write_reg(write_reg), .write_data(write_data),
        .ampct_zero(ampct_zero), .codes(control_codes), .selector(selector),
        .duration_phase(duration_phase), .filter_phase(phase),
        .filter_phase_edge(phase_edge), .powered_down(powered_down),
        .pw3(pw3), .pw3_known(pw3_known),
        .fric1(fric1), .fric1_known(fric1_known),
        .fric2(fric2), .fric2_known(fric2_known), .inflection(),
        .debug_phone(), .debug_phone_valid(), .debug_scan_phase(),
        .debug_held_values(), .debug_held_known(), .debug_dda_a(), .debug_dda_b(),
        .debug_dda_c(), .debug_dda_target(), .debug_dda_up(), .debug_duration_left(),
        .debug_articulation_left(), .debug_amplitude_left(), .debug_filter_left(),
        .debug_filter_frequency(), .debug_registers(), .debug_windows()
    );
    ssi263_native_source #(.VOICE_TRIM_Q16(VOICE_TRIM_Q16)) source_i (
        .clk(clk), .rstn(datapath_rstn), .xck_ce(source_ce_q), .codes(control_codes),
        .selector(selector), .pw3(pw3), .pw3_known(pw3_known), .inflection(pitch_q),
        .fric1(fric1), .fric1_known(fric1_known), .fric2(fric2), .fric2_known(fric2_known),
        .filter_phase(phase), .filter_phase_edge(phase_edge), .powered_down(powered_down),
        .event_codes(analog_codes), .voice_drive(voice_drive), .fric_drive(fric_drive),
        .event_phase(event_phase), .event_phase_edge(event_phase_edge),
        .event_fric1(event_fric1), .event_fric2(event_fric2),
        .event_output_open(event_output_open), .ampct_zero(ampct_zero), .source_fault(source_fault),
        .source_valid(source_valid), .source_busy(source_busy)
    );
    ssi263_native_tract #(.OUTPUT_GAIN(OUTPUT_GAIN)) tract_i (
        .clk(clk), .rstn(datapath_rstn), .event_valid(source_valid), .event_ready(tract_ready),
        .phase(event_phase), .phase_edge(event_phase_edge), .codes(analog_codes),
        .voice_drive(voice_drive), .fric_drive(fric_drive),
        .fric1_route(event_fric1), .fric2_route(event_fric2), .output_open(event_output_open),
        .busy(tract_busy), .event_done(tract_done), .reconstruction(reconstruction),
        .sample(tract_sample), .fault(tract_fault), .state_saturated()
    );

    always_ff @(posedge clk) begin
        if (!datapath_rstn) begin
            div2_q <= 0;
            source_ce_q <= 0;
            pitch_q <= 0;
            sample_pending_q <= 0;
            schedule_fault_q <= 0;
            audio <= 0;
            audio_valid <= 0;
        end else begin
            if (xck_ce) div2_q <= !div2_q;
            source_ce_q <= effective_ce;
            if (effective_ce) begin
                pitch_q <= pitch_for_tick;
                // Physical XCK cannot be stalled. Make a missed deadline
                // explicit instead of replacing or merging charge events.
                if (busy) schedule_fault_q <= 1;
            end
            if (source_valid && !tract_ready) schedule_fault_q <= 1;
            if (audio_tick && sample_pending_q) schedule_fault_q <= 1;
            audio_valid <= 0;
            if (audio_tick) sample_pending_q <= 1;
            // A request includes any XCK accepted on the same edge. Wait
            // for that job, then publish one coherent sample. At the Phasor
            // clock rate this delay is less than one effective XCK period.
            if ((sample_pending_q || audio_tick) && !busy && !effective_ce) begin
                audio <= tract_sample;
                audio_valid <= 1;
                sample_pending_q <= 0;
            end
        end
    end
endmodule
