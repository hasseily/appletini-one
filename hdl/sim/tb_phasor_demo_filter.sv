`timescale 1ns / 1ps

// Real Phasor bus + both SSI263AP voices. The initialization writes are from
// PHASOR1.DSK Ver 1.1.0, TTS $775B-$776B. Disk SHA256:
// 1ac1856df6e54f026f642c5f75e004fb6fc044c95eed1d413d6604d17dc1f9f8
// ReActiveMicro distribution: Apple II Items/Hardware/Phasor/Software/PHASOR1.DSK.
// The demo's Pitch parameter at $770F is register4, not inflection. Its image
// default is $E8. Native FF128 is a divider setting, not a bypass.
module tb_phasor_demo_filter;
    `define PRIMARY dut.ssi263_primary_i.bus_wrapper_i
    `define SECONDARY dut.ssi263_secondary_i.bus_wrapper_i
    `define PBACK `PRIMARY.native_engine_i
    `define SBACK `SECONDARY.native_engine_i
    `define PTIMING `PRIMARY.response_timing_i
    `define STIMING `SECONDARY.response_timing_i

    localparam integer FRAME_CYCLES = 2778;
    localparam integer AUDIO_FRAMES = 2048;
    localparam integer MODE_FRAMES = 1024;
    logic clk = 1'b0;
    logic rstn = 1'b0;
    logic q3 = 1'b0;
    logic q3_ringing = 1'b0;
    wire q3_pin = q3 ^ (q3_ringing &&
                  ((q3_div >= 10 && q3_div < 14) ||
                   (q3_div >= 42 && q3_div < 48)));
    logic audio_tick = 1'b0;
    logic [2:0] slot = 3'd4;
    logic [31:0] audio_control = 32'h10000000; // +2 dB SSI; neutral common tone controls.
    globals::AppleBus_read bus = '0;
    globals::SoftSwitchState sss = '0;
    globals::AppleBus_write response;
    logic signed [15:0] audio_l, audio_r;
    integer checks = 0;
    integer write_count = 0;
    integer source_checks = 0;
    integer primary_samples = 0, secondary_samples = 0, completions = 0;
    integer audio_frames = 0;
    integer q3_div = 0;
    integer audio_file;
    integer mode_audio_file;
    integer mode_pcm_checks = 0;
    integer ringing_pcm_checks = 0;
    logic signed [15:0] mode_primary_reference [0:MODE_FRAMES-1];
    logic signed [15:0] mode_secondary_reference [0:MODE_FRAMES-1];
    integer secondary_expected;
    logic compare_sources = 1'b0;

    always #3.75 clk = ~clk;
    // Worst-case physical input spacing: Q3 every65 fabric clocks and DIV2
    // gives130 clocks/effective XCK. Audio requests remain about48 kHz.
    always @(negedge clk) begin
        if (!rstn) begin
            q3_div = 0;
            q3 = 1'b0;
        end else begin
            q3_div = (q3_div + 1) % 65;
            q3 = q3_div >= 28; // About 280 ns high / 210 ns low.
        end
    end

    mockingboard dut (
        .clk(clk), .rstn(rstn), .apple_q3_raw(q3_pin), .ab_read(bus), .sss(sss),
        .slot_assign(slot), .pan(48'h888888888888), .ssi_pan(8'hF0), .audio_control(audio_control),
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
        q3_ringing = 1'b0;
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
        require(`PBACK.controller_i.debug_filter_frequency == 0 &&
                `SBACK.controller_i.debug_filter_frequency == 0,
                "native controller FF reset must match retained bus contract");
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
        require(`PRIMARY.filter_freq_q == 8'(primary_ff) &&
                `SECONDARY.filter_freq_q == 8'(secondary_ff),
                "socket FF value/selection mismatch");
        require(`PBACK.controller_i.debug_filter_frequency == 8'(primary_ff) &&
                `SBACK.controller_i.debug_filter_frequency == 8'(secondary_ff),
                "bus FF did not reach native controller");
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
        if (rstn) begin
            if (`PRIMARY.native_fault || `SECONDARY.native_fault)
                $fatal(1, "PHASOR DEMO FILTER FAIL: native engine scheduling fault");
            if (dut.ssi1_audio_valid) primary_samples = primary_samples + 1;
            if (dut.ssi0_audio_valid) secondary_samples = secondary_samples + 1;
        end
        if (compare_sources && rstn) begin
            source_checks = source_checks + 1;
            // FF can change source feedback/phase state. Only the independent
            // response counters and Apple-visible completion path must match.
            if ({`PTIMING.duration_left_q, `PTIMING.response_left_q,
                 `PTIMING.duration_phase_q, `PTIMING.response_phase_q,
                 `PRIMARY.backend_done, `PRIMARY.d7_q, `PRIMARY.direct_irq_q} !==
                {`STIMING.duration_left_q, `STIMING.response_left_q,
                 `STIMING.duration_phase_q, `STIMING.response_phase_q,
                 `SECONDARY.backend_done, `SECONDARY.d7_q, `SECONDARY.direct_irq_q})
                $fatal(1, "PHASOR DEMO FILTER FAIL: FF changed independent response/IRQ timing");
            if (`PRIMARY.backend_done) completions = completions + 1;
        end
    end

    task automatic audio_window(input integer ff);
        integer different, present_primary, present_reference, card_present;
        integer primary_before, secondary_before, completion_before;
        different = 0;
        present_primary = 0;
        present_reference = 0;
        card_present = 0;
        reset_card();
        select_mode(3'd5);
        // Function2 immediate pitch, then fastest duration/rate and ART7.
        // High pitch gives enough pulses even at FF255's narrow glottal drive.
        write_bus(16'hC463, 8'hFF);
        write_bus(16'hC460, 8'h80);
        write_bus(16'hC461, 8'hEC);
        write_bus(16'hC462, 8'hF8);
        write_bus(16'hC464, 8'h80);
        write_bus(16'hC444, 8'(ff));
        write_bus(16'hC463, 8'h7F);
        write_bus(16'hC460, 8'hCE); // AH, D3; both sockets start on this edge.
        check_filters(ff, 128);
        compare_sources = 1'b1;
        completion_before = completions;
        for (integer frame = 0; frame < AUDIO_FRAMES; frame = frame + 1) begin
            @(negedge clk);
            primary_before = primary_samples;
            secondary_before = secondary_samples;
            audio_tick = 1'b1;
            @(negedge clk);
            audio_tick = 1'b0;
            repeat (FRAME_CYCLES - 2) @(negedge clk);
            require(primary_samples == primary_before + 1 && secondary_samples == secondary_before + 1,
                    "native voices must publish exactly one valid sample per audio tick");
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
        require(present_primary > 16 && present_reference > 32 && card_present > 32,
                "real speech/output path did not produce enough nonzero samples");
        require(ff == 128 ? different == 0 : different > 32,
                "identical FF mismatch or low/high FF failed to change speech output");
        require(completions > completion_before + 2, "audio window missed response/IRQ completion cycles");
        $display("PHASOR AUDIO ff=%0d changed=%0d primary_nonzero=%0d reference_nonzero=%0d card_nonzero=%0d",
                 ff, different, present_primary, present_reference, card_present);
    endtask

    task automatic mode_audio_window(input integer ff, input logic [2:0] mode,
                                     input logic ringing = 1'b0);
        integer primary_before, secondary_before, nonzero_primary, nonzero_secondary;
        nonzero_primary = 0;
        nonzero_secondary = 0;
        reset_card();
        // Opposite-level 30/45 ns pulses occur after each true edge has
        // settled. They must not change either socket's clock or PCM.
        q3_ringing = ringing;
        // Perform one mode access in both runs so Q3 and sample phases match.
        select_mode(mode);
        write_bus(16'hC463, 8'hFF);
        write_bus(16'hC460, 8'h80); // Immediate pitch, identical for both modes.
        write_bus(16'hC461, 8'hEC);
        write_bus(16'hC462, 8'hF8);
        write_bus(16'hC464, 8'(ff));
        write_bus(16'hC463, 8'h7F);
        write_bus(16'hC460, 8'hCE); // AH on both sockets.
        check_filters(ff, ff);
        for (integer frame = 0; frame < MODE_FRAMES; frame = frame + 1) begin
            @(negedge clk);
            primary_before = primary_samples;
            secondary_before = secondary_samples;
            audio_tick = 1'b1;
            @(negedge clk);
            audio_tick = 1'b0;
            repeat (FRAME_CYCLES - 2) @(negedge clk);
            require(primary_samples == primary_before + 1 && secondary_samples == secondary_before + 1,
                    "mode comparison must publish one sample per socket and request");
            if (dut.ssi1_audio != 0) nonzero_primary = nonzero_primary + 1;
            if (dut.ssi0_audio != 0) nonzero_secondary = nonzero_secondary + 1;
            if (mode == 0) begin
                mode_primary_reference[frame] = dut.ssi1_audio;
                mode_secondary_reference[frame] = dut.ssi0_audio;
            end else begin
                // Compare before mixing: the card mode legitimately changes
                // AY selection and interrupt routing, not native SSI PCM.
                require(dut.ssi1_audio === mode_primary_reference[frame],
                        "primary SSI PCM changed between Mockingboard and Phasor modes");
                require(dut.ssi0_audio === mode_secondary_reference[frame],
                        "secondary SSI PCM changed between Mockingboard and Phasor modes");
                if (ringing) ringing_pcm_checks = ringing_pcm_checks + 2;
                else mode_pcm_checks = mode_pcm_checks + 2;
            end
            if (!ringing)
                $fdisplay(mode_audio_file, "%0d,%0d,%0d,%0d,%0d", ff, mode, frame,
                          $signed(dut.ssi1_audio), $signed(dut.ssi0_audio));
        end
        require(nonzero_primary > 16 && nonzero_secondary > 16,
                "mode comparison must include nonzero native speech on both sockets");
        $display("PHASOR MODE AUDIO ff=%0d mode=%0d frames=%0d primary_nonzero=%0d secondary_nonzero=%0d",
                 ff, mode, MODE_FRAMES, nonzero_primary, nonzero_secondary);
    endtask

    initial begin
        audio_file = $fopen("phasor_audio.csv", "w");
        require(audio_file != 0, "cannot open audio evidence");
        mode_audio_file = $fopen("phasor_mode_audio.csv", "w");
        require(mode_audio_file != 0, "cannot open mode audio evidence");
        for (integer mode = 0; mode < 2; mode = mode + 1) begin
            reset_card();
            if (mode) select_mode(3'd5);
            replay_demo(8'hE8);
            replay_demo(8'h80);
            secondary_expected = 0;
            // Aliases4..7 and every data value, each socket separately then both.
            for (integer alias_reg = 4; alias_reg < 8; alias_reg = alias_reg + 1) begin
                for (integer value = 0; value < 256; value = value + 1) begin
                    write_bus(16'hC440 + 16'(alias_reg), 8'(value));
                    check_filters(value, secondary_expected);
                    write_bus(16'hC420 + 16'(alias_reg), 8'(255 - value));
                    check_filters(value, 255 - value);
                    write_bus(16'hC460 + 16'(alias_reg), 8'(value));
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
                write_bus(16'hC464, 8'(value));
                check_filters(128, 113);
            end
        end
        audio_window(0);
        audio_window(128);
        audio_window(255);
        $fclose(audio_file);
        mode_audio_window(128, 3'd0);
        mode_audio_window(128, 3'd5);
        mode_audio_window(232, 3'd0);
        mode_audio_window(232, 3'd5);
        mode_audio_window(232, 3'd5, 1'b1);
        $fclose(mode_audio_file);
        $display("PHASOR Q3 RINGING PCM PASS comparisons=%0d frames=%0d",
                 ringing_pcm_checks, MODE_FRAMES);
        $display("PHASOR MODE PCM PASS comparisons=%0d frames_per_run=%0d", mode_pcm_checks, MODE_FRAMES);
        $display("PHASOR DEMO FILTER PASS checks=%0d writes=%0d source_checks=%0d audio_frames=%0d completions=%0d",
                 checks, write_count, source_checks, audio_frames, completions);
        $finish;
    end

    initial begin
        #300000000;
        $fatal(1, "PHASOR DEMO FILTER FAIL: timeout");
    end
endmodule
