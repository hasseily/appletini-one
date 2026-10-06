`timescale 1ns / 1ps

// Native SSI source candidate, matching scripts/ssi263_host/native_source.cpp.
// Prototype gates and deterministic host startup policies remain distinct:
// unknown cold FRIC routing uses FRIC1, and CTL hard-mutes the sources. Neither
// policy claims to reproduce an observed SC-02 prototype reset circuit.
module ssi263_native_source #(
    parameter integer VOICE_TRIM_Q16 = 2048
) (
    input  logic               clk,
    input  logic               rstn,
    input  logic               xck_ce,
    input  logic [31:0]        codes,
    input  logic [2:0]         selector,
    input  logic               pw3,
    input  logic               pw3_known,
    input  logic [11:0]        inflection,
    input  logic               fric1,
    input  logic               fric1_known,
    input  logic               fric2,
    input  logic               fric2_known,
    input  logic               filter_phase,
    input  logic               filter_phase_edge,
    input  logic               powered_down,
    // Low nibble first: F1, F2, F2Q, F3, F4, filter AMP, VA, FA.
    output logic [31:0]        event_codes,
    output logic signed [23:0] voice_drive,
    output logic signed [17:0] fric_drive,
    output logic               event_phase,
    output logic               event_phase_edge,
    output logic               event_fric1,
    output logic               event_fric2,
    output logic               event_output_open,
    output logic               ampct_zero,
    output logic               source_fault,
    output logic               source_valid,
    output logic               source_busy
);
    typedef struct packed {
        logic [31:0] analog_codes;
        logic [14:0] voice_left;
        logic [3:0] voice_count;
        logic [3:0] ampct;
        logic [3:0] noise_d1, noise_d3, noise_count;
        logic [4:0] noise_d2, noise_d4;
        logic u62, u68_clock, noise_clock;
        logic pitch_sync1, pitch_sync2, load_pending, noise_bit;
        logic route1, route2;
        logic fault, changed;
    } source_state_t;

    source_state_t q, working_q, settled, pitched, finished;
    logic [3:0] stage_q;
    logic [31:0] codes_q;
    logic [2:0] selector_q;
    logic [11:0] inflection_q;
    logic pw3_q, phase_q, phase_edge_q, powered_down_q;
    logic overrun_q;
    logic blocked, up, nco;
    logic old_sync1;

    // Exhaustive enumeration of every U68 count, gate-clock state, U62,
    // PW3, source-amplitude presence and selector proves convergence in
    // at most three passes. The host's remaining 13 passes do no work.
    // Run one pass per fabric cycle, before and after the pitch operation.
    // Eight busy cycles per XCK leave time for the filter arithmetic.
    function automatic source_state_t settle_once(
        input source_state_t initial_state,
        input logic pw3_value,
        input logic [2:0] selected
    );
        source_state_t s;
        logic gate_blocked, count_up, not_carry, next_clock;
        logic changed, next_noise_clock, feedback, force_bit;
        begin
            s = initial_state;
            changed = 1'b0;
            gate_blocked = pw3_value && !s.u62;
            count_up = !gate_blocked && (|s.analog_codes[31:24]);
            not_carry = count_up ? s.ampct != 4'd15 : s.ampct != 4'd0;
            if ((gate_blocked || not_carry) && s.u62) begin
                s.u62 = 1'b0;
                changed = 1'b1;
            end
            gate_blocked = pw3_value && !s.u62;
            count_up = !gate_blocked && (|s.analog_codes[31:24]);
            not_carry = count_up ? s.ampct != 4'd15 : s.ampct != 4'd0;
            next_clock = selected[2] &&
                !((!not_carry && count_up) ||
                  (!count_up && s.ampct[3:1] == 3'd0));
            if (next_clock && !s.u68_clock) begin
                s.ampct = s.ampct + (count_up ? 4'd1 : 4'd15);
                changed = 1'b1;
            end
            if (next_clock != s.u68_clock) changed = 1'b1;
            s.u68_clock = next_clock;

            next_noise_clock = !(pw3_value && !s.u62) &&
                !selected[1] && (|s.analog_codes[31:28]);
            if (next_noise_clock && !s.noise_clock) begin
                s.noise_count = s.noise_count == 4'd15 ?
                                4'd1 : s.noise_count + 4'd1;
            end else if (!next_noise_clock && s.noise_clock) begin
                force_bit = s.noise_count[3:2] == 2'd0;
                feedback = force_bit ^ s.noise_d1[3] ^ s.noise_d2[4] ^
                           s.noise_d4[3] ^ s.noise_d4[4];
                // Concatenation evaluates all four sections before assignment.
                {s.noise_d1, s.noise_d2, s.noise_d3, s.noise_d4} =
                    {{s.noise_d1[2:0], s.noise_d3[3]},
                     {s.noise_d2[3:0], s.noise_d4[4]},
                     {s.noise_d3[2:0], s.noise_d2[4]},
                     {s.noise_d4[3:0], feedback}};
            end
            s.noise_clock = next_noise_clock;
            s.changed = changed;
            settle_once = s;
        end
    endfunction

    always_comb begin
        settled = settle_once(working_q, pw3_q, selector_q);
        // Inflection takes effect on reload, with no reset on phone/CTL writes.
        pitched = working_q;
        pitched.voice_left = working_q.voice_left - 15'd1;
        blocked = pw3_q && !working_q.u62;
        up = !blocked && (|working_q.analog_codes[31:24]);
        nco = up ? working_q.ampct != 4'd15 : working_q.ampct != 4'd0;
        if (pitched.voice_left == 15'd0) begin
            pitched.voice_left = (15'd4096 - {3'b000, inflection_q}) << 2;
            if (!(blocked || nco)) pitched.u62 = !working_q.u62;
        end

        finished = working_q;
        old_sync1 = working_q.pitch_sync1;
        if (powered_down_q) begin
            finished.pitch_sync1 = 1'b0;
            finished.pitch_sync2 = 1'b0;
            finished.load_pending = 1'b0;
        end else if (phase_edge_q && !phase_q) begin
            finished.pitch_sync1 = working_q.u62;
            finished.pitch_sync2 = old_sync1;
            if (working_q.u62 && !old_sync1) finished.load_pending = 1'b1;
        end
        if (phase_edge_q) begin
            if (phase_q) begin
                if (!powered_down_q && finished.load_pending) begin
                    finished.voice_count = 4'd11;
                    finished.load_pending = 1'b0;
                end else if (working_q.voice_count != 4'd15) begin
                    finished.voice_count = working_q.voice_count + 4'd1;
                end
            end else begin
                finished.analog_codes[23:20] = codes_q[19:16] &
                    {working_q.ampct[3:1], !(pw3_q && !working_q.u62)};
            end
        end
        finished.noise_bit = !(working_q.noise_d3[3] || (pw3_q && !working_q.u62)) &&
                            (!working_q.u62 || working_q.analog_codes[27:24] == 4'd0);
    end

    always_ff @(posedge clk) begin
        if (!rstn) begin
            q <= '0;
            q.voice_left <= 15'd16384;
            q.voice_count <= 4'd15;
            q.noise_d1 <= 4'd1;
            q.noise_count <= 4'd15;
            // Provisional production-SSI startup policy, not a proven U20 reset.
            q.route1 <= 1'b1;
            working_q <= '0;
            stage_q <= 4'd0;
            codes_q <= '0;
            selector_q <= '0;
            inflection_q <= '0;
            pw3_q <= 1'b0;
            phase_q <= 1'b0;
            phase_edge_q <= 1'b0;
            powered_down_q <= 1'b1;
            overrun_q <= 1'b0;
            source_valid <= 1'b0;
            voice_drive <= 24'sd0;
            fric_drive <= 18'sd0;
            event_phase <= 1'b0;
            event_phase_edge <= 1'b0;
            event_output_open <= 1'b0;
        end else begin
            source_valid <= 1'b0;
            event_phase_edge <= 1'b0;
            event_output_open <= 1'b0;
            if (xck_ce && source_busy) overrun_q <= 1'b1;
            if (stage_q == 4'd0) begin
                if (xck_ce) begin
                    codes_q <= codes;
                    selector_q <= selector;
                    inflection_q <= inflection;
                    pw3_q <= pw3_known && pw3;
                    phase_q <= filter_phase;
                    phase_edge_q <= filter_phase_edge;
                    powered_down_q <= powered_down;
                    working_q <= q;
                    // Phase latches track on every XCK within their open phase.
                    if (!filter_phase) begin
                        working_q.analog_codes[3:0] <= codes[3:0];
                        working_q.analog_codes[15:12] <= codes[15:12];
                        working_q.analog_codes[27:24] <= codes[23:20];
                    end else begin
                        working_q.analog_codes[7:4] <= codes[7:4];
                        working_q.analog_codes[11:8] <= codes[11:8];
                        working_q.analog_codes[19:16] <= codes[15:12];
                        working_q.analog_codes[31:28] <= codes[27:24];
                    end
                    if (fric1_known) working_q.route1 <= fric1;
                    if (fric2_known) working_q.route2 <= fric2;
                    stage_q <= 4'd1;
                end
            end else begin
                stage_q <= stage_q + 4'd1;
                case (stage_q)
                    4'd1, 4'd2, 4'd3, 4'd5, 4'd6, 4'd7: begin
                        working_q <= settled;
                        if (stage_q == 4'd3 || stage_q == 4'd7)
                            working_q.fault <= settled.fault || settled.changed;
                    end
                    4'd4: working_q <= pitched;
                    4'd8: begin
                        q <= finished;
                        voice_drive <= !powered_down_q && finished.voice_count != 4'd15 ?
                                       -24'(VOICE_TRIM_Q16) : 24'sd0;
                        fric_drive <= powered_down_q ? 18'sd0 :
                                      finished.noise_bit ? 18'sd301 : -18'sd301;
                        event_phase <= phase_q;
                        event_phase_edge <= phase_edge_q;
                        event_output_open <= phase_edge_q && !phase_q;
                        source_valid <= 1'b1;
                        stage_q <= 4'd0;
                    end
                    default: begin
                        stage_q <= 4'd0;
                        overrun_q <= 1'b1;
                    end
                endcase
            end
        end
    end

    assign event_codes = q.analog_codes;
    assign event_fric1 = q.route1;
    assign event_fric2 = q.route2;
    assign ampct_zero = q.ampct[3:1] == 3'd0;
    assign source_fault = q.fault || overrun_q;
    assign source_busy = stage_q != 4'd0;
endmodule
