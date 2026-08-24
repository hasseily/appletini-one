`timescale 1ns / 1ps

// Card-level regression for the two fixed SSI-263AP sockets in the virtual
// Phasor.  All register changes enter through AppleBus_read.  Hierarchical
// taps only observe the two real voice/core/audio instances.
module tb_phasor_dual_ssi263;

    localparam logic [2:0] SLOT = 3'd4;
    localparam logic [15:0] SLOT_BASE = 16'hC400;
    localparam logic [15:0] MODE_MB = 16'hC0C8;
    localparam logic [15:0] MODE_NATIVE = 16'hC0CD;
    localparam logic [15:0] MODE_ECHO = 16'hC0CF;

    logic clk = 1'b0;
    logic rstn = 1'b0;
    globals::AppleBus_read ab_read = '0;
    globals::SoftSwitchState sss = '0;
    globals::AppleBus_write ab_write;
    logic [2:0] slot_assign = SLOT;
    logic card_enable = 1'b1;
    logic [47:0] pan = 48'h5555_5555_5555;
    logic [31:0] audio_control = 32'h0204_0000;
    logic audio_sample_tick = 1'b0;
    logic signed [15:0] audio_l;
    logic signed [15:0] audio_r;
    logic dbg_ssi_irq;
    logic dbg_ssi_backend_done;
    logic dbg_ssi_enable_ints;

    logic [11:0] sample_div_q = 12'd0;
    integer failures = 0;
    integer checks = 0;
    integer effective_tick_total = 0;
    integer hcc_shifts = 0;
    integer voiced_reloads = 0;

    always #5 clk = ~clk;

    always_ff @(posedge clk) begin
        if (!rstn) begin
            sample_div_q <= 12'd0;
            audio_sample_tick <= 1'b0;
        end else if (sample_div_q == 12'd2777) begin
            sample_div_q <= 12'd0;
            audio_sample_tick <= 1'b1;
        end else begin
            sample_div_q <= sample_div_q + 12'd1;
            audio_sample_tick <= 1'b0;
        end
    end

    mockingboard dut (
        .clk(clk),
        .rstn(rstn),
        .ab_read(ab_read),
        .sss(sss),
        .slot_assign(slot_assign),
        .card_enable(card_enable),
        .pan(pan),
        .audio_control(audio_control),
        .audio_sample_tick(audio_sample_tick),
        .ab_write(ab_write),
        .audio_l(audio_l),
        .audio_r(audio_r),
        .dbg_ssi_irq(dbg_ssi_irq),
        .dbg_ssi_backend_done(dbg_ssi_backend_done),
        .dbg_ssi_enable_ints(dbg_ssi_enable_ints)
    );

    task automatic check(input logic condition, input string message);
        begin
            checks = checks + 1;
            if (condition !== 1'b1) begin
                failures = failures + 1;
                $display("PHASOR DUAL SSI263 FAIL: %s", message);
            end
        end
    endtask

    task automatic drive_idle;
        begin
            ab_read.addr = 16'h0000;
            ab_read.data = 8'h00;
            ab_read.rw = 1'b1;
            ab_read.data_en = 1'b0;
            ab_read.addr_en = 1'b0;
            ab_read.sss_en = 1'b0;
            ab_read.serve_en = 1'b0;
            ab_read.drive_en = 1'b0;
            ab_read.cycle_valid = 1'b1;
            sss.slot_access = 1'b0;
        end
    endtask

    task automatic hard_reset;
        begin
            @(negedge clk);
            drive_idle();
            ab_read.res = 1'b1;
            slot_assign = SLOT;
            card_enable = 1'b1;
            rstn = 1'b0;
            repeat (6) @(posedge clk);
            @(negedge clk);
            rstn = 1'b1;
            repeat (6) @(posedge clk);
            #1;
        end
    endtask

    task automatic mode_access(input logic [15:0] address);
        begin
            @(negedge clk);
            ab_read.addr = address;
            ab_read.data = 8'h00;
            ab_read.rw = 1'b1;
            ab_read.serve_en = 1'b1;
            ab_read.data_en = 1'b0;
            ab_read.addr_en = 1'b0;
            ab_read.cycle_valid = 1'b1;
            sss.slot_access = 1'b0;
            @(negedge clk);
            drive_idle();
            ab_read.addr_en = 1'b1;
            @(negedge clk);
            ab_read.addr_en = 1'b0;
            repeat (2) @(posedge clk);
            #1;
        end
    endtask

    task automatic apple_write(input logic [15:0] address,
                               input logic [7:0] value);
        begin
            @(negedge clk);
            ab_read.addr = address;
            ab_read.data = value;
            ab_read.rw = 1'b0;
            ab_read.serve_en = 1'b1;
            ab_read.data_en = 1'b0;
            ab_read.addr_en = 1'b0;
            ab_read.cycle_valid = 1'b1;
            sss.slot_access = 1'b1;
            @(negedge clk);
            ab_read.serve_en = 1'b0;
            ab_read.data_en = 1'b1;
            @(negedge clk);
            drive_idle();
            // The SSI latches on this selected-write falling edge.
            repeat (2) @(posedge clk);
            #1;
        end
    endtask

    task automatic apple_read(input logic [15:0] address,
                              input logic [6:0] floating_low,
                              output logic drove,
                              output logic [7:0] value,
                              output logic speech_selected);
        begin
            @(negedge clk);
            ab_read.addr = address;
            ab_read.data = {1'b0, floating_low};
            ab_read.rw = 1'b1;
            ab_read.serve_en = 1'b1;
            ab_read.data_en = 1'b0;
            ab_read.addr_en = 1'b0;
            ab_read.cycle_valid = 1'b1;
            sss.slot_access = 1'b1;
            @(posedge clk);
            #1;
            drove = ab_write.wr_data_en;
            value = ab_write.wr_data;
            speech_selected = dut.ssi_read_drive;
            @(negedge clk);
            ab_read.serve_en = 1'b0;
            ab_read.data_en = 1'b1;
            @(negedge clk);
            drive_idle();
            ab_read.addr_en = 1'b1;
            @(negedge clk);
            ab_read.addr_en = 1'b0;
            repeat (2) @(posedge clk);
            #1;
        end
    endtask

    task automatic apple_write_reset_collision(input logic [15:0] address,
                                               input logic [7:0] value);
        begin
            @(negedge clk);
            ab_read.addr = address;
            ab_read.data = value;
            ab_read.rw = 1'b0;
            ab_read.serve_en = 1'b1;
            ab_read.data_en = 1'b0;
            ab_read.cycle_valid = 1'b1;
            sss.slot_access = 1'b1;
            @(negedge clk);
            ab_read.serve_en = 1'b0;
            ab_read.data_en = 1'b1;
            // Capture the held address/data and selected-write level.
            @(negedge clk);
            drive_idle();
            // AP PD/RST falls on the same fabric edge that sees write_end.
            ab_read.res = 1'b0;
            repeat (4) @(posedge clk);
            #1;
            @(negedge clk);
            ab_read.res = 1'b1;
            repeat (3) @(posedge clk);
            #1;
        end
    endtask

    task automatic wait_for_both_pending;
        integer timeout;
        begin
            timeout = 0;
            while (!(dut.ssi0_d7 && dut.ssi1_d7) && timeout < 1_000_000) begin
                @(posedge clk);
                timeout = timeout + 1;
            end
            #1;
            check(timeout < 1_000_000,
                  "both real SSI cores did not reach a response boundary");
        end
    endtask

    task automatic wait_for_secondary_fricative;
        integer timeout;
        begin
            timeout = 0;
            while (!(dut.ssi263_secondary_i.core_i.phone_fricative &&
                     dut.ssi263_secondary_i.core_i.fricative) &&
                   timeout < 1_000_000) begin
                @(posedge clk);
                timeout = timeout + 1;
            end
            #1;
            check(timeout < 1_000_000,
                  "secondary held fricative did not reach the audio source");
        end
    endtask

    task automatic wait_for_secondary_voiced;
        integer timeout;
        begin
            timeout = 0;
            while (!(dut.ssi263_secondary_i.core_i.phone_voiced &&
                     dut.ssi263_secondary_i.core_i.voiced) &&
                   timeout < 500_000) begin
                @(posedge clk);
                timeout = timeout + 1;
            end
            #1;
            check(timeout < 500_000,
                  "secondary held voiced phone did not reach the audio source");
        end
    endtask

    task automatic wait_for_primary_voiced;
        integer timeout;
        begin
            timeout = 0;
            while (!(dut.ssi263_primary_i.core_i.phone_voiced &&
                     dut.ssi263_primary_i.core_i.voiced) &&
                   timeout < 500_000) begin
                @(posedge clk);
                timeout = timeout + 1;
            end
            #1;
            check(timeout < 500_000,
                  "primary held voiced phone did not reach the audio source");
        end
    endtask

    task automatic wait_for_a5_left_audio;
        integer timeout;
        integer chip_peak;
        integer card_peak;
        integer chip_level;
        integer card_level;
        integer level_samples;
        logic saw_chip_audio;
        logic saw_left_audio;
        logic saw_filter_state;
        logic leaked_right;
        begin
            timeout = 0;
            saw_chip_audio = 1'b0;
            saw_left_audio = 1'b0;
            saw_filter_state = 1'b0;
            leaked_right = 1'b0;
            chip_peak = 0;
            card_peak = 0;
            level_samples = 0;
            while (!(saw_chip_audio && saw_left_audio && saw_filter_state) &&
                   timeout < 3_000_000) begin
                @(posedge clk);
                #1;
                if (dut.ssi0_audio != 16'sd0)
                    saw_chip_audio = 1'b1;
                if (audio_l != 16'sd0)
                    saw_left_audio = 1'b1;
                if (dut.ssi263_secondary_i.audio_i.f1_state_q != 24'sd0 &&
                    dut.ssi263_secondary_i.audio_i.f5_state_q != 24'sd0)
                    saw_filter_state = 1'b1;
                if (dut.ssi1_audio != 16'sd0 || audio_r != 16'sd0 ||
                    dut.ssi263_primary_i.audio_i.f1_state_q != 24'sd0 ||
                    dut.ssi263_primary_i.audio_i.f5_state_q != 24'sd0)
                    leaked_right = 1'b1;
                timeout = timeout + 1;
            end
            while (level_samples < 512 && timeout < 3_000_000) begin
                @(posedge clk);
                #1;
                if (dut.audio_sample_tick) begin
                    chip_level = dut.ssi0_audio;
                    card_level = audio_l;
                    if (chip_level < 0)
                        chip_level = -chip_level;
                    if (card_level < 0)
                        card_level = -card_level;
                    if (chip_level > chip_peak)
                        chip_peak = chip_level;
                    if (card_level > card_peak)
                        card_peak = card_level;
                    level_samples = level_samples + 1;
                end
                timeout = timeout + 1;
            end
            $display("PHASOR SSI263 LEVEL A5 chip=%0d card=%0d samples=%0d",
                     chip_peak, card_peak, level_samples);
            check(chip_peak >= 16'sd2000 && card_peak >= 16'sd1500 &&
                  chip_peak < 16'sd32767 && card_peak < 16'sd32767,
                  "settled A5 voice missed its audible unclipped range");
            check(timeout < 3_000_000 && saw_chip_audio && saw_left_audio &&
                  saw_filter_state && !leaked_right,
                  "A5 speech did not remain on the left channel only");
            check(!dut.ssi263_secondary_i.audio_i.engine_overrun_q &&
                  !dut.ssi263_primary_i.audio_i.engine_overrun_q,
                  "A5-only speech overran an actual audio engine");
        end
    endtask

    task automatic measure_a5_fricative_level;
        integer timeout;
        integer chip_peak;
        integer card_peak;
        integer chip_level;
        integer card_level;
        integer level_samples;
        begin
            timeout = 0;
            chip_peak = 0;
            card_peak = 0;
            level_samples = 0;
            while (level_samples < 512 && timeout < 3_000_000) begin
                @(posedge clk);
                #1;
                if (dut.audio_sample_tick) begin
                    chip_level = dut.ssi0_audio;
                    card_level = audio_l;
                    if (chip_level < 0)
                        chip_level = -chip_level;
                    if (card_level < 0)
                        card_level = -card_level;
                    if (chip_level > chip_peak)
                        chip_peak = chip_level;
                    if (card_level > card_peak)
                        card_peak = card_level;
                    level_samples = level_samples + 1;
                end
                timeout = timeout + 1;
            end
            $display("PHASOR SSI263 LEVEL fric chip=%0d card=%0d samples=%0d",
                     chip_peak, card_peak, level_samples);
            check(level_samples == 512,
                  "fricative level window did not collect every sample");
            check(chip_peak >= 16'sd5000 && card_peak >= 16'sd4000 &&
                  chip_peak < 16'sd32767 && card_peak < 16'sd32767,
                  "settled A5 fricative missed its audible unclipped range");
        end
    endtask

    task automatic wait_for_a6_right_audio;
        integer timeout;
        logic saw_chip_audio;
        logic saw_right_audio;
        logic saw_filter_state;
        logic leaked_left;
        begin
            timeout = 0;
            saw_chip_audio = 1'b0;
            saw_right_audio = 1'b0;
            saw_filter_state = 1'b0;
            leaked_left = 1'b0;
            while (!(saw_chip_audio && saw_right_audio && saw_filter_state) &&
                   timeout < 3_000_000) begin
                @(posedge clk);
                #1;
                if (dut.ssi1_audio != 16'sd0)
                    saw_chip_audio = 1'b1;
                if (audio_r != 16'sd0)
                    saw_right_audio = 1'b1;
                if (dut.ssi263_primary_i.audio_i.f1_state_q != 24'sd0 &&
                    dut.ssi263_primary_i.audio_i.f5_state_q != 24'sd0)
                    saw_filter_state = 1'b1;
                if (dut.ssi0_audio != 16'sd0 || audio_l != 16'sd0 ||
                    dut.ssi263_secondary_i.audio_i.f1_state_q != 24'sd0 ||
                    dut.ssi263_secondary_i.audio_i.f5_state_q != 24'sd0)
                    leaked_left = 1'b1;
                timeout = timeout + 1;
            end
            check(timeout < 3_000_000 && saw_chip_audio && saw_right_audio &&
                  saw_filter_state && !leaked_left,
                  "A6 speech did not remain on the right channel only");
            check(!dut.ssi263_secondary_i.audio_i.engine_overrun_q &&
                  !dut.ssi263_primary_i.audio_i.engine_overrun_q,
                  "A6-only speech overran an actual audio engine");
        end
    endtask

    task automatic wait_for_dual_stereo_audio;
        integer timeout;
        logic saw_both_chip_audio;
        logic saw_both_card_audio;
        logic saw_both_filter_states;
        begin
            timeout = 0;
            saw_both_chip_audio = 1'b0;
            saw_both_card_audio = 1'b0;
            saw_both_filter_states = 1'b0;
            while (!(saw_both_chip_audio && saw_both_card_audio &&
                     saw_both_filter_states) && timeout < 3_000_000) begin
                @(posedge clk);
                #1;
                if (dut.ssi0_audio != 16'sd0 && dut.ssi1_audio != 16'sd0)
                    saw_both_chip_audio = 1'b1;
                if (audio_l != 16'sd0 && audio_r != 16'sd0)
                    saw_both_card_audio = 1'b1;
                if (dut.ssi263_secondary_i.audio_i.f1_state_q != 24'sd0 &&
                    dut.ssi263_secondary_i.audio_i.f5_state_q != 24'sd0 &&
                    dut.ssi263_primary_i.audio_i.f1_state_q != 24'sd0 &&
                    dut.ssi263_primary_i.audio_i.f5_state_q != 24'sd0)
                    saw_both_filter_states = 1'b1;
                timeout = timeout + 1;
            end
            check(timeout < 3_000_000 && saw_both_chip_audio &&
                  saw_both_card_audio && saw_both_filter_states &&
                  dut.ssi263_secondary_i.core_i.phone_active &&
                  dut.ssi263_primary_i.core_i.phone_active,
                  "both SSI audio engines did not run independently");
            check(!dut.ssi263_secondary_i.audio_i.engine_overrun_q &&
                  !dut.ssi263_primary_i.audio_i.engine_overrun_q,
                  "simultaneous speech overran an actual audio engine");
        end
    endtask

    task automatic disable_card_during_native_read;
        begin
            @(negedge clk);
            ab_read.addr = SLOT_BASE + 16'h0060;
            ab_read.data = 8'h35;
            ab_read.rw = 1'b1;
            ab_read.serve_en = 1'b1;
            ab_read.data_en = 1'b1;
            ab_read.addr_en = 1'b0;
            ab_read.cycle_valid = 1'b1;
            sss.slot_access = 1'b1;
            @(posedge clk);
            #1;
            check(ab_write.wr_data_en && dut.ssi_read_drive &&
                  ab_write.assert_irq && dbg_ssi_irq,
                  "disable test did not start with an active native read and IRQ");
            check(audio_l != 16'sd0 && audio_r != 16'sd0,
                  "disable test did not start with live stereo speech");

            @(negedge clk);
            drive_idle();
            card_enable = 1'b0;
            repeat (8) @(posedge clk);
            #1;
            check(!dut.card_enabled && !ab_write.wr_data_en &&
                  !ab_write.assert_irq && !dbg_ssi_irq &&
                  !dut.ssi_read_drive,
                  "slot disable did not clear IRQ and registered read drive");
            check(audio_l == 16'sd0 && audio_r == 16'sd0 &&
                  dut.ssi0_audio == 16'sd0 && dut.ssi1_audio == 16'sd0,
                  "slot disable did not mute both card and SSI audio outputs");
            check(dut.phasor_mode_q == 3'd0 &&
                  dut.ssi263_secondary_i.core_i.powered_down &&
                  dut.ssi263_primary_i.core_i.powered_down &&
                  !dut.ssi0_d7 && !dut.ssi1_d7 &&
                  !dut.ssi263_secondary_i.audio_i.engine_busy_q &&
                  !dut.ssi263_primary_i.audio_i.engine_busy_q,
                  "slot disable did not reset both SSI sockets and card mode");

            @(negedge clk);
            card_enable = 1'b1;
            repeat (8) @(posedge clk);
            #1;
            check(dut.card_enabled && dut.phasor_mode_q == 3'd0 &&
                  dut.ssi263_secondary_i.core_i.powered_down &&
                  dut.ssi263_primary_i.core_i.powered_down &&
                  !dut.ssi0_d7 && !dut.ssi1_d7 &&
                  !ab_write.wr_data_en && !ab_write.assert_irq &&
                  audio_l == 16'sd0 && audio_r == 16'sd0,
                  "slot re-enable did not start from clean reset state");

            apple_write(SLOT_BASE + 16'h0021, 8'h52);
            apple_write(SLOT_BASE + 16'h0041, 8'h64);
            check(dut.ssi263_secondary_i.core_i.inflection_high_q == 8'h52 &&
                  dut.ssi263_primary_i.core_i.inflection_high_q == 8'h64 &&
                  !ab_write.assert_irq,
                  "slot re-enable did not restore clean independent bus writes");
        end
    endtask

    task automatic count_hcc_shifts(input integer wanted);
        integer timeout;
        logic advance_now;
        logic [17:0] old_hcc;
        logic [17:0] new_hcc;
        begin
            timeout = 0;
            hcc_shifts = 0;
            while (hcc_shifts < wanted && timeout < 300_000) begin
                @(posedge clk);
                advance_now = dut.ssi263_secondary_i.audio_i.noise_advance;
                old_hcc = {
                    dut.ssi263_secondary_i.audio_i.noise_d1_q,
                    dut.ssi263_secondary_i.audio_i.noise_d2_q,
                    dut.ssi263_secondary_i.audio_i.noise_d3_q,
                    dut.ssi263_secondary_i.audio_i.noise_d4_q
                };
                #1;
                if (advance_now) begin
                    new_hcc = {
                        dut.ssi263_secondary_i.audio_i.noise_d1_q,
                        dut.ssi263_secondary_i.audio_i.noise_d2_q,
                        dut.ssi263_secondary_i.audio_i.noise_d3_q,
                        dut.ssi263_secondary_i.audio_i.noise_d4_q
                    };
                    check(new_hcc != old_hcc,
                          "a gated HCC4006 clock did not shift its state");
                    hcc_shifts = hcc_shifts + 1;
                end
                timeout = timeout + 1;
            end
            check(hcc_shifts >= wanted,
                  "held fricative phone did not cause repeated HCC4006 shifts");
        end
    endtask

    task automatic measure_voiced_period;
        integer timeout;
        integer rising_events;
        integer first_tick;
        integer second_tick;
        integer third_tick;
        integer expected_ticks;
        logic old_toggle;
        logic [3:0] old_shape;
        begin
            timeout = 0;
            rising_events = 0;
            effective_tick_total = 0;
            voiced_reloads = 0;
            first_tick = 0;
            second_tick = 0;
            third_tick = 0;
            old_toggle = dut.ssi263_secondary_i.voice_toggle;
            old_shape = dut.ssi263_secondary_i.audio_i.voice_shape_q;

            // U58/U59 run even while the chip is powered down.  A host write
            // changes the next reload value but does not restart their live
            // countdown, so allow one worst-case I=$000 interval before the
            // first measured I=$FFD event.
            while (rising_events < 3 && timeout < 3_000_000) begin
                @(posedge clk);
                if (dut.ssi263_secondary_i.core_i.effective_xck_ce)
                    effective_tick_total = effective_tick_total + 1;
                #1;
                if (!old_toggle &&
                    dut.ssi263_secondary_i.voice_toggle) begin
                    rising_events = rising_events + 1;
                    case (rising_events)
                        1: first_tick = effective_tick_total;
                        2: second_tick = effective_tick_total;
                        default: third_tick = effective_tick_total;
                    endcase
                end
                if (old_shape != 4'h0 &&
                    dut.ssi263_secondary_i.audio_i.voice_shape_q == 4'h0)
                    voiced_reloads = voiced_reloads + 1;
                old_toggle = dut.ssi263_secondary_i.voice_toggle;
                old_shape = dut.ssi263_secondary_i.audio_i.voice_shape_q;
                timeout = timeout + 1;
            end

            expected_ticks = 8 *
                (4096 - dut.ssi263_secondary_i.core_i.pitch_inflection);
            check(rising_events == 3,
                  "three final voiced excitation events were not observed");
            check((second_tick - first_tick) == expected_ticks,
                  "first final voiced event interval was not 8*(4096-I)");
            check((third_tick - second_tick) == expected_ticks,
                  "second final voiced event interval was not 8*(4096-I)");
            check(voiced_reloads >= 2,
                  "final voiced events did not reload the audio pulse shaper");
        end
    endtask

    logic read_drove;
    logic [7:0] read_value;
    logic speech_selected;
    logic [17:0] primary_hcc_before;
    logic [17:0] primary_hcc_after;
    logic [2:0] echo_selector_before;
    integer echo_timeout;

    initial begin
        drive_idle();
        ab_read.res = 1'b1;
        hard_reset();

        check(dut.phasor_mode_q == 3'd0,
              "card did not reset to Mockingboard mode");
        check(dut.ssi263_secondary_i.core_i.powered_down &&
              dut.ssi263_primary_i.core_i.powered_down,
              "both AP sockets did not reset into power-down");

        // A5, A6, and A5+A6 writes must hit only the decoded physical sockets.
        apple_write(SLOT_BASE + 16'h0021, 8'h5A);
        check(dut.ssi263_secondary_i.core_i.inflection_high_q == 8'h5A,
              "A5 write did not reach the secondary SSI");
        check(dut.ssi263_primary_i.core_i.inflection_high_q == 8'h00,
              "A5 write crossed into the primary SSI");

        apple_write(SLOT_BASE + 16'h0041, 8'hA6);
        check(dut.ssi263_secondary_i.core_i.inflection_high_q == 8'h5A,
              "A6 write changed the secondary SSI");
        check(dut.ssi263_primary_i.core_i.inflection_high_q == 8'hA6,
              "A6 write did not reach the primary SSI");

        apple_write(SLOT_BASE + 16'h0061, 8'h3C);
        check(dut.ssi263_secondary_i.core_i.inflection_high_q == 8'h3C &&
              dut.ssi263_primary_i.core_i.inflection_high_q == 8'h3C,
              "A5+A6 write did not update both independent SSI registers");

        // Enable each VIA's CA1 source while still in Mockingboard mode.
        apple_write(SLOT_BASE + 16'h000E, 8'h82);
        apple_write(SLOT_BASE + 16'h008E, 8'h82);

        // Configure both chips through dual-select writes.  Frame mode and
        // R=15 give the shortest request boundary; I=$FFF also exercises the
        // final pitch path while both instances remain fully independent.
        apple_write(SLOT_BASE + 16'h0061, 8'hFF);
        apple_write(SLOT_BASE + 16'h0062, 8'hFF);
        apple_write(SLOT_BASE + 16'h0060, 8'h41);
        apple_write(SLOT_BASE + 16'h0063, 8'h7F);
        wait_for_both_pending();
        repeat (5) @(posedge clk);
        #1;

        check(dut.ssi0_ar_drive_low && dut.ssi1_ar_drive_low,
              "both pending requests did not drive their A/R pins low");
        check(!dbg_ssi_irq,
              "Mockingboard mode exposed the native direct SSI IRQ");
        check(!dut.via0.ca1_in && !dut.via1.ca1_in,
              "Mockingboard mode did not route each A/R pin to matching CA1");
        check(dut.via0.irq_ca1 && dut.via1.irq_ca1,
              "both VIA CA1 edge latches did not observe the SSI requests");
        check(ab_write.assert_irq,
              "enabled VIA CA1 requests did not assert the card IRQ");

        apple_read(SLOT_BASE + 16'h0001, 7'h11,
                   read_drove, read_value, speech_selected);
        check(!dut.via0.irq_ca1 && dut.via1.irq_ca1,
              "VIA0 Port A read did not clear only the A5 CA1 latch");
        check(ab_write.assert_irq,
              "VIA1 CA1 did not keep the Mockingboard IRQ asserted");
        apple_read(SLOT_BASE + 16'h0081, 7'h22,
                   read_drove, read_value, speech_selected);
        check(!dut.via0.irq_ca1 && !dut.via1.irq_ca1,
              "VIA1 Port A read did not clear the remaining CA1 latch");
        check(!ab_write.assert_irq,
              "Mockingboard IRQ remained after both VIA CA1 clears");

        // Echo+ hides both SSI bus lanes but must not stop or reset either
        // running chip.  The Echo VIA may still serve this bus address.
        mode_access(MODE_ECHO);
        check(dut.phasor_mode_q == 3'd7,
              "mode access did not select Echo+");
        echo_selector_before = dut.ssi263_secondary_i.core_i.selector;
        apple_write(SLOT_BASE + 16'h0021, 8'h12);
        check(dut.ssi263_secondary_i.core_i.inflection_high_q == 8'hFF &&
              dut.ssi263_secondary_i.core_i.d7_pending,
              "Echo+ write reached or acknowledged A5");
        check(dut.ssi263_primary_i.core_i.inflection_high_q == 8'hFF &&
              dut.ssi263_primary_i.core_i.d7_pending,
              "Echo+ write changed A6");
        apple_read(SLOT_BASE + 16'h0060, 7'h35,
                   read_drove, read_value, speech_selected);
        check(!speech_selected,
              "Echo+ read selected an SSI data driver");
        echo_timeout = 0;
        while (dut.ssi263_secondary_i.core_i.selector == echo_selector_before &&
               echo_timeout < 10_000) begin
            @(posedge clk);
            echo_timeout = echo_timeout + 1;
        end
        check(echo_timeout < 10_000 && dut.ssi0_d7 && dut.ssi1_d7,
              "Echo+ hid the bus by stopping or resetting an SSI core");

        // Native mode exposes direct IRQ and D7.  Reads must keep D0-D6 from
        // the floating Apple data bus.  A later split-pending state proves
        // that A5 wins a simultaneous A5+A6 read.
        mode_access(MODE_NATIVE);
        check(dut.phasor_mode_q == 3'd5,
              "mode access did not select native Phasor");
        check(dbg_ssi_irq && ab_write.assert_irq,
              "native IRQ did not OR the two pending A/R pins");
        apple_read(SLOT_BASE + 16'h0020, 7'h2D,
                   read_drove, read_value, speech_selected);
        check(read_drove && speech_selected && read_value == 8'hAD,
              "native A5 D7 read did not preserve floating D0-D6");

        apple_write(SLOT_BASE + 16'h0021, 8'hA5);
        check(!dut.ssi0_d7 && dut.ssi1_d7,
              "A5 acknowledgment cleared the wrong request state");
        check(dut.ssi263_secondary_i.core_i.inflection_high_q == 8'hA5 &&
              dut.ssi263_primary_i.core_i.inflection_high_q == 8'hFF,
              "A5 acknowledgment crossed into A6 state");
        check(dbg_ssi_irq && ab_write.assert_irq,
              "A6 did not keep native IRQ asserted after A5 acknowledgment");

        apple_read(SLOT_BASE + 16'h0020, 7'h55,
                   read_drove, read_value, speech_selected);
        check(read_drove && speech_selected && read_value == 8'h55,
              "cleared A5 D7 read changed the lower bus bits");
        apple_read(SLOT_BASE + 16'h0040, 7'h55,
                   read_drove, read_value, speech_selected);
        check(read_drove && speech_selected && read_value == 8'hD5,
              "pending A6 D7 read did not preserve the lower bus bits");
        apple_read(SLOT_BASE + 16'h0060, 7'h55,
                   read_drove, read_value, speech_selected);
        check(read_drove && speech_selected && read_value == 8'h55,
              "dual native read did not give cleared A5 priority over A6");

        apple_write(SLOT_BASE + 16'h0041, 8'h6A);
        check(!dut.ssi0_d7 && !dut.ssi1_d7,
              "A6 acknowledgment did not clear only the remaining request");
        check(dut.ssi263_secondary_i.core_i.inflection_high_q == 8'hA5 &&
              dut.ssi263_primary_i.core_i.inflection_high_q == 8'h6A,
              "A6 acknowledgment crossed into A5 state");
        repeat (4) @(posedge clk);
        #1;
        check(!dbg_ssi_irq && !ab_write.assert_irq,
              "native IRQ did not release after both independent requests cleared");

        // SSI-263AP PD/RST wins the exact falling-write collision and keeps
        // the old register value.  This uses Apple RESET, not a testbench
        // reset forced inside either voice.
        hard_reset();
        apple_write(SLOT_BASE + 16'h0021, 8'h5A);
        apple_write_reset_collision(SLOT_BASE + 16'h0021, 8'hC3);
        check(dut.ssi263_secondary_i.core_i.inflection_high_q == 8'h5A,
              "Apple RESET lost a selected-write falling-edge collision");
        check(dut.ssi263_secondary_i.core_i.powered_down &&
              !dut.ssi263_secondary_i.core_i.d7_pending &&
              !dut.ssi263_secondary_i.core_i.phone_active,
              "AP PD/RST did not clear active/request state on collision");
        check(dut.phasor_mode_q == 3'd0,
              "Apple RESET collision did not restore Mockingboard mode");

        // Start end-to-end excitation on A5 only.  The other real core and
        // audio block must remain at their reset host/excitation state.
        hard_reset();
        primary_hcc_before = {
            dut.ssi263_primary_i.audio_i.noise_d1_q,
            dut.ssi263_primary_i.audio_i.noise_d2_q,
            dut.ssi263_primary_i.audio_i.noise_d3_q,
            dut.ssi263_primary_i.audio_i.noise_d4_q
        };
        apple_write(SLOT_BASE + 16'h0021, 8'hFF);
        apple_write(SLOT_BASE + 16'h0022, 8'hFF);
        apple_write(SLOT_BASE + 16'h0020, 8'h67); // DR=01, phone $27
        apple_write(SLOT_BASE + 16'h0023, 8'h7F);
        wait_for_secondary_fricative();
        count_hcc_shifts(8);
        measure_a5_fricative_level();
        check(dut.ssi263_secondary_i.core_i.phone_active &&
              dut.ssi263_secondary_i.core_i.phone_fricative,
              "fricative phone was not held while HCC state advanced");

        primary_hcc_after = {
            dut.ssi263_primary_i.audio_i.noise_d1_q,
            dut.ssi263_primary_i.audio_i.noise_d2_q,
            dut.ssi263_primary_i.audio_i.noise_d3_q,
            dut.ssi263_primary_i.audio_i.noise_d4_q
        };
        check(primary_hcc_after == primary_hcc_before &&
              dut.ssi263_primary_i.core_i.inflection_high_q == 8'h00 &&
              dut.ssi263_primary_i.core_i.rate_inflection_q == 8'h00 &&
              dut.ssi263_primary_i.core_i.duration_phoneme_q == 8'hC0 &&
              dut.ssi263_primary_i.core_i.voice_amp_code == 4'h0 &&
              dut.ssi263_primary_i.core_i.fric_amp_code == 4'h0,
              "secondary fricative activity changed the other SSI socket");

        // I=$FFD gives an expected final glottal-event period of 24
        // effective ticks.  The check computes the general 8*(4096-I) form
        // from the live core value and observes the real audio pitch toggle.
        apple_write(SLOT_BASE + 16'h0021, 8'hFF);
        apple_write(SLOT_BASE + 16'h0022, 8'hFD);
        apple_write(SLOT_BASE + 16'h0020, 8'h41); // DR=01, voiced phone $01
        wait_for_secondary_voiced();
        check(dut.ssi263_secondary_i.core_i.pitch_inflection == 12'hFFD,
              "voiced-period setup did not reach the final pitch path");
        measure_voiced_period();
        check(dut.ssi263_primary_i.audio_i.noise_d1_q ==
                  primary_hcc_before[17:14] &&
              dut.ssi263_primary_i.core_i.phone_active == 1'b0 &&
              dut.ssi263_primary_i.audio_i.voice_source == 24'sd0 &&
              dut.ssi263_primary_i.audio_i.fric_source == 24'sd0,
              "secondary voiced events changed the other SSI socket");

        // Use the real-driver pitch/filter vector for the level check.  The
        // earlier I=$FFD case proves the shortest period but can reload U60
        // before its 15 filter counts finish and is not an acoustic vector.
        apple_write(SLOT_BASE + 16'h0021, 8'h40);
        apple_write(SLOT_BASE + 16'h0022, 8'hF8);
        apple_write(SLOT_BASE + 16'h0024, 8'hE8);
        apple_write(SLOT_BASE + 16'h0020, 8'h41);
        wait_for_secondary_voiced();
        wait_for_a5_left_audio();

        // Reset, then excite A6 alone to prove the opposite fixed stereo
        // route through the actual core, filter engine, and card mixer.
        hard_reset();
        apple_write(SLOT_BASE + 16'h0041, 8'hFF);
        apple_write(SLOT_BASE + 16'h0042, 8'hFD);
        apple_write(SLOT_BASE + 16'h0040, 8'h41); // DR=01, voiced phone $01
        apple_write(SLOT_BASE + 16'h0043, 8'h7F);
        wait_for_primary_voiced();
        wait_for_a6_right_audio();

        // Leave A6 running and start A5.  Both independent filter engines and
        // both final card channels must carry speech at the same time.
        apple_write(SLOT_BASE + 16'h0021, 8'hFF);
        apple_write(SLOT_BASE + 16'h0022, 8'hFD);
        apple_write(SLOT_BASE + 16'h0020, 8'h41); // DR=01, voiced phone $01
        apple_write(SLOT_BASE + 16'h0023, 8'h7F);
        wait_for_secondary_voiced();
        wait_for_dual_stereo_audio();

        // Disable slot 4 while both chips have pending native requests, an
        // active read drive, and live stereo output.  This models a runtime
        // menu disable and then proves a clean card insertion on re-enable.
        mode_access(MODE_NATIVE);
        wait_for_both_pending();
        wait_for_dual_stereo_audio();
        disable_card_during_native_read();

        if (failures == 0) begin
            $display("PHASOR DUAL SSI263 PASS checks=%0d hcc_shifts=%0d voiced_reloads=%0d",
                     checks, hcc_shifts, voiced_reloads);
        end else begin
            $display("PHASOR DUAL SSI263 FAIL count=%0d checks=%0d",
                     failures, checks);
            $fatal(1);
        end
        $finish;
    end

endmodule
