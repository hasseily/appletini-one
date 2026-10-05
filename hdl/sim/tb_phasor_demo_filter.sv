`timescale 1ns / 1ps

// Real Phasor bus + both SSI263AP voices. The initialization writes are from
// PHASOR1.DSK Ver 1.1.0, TTS $775B-$776B. Disk SHA256:
// 1ac1856df6e54f026f642c5f75e004fb6fc044c95eed1d413d6604d17dc1f9f8
// ReActiveMicro distribution: Apple II Items/Hardware/Phasor/Software/PHASOR1.DSK.
// The demo's Pitch parameter at $770F is register4, not inflection. Its image
// default is $E8; this regression also checks the new explicit $80 bypass.
module tb_phasor_demo_filter;
    `define PRIMARY dut.ssi263_primary_i.bus_wrapper_i
    `define SECONDARY dut.ssi263_secondary_i.bus_wrapper_i
    `define PBACK `PRIMARY.formant_backend_i
    `define SBACK `SECONDARY.formant_backend_i

    localparam integer FRAME_CYCLES = 512;
    logic clk = 1'b0;
    logic rstn = 1'b0;
    logic q3 = 1'b0;
    logic audio_tick = 1'b0;
    logic [2:0] slot = 3'd4;
    logic [31:0] audio_control = 32'd0;
    globals::AppleBus_read bus = '0;
    globals::SoftSwitchState sss = '0;
    globals::AppleBus_write response;
    logic signed [15:0] audio_l, audio_r;
    integer checks = 0;
    integer write_count = 0;
    integer source_checks = 0;
    integer audio_frames = 0;
    integer q3_div = 0;
    integer audio_file;
    integer secondary_expected;
    logic compare_sources = 1'b0;

    always #5 clk = ~clk;
    // Compressed simulation cadence. Real synchronizer and XCK conversion run;
    // this test makes no claim about the resulting physical sample frequency.
    always @(negedge clk) begin
        if (!rstn) begin
            q3_div = 0;
            q3 = 1'b0;
        end else begin
            q3_div = (q3_div + 1) % 8;
            q3 = q3_div >= 4;
        end
    end

    mockingboard dut (
        .clk(clk), .rstn(rstn), .apple_q3_raw(q3), .ab_read(bus), .sss(sss),
        .slot_assign(slot), .pan(48'h888888888888), .audio_control(audio_control),
        .audio_sample_tick(audio_tick), .ab_write(response),
        .audio_l(audio_l), .audio_r(audio_r), .dbg_ssi_irq(),
        .dbg_ssi_backend_done(), .dbg_ssi_enable_ints()
    );

    task automatic require(input logic condition, input string message);
        checks = checks + 1;
        if (condition !== 1'b1)
            $fatal(1, "PHASOR DEMO FILTER FAIL: %s", message);
    endtask

    task automatic reset_card;
        @(negedge clk);
        compare_sources = 1'b0;
        rstn = 1'b0;
        audio_tick = 1'b0;
        bus = '0;
        bus.res = 1'b1;
        bus.rw = 1'b1;
        bus.cycle_valid = 1'b1;
        sss = '0;
        repeat (8) @(negedge clk);
        rstn = 1'b1;
        repeat (8) @(negedge clk);
        require(dut.phasor_mode_q == 0, "reset did not select Mockingboard mode");
        require(`PRIMARY.filter_freq_q == 0 && `SECONDARY.filter_freq_q == 0,
                "hardware FF reset registers must remain zero");
        require(`PBACK.filter_step_q == 128 && `SBACK.filter_step_q == 128,
                "FF zero did not settle to Q8 step128");
    endtask

    task automatic bus_access(input logic [15:0] address,
                              input logic [7:0] value,
                              input logic reading,
                              input logic slot_access,
                              input logic write_phase);
        @(negedge clk);
        bus.addr = address;
        bus.data = value;
        bus.rw = reading;
        bus.serve_en = 1'b1;
        bus.data_en = write_phase;
        sss.slot_access = slot_access;
        @(negedge clk);
        bus.serve_en = 1'b0;
        bus.data_en = 1'b0;
        sss.slot_access = 1'b0;
        repeat (4) @(negedge clk);
    endtask

    task automatic write_bus(input logic [15:0] address, input logic [7:0] value);
        bus_access(address, value, 1'b0, 1'b1, 1'b1);
        write_count = write_count + 1;
    endtask

    task automatic select_mode(input logic [2:0] mode);
        // Real slot4 $C0C8 clear-and-select mode access. $C0CD is the TTS read.
        bus_access(16'hC0C8 | {13'd0, mode}, 8'd0, 1'b1, 1'b0, 1'b0);
        require(dut.phasor_mode_q == mode, "Phasor C0nX mode selection failed");
    endtask

    task automatic check_filters(input integer primary_ff, input integer secondary_ff);
        require(`PRIMARY.filter_freq_q == primary_ff &&
                `SECONDARY.filter_freq_q == secondary_ff,
                "socket FF value/selection mismatch");
        require(`PBACK.filter_freq == primary_ff && `SBACK.filter_freq == secondary_ff,
                "bus FF did not reach real formant backend");
        require(`PBACK.filter_step_q == 128 + primary_ff &&
                `SBACK.filter_step_q == 128 + secondary_ff,
                "Q8 step must vary smoothly by one for each FF code");
    endtask

    task automatic replay_demo(input logic [7:0] pitch);
        write_bus(16'hC443, 8'h80);
        write_bus(16'hC440, 8'hC0);
        write_bus(16'hC441, 8'h40);
        write_bus(16'hC442, 8'hA8);
        write_bus(16'hC443, 8'h5A);
        write_bus(16'hC444, pitch);
        require(`PRIMARY.duration_phoneme_q == 8'hC0 &&
                `PRIMARY.inflection_q == 8'h40 &&
                `PRIMARY.rate_inflection_q == 8'hA8 &&
                `PRIMARY.ctrl_art_amp_q == 8'h5A,
                "original TTS initialization changed another speech register");
        require(!`PRIMARY.active_is_votrax_q, "demo must run SSI263, not SC01");
        check_filters(pitch, 0);
    endtask

    always @(posedge clk) begin
        #1;
        if (compare_sources && rstn) begin
            source_checks = source_checks + 1;
            if ({`PBACK.core_ticks, `PBACK.core_pitch, `PBACK.core_pitch_noise_gate,
                 `PBACK.core_noise_bit, `PBACK.core_closure_age, `PBACK.core_cur_f1,
                 `PBACK.core_cur_f2, `PBACK.core_cur_f3, `PBACK.core_cur_fa,
                 `PBACK.core_cur_fc, `PBACK.core_cur_va, `PBACK.core_cur_f2q,
                 `PBACK.phoneme_done, `PBACK.response_done} !==
                {`SBACK.core_ticks, `SBACK.core_pitch, `SBACK.core_pitch_noise_gate,
                 `SBACK.core_noise_bit, `SBACK.core_closure_age, `SBACK.core_cur_f1,
                 `SBACK.core_cur_f2, `SBACK.core_cur_f3, `SBACK.core_cur_fa,
                 `SBACK.core_cur_fc, `SBACK.core_cur_va, `SBACK.core_cur_f2q,
                 `SBACK.phoneme_done, `SBACK.response_done})
                $fatal(1, "PHASOR DEMO FILTER FAIL: FF changed source or completion timing");
        end
    end

    task automatic audio_window(input integer ff);
        integer different, present_primary, present_reference, card_present;
        different = 0;
        present_primary = 0;
        present_reference = 0;
        card_present = 0;
        reset_card();
        select_mode(3'd5);
        // Both-select writes start the same voiced SSI phoneme on the same edge.
        write_bus(16'hC463, 8'h8F);
        write_bus(16'hC460, 8'hAD);
        write_bus(16'hC461, 8'h52);
        write_bus(16'hC462, 8'hB8);
        write_bus(16'hC444, ff);
        write_bus(16'hC424, 8'h80);
        write_bus(16'hC463, 8'h0F);
        check_filters(ff, 128);
        compare_sources = 1'b1;
        for (integer frame = 0; frame < 1024; frame = frame + 1) begin
            @(negedge clk);
            audio_tick = 1'b1;
            @(negedge clk);
            audio_tick = 1'b0;
            repeat (FRAME_CYCLES - 1) @(negedge clk);
            require(`PBACK.synth_state_q == `PBACK.SYNTH_IDLE &&
                    `SBACK.synth_state_q == `SBACK.SYNTH_IDLE,
                    "real speech backend missed audio sample budget");
            require((^dut.ssi1_audio) !== 1'bx && (^dut.ssi0_audio) !== 1'bx,
                    "unknown speech audio sample");
            if (dut.ssi1_audio != dut.ssi0_audio) different = different + 1;
            if (dut.ssi1_audio != 0) present_primary = present_primary + 1;
            if (dut.ssi0_audio != 0) present_reference = present_reference + 1;
            if (audio_l != 0 || audio_r != 0) card_present = card_present + 1;
            $fdisplay(audio_file, "%0d,%0d,%0d,%0d,%0d,%0d", ff, frame,
                      $signed(dut.ssi1_audio), $signed(dut.ssi0_audio),
                      $signed(audio_l), $signed(audio_r));
            audio_frames = audio_frames + 1;
        end
        compare_sources = 1'b0;
        require(present_primary > 32 && present_reference > 32 && card_present > 32,
                "real speech/output path did not produce enough nonzero samples");
        require(ff == 128 ? different == 0 : different > 32,
                "FF bypass mismatch or low/high FF failed to change speech output");
        $display("PHASOR AUDIO ff=%0d changed=%0d primary_nonzero=%0d reference_nonzero=%0d card_nonzero=%0d",
                 ff, different, present_primary, present_reference, card_present);
    endtask

    initial begin
        audio_file = $fopen("phasor_audio.csv", "w");
        require(audio_file != 0, "cannot open audio evidence");
        for (integer mode = 0; mode < 2; mode = mode + 1) begin
            reset_card();
            if (mode) select_mode(3'd5);
            replay_demo(8'hE8);
            replay_demo(8'h80);
            require(`PBACK.filter_step_q == 256, "128 must be the exact bypass step");
            secondary_expected = 0;
            // Aliases4..7 and every data value, each socket separately then both.
            for (integer alias_reg = 4; alias_reg < 8; alias_reg = alias_reg + 1) begin
                for (integer value = 0; value < 256; value = value + 1) begin
                    write_bus(16'hC440 + alias_reg, value);
                    check_filters(value, secondary_expected);
                    write_bus(16'hC420 + alias_reg, 255 - value);
                    check_filters(value, 255 - value);
                    write_bus(16'hC460 + alias_reg, value);
                    check_filters(value, value);
                    secondary_expected = value;
                    require(!`PRIMARY.backend_start_q && !`SECONDARY.backend_start_q,
                            "filter write restarted speech");
                end
            end
            write_bus(16'hC444, 8'h80);
            write_bus(16'hC424, 8'h71);
            bus_access(16'hC544, 8'h33, 1'b0, 1'b1, 1'b1);
            bus_access(16'hC444, 8'h33, 1'b0, 1'b0, 1'b1);
            bus_access(16'hC444, 8'h33, 1'b0, 1'b1, 1'b0);
            bus_access(16'hC444, 8'h33, 1'b1, 1'b1, 1'b1);
            check_filters(128, 113);
            select_mode(3'd7);
            for (integer value = 0; value < 256; value = value + 1) begin
                write_bus(16'hC464, value);
                check_filters(128, 113);
            end
        end
        audio_window(0);
        audio_window(128);
        audio_window(255);
        $fclose(audio_file);
        $display("PHASOR DEMO FILTER PASS checks=%0d writes=%0d source_checks=%0d audio_frames=%0d",
                 checks, write_count, source_checks, audio_frames);
        $finish;
    end

    initial begin
        #50000000;
        $fatal(1, "PHASOR DEMO FILTER FAIL: timeout");
    end
endmodule
