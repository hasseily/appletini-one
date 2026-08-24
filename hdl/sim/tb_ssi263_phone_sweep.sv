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
        logic controls_match;
        begin
            timeout = 0;
            positive_rails = 0;
            negative_rails = 0;
            unknowns = 0;
            reconstruction_max = -8_388_608;
            reconstruction_min = 8_388_607;
            row = phone * 8;
            controls_match = 1'b0;
            while (!controls_match && timeout < 200000) begin
                @(posedge clk);
                #1;
                if (audio_tick)
                    note_pcm_state(positive_rails, negative_rails, unknowns,
                                   reconstruction_max, reconstruction_min);
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
                    dut.core_i.pw_0 == expected_rom[row + 0][0] &&
                    dut.core_i.pw_1 == expected_rom[row + 1][0] &&
                    dut.core_i.pw_2 == expected_rom[row + 2][2] &&
                    dut.core_i.pw_3 == !expected_rom[row + 2][1] &&
                    dut.core_i.pw_5 == !expected_rom[row + 2][2];
                if (xck_ce)
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

    initial begin
        $readmemh("ssi263_sc02_rom.mem", expected_rom);

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
