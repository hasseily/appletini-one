`timescale 1ns / 1ps

// Integrated native SSI-263/SC-02 phone regression.  The bus writes enter
// ssi263_voice, so every source and filter control used below comes from the
// real core and its 512-byte ROM before reaching the audio block.
module tb_ssi263_phone_sweep;

    localparam integer FABRIC_HZ = 133333344;
    localparam integer RAW_XCK_HZ = 2045454;
    localparam integer AUDIO_HZ = 48000;
    localparam integer OBSERVE_SAMPLES = 384;

    logic clk = 1'b0;
    logic rstn = 1'b0;
    logic apple_res = 1'b1;
    logic card_enabled = 1'b1;
    logic xck_run = 1'b0;
    logic xck_ce;
    logic audio_tick = 1'b0;
    logic ssi_write_active = 1'b0;
    logic [2:0] ssi_reg = 3'd0;
    logic [7:0] ssi_wdata = 8'd0;
    logic ssi_d7;
    logic ar_drive_low;
    logic signed [15:0] audio;
    logic dbg_backend_done;
    logic dbg_enable_ints;

    logic [27:0] xck_accumulator_q = 28'd0;
    logic [27:0] audio_accumulator_q = 28'd0;
    logic [7:0] expected_rom [0:511];

    integer failures = 0;
    integer checks = 0;
    integer phone_positive_rails [0:63];
    integer phone_negative_rails [0:63];
    integer phone_unknowns [0:63];
    integer phone_reconstruction_max [0:63];
    integer phone_reconstruction_min [0:63];

    integer phone_index;
    integer transient_positive_rails;
    integer transient_negative_rails;
    integer transient_unknowns;
    integer transient_reconstruction_max;
    integer transient_reconstruction_min;
    integer active_phone_count = 0;
    integer pw0_set_events = 0;
    integer pw1_set_events = 0;
    integer pw3_load_events = 0;
    integer u96_hold_events = 0;
    integer u96_write_events = 0;
    integer u96_voice_write_events = 0;
    integer u96_fric_write_events = 0;
    integer target_checks = 0;
    integer ampzero_target_checks = 0;
    integer nonzero_fric_phone_count = 0;
    integer noise_rise_events = 0;
    integer noise_fall_events = 0;
    integer routed_fric_events = 0;

    always #5 clk = ~clk;

    // Keep the real fabric-to-Q3 ratio. The audio engine is pipelined in the
    // 133 MHz domain, so treating every fabric clock as an XCK edge would
    // create an impossible card input rate and could hide a real FIFO check
    // behind a test-only overrun.
    always_ff @(posedge clk) begin
        if (!rstn || !xck_run) begin
            xck_accumulator_q <= 28'd0;
            audio_accumulator_q <= 28'd0;
            xck_ce <= 1'b0;
            audio_tick <= 1'b0;
        end else begin
            if (xck_accumulator_q >= FABRIC_HZ - RAW_XCK_HZ) begin
                xck_accumulator_q <=
                    xck_accumulator_q + RAW_XCK_HZ - FABRIC_HZ;
                xck_ce <= 1'b1;
            end else begin
                xck_accumulator_q <= xck_accumulator_q + RAW_XCK_HZ;
                xck_ce <= 1'b0;
            end

            if (audio_accumulator_q >= FABRIC_HZ - AUDIO_HZ) begin
                audio_accumulator_q <=
                    audio_accumulator_q + AUDIO_HZ - FABRIC_HZ;
                audio_tick <= 1'b1;
            end else begin
                audio_accumulator_q <= audio_accumulator_q + AUDIO_HZ;
                audio_tick <= 1'b0;
            end
        end
    end

    ssi263_voice dut (
        .clk(clk),
        .rstn(rstn),
        .apple_res(apple_res),
        .card_enabled(card_enabled),
        .audio_tick(audio_tick),
        .xck_ce(xck_ce),
        .ssi_write_active(ssi_write_active),
        .ssi_reg(ssi_reg),
        .ssi_wdata(ssi_wdata),
        .ssi_d7(ssi_d7),
        .ar_drive_low(ar_drive_low),
        .audio(audio),
        .dbg_backend_done(dbg_backend_done),
        .dbg_enable_ints(dbg_enable_ints)
    );

    task automatic check(input logic condition, input string message);
        begin
            checks = checks + 1;
            if (condition !== 1'b1) begin
                failures = failures + 1;
                $display("SSI263 PHONE SWEEP FAIL: %s", message);
            end
        end
    endtask

    task automatic reset_dut;
        begin
            @(negedge clk);
            rstn = 1'b0;
            xck_run = 1'b0;
            apple_res = 1'b1;
            card_enabled = 1'b1;
            ssi_write_active = 1'b0;
            repeat (8) @(negedge clk);
            rstn = 1'b1;
            repeat (3) @(negedge clk);
        end
    endtask

    task automatic write_register(
        input logic [2:0] address,
        input logic [7:0] value
    );
        begin
            @(negedge clk);
            ssi_reg = address;
            ssi_wdata = value;
            ssi_write_active = 1'b1;
            repeat (2) @(negedge clk);
            ssi_write_active = 1'b0;
            repeat (2) @(negedge clk);
        end
    endtask

    task automatic write_phone(input logic [5:0] phone);
        begin
            write_register(3'd0, {2'b01, phone});
        end
    endtask

    task automatic start_phone(input logic [5:0] phone);
        begin
            // Known full-level driver vector: I=$A00, R=$F, ART=$7, AMP=$F,
            // and FILT=$E8.  Keep XCK stopped until FILT is below the engine's
            // minimum safe phase interval.
            reset_dut();
            write_register(3'd1, 8'h40);
            write_register(3'd2, 8'hF8);
            write_register(3'd4, 8'hE8);
            write_phone(phone);
            write_register(3'd3, 8'h7F);
            @(negedge clk);
            xck_run = 1'b1;
        end
    endtask

    task automatic note_pcm_state(
        inout integer positive_rails,
        inout integer negative_rails,
        inout integer unknowns,
        inout integer reconstruction_max,
        inout integer reconstruction_min
    );
        integer value;
        integer reconstruction;
        begin
            if ($isunknown({audio, dut.audio_i.reconstruction_hold_q})) begin
                unknowns = unknowns + 1;
            end else begin
                value = $signed(audio);
                reconstruction =
                    $signed(dut.audio_i.reconstruction_hold_q);
                if (value == 32767)
                    positive_rails = positive_rails + 1;
                if (value == -32768)
                    negative_rails = negative_rails + 1;
                if (reconstruction > reconstruction_max)
                    reconstruction_max = reconstruction;
                if (reconstruction < reconstruction_min)
                    reconstruction_min = reconstruction;
            end
        end
    endtask

    task automatic wait_phone_controls(
        input logic [5:0] phone,
        output integer positive_rails,
        output integer negative_rails,
        output integer unknowns,
        output integer reconstruction_max,
        output integer reconstruction_min
    );
        integer timeout;
        integer row;
        logic [7:0] selectors_seen;
        logic controls_match;
        begin
            timeout = 0;
            positive_rails = 0;
            negative_rails = 0;
            unknowns = 0;
            reconstruction_max = -8_388_608;
            reconstruction_min = 8_388_607;
            row = phone * 8;
            selectors_seen = 8'h00;
            controls_match = 1'b0;
            while (!controls_match && timeout < 8192) begin
                @(posedge clk);
                #1;
                if (audio_tick)
                    note_pcm_state(positive_rails, negative_rails, unknowns,
                                   reconstruction_max, reconstruction_min);
                if (dut.core_i.phone_active &&
                    dut.core_i.phoneme == phone)
                    selectors_seen[dut.core_i.selector_q] = 1'b1;

                // U37/U38 and U32/U33/U34 make PW0, PW1, and PW3 timed
                // state. They need not, and usually do not, equal the low
                // ROM bits as soon as a phone is written. PW2/PW5 are the
                // only pair loaded directly in the slot-2 LATCH window.
                controls_match =
                    dut.core_i.phone_active &&
                    dut.core_i.phoneme == phone &&
                    selectors_seen == 8'hFF &&
                    !dut.core_i.phone_setup_pending_q &&
                    !dut.core_i.phone_setup_window_q &&
                    dut.core_i.pw_2_q == expected_rom[row + 2][2] &&
                    dut.core_i.pw_5_q == !expected_rom[row + 2][2];
                if (xck_ce)
                    timeout = timeout + 1;
            end
            check(timeout < 8192,
                  $sformatf("phone %02h setup did not cross a full scan",
                            phone));
        end
    endtask

    // Check the two U79/U78 amplitude-target gates through the bus-visible
    // core. Phone $2F has nonzero voice and fricative ROM targets, so zeroing
    // host amplitude must mask both targets without demanding an immediate
    // change from the U83/U84 transition RAM.
    task automatic check_ampzero_targets;
        integer timeout;
        logic voice_target_seen;
        logic fric_target_seen;
        begin
            start_phone(6'h2F);
            wait_phone_controls(6'h2F,
                                transient_positive_rails,
                                transient_negative_rails,
                                transient_unknowns,
                                transient_reconstruction_max,
                                transient_reconstruction_min);
            check(expected_rom[(6'h2F * 8) + 5][7:4] != 4'd0 &&
                  expected_rom[(6'h2F * 8) + 6][7:4] != 4'd0,
                  "AMPZERO vector does not have two nonzero ROM targets");
            write_register(3'd3, 8'h70);
            timeout = 0;
            voice_target_seen = 1'b0;
            fric_target_seen = 1'b0;
            while (!(voice_target_seen && fric_target_seen) &&
                   timeout < 2048) begin
                @(posedge clk);
                #1;
                if (!voice_target_seen &&
                    dut.core_i.phoneme == 6'h2F &&
                    dut.core_i.selector_q == 3'd5) begin
                    check(dut.core_i.selector_target == 4'd0,
                          "AMPZERO did not mask the voice target");
                    voice_target_seen = 1'b1;
                    ampzero_target_checks = ampzero_target_checks + 1;
                end
                if (!fric_target_seen &&
                    dut.core_i.phoneme == 6'h2F &&
                    dut.core_i.selector_q == 3'd6) begin
                    check(dut.core_i.selector_target == 4'd0,
                          "AMPZERO did not mask the fricative target");
                    fric_target_seen = 1'b1;
                    ampzero_target_checks = ampzero_target_checks + 1;
                end
                if (xck_ce)
                    timeout = timeout + 1;
            end
            check(voice_target_seen && fric_target_seen,
                  "AMPZERO target slots did not appear in one scan");
        end
    endtask

    task automatic wait_audio_sample;
        begin
            @(posedge clk);
            #1;
            while (!audio_tick) begin
                @(posedge clk);
                #1;
            end
        end
    endtask

    task automatic measure_phone(
        input integer phone,
        input integer prior_positive_rails,
        input integer prior_negative_rails,
        input integer prior_unknowns,
        input integer prior_reconstruction_max,
        input integer prior_reconstruction_min
    );
        integer sample_count;
        integer positive_rails;
        integer negative_rails;
        integer unknowns;
        integer reconstruction_max;
        integer reconstruction_min;
        begin
            positive_rails = prior_positive_rails;
            negative_rails = prior_negative_rails;
            unknowns = prior_unknowns;
            reconstruction_max = prior_reconstruction_max;
            reconstruction_min = prior_reconstruction_min;
            for (sample_count = 0;
                 sample_count < OBSERVE_SAMPLES;
                 sample_count = sample_count + 1) begin
                wait_audio_sample();
                note_pcm_state(positive_rails, negative_rails, unknowns,
                               reconstruction_max, reconstruction_min);
            end

            phone_positive_rails[phone] = positive_rails;
            phone_negative_rails[phone] = negative_rails;
            phone_unknowns[phone] = unknowns;
            phone_reconstruction_max[phone] = reconstruction_max;
            phone_reconstruction_min[phone] = reconstruction_min;
            if (reconstruction_max > reconstruction_min)
                active_phone_count = active_phone_count + 1;
            if (dut.core_i.fric_amp_code != 0)
                nonzero_fric_phone_count = nonzero_fric_phone_count + 1;

            $display("SSI263 ROM ROW phone=%02h pw=%0d%0d%0d%0d%0d voice_amp=%0h fric_amp=%0h recon_min=%0d recon_max=%0d positive_rails=%0d negative_rails=%0d unknowns=%0d",
                     phone[5:0], dut.core_i.pw_5, dut.core_i.pw_3,
                     dut.core_i.pw_2, dut.core_i.pw_1, dut.core_i.pw_0,
                     dut.core_i.voice_amp_code, dut.core_i.fric_amp_code,
                     reconstruction_min, reconstruction_max,
                     positive_rails, negative_rails, unknowns);

            // The explicit signed-16 limiter may reach either output rail.
            // Report both counts above; use the wider reconstructed AO state
            // below to distinguish that boundary limit from numeric overflow.
            check(unknowns == 0,
                  $sformatf("phone %02h produced unknown PCM", phone));
            check(reconstruction_max < 8_388_607 &&
                  reconstruction_min > -8_388_608,
                  $sformatf("phone %02h reached a 24-bit internal rail",
                            phone));
            check(!dut.audio_i.engine_overrun_q,
                  $sformatf("phone %02h overran the audio engine", phone));
        end
    endtask

    // Cross the core/audio boundary: require the real U41C rise and fall
    // pulses, and require at least one routed nonzero fricative event to enter
    // the charge engine. The audio bench separately proves a voice-zero
    // FRIC1 event reaches U148.
    always @(negedge clk) begin
        if (rstn) begin
            if (dut.core_i.noise_clock_ce_q)
                noise_rise_events = noise_rise_events + 1;
            if (dut.core_i.noise_shift_ce_q)
                noise_fall_events = noise_fall_events + 1;
            if (dut.audio_i.engine_busy_q &&
                dut.audio_i.active_event_q.fric_mask != 0 &&
                (dut.audio_i.active_event_q.fric1_route ||
                 dut.audio_i.active_event_q.fric2_route))
                routed_fric_events = routed_fric_events + 1;
        end
    end

    // Sample the exact inputs seen by the core's sequential logic, then check
    // its outputs after nonblocking assignments settle. This checks the timed
    // PW latches and checks every U83/U84 A/B/C/CY update or hold.
    always @(posedge clk) begin : schematic_timing_monitor
        integer ram_index;
        integer sampled_selector;
        logic sampled_effective_xck;
        logic sampled_selector_write;
        logic sampled_transition_clear;
        logic sampled_phone_pending;
        logic sampled_resa_equal;
        logic sampled_u96_permit;
        logic sampled_write_commit;
        logic expected_u96_permit;
        logic sampled_pw0;
        logic sampled_pw1;
        logic sampled_pw3;
        logic sampled_pw0_set;
        logic sampled_pw1_set;
        logic sampled_pw3_load;
        logic sampled_phone_write_low;
        logic sampled_pw3_data;
        logic [3:0] sampled_resa [0:7];
        logic [3:0] sampled_resb [0:7];
        logic [3:0] sampled_resc [0:7];
        logic [7:0] sampled_rescy;
        logic [3:0] expected_resa;
        logic [3:0] expected_resb;
        logic [3:0] expected_resc;
        logic       expected_rescy;
        logic [4:0] expected_bc_sum;
        logic [3:0] expected_target;
        logic [3:0] sampled_target;
        logic       expected_parameter_write;

        if (rstn) begin
            sampled_selector = dut.core_i.selector_q;
            sampled_effective_xck = dut.core_i.effective_xck_ce;
            sampled_selector_write = dut.core_i.selector_write_rise;
            sampled_transition_clear = dut.core_i.transition_a_clr;
            sampled_phone_pending = dut.core_i.phone_setup_pending_q;
            sampled_resa_equal = dut.core_i.transition_resa_equal;
            sampled_u96_permit = dut.core_i.u96_write_permit;
            sampled_write_commit = dut.core_i.write_commit;
            sampled_pw0 = dut.core_i.pw_0_q;
            sampled_pw1 = dut.core_i.pw_1_q;
            sampled_pw3 = dut.core_i.pw_3_q;
            sampled_pw0_set = dut.core_i.pw0_set_level;
            sampled_pw1_set = dut.core_i.pw1_set_level;
            sampled_pw3_load = dut.core_i.pw3_load_level;
            sampled_phone_write_low = dut.core_i.phone_write_active;
            sampled_pw3_data = dut.core_i.u183a_q ||
                               !dut.core_i.selector_flags[1];
            sampled_rescy = dut.core_i.parameter_rescy_q;
            sampled_target = dut.core_i.selector_target;
            for (ram_index = 0; ram_index < 8; ram_index = ram_index + 1) begin
                sampled_resa[ram_index] =
                    dut.core_i.parameter_resa_q[ram_index];
                sampled_resb[ram_index] =
                    dut.core_i.parameter_resb_q[ram_index];
                sampled_resc[ram_index] =
                    dut.core_i.parameter_resc_q[ram_index];
            end

            case (sampled_selector)
                0, 1, 3:
                    expected_u96_permit = dut.core_i.tc_edge_window_q &&
                        !(dut.core_i.pw_5_q &&
                          (dut.core_i.duration_phoneme_q[5] ||
                           dut.core_i.voice_amp_code_q != 4'd0 ||
                           dut.core_i.fric_amp_code_q != 4'd0));
                2:
                    expected_u96_permit = dut.core_i.tc_edge_window_q;
                4:
                    expected_u96_permit =
                        dut.core_i.duration_edge_window_q &&
                        dut.core_i.u166b_nq_q &&
                        dut.core_i.control_articulation_amplitude_q[3:0] !=
                        4'd0;
                5:
                    expected_u96_permit = dut.core_i.pw_0_q &&
                        dut.core_i.u93_rate_q1 &&
                        !dut.core_i.u93_rate_q2;
                6:
                    expected_u96_permit = dut.core_i.pw_1_q &&
                        dut.core_i.u93_rate_q1 &&
                        !dut.core_i.u93_rate_q2;
                default:
                    expected_u96_permit = 1'b0;
            endcase

            if (sampled_selector == 4)
                expected_target =
                    dut.core_i.control_articulation_amplitude_q[3:0];
            else if ((sampled_selector == 5 || sampled_selector == 6) &&
                     dut.core_i.control_articulation_amplitude_q[3:0] == 0)
                expected_target = 4'd0;
            else
                expected_target =
                    expected_rom[(dut.core_i.phoneme * 8) +
                                 sampled_selector][7:4];

            #1;

            if (sampled_pw0_set || sampled_phone_write_low ||
                dut.core_i.pw_0_q != sampled_pw0) begin
                check(dut.core_i.pw_0_q ==
                      (sampled_pw0_set ? 1'b1 :
                       (sampled_phone_write_low ? 1'b0 : sampled_pw0)),
                      "U32 timed PW0 latch update mismatch");
                if (sampled_pw0_set)
                    pw0_set_events = pw0_set_events + 1;
            end
            if (sampled_pw1_set || sampled_phone_write_low ||
                dut.core_i.pw_1_q != sampled_pw1) begin
                check(dut.core_i.pw_1_q ==
                      (sampled_pw1_set ? 1'b1 :
                       (sampled_phone_write_low ? 1'b0 : sampled_pw1)),
                      "U33 timed PW1 latch update mismatch");
                if (sampled_pw1_set)
                    pw1_set_events = pw1_set_events + 1;
            end
            if (sampled_pw3_load || dut.core_i.pw_3_q != sampled_pw3) begin
                check(dut.core_i.pw_3_q ==
                      (sampled_pw3_load ? sampled_pw3_data : sampled_pw3),
                      "U34 timed PW3 latch update mismatch");
                if (sampled_pw3_load)
                    pw3_load_events = pw3_load_events + 1;
            end

            if (sampled_effective_xck && sampled_selector_write) begin
                check(sampled_u96_permit == expected_u96_permit,
                      "U96 write-mux truth table mismatch");
                check(sampled_target == expected_target,
                      "schematic selector target mismatch");
                target_checks = target_checks + 1;

                expected_resa = sampled_resa[sampled_selector];
                expected_resb = sampled_resb[sampled_selector];
                expected_resc = sampled_resc[sampled_selector];
                expected_rescy = sampled_rescy[sampled_selector];
                expected_parameter_write = 1'b0;
                if (!sampled_write_commit && sampled_transition_clear) begin
                    expected_resb = sampled_target - expected_resa;
                    expected_resc = 4'd8;
                    expected_rescy = sampled_target >= expected_resa;
                end else if (!sampled_write_commit &&
                             !sampled_phone_pending &&
                             sampled_u96_permit &&
                             !sampled_resa_equal) begin
                    expected_bc_sum =
                        {1'b0, sampled_resb[sampled_selector]} +
                        {1'b0, sampled_resc[sampled_selector]};
                    expected_resc = expected_bc_sum[3:0];
                    if (sampled_rescy[sampled_selector] &&
                        expected_bc_sum[4])
                        expected_resa = expected_resa + 4'd1;
                    else if (!sampled_rescy[sampled_selector] &&
                             !expected_bc_sum[4])
                        expected_resa = expected_resa - 4'd1;
                    expected_parameter_write = 1'b1;
                end

                check(dut.core_i.parameter_resa_q[sampled_selector] ==
                          expected_resa &&
                      dut.core_i.parameter_resb_q[sampled_selector] ==
                          expected_resb &&
                      dut.core_i.parameter_resc_q[sampled_selector] ==
                          expected_resc &&
                      dut.core_i.parameter_rescy_q[sampled_selector] ==
                          expected_rescy,
                      "U83/U84 DDA selected-state mismatch");
                check(dut.core_i.parameter_write_ce_q ==
                          expected_parameter_write,
                      "U83/U84 parameter_write_ce mismatch");

                if (!sampled_transition_clear &&
                    !sampled_phone_pending && !sampled_u96_permit &&
                    !sampled_write_commit) begin
                    u96_hold_events = u96_hold_events + 1;
                end

                for (ram_index = 0; ram_index < 8;
                     ram_index = ram_index + 1) begin
                    if (ram_index != sampled_selector)
                        check(dut.core_i.parameter_resa_q[ram_index] ==
                                  sampled_resa[ram_index] &&
                              dut.core_i.parameter_resb_q[ram_index] ==
                                  sampled_resb[ram_index] &&
                              dut.core_i.parameter_resc_q[ram_index] ==
                                  sampled_resc[ram_index] &&
                              dut.core_i.parameter_rescy_q[ram_index] ==
                                  sampled_rescy[ram_index],
                              "WRITE changed nonselected U83/U84 state");
                end

                check(dut.core_i.parameter_resa_q[7] == sampled_resa[7],
                      "grounded U96 selector 7 changed RESA7");
            end

            if (dut.core_i.parameter_write_ce_q) begin
                check(sampled_effective_xck && sampled_selector_write &&
                      !sampled_transition_clear &&
                      !sampled_phone_pending && sampled_u96_permit &&
                      !sampled_resa_equal,
                      "parameter_write_ce escaped the U96 write gate");
                u96_write_events = u96_write_events + 1;
                if (sampled_selector == 5)
                    u96_voice_write_events = u96_voice_write_events + 1;
                if (sampled_selector == 6)
                    u96_fric_write_events = u96_fric_write_events + 1;
            end
        end
    end

    initial begin
        $readmemh("ssi263_sc02_rom.mem", expected_rom);

        check_ampzero_targets();
        start_phone(6'h00);
        for (phone_index = 0; phone_index < 64;
             phone_index = phone_index + 1) begin
            write_phone(phone_index[5:0]);
            wait_phone_controls(phone_index[5:0],
                                transient_positive_rails,
                                transient_negative_rails,
                                transient_unknowns,
                                transient_reconstruction_max,
                                transient_reconstruction_min);
            measure_phone(phone_index, transient_positive_rails,
                          transient_negative_rails, transient_unknowns,
                          transient_reconstruction_max,
                          transient_reconstruction_min);
        end

        check(pw0_set_events > 0 && pw1_set_events > 0 &&
              pw3_load_events > 0,
              "64-phone sweep did not exercise all timed PW latches");
        check(u96_hold_events > 0,
              "64-phone sweep did not exercise a U96 hold");
        check(u96_write_events > 0,
              "64-phone sweep did not exercise a U96 write");
        check(u96_voice_write_events > 0,
              "64-phone sweep did not exercise the voice U96 write");
        check(u96_fric_write_events > 0,
              "64-phone sweep did not exercise the fricative U96 write");
        check(target_checks > 0 && ampzero_target_checks == 2,
              "64-phone sweep did not check schematic targets");
        check(active_phone_count > 0,
              "64-phone sweep produced no reconstructed audio activity");
        check(nonzero_fric_phone_count > 0 &&
              noise_rise_events > 0 && noise_fall_events > 0 &&
              routed_fric_events > 0,
              "64-phone sweep did not carry fricative state into audio");

        if (failures == 0) begin
            $display("SSI263 PHONE SWEEP PASS (%0d checks, 64 phones)", checks);
        end else begin
            $display("SSI263 PHONE SWEEP FAIL (%0d failures, %0d checks)",
                     failures, checks);
            $fatal(1);
        end
        $finish;
    end

endmodule
