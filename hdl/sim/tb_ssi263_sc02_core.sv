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
    logic filter_phase_ce;
    logic filter_phase;
    logic [2:0] selector;
    logic [1:0] selector_phase;
    logic selector_step_ce;
    logic [7:0] selector_rom_data;
    logic [3:0] selector_flags;
    logic phone_fricative;
    logic phone_voiced;
    logic pw_0;
    logic pw_1;
    logic pw_2;
    logic pw_3;
    logic pw_5;
    logic fric1_sw;
    logic fric2_sw;
    logic fricative;
    logic voiced;
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
        .filter_phase_ce(filter_phase_ce),
        .filter_phase(filter_phase),
        .selector(selector),
        .selector_phase(selector_phase),
        .selector_step_ce(selector_step_ce),
        .selector_rom_data(selector_rom_data),
        .selector_flags(selector_flags),
        .phone_fricative(phone_fricative),
        .phone_voiced(phone_voiced),
        .pw_0(pw_0),
        .pw_1(pw_1),
        .pw_2(pw_2),
        .pw_3(pw_3),
        .pw_5(pw_5),
        .fric1_sw(fric1_sw),
        .fric2_sw(fric2_sw),
        .fricative(fricative),
        .voiced(voiced),
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
        .filter_phase_ce(),
        .filter_phase(),
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
        .fric1_sw(),
        .fric2_sw(),
        .fricative(),
        .voiced(),
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
            check(pw_3 == code[1], "lower code PW3");
            check(pw_5 == !code[2], "lower code PW5");
            check(fric1_sw == code[3], "lower code FRIC1");
            check(fric2_sw == !code[3], "lower code FRIC2");
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

        // Sheet 3: a selector lasts 16 effective FASTCLK ticks. Parameter RAM
        // state must not move until U94 launches an articulation sweep.
        reset_chips();
        raw_xck_edges(15);
        check(selector == 3'd0, "selector advanced before 16 ticks");
        raw_xck_edges(1);
        check(selector == 3'd1 && f1_code == 4'd0,
               "selector moved a parameter without a sweep");

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
        check(pitch_events_seen - pitch_before == 2,
               "U62 final pitch cadence");

        // U52C is U49 terminal count gated by U43B Q. With FF=FF, its
        // one-clock synchronous form occurs every two effective ticks.
        reset_chips();
        write_register(3'd4, 8'hFF);
        wait_closure_pulse(first_wait);
        wait_closure_pulse(second_wait);
        check(first_wait == 2 && second_wait == 2,
               "CLOSURE U49/U43 cadence");

        // PW3 is held phone state. U41C still produces recurring positive
        // edges from the SEL1/U62 qualification; it is not a PW3 edge clock.
        reset_chips();
        write_register(3'd1, 8'hFF);
        write_register(3'd2, 8'hFF);
        write_register(3'd0, 8'hB0);
        write_register(3'd3, 8'h70);
        raw_xck_edges(17000);
        check(pw_3 && fric_amp_code != 4'd0,
               "noise test controls did not settle");
        noise_before = noise_edges_seen;
        raw_xck_edges(128);
        check(noise_edges_seen - noise_before > 2,
               "held PW3 did not produce sustained U41C edges");

        // Selector 3 feeds both F3 and F4. Selector 7 is absent from the
        // seven-bit sweep mask, so a complete sweep cannot write it.
        reset_chips();
        write_register(3'd2, 8'hF0);
        write_register(3'd3, 8'hF0);
        wait_articulation_pulse(first_wait);
        raw_xck_edges(128);
        check(f1_code == 4'd1, "selector 0 sweep write");
        check(f3_code == 4'd1 && f4_code == 4'd1,
               "selector 3 must update F3 and F4");
        check(f3_code == f4_code,
               "selector 7 changed only one F3/F4 path");

        // The only low nibbles present in active ROM are 0/1 in slots 0/1
        // and 4/6/8/A/C/E in slot 2. Check every slot-2 code.
        check_lower_code(6'h28, 4'h4);
        check_lower_code(6'h2F, 4'h6);
        check_lower_code(6'h2B, 4'h8);
        check_lower_code(6'h00, 4'hA);
        check_lower_code(6'h24, 4'hC);
        check_lower_code(6'h01, 4'hE);

        reset_chips();
        write_register(3'd0, 8'h27);
        raw_xck_edges(32);
        check(phone_fricative && !phone_voiced && pw_0 && !pw_1,
               "fricative lower-ROM source flag");
        reset_chips();
        write_register(3'd0, 8'h01);
        raw_xck_edges(32);
        check(!phone_fricative && phone_voiced && !pw_0 && pw_1,
               "voiced lower-ROM source flag");

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
