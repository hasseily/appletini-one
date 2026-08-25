`timescale 1ns / 1ps

// Native SSI-263A / SC-02 digital control core.
//
// The system clock only stores state. xck_ce marks rising edges at the chip's
// XCK pin. DIV2 is kept inside this core so tests can distinguish the pin clock
// from the effective SSI time base.
module ssi263_sc02_core #(
    parameter bit REVISION_AP = 1'b1,
    parameter ROM_FILE = "ssi263_sc02_rom.mem"
) (
    input  logic        clk,
    input  logic        rstn,

    input  logic        pd_rst_n,
    input  logic        xck_ce,
    input  logic        div2,

    // This is the complete selected-write condition, not a pulse. Address and
    // data latch when it falls, as on the SSI-263 bus.
    input  logic        write_active,
    input  logic [2:0]  write_reg,
    input  logic [7:0]  write_data,

    output logic        d7_pending,
    output logic        ar_drive_low,
    output logic        powered_down,
    output logic        phone_active,
    output logic        ar_enabled,
    output logic        response_phoneme,
    output logic        transitioned_pitch,

    output logic [5:0]  phoneme,
    output logic [1:0]  duration,
    output logic [3:0]  rate,
    output logic [11:0] inflection,
    output logic [11:0] pitch_inflection,
    output logic [7:0]  transitioned_inflection_state,
    output logic [2:0]  articulation,
    output logic [3:0]  amplitude,
    output logic [7:0]  filter_frequency,

    output logic        effective_xck_ce,
    output logic        response_boundary_ce,
    output logic        voice_clock_ce,
    output logic        voice_toggle,
    output logic        pitch_period_ce,
    output logic        noise_clock_ce,
    output logic        noise_shift_ce,
    output logic        filter_phase_ce,
    output logic        filter_phase,

    // The SC-02 scans eight ROM columns. U45 Q1 and Q2 form the WRITE/LATCH
    // timing phases; Q3/Q4/Q5 drive SEL0/SEL1/SEL2.
    output logic [2:0]  selector,
    output logic        selector_phase,
    output logic        selector_step_ce,
    output logic [7:0]  selector_rom_data,
    output logic [3:0]  selector_flags,

    // Sheet 5 stores the low-ROM control bits while selectors 0, 1, and 2
    // pass. Sheet 6 and the audio sheets consume these held controls.
    output logic        pw_0,
    output logic        pw_1,
    output logic        pw_2,
    output logic        pw_3,
    output logic        pw_5,
    output logic        fric1_sw,
    output logic        fric2_sw,
    output logic        closure,

    // These pulses expose the sheet-3/4/6 control state for the audio core
    // and for cycle checks. parameter_write_selector names the RAM slot.
    output logic        rate_clock_ce,
    output logic        rate_clock_div2_ce,
    output logic        articulation_step_ce,
    output logic        inflection_step_ce,
    output logic        parameter_write_ce,
    output logic [2:0]  parameter_write_selector,

    // Persistent four-bit control values consumed by the audio section.
    // F3 and F4 use the same state-3 value but separate analog sections.
    output logic [3:0]  f1_code,
    output logic [3:0]  f2_code,
    output logic [3:0]  f2_res_code,
    output logic [3:0]  f3_code,
    output logic [3:0]  f4_code,
    output logic [3:0]  filter_amp_code,
    output logic [3:0]  voice_amp_code,
    output logic [3:0]  fric_amp_code
);

    localparam logic [1:0] MODE_FRAME_IMMEDIATE       = 2'b01;
    localparam logic [1:0] MODE_PHONEME_IMMEDIATE     = 2'b10;
    localparam logic [1:0] MODE_PHONEME_TRANSITIONED  = 2'b11;

    logic [7:0] rom_q [0:511];

    logic [7:0] duration_phoneme_q;
    logic [7:0] inflection_high_q;
    logic [7:0] rate_inflection_q;
    logic [7:0] control_articulation_amplitude_q;
    logic [7:0] filter_frequency_q;

    logic       pending_q;
    logic       phone_active_q;
    logic       ar_enabled_q;
    logic       response_phoneme_q;
    logic       transitioned_pitch_q;

    logic       write_active_q;
    logic [2:0] write_reg_hold_q;
    logic [7:0] write_data_hold_q;

    logic       pd_rst_n_q;
    logic       div2_q;
    logic       div2_phase_q;

    logic [16:0] frame_ticks_left_q;
    logic [2:0]  frames_left_q;
    logic [15:0] voice_clock_ticks_left_q;
    logic [8:0]  filter_ticks_left_q;
    // Sheet 6 amplitude counter and glottal divider.
    logic        u62_q;
    logic        u41c_level_q;
    logic [3:0]  ampct_q;
    logic        u68_clock_level_q;
    logic        u20b_q;

    logic [4:0] rate_edges_left_q;
    logic       rate_clock_q;
    logic       rate_clock_div2_q;
    logic [3:0] articulation_edges_left_q;
    logic [3:0] inflection_edges_left_q;
    logic [7:0] transitioned_inflection_q;
    logic [6:0] parameter_sweep_q;

    logic [1:0] slow_div_q;
    logic       selector_phase_q;
    logic       selector_latch_phase_q;
    logic [2:0] selector_q;

    // Sheet 4 U89 RESA stores one transition result per selector. Sheets 7
    // U106-U114 then latch the selected slot, and U170-U176 form the separate
    // filter-phase latch layer consumed by the analog switches.
    logic [3:0] parameter_resa_q [0:6];
    logic [3:0] f1_first_q;
    logic [3:0] f2_first_q;
    logic [3:0] f2_res_first_q;
    logic [3:0] f3_first_q;
    logic [3:0] f4_first_q;
    logic [3:0] filter_amp_first_q;
    logic [3:0] voice_amp_first_q;
    logic [3:0] fric_amp_first_q;

    logic [3:0] f1_code_q;
    logic [3:0] f2_code_q;
    logic [3:0] f2_res_code_q;
    logic [3:0] f3_code_q;
    logic [3:0] f4_code_q;
    logic [3:0] filter_amp_code_q;
    logic [3:0] voice_amp_code_q;
    logic [3:0] fric_amp_code_q;

    logic pw_0_q;
    logic pw_1_q;
    logic pw_2_q;
    logic pw_3_q;
    logic pw_5_q;
    logic fric1_sw_q;
    logic fric2_sw_q;

    logic       response_boundary_ce_q;
    logic       voice_clock_ce_q;
    logic       pitch_period_ce_q;
    logic       noise_clock_ce_q;
    logic       noise_shift_ce_q;
    logic       filter_phase_ce_q;
    logic       filter_phase_q;
    logic       selector_step_ce_q;
    logic       rate_clock_ce_q;
    logic       rate_clock_div2_ce_q;
    logic       articulation_step_ce_q;
    logic       inflection_step_ce_q;
    logic       parameter_write_ce_q;
    logic [2:0] parameter_write_selector_q;

    logic [8:0] rom_address;
    logic [3:0] selector_target;
    logic [7:0] transitioned_inflection_target;
    logic [11:0] transitioned_inflection_word;
    logic       write_end;
    logic       write_commit;
    logic       u41c_level;
    logic       u104c;
    logic       ampct0;
    logic       ampct_zero;
    logic       ampct_enable;
    logic       ampct_up;
    logic       ampct_nco;
    logic       u68_clock_level;
    logic       u62_reset;
    logic       u20_clock_enable;
    logic       selector_write_rise;
    logic       selector_write_level;
    logic       selector_latch_rise;
    logic       selector_latch_level;
    logic [3:0] filter_amp_masked;
    logic [3:0] u37_q;
    logic       u183a_q;
    logic       pho_write_low;
    logic       duration_clock_rise;
    logic       u29a_level;
    logic       u29a_level_q;
    logic       u38_equal;
    logic       pw0_set_level;
    logic       pw1_set_level;
    logic       pw3_load_level;

    initial begin
        $readmemh(ROM_FILE, rom_q);
    end

    function automatic logic [16:0] frame_tick_count(input logic [3:0] value);
        logic [4:0] factor;
        begin
            factor = 5'd16 - {1'b0, value};
            frame_tick_count = {factor, 12'b0};
        end
    endfunction

    function automatic logic [2:0] boundary_frame_count(
        input logic       use_phoneme_timing,
        input logic [1:0] duration_value
    );
        begin
            if (use_phoneme_timing) begin
                boundary_frame_count = 3'd4 - {1'b0, duration_value};
            end else begin
                boundary_frame_count = 3'd1;
            end
        end
    endfunction

    function automatic logic [15:0] voice_clock_tick_count(
        input logic [11:0] value
    );
        logic [12:0] delta;
        begin
            delta = 13'd4096 - {1'b0, value};
            voice_clock_tick_count = {1'b0, delta, 2'b0};
        end
    endfunction

    function automatic logic [8:0] filter_half_tick_count(
        input logic [7:0] value
    );
        begin
            filter_half_tick_count = 9'd256 - {1'b0, value};
        end
    endfunction

    function automatic logic [3:0] move_one_toward(
        input logic [3:0] value,
        input logic [3:0] target
    );
        begin
            if (value < target) begin
                move_one_toward = value + 4'd1;
            end else if (value > target) begin
                move_one_toward = value - 4'd1;
            end else begin
                move_one_toward = value;
            end
        end
    endfunction

    function automatic logic [7:0] move_one_toward_8(
        input logic [7:0] value,
        input logic [7:0] target
    );
        begin
            if (value < target) begin
                move_one_toward_8 = value + 8'd1;
            end else if (value > target) begin
                move_one_toward_8 = value - 8'd1;
            end else begin
                move_one_toward_8 = value;
            end
        end
    endfunction

    function automatic logic [4:0] rate_edge_count(input logic [3:0] value);
        begin
            rate_edge_count = 5'd16 - {1'b0, value};
        end
    endfunction

    function automatic logic [3:0] movement_edge_count(
        input logic [2:0] value
    );
        begin
            movement_edge_count = 4'd8 - {1'b0, value};
        end
    endfunction

    assign phoneme = duration_phoneme_q[5:0];
    assign duration = duration_phoneme_q[7:6];
    assign rate = rate_inflection_q[7:4];
    assign inflection = {
        rate_inflection_q[3],
        inflection_high_q,
        rate_inflection_q[2:0]
    };
    // In transitioned mode U65/U64 replace I10:I3 with their eight-bit
    // up/down state. I11 and I2:I0 always remain direct register bits.
    assign transitioned_inflection_target = {
        inflection_high_q[7:3], 3'b000
    };
    assign transitioned_inflection_word = {
        rate_inflection_q[3],
        transitioned_inflection_q,
        rate_inflection_q[2:0]
    };
    assign transitioned_inflection_state = transitioned_inflection_q;
    assign pitch_inflection = transitioned_pitch_q ?
                              transitioned_inflection_word : inflection;
    assign articulation = control_articulation_amplitude_q[6:4];
    assign amplitude = control_articulation_amplitude_q[3:0];
    assign filter_frequency = filter_frequency_q;

    assign d7_pending = pending_q;
    assign phone_active = phone_active_q;
    assign ar_enabled = ar_enabled_q;
    assign response_phoneme = response_phoneme_q;
    assign transitioned_pitch = transitioned_pitch_q;
    assign powered_down = control_articulation_amplitude_q[7] ||
                          (REVISION_AP && !pd_rst_n);
    assign ar_drive_low = pending_q && ar_enabled_q && !powered_down;

    assign effective_xck_ce = xck_ce &&
                              ((div2 == div2_q) || !rstn) &&
                              (!div2 || div2_phase_q);
    assign response_boundary_ce = response_boundary_ce_q;
    assign voice_clock_ce = voice_clock_ce_q;
    assign voice_toggle = u62_q;
    assign pitch_period_ce = pitch_period_ce_q;
    assign noise_clock_ce = noise_clock_ce_q;
    assign noise_shift_ce = noise_shift_ce_q;
    assign filter_phase_ce = filter_phase_ce_q;
    assign filter_phase = filter_phase_q;

    assign selector = selector_q;
    assign selector_phase = selector_phase_q;
    assign selector_step_ce = selector_step_ce_q;
    assign rom_address = {duration_phoneme_q[5:0], selector_q};
    assign selector_rom_data = rom_q[rom_address];
    assign selector_flags = selector_rom_data[3:0];
    // U79B/U79C and U78A-D clear the voice/fricative targets when AMPZERO is
    // true. Selector four takes its target directly from the host amplitude.
    assign selector_target = (selector_q == 3'd4) ?
                             control_articulation_amplitude_q[3:0] :
                             (((selector_q == 3'd5 || selector_q == 3'd6) &&
                               control_articulation_amplitude_q[3:0] == 4'd0) ?
                              4'd0 : selector_rom_data[7:4]);

    assign f1_code = f1_code_q;
    assign f2_code = f2_code_q;
    assign f2_res_code = f2_res_code_q;
    assign f3_code = f3_code_q;
    assign f4_code = f4_code_q;
    assign filter_amp_code = filter_amp_code_q;
    assign voice_amp_code = voice_amp_code_q;
    assign fric_amp_code = fric_amp_code_q;

    assign pw_0 = pw_0_q;
    assign pw_1 = pw_1_q;
    assign pw_2 = pw_2_q;
    assign pw_3 = pw_3_q;
    assign pw_5 = pw_5_q;
    assign fric1_sw = fric1_sw_q;
    assign fric2_sw = fric2_sw_q;
    // Sheet 3 U52C is U49 TC AND U43B Q. The model toggles filter_phase_q at
    // the end of each TC interval, so !filter_phase_q names the Q-high
    // interval that just ended. This is a timing pulse, not a phone closure.
    assign closure = filter_phase_ce_q && !filter_phase_q;

    assign rate_clock_ce = rate_clock_ce_q;
    assign rate_clock_div2_ce = rate_clock_div2_ce_q;
    assign articulation_step_ce = articulation_step_ce_q;
    assign inflection_step_ce = inflection_step_ce_q;
    assign parameter_write_ce = parameter_write_ce_q;
    assign parameter_write_selector = parameter_write_selector_q;

    assign write_end = write_active_q && !write_active;
    // AP PD/RST owns the whole edge. The faulty P revision ignores PD/RST,
    // including when it collides with the falling edge of a host write.
    assign write_commit = write_end && (pd_rst_n || !REVISION_AP);
    // Sheet 6: U104C.8 is U62 /Q (also tied to U62 D), not U41C
    // feedback. U72A inverts U104C, U42D is NOR(SEL1, FRIC_AMP_ZERO),
    // and U41C ANDs those two terms before clocking U75 and U73.
    assign u41c_level = !(pw_3_q && !u62_q) &&
                        !selector_q[1] &&
                        (fric_amp_code_q != 4'd0);

    // Sheet 6 U68/U85 amplitude counter. U68 pin 9 (B/D) is tied to the
    // VCC loop, so the CD4029 runs in binary mode. Carry-in and preset-enable
    // are low and all jam inputs are grounded.
    // Q1 is not used; Q2..Q4 form AMPCT1..3. /CO is low only at the binary
    // terminal selected by U/D. U85C resets U62 everywhere except the
    // terminal phase allowed by U104C.
    assign u104c = pw_3_q && !u62_q;
    assign ampct0 = !u104c;
    assign ampct_zero = !(ampct_q[1] | ampct_q[2] | ampct_q[3]);
    assign ampct_enable = !(voice_amp_code_q == 4'd0 &&
                            fric_amp_code_q == 4'd0);
    assign ampct_up = ampct0 && ampct_enable;
    assign ampct_nco = ampct_up ? (ampct_q != 4'd15) :
                                  (ampct_q != 4'd0);
    assign u68_clock_level = selector_q[2] &&
                             !((!ampct_nco && ampct_up) ||
                               (!ampct_up && ampct_zero));
    assign u62_reset = u104c || ampct_nco;

    // Sheet 7 U20B samples TPARM3 only on the gated WR_SEL2 edge. WR_SEL2
    // also updates the sheet-5 PW2 latch: old PW2=1 passes the early edge,
    // while TPARM2=1 opens the gate later in the same slot. Their OR is the
    // exact settled-slot condition for whether U20 receives a rising edge.
    assign u20_clock_enable =
        ((pw_0_q && pw_1_q && ampct_zero) ||
         (fric_amp_code_q == 4'd0)) &&
        pw_1_q &&
        (pw_2_q || ((selector_q == 3'd2) && selector_flags[2]));

    // With r={Q2,Q1,U44.QB,U44.QA}, WRITE rises at r=2 and LATCH rises at
    // r=10. The level term covers r=10 and r=11, when the CD4042 first stage
    // is transparent.
    assign selector_write_rise =
        (slow_div_q == 2'd1) && !selector_phase_q &&
        !selector_latch_phase_q;
    assign selector_write_level = slow_div_q[1] &&
                                  !selector_phase_q &&
                                  !selector_latch_phase_q;
    assign selector_latch_rise =
        (slow_div_q == 2'd1) && !selector_phase_q &&
        selector_latch_phase_q;
    assign selector_latch_level = slow_div_q[1] &&
                                  !selector_phase_q &&
                                  selector_latch_phase_q;

    // U70A-D gate U111 with AMPCT. AMPCT0 is /U104C; AMPCT1..3 are U68
    // Q2..Q4. U206 captures this bus only on the positive Phi0 edge.
    assign filter_amp_masked = {
        filter_amp_first_q[3] && ampct_q[3],
        filter_amp_first_q[2] && ampct_q[2],
        filter_amp_first_q[1] && ampct_q[1],
        filter_amp_first_q[0] && ampct0
    };

    // Sheet 5 U37/U38. U37 advances on the falling edge of
    // U29A=NOT(DURCLK OR /PHO_WRITE): once when a phoneme write starts and once
    // at a duration boundary. U38 compares the inverted low nibble against
    // {1,TPARM0,0,1}, which reduces to q=2 or q=6.
    assign pho_write_low = write_active && write_reg == 3'd0;
    assign duration_clock_rise = effective_xck_ce && phone_active_q &&
                                 !powered_down &&
                                 frame_ticks_left_q == 17'd1 &&
                                 frames_left_q == 3'd1;
    assign u29a_level = !(duration_clock_rise || pho_write_low);
    assign u38_equal = (u37_q == (selector_flags[0] ? 4'd2 : 4'd6));
    assign pw0_set_level = selector_latch_level && selector_q == 3'd0 &&
                           u38_equal;
    assign pw1_set_level = selector_latch_level && selector_q == 3'd1 &&
                           u38_equal;
    assign pw3_load_level = selector_latch_level && selector_q == 3'd2 &&
                            u38_equal;

    always_ff @(posedge clk) begin
        response_boundary_ce_q <= 1'b0;
        voice_clock_ce_q <= 1'b0;
        pitch_period_ce_q <= 1'b0;
        noise_clock_ce_q <= 1'b0;
        noise_shift_ce_q <= 1'b0;
        filter_phase_ce_q <= 1'b0;
        selector_step_ce_q <= 1'b0;
        rate_clock_ce_q <= 1'b0;
        rate_clock_div2_ce_q <= 1'b0;
        articulation_step_ce_q <= 1'b0;
        inflection_step_ce_q <= 1'b0;
        parameter_write_ce_q <= 1'b0;

        if (!rstn) begin
            duration_phoneme_q <= 8'hC0;
            inflection_high_q <= 8'h00;
            rate_inflection_q <= 8'h00;
            control_articulation_amplitude_q <= 8'h80;
            filter_frequency_q <= 8'hFF;

            pending_q <= 1'b0;
            phone_active_q <= 1'b0;
            ar_enabled_q <= 1'b0;
            response_phoneme_q <= 1'b1;
            transitioned_pitch_q <= 1'b1;

            write_active_q <= 1'b0;
            write_reg_hold_q <= 3'd0;
            write_data_hold_q <= 8'd0;

            pd_rst_n_q <= 1'b1;
            div2_q <= div2;
            div2_phase_q <= 1'b0;

            frame_ticks_left_q <= frame_tick_count(4'h0);
            frames_left_q <= 3'd1;
            voice_clock_ticks_left_q <= voice_clock_tick_count(12'h000);
            filter_ticks_left_q <= filter_half_tick_count(8'hFF);
            u62_q <= 1'b0;
            u41c_level_q <= 1'b0;
            ampct_q <= 4'd0;
            u68_clock_level_q <= 1'b0;
            u20b_q <= 1'b0;

            rate_edges_left_q <= rate_edge_count(4'h0);
            rate_clock_q <= 1'b0;
            rate_clock_div2_q <= 1'b0;
            articulation_edges_left_q <= movement_edge_count(3'h0);
            inflection_edges_left_q <= movement_edge_count(3'h0);
            transitioned_inflection_q <= 8'h00;
            parameter_sweep_q <= 7'h00;

            slow_div_q <= 2'd0;
            selector_phase_q <= 1'b0;
            selector_latch_phase_q <= 1'b0;
            selector_q <= 3'd0;

            parameter_resa_q[0] <= 4'd0;
            parameter_resa_q[1] <= 4'd0;
            parameter_resa_q[2] <= 4'd0;
            parameter_resa_q[3] <= 4'd0;
            parameter_resa_q[4] <= 4'd0;
            parameter_resa_q[5] <= 4'd0;
            parameter_resa_q[6] <= 4'd0;
            f1_first_q <= 4'd0;
            f2_first_q <= 4'd0;
            f2_res_first_q <= 4'd0;
            f3_first_q <= 4'd0;
            f4_first_q <= 4'd0;
            filter_amp_first_q <= 4'd0;
            voice_amp_first_q <= 4'd0;
            fric_amp_first_q <= 4'd0;

            f1_code_q <= 4'd0;
            f2_code_q <= 4'd0;
            f2_res_code_q <= 4'd0;
            f3_code_q <= 4'd0;
            f4_code_q <= 4'd0;
            filter_amp_code_q <= 4'd0;
            voice_amp_code_q <= 4'd0;
            fric_amp_code_q <= 4'd0;

            pw_0_q <= 1'b0;
            pw_1_q <= 1'b0;
            pw_2_q <= 1'b0;
            pw_3_q <= 1'b0;
            pw_5_q <= 1'b1;
            fric1_sw_q <= 1'b0;
            fric2_sw_q <= 1'b1;
            u37_q <= 4'd0;
            u183a_q <= 1'b1;
            u29a_level_q <= 1'b1;

            parameter_write_selector_q <= 3'd0;

            filter_phase_q <= 1'b0;
        end else begin
            write_active_q <= write_active;
            pd_rst_n_q <= pd_rst_n;
            div2_q <= div2;

            // U183A is set by T/PD /RST and otherwise samples CTRL on the
            // falling FASTCLK edge. One effective-edge sample preserves the
            // drawn stable value used by the later slot-2 latch.
            if (!pd_rst_n)
                u183a_q <= 1'b1;
            else if (effective_xck_ce)
                u183a_q <= control_articulation_amplitude_q[7];

            // U37 is a negative-edge CD4040. Retain U29A's full level so an
            // overlapping phone-write/DURCLK condition still creates only
            // one falling edge.
            if (u29a_level_q && !u29a_level)
                u37_q <= u37_q + 4'd1;
            else if (pending_q && u37_q == 4'd0)
                u37_q <= 4'd0;
            u29a_level_q <= u29a_level;

            // U32/U33 are set-only cross-NAND latches. /PHO_WRITE low clears
            // them unless the selected comparator set path is active. U34
            // has no phone-write clear; it loads the CTRL/TPARM1 result only
            // while the slot-2 comparator path is active.
            if (pw0_set_level)
                pw_0_q <= 1'b1;
            else if (pho_write_low)
                pw_0_q <= 1'b0;
            if (pw1_set_level)
                pw_1_q <= 1'b1;
            else if (pho_write_low)
                pw_1_q <= 1'b0;
            if (pw3_load_level)
                pw_3_q <= u183a_q || !selector_flags[1];

            // Preserve the positive edges of the gated U41C waveform as
            // one-clk enables. Its sources change only in this clock domain.
            noise_clock_ce_q <= u41c_level && !u41c_level_q;
            noise_shift_ce_q <= !u41c_level && u41c_level_q;
            u41c_level_q <= u41c_level;

            // U68 counts on each positive edge of its gated SEL2 clock.
            // The gate can itself open while SEL2 is high, so detect the
            // complete drawn clock level rather than only selector changes.
            if (u68_clock_level && !u68_clock_level_q) begin
                if (ampct_up)
                    ampct_q <= ampct_q + 4'd1;
                else
                    ampct_q <= ampct_q - 4'd1;
            end
            u68_clock_level_q <= u68_clock_level;

            // U106-U114 are transparent only in their selected LATCH window.
            // Selector 3 feeds two distinct first-stage latches for F3/F4.
            if (selector_latch_level) begin
                case (selector_q)
                    3'd0: f1_first_q <= parameter_resa_q[0];
                    3'd1: f2_first_q <= parameter_resa_q[1];
                    3'd2: f2_res_first_q <= parameter_resa_q[2];
                    3'd3: begin
                        f3_first_q <= parameter_resa_q[3];
                        f4_first_q <= parameter_resa_q[3];
                    end
                    3'd4: filter_amp_first_q <= parameter_resa_q[4];
                    3'd5: voice_amp_first_q <= parameter_resa_q[5];
                    3'd6: fric_amp_first_q <= parameter_resa_q[6];
                    default: begin
                    end
                endcase
            end

            // U170/U173/U175 follow their first stages only in Phi0_X.
            // U171/U172/U174/U176 and U112 follow only in Phi1_X. The phase
            // latch closes before the matching analog bank leaves ground.
            if (!filter_phase_q) begin
                f1_code_q <= f1_first_q;
                f3_code_q <= f3_first_q;
                voice_amp_code_q <= voice_amp_first_q;
            end else begin
                f2_code_q <= f2_first_q;
                f2_res_code_q <= f2_res_first_q;
                f4_code_q <= f4_first_q;
                fric_amp_code_q <= fric_amp_first_q;
                fric1_sw_q <= u20b_q;
            end

            // U85C drives U62's active-high asynchronous reset.
            if (u62_reset)
                u62_q <= 1'b0;

            if (write_active) begin
                write_reg_hold_q <= write_reg;
                write_data_hold_q <= write_data;
            end

            // DIV2 is normally strapped. Suppress a tick and set a known phase
            // if a test or build-time override changes it while running.
            if (div2 != div2_q) begin
                div2_phase_q <= 1'b0;
            end else if (xck_ce && div2) begin
                div2_phase_q <= ~div2_phase_q;
            end else if (!div2) begin
                div2_phase_q <= 1'b0;
            end

            if (effective_xck_ce) begin
                // U58/U59 have no phone-active or CTL/PD gate. Their raw
                // VOICECLK keeps running; U62 divides every rising edge.
                if (voice_clock_ticks_left_q == 16'd1) begin
                    voice_clock_ticks_left_q <= voice_clock_tick_count(
                        pitch_inflection
                    );
                    voice_clock_ce_q <= 1'b1;
                    if (!u62_reset)
                        u62_q <= ~u62_q;
                    if (!u62_reset && !u62_q)
                        pitch_period_ce_q <= 1'b1;
                end else begin
                    voice_clock_ticks_left_q <=
                        voice_clock_ticks_left_q - 16'd1;
                end

                // Filter divider state is not reset by an FF register write.
                // U48/U49 use the new parallel value at the next reload.
                if (filter_ticks_left_q == 9'd1) begin
                    filter_ticks_left_q <= filter_half_tick_count(
                        filter_frequency_q
                    );
                    filter_phase_q <= ~filter_phase_q;
                    filter_phase_ce_q <= 1'b1;
                    if (filter_phase_q) begin
                        // U206 is a positive-edge register clocked by Phi0,
                        // not a transparent phase latch.
                        filter_amp_code_q <= filter_amp_masked;
                        fric2_sw_q <= !u20b_q;
                    end
                end else begin
                    filter_ticks_left_q <= filter_ticks_left_q - 9'd1;
                end

                if (phone_active_q && !powered_down) begin
                    if (frame_ticks_left_q == 17'd1) begin
                        frame_ticks_left_q <= frame_tick_count(
                            rate_inflection_q[7:4]
                        );
                        if (frames_left_q == 3'd1) begin
                            frames_left_q <= boundary_frame_count(
                                response_phoneme_q,
                                duration_phoneme_q[7:6]
                            );
                            pending_q <= 1'b1;
                            response_boundary_ce_q <= 1'b1;
                        end else begin
                            frames_left_q <= frames_left_q - 3'd1;
                        end
                    end else begin
                        frame_ticks_left_q <= frame_ticks_left_q - 17'd1;
                    end
                end

                // U46/U47 WRITE rises at selector sub-tick 2. Sheet-4 RAM
                // work completes here, eight FASTCLK ticks before LATCH.
                if (selector_write_rise && selector_q <= 3'd6 &&
                    parameter_sweep_q[selector_q]) begin
                    parameter_sweep_q[selector_q] <= 1'b0;
                    if (!write_commit &&
                        parameter_resa_q[selector_q] != selector_target) begin
                        parameter_resa_q[selector_q] <= move_one_toward(
                            parameter_resa_q[selector_q], selector_target
                        );
                        parameter_write_ce_q <= 1'b1;
                        parameter_write_selector_q <= selector_q;
                    end
                end

                // U39 routes the LATCH pulse to the low-ROM control latches
                // only in slots zero through two. U20 sees old PW2 at the
                // leading edge and also sees a new high TPARM2 while the U31
                // data latch is transparent.
                if (selector_latch_rise && !write_commit) begin
                    case (selector_q)
                        3'd2: begin
                            if (u20_clock_enable)
                                u20b_q <= selector_flags[3];
                            pw_2_q <= selector_flags[2];
                            pw_5_q <= !selector_flags[2];
                        end
                        default: begin
                        end
                    endcase
                end

                // U44A/U44B divide FASTCLK by four. U45 Q1/Q2 drive the
                // WRITE/LATCH timing gates and Q3/Q4/Q5 are SEL0/SEL1/SEL2.
                // One selector slot therefore spans four SLOWCLK edges, or
                // sixteen effective FASTCLK ticks.
                if (slow_div_q == 2'd3) begin
                    slow_div_q <= 2'd0;
                    if (selector_phase_q) begin
                        selector_phase_q <= 1'b0;
                        if (selector_latch_phase_q) begin
                            selector_latch_phase_q <= 1'b0;
                            selector_q <= selector_q + 3'd1;
                            selector_step_ce_q <= 1'b1;

                            // SEL1 rises at the 1->2 and 5->6 boundaries. U27
                            // divides those edges by 16-R and U21 makes
                            // RATECLK.
                            if (selector_q == 3'd1 || selector_q == 3'd5) begin
                            if (rate_edges_left_q == 5'd1) begin
                                rate_edges_left_q <= rate_edge_count(
                                    rate_inflection_q[7:4]
                                );
                                rate_clock_q <= !rate_clock_q;

                                if (!rate_clock_q) begin
                                    rate_clock_ce_q <= 1'b1;

                                    // U66 clocks on RATECLK rises. INF5:3
                                    // set its period; U65/U64 then move one
                                    // count toward the target I10:I6 value.
                                    if (inflection_edges_left_q == 4'd1) begin
                                        inflection_edges_left_q <=
                                            movement_edge_count(
                                                inflection_high_q[2:0]
                                            );
                                        if (transitioned_inflection_q !=
                                            transitioned_inflection_target) begin
                                            transitioned_inflection_q <=
                                                move_one_toward_8(
                                                    transitioned_inflection_q,
                                                    transitioned_inflection_target
                                                );
                                            inflection_step_ce_q <= 1'b1;
                                        end
                                    end else begin
                                        inflection_edges_left_q <=
                                            inflection_edges_left_q - 4'd1;
                                    end

                                    rate_clock_div2_q <= !rate_clock_div2_q;
                                    if (!rate_clock_div2_q) begin
                                        rate_clock_div2_ce_q <= 1'b1;
                                        if (articulation_edges_left_q == 4'd1) begin
                                            articulation_edges_left_q <=
                                                movement_edge_count(
                                                    control_articulation_amplitude_q[6:4]
                                                );
                                            articulation_step_ce_q <= 1'b1;
                                            parameter_sweep_q <= 7'h7F;
                                        end else begin
                                            articulation_edges_left_q <=
                                                articulation_edges_left_q - 4'd1;
                                        end
                                    end
                                end
                            end else begin
                                rate_edges_left_q <= rate_edges_left_q - 5'd1;
                            end
                            end
                        end else begin
                            selector_latch_phase_q <= 1'b1;
                        end
                    end else begin
                        selector_phase_q <= 1'b1;
                    end
                end else begin
                    slow_div_q <= slow_div_q + 2'd1;
                end
            end

            // Writes come after background work so an acknowledgment wins a
            // collision with a response boundary on the same fabric edge.
            if (write_commit) begin
                if (write_reg_hold_q <= 3'd2 ||
                    (write_reg_hold_q == 3'd3 && write_data_hold_q[7])) begin
                    pending_q <= 1'b0;
                end

                case (write_reg_hold_q)
                    3'd0: begin
                        duration_phoneme_q <= write_data_hold_q;
                        if (!powered_down) begin
                            phone_active_q <= 1'b1;
                            frame_ticks_left_q <= frame_tick_count(
                                rate_inflection_q[7:4]
                            );
                            frames_left_q <= boundary_frame_count(
                                response_phoneme_q,
                                write_data_hold_q[7:6]
                            );
                        end
                    end

                    3'd1: begin
                        inflection_high_q <= write_data_hold_q;
                    end

                    3'd2: begin
                        rate_inflection_q <= write_data_hold_q;
                    end

                    3'd3: begin
                        if (write_data_hold_q[7]) begin
                            control_articulation_amplitude_q <=
                                write_data_hold_q;
                            phone_active_q <= 1'b0;
                            ar_enabled_q <= 1'b0;
                            pending_q <= 1'b0;
                        end else if (control_articulation_amplitude_q[7]) begin
                            control_articulation_amplitude_q <=
                                write_data_hold_q;
                            phone_active_q <= 1'b1;
                            frame_ticks_left_q <= frame_tick_count(
                                rate_inflection_q[7:4]
                            );

                            case (duration_phoneme_q[7:6])
                                MODE_PHONEME_TRANSITIONED: begin
                                    response_phoneme_q <= 1'b1;
                                    transitioned_pitch_q <= 1'b1;
                                    ar_enabled_q <= 1'b1;
                                    frames_left_q <= boundary_frame_count(
                                        1'b1, duration_phoneme_q[7:6]
                                    );
                                end

                                MODE_PHONEME_IMMEDIATE: begin
                                    response_phoneme_q <= 1'b1;
                                    transitioned_pitch_q <= 1'b0;
                                    ar_enabled_q <= 1'b1;
                                    frames_left_q <= boundary_frame_count(
                                        1'b1, duration_phoneme_q[7:6]
                                    );
                                end

                                MODE_FRAME_IMMEDIATE: begin
                                    response_phoneme_q <= 1'b0;
                                    transitioned_pitch_q <= 1'b0;
                                    ar_enabled_q <= 1'b1;
                                    frames_left_q <= 3'd1;
                                end

                                default: begin
                                    // DR=00 only disables A/R. It keeps the
                                    // prior response and inflection modes.
                                    ar_enabled_q <= 1'b0;
                                    frames_left_q <= boundary_frame_count(
                                        response_phoneme_q,
                                        duration_phoneme_q[7:6]
                                    );
                                end
                            endcase
                        end else begin
                            // CTL was already low: change articulation and
                            // amplitude without re-latching DR or restarting.
                            control_articulation_amplitude_q <=
                                write_data_hold_q;
                        end
                    end

                    default: begin
                        // RS2=1 aliases registers 4, 5, 6, and 7.
                        filter_frequency_q <= write_data_hold_q;
                    end
                endcase
            end

            // SSI-263AP fixes the P revision's PD/RST bug. The AP part enters
            // power-down, clears D7/A-R, and retains the other registers.
            if (!pd_rst_n && REVISION_AP) begin
                control_articulation_amplitude_q[7] <= 1'b1;
                pending_q <= 1'b0;
                phone_active_q <= 1'b0;
                ar_enabled_q <= 1'b0;
            end else if (pd_rst_n_q && !pd_rst_n && !REVISION_AP &&
                         !control_articulation_amplitude_q[7]) begin
                // On the P revision, reset does not power down or stop the
                // phone. A reset edge re-latches nonzero DR modes.
                case (duration_phoneme_q[7:6])
                    MODE_PHONEME_TRANSITIONED: begin
                        response_phoneme_q <= 1'b1;
                        transitioned_pitch_q <= 1'b1;
                        ar_enabled_q <= 1'b1;
                    end
                    MODE_PHONEME_IMMEDIATE: begin
                        response_phoneme_q <= 1'b1;
                        transitioned_pitch_q <= 1'b0;
                        ar_enabled_q <= 1'b1;
                    end
                    MODE_FRAME_IMMEDIATE: begin
                        response_phoneme_q <= 1'b0;
                        transitioned_pitch_q <= 1'b0;
                        ar_enabled_q <= 1'b1;
                    end
                    default: ar_enabled_q <= 1'b0;
                endcase
            end
        end
    end

endmodule
