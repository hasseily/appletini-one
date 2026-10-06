`timescale 1ns / 1ps

module tb_ssi263_control_core;
    logic clk = 0;
    logic cold_resetn = 0;
    logic [1:0] phone_write = 0;
    logic [5:0] phone_data [0:1];
    logic [2:0] selector = 0;
    logic selector_write_ce = 0;
    logic [3:0] host_amplitude = 0, duration_phase = 0;
    logic latched_ctrl = 0, ampct_zero = 0;
    logic va_zero = 1, fa_zero = 1, u62_q_n = 0, d3_or_d4 = 0;
    logic phi1 = 0, phi0_rise = 0;
    logic normal_transition_ce = 0, amplitude_rate_ce = 0;
    logic xck_ce = 0, div2 = 1;
    logic [11:0] active_inflection = 4095;
    logic [7:0] filter_frequency = 255;
    logic pitch_run = 0, pitch_reload = 0, filter_run = 0, filter_reload = 0;
    wire [1:0] effective_ce, pitch_ce, filter_ce, phone_valid, target_valid;
    wire [1:0] permit, permit_known;
    wire [5:0] phone [0:1];
    wire [7:0] parameter_byte [0:1], state [0:1], known [0:1];
    wire [3:0] target [0:1], tparm [0:1], terms [0:1], terms_known [0:1];
    logic [7:0] rom [0:511];
    integer checks = 0;
    integer contract_case = 0;
    integer pitch_count, filter_count, effective_count;
    logic [7:0] before_state [0:1], before_known [0:1];

    always #5 clk = ~clk;

    for (genvar socket = 0; socket < 2; socket++) begin : sockets
        ssi263_control_core dut (
            .clk(clk), .cold_resetn(cold_resetn),
            .phone_write(phone_write[socket]), .phone_data(phone_data[socket]),
            .selector(selector), .selector_write_ce(selector_write_ce),
            .host_amplitude(host_amplitude), .duration_phase(duration_phase),
            .latched_ctrl(latched_ctrl), .ampct_zero(ampct_zero),
            .voice_amplitude_zero(va_zero), .fricative_amplitude_zero(fa_zero),
            .u62_q_n(u62_q_n), .d3_or_d4(d3_or_d4),
            .phi1(phi1), .phi0_rise(phi0_rise),
            .normal_transition_ce(normal_transition_ce), .amplitude_rate_ce(amplitude_rate_ce),
            .xck_ce(xck_ce), .div2(div2), .active_inflection(active_inflection),
            .filter_frequency(filter_frequency), .pitch_run(pitch_run),
            .pitch_reload(pitch_reload), .filter_run(filter_run), .filter_reload(filter_reload),
            .effective_xck_ce(effective_ce[socket]), .pitch_cycle_ce(pitch_ce[socket]),
            .filter_cycle_ce(filter_ce[socket]),
            .phone(phone[socket]), .phone_valid(phone_valid[socket]),
            .parameter_byte(parameter_byte[socket]), .target(target[socket]),
            .tparm(tparm[socket]), .target_valid(target_valid[socket]),
            .transition_permit(permit[socket]), .transition_permit_known(permit_known[socket]),
            .pw0(state[socket][0]), .pw1(state[socket][1]), .pw2(state[socket][2]),
            .pw3(state[socket][3]), .pw5(state[socket][4]), .u20(state[socket][5]),
            .fric1_sw(state[socket][6]), .fric2_sw(state[socket][7]),
            .u104c(terms[socket][0]), .ampct0(terms[socket][1]),
            .fricative(terms[socket][2]), .u32b(terms[socket][3]),
            .source_state_known(known[socket]), .source_logic_known(terms_known[socket])
        );
    end

    task automatic cycle;
        @(posedge clk); #1;
        @(negedge clk); #1;
    endtask

    task automatic check(input logic condition, input string label);
        checks++;
        if (condition !== 1'b1) $fatal(1, "SSI263 CONTROL FAIL: %s", label);
    endtask

    task automatic phones(input logic [5:0] left, input logic [5:0] right);
        phone_data[0] = left;
        phone_data[1] = right;
        phone_write = 3;
        cycle();
        phone_write = 0;
        #1;
    endtask

    task automatic scan(input logic [2:0] slot, input logic [3:0] phase);
        selector = slot;
        duration_phase = phase;
        selector_write_ce = 1;
        cycle();
        selector_write_ce = 0;
        #1;
    endtask

    task automatic compare_permit(input integer slot, input logic opportunity,
                                  input logic expected, input logic expected_known);
        selector = slot;
        selector_write_ce = 1;
        normal_transition_ce = opportunity;
        amplitude_rate_ce = opportunity;
        #1;
        check(permit === {2{expected}} && permit_known === {2{expected_known}},
              "native transition permission");
        selector_write_ce = 0;
        normal_transition_ce = 0;
        amplitude_rate_ce = 0;
        #1;
    endtask

    initial begin
        $readmemh("ssi263_sc02_rom.mem", rom);
        if ($test$plusargs("scan_before_phone")) contract_case = 1;
        if ($test$plusargs("phone_scan_overlap")) contract_case = 2;
        phone_data[0] = 0;
        phone_data[1] = 0;
        cycle();
        check(phone_valid === 0 && target_valid === 0, "cold ROM target invalid");
        check(known[0] === 0 && known[1] === 0, "cold source state unproved");
        check(!terms_known[0][3] && !terms_known[1][3], "cold TPHO5 not claimed known");
        cold_resetn = 1;
        cycle();
        check(!terms_known[0][3] && !terms_known[1][3], "unwritten phone still unknown");
        if (contract_case == 1) begin
            selector_write_ce = 1;
            cycle();
            $fatal(1, "missing pre-phone scan rejection");
        end
        phones(0, 63);
        if (contract_case == 2) begin
            phone_write = 1;
            selector_write_ce = 1;
            cycle();
            $fatal(1, "missing overlapping phone/selector rejection");
        end

        // All 512 bytes, both socket address orders, native target fields and
        // live host-amplitude substitution. Lookup alone must not mutate PW.
        for (integer p = 0; p < 64; p++) begin
            phones(p, 63 - p);
            for (integer s = 0; s < 8; s++) begin
                selector = s;
                host_amplitude = (p + s) & 15;
                #1;
                for (integer chip = 0; chip < 2; chip++) begin
                    check(parameter_byte[chip] === rom[{phone[chip], selector}], "native ROM byte");
                    check(tparm[chip] === parameter_byte[chip][3:0], "TPARM separate from target");
                    check(target[chip] === (s == 4 ? host_amplitude : parameter_byte[chip][7:4]),
                          "native target or host amplitude");
                    check(target_valid[chip] === (s != 7), "selector7 has no parameter write");
                    check(state[chip][1:0] === 0 && known[chip][1:0] === 3,
                          "ROM lookup cannot set phone timing latches");
                end
            end
        end

        // HF/HFC differ through PW3 despite equal upper-nibble ROM targets.
        phones(6'h2c, 6'h2d);
        scan(0, 2); // Both selector0 bytes are71: PW0 sets at2/16.
        scan(1, 6); // Both selector1 bytes are90: PW1 sets at6/16.
        latched_ctrl = 0;
        scan(2, 6);
        u62_q_n = 1;
        d3_or_d4 = 0;
        #1;
        check(known[0][3] && known[1][3], "both PW3 states established");
        check(!state[0][3] && state[1][3], "HF follows CTRL; HFC forces PW3");
        check(terms_known[0][0] && terms_known[1][0] && !terms[0][0] && terms[1][0],
              "HF/HFC source gate differs");
        check(terms[0][2] && !terms[1][2], "HF/HFC fricative gate differs");
        // Both phones have TPARM2=0 and TPHO5=1: held PW5 must block
        // the formant transition even when its normal timing pulse arrives.
        for (integer s = 0; s < 4; s++) if (s != 2) begin
            compare_permit(s, 1, 0, 1);
            compare_permit(s, 0, 0, 1);
        end

        // Changing CTRL between selector2 edges does not rewrite held PW3.
        latched_ctrl = 1;
        cycle();
        check(!state[0][3] && state[1][3], "held CTRL is not a live source mux");
        scan(2, 6);
        check(state[0][3] && state[1][3], "qualified CTRL update reaches HF");

        // Only the addressed socket's PW0/PW1 clear on its phone write.
        for (integer chip = 0; chip < 2; chip++) begin
            before_state[chip] = state[chip];
            before_known[chip] = known[chip];
        end
        phone_data[0] = 1;
        phone_write = 1;
        cycle();
        phone_write = 0;
        check(state[0][1:0] === 0 && state[0][7:2] === before_state[0][7:2],
              "phone write retains PW2/3/5 and routes");
        check(known[0][7:2] === before_known[0][7:2], "phone write retains evidence flags");
        check(state[1] === before_state[1] && known[1] === before_known[1], "other socket retains state");

        // Gate qualified native transitions; never substitute an invented rule
        // for selectors2/4. A zero opportunity proves no write independently
        // of held-state knowledge.
        phones(6'h0e, 6'h0e);
        compare_permit(5, 1, 0, 1);
        compare_permit(6, 1, 0, 1);
        selector = 0; #1;
        scan(0, tparm[0][0] ? 2 : 6);
        selector = 1; #1;
        scan(1, tparm[0][0] ? 2 : 6);
        compare_permit(5, 1, 1, 1);
        compare_permit(6, 1, 1, 1);
        compare_permit(5, 0, 0, 1);
        compare_permit(2, 1, 0, 0);
        compare_permit(4, 1, 0, 0);
        compare_permit(7, 1, 0, 1);
        scan(2, 6); // AH selector2 hasTPARM2=1; clears PW5.
        check(!state[0][4] && !state[1][4], "native AH permits formant transitions");
        for (integer s = 0; s < 4; s++) if (s != 2) begin
            compare_permit(s, 1, 1, 1);
            compare_permit(s, 0, 0, 1);
        end

        // Follow actual native ROM requests through both held routes. S has
        // TPARM3=0 while AH has1. U112 follows duringPhi1; U166 changes only
        // atPhi0, including the intermediate both-off/both-on states.
        phi1 = 1;
        cycle();
        check(state[0][6] && state[1][6], "AH route reaches transparent FRIC1");
        phi1 = 0;
        phi0_rise = 1;
        cycle();
        phi0_rise = 0;
        check(state[0][7:6] === 2'b01 && state[1][7:6] === 2'b01,
              "AH routes settle independently");
        phones(6'h30, 6'h30);
        check(state[0][7:6] === 2'b01, "new phone does not apply live route bit");
        selector = 1; #1;
        scan(1, tparm[0][0] ? 2 : 6);
        phi1 = 1;
        scan(2, 6);
        check(known[0][7:5] === 7 && state[0][7:6] === 0 && state[1][7:6] === 0,
              "new U20 passes openPhi1 whileFRIC2holds: both routes off");
        phi1 = 0;
        phi0_rise = 1;
        cycle();
        phi0_rise = 0;
        check(state[0][7:6] === 2'b10 && state[1][7:6] === 2'b10,
              "S settles to late noise route");
        phones(6'h0e, 6'h0e);
        selector = 1; #1;
        scan(1, tparm[0][0] ? 2 : 6);
        phi1 = 1;
        scan(2, 6);
        check(state[0][7:6] === 3 && state[1][7:6] === 3,
              "return route change leaves both routes on untilPhi0");
        phi1 = 0;
        phi0_rise = 1;
        cycle();
        phi0_rise = 0;
        check(state[0][7:6] === 1 && state[1][7:6] === 1, "AH route restore");

        // Divider periods are independent of phone/PW changes and selector
        // events. Both cores see exact8/2effective-clock periods withDIV2.
        pitch_run = 1;
        filter_run = 1;
        effective_count = 0;
        pitch_count = 0;
        filter_count = 0;
        for (integer raw = 1; raw <= 64; raw++) begin
            xck_ce = 1;
            if (raw == 5) phone_write = 1;
            else phone_write = 0;
            cycle();
            check(effective_ce === {2{(raw % 2) == 0}}, "native DIV2 cadence");
            check(pitch_ce === {2{(raw % 16) == 0}}, "native pitch cadence");
            check(filter_ce === {2{(raw % 4) == 0}}, "native FF255 cadence");
            effective_count += effective_ce[0];
            pitch_count += pitch_ce[0];
            filter_count += filter_ce[0];
        end
        xck_ce = 0;
        check(effective_count == 32 && pitch_count == 4 && filter_count == 16,
              "independent native clock counts");
        $display("SSI263 CONTROL CORE PASS checks=%0d bytes=512 sockets=2", checks);
        $finish;
    end
endmodule
