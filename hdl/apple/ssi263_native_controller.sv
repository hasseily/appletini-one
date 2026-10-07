`timescale 1ns / 1ps

// Fabric port of scripts/ssi263_host/native_control.cpp, frozen listening
// candidate. One xck_ce advances one effective SSI XCK. Writes precede that
// tick when both arrive together; writes without a tick still take effect.
//
// Deliberate prototype departures remain unchanged: fixed ART reference
// RATE, full duration restart, omitted U166B permit inhibit, and host CTL
// policy. AMP=0 retains selector-4 state as in the prototype gates.
// See docs/SSI263_ATTACK_AUDIT.md. This module does not
// turn a bounded cold seed into a known held-control value.
module ssi263_native_controller #(
    parameter integer ART_REFERENCE_RATE = 8,
    parameter ROM_FILE = "ssi263_sc02_rom.mem",
    parameter logic [7:0] RESET_FILTER_FREQUENCY = 8'hff
) (
    input  logic        clk,
    input  logic        rstn,
    input  logic        warm_reset,
    input  logic        xck_ce,
    input  logic        write_strobe,
    input  logic [2:0]  write_reg,
    input  logic [7:0]  write_data,
    input  logic        ampct_zero,
    output logic [31:0] codes,
    output logic [2:0]  selector,
    output logic [3:0]  duration_phase,
    output logic        filter_phase,
    output logic        filter_phase_edge,
    output logic        powered_down,
    output logic        pw3,
    output logic        pw3_known,
    output logic        fric1,
    output logic        fric1_known,
    output logic        fric2,
    output logic        fric2_known,
    output logic [11:0] inflection,
    output logic [5:0]  debug_phone,
    output logic        debug_phone_valid,
    output logic [3:0]  debug_scan_phase,
    // Bit order: PW0, PW1, PW2, PW3, PW5, U20, FRIC1, FRIC2.
    output logic [7:0]  debug_held_values,
    output logic [7:0]  debug_held_known,
    output logic [31:0] debug_dda_a,
    output logic [31:0] debug_dda_b,
    output logic [31:0] debug_dda_c,
    output logic [31:0] debug_dda_target,
    output logic [7:0]  debug_dda_up,
    output logic [14:0] debug_duration_left,
    output logic [15:0] debug_articulation_left,
    output logic [12:0] debug_amplitude_left,
    output logic [8:0]  debug_filter_left,
    output logic [7:0]  debug_filter_frequency,
    output logic [39:0] debug_registers,
    // pending/window pairs, low first: phone, control, ART, amplitude, duration.
    output logic [9:0]  debug_windows
);
    // {known, value}. Unknown values have a deterministic zero payload.
    localparam logic [1:0] UNKNOWN_BIT = 2'b00;
    localparam logic [1:0] KNOWN_ZERO = 2'b10;
    localparam logic [1:0] KNOWN_ONE = 2'b11;

    typedef struct packed {
        logic [4:0][7:0] regs;
        logic [7:0][3:0] a, b, c, target, codes;
        logic [7:0] upward;
        logic [2:0] selector;
        logic [3:0] scan_phase, duration_phase;
        logic [14:0] duration_left;
        logic [15:0] articulation_left;
        logic [12:0] amplitude_left;
        logic [8:0] filter_left;
        logic active, phone_valid, filter_phase, filter_edge;
        logic phone_pending, phone_window, control_pending, control_window;
        logic articulation_pending, articulation_window;
        logic amplitude_pending, amplitude_window;
        logic duration_pending, duration_window;
        logic [1:0] pw0, pw1, pw2, pw3, pw5, u20, fric1, fric2;
    } state_t;

    state_t q, n;
    (* rom_style = "distributed" *) logic [7:0] parameter_rom [0:511];
    initial $readmemh(ROM_FILE, parameter_rom);

    function automatic logic [1:0] bit_and(input logic [1:0] a, b);
        if (a == KNOWN_ZERO || b == KNOWN_ZERO) bit_and = KNOWN_ZERO;
        else if (!a[1] || !b[1]) bit_and = UNKNOWN_BIT;
        else bit_and = KNOWN_ONE;
    endfunction

    function automatic logic [1:0] bit_or(input logic [1:0] a, b);
        if (a == KNOWN_ONE || b == KNOWN_ONE) bit_or = KNOWN_ONE;
        else if (!a[1] || !b[1]) bit_or = UNKNOWN_BIT;
        else bit_or = KNOWN_ZERO;
    endfunction

    function automatic logic [1:0] held_load(
        input logic [1:0] old_value, enable, new_value
    );
        if (enable == KNOWN_ONE) held_load = new_value;
        else if (enable == KNOWN_ZERO || old_value == new_value) held_load = old_value;
        else held_load = UNKNOWN_BIT;
    endfunction

    function automatic logic [14:0] duration_period(
        input logic [3:0] rate,
        input logic [1:0] duration
    );
        logic [6:0] periods;
        periods = (7'd16 - {3'b0, rate}) * (7'd4 - {5'b0, duration});
        duration_period = {periods, 8'b0};
    endfunction

    function automatic logic [15:0] articulation_period(input logic [2:0] articulation);
        logic [7:0] periods;
        periods = 8'(16 - ART_REFERENCE_RATE) * (8'd8 - {5'b0, articulation});
        articulation_period = {periods, 8'b0};
    endfunction

    function automatic logic [12:0] amplitude_period(
        input logic [3:0] rate,
        input logic duration_high
    );
        logic [5:0] periods;
        periods = (6'd16 - {2'b0, rate}) << (duration_high ? 1'b0 : 1'b1);
        amplitude_period = {periods, 7'b0};
    endfunction

    logic [2:0] reg_index;
    logic [3:0] rom_target;
    logic early_duration_flag;
    logic [3:1] route_flags;
    logic [3:0] target_value;
    logic setup, permit;
    logic phone_write, control_release;
    logic [4:0] dda_sum;
    logic [1:0] blocked, settled, route_enable;

    always_comb begin
        n = q;
        reg_index = write_reg >= 3'd4 ? 3'd4 : write_reg;
        phone_write = write_strobe && reg_index == 3'd0;
        control_release = write_strobe && reg_index == 3'd3 && q.regs[3][7] && !write_data[7];
        rom_target = 4'd0;
        early_duration_flag = 1'b0;
        route_flags = 3'd0;
        target_value = 4'd0;
        setup = 1'b0;
        permit = 1'b0;
        dda_sum = 5'd0;
        blocked = UNKNOWN_BIT;
        settled = UNKNOWN_BIT;
        route_enable = UNKNOWN_BIT;

        if (write_strobe) begin
            n.regs[reg_index] = write_data;
            if (reg_index == 3'd0) begin
                n.phone_valid = 1'b1;
                n.pw0 = KNOWN_ZERO;
                n.pw1 = KNOWN_ZERO;
                n.phone_pending = 1'b1;
                n.duration_phase = 4'd0;
                n.duration_left = duration_period(n.regs[2][7:4], n.regs[0][7:6]);
                n.duration_pending = 1'b0;
                n.duration_window = 1'b0;
            end else if (reg_index == 3'd3) begin
                n.control_pending = 1'b1;
                n.active = !write_data[7];
                if (q.regs[3][7] && !write_data[7]) begin
                    n.duration_phase = 4'd0;
                    n.duration_left = duration_period(n.regs[2][7:4], n.regs[0][7:6]);
                    n.articulation_left = articulation_period(n.regs[3][6:4]);
                    n.amplitude_left = amplitude_period(n.regs[2][7:4], n.regs[0][7]);
                    n.duration_pending = 1'b0;
                    n.duration_window = 1'b0;
                    n.articulation_pending = 1'b0;
                    n.articulation_window = 1'b0;
                    n.amplitude_pending = 1'b0;
                    n.amplitude_window = 1'b0;
                end
            end
        end

        if (xck_ce) begin
            n.filter_edge = 1'b0;
            if (n.active && n.phone_valid) begin
                n.duration_left = n.duration_left - 15'd1;
                if (n.duration_left == 15'd0) begin
                    n.duration_left = duration_period(n.regs[2][7:4], n.regs[0][7:6]);
                    n.duration_phase = n.duration_phase + 4'd1;
                    n.duration_pending = 1'b1;
                end
                n.articulation_left = n.articulation_left - 16'd1;
                if (n.articulation_left == 16'd0) begin
                    n.articulation_left = articulation_period(n.regs[3][6:4]);
                    n.articulation_pending = 1'b1;
                end
                n.amplitude_left = n.amplitude_left - 13'd1;
                if (n.amplitude_left == 13'd0) begin
                    n.amplitude_left = amplitude_period(n.regs[2][7:4], n.regs[0][7]);
                    n.amplitude_pending = 1'b1;
                end
            end

            n.scan_phase = q.scan_phase + 4'd1;
            if (n.scan_phase == 4'd0) begin
                n.selector = q.selector + 3'd1;
                if (q.selector == 3'd3) begin
                    n.phone_window = n.phone_pending;
                    n.control_window = n.control_pending;
                    n.articulation_window = n.articulation_pending;
                    n.amplitude_window = n.amplitude_pending;
                    n.duration_window = n.duration_pending;
                    n.phone_pending = 1'b0;
                    n.control_pending = 1'b0;
                    n.articulation_pending = 1'b0;
                    n.amplitude_pending = 1'b0;
                    n.duration_pending = 1'b0;
                end
            end

            // A parameter is consumed only at r2/r10/r11, where selector
            // cannot advance. Keep its r0 increment out of the ROM path.
            rom_target = parameter_rom[{n.regs[0][5:0], q.selector}][7:4];
            early_duration_flag = parameter_rom[{n.regs[0][5:0], q.selector}][0];
            // Route flags always come from selector 2. A dedicated read
            // keeps the general selector mux out of the held-route path.
            route_flags = parameter_rom[{n.regs[0][5:0], 3'd2}][3:1];
            target_value = rom_target;
            if (q.selector == 3'd4) target_value = n.regs[3][3:0];
            else if ((q.selector == 3'd5 || q.selector == 3'd6) && n.regs[3][3:0] == 4'd0)
                target_value = 4'd0;

            if (n.scan_phase == 4'd2 && n.phone_valid && q.selector != 3'd7) begin
                // r0 is the only setup-window load; at r2 both setup
                // windows and the selected DDA bank still name old state.
                setup = (q.phone_window && q.selector != 3'd4) ||
                        (q.control_window && q.selector >= 3'd4 && q.selector <= 3'd6 &&
                         (q.selector != 3'd4 || n.regs[3][3:0] != 4'd0));
                blocked = bit_and(q.pw5, {1'b1, n.regs[0][5] ||
                                  q.codes[5] != 4'd0 || q.codes[6] != 4'd0});
                // r0 cannot load a new permit window on this r2 write.
                // Only a coincident bus write can clear an old window:
                // phone writes clear duration; CTL release clears all three.
                case (q.selector)
                    3'd0, 3'd1, 3'd3: permit = q.articulation_window && !control_release && blocked == KNOWN_ZERO;
                    3'd2: permit = q.articulation_window && !control_release;
                    3'd4: permit = q.duration_window && !phone_write && !control_release &&
                                   n.regs[3][3:0] != 4'd0;
                    3'd5: permit = q.amplitude_window && !control_release && !phone_write && q.pw0 == KNOWN_ONE;
                    3'd6: permit = q.amplitude_window && !control_release && !phone_write && q.pw1 == KNOWN_ONE;
                    default: permit = 1'b0;
                endcase
                if (setup) begin
                    n.target[q.selector] = target_value;
                    n.b[q.selector] = target_value - q.a[q.selector];
                    n.c[q.selector] = 4'd8;
                    n.upward[q.selector] = target_value >= q.a[q.selector];
                end else if (!q.phone_pending && !phone_write && n.active && permit &&
                             q.a[q.selector] != q.target[q.selector]) begin
                    dda_sum = {1'b0, q.b[q.selector]} + {1'b0, q.c[q.selector]};
                    n.c[q.selector] = dda_sum[3:0];
                    if (q.upward[q.selector] && dda_sum[4])
                        n.a[q.selector] = q.a[q.selector] + 4'd1;
                    if (!q.upward[q.selector] && !dda_sum[4])
                        n.a[q.selector] = q.a[q.selector] - 4'd1;
                end
            end

            if ((n.scan_phase == 4'd10 || n.scan_phase == 4'd11) && n.phone_valid) begin
                // DDA writes occur only at r2, so r10/r11 must latch q.a.
                // Naming this state directly removes a false serial path
                // through the DDA adder and selector's unrelated r0 update.
                if (q.selector < 3'd7) n.codes[q.selector] = q.a[q.selector];
                if (q.selector < 3'd2 && n.duration_phase == (early_duration_flag ? 4'd2 : 4'd6)) begin
                    if (q.selector == 3'd0) n.pw0 = KNOWN_ONE;
                    else n.pw1 = KNOWN_ONE;
                end else if (q.selector == 3'd2) begin
                    n.pw3 = held_load(n.pw3, n.pw1, {1'b1, n.regs[3][7] || !route_flags[1]});
                    if (n.scan_phase == 4'd10) begin
                        settled = bit_or(bit_and(bit_and(n.pw0, n.pw1), {1'b1, ampct_zero}),
                                         {1'b1, q.codes[6] == 4'd0});
                        route_enable = bit_and(bit_and(n.pw1, bit_or(n.pw2, {1'b1, route_flags[2]})), settled);
                        n.u20 = held_load(n.u20, route_enable, {1'b1, route_flags[3]});
                        n.pw2 = {1'b1, route_flags[2]};
                        n.pw5 = {1'b1, !route_flags[2]};
                    end
                end
            end

            n.filter_left = n.filter_left - 9'd1;
            if (n.filter_left == 9'd0) begin
                n.filter_left = 9'd256 - {1'b0, n.regs[4]};
                n.filter_phase = !n.filter_phase;
                n.filter_edge = 1'b1;
                // A coincident U20 load/Phi0 observes pre-tick U20, as in
                // the frozen host. This is not measured gate propagation.
                if (!n.filter_phase)
                    n.fric2 = q.u20[1] ? {1'b1, !q.u20[0]} : UNKNOWN_BIT;
            end
            if (n.filter_phase) n.fric1 = n.u20;
        end
    end

    always_ff @(posedge clk) begin
        if (!rstn) begin
            q <= '0;
            q.regs[0] <= 8'hc0;
            q.regs[3] <= 8'h80;
            q.regs[4] <= RESET_FILTER_FREQUENCY;
            q.c <= 32'h88888888;
            q.upward <= 8'hff;
            q.duration_left <= duration_period(4'd0, 2'd3);
            q.articulation_left <= articulation_period(3'd0);
            q.amplitude_left <= amplitude_period(4'd0, 1'b1);
            q.filter_left <= 9'd1;
        end else if (warm_reset) begin
            // AP board-reset policy: retain programming registers, stop the
            // voice and clear dynamic state. This preserves the established
            // bus contract; prototype power-up nodes do not prove these seeds.
            q <= '0;
            q.regs <= q.regs;
            q.regs[3] <= 8'h80;
            q.c <= 32'h88888888;
            q.upward <= 8'hff;
            q.phone_valid <= 1'b1;
            q.phone_pending <= 1'b1;
            q.duration_left <= duration_period(q.regs[2][7:4], q.regs[0][7:6]);
            q.articulation_left <= articulation_period(3'd0);
            q.amplitude_left <= amplitude_period(q.regs[2][7:4], q.regs[0][7]);
            q.filter_left <= 9'd256 - {1'b0, q.regs[4]};
        end else q <= n;
    end

    assign codes = q.codes;
    assign selector = q.selector;
    assign duration_phase = q.duration_phase;
    assign filter_phase = q.filter_phase;
    // Held until the next effective tick, so a later source fabric slot can
    // consume it. It is not a free-running one-fabric-cycle pulse.
    assign filter_phase_edge = q.filter_edge;
    assign powered_down = q.regs[3][7];
    assign pw3 = q.pw3[0];
    assign pw3_known = q.pw3[1];
    assign fric1 = q.fric1[0];
    assign fric1_known = q.fric1[1];
    assign fric2 = q.fric2[0];
    assign fric2_known = q.fric2[1];
    assign inflection = {q.regs[2][3], q.regs[1], q.regs[2][2:0]};
    assign debug_phone = q.regs[0][5:0];
    assign debug_phone_valid = q.phone_valid;
    assign debug_scan_phase = q.scan_phase;
    assign debug_held_values = {q.fric2[0], q.fric1[0], q.u20[0], q.pw5[0], q.pw3[0], q.pw2[0], q.pw1[0], q.pw0[0]};
    assign debug_held_known = {q.fric2[1], q.fric1[1], q.u20[1], q.pw5[1], q.pw3[1], q.pw2[1], q.pw1[1], q.pw0[1]};
    assign debug_dda_a = q.a;
    assign debug_dda_b = q.b;
    assign debug_dda_c = q.c;
    assign debug_dda_target = q.target;
    assign debug_dda_up = q.upward;
    assign debug_duration_left = q.duration_left;
    assign debug_articulation_left = q.articulation_left;
    assign debug_amplitude_left = q.amplitude_left;
    assign debug_filter_left = q.filter_left;
    assign debug_filter_frequency = q.regs[4];
    assign debug_registers = q.regs;
    assign debug_windows = {q.duration_window, q.duration_pending,
                           q.amplitude_window, q.amplitude_pending,
                           q.articulation_window, q.articulation_pending,
                           q.control_window, q.control_pending,
                           q.phone_window, q.phone_pending};

    // synthesis translate_off
    initial begin
        if (ART_REFERENCE_RATE < 0 || ART_REFERENCE_RATE > 15)
            $fatal(1, "SSI native controller: ART_REFERENCE_RATE must be 0..15");
    end
    // synthesis translate_on
endmodule
