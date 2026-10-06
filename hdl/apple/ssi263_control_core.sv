`timescale 1ns / 1ps

// Native SSI control state and clock interface.
//
// This path owns the raw phone/ROM and the documented TPARM/source/route
// latches. Its clocks count XCK edges, independent of the output sample rate.
// It has no dependency on the inherited pulse counter or converted targets.
//
// Missing native sequencer/envelope behavior stays at explicit input ports:
// WR_SEL, U37.low, latched CTRL, U62./Q, D3+4, U68 zero and native VA/FA zero.
// These are not aliases for the old audio engine's pitch/amplitude signals.
// latched_ctrl is the prototype's held CTRL signal, not a guessed CTL decode.
// Phi0/Phi1 are native source phases; filter_cycle_ce is a complete filter
// clock cycle and must not be substituted for these phases without evidence.
//
// cold_resetn seeds model state only. There is intentionally no CTL/warm-reset
// input that clears all held state. Source-control known masks distinguish
// bounded FPGA startup values from state established by native events.
module ssi263_control_core #(
    parameter ROM_FILE = "ssi263_sc02_rom.mem"
) (
    input  logic        clk,
    input  logic        cold_resetn,
    input  logic        phone_write,
    input  logic [5:0]  phone_data,
    input  logic [2:0]  selector,
    input  logic        selector_write_ce,
    input  logic [3:0]  host_amplitude,
    input  logic [3:0]  duration_phase,
    input  logic        latched_ctrl,
    input  logic        ampct_zero,
    input  logic        voice_amplitude_zero,
    input  logic        fricative_amplitude_zero,
    input  logic        u62_q_n,
    input  logic        d3_or_d4,
    input  logic        phi1,
    input  logic        phi0_rise,
    input  logic        normal_transition_ce,
    input  logic        amplitude_rate_ce,

    // Already resolved active inflection: immediate/transitioned selection
    // belongs upstream. Divider run/reload policy is explicit, not inferred
    // from phone writes or unverified CTL phase behavior.
    input  logic        xck_ce,
    input  logic        div2,
    input  logic [11:0] active_inflection,
    input  logic [7:0]  filter_frequency,
    input  logic        pitch_run,
    input  logic        pitch_reload,
    input  logic        filter_run,
    input  logic        filter_reload,
    output logic        effective_xck_ce,
    output logic        pitch_cycle_ce,
    output logic        filter_cycle_ce,

    output logic [5:0]  phone,
    output logic        phone_valid,
    output logic [7:0]  parameter_byte,
    output logic [3:0]  target,
    output logic [3:0]  tparm,
    output logic        target_valid,
    output logic        transition_permit,
    output logic        transition_permit_known,
    output logic        pw0,
    output logic        pw1,
    output logic        pw2,
    output logic        pw3,
    output logic        pw5,
    output logic        u20,
    output logic        fric1_sw,
    output logic        fric2_sw,
    output logic        u104c,
    output logic        ampct0,
    output logic        fricative,
    output logic        u32b,
    output logic [7:0]  source_state_known,
    output logic [3:0]  source_logic_known
);
    wire write_parameter = phone_valid && selector_write_ce && !phone_write;
    wire [3:0] source_logic_known_raw;

    always_ff @(posedge clk) begin
        if (!cold_resetn) begin
            phone <= 6'd0;
            phone_valid <= 1'b0;
        end else if (phone_write) begin
            phone <= phone_data;
            phone_valid <= 1'b1;
        end
    end

    ssi263_parameter_rom #(.ROM_FILE(ROM_FILE)) parameter_rom_i (
        .phone(phone), .selector(selector), .parameter_byte(parameter_byte)
    );
    assign tparm = parameter_byte[3:0];
    assign target = (selector == 3'd4) ? host_amplitude : parameter_byte[7:4];
    assign target_valid = phone_valid && (selector != 3'd7);

    ssi263_clock_core clock_core_i (
        .clk(clk), .rstn(cold_resetn), .xck_ce(xck_ce), .div2(div2),
        .pitch_run(pitch_run), .pitch_reload(pitch_reload),
        .immediate_inflection(active_inflection),
        .filter_run(filter_run), .filter_reload(filter_reload),
        .filter_frequency(filter_frequency), .effective_xck_ce(effective_xck_ce),
        .pitch_ce(pitch_cycle_ce), .filter_ce(filter_cycle_ce)
    );

    ssi263_source_control source_control_i (
        .clk(clk), .cold_resetn(cold_resetn), .phone_write(phone_write),
        .wr_sel0(write_parameter && selector == 3'd0),
        .wr_sel1(write_parameter && selector == 3'd1),
        .wr_sel2(write_parameter && selector == 3'd2),
        .duration_phase(duration_phase), .tparm(tparm), .latched_ctrl(latched_ctrl),
        .ampct_zero(ampct_zero), .voice_amplitude_zero(voice_amplitude_zero),
        .fricative_amplitude_zero(fricative_amplitude_zero),
        .tpho5(phone[5]), .u62_q_n(u62_q_n), .d3_or_d4(d3_or_d4),
        .phi1(phi1), .phi0_rise(phi0_rise),
        .pw0(pw0), .pw1(pw1), .pw2(pw2), .pw3(pw3), .pw5(pw5),
        .u20(u20), .fric1_sw(fric1_sw), .fric2_sw(fric2_sw),
        .u104c(u104c), .ampct0(ampct0), .fricative(fricative), .u32b(u32b),
        .state_known(source_state_known), .logic_known(source_logic_known_raw)
    );
    // TPHO5 is not established until a phone has been written. The source
    // block assumes its external inputs are known; do not let the bounded
    // cold phone seed become a claimed fact about the U32B gate.
    assign source_logic_known = {source_logic_known_raw[3] && phone_valid,
                                 source_logic_known_raw[2:0]};

    // Only permissions established by the recorded prototype logic are
    // exposed. F2Q (selector 2) and host-amplitude (4) write timing remain
    // unspecified; exposing their target does not invent a transition rule.
    // Never issue a transition from an unproved held state.
    // This is a combinational qualification of the selector edge, not a
    // registered completion pulse. A consumer samples it on that same edge.
    always_comb begin
        transition_permit = 1'b0;
        transition_permit_known = 1'b1;
        if (write_parameter) begin
            case (selector)
                3'd0, 3'd1, 3'd3: begin
                    transition_permit_known = !normal_transition_ce || source_logic_known[3];
                    transition_permit = normal_transition_ce &&
                                        source_logic_known[3] && u32b;
                end
                3'd5: begin
                    transition_permit_known = !amplitude_rate_ce || source_state_known[0];
                    transition_permit = amplitude_rate_ce && source_state_known[0] && pw0;
                end
                3'd6: begin
                    transition_permit_known = !amplitude_rate_ce || source_state_known[1];
                    transition_permit = amplitude_rate_ce && source_state_known[1] && pw1;
                end
                3'd2, 3'd4: transition_permit_known = 1'b0;
                default: begin end // Selector 7 does not write a parameter.
            endcase
        end
    end

    // synthesis translate_off
    always @(posedge clk) begin
        if (cold_resetn) begin
            if (phone_write && selector_write_ce)
                $fatal(1, "SSI263 control contract: serialize phone and selector writes");
            if (selector_write_ce && !phone_valid)
                $fatal(1, "SSI263 control contract: establish a phone before scanning");
        end
    end
    // synthesis translate_on
endmodule
