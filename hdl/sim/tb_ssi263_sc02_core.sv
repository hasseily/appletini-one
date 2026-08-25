`timescale 1ns / 1ps

module tb_ssi263_sc02_core;

    logic clk = 1'b0;
    logic rstn = 1'b0;
    logic pd_rst_n = 1'b1;
    logic xck_ce = 1'b0;
    logic div2 = 1'b0;
    logic write_active = 1'b0;
    logic [2:0] write_reg = 3'd0;
    logic [7:0] write_data = 8'd0;

    logic d7_pending;
    logic ar_drive_low;
    logic powered_down;
    logic phone_active;
    logic ar_enabled;
    logic response_phoneme;
    logic transitioned_pitch;
    logic [5:0] phoneme;
    logic [1:0] duration;
    logic [3:0] rate;
    logic [11:0] inflection;
    logic [11:0] pitch_inflection;
    logic [7:0] transitioned_inflection_state;
    logic [2:0] articulation;
    logic [3:0] amplitude;
    logic [7:0] filter_frequency;
    logic effective_xck_ce;
    logic response_boundary_ce;
    logic voice_clock_ce;
    logic voice_toggle;
    logic pitch_period_ce;
    logic noise_clock_ce;
    logic noise_shift_ce;
    logic filter_phase_ce;
    logic filter_phase;
    logic [2:0] selector;
    logic selector_phase;
    logic selector_step_ce;
    logic [7:0] selector_rom_data;
    logic [3:0] selector_flags;
    logic pw_0;
    logic pw_1;
    logic pw_2;
    logic pw_3;
    logic pw_5;
    logic fric1_sw;
    logic fric2_sw;
    logic closure;
    logic rate_clock_ce;
    logic rate_clock_div2_ce;
    logic articulation_step_ce;
    logic inflection_step_ce;
    logic parameter_write_ce;
    logic [2:0] parameter_write_selector;
    logic [3:0] f1_code;
    logic [3:0] f2_code;
    logic [3:0] f2_res_code;
    logic [3:0] f3_code;
    logic [3:0] f4_code;
    logic [3:0] filter_amp_code;
    logic [3:0] voice_amp_code;
    logic [3:0] fric_amp_code;

    logic p_d7_pending;
    logic p_ar_drive_low;
    logic p_powered_down;
    logic p_phone_active;
    logic [11:0] p_inflection;

    integer failures = 0;
    integer effective_ticks_seen = 0;
    integer selector_steps_seen = 0;
    integer filter_edges_seen = 0;
    integer inflection_steps_seen = 0;
    integer articulation_steps_seen = 0;
    integer setting;
    integer first_wait;
    integer second_wait;
    integer state_before;
    integer steps_before;
    integer filter_before;
    integer voice_edges_seen = 0;
    integer pitch_events_seen = 0;
    integer noise_edges_seen = 0;
    integer closure_events_seen = 0;
    integer voice_before;
    integer pitch_before;
    integer noise_before;

    always #5 clk = ~clk;

    ssi263_sc02_core #(
        .REVISION_AP(1'b1),
        .ROM_FILE("ssi263_sc02_rom.mem")
    ) dut (
        .clk(clk),
        .rstn(rstn),
        .pd_rst_n(pd_rst_n),
        .xck_ce(xck_ce),
        .div2(div2),
        .write_active(write_active),
        .write_reg(write_reg),
        .write_data(write_data),
        .d7_pending(d7_pending),
        .ar_drive_low(ar_drive_low),
        .powered_down(powered_down),
        .phone_active(phone_active),
        .ar_enabled(ar_enabled),
        .response_phoneme(response_phoneme),
        .transitioned_pitch(transitioned_pitch),
        .phoneme(phoneme),
        .duration(duration),
        .rate(rate),
        .inflection(inflection),
        .pitch_inflection(pitch_inflection),
        .transitioned_inflection_state(transitioned_inflection_state),
        .articulation(articulation),
        .amplitude(amplitude),
        .filter_frequency(filter_frequency),
        .effective_xck_ce(effective_xck_ce),
        .response_boundary_ce(response_boundary_ce),
        .voice_clock_ce(voice_clock_ce),
        .voice_toggle(voice_toggle),
        .pitch_period_ce(pitch_period_ce),
        .noise_clock_ce(noise_clock_ce),
        .noise_shift_ce(noise_shift_ce),
        .filter_phase_ce(filter_phase_ce),
        .filter_phase(filter_phase),
        .selector(selector),
        .selector_phase(selector_phase),
        .selector_step_ce(selector_step_ce),
        .selector_rom_data(selector_rom_data),
        .selector_flags(selector_flags),
        .pw_0(pw_0),
        .pw_1(pw_1),
        .pw_2(pw_2),
        .pw_3(pw_3),
        .pw_5(pw_5),
        .fric1_sw(fric1_sw),
        .fric2_sw(fric2_sw),
        .closure(closure),
        .rate_clock_ce(rate_clock_ce),
        .rate_clock_div2_ce(rate_clock_div2_ce),
        .articulation_step_ce(articulation_step_ce),
        .inflection_step_ce(inflection_step_ce),
        .parameter_write_ce(parameter_write_ce),
        .parameter_write_selector(parameter_write_selector),
        .f1_code(f1_code),
        .f2_code(f2_code),
        .f2_res_code(f2_res_code),
        .f3_code(f3_code),
        .f4_code(f4_code),
        .filter_amp_code(filter_amp_code),
        .voice_amp_code(voice_amp_code),
        .fric_amp_code(fric_amp_code)
    );

    ssi263_sc02_core #(
        .REVISION_AP(1'b0),
        .ROM_FILE("ssi263_sc02_rom.mem")
    ) dut_p (
        .clk(clk),
        .rstn(rstn),
        .pd_rst_n(pd_rst_n),
        .xck_ce(xck_ce),
        .div2(div2),
        .write_active(write_active),
        .write_reg(write_reg),
        .write_data(write_data),
        .d7_pending(p_d7_pending),
        .ar_drive_low(p_ar_drive_low),
        .powered_down(p_powered_down),
        .phone_active(p_phone_active),
        .ar_enabled(),
        .response_phoneme(),
        .transitioned_pitch(),
        .phoneme(),
        .duration(),
        .rate(),
        .inflection(p_inflection),
        .pitch_inflection(),
        .transitioned_inflection_state(),
        .articulation(),
        .amplitude(),
        .filter_frequency(),
        .effective_xck_ce(),
        .response_boundary_ce(),
        .voice_clock_ce(),
        .voice_toggle(),
        .pitch_period_ce(),
        .noise_clock_ce(),
        .noise_shift_ce(),
        .filter_phase_ce(),
        .filter_phase(),
        .selector(),
        .selector_phase(),
        .selector_step_ce(),
        .selector_rom_data(),
        .selector_flags(),
        .pw_0(),
        .pw_1(),
        .pw_2(),
        .pw_3(),
        .pw_5(),
        .fric1_sw(),
        .fric2_sw(),
        .closure(),
        .rate_clock_ce(),
        .rate_clock_div2_ce(),
        .articulation_step_ce(),
        .inflection_step_ce(),
        .parameter_write_ce(),
        .parameter_write_selector(),
        .f1_code(),
        .f2_code(),
        .f2_res_code(),
        .f3_code(),
        .f4_code(),
        .filter_amp_code(),
        .voice_amp_code(),
        .fric_amp_code()
    );

    always @(posedge clk) begin
        if (effective_xck_ce) begin
            effective_ticks_seen <= effective_ticks_seen + 1;
        end
        if (selector_step_ce) begin
            selector_steps_seen <= selector_steps_seen + 1;
        end
        if (filter_phase_ce) begin
            filter_edges_seen <= filter_edges_seen + 1;
        end
        if (voice_clock_ce) begin
            voice_edges_seen <= voice_edges_seen + 1;
        end
        if (pitch_period_ce) begin
            pitch_events_seen <= pitch_events_seen + 1;
        end
        if (noise_clock_ce) begin
            noise_edges_seen <= noise_edges_seen + 1;
        end
        if (closure) begin
            closure_events_seen <= closure_events_seen + 1;
        end
        if (inflection_step_ce) begin
            inflection_steps_seen <= inflection_steps_seen + 1;
        end
        if (articulation_step_ce) begin
            articulation_steps_seen <= articulation_steps_seen + 1;
        end
    end

    task automatic check(input logic condition, input string message);
        begin
            if (!condition) begin
                $display("SSI263 SC02 CORE FAIL: %s", message);
                failures = failures + 1;
            end
        end
    endtask

    task automatic write_register(input logic [2:0] address,
                                  input logic [7:0] value);
        begin
            @(negedge clk);
            write_reg = address;
            write_data = value;
            write_active = 1'b1;
            repeat (2) @(negedge clk);
            write_active = 1'b0;
            @(negedge clk);
        end
    endtask

    task automatic raw_xck_edges(input integer count);
        integer i;
        begin
            for (i = 0; i < count; i = i + 1) begin
                @(negedge clk);
                xck_ce = 1'b1;
                @(negedge clk);
                xck_ce = 1'b0;
            end
            @(negedge clk);
        end
    endtask

    task automatic reset_chips;
        begin
            @(negedge clk);
            rstn = 1'b0;
            write_active = 1'b0;
            xck_ce = 1'b0;
            pd_rst_n = 1'b1;
            div2 = 1'b0;
            repeat (3) @(negedge clk);
            rstn = 1'b1;
            repeat (2) @(negedge clk);
        end
    endtask

    task automatic wait_articulation_pulse(output integer ticks);
        begin
            ticks = 0;
            if (articulation_step_ce) begin
                @(negedge clk);
            end
            while (!articulation_step_ce) begin
                @(negedge clk);
                xck_ce = 1'b1;
                @(negedge clk);
                xck_ce = 1'b0;
                ticks = ticks + 1;
            end
        end
    endtask

    task automatic wait_inflection_pulse(output integer ticks);
        begin
            ticks = 0;
            if (inflection_step_ce) begin
                @(negedge clk);
            end
            while (!inflection_step_ce) begin
                @(negedge clk);
                xck_ce = 1'b1;
                @(negedge clk);
                xck_ce = 1'b0;
                ticks = ticks + 1;
            end
        end
    endtask

    task automatic wait_closure_pulse(output integer ticks);
        begin
            ticks = 0;
            if (closure) begin
                @(negedge clk);
            end
            while (!closure) begin
                @(negedge clk);
                xck_ce = 1'b1;
                @(negedge clk);
                xck_ce = 1'b0;
                ticks = ticks + 1;
            end
        end
    endtask

    task automatic check_lower_code(input logic [5:0] phone,
                                    input logic [3:0] code);
        begin
            reset_chips();
            write_register(3'd0, {2'b00, phone});
            raw_xck_edges(48);
            check(pw_2 == code[2], "lower code PW2");
            check(pw_5 == !code[2], "lower code PW5");
        end
    endtask

    initial begin
        reset_chips();
        check(powered_down && !d7_pending && filter_frequency == 8'hFF,
               "power reset state");

        // Address/data change during an active write must not update the chip.
        @(negedge clk);
        write_reg = 3'd4;
        write_data = 8'hE9;
        write_active = 1'b1;
        repeat (2) @(negedge clk);
        check(filter_frequency == 8'hFF, "write changed before its end");
        write_active = 1'b0;
        @(negedge clk);
        check(filter_frequency == 8'hE9, "write did not latch at its end");

        // Register aliases 4-7 all write FF.
        write_register(3'd7, 8'h7F);
        check(filter_frequency == 8'h7F, "filter alias register 7");

        write_register(3'd1, 8'h50);
        write_register(3'd2, 8'hF8);
        check(inflection == 12'hA80, "12-bit inflection assembly");

        // DR=11: phoneme timing, transitioned pitch, shortest duration.
        write_register(3'd0, 8'hC0);
        write_register(3'd3, 8'h00);
        check(phone_active && ar_enabled && response_phoneme &&
               transitioned_pitch, "DR=11 mode latch");
        raw_xck_edges(4095);
        check(!d7_pending, "phoneme response arrived one tick early");
        raw_xck_edges(1);
        check(d7_pending && ar_drive_low, "phoneme response missing");

        // An inflection write acknowledges but does not restart the timer.
        write_register(3'd1, 8'h50);
        check(!d7_pending && phone_active, "reg1 acknowledgment");
        raw_xck_edges(4096);
        check(d7_pending && ar_drive_low, "repeat response after ack");

        // DR=00 preserves phoneme timing, uses its live D=0 duration, sets D7,
        // and blocks only the external A/R drive.
        write_register(3'd3, 8'h80);
        write_register(3'd0, 8'h00);
        write_register(3'd3, 8'h00);
        check(response_phoneme && transitioned_pitch && !ar_enabled,
               "DR=00 did not preserve prior response mode");
        raw_xck_edges(16383);
        check(!d7_pending, "DR=00 response arrived one tick early");
        raw_xck_edges(1);
        check(d7_pending && !ar_drive_low,
               "DR=00 D7 and A/R split");

        // Frame mode remains frame mode through a later DR=00 wake.
        write_register(3'd3, 8'h80);
        write_register(3'd0, 8'h40);
        write_register(3'd3, 8'h00);
        check(!response_phoneme && !transitioned_pitch && ar_enabled,
               "DR=01 frame mode latch");
        write_register(3'd3, 8'h80);
        write_register(3'd0, 8'h00);
        write_register(3'd3, 8'h00);
        raw_xck_edges(4096);
        check(d7_pending && !ar_drive_low,
               "DR=00 did not retain frame timing");

        // A write acknowledgment wins when it ends on the response tick.
        write_register(3'd3, 8'h80);
        write_register(3'd0, 8'h40);
        write_register(3'd3, 8'h00);
        raw_xck_edges(4095);
        @(negedge clk);
        write_reg = 3'd1;
        write_data = 8'h50;
        write_active = 1'b1;
        @(negedge clk);
        xck_ce = 1'b1;
        write_active = 1'b0;
        @(negedge clk);
        xck_ce = 1'b0;
        check(!d7_pending, "ack lost a boundary collision");

        // 2 XCK pin edges with DIV2 set produce one effective tick.
        reset_chips();
        div2 = 1'b1;
        @(negedge clk);
        effective_ticks_seen = 0;
        raw_xck_edges(20);
        check(effective_ticks_seen == 10, "DIV2 effective XCK count");

        // Sheet 3: U44 divides by four, U45 Q1/Q2 form the WRITE/LATCH
        // phases, and Q3/Q4/Q5 select the ROM column. A selector lasts
        // sixteen FASTCLK ticks.
        // Parameter RAM
        // state must not move until U94 launches an articulation sweep.
        reset_chips();
        raw_xck_edges(15);
        check(selector == 3'd0, "selector advanced before sixteen ticks");
        raw_xck_edges(1);
        check(selector == 3'd1 && f1_code == 4'd0,
               "selector moved a parameter without a sweep");

        // The complete U44/U45/U46/U47 waveform has one 16-tick slot per
        // selector. WRITE is high at r=2/3 and LATCH at r=10/11.
        reset_chips();
        for (setting = 0; setting < 128; setting = setting + 1) begin
            check(selector == setting / 16,
                  "selector changed inside a 16-tick slot");
            check(dut.selector_write_level ==
                  ((setting % 16) == 2 || (setting % 16) == 3),
                  "U46/U47 WRITE waveform mismatch");
            check(dut.selector_latch_level ==
                  ((setting % 16) == 10 || (setting % 16) == 11),
                  "U46/U47 LATCH waveform mismatch");
            check(!(dut.selector_write_level &&
                    dut.selector_latch_level),
                  "WRITE and LATCH overlapped");
            raw_xck_edges(1);
        end
        check(selector == 3'd0,
              "128-tick selector scan did not wrap exactly");

        // Every articulation code reloads U94. Its steady pulse interval is
        // 256 * (16-R) * (8-T) effective ticks.
        for (setting = 0; setting < 8; setting = setting + 1) begin
            reset_chips();
            write_register(3'd2, 8'hF0);
            write_register(3'd3, 8'h80 | (setting << 4));
            wait_articulation_pulse(first_wait);
            wait_articulation_pulse(second_wait);
            check(second_wait == 256 * (8 - setting),
                   "articulation terminal pace");
        end

        // Every I5:I3 slope code reloads U66. U65/U64 move their own state
        // one count per terminal, and that state drives transitioned pitch.
        for (setting = 0; setting < 8; setting = setting + 1) begin
            reset_chips();
            write_register(3'd1, 8'hF8 | setting);
            write_register(3'd2, 8'hF0);
            wait_inflection_pulse(first_wait);
            state_before = transitioned_inflection_state;
            wait_inflection_pulse(second_wait);
            check(second_wait == 128 * (8 - setting),
                   "inflection terminal pace");
            check(transitioned_inflection_state == state_before + 1,
                   "transitioned inflection count");
            check(pitch_inflection[10:3] ==
                  transitioned_inflection_state,
                  "transition state did not drive pitch");
        end

        // Immediate mode bypasses the transitioned I10:I3 state. The raw U59
        // VOICECLK is /4 and U62-Q rising is the final /8 pitch event. U58,
        // U59, and U62 keep running while the phone is down.
        reset_chips();
        write_register(3'd1, 8'hFF);
        write_register(3'd2, 8'h0F);
        write_register(3'd0, 8'h80);
        write_register(3'd3, 8'h00);
        check(!transitioned_pitch && pitch_inflection == inflection,
               "immediate pitch bypass");
        raw_xck_edges(16384);
        voice_before = voice_edges_seen;
        pitch_before = pitch_events_seen;
        raw_xck_edges(16);
        check(voice_edges_seen - voice_before == 4,
               "raw VOICECLK cadence");

        // U52C is U49 terminal count gated by U43B Q. With FF=FF, its
        // one-clock synchronous form occurs every two effective ticks.
        reset_chips();
        write_register(3'd4, 8'hFF);
        wait_closure_pulse(first_wait);
        wait_closure_pulse(second_wait);
        check(first_wait == 2 && second_wait == 2,
               "CLOSURE U49/U43 cadence");

        // The only low nibbles present in active ROM are 0/1 in slots 0/1
        // and 4/6/8/A/C/E in slot 2. Check every slot-2 code.
        check_lower_code(6'h28, 4'h4);
        check_lower_code(6'h2F, 4'h6);
        check_lower_code(6'h2B, 4'h8);
        check_lower_code(6'h00, 4'hA);
        check_lower_code(6'h24, 4'hC);
        check_lower_code(6'h01, 4'hE);

        // U29A falls when the decoded /PHO_WRITE condition asserts and on a
        // DURCLK rise. U38 equality is q=2 for TPARM0=1 and q=6 otherwise.
        reset_chips();
        write_register(3'd3, 8'h00);
        @(negedge clk);
        write_reg = 3'd0;
        write_data = 8'h00;
        write_active = 1'b1;
        @(posedge clk);
        #1;
        check(dut.u37_q == 4'd1,
              "U37 did not advance at phoneme-write assertion");
        @(negedge clk);
        write_active = 1'b0;
        @(negedge clk);
        force dut.frame_ticks_left_q = 17'd1;
        force dut.frames_left_q = 3'd1;
        raw_xck_edges(1);
        check(dut.u37_q == 4'd2,
              "U37 did not advance on DURCLK rise");
        release dut.frame_ticks_left_q;
        release dut.frames_left_q;

        // DONE_RB is a zero hold, not an edge reset. It must beat the same
        // write-assertion clock which acknowledges the pending request.
        @(negedge clk);
        dut.u37_q = 4'd0;
        force dut.pending_q = 1'b1;
        write_active = 1'b1;
        @(posedge clk);
        #1;
        check(dut.u37_q == 4'd0,
              "DONE_RB zero hold lost a coincident U37 clock");
        @(negedge clk);
        write_active = 1'b0;
        release dut.pending_q;
        @(posedge clk);
        @(negedge clk);
        dut.u37_q = 4'd0;
        write_active = 1'b1;
        @(posedge clk);
        #1;
        check(dut.u37_q == 4'd1,
              "U37 zero state did not advance without DONE_RB hold");
        @(negedge clk);
        write_active = 1'b0;

        // DONE high does not reset a nonzero count. It can wrap F->0 on one
        // falling edge, after which the zero hold blocks the next edge.
        reset_chips();
        @(negedge clk);
        dut.u37_q = 4'hF;
        force dut.pending_q = 1'b1;
        write_reg = 3'd0;
        write_active = 1'b1;
        @(posedge clk);
        #1;
        check(dut.u37_q == 4'd0,
              "DONE_RB stopped nonzero U37 before modulo wrap");
        @(negedge clk);
        write_active = 1'b0;
        @(posedge clk);
        @(negedge clk);
        write_active = 1'b1;
        @(posedge clk);
        #1;
        check(dut.u37_q == 4'd0,
              "DONE_RB did not hold U37 after wrap reached zero");
        @(negedge clk);
        write_active = 1'b0;
        release dut.pending_q;

        // A DURCLK edge raises DONE_RB later on the same fabric edge. U37
        // therefore sees the old DONE level and advances before the new hold.
        reset_chips();
        @(negedge clk);
        dut.u37_q = 4'd0;
        force dut.phone_active_q = 1'b1;
        force dut.control_articulation_amplitude_q = 8'h00;
        force dut.frame_ticks_left_q = 17'd1;
        force dut.frames_left_q = 3'd1;
        raw_xck_edges(1);
        check(dut.u37_q == 4'd1 && dut.pending_q,
              "same-edge DURCLK/DONE did not use the old DONE level");
        release dut.phone_active_q;
        release dut.control_articulation_amplitude_q;
        release dut.frame_ticks_left_q;
        release dut.frames_left_q;

        // PW0/PW1 are timed set-only latches. A phone-write level clears
        // them only after the selected set path closes.
        force dut.selector_phase_q = 1'b0;
        force dut.selector_latch_phase_q = 1'b1;
        force dut.slow_div_q = 2'd2;
        force dut.u37_q = 4'd2;
        force dut.selector_q = 3'd0;
        force dut.selector_flags = 4'b0001;
        @(posedge clk);
        #1;
        check(pw_0, "U32 PW0 set path missed q=2");
        force dut.slow_div_q = 2'd0;
        write_reg = 3'd0;
        write_active = 1'b1;
        @(posedge clk);
        #1;
        check(!pw_0, "U32 PW0 did not clear during /PHO_WRITE low");
        write_active = 1'b0;

        force dut.slow_div_q = 2'd2;
        force dut.u37_q = 4'd6;
        force dut.selector_q = 3'd1;
        force dut.selector_flags = 4'b0000;
        @(posedge clk);
        #1;
        check(pw_1, "U33 PW1 set path missed q=6");
        force dut.slow_div_q = 2'd0;
        force dut.selector_flags = 4'b0001;
        @(posedge clk);
        #1;
        check(pw_1, "U33 PW1 did not retain outside its set window");

        // U34 loads Qctrl OR /TPARM1 only at the timed slot-2 equality.
        force dut.slow_div_q = 2'd2;
        force dut.u37_q = 4'd2;
        force dut.selector_q = 3'd2;
        force dut.selector_flags = 4'b0011;
        force dut.u183a_q = 1'b0;
        @(posedge clk);
        #1;
        check(!pw_3, "U34 PW3 did not load low for CTRL=0/TPARM1=1");
        force dut.u183a_q = 1'b1;
        @(posedge clk);
        #1;
        check(pw_3, "U34 PW3 did not load high from CTRL");
        force dut.u183a_q = 1'b0;
        force dut.selector_flags = 4'b0001;
        @(posedge clk);
        #1;
        check(pw_3, "U34 PW3 did not load high from /TPARM1");
        release dut.selector_phase_q;
        release dut.selector_latch_phase_q;
        release dut.slow_div_q;
        release dut.u37_q;
        release dut.selector_q;
        release dut.selector_flags;
        release dut.u183a_q;
        reset_chips();

        // Sheet-4 setup is target-A modulo 16 with C=8 and a stored direction
        // carry. The following WRITE events run the exact U83/U84 DDA.
        @(negedge clk);
        dut.parameter_resa_q[0] = 4'h3;
        dut.parameter_resb_q[0] = 4'h0;
        dut.parameter_resc_q[0] = 4'h0;
        dut.parameter_rescy_q[0] = 1'b0;
        force dut.selector_q = 3'd0;
        force dut.selector_phase_q = 1'b0;
        force dut.selector_latch_phase_q = 1'b0;
        force dut.slow_div_q = 2'd1;
        force dut.selector_target = 4'hB;
        force dut.phone_setup_window_q = 1'b1;
        raw_xck_edges(1);
        check(dut.parameter_resa_q[0] == 4'h3 &&
              dut.parameter_resb_q[0] == 4'h8 &&
              dut.parameter_resc_q[0] == 4'h8 &&
              dut.parameter_rescy_q[0],
              "U83/U84 upward setup vector mismatch");
        force dut.phone_setup_window_q = 1'b0;
        force dut.phone_setup_pending_q = 1'b0;
        force dut.u96_write_permit = 1'b1;
        raw_xck_edges(1);
        check(dut.parameter_resa_q[0] == 4'h4 &&
              dut.parameter_resc_q[0] == 4'h0,
              "DDA upward event 1 mismatch");
        raw_xck_edges(1);
        check(dut.parameter_resa_q[0] == 4'h4 &&
              dut.parameter_resc_q[0] == 4'h8,
              "DDA upward event 2 mismatch");
        raw_xck_edges(1);
        check(dut.parameter_resa_q[0] == 4'h5 &&
              dut.parameter_resc_q[0] == 4'h0,
              "DDA upward event 3 mismatch");
        for (setting = 0; setting < 12; setting = setting + 1)
            raw_xck_edges(1);
        check(dut.parameter_resa_q[0] == 4'hB &&
              dut.parameter_resc_q[0] == 4'h0,
              "DDA upward vector did not reach target on event 15");
        raw_xck_edges(1);
        check(dut.parameter_resa_q[0] == 4'hB &&
              dut.parameter_resc_q[0] == 4'h0,
              "RESA_EQUALS failed to block the next DDA write");

        @(negedge clk);
        dut.parameter_resa_q[0] = 4'hC;
        force dut.selector_target = 4'h3;
        force dut.phone_setup_window_q = 1'b1;
        raw_xck_edges(1);
        check(dut.parameter_resb_q[0] == 4'h7 &&
              dut.parameter_resc_q[0] == 4'h8 &&
              !dut.parameter_rescy_q[0] &&
              dut.parameter_resa_q[0] == 4'hC,
              "U83/U84 downward setup vector mismatch");
        force dut.phone_setup_window_q = 1'b0;
        raw_xck_edges(1);
        check(dut.parameter_resa_q[0] == 4'hB &&
              dut.parameter_resc_q[0] == 4'hF,
              "DDA downward event 1 mismatch");
        raw_xck_edges(1);
        check(dut.parameter_resa_q[0] == 4'hB &&
              dut.parameter_resc_q[0] == 4'h6,
              "DDA downward event 2 mismatch");
        for (setting = 0; setting < 14; setting = setting + 1)
            raw_xck_edges(1);
        check(dut.parameter_resa_q[0] == 4'h3 &&
              dut.parameter_resc_q[0] == 4'h8,
              "DDA downward vector did not reach target on event 16");

        // A host write ending on r=2 owns both forms of the sheet-4 collision:
        // neither A_CLR setup nor a normal U96 write may touch the DDA RAMs.
        @(negedge clk);
        dut.parameter_resa_q[0] = 4'h3;
        dut.parameter_resb_q[0] = 4'h2;
        dut.parameter_resc_q[0] = 4'hE;
        dut.parameter_rescy_q[0] = 1'b1;
        force dut.phone_setup_window_q = 1'b1;
        dut.write_reg_hold_q = 3'd4;
        dut.write_data_hold_q = 8'h80;
        write_active = 1'b0;
        @(negedge clk);
        dut.write_active_q = 1'b1;
        xck_ce = 1'b1;
        @(negedge clk);
        xck_ce = 1'b0;
        check(dut.parameter_resa_q[0] == 4'h3 &&
              dut.parameter_resb_q[0] == 4'h2 &&
              dut.parameter_resc_q[0] == 4'hE,
              "host write did not suppress colliding A_CLR setup");
        force dut.phone_setup_window_q = 1'b0;
        force dut.u96_write_permit = 1'b1;
        @(negedge clk);
        dut.write_active_q = 1'b1;
        xck_ce = 1'b1;
        @(negedge clk);
        xck_ce = 1'b0;
        check(dut.parameter_resa_q[0] == 4'h3 &&
              dut.parameter_resc_q[0] == 4'hE,
              "host write did not suppress colliding U96 write");
        release dut.selector_q;
        release dut.selector_phase_q;
        release dut.selector_latch_phase_q;
        release dut.slow_div_q;
        release dut.selector_target;
        release dut.phone_setup_window_q;
        release dut.phone_setup_pending_q;
        release dut.u96_write_permit;
        reset_chips();

        // A control write clocks U166B, pulling /Q low. The next sampled
        // control-write window at SEL2 resets it and restores /Q without any
        // DURCLK dependency.
        write_register(3'd3, 8'h0F);
        check(!dut.u166b_nq_q,
              "WR3 did not pull U166B./Q low");
        force dut.selector_q = 3'd3;
        force dut.selector_phase_q = 1'b1;
        force dut.selector_latch_phase_q = 1'b1;
        force dut.slow_div_q = 2'd3;
        raw_xck_edges(1);
        check(dut.control_setup_window_q && dut.u166b_nq_q,
              "SEL2 setup window did not reset U166B");
        release dut.selector_q;
        release dut.selector_phase_q;
        release dut.selector_latch_phase_q;
        release dut.slow_div_q;
        reset_chips();

        // U93 Q3/Q4 sample the selected rate on SEL2. A 0->1 sample produces
        // one scan of RATEEDGE; steady high, falling, and steady low do not.
        force dut.duration_phoneme_q = 8'h80;
        force dut.rate_clock_q = 1'b1;
        force dut.selector_q = 3'd3;
        force dut.selector_phase_q = 1'b1;
        force dut.selector_latch_phase_q = 1'b1;
        force dut.slow_div_q = 2'd3;
        raw_xck_edges(1);
        check(dut.u93_rate_q1 && !dut.u93_rate_q2,
              "U93 missed selected-rate rising sample");
        raw_xck_edges(1);
        check(dut.u93_rate_q1 && dut.u93_rate_q2,
              "U93 steady-high sample made a second RATEEDGE");
        force dut.rate_clock_q = 1'b0;
        raw_xck_edges(1);
        check(!dut.u93_rate_q1 && dut.u93_rate_q2,
              "U93 falling sample decoded as RATEEDGE");
        raw_xck_edges(1);
        check(!dut.u93_rate_q1 && !dut.u93_rate_q2,
              "U93 stable-low sample did not settle");
        release dut.duration_phoneme_q;
        release dut.rate_clock_q;
        release dut.selector_q;
        release dut.selector_phase_q;
        release dut.selector_latch_phase_q;
        release dut.slow_div_q;
        reset_chips();

        // Prototype U96 routing: TCEDGE owns 0-3, DUREDGE owns 4, RATEEDGE
        // plus PW0/PW1 own 5/6, and selector 7 is grounded.
        force dut.tc_edge_window_q = 1'b1;
        force dut.pw_5_q = 1'b0;
        for (setting = 0; setting < 4; setting = setting + 1) begin
            force dut.selector_q = setting[2:0];
            #1;
            check(dut.u96_write_permit,
                  "U96 rejected a TCEDGE parameter slot");
        end
        force dut.selector_q = 3'd7;
        #1;
        check(!dut.u96_write_permit,
              "U96 selector 7 was not grounded");
        force dut.selector_q = 3'd4;
        force dut.duration_edge_window_q = 1'b1;
        force dut.u166b_nq_q = 1'b1;
        force dut.control_articulation_amplitude_q = 8'h0F;
        #1;
        check(dut.u96_write_permit,
              "U96 rejected filter amplitude DUREDGE");
        force dut.selector_q = 3'd5;
        force dut.pw_0_q = 1'b1;
        force dut.u93_rate_q1 = 1'b1;
        force dut.u93_rate_q2 = 1'b0;
        #1;
        check(dut.u96_write_permit,
              "U96 rejected voice amplitude RATEEDGE");
        force dut.selector_q = 3'd6;
        force dut.pw_1_q = 1'b1;
        #1;
        check(dut.u96_write_permit,
              "U96 rejected fricative amplitude RATEEDGE");

        // Exercise every input denial, not just the selected positive route.
        force dut.pw_5_q = 1'b1;
        force dut.duration_phoneme_q = 8'h20;
        force dut.tc_edge_window_q = 1'b1;
        force dut.selector_q = 3'd0;
        #1;
        check(!dut.u96_write_permit,
              "U32B failed to hold selector 0");
        force dut.selector_q = 3'd1;
        #1;
        check(!dut.u96_write_permit,
              "U32B failed to hold selector 1");
        force dut.selector_q = 3'd3;
        #1;
        check(!dut.u96_write_permit,
              "U32B failed to hold selector 3");

        // U32B ORs three independent source-active terms. Prove each one,
        // the all-zero allow state, and the slot-2 bypass of U32B.
        force dut.selector_q = 3'd0;
        force dut.duration_phoneme_q = 8'h00;
        force dut.voice_amp_code_q = 4'hF;
        force dut.fric_amp_code_q = 4'h0;
        #1;
        check(!dut.u96_write_permit,
              "U32B ignored nonzero voice amplitude");
        force dut.voice_amp_code_q = 4'h0;
        force dut.fric_amp_code_q = 4'hF;
        #1;
        check(!dut.u96_write_permit,
              "U32B ignored nonzero fricative amplitude");
        force dut.fric_amp_code_q = 4'h0;
        #1;
        check(dut.u96_write_permit,
              "U32B rejected the all-zero source state");

        force dut.tc_edge_window_q = 1'b0;
        for (setting = 0; setting < 4; setting = setting + 1) begin
            force dut.selector_q = setting[2:0];
            #1;
            check(!dut.u96_write_permit,
                  "U96 accepted selector 0-3 without TCEDGE");
        end
        force dut.tc_edge_window_q = 1'b1;
        force dut.selector_q = 3'd2;
        force dut.voice_amp_code_q = 4'hF;
        #1;
        check(dut.u96_write_permit,
              "U96 selector 2 incorrectly passed through U32B");

        force dut.selector_q = 3'd4;
        force dut.duration_edge_window_q = 1'b0;
        force dut.u166b_nq_q = 1'b1;
        force dut.control_articulation_amplitude_q = 8'h0F;
        #1;
        check(!dut.u96_write_permit,
              "selector 4 ignored missing DUREDGE");
        force dut.duration_edge_window_q = 1'b1;
        force dut.u166b_nq_q = 1'b0;
        #1;
        check(!dut.u96_write_permit,
              "selector 4 ignored U166B./Q low");
        force dut.u166b_nq_q = 1'b1;
        force dut.control_articulation_amplitude_q = 8'h00;
        #1;
        check(!dut.u96_write_permit,
              "selector 4 ignored AMPZERO");
        force dut.selector_q = 3'd5;
        force dut.pw_0_q = 1'b0;
        force dut.u93_rate_q1 = 1'b1;
        force dut.u93_rate_q2 = 1'b0;
        #1;
        check(!dut.u96_write_permit,
              "selector 5 ignored PW0 low");
        force dut.pw_0_q = 1'b1;
        force dut.u93_rate_q1 = 1'b0;
        #1;
        check(!dut.u96_write_permit,
              "selector 5 ignored missing RATEEDGE");
        force dut.selector_q = 3'd6;
        force dut.pw_1_q = 1'b0;
        force dut.u93_rate_q1 = 1'b1;
        #1;
        check(!dut.u96_write_permit,
              "selector 6 ignored PW1 low");
        force dut.pw_1_q = 1'b1;
        force dut.u93_rate_q2 = 1'b1;
        #1;
        check(!dut.u96_write_permit,
              "selector 6 ignored missing RATEEDGE");
        release dut.tc_edge_window_q;
        release dut.pw_5_q;
        release dut.selector_q;
        release dut.duration_phoneme_q;
        release dut.duration_edge_window_q;
        release dut.u166b_nq_q;
        release dut.control_articulation_amplitude_q;
        release dut.pw_0_q;
        release dut.u93_rate_q1;
        release dut.u93_rate_q2;
        release dut.pw_1_q;
        release dut.voice_amp_code_q;
        release dut.fric_amp_code_q;
        reset_chips();

        // Sheet 6 ties U68 B/D to VCC, so the CD4029 counts in binary. In the
        // up direction /CO is active only at F; down remains terminal at 0.
        // U85C ORs that terminal signal with U104C before resetting U62.
        reset_chips();
        force dut.voice_amp_code_q = 4'hF;
        force dut.fric_amp_code_q = 4'h0;
        force dut.pw_3_q = 1'b0;
        force dut.u62_q = 1'b0;
        force dut.selector_q = 3'd4;
        force dut.ampct_q = 4'd14;
        #1;
        check(dut.ampct_up && dut.ampct_nco &&
              dut.u68_clock_level && dut.u62_reset,
              "U68 up-count nonterminal or U85C state is wrong");
        force dut.ampct_q = 4'd15;
        #1;
        check(dut.ampct_up && !dut.ampct_nco &&
              !dut.u68_clock_level && !dut.u62_reset,
              "U68 did not stop at the binary up terminal F");
        force dut.pw_3_q = 1'b1;
        force dut.ampct_q = 4'd2;
        #1;
        check(!dut.ampct_up && dut.ampct_nco &&
              dut.u68_clock_level && dut.u62_reset,
              "U68 down-count nonterminal or U85C state is wrong");
        force dut.ampct_q = 4'd0;
        #1;
        check(!dut.ampct_up && !dut.ampct_nco &&
              !dut.u68_clock_level && dut.u62_reset,
              "U68 did not stop at binary zero or U104C missed U85C");
        release dut.voice_amp_code_q;
        release dut.fric_amp_code_q;
        release dut.pw_3_q;
        release dut.u62_q;
        release dut.selector_q;
        release dut.ampct_q;

        // Prove the actual four-bit recurrence across the old BCD boundary.
        // A low/high SEL2 pair supplies one drawn U68 positive clock edge.
        reset_chips();
        force dut.voice_amp_code_q = 4'hF;
        force dut.fric_amp_code_q = 4'h0;
        force dut.pw_3_q = 1'b0;
        force dut.u62_q = 1'b0;
        force dut.selector_q = 3'd0;
        @(posedge clk);
        #1;
        for (setting = 1; setting <= 15; setting = setting + 1) begin
            force dut.selector_q = 3'd4;
            @(posedge clk);
            #1;
            check(dut.ampct_q == setting[3:0],
                  "U68 binary up-count recurrence changed");
            force dut.selector_q = 3'd0;
            @(posedge clk);
            #1;
        end
        check(dut.ampct_q == 4'hF && !dut.ampct_nco,
              "U68 did not reach and hold binary terminal F");

        force dut.pw_3_q = 1'b1;
        for (setting = 14; setting >= 1; setting = setting - 1) begin
            force dut.selector_q = 3'd4;
            @(posedge clk);
            #1;
            check(dut.ampct_q == setting[3:0],
                  "U68 binary down-count recurrence changed");
            force dut.selector_q = 3'd0;
            @(posedge clk);
            #1;
        end
        check(dut.ampct_q == 4'h1 && dut.ampct_nco && dut.ampct_zero,
              "drawn AMPCT_ZERO gate did not stop the live down count at 1");
        release dut.voice_amp_code_q;
        release dut.fric_amp_code_q;
        release dut.pw_3_q;
        release dut.u62_q;
        release dut.selector_q;

        // U75 counts on the U41C rise; U73 shifts on its fall. The two chip
        // edges must never collapse into one enable.
        reset_chips();
        force dut.pw_3_q = 1'b0;
        force dut.fric_amp_code_q = 4'hF;
        force dut.selector_q = 3'd0;
        #1;
        check(dut.u41c_level,
              "U41C high level was not formed from its drawn inputs");
        @(posedge clk);
        #1;
        check(noise_clock_ce && !noise_shift_ce,
              "U41C rise did not select U75 alone");
        force dut.selector_q = 3'd2;
        #1;
        check(!dut.u41c_level,
              "SEL1 did not pull U41C low");
        @(posedge clk);
        #1;
        check(!noise_clock_ce && noise_shift_ce,
              "U41C fall did not select U73 alone");
        release dut.pw_3_q;
        release dut.fric_amp_code_q;
        release dut.selector_q;

        // Sheet 7 U20B samples TPARM3 only at the gated WR_SEL2 edge.
        // First open the literal PW0/PW1/PW2/AMPCT_ZERO gate and sample one.
        reset_chips();
        force dut.pw_0_q = 1'b1;
        force dut.pw_1_q = 1'b1;
        force dut.pw_2_q = 1'b1;
        force dut.ampct_q = 4'd0;
        force dut.fric_amp_code_q = 4'hF;
        force dut.selector_q = 3'd2;
        force dut.selector_phase_q = 1'b0;
        force dut.selector_latch_phase_q = 1'b1;
        force dut.slow_div_q = 2'd1;
        force dut.selector_flags = 4'h8;
        #1;
        check(dut.u20_clock_enable,
              "U20B WR_SEL2 gate did not open on its literal inputs");
        raw_xck_edges(1);
        check(dut.u20b_q,
              "U20B did not sample TPARM3 high at WR_SEL2");

        // A nonzero AMPCT closes that gate when FRIC_AMP is nonzero, so a
        // later zero TPARM3 must leave U20B unchanged.
        force dut.ampct_q = 4'd2;
        force dut.selector_flags = 4'h0;
        #1;
        check(!dut.u20_clock_enable,
              "U20B WR_SEL2 gate ignored AMPCT_ZERO");
        raw_xck_edges(1);
        check(dut.u20b_q,
              "closed WR_SEL2 gate changed U20B");

        // FRIC_AMP_ZERO is the alternate gate term. It permits the same
        // WR_SEL2 edge to sample TPARM3 low.
        force dut.fric_amp_code_q = 4'h0;
        #1;
        check(dut.u20_clock_enable,
              "FRIC_AMP_ZERO did not open the U20B gate");
        raw_xck_edges(1);
        check(!dut.u20b_q,
              "U20B did not sample TPARM3 low at WR_SEL2");
        release dut.pw_0_q;
        release dut.pw_1_q;
        release dut.pw_2_q;
        release dut.ampct_q;
        release dut.fric_amp_code_q;
        release dut.selector_q;
        release dut.selector_phase_q;
        release dut.selector_latch_phase_q;
        release dut.slow_div_q;
        release dut.selector_flags;

        // U112 follows U20B only in Phi1_X. U166A samples !U20B only on the
        // positive Phi0_X boundary; both outputs hold through the other phase.
        force dut.filter_phase_q = 1'b0;
        force dut.u20b_q = 1'b1;
        @(posedge clk);
        #1;
        check(!fric1_sw,
              "U112 changed while Phi1_X was low");
        force dut.filter_phase_q = 1'b1;
        @(posedge clk);
        #1;
        check(fric1_sw,
              "U112 did not follow U20B during Phi1_X");
        force dut.u20b_q = 1'b0;
        @(posedge clk);
        #1;
        check(!fric1_sw,
              "U112 did not remain transparent in Phi1_X");
        force dut.filter_ticks_left_q = 9'd1;
        raw_xck_edges(1);
        check(fric2_sw,
              "U166A did not sample complementary U20B at Phi0_X");
        force dut.u20b_q = 1'b1;
        force dut.filter_phase_q = 1'b1;
        force dut.filter_ticks_left_q = 9'd1;
        raw_xck_edges(1);
        check(!fric2_sw,
              "U166A did not resample complementary U20B");
        release dut.filter_phase_q;
        release dut.filter_ticks_left_q;
        release dut.u20b_q;
        reset_chips();

        // U106-U114 first-stage latches follow RESA only during the selected
        // slot's LATCH window and retain it after the window closes.
        @(negedge clk);
        dut.parameter_resa_q[0] = 4'hA;
        force dut.selector_q = 3'd0;
        force dut.selector_phase_q = 1'b0;
        force dut.selector_latch_phase_q = 1'b1;
        force dut.slow_div_q = 2'd2;
        @(posedge clk);
        #1;
        check(dut.f1_first_q == 4'hA,
              "U106 did not follow RESA during LATCH");
        force dut.slow_div_q = 2'd0;
        @(negedge clk);
        dut.parameter_resa_q[0] = 4'h5;
        @(posedge clk);
        #1;
        check(dut.f1_first_q == 4'hA,
              "U106 changed after LATCH closed");
        release dut.selector_q;
        release dut.selector_phase_q;
        release dut.selector_latch_phase_q;
        release dut.slow_div_q;

        // Phi0_X owns F1/F3/voice; Phi1_X owns F2/F2_RES/F4/fricative.
        force dut.f1_first_q = 4'hA;
        force dut.f3_first_q = 4'hB;
        force dut.voice_amp_first_q = 4'hC;
        force dut.filter_phase_q = 1'b0;
        @(posedge clk);
        #1;
        check(f1_code == 4'hA && f3_code == 4'hB &&
              voice_amp_code == 4'hC,
              "Phi0_X latches did not follow their first stages");
        force dut.filter_phase_q = 1'b1;
        force dut.f1_first_q = 4'h1;
        force dut.f3_first_q = 4'h2;
        force dut.voice_amp_first_q = 4'h3;
        @(posedge clk);
        #1;
        check(f1_code == 4'hA && f3_code == 4'hB &&
              voice_amp_code == 4'hC,
              "Phi0_X latches changed during Phi1");

        force dut.f2_first_q = 4'h4;
        force dut.f2_res_first_q = 4'h5;
        force dut.f4_first_q = 4'h6;
        force dut.fric_amp_first_q = 4'h7;
        @(posedge clk);
        #1;
        check(f2_code == 4'h4 && f2_res_code == 4'h5 &&
              f4_code == 4'h6 && fric_amp_code == 4'h7,
              "Phi1_X latches did not follow their first stages");
        force dut.filter_phase_q = 1'b0;
        force dut.f2_first_q = 4'h8;
        force dut.f2_res_first_q = 4'h9;
        force dut.f4_first_q = 4'hA;
        force dut.fric_amp_first_q = 4'hB;
        @(posedge clk);
        #1;
        check(f2_code == 4'h4 && f2_res_code == 4'h5 &&
              f4_code == 4'h6 && fric_amp_code == 4'h7,
              "Phi1_X latches changed during Phi0");
        release dut.f1_first_q;
        release dut.f2_first_q;
        release dut.f2_res_first_q;
        release dut.f3_first_q;
        release dut.f4_first_q;
        release dut.voice_amp_first_q;
        release dut.fric_amp_first_q;
        release dut.filter_phase_q;
        reset_chips();

        // U206 samples raw filter amplitude AND AMPCT only on Phi0 entry.
        force dut.filter_amp_first_q = 4'hF;
        force dut.ampct_q = 4'b1010;
        force dut.pw_3_q = 1'b0;
        force dut.filter_phase_q = 1'b1;
        force dut.filter_ticks_left_q = 9'd1;
        raw_xck_edges(1);
        check(filter_amp_code == 4'hB,
              "U206 did not capture U111 AND AMPCT on Phi0 entry");
        force dut.filter_amp_first_q = 4'h5;
        force dut.ampct_q = 4'hF;
        @(posedge clk);
        #1;
        check(filter_amp_code == 4'hB,
              "U206 changed after the Phi0 capture edge");
        force dut.filter_phase_q = 1'b1;
        force dut.filter_ticks_left_q = 9'd1;
        raw_xck_edges(1);
        check(filter_amp_code == 4'h5,
              "U206 missed the next Phi0 capture edge");
        release dut.filter_amp_first_q;
        release dut.ampct_q;
        release dut.pw_3_q;
        release dut.filter_phase_q;
        release dut.filter_ticks_left_q;
        reset_chips();

        // CTL/PD gates the request and analog source paths. The schematic has
        // no CTL/PD gate on U44/U45, U48/U49, U66, or U94, so those digital
        // states keep running during power down and retain phase on wake.
        reset_chips();
        write_register(3'd0, 8'h00);
        write_register(3'd1, 8'hFF);
        write_register(3'd2, 8'hF0);
        write_register(3'd4, 8'hFF);
        steps_before = selector_steps_seen;
        filter_before = filter_edges_seen;
        raw_xck_edges(4097);
        check(powered_down && selector_steps_seen > steps_before,
               "selector stopped in power down");
        check(filter_edges_seen > filter_before,
               "filter divider stopped in power down");
        check(transitioned_inflection_state != 8'h00,
               "transitioned inflection stopped in power down");
        state_before = transitioned_inflection_state;
        steps_before = selector_steps_seen;
        write_register(3'd3, 8'h00);
        check(transitioned_inflection_state == state_before,
               "wake reset transitioned inflection");
        raw_xck_edges(128);
        check(selector_steps_seen > steps_before,
               "selector did not continue after wake");

        // AP PD/RST powers down and clears D7. P keeps the phone and request.
        write_register(3'd2, 8'hF0);
        write_register(3'd0, 8'hC0);
        write_register(3'd3, 8'h00);
        raw_xck_edges(4096);
        check(d7_pending && p_d7_pending, "pre-reset request state");
        @(negedge clk);
        pd_rst_n = 1'b0;
        repeat (2) @(negedge clk);
        check(powered_down && !d7_pending && !phone_active,
               "AP PD/RST behavior");
        check(!p_powered_down && p_d7_pending && p_phone_active,
               "P reset bug behavior");
        pd_rst_n = 1'b1;
        repeat (2) @(negedge clk);
        check(powered_down, "AP must remain down after PD/RST release");

        // AP PD/RST owns the complete edge, so no retained register may
        // change on a colliding write end or while reset stays asserted. The
        // faulty P revision still accepts both writes.
        reset_chips();
        write_register(3'd3, 8'h00);
        write_register(3'd1, 8'h25);
        @(negedge clk);
        write_reg = 3'd1;
        write_data = 8'hAA;
        write_active = 1'b1;
        repeat (2) @(negedge clk);
        pd_rst_n = 1'b0;
        write_active = 1'b0;
        @(negedge clk);
        check(inflection[10:3] == 8'h25 && powered_down,
               "AP reset/write collision changed a retained register");
        check(p_inflection[10:3] == 8'hAA && !p_powered_down,
               "P reset bug did not accept colliding write");
        write_register(3'd1, 8'h55);
        check(inflection[10:3] == 8'h25,
               "AP accepted a write while PD/RST was held");
        check(p_inflection[10:3] == 8'h55,
               "P rejected a write while PD/RST was held");
        pd_rst_n = 1'b1;
        repeat (2) @(negedge clk);

        if (failures == 0) begin
            $display("SSI263 SC02 CORE PASS");
        end else begin
            $display("SSI263 SC02 CORE FAIL count=%0d", failures);
            $fatal(1);
        end
        $finish;
    end

endmodule
