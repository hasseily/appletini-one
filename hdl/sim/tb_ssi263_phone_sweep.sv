`timescale 1ns / 1ps

// Integrated native SSI-263/SC-02 phone regression.  The bus writes enter
// ssi263_voice, so every source and filter control used below comes from the
// real core and its 512-byte ROM before reaching the audio block.
module tb_ssi263_phone_sweep;

    localparam integer RAW_XCK_HZ = 2045454;
    localparam integer AUDIO_HZ = 48000;
    localparam integer SETTLE_SAMPLES = 128;
    localparam integer LEVEL_SAMPLES = 256;

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

    logic [20:0] audio_accumulator_q = 21'd0;
    logic [7:0] expected_rom [0:511];

    integer failures = 0;
    integer checks = 0;
    integer phone_peak [0:63];
    integer phone_rms [0:63];
    integer phone_occupancy [0:63];
    integer phone_clips [0:63];
    integer phone_unknowns [0:63];
    longint phone_mean_square [0:63];
    logic [63:0] stop_mask = 64'd0;

    integer phone_index;
    integer transient_clips;
    integer transient_unknowns;
    integer precharge_peak;
    integer early_tail_peak;
    integer early_f5_peak;
    integer late_stop_peak;
    integer late_f5_peak;
    integer following_peak;
    integer following_occupancy;
    integer following_clips;
    integer following_unknowns;
    longint following_mean_square;

    always #5 clk = ~clk;

    assign xck_ce = rstn && xck_run;

    // One fabric cycle represents one raw XCK edge.  This accumulator keeps
    // the 48 kHz observation cadence tied to the 2.045454 MHz pin clock while
    // allowing the test to run much faster than the 100 MHz card simulation.
    always_ff @(posedge clk) begin
        if (!rstn || !xck_run) begin
            audio_accumulator_q <= 21'd0;
            audio_tick <= 1'b0;
        end else if (audio_accumulator_q >= RAW_XCK_HZ - AUDIO_HZ) begin
            audio_accumulator_q <= audio_accumulator_q + AUDIO_HZ - RAW_XCK_HZ;
            audio_tick <= 1'b1;
        end else begin
            audio_accumulator_q <= audio_accumulator_q + AUDIO_HZ;
            audio_tick <= 1'b0;
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

    function automatic integer sample_magnitude(
        input logic signed [23:0] value
    );
        begin
            sample_magnitude = (value < 0) ? -value : value;
        end
    endfunction

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
        inout integer clips,
        inout integer unknowns
    );
        integer value;
        begin
            if ($isunknown(audio)) begin
                unknowns = unknowns + 1;
            end else begin
                value = $signed(audio);
                if (value == 32767 || value == -32768)
                    clips = clips + 1;
            end
        end
    endtask

    task automatic wait_phone_controls(
        input logic [5:0] phone,
        output integer clips,
        output integer unknowns
    );
        integer timeout;
        integer row;
        logic controls_match;
        begin
            timeout = 0;
            clips = 0;
            unknowns = 0;
            row = phone * 8;
            controls_match = 1'b0;
            while (!controls_match && timeout < 200000) begin
                @(posedge clk);
                #1;
                if (audio_tick)
                    note_pcm_state(clips, unknowns);
                controls_match =
                    dut.core_i.phone_active &&
                    dut.core_i.phoneme == phone &&
                    dut.core_i.f1_code == expected_rom[row + 0][7:4] &&
                    dut.core_i.f2_code == expected_rom[row + 1][7:4] &&
                    dut.core_i.f2_res_code == expected_rom[row + 2][7:4] &&
                    dut.core_i.f3_code == expected_rom[row + 3][7:4] &&
                    dut.core_i.f4_code == expected_rom[row + 3][7:4] &&
                    dut.core_i.filter_amp_code == 4'hF &&
                    dut.core_i.voice_amp_code == expected_rom[row + 5][7:4] &&
                    dut.core_i.fric_amp_code == expected_rom[row + 6][7:4] &&
                    dut.core_i.phone_voiced == !expected_rom[row + 0][0] &&
                    dut.core_i.phone_fricative == !expected_rom[row + 1][0] &&
                    dut.core_i.pw_2 == expected_rom[row + 2][2] &&
                    dut.core_i.pw_3 == expected_rom[row + 2][1] &&
                    dut.core_i.fric1_sw == expected_rom[row + 2][3] &&
                    dut.core_i.fric2_sw == !expected_rom[row + 2][3];
                timeout = timeout + 1;
            end
            check(timeout < 200000,
                  $sformatf("phone %02h controls did not reach its ROM row",
                            phone));
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
        input integer prior_clips,
        input integer prior_unknowns
    );
        integer sample_count;
        integer sample_value;
        integer sample_abs;
        integer peak;
        integer occupancy;
        integer clips;
        integer unknowns;
        longint square_sum;
        begin
            clips = prior_clips;
            unknowns = prior_unknowns;
            for (sample_count = 0;
                 sample_count < SETTLE_SAMPLES;
                 sample_count = sample_count + 1) begin
                wait_audio_sample();
                note_pcm_state(clips, unknowns);
            end

            peak = 0;
            occupancy = 0;
            square_sum = 0;
            for (sample_count = 0;
                 sample_count < LEVEL_SAMPLES;
                 sample_count = sample_count + 1) begin
                wait_audio_sample();
                note_pcm_state(clips, unknowns);
                if (!$isunknown(audio)) begin
                    sample_value = $signed(audio);
                    sample_abs = (sample_value < 0) ?
                                 -sample_value : sample_value;
                    if (sample_abs > peak)
                        peak = sample_abs;
                    if (sample_value != 0)
                        occupancy = occupancy + 1;
                    square_sum = square_sum + sample_value * sample_value;
                end
            end

            phone_peak[phone] = peak;
            phone_mean_square[phone] = square_sum / LEVEL_SAMPLES;
            phone_rms[phone] = integer_sqrt(
                square_sum / LEVEL_SAMPLES
            );
            phone_occupancy[phone] = occupancy;
            phone_clips[phone] = clips;
            phone_unknowns[phone] = unknowns;

            $display("SSI263 PHONE phone=%02h peak=%0d rms=%0d ms=%0d occupancy=%0d clips=%0d unknowns=%0d stop=%0d voiced=%0d fricative=%0d fric1=%0d",
                     phone[5:0], peak, phone_rms[phone],
                     phone_mean_square[phone], occupancy, clips, unknowns,
                     dut.audio_i.stop_class, dut.core_i.voiced,
                     dut.core_i.fricative, dut.core_i.fric1_sw);

            check(clips == 0,
                  $sformatf("phone %02h reached a PCM rail", phone));
            check(unknowns == 0,
                  $sformatf("phone %02h produced unknown PCM", phone));
            check(!dut.audio_i.engine_overrun_q,
                  $sformatf("phone %02h overran the audio engine", phone));
        end
    endtask

    task automatic measure_simple_window(
        input integer samples,
        output integer peak,
        output integer occupancy,
        output longint mean_square,
        output integer clips,
        output integer unknowns
    );
        integer sample_count;
        integer sample_value;
        integer sample_abs;
        longint square_sum;
        begin
            peak = 0;
            occupancy = 0;
            clips = 0;
            unknowns = 0;
            square_sum = 0;
            for (sample_count = 0;
                 sample_count < samples;
                 sample_count = sample_count + 1) begin
                wait_audio_sample();
                note_pcm_state(clips, unknowns);
                if (!$isunknown(audio)) begin
                    sample_value = $signed(audio);
                    sample_abs = (sample_value < 0) ?
                                 -sample_value : sample_value;
                    if (sample_abs > peak)
                        peak = sample_abs;
                    if (sample_value != 0)
                        occupancy = occupancy + 1;
                    square_sum = square_sum + sample_value * sample_value;
                end
            end
            mean_square = square_sum / samples;
        end
    endtask

    task automatic test_held_stop_and_following_phone;
        integer timeout;
        integer sample_count;
        integer unused_occupancy;
        integer local_clips;
        integer local_unknowns;
        integer f5_abs;
        longint unused_mean_square;
        logic source_stayed_silent;
        logic saw_following_source;
        begin
            start_phone(6'h01);
            wait_phone_controls(6'h01, transient_clips, transient_unknowns);
            // The $A00 pitch vector can leave more than 256 audio samples
            // between source pulses.  Wait for a real F5 response, then put
            // the stop marker in while that response still occupies the
            // tract.  This tests decay instead of an arbitrary time window.
            timeout = 0;
            precharge_peak = 0;
            unused_occupancy = 0;
            local_clips = 0;
            local_unknowns = 0;
            while (sample_magnitude(dut.audio_i.f5_state_q) < 256 &&
                   timeout < 200000) begin
                @(posedge clk);
                #1;
                if (audio_tick) begin
                    if (sample_magnitude(audio) > precharge_peak)
                        precharge_peak = sample_magnitude(audio);
                    if (audio != 16'sd0)
                        unused_occupancy = unused_occupancy + 1;
                    note_pcm_state(local_clips, local_unknowns);
                end
                timeout = timeout + 1;
            end
            check(timeout < 200000 &&
                  sample_magnitude(dut.audio_i.f5_state_q) >= 256,
                  "voiced precharge did not fill the tract");
            measure_simple_window(16, precharge_peak, unused_occupancy,
                                  unused_mean_square, local_clips,
                                  local_unknowns);
            check(precharge_peak > 0 && unused_occupancy > 0,
                  "voiced F5 response did not reach PCM");
            check(local_clips == 0 && local_unknowns == 0,
                  "voiced precharge produced invalid PCM");

            write_phone(6'h27); // P: one of the five documented silent stops.
            timeout = 0;
            while (!dut.audio_i.stop_class && timeout < 20000) begin
                @(posedge clk);
                #1;
                timeout = timeout + 1;
            end
            check(timeout < 20000 && dut.core_i.phoneme == 6'h27,
                  "P did not expose its ROM stop marker");
            check(!dut.audio_i.source_voiced &&
                  !dut.audio_i.source_fricative,
                  "held P did not mute both normal sources");

            early_tail_peak = 0;
            early_f5_peak = 0;
            source_stayed_silent = 1'b1;
            for (sample_count = 0; sample_count < 128;
                 sample_count = sample_count + 1) begin
                wait_audio_sample();
                if (sample_magnitude(audio) > early_tail_peak)
                    early_tail_peak = sample_magnitude(audio);
                f5_abs = sample_magnitude(dut.audio_i.f5_state_q);
                if (f5_abs > early_f5_peak)
                    early_f5_peak = f5_abs;
                if (dut.audio_i.source_voiced ||
                    dut.audio_i.source_fricative ||
                    dut.audio_i.voice_source != 24'sd0 ||
                    dut.audio_i.fric_source != 24'sd0)
                    source_stayed_silent = 1'b0;
            end
            check(source_stayed_silent,
                  "held P generated a replacement release source");
            check(early_f5_peak > 0,
                  "voiced tract had no decaying state after P began");

            wait_phone_controls(6'h27, transient_clips, transient_unknowns);
            for (sample_count = 0; sample_count < 1536;
                 sample_count = sample_count + 1) begin
                wait_audio_sample();
                if (dut.audio_i.source_voiced ||
                    dut.audio_i.source_fricative)
                    source_stayed_silent = 1'b0;
            end

            late_stop_peak = 0;
            late_f5_peak = 0;
            for (sample_count = 0; sample_count < 256;
                 sample_count = sample_count + 1) begin
                wait_audio_sample();
                if (sample_magnitude(audio) > late_stop_peak)
                    late_stop_peak = sample_magnitude(audio);
                f5_abs = sample_magnitude(dut.audio_i.f5_state_q);
                if (f5_abs > late_f5_peak)
                    late_f5_peak = f5_abs;
                if (dut.audio_i.source_voiced ||
                    dut.audio_i.source_fricative)
                    source_stayed_silent = 1'b0;
            end
            check(source_stayed_silent,
                  "held P source did not stay silent");
            check(late_f5_peak * 32 < early_f5_peak,
                  "held P tract state did not decay toward silence");
            check(late_stop_peak * 32 < precharge_peak,
                  "held P PCM did not decay toward silence");

            $display("SSI263 STOP DECAY precharge_peak=%0d early_tail_peak=%0d early_f5_peak=%0d late_peak=%0d late_f5_peak=%0d",
                     precharge_peak, early_tail_peak, early_f5_peak,
                     late_stop_peak, late_f5_peak);

            // The next ordinary phone owns the onset.  HF is a normal
            // FRIC_1 phone, so no stop-release state or guessed burst is used.
            write_phone(6'h2C);
            timeout = 0;
            saw_following_source = 1'b0;
            while (!saw_following_source && timeout < 20000) begin
                @(posedge clk);
                #1;
                saw_following_source =
                    dut.core_i.phoneme == 6'h2C &&
                    !dut.audio_i.stop_class &&
                    dut.audio_i.source_fricative &&
                    dut.audio_i.source_fric1_sw;
                timeout = timeout + 1;
            end
            check(timeout < 20000 && saw_following_source,
                  "P to HF did not enable the following ROM source");

            wait_phone_controls(6'h2C, transient_clips, transient_unknowns);
            measure_simple_window(256, following_peak, following_occupancy,
                                  following_mean_square, following_clips,
                                  following_unknowns);
            check(following_peak > 0 && following_occupancy > 0,
                  "following HF phone produced no normal onset");
            check(following_clips == 0 && following_unknowns == 0,
                  "following HF onset produced invalid PCM");
            $display("SSI263 STOP FOLLOW phone=2C peak=%0d rms=%0d occupancy=%0d clips=%0d",
                     following_peak, integer_sqrt(following_mean_square),
                     following_occupancy, following_clips);
        end
    endtask

    initial begin
        $readmemh("ssi263_sc02_rom.mem", expected_rom);

        start_phone(6'h00);
        for (phone_index = 0; phone_index < 64;
             phone_index = phone_index + 1) begin
            write_phone(phone_index[5:0]);
            wait_phone_controls(phone_index[5:0], transient_clips,
                                transient_unknowns);
            if (dut.audio_i.stop_class)
                stop_mask = stop_mask | (64'b1 << phone_index);
            measure_phone(phone_index, transient_clips, transient_unknowns);
        end

        $display("SSI263 STOP MASK actual=%016h expected=000003b000000000",
                 stop_mask);
        check(stop_mask == 64'h000003B000000000,
              "PW2/PW3 stop marker set was not B,D,P,T,K");

        // These three sustained fricatives cover both injection routes:
        // S uses FRIC_2 while F and SCH use FRIC_1.
        check(phone_peak[6'h30] > 0 && phone_rms[6'h30] > 0 &&
              phone_occupancy[6'h30] >= LEVEL_SAMPLES / 4,
              "sustained S output was absent or sparse");
        check(phone_peak[6'h34] > 0 && phone_rms[6'h34] > 0 &&
              phone_occupancy[6'h34] >= LEVEL_SAMPLES / 4,
              "sustained F output was absent or sparse");
        check(phone_peak[6'h32] > 0 && phone_rms[6'h32] > 0 &&
              phone_occupancy[6'h32] >= LEVEL_SAMPLES / 4,
              "sustained SCH output was absent or sparse");
        check(!expected_rom[6'h30 * 8 + 2][3] &&
              expected_rom[6'h34 * 8 + 2][3] &&
              expected_rom[6'h32 * 8 + 2][3],
              "representative ROM fricative route assumptions changed");

        $display("SSI263 REPRESENTATIVE E01 peak=%0d rms=%0d occupancy=%0d; S30 peak=%0d rms=%0d occupancy=%0d; SCH32 peak=%0d rms=%0d occupancy=%0d; F34 peak=%0d rms=%0d occupancy=%0d",
                 phone_peak[6'h01], phone_rms[6'h01],
                 phone_occupancy[6'h01], phone_peak[6'h30],
                 phone_rms[6'h30], phone_occupancy[6'h30],
                 phone_peak[6'h32], phone_rms[6'h32],
                 phone_occupancy[6'h32], phone_peak[6'h34],
                 phone_rms[6'h34], phone_occupancy[6'h34]);

        test_held_stop_and_following_phone();

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
