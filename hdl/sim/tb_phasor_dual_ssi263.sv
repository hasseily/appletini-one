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
    integer fric_source_changes = 0;

    integer voice_chip_peak = 0;
    integer voice_chip_mean_abs = 0;
    integer voice_chip_occupancy = 0;
    integer voice_chip_clips = 0;
    longint voice_chip_mean_square = 0;
    integer voice_card_peak = 0;
    integer voice_card_mean_abs = 0;
    integer voice_card_occupancy = 0;
    integer voice_card_clips = 0;
    longint voice_card_mean_square = 0;

    integer s_chip_peak = 0;
    integer s_chip_mean_abs = 0;
    integer s_chip_occupancy = 0;
    integer s_chip_clips = 0;
    longint s_chip_mean_square = 0;
    integer s_card_peak = 0;
    integer s_card_mean_abs = 0;
    integer s_card_occupancy = 0;
    integer s_card_clips = 0;
    longint s_card_mean_square = 0;

    logic [1:0] sample_period_q = 2'd0;

    always #5 clk = ~clk;

    // A 100 MHz fabric clock cannot divide evenly to 48 kHz. Two 2083-clock
    // periods followed by one 2084-clock period give an exact 48 kHz average.
    always_ff @(posedge clk) begin
        if (!rstn) begin
            sample_div_q <= 12'd0;
            sample_period_q <= 2'd0;
            audio_sample_tick <= 1'b0;
        end else if ((sample_period_q != 2'd2 &&
                      sample_div_q == 12'd2082) ||
                     (sample_period_q == 2'd2 &&
                      sample_div_q == 12'd2083)) begin
            sample_div_q <= 12'd0;
            if (sample_period_q == 2'd2)
                sample_period_q <= 2'd0;
            else
                sample_period_q <= sample_period_q + 2'd1;
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

    function automatic integer integer_sqrt(input longint value);
        longint estimate;
        longint next_estimate;
        begin
            if (value <= 0) begin
                integer_sqrt = 0;
            end else begin
                estimate = value;
                next_estimate = (estimate + value / estimate) / 2;
                while (next_estimate < estimate) begin
                    estimate = next_estimate;
                    next_estimate = (estimate + value / estimate) / 2;
                end
                integer_sqrt = estimate;
            end
        end
    endfunction

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

    task automatic start_a5_acoustic_phone(
        input logic [7:0] duration_phone
    );
        begin
            // Known speech-driver vector: I=$A00, R=$F, ART=$7, AMP=$F,
            // and FILT=$E8. Keep every phone in the sweep on this vector.
            apple_write(SLOT_BASE + 16'h0021, 8'h40);
            apple_write(SLOT_BASE + 16'h0022, 8'hF8);
            apple_write(SLOT_BASE + 16'h0024, 8'hE8);
            apple_write(SLOT_BASE + 16'h0020, duration_phone);
            apple_write(SLOT_BASE + 16'h0023, 8'h7F);
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
            check(timeout < 3_000_000 && saw_chip_audio && saw_left_audio &&
                  saw_filter_state && !leaked_right,
                  "A5 speech did not remain on the left channel only");
            check(!dut.ssi263_secondary_i.audio_i.engine_overrun_q &&
                  !dut.ssi263_primary_i.audio_i.engine_overrun_q,
                  "A5-only speech overran an actual audio engine");
        end
    endtask

    task automatic measure_a5_acoustic_window(
        input string label,
        input logic is_voice,
        input logic is_s
    );
        integer timeout;
        integer settle_samples;
        integer level_samples;
        integer chip_sample;
        integer card_sample;
        integer chip_peak;
        integer card_peak;
        integer chip_abs;
        integer card_abs;
        integer chip_abs_sum;
        integer card_abs_sum;
        integer chip_occupancy;
        integer card_occupancy;
        integer chip_clips;
        integer card_clips;
        longint chip_square_sum;
        longint card_square_sum;
        longint chip_mean_square;
        longint card_mean_square;
        logic signed [23:0] old_fric_source;
        begin
            timeout = 0;
            settle_samples = 0;
            level_samples = 0;
            chip_peak = 0;
            card_peak = 0;
            chip_abs_sum = 0;
            card_abs_sum = 0;
            chip_occupancy = 0;
            card_occupancy = 0;
            chip_clips = 0;
            card_clips = 0;
            chip_square_sum = 0;
            card_square_sum = 0;
            old_fric_source = dut.ssi263_secondary_i.audio_i.fric_source;
            if (is_s)
                fric_source_changes = 0;

            // Let the ROM controls and every tract section reach the new
            // sustained phone before measuring the following 512 samples.
            while (settle_samples < 256 && timeout < 3_000_000) begin
                @(posedge clk);
                #1;
                if (is_s) begin
                    if (dut.ssi263_secondary_i.audio_i.fric_source !=
                        old_fric_source)
                        fric_source_changes = fric_source_changes + 1;
                    old_fric_source =
                        dut.ssi263_secondary_i.audio_i.fric_source;
                end
                if (dut.audio_sample_tick)
                    settle_samples = settle_samples + 1;
                timeout = timeout + 1;
            end

            while (level_samples < 512 && timeout < 3_000_000) begin
                @(posedge clk);
                #1;
                if (is_s) begin
                    if (dut.ssi263_secondary_i.audio_i.fric_source !=
                        old_fric_source)
                        fric_source_changes = fric_source_changes + 1;
                    old_fric_source =
                        dut.ssi263_secondary_i.audio_i.fric_source;
                end
                if (dut.audio_sample_tick) begin
                    chip_sample = $signed(dut.ssi0_audio);
                    card_sample = $signed(audio_l);
                    chip_abs = (chip_sample < 0) ? -chip_sample : chip_sample;
                    card_abs = (card_sample < 0) ? -card_sample : card_sample;
                    if (chip_abs > chip_peak)
                        chip_peak = chip_abs;
                    if (card_abs > card_peak)
                        card_peak = card_abs;
                    chip_abs_sum = chip_abs_sum + chip_abs;
                    card_abs_sum = card_abs_sum + card_abs;
                    chip_square_sum = chip_square_sum +
                                      chip_sample * chip_sample;
                    card_square_sum = card_square_sum +
                                      card_sample * card_sample;
                    if (chip_sample != 0)
                        chip_occupancy = chip_occupancy + 1;
                    if (card_sample != 0)
                        card_occupancy = card_occupancy + 1;
                    if (chip_sample == 32767 || chip_sample == -32768)
                        chip_clips = chip_clips + 1;
                    if (card_sample == 32767 || card_sample == -32768)
                        card_clips = card_clips + 1;
                    level_samples = level_samples + 1;
                end
                timeout = timeout + 1;
            end

            if (level_samples == 512) begin
                chip_mean_square = chip_square_sum / 512;
                card_mean_square = card_square_sum / 512;
            end else begin
                chip_mean_square = 0;
                card_mean_square = 0;
            end

            $display("PHASOR SSI263 ACOUSTIC %s chip_peak=%0d chip_rms=%0d chip_ms=%0d chip_mean_abs=%0d chip_occupancy=%0d chip_clips=%0d card_peak=%0d card_rms=%0d card_ms=%0d card_mean_abs=%0d card_occupancy=%0d card_clips=%0d samples=%0d",
                     label, chip_peak, integer_sqrt(chip_mean_square),
                     chip_mean_square, chip_abs_sum / 512,
                     chip_occupancy, chip_clips,
                     card_peak, integer_sqrt(card_mean_square),
                     card_mean_square, card_abs_sum / 512,
                     card_occupancy, card_clips, level_samples);

            check(settle_samples == 256 && level_samples == 512,
                  "acoustic window did not collect every settled sample");
            check(chip_peak >= 256 && card_peak >= 128 &&
                  chip_mean_square >= 4096 && card_mean_square >= 1024 &&
                  chip_abs_sum / 512 >= 32 && card_abs_sum / 512 >= 16 &&
                  chip_occupancy >= 64 && card_occupancy >= 64,
                  "settled acoustic output was trivial or sparse");
            check(chip_clips == 0 && card_clips == 0,
                  "settled acoustic output clipped");
            if (is_s)
                check(fric_source_changes >= 2,
                      "sustained S did not vary its live fricative source");

            if (is_voice) begin
                voice_chip_peak = chip_peak;
                voice_chip_mean_square = chip_mean_square;
                voice_chip_mean_abs = chip_abs_sum / 512;
                voice_chip_occupancy = chip_occupancy;
                voice_chip_clips = chip_clips;
                voice_card_peak = card_peak;
                voice_card_mean_square = card_mean_square;
                voice_card_mean_abs = card_abs_sum / 512;
                voice_card_occupancy = card_occupancy;
                voice_card_clips = card_clips;
            end else if (is_s) begin
                s_chip_peak = chip_peak;
                s_chip_mean_square = chip_mean_square;
                s_chip_mean_abs = chip_abs_sum / 512;
                s_chip_occupancy = chip_occupancy;
                s_chip_clips = chip_clips;
                s_card_peak = card_peak;
                s_card_mean_square = card_mean_square;
                s_card_mean_abs = card_abs_sum / 512;
                s_card_occupancy = card_occupancy;
                s_card_clips = card_clips;
            end
        end
    endtask

    task automatic check_a5_acoustic_balance;
        begin
            $display("PHASOR SSI263 BALANCE voice_ms=%0d s_ms=%0d voice_card_ms=%0d s_card_ms=%0d",
                     voice_chip_mean_square, s_chip_mean_square,
                     voice_card_mean_square, s_card_mean_square);
            // A specific phone may differ in level, but neither source class
            // should overwhelm the other by more than 18 dB RMS.
            check(voice_chip_mean_square > 0 && s_chip_mean_square > 0 &&
                  s_chip_mean_square <= voice_chip_mean_square * 64 &&
                  voice_chip_mean_square <= s_chip_mean_square * 64,
                  "SSI voice/fricative power ratio was not sane");
            check(voice_card_mean_square > 0 && s_card_mean_square > 0 &&
                  s_card_mean_square <= voice_card_mean_square * 64 &&
                  voice_card_mean_square <= s_card_mean_square * 64,
                  "card voice/fricative power ratio was not sane");
        end
    endtask

    task automatic measure_a5_held_p_silence;
        integer timeout;
        integer settle_samples;
        integer level_samples;
        integer chip_sample;
        integer card_sample;
        integer chip_abs;
        integer card_abs;
        integer chip_peak;
        integer card_peak;
        integer chip_occupancy;
        integer card_occupancy;
        integer chip_clips;
        integer card_clips;
        longint chip_square_sum;
        longint card_square_sum;
        longint chip_mean_square;
        longint card_mean_square;
        begin
            timeout = 0;
            while (!(dut.ssi263_secondary_i.audio_i.stop_class &&
                     dut.ssi263_secondary_i.core_i.phone_active) &&
                   timeout < 500_000) begin
                @(posedge clk);
                timeout = timeout + 1;
            end
            #1;
            check(timeout < 500_000,
                  "held P did not reach the guide-level stop gate");

            settle_samples = 0;
            level_samples = 0;
            chip_peak = 0;
            card_peak = 0;
            chip_occupancy = 0;
            card_occupancy = 0;
            chip_clips = 0;
            card_clips = 0;
            chip_square_sum = 0;
            card_square_sum = 0;
            while (settle_samples < 256 && timeout < 3_000_000) begin
                @(posedge clk);
                #1;
                if (dut.audio_sample_tick)
                    settle_samples = settle_samples + 1;
                timeout = timeout + 1;
            end
            while (level_samples < 256 && timeout < 3_000_000) begin
                @(posedge clk);
                #1;
                if (dut.audio_sample_tick) begin
                    chip_sample = $signed(dut.ssi0_audio);
                    card_sample = $signed(audio_l);
                    chip_abs = (chip_sample < 0) ? -chip_sample : chip_sample;
                    card_abs = (card_sample < 0) ? -card_sample : card_sample;
                    if (chip_abs > chip_peak)
                        chip_peak = chip_abs;
                    if (card_abs > card_peak)
                        card_peak = card_abs;
                    chip_square_sum = chip_square_sum +
                                      chip_sample * chip_sample;
                    card_square_sum = card_square_sum +
                                      card_sample * card_sample;
                    if (chip_sample != 0)
                        chip_occupancy = chip_occupancy + 1;
                    if (card_sample != 0)
                        card_occupancy = card_occupancy + 1;
                    if (chip_sample == 32767 || chip_sample == -32768)
                        chip_clips = chip_clips + 1;
                    if (card_sample == 32767 || card_sample == -32768)
                        card_clips = card_clips + 1;
                    level_samples = level_samples + 1;
                end
                timeout = timeout + 1;
            end
            chip_mean_square = (level_samples == 256) ?
                               chip_square_sum / 256 : 0;
            card_mean_square = (level_samples == 256) ?
                               card_square_sum / 256 : 0;
            $display("PHASOR SSI263 ACOUSTIC P_HELD chip_peak=%0d chip_rms=%0d chip_ms=%0d chip_occupancy=%0d chip_clips=%0d card_peak=%0d card_rms=%0d card_ms=%0d card_occupancy=%0d card_clips=%0d samples=%0d",
                     chip_peak, integer_sqrt(chip_mean_square),
                     chip_mean_square, chip_occupancy, chip_clips,
                     card_peak, integer_sqrt(card_mean_square),
                     card_mean_square, card_occupancy, card_clips,
                     level_samples);
            check(settle_samples == 256 && level_samples == 256,
                  "held P silence window did not collect every sample");
            check(dut.ssi263_secondary_i.audio_i.stop_class &&
                  dut.ssi263_secondary_i.audio_i.voice_source == 24'sd0 &&
                  dut.ssi263_secondary_i.audio_i.fric_source == 24'sd0,
                  "held P did not remain a silent guide-level stop");
            check(chip_peak <= 64 && card_peak <= 64 &&
                  chip_mean_square <= 256 && card_mean_square <= 256 &&
                  chip_clips == 0 && card_clips == 0,
                  "held P repeated an audible source instead of silence");
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
        integer source_timeout;
        integer level_samples;
        integer ssi0_occupancy;
        integer ssi1_occupancy;
        integer left_occupancy;
        integer right_occupancy;
        integer ssi0_clips;
        integer ssi1_clips;
        integer left_clips;
        integer right_clips;
        logic saw_secondary_filter_state;
        logic saw_primary_filter_state;
        logic saw_secondary_source;
        logic saw_primary_source;
        begin
            // U58/U59 keep counting from reset when I changes. Wait through
            // the possible old I=$000 interval for one real source pulse from
            // each chip before opening the fixed-length route window.
            source_timeout = 0;
            saw_secondary_source = 1'b0;
            saw_primary_source = 1'b0;
            while (!(saw_secondary_source && saw_primary_source) &&
                   source_timeout < 3_000_000) begin
                @(posedge clk);
                #1;
                if (dut.ssi263_secondary_i.audio_i.voice_source != 24'sd0)
                    saw_secondary_source = 1'b1;
                if (dut.ssi263_primary_i.audio_i.voice_source != 24'sd0)
                    saw_primary_source = 1'b1;
                source_timeout = source_timeout + 1;
            end
            check(saw_secondary_source && saw_primary_source,
                  "both SSI chips did not produce a live voice pulse");

            timeout = 0;
            level_samples = 0;
            ssi0_occupancy = 0;
            ssi1_occupancy = 0;
            left_occupancy = 0;
            right_occupancy = 0;
            ssi0_clips = 0;
            ssi1_clips = 0;
            left_clips = 0;
            right_clips = 0;
            saw_secondary_filter_state = 1'b0;
            saw_primary_filter_state = 1'b0;
            while (level_samples < 512 && timeout < 3_000_000) begin
                @(posedge clk);
                #1;
                if (dut.ssi263_secondary_i.audio_i.f1_state_q != 24'sd0 &&
                    dut.ssi263_secondary_i.audio_i.f5_state_q != 24'sd0)
                    saw_secondary_filter_state = 1'b1;
                if (dut.ssi263_primary_i.audio_i.f1_state_q != 24'sd0 &&
                    dut.ssi263_primary_i.audio_i.f5_state_q != 24'sd0)
                    saw_primary_filter_state = 1'b1;
                if (dut.audio_sample_tick) begin
                    if (dut.ssi0_audio != 16'sd0)
                        ssi0_occupancy = ssi0_occupancy + 1;
                    if (dut.ssi1_audio != 16'sd0)
                        ssi1_occupancy = ssi1_occupancy + 1;
                    if (audio_l != 16'sd0)
                        left_occupancy = left_occupancy + 1;
                    if (audio_r != 16'sd0)
                        right_occupancy = right_occupancy + 1;
                    if (dut.ssi0_audio == 16'sh7FFF ||
                        dut.ssi0_audio == -16'sh8000)
                        ssi0_clips = ssi0_clips + 1;
                    if (dut.ssi1_audio == 16'sh7FFF ||
                        dut.ssi1_audio == -16'sh8000)
                        ssi1_clips = ssi1_clips + 1;
                    if (audio_l == 16'sh7FFF || audio_l == -16'sh8000)
                        left_clips = left_clips + 1;
                    if (audio_r == 16'sh7FFF || audio_r == -16'sh8000)
                        right_clips = right_clips + 1;
                    level_samples = level_samples + 1;
                end
                timeout = timeout + 1;
            end
            $display("PHASOR SSI263 DUAL samples=%0d ssi0_occ=%0d ssi1_occ=%0d left_occ=%0d right_occ=%0d ssi0_clips=%0d ssi1_clips=%0d left_clips=%0d right_clips=%0d",
                     level_samples, ssi0_occupancy, ssi1_occupancy,
                     left_occupancy, right_occupancy, ssi0_clips, ssi1_clips,
                     left_clips, right_clips);
            // The two pulse trains need not be nonzero on the same sample.
            // Prove each source, tract, and fixed stereo route independently.
            check(level_samples == 512 && ssi0_occupancy >= 16 &&
                  ssi1_occupancy >= 16 && left_occupancy >= 16 &&
                  right_occupancy >= 16 && saw_secondary_filter_state &&
                  saw_primary_filter_state &&
                  dut.ssi263_secondary_i.core_i.phone_active &&
                  dut.ssi263_primary_i.core_i.phone_active,
                  "both SSI audio engines did not run independently");
            check(ssi0_clips == 0 && ssi1_clips == 0 &&
                  left_clips == 0 && right_clips == 0,
                  "simultaneous speech clipped a chip or card route");
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
            check(dut.ssi263_secondary_i.core_i.phone_active &&
                  dut.ssi263_primary_i.core_i.phone_active &&
                  dut.ssi263_secondary_i.core_i.voiced &&
                  dut.ssi263_primary_i.core_i.voiced &&
                  !dut.ssi263_secondary_i.core_i.powered_down &&
                  !dut.ssi263_primary_i.core_i.powered_down,
                  "disable test did not start with two live speech sources");

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

    task automatic measure_voiced_period;
        integer timeout;
        integer rising_events;
        integer first_tick;
        integer second_tick;
        integer third_tick;
        integer expected_ticks;
        logic old_toggle;
        begin
            timeout = 0;
            rising_events = 0;
            effective_tick_total = 0;
            first_tick = 0;
            second_tick = 0;
            third_tick = 0;
            old_toggle = dut.ssi263_secondary_i.voice_toggle;

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
                old_toggle = dut.ssi263_secondary_i.voice_toggle;
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

        // Start sustained S ($30) on A5 with one complete known driver
        // vector. The old test used P ($27) as if it were a held S and left
        // FILT at its reset value, so its peak was not a useful noise check.
        // The other real core and audio block must remain at reset state.
        hard_reset();
        primary_hcc_before = {
            dut.ssi263_primary_i.audio_i.noise_d1_q,
            dut.ssi263_primary_i.audio_i.noise_d2_q,
            dut.ssi263_primary_i.audio_i.noise_d3_q,
            dut.ssi263_primary_i.audio_i.noise_d4_q
        };
        start_a5_acoustic_phone(8'h70); // DR=01, sustained S $30
        wait_for_secondary_fricative();
        measure_a5_acoustic_window("S", 1'b0, 1'b1);
        check(dut.ssi263_secondary_i.core_i.phone_active &&
              dut.ssi263_secondary_i.core_i.phone_fricative,
              "sustained S did not remain an active fricative phone");

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

        // Sweep both fricative injection classes and a mixed affricate on the
        // same driver vector. These rows are calibration evidence, not a set
        // of phone-specific gain targets.
        start_a5_acoustic_phone(8'h74); // DR=01, F $34, FRIC_1
        wait_for_secondary_fricative();
        measure_a5_acoustic_window("F", 1'b0, 1'b0);
        start_a5_acoustic_phone(8'h72); // DR=01, SCH $32, FRIC_1
        wait_for_secondary_fricative();
        measure_a5_acoustic_window("SCH", 1'b0, 1'b0);
        start_a5_acoustic_phone(8'h71); // DR=01, J $31, mixed source
        wait_for_secondary_fricative();
        measure_a5_acoustic_window("J", 1'b0, 1'b0);
        start_a5_acoustic_phone(8'h6F); // DR=01, Z $2F, FRIC_2
        wait_for_secondary_fricative();
        measure_a5_acoustic_window("Z", 1'b0, 1'b0);

        // Stops do not form a held noise source. A fresh reset makes held-P
        // silence measurable; a normal following HF phone must then go live.
        hard_reset();
        start_a5_acoustic_phone(8'h67); // DR=01, P $27
        measure_a5_held_p_silence();
        start_a5_acoustic_phone(8'h6C); // DR=01, P -> HF $2C
        wait_for_secondary_fricative();
        measure_a5_acoustic_window("P_TO_HF", 1'b0, 1'b0);

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
        apple_write(SLOT_BASE + 16'h0023, 8'h7F);
        apple_write(SLOT_BASE + 16'h0020, 8'h41);
        wait_for_secondary_voiced();
        wait_for_a5_left_audio();
        measure_a5_acoustic_window("VOICE01", 1'b1, 1'b0);
        check_a5_acoustic_balance();

        // Reset, then excite A6 alone to prove the opposite fixed stereo
        // route through the actual core, filter engine, and card mixer.
        hard_reset();
        apple_write(SLOT_BASE + 16'h0041, 8'h40);
        apple_write(SLOT_BASE + 16'h0042, 8'hF8);
        apple_write(SLOT_BASE + 16'h0044, 8'hE8);
        apple_write(SLOT_BASE + 16'h0040, 8'h41); // DR=01, voiced phone $01
        apple_write(SLOT_BASE + 16'h0043, 8'h7F);
        wait_for_primary_voiced();
        wait_for_a6_right_audio();

        // Start both sockets from the same reset edge and one real A5+A6
        // vector. Earlier checks already prove separate A5/A6 write decode;
        // this case proves two live engines and both fixed stereo routes.
        hard_reset();
        apple_write(SLOT_BASE + 16'h0061, 8'h40);
        apple_write(SLOT_BASE + 16'h0062, 8'hF8);
        apple_write(SLOT_BASE + 16'h0064, 8'hE8);
        apple_write(SLOT_BASE + 16'h0060, 8'h41); // DR=01, voiced phone $01
        apple_write(SLOT_BASE + 16'h0063, 8'h7F);
        wait_for_secondary_voiced();
        wait_for_primary_voiced();
        wait_for_dual_stereo_audio();

        // Disable slot 4 while both chips have pending native requests, an
        // active read drive, and live stereo output.  This models a runtime
        // menu disable and then proves a clean card insertion on re-enable.
        mode_access(MODE_NATIVE);
        wait_for_both_pending();
        wait_for_dual_stereo_audio();
        disable_card_during_native_read();

        if (failures == 0) begin
            $display("PHASOR DUAL SSI263 PASS checks=%0d fric_source_changes=%0d",
                     checks, fric_source_changes);
        end else begin
            $display("PHASOR DUAL SSI263 FAIL count=%0d checks=%0d",
                     failures, checks);
            $fatal(1);
        end
        $finish;
    end

endmodule
