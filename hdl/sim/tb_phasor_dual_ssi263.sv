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
    logic apple_q3_raw = 1'b0;
    globals::AppleBus_read ab_read = '0;
    globals::SoftSwitchState sss = '0;
    globals::AppleBus_write ab_write;
    logic [2:0] slot_assign = SLOT;
    logic card_enable = 1'b1;
    logic [47:0] pan = 48'h5555_5555_5555;
    logic [31:0] audio_control = 32'h0200_0000;
    logic audio_sample_tick = 1'b0;
    logic signed [15:0] audio_l;
    logic signed [15:0] audio_r;
    logic dbg_ssi_irq;
    logic dbg_ssi_backend_done;
    logic dbg_ssi_enable_ints;

    logic [11:0] sample_div_q = 12'd0;
    logic [7:0] expected_rom [0:511];
    integer failures = 0;
    integer checks = 0;
    logic [6:0] secondary_latch_coverage = 7'h00;
    logic [6:0] primary_latch_coverage = 7'h00;
    logic [1:0] secondary_phase_coverage = 2'b00;
    logic [1:0] primary_phase_coverage = 2'b00;
    logic secondary_latch_error = 1'b0;
    logic primary_latch_error = 1'b0;

    logic [3:0] sample_period_q = 4'd0;

    // Match the production 133.333344 MHz fabric clock. At the physical Q3
    // rate this gives about 65.185 engine clocks per rising edge.
    always #3.75 clk = ~clk;
    // Nominal Apple Q3 input, asynchronous to this bench's fabric clock.
    always #244.444 apple_q3_raw = ~apple_q3_raw;

    // 133.333344 MHz does not divide evenly to 48 kHz. Seven 2778-clock
    // periods and two 2777-clock periods give the required average cadence.
    always_ff @(posedge clk) begin
        if (!rstn) begin
            sample_div_q <= 12'd0;
            sample_period_q <= 4'd0;
            audio_sample_tick <= 1'b0;
        end else if (((sample_period_q == 4'd3 ||
                       sample_period_q == 4'd7) &&
                      sample_div_q == 12'd2776) ||
                     (sample_period_q != 4'd3 &&
                      sample_period_q != 4'd7 &&
                      sample_div_q == 12'd2777)) begin
            sample_div_q <= 12'd0;
            if (sample_period_q == 4'd8)
                sample_period_q <= 4'd0;
            else
                sample_period_q <= sample_period_q + 4'd1;
            audio_sample_tick <= 1'b1;
        end else begin
            sample_div_q <= sample_div_q + 12'd1;
            audio_sample_tick <= 1'b0;
        end
    end

    mockingboard dut (
        .clk(clk),
        .rstn(rstn),
        .apple_q3_raw(apple_q3_raw),
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

    // Prove both live dies carry each selected RESA value through the
    // transparent U106-U114 layer and then through the Phi0/Phi1 U170-U176
    // layer. These checks accept every legal intermediate DDA value; they do
    // not turn the rate-gated transition RAM into an immediate ROM copy.
    always @(posedge clk) begin : secondary_latch_monitor
        integer sampled_selector;
        logic sampled_latch_level;
        logic [3:0] sampled_resa;
        logic sampled_phase;
        logic [3:0] sampled_f1_first;
        logic [3:0] sampled_f2_first;
        logic [3:0] sampled_f2_res_first;
        logic [3:0] sampled_f3_first;
        logic [3:0] sampled_f4_first;
        logic [3:0] sampled_voice_first;
        logic [3:0] sampled_fric_first;

        if (rstn) begin
            sampled_selector = dut.ssi263_secondary_i.core_i.selector_q;
            sampled_latch_level =
                dut.ssi263_secondary_i.core_i.selector_latch_level;
            sampled_resa = dut.ssi263_secondary_i.core_i.parameter_resa_q[
                sampled_selector];
            sampled_phase = dut.ssi263_secondary_i.core_i.filter_phase_q;
            sampled_f1_first = dut.ssi263_secondary_i.core_i.f1_first_q;
            sampled_f2_first = dut.ssi263_secondary_i.core_i.f2_first_q;
            sampled_f2_res_first =
                dut.ssi263_secondary_i.core_i.f2_res_first_q;
            sampled_f3_first = dut.ssi263_secondary_i.core_i.f3_first_q;
            sampled_f4_first = dut.ssi263_secondary_i.core_i.f4_first_q;
            sampled_voice_first =
                dut.ssi263_secondary_i.core_i.voice_amp_first_q;
            sampled_fric_first =
                dut.ssi263_secondary_i.core_i.fric_amp_first_q;
            #1;

            if (sampled_latch_level && sampled_selector < 7) begin
                secondary_latch_coverage[sampled_selector] = 1'b1;
                case (sampled_selector)
                    0: if (dut.ssi263_secondary_i.core_i.f1_first_q !=
                           sampled_resa) secondary_latch_error = 1'b1;
                    1: if (dut.ssi263_secondary_i.core_i.f2_first_q !=
                           sampled_resa) secondary_latch_error = 1'b1;
                    2: if (dut.ssi263_secondary_i.core_i.f2_res_first_q !=
                           sampled_resa) secondary_latch_error = 1'b1;
                    3: if (dut.ssi263_secondary_i.core_i.f3_first_q !=
                           sampled_resa ||
                           dut.ssi263_secondary_i.core_i.f4_first_q !=
                           sampled_resa) secondary_latch_error = 1'b1;
                    4: if (dut.ssi263_secondary_i.core_i.filter_amp_first_q !=
                           sampled_resa) secondary_latch_error = 1'b1;
                    5: if (dut.ssi263_secondary_i.core_i.voice_amp_first_q !=
                           sampled_resa) secondary_latch_error = 1'b1;
                    6: if (dut.ssi263_secondary_i.core_i.fric_amp_first_q !=
                           sampled_resa) secondary_latch_error = 1'b1;
                    default: begin
                    end
                endcase
            end

            secondary_phase_coverage[sampled_phase] = 1'b1;
            if (!sampled_phase) begin
                if (dut.ssi263_secondary_i.core_i.f1_code_q !=
                        sampled_f1_first ||
                    dut.ssi263_secondary_i.core_i.f3_code_q !=
                        sampled_f3_first ||
                    dut.ssi263_secondary_i.core_i.voice_amp_code_q !=
                        sampled_voice_first)
                    secondary_latch_error = 1'b1;
            end else if (dut.ssi263_secondary_i.core_i.f2_code_q !=
                             sampled_f2_first ||
                         dut.ssi263_secondary_i.core_i.f2_res_code_q !=
                             sampled_f2_res_first ||
                         dut.ssi263_secondary_i.core_i.f4_code_q !=
                             sampled_f4_first ||
                         dut.ssi263_secondary_i.core_i.fric_amp_code_q !=
                             sampled_fric_first) begin
                secondary_latch_error = 1'b1;
            end
        end
    end

    always @(posedge clk) begin : primary_latch_monitor
        integer sampled_selector;
        logic sampled_latch_level;
        logic [3:0] sampled_resa;
        logic sampled_phase;
        logic [3:0] sampled_f1_first;
        logic [3:0] sampled_f2_first;
        logic [3:0] sampled_f2_res_first;
        logic [3:0] sampled_f3_first;
        logic [3:0] sampled_f4_first;
        logic [3:0] sampled_voice_first;
        logic [3:0] sampled_fric_first;

        if (rstn) begin
            sampled_selector = dut.ssi263_primary_i.core_i.selector_q;
            sampled_latch_level =
                dut.ssi263_primary_i.core_i.selector_latch_level;
            sampled_resa = dut.ssi263_primary_i.core_i.parameter_resa_q[
                sampled_selector];
            sampled_phase = dut.ssi263_primary_i.core_i.filter_phase_q;
            sampled_f1_first = dut.ssi263_primary_i.core_i.f1_first_q;
            sampled_f2_first = dut.ssi263_primary_i.core_i.f2_first_q;
            sampled_f2_res_first =
                dut.ssi263_primary_i.core_i.f2_res_first_q;
            sampled_f3_first = dut.ssi263_primary_i.core_i.f3_first_q;
            sampled_f4_first = dut.ssi263_primary_i.core_i.f4_first_q;
            sampled_voice_first =
                dut.ssi263_primary_i.core_i.voice_amp_first_q;
            sampled_fric_first =
                dut.ssi263_primary_i.core_i.fric_amp_first_q;
            #1;

            if (sampled_latch_level && sampled_selector < 7) begin
                primary_latch_coverage[sampled_selector] = 1'b1;
                case (sampled_selector)
                    0: if (dut.ssi263_primary_i.core_i.f1_first_q !=
                           sampled_resa) primary_latch_error = 1'b1;
                    1: if (dut.ssi263_primary_i.core_i.f2_first_q !=
                           sampled_resa) primary_latch_error = 1'b1;
                    2: if (dut.ssi263_primary_i.core_i.f2_res_first_q !=
                           sampled_resa) primary_latch_error = 1'b1;
                    3: if (dut.ssi263_primary_i.core_i.f3_first_q !=
                           sampled_resa ||
                           dut.ssi263_primary_i.core_i.f4_first_q !=
                           sampled_resa) primary_latch_error = 1'b1;
                    4: if (dut.ssi263_primary_i.core_i.filter_amp_first_q !=
                           sampled_resa) primary_latch_error = 1'b1;
                    5: if (dut.ssi263_primary_i.core_i.voice_amp_first_q !=
                           sampled_resa) primary_latch_error = 1'b1;
                    6: if (dut.ssi263_primary_i.core_i.fric_amp_first_q !=
                           sampled_resa) primary_latch_error = 1'b1;
                    default: begin
                    end
                endcase
            end

            primary_phase_coverage[sampled_phase] = 1'b1;
            if (!sampled_phase) begin
                if (dut.ssi263_primary_i.core_i.f1_code_q !=
                        sampled_f1_first ||
                    dut.ssi263_primary_i.core_i.f3_code_q !=
                        sampled_f3_first ||
                    dut.ssi263_primary_i.core_i.voice_amp_code_q !=
                        sampled_voice_first)
                    primary_latch_error = 1'b1;
            end else if (dut.ssi263_primary_i.core_i.f2_code_q !=
                             sampled_f2_first ||
                         dut.ssi263_primary_i.core_i.f2_res_code_q !=
                             sampled_f2_res_first ||
                         dut.ssi263_primary_i.core_i.f4_code_q !=
                             sampled_f4_first ||
                         dut.ssi263_primary_i.core_i.fric_amp_code_q !=
                             sampled_fric_first) begin
                primary_latch_error = 1'b1;
            end
        end
    end

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

    task automatic start_a5_phone(
        input logic [7:0] duration_phone
    );
        begin
            // Fixed register vector. ROM outputs remain the only per-phone
            // source of tract and switch controls.
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

    task automatic wait_for_secondary_scan(input logic [5:0] phone);
        integer timeout;
        integer row;
        logic [7:0] selectors_seen;
        logic [7:0] rom_rows_seen;
        begin
            timeout = 0;
            row = phone * 8;
            selectors_seen = 8'h00;
            rom_rows_seen = 8'h00;
            while (!(dut.ssi263_secondary_i.core_i.phone_active &&
                     dut.ssi263_secondary_i.core_i.phoneme == phone &&
                     selectors_seen == 8'hFF &&
                     rom_rows_seen == 8'hFF &&
                     !dut.ssi263_secondary_i.core_i.phone_setup_pending_q &&
                     !dut.ssi263_secondary_i.core_i.phone_setup_window_q &&
                     dut.ssi263_secondary_i.core_i.pw_2 ==
                         expected_rom[row + 2][2] &&
                     dut.ssi263_secondary_i.core_i.pw_5 ==
                         !expected_rom[row + 2][2]) &&
                   timeout < 200_000) begin
                @(posedge clk);
                #1;
                if (dut.ssi263_secondary_i.core_i.phone_active &&
                    dut.ssi263_secondary_i.core_i.phoneme == phone) begin
                    selectors_seen[
                        dut.ssi263_secondary_i.core_i.selector_q] = 1'b1;
                    if (dut.ssi263_secondary_i.core_i.selector_rom_data ==
                        expected_rom[
                            row + dut.ssi263_secondary_i.core_i.selector_q])
                        rom_rows_seen[
                            dut.ssi263_secondary_i.core_i.selector_q] = 1'b1;
                end
                timeout = timeout + 1;
            end
            #1;
            check(timeout < 200_000 && selectors_seen == 8'hFF &&
                  rom_rows_seen == 8'hFF,
                  "A5 secondary socket did not finish its ROM scan");
        end
    endtask

    task automatic wait_for_primary_scan(input logic [5:0] phone);
        integer timeout;
        integer row;
        logic [7:0] selectors_seen;
        logic [7:0] rom_rows_seen;
        begin
            timeout = 0;
            row = phone * 8;
            selectors_seen = 8'h00;
            rom_rows_seen = 8'h00;
            while (!(dut.ssi263_primary_i.core_i.phone_active &&
                     dut.ssi263_primary_i.core_i.phoneme == phone &&
                     selectors_seen == 8'hFF &&
                     rom_rows_seen == 8'hFF &&
                     !dut.ssi263_primary_i.core_i.phone_setup_pending_q &&
                     !dut.ssi263_primary_i.core_i.phone_setup_window_q &&
                     dut.ssi263_primary_i.core_i.pw_2 ==
                         expected_rom[row + 2][2] &&
                     dut.ssi263_primary_i.core_i.pw_5 ==
                         !expected_rom[row + 2][2]) &&
                   timeout < 200_000) begin
                @(posedge clk);
                #1;
                if (dut.ssi263_primary_i.core_i.phone_active &&
                    dut.ssi263_primary_i.core_i.phoneme == phone) begin
                    selectors_seen[
                        dut.ssi263_primary_i.core_i.selector_q] = 1'b1;
                    if (dut.ssi263_primary_i.core_i.selector_rom_data ==
                        expected_rom[
                            row + dut.ssi263_primary_i.core_i.selector_q])
                        rom_rows_seen[
                            dut.ssi263_primary_i.core_i.selector_q] = 1'b1;
                end
                timeout = timeout + 1;
            end
            #1;
            check(timeout < 200_000 && selectors_seen == 8'hFF &&
                  rom_rows_seen == 8'hFF,
                  "A6 primary socket did not finish its ROM scan");
        end
    endtask

    task automatic check_q3_div2_path(input string mode_name);
        integer pin_edges;
        integer secondary_edges;
        integer primary_edges;
        begin
            pin_edges = 0;
            secondary_edges = 0;
            primary_edges = 0;
            while (pin_edges < 32) begin
                @(posedge clk);
                #1;
                if (dut.ssi_xck_ce) begin
                    pin_edges = pin_edges + 1;
                    if (dut.ssi263_secondary_i.core_i.effective_xck_ce)
                        secondary_edges = secondary_edges + 1;
                    if (dut.ssi263_primary_i.core_i.effective_xck_ce)
                        primary_edges = primary_edges + 1;
                end
            end
            check(secondary_edges == 16 && primary_edges == 16,
                  $sformatf("%s did not feed Q3 through one DIV2 in each socket",
                            mode_name));
            check(dut.ssi263_secondary_i.core_i.div2_q &&
                  dut.ssi263_primary_i.core_i.div2_q,
                  $sformatf("%s changed an SSI DIV2 strap", mode_name));
        end
    endtask

    task automatic wait_for_a5_channel_a_audio;
        integer timeout;
        logic saw_chip_audio;
        logic saw_channel_a;
        logic crossed_to_channel_b;
        begin
            timeout = 0;
            saw_chip_audio = 1'b0;
            saw_channel_a = 1'b0;
            crossed_to_channel_b = 1'b0;
            while (!(saw_chip_audio && saw_channel_a) &&
                   timeout < 3_000_000) begin
                @(posedge clk);
                #1;
                if (dut.ssi0_audio != 16'sd0)
                    saw_chip_audio = 1'b1;
                if (audio_l != 16'sd0)
                    saw_channel_a = 1'b1;
                if (dut.ssi1_audio != 16'sd0 || audio_r != 16'sd0 ||
                    dut.ssi263_primary_i.audio_i.f1_state_q != 24'sd0 ||
                    dut.ssi263_primary_i.audio_i.f5_state_q != 24'sd0)
                    crossed_to_channel_b = 1'b1;
                timeout = timeout + 1;
            end
            check(timeout < 3_000_000 && saw_chip_audio && saw_channel_a &&
                  !crossed_to_channel_b,
                  "A5 secondary socket did not stay on channel A only");
            check(!dut.ssi263_secondary_i.audio_i.engine_overrun_q &&
                  !dut.ssi263_primary_i.audio_i.engine_overrun_q,
                  "A5-only route overran an audio engine");
        end
    endtask

    task automatic wait_for_a6_channel_b_audio;
        integer timeout;
        logic saw_chip_audio;
        logic saw_channel_b;
        logic crossed_to_channel_a;
        begin
            timeout = 0;
            saw_chip_audio = 1'b0;
            saw_channel_b = 1'b0;
            crossed_to_channel_a = 1'b0;
            while (!(saw_chip_audio && saw_channel_b) &&
                   timeout < 3_000_000) begin
                @(posedge clk);
                #1;
                if (dut.ssi1_audio != 16'sd0)
                    saw_chip_audio = 1'b1;
                if (audio_r != 16'sd0)
                    saw_channel_b = 1'b1;
                if (dut.ssi0_audio != 16'sd0 || audio_l != 16'sd0 ||
                    dut.ssi263_secondary_i.audio_i.f1_state_q != 24'sd0 ||
                    dut.ssi263_secondary_i.audio_i.f5_state_q != 24'sd0)
                    crossed_to_channel_a = 1'b1;
                timeout = timeout + 1;
            end
            check(timeout < 3_000_000 && saw_chip_audio && saw_channel_b &&
                  !crossed_to_channel_a,
                  "A6 primary socket did not stay on channel B only");
            check(!dut.ssi263_secondary_i.audio_i.engine_overrun_q &&
                  !dut.ssi263_primary_i.audio_i.engine_overrun_q,
                  "A6-only route overran an audio engine");
        end
    endtask

    task automatic wait_for_dual_stereo_audio;
        integer timeout;
        integer source_timeout;
        integer level_samples;
        integer ssi0_clips;
        integer ssi1_clips;
        integer left_clips;
        integer right_clips;
        integer unknowns;
        logic saw_secondary_filter_state;
        logic saw_primary_filter_state;
        logic saw_secondary_source;
        logic saw_primary_source;
        logic saw_ssi0_output;
        logic saw_ssi1_output;
        logic saw_channel_a;
        logic saw_channel_b;
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
                if (dut.ssi263_secondary_i.audio_i.voice_source_state_q != 24'sd0)
                    saw_secondary_source = 1'b1;
                if (dut.ssi263_primary_i.audio_i.voice_source_state_q != 24'sd0)
                    saw_primary_source = 1'b1;
                source_timeout = source_timeout + 1;
            end
            check(saw_secondary_source && saw_primary_source,
                  "both SSI chips did not produce a live voice pulse");

            timeout = 0;
            level_samples = 0;
            ssi0_clips = 0;
            ssi1_clips = 0;
            left_clips = 0;
            right_clips = 0;
            unknowns = 0;
            saw_secondary_filter_state = 1'b0;
            saw_primary_filter_state = 1'b0;
            saw_ssi0_output = 1'b0;
            saw_ssi1_output = 1'b0;
            saw_channel_a = 1'b0;
            saw_channel_b = 1'b0;
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
                        saw_ssi0_output = 1'b1;
                    if (dut.ssi1_audio != 16'sd0)
                        saw_ssi1_output = 1'b1;
                    if (audio_l != 16'sd0)
                        saw_channel_a = 1'b1;
                    if (audio_r != 16'sd0)
                        saw_channel_b = 1'b1;
                    if ($isunknown({dut.ssi0_line_audio,
                                    dut.ssi1_line_audio,
                                    dut.ssi0_audio, dut.ssi1_audio,
                                    dut.ssi0_output_clipped,
                                    dut.ssi1_output_clipped,
                                    audio_l, audio_r}))
                        unknowns = unknowns + 1;
                    if (dut.ssi0_output_clipped)
                        ssi0_clips = ssi0_clips + 1;
                    if (dut.ssi1_output_clipped)
                        ssi1_clips = ssi1_clips + 1;
                    if (audio_l == 16'sh7FFF || audio_l == -16'sh8000)
                        left_clips = left_clips + 1;
                    if (audio_r == 16'sh7FFF || audio_r == -16'sh8000)
                        right_clips = right_clips + 1;
                    level_samples = level_samples + 1;
                end
                timeout = timeout + 1;
            end
            $display("PHASOR SSI263 DUAL ROUTE samples=%0d ssi0=%0d ssi1=%0d channel_a=%0d channel_b=%0d clips=%0d unknowns=%0d",
                     level_samples, saw_ssi0_output, saw_ssi1_output,
                     saw_channel_a, saw_channel_b,
                     ssi0_clips + ssi1_clips + left_clips + right_clips,
                     unknowns);
            // The two pulse trains need not be nonzero on the same sample.
            check(level_samples == 512 && saw_ssi0_output &&
                  saw_ssi1_output && saw_channel_a && saw_channel_b &&
                  saw_secondary_filter_state &&
                  saw_primary_filter_state &&
                  dut.ssi263_secondary_i.core_i.phone_active &&
                  dut.ssi263_primary_i.core_i.phone_active,
                  "both SSI audio engines did not run independently");
            check(ssi0_clips == 0 && ssi1_clips == 0 &&
                  left_clips == 0 && right_clips == 0 && unknowns == 0,
                  "simultaneous speech clipped a chip or card route");
            check(!dut.ssi263_secondary_i.audio_i.engine_overrun_q &&
                  !dut.ssi263_primary_i.audio_i.engine_overrun_q,
                  "simultaneous speech overran an actual audio engine");
        end
    endtask

    task automatic mask_card_during_native_read;
        logic [2:0] secondary_selector_before;
        logic [2:0] primary_selector_before;
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
                  !dut.ssi263_secondary_i.core_i.powered_down &&
                  !dut.ssi263_primary_i.core_i.powered_down,
                  "disable test did not start with two active SSI dies");
            secondary_selector_before =
                dut.ssi263_secondary_i.core_i.selector;
            primary_selector_before = dut.ssi263_primary_i.core_i.selector;

            @(negedge clk);
            drive_idle();
            card_enable = 1'b0;
            repeat (2_000) @(posedge clk);
            #1;
            check(!dut.card_enabled && !ab_write.wr_data_en &&
                  !ab_write.assert_irq && !dbg_ssi_irq &&
                  !dut.ssi_read_drive,
                  "slot disable did not clear IRQ and registered read drive");
            check(audio_l == 16'sd0 && audio_r == 16'sd0 &&
                   dut.ssi0_audio == 16'sd0 && dut.ssi1_audio == 16'sd0,
                  "slot disable did not mute both card and SSI audio outputs");
            check(dut.phasor_mode_q == 3'd0 &&
                  !dut.ssi0_d7 && !dut.ssi1_d7 &&
                  !dut.ssi263_secondary_i.core_i.powered_down &&
                  !dut.ssi263_primary_i.core_i.powered_down &&
                  dut.ssi263_secondary_i.core_i.phone_active &&
                  dut.ssi263_primary_i.core_i.phone_active &&
                  dut.ssi263_secondary_i.core_i.d7_pending &&
                  dut.ssi263_primary_i.core_i.d7_pending &&
                  (dut.ssi263_secondary_i.core_i.selector !=
                       secondary_selector_before) &&
                  (dut.ssi263_primary_i.core_i.selector !=
                       primary_selector_before),
                  "slot disable reset a die instead of masking the card boundary");

            @(negedge clk);
            card_enable = 1'b1;
            repeat (8) @(posedge clk);
            #1;
            check(dut.card_enabled && dut.phasor_mode_q == 3'd0 &&
                  !dut.ssi263_secondary_i.core_i.powered_down &&
                  !dut.ssi263_primary_i.core_i.powered_down &&
                  dut.ssi0_d7 && dut.ssi1_d7 && !ab_write.wr_data_en,
                  "slot re-enable did not expose the two preserved SSI dies");

            apple_write(SLOT_BASE + 16'h0021, 8'h52);
            apple_write(SLOT_BASE + 16'h0041, 8'h64);
            check(dut.ssi263_secondary_i.core_i.inflection_high_q == 8'h52 &&
                  dut.ssi263_primary_i.core_i.inflection_high_q == 8'h64 &&
                  !ab_write.assert_irq,
                  "slot re-enable did not restore independent bus writes");
        end
    endtask

    logic read_drove;
    logic [7:0] read_value;
    logic speech_selected;
    logic [2:0] echo_selector_before;
    integer echo_timeout;

    initial begin
        $readmemh("ssi263_sc02_rom.mem", expected_rom);
        drive_idle();
        ab_read.res = 1'b1;
        hard_reset();

        check(dut.phasor_mode_q == 3'd0,
              "card did not reset to Mockingboard mode");
        check(dut.ssi263_secondary_i.core_i.powered_down &&
              dut.ssi263_primary_i.core_i.powered_down,
              "both AP sockets did not reset into power-down");

        // The original card feeds Q3 to both XCK pins and straps DIV2 high.
        // Neither of the three card modes adds another speech-clock divide.
        check_q3_div2_path("Mockingboard mode");
        mode_access(MODE_NATIVE);
        check_q3_div2_path("native mode");
        mode_access(MODE_ECHO);
        check_q3_div2_path("Echo+ mode");
        hard_reset();

        // Each physical socket owns its undefined U65/U64 cold-state seed.
        // Waking one die must not consume or change the other die's seed.
        apple_write(SLOT_BASE + 16'h0021, 8'h50);
        apple_write(SLOT_BASE + 16'h0022, 8'hA8);
        apple_write(SLOT_BASE + 16'h0020, 8'hC0);
        apple_write(SLOT_BASE + 16'h0041, 8'hA0);
        apple_write(SLOT_BASE + 16'h0042, 8'h57);
        apple_write(SLOT_BASE + 16'h0040, 8'hC0);
        check(!dut.ssi263_secondary_i.core_i.transitioned_inflection_seeded_q &&
              !dut.ssi263_primary_i.core_i.transitioned_inflection_seeded_q,
              "hard reset did not arm both independent pitch seeds");
        apple_write(SLOT_BASE + 16'h0023, 8'h5C);
        check(dut.ssi263_secondary_i.core_i.transitioned_inflection_seeded_q &&
              dut.ssi263_secondary_i.core_i.transitioned_inflection_state ==
                  8'h50 &&
              dut.ssi263_secondary_i.core_i.pitch_inflection == 12'hA80 &&
              !dut.ssi263_primary_i.core_i.transitioned_inflection_seeded_q &&
              dut.ssi263_primary_i.core_i.powered_down,
              "A5 wake consumed or changed the A6 pitch seed");
        apple_write(SLOT_BASE + 16'h0043, 8'h5C);
        check(dut.ssi263_primary_i.core_i.transitioned_inflection_seeded_q &&
              dut.ssi263_primary_i.core_i.transitioned_inflection_state ==
                  8'hA0 &&
              dut.ssi263_primary_i.core_i.pitch_inflection == 12'h507 &&
              dut.ssi263_secondary_i.core_i.transitioned_inflection_state ==
                  8'h50 &&
              dut.ssi263_secondary_i.core_i.pitch_inflection == 12'hA80,
              "A6 wake did not keep both pitch seeds independent");
        hard_reset();

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

        // Load row $30 through A5 and compare the live raw ROM controls. The
        // untouched A6 socket must keep its bus-programmed state even though
        // its free-running clocked circuits continue to advance.
        hard_reset();
        start_a5_phone(8'h70);
        wait_for_secondary_scan(6'h30);
        check(dut.ssi263_primary_i.core_i.inflection_high_q == 8'h00 &&
              dut.ssi263_primary_i.core_i.rate_inflection_q == 8'h00 &&
              dut.ssi263_primary_i.core_i.duration_phoneme_q == 8'hC0 &&
              dut.ssi263_primary_i.core_i.voice_amp_code == 4'h0 &&
              dut.ssi263_primary_i.core_i.fric_amp_code == 4'h0,
              "A5 ROM activity changed the A6 socket registers");

        // Prove that the host vector reaches the final pitch path. Detailed
        // U58/U59/U60 timing belongs to the later timing pass.
        apple_write(SLOT_BASE + 16'h0021, 8'hFF);
        apple_write(SLOT_BASE + 16'h0022, 8'hFD);
        apple_write(SLOT_BASE + 16'h0020, 8'h41);
        wait_for_secondary_scan(6'h01);
        check(dut.ssi263_secondary_i.core_i.pitch_inflection == 12'hFFD,
              "period setup did not reach the final pitch path");

        // Restore the fixed vector, then prove the original channel-A route.
        apple_write(SLOT_BASE + 16'h0021, 8'h40);
        apple_write(SLOT_BASE + 16'h0022, 8'hF8);
        apple_write(SLOT_BASE + 16'h0024, 8'hE8);
        apple_write(SLOT_BASE + 16'h0023, 8'h7F);
        // Row $01 has TPARM1 high and therefore schematic PW3 low. This is
        // the ordinary-vowel regression for the live U62/U116 source path.
        apple_write(SLOT_BASE + 16'h0020, 8'h41);
        wait_for_secondary_scan(6'h01);
        wait_for_a5_channel_a_audio();

        // Reset, then prove the original A6 primary/channel-B route.
        hard_reset();
        apple_write(SLOT_BASE + 16'h0041, 8'h40);
        apple_write(SLOT_BASE + 16'h0042, 8'hF8);
        apple_write(SLOT_BASE + 16'h0044, 8'hE8);
        apple_write(SLOT_BASE + 16'h0040, 8'h41);
        apple_write(SLOT_BASE + 16'h0043, 8'h7F);
        wait_for_primary_scan(6'h01);
        wait_for_a6_channel_b_audio();

        // Start both sockets from the same reset edge and one real A5+A6
        // vector. Earlier checks already prove separate A5/A6 write decode;
        // this case proves two live engines and both fixed stereo routes.
        hard_reset();
        apple_write(SLOT_BASE + 16'h0061, 8'h40);
        apple_write(SLOT_BASE + 16'h0062, 8'hF8);
        apple_write(SLOT_BASE + 16'h0064, 8'hE8);
        apple_write(SLOT_BASE + 16'h0060, 8'h41);
        apple_write(SLOT_BASE + 16'h0063, 8'h7F);
        wait_for_secondary_scan(6'h01);
        wait_for_primary_scan(6'h01);
        wait_for_dual_stereo_audio();

        // Card disable masks the virtual backplane boundary. It must not act
        // as a third reset pin on either SSI die.
        mode_access(MODE_NATIVE);
        wait_for_both_pending();
        wait_for_dual_stereo_audio();
        mask_card_during_native_read();

        check(secondary_latch_coverage == 7'h7F &&
              primary_latch_coverage == 7'h7F,
              "both SSI dies did not exercise all U106-U114 latches");
        check(secondary_phase_coverage == 2'b11 &&
              primary_phase_coverage == 2'b11,
              "both SSI dies did not exercise both phase latch banks");
        check(!secondary_latch_error && !primary_latch_error,
              "RESA did not cross both live SSI latch layers exactly");

        if (failures == 0) begin
            $display("PHASOR DUAL SSI263 PASS checks=%0d", checks);
        end else begin
            $display("PHASOR DUAL SSI263 FAIL count=%0d checks=%0d",
                     failures, checks);
            $fatal(1);
        end
        $finish;
    end

endmodule
