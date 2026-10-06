`timescale 1ns / 1ps

// SC-02 prototype TPARM latches and phase-held fricative routes.
// Evidence and net names: docs/SSI263_SC02_ROM_FORMAT.md.
//
// All enables and phase levels belong to clk's domain. WR_SEL0..2 are single
// edge enables, not clocks. The caller supplies the actual scan, duration,
// amplitude-zero, U62 and D3+4 signals; this block does not infer them from the
// old audio engine. Phi1 must remain at each level across a clk edge. While
// Phi1 is high, U112 passes U20, including a new selector-2 value; the hold
// register retains that value when Phi1 closes. Phi0_rise samples NOT U20.
//
// Phase-order contract, not a claim about unknown silicon coincidence:
// - Serialize a phone write and any selector edge.
// - Serialize Phi0_rise and a selector-2 edge that could write U20.
// Simulation assertions reject those ambiguous event combinations.
//
// cold_resetn is a model cold-start seed ONLY, not CTL or an Apple warm reset.
// No reset state for these prototype latches has been established. The seeds
// are bounded hardware values, accompanied by known=0 until an observed event
// establishes the state. Phone writes clear only PW0 and PW1.
module ssi263_source_control (
    input  logic       clk,
    input  logic       cold_resetn,
    input  logic       phone_write,
    input  logic       wr_sel0,
    input  logic       wr_sel1,
    input  logic       wr_sel2,
    input  logic [3:0] duration_phase,
    input  logic [3:0] tparm,
    input  logic       latched_ctrl,
    input  logic       ampct_zero,
    input  logic       voice_amplitude_zero,
    input  logic       fricative_amplitude_zero,
    input  logic       tpho5,
    input  logic       u62_q_n,
    input  logic       d3_or_d4,
    input  logic       phi1,
    input  logic       phi0_rise,
    output logic       pw0,
    output logic       pw1,
    output logic       pw2,
    output logic       pw3,
    output logic       pw5,
    output logic       u20,
    output logic       fric1_sw,
    output logic       fric2_sw,
    output logic       u104c,
    output logic       ampct0,
    output logic       fricative,
    output logic       u32b,
    // Bit order: {FRIC2, FRIC1, U20, PW5, PW3, PW2, PW1, PW0}.
    output logic [7:0] state_known,
    // Bit order: {U32B, FRICATIVE, AMPCT0, U104C}.
    output logic [3:0] logic_known
);

    // A pair is {known, value}. Unknown values remain deterministic in
    // hardware; no synthesizable X is used. Boolean controlling values and
    // equal mux branches preserve facts even when another input is unknown.
    function automatic logic [1:0] known_and(
        input logic [1:0] a, input logic [1:0] b
    );
        known_and = {(a[1] && b[1]) || (a[1] && !a[0]) ||
                     (b[1] && !b[0]), a[0] && b[0]};
    endfunction

    function automatic logic [1:0] known_or(
        input logic [1:0] a, input logic [1:0] b
    );
        known_or = {(a[1] && b[1]) || (a[1] && a[0]) ||
                    (b[1] && b[0]), a[0] || b[0]};
    endfunction

    function automatic logic [1:0] known_not(input logic [1:0] a);
        known_not = {a[1], !a[0]};
    endfunction

    function automatic logic [1:0] known_mux(
        input logic [1:0] select,
        input logic [1:0] when_true,
        input logic [1:0] when_false
    );
        known_mux[0] = select[0] ? when_true[0] : when_false[0];
        known_mux[1] = select[1] ?
            (select[0] ? when_true[1] : when_false[1]) :
            (when_true[1] && when_false[1] &&
             (when_true[0] == when_false[0]));
    endfunction

    logic [1:0] pw0_q, pw1_q, pw2_q, pw3_q, pw5_q;
    logic [1:0] u20_q, u20_d, fric1_hold_q, fric1_state, fric2_q;
    logic [1:0] u20_clock, u104c_state, ampct0_state;
    logic [1:0] fricative_state, u32b_state;
    wire duration_match = duration_phase == (tparm[0] ? 4'd2 : 4'd6);

    always_comb begin
        // PW2 here is the OLD held state. Current TPARM2 also opens the gate
        // at this selector edge, before PW2 receives the current ROM bit.
        u20_clock = known_and(pw1_q,
            known_and(known_or(pw2_q, {1'b1, tparm[2]}),
                known_or(known_and(known_and(pw0_q, pw1_q),
                                  {1'b1, ampct_zero}),
                         {1'b1, fricative_amplitude_zero})));
        u20_d = u20_q;
        if (wr_sel2)
            u20_d = known_mux(u20_clock, {1'b1, tparm[3]}, u20_q);

        // U112 is transparent while Phi1 is open, independently of U166A.
        fric1_state = phi1 ? u20_q : fric1_hold_q;
        u104c_state = known_and(pw3_q, {1'b1, u62_q_n});
        ampct0_state = known_not(u104c_state);
        fricative_state = known_and(
            known_not(known_or({1'b1, d3_or_d4}, u104c_state)),
            {1'b1, u62_q_n || voice_amplitude_zero});
        u32b_state = known_not(known_and(pw5_q,
            {1'b1, tpho5 || !voice_amplitude_zero ||
                   !fricative_amplitude_zero}));

        {pw0, pw1, pw2, pw3, pw5} =
            {pw0_q[0], pw1_q[0], pw2_q[0], pw3_q[0], pw5_q[0]};
        {u20, fric1_sw, fric2_sw} =
            {u20_q[0], fric1_state[0], fric2_q[0]};
        {u104c, ampct0, fricative, u32b} =
            {u104c_state[0], ampct0_state[0],
             fricative_state[0], u32b_state[0]};
        state_known = {fric2_q[1], fric1_state[1], u20_q[1],
                       pw5_q[1], pw3_q[1], pw2_q[1], pw1_q[1], pw0_q[1]};
        logic_known = {u32b_state[1], fricative_state[1],
                       ampct0_state[1], u104c_state[1]};
    end

    always_ff @(posedge clk) begin
        if (!cold_resetn) begin
            pw0_q        <= 2'b00;
            pw1_q        <= 2'b00;
            pw2_q        <= 2'b00;
            pw3_q        <= 2'b00;
            pw5_q        <= 2'b01;
            u20_q        <= 2'b00;
            fric1_hold_q <= 2'b00;
            fric2_q      <= 2'b00;
        end else begin
            if (phone_write) begin
                pw0_q <= 2'b10;
                pw1_q <= 2'b10;
            end else begin
                if (wr_sel0 && duration_match)
                    pw0_q <= 2'b11;
                if (wr_sel1 && duration_match)
                    pw1_q <= 2'b11;
            end

            if (wr_sel2) begin
                pw2_q <= {1'b1, tparm[2]};
                pw5_q <= {1'b1, !tparm[2]};
                pw3_q <= known_mux(pw1_q,
                    {1'b1, latched_ctrl || !tparm[1]}, pw3_q);
            end
            u20_q <= u20_d;

            // Capture next U20 while open: a write in an already-open Phi1
            // must reach the held route, rather than leave the prior value.
            if (phi1)
                fric1_hold_q <= u20_d;
            if (phi0_rise)
                fric2_q <= known_not(u20_q);
        end
    end

    // These exclusions keep unspecified physical event ordering explicit.
    // synthesis translate_off
    always @(posedge clk) begin
        if (cold_resetn) begin
            if (phone_write && (wr_sel0 || wr_sel1 || wr_sel2))
                $fatal(1, "SSI263 source contract: serialize phone and selector writes");
            if ((wr_sel0 && wr_sel1) || (wr_sel0 && wr_sel2) ||
                (wr_sel1 && wr_sel2))
                $fatal(1, "SSI263 source contract: only one selector edge per clk");
            if (phi0_rise && wr_sel2 && (u20_clock != 2'b10))
                $fatal(1, "SSI263 source contract: serialize Phi0 and possible U20 writes");
        end
    end
    // synthesis translate_on

endmodule
