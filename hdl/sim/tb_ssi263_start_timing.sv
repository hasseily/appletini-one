`timescale 1ns / 1ps

module tb_ssi263_start_timing;

    localparam logic [2:0] SSI_DURPHON = 3'd0;
    localparam logic [2:0] SSI_INFLECT = 3'd1;
    localparam logic [2:0] SSI_RATEINF = 3'd2;
    localparam logic [2:0] SSI_CTTRAMP = 3'd3;
    localparam int unsigned IFR_CB1_VOTRAX = 4;

    logic clk = 1'b0;
    logic rstn = 1'b0;
    logic apple_res = 1'b1;
    logic card_enabled = 1'b1;
    logic [2:0] card_mode = 3'd0;
    logic audio_tick = 1'b0;
    logic xck_ce = 1'b0;
    logic ssi_write_strobe = 1'b0;
    logic [2:0] ssi_reg = 3'd0;
    logic [7:0] ssi_wdata = 8'd0;
    logic ssi_d7;
    logic votrax_write_strobe = 1'b0;
    logic [7:0] votrax_wdata = 8'd0;
    logic [7:0] via_pcr = 8'd0;
    logic [6:0] via_ifr_set;
    logic [6:0] via_ifr_clr;
    logic signed [15:0] audio;
    logic direct_irq;
    logic dbg_backend_done;
    logic dbg_enable_ints;

    integer wrapper_start_pulses = 0;
    integer votrax_ifr_clear_pulses = 0;
    integer formant_start_edges = 0;
    integer checks = 0;

    always #5 clk = ~clk;

    ssi263_bus_wrapper #(
        .SSI263_TYPE(2),
        .HAS_SC01(1'b1)
    ) dut (
        .clk(clk),
        .rstn(rstn),
        .apple_res(apple_res),
        .card_enabled(card_enabled),
        .card_mode(card_mode),
        .audio_tick(audio_tick),
        .xck_ce(xck_ce),
        .ssi_write_strobe(ssi_write_strobe),
        .ssi_reg(ssi_reg),
        .ssi_wdata(ssi_wdata),
        .ssi_d7(ssi_d7),
        .votrax_write_strobe(votrax_write_strobe),
        .votrax_wdata(votrax_wdata),
        .via_pcr(via_pcr),
        .via_ifr_set(via_ifr_set),
        .via_ifr_clr(via_ifr_clr),
        .audio(audio),
        .direct_irq(direct_irq),
        .dbg_backend_done(dbg_backend_done),
        .dbg_enable_ints(dbg_enable_ints)
    );

    // Count the registered pulse as the formant backend sees it. Both the
    // wrapper pulse and IFR-clear pulse must last exactly one clock.
    always @(posedge clk) begin
        if (dut.backend_start_q) begin
            wrapper_start_pulses = wrapper_start_pulses + 1;
        end
        if (via_ifr_clr[IFR_CB1_VOTRAX]) begin
            votrax_ifr_clear_pulses = votrax_ifr_clear_pulses + 1;
        end
        if (rstn && card_enabled && !dut.formant_backend_reset &&
            dut.formant_backend_start) begin
            formant_start_edges = formant_start_edges + 1;
        end
    end

    task automatic require(input logic condition, input string message);
        begin
            checks = checks + 1;
            if (condition !== 1'b1) begin
                $display("SSI263 START TIMING FAIL: %s", message);
                $fatal(1);
            end
        end
    endtask

    task automatic drive_idle;
        begin
            ssi_write_strobe = 1'b0;
            votrax_write_strobe = 1'b0;
            audio_tick = 1'b0;
        end
    endtask

    task automatic expect_quiet_cycle(input string label);
        begin
            @(posedge clk);
            #1;
            require(dut.backend_start_q == 1'b0,
                    {label, ": backend start duplicated"});
            require(via_ifr_clr == 7'd0,
                    {label, ": IFR clear duplicated"});
        end
    endtask

    task automatic issue_votrax(
        input logic [5:0] sc01_phone,
        input logic [5:0] expected_ssi_phone,
        input string label
    );
        integer start_before;
        integer clear_before;
        integer consume_before;
        begin
            start_before = wrapper_start_pulses;
            clear_before = votrax_ifr_clear_pulses;
            consume_before = formant_start_edges;

            @(negedge clk);
            votrax_wdata = {2'b00, sc01_phone};
            votrax_write_strobe = 1'b1;
            @(posedge clk);
            #1;
            require(dut.backend_start_q == 1'b1,
                    {label, ": accepted edge missed backend start"});
            require(via_ifr_clr == 7'b0010000,
                    {label, ": accepted edge missed sole CB1 IFR clear"});
            require(dut.backend_phoneme_q == expected_ssi_phone,
                    {label, ": translated SSI phoneme mismatch"});
            require(dut.backend_sc01_phone_q == sc01_phone,
                    {label, ": SC-01 phoneme mismatch"});
            require(dut.backend_votrax_q == 1'b1 &&
                    dut.active_is_votrax_q == 1'b1,
                    {label, ": Votrax tuple tag mismatch"});
            require(dut.duration_phoneme_q == 8'd0,
                    {label, ": Votrax start did not clear duration"});

            @(negedge clk);
            votrax_write_strobe = 1'b0;
            @(posedge clk);
            #1;
            require(dut.backend_start_q == 1'b0,
                    {label, ": backend start wider than one clock"});
            require(via_ifr_clr == 7'd0,
                    {label, ": IFR clear wider than one clock"});
            require(dut.formant_backend_i.active_valid_q == 1'b1 &&
                    dut.formant_backend_i.is_votrax_q == 1'b1,
                    {label, ": formant backend did not start as Votrax"});
            require(dut.formant_backend_i.digital_core_i.rom_phone == sc01_phone,
                    {label, ": formant core consumed the wrong SC-01 phone"});
            require(dut.formant_backend_i.synth_state_q == 5'd0 &&
                    dut.formant_backend_i.mac_tap_q == 3'd0 &&
                    dut.formant_backend_i.mac_accum_q == 56'sd0,
                    {label, ": formant pipeline did not clear on start edge"});
            require(wrapper_start_pulses == start_before + 1 &&
                    votrax_ifr_clear_pulses == clear_before + 1 &&
                    formant_start_edges == consume_before + 1,
                    {label, ": start/clear pulse count mismatch"});

            @(negedge clk);
            expect_quiet_cycle(label);
        end
    endtask

    task automatic issue_ssi(
        input logic [2:0] reg_index,
        input logic [7:0] value,
        input logic [5:0] expected_ssi_phone,
        input logic [5:0] expected_sc01_phone,
        input string label
    );
        integer start_before;
        integer clear_before;
        integer consume_before;
        begin
            start_before = wrapper_start_pulses;
            clear_before = votrax_ifr_clear_pulses;
            consume_before = formant_start_edges;

            @(negedge clk);
            ssi_reg = reg_index;
            ssi_wdata = value;
            ssi_write_strobe = 1'b1;
            @(posedge clk);
            #1;
            require(dut.backend_start_q == 1'b1,
                    {label, ": accepted edge missed backend start"});
            require(via_ifr_clr == 7'd0,
                    {label, ": normal SSI start cleared Votrax IFR"});
            require(dut.backend_phoneme_q == expected_ssi_phone &&
                    dut.backend_sc01_phone_q == expected_sc01_phone &&
                    dut.backend_votrax_q == 1'b0,
                    {label, ": normal SSI tuple mismatch"});

            @(negedge clk);
            ssi_write_strobe = 1'b0;
            @(posedge clk);
            #1;
            require(dut.backend_start_q == 1'b0 && via_ifr_clr == 7'd0,
                    {label, ": normal SSI pulse did not end"});
            require(dut.formant_backend_i.active_valid_q == 1'b1 &&
                    dut.formant_backend_i.is_votrax_q == 1'b0,
                    {label, ": formant backend did not start as SSI263"});
            require(dut.formant_backend_i.digital_core_i.rom_phone ==
                    expected_sc01_phone,
                    {label, ": formant core consumed the wrong SSI phone"});
            require(wrapper_start_pulses == start_before + 1 &&
                    votrax_ifr_clear_pulses == clear_before &&
                    formant_start_edges == consume_before + 1,
                    {label, ": normal SSI pulse count mismatch"});

            @(negedge clk);
            expect_quiet_cycle(label);
        end
    endtask

    task automatic expect_cancelled_votrax(
        input string label,
        input logic use_hard_reset,
        input logic use_apple_reset,
        input logic use_card_disable
    );
        integer start_before;
        integer clear_before;
        integer consume_before;
        begin
            start_before = wrapper_start_pulses;
            clear_before = votrax_ifr_clear_pulses;
            consume_before = formant_start_edges;

            @(negedge clk);
            votrax_wdata = 8'h15;
            votrax_write_strobe = 1'b1;
            if (use_hard_reset) begin
                rstn = 1'b0;
            end
            if (use_apple_reset) begin
                apple_res = 1'b0;
            end
            if (use_card_disable) begin
                card_enabled = 1'b0;
            end
            @(posedge clk);
            #1;
            require(dut.backend_start_q == 1'b0 && via_ifr_clr == 7'd0,
                    {label, ": reset/disable accepted a Votrax write"});
            require(dut.formant_backend_i.active_valid_q == 1'b0,
                    {label, ": formant playback survived reset/disable"});

            @(negedge clk);
            votrax_write_strobe = 1'b0;
            rstn = 1'b1;
            apple_res = 1'b1;
            card_enabled = 1'b1;
            @(posedge clk);
            #1;
            require(dut.backend_start_q == 1'b0 && via_ifr_clr == 7'd0,
                    {label, ": cancelled write appeared one cycle late"});
            require(wrapper_start_pulses == start_before &&
                    votrax_ifr_clear_pulses == clear_before &&
                    formant_start_edges == consume_before,
                    {label, ": cancelled write changed pulse counts"});
        end
    endtask

    task automatic write_stopped_ssi(input logic [2:0] reg_index,
                                     input logic [7:0] value,
                                     input string label);
        integer start_before;
        begin
            start_before = wrapper_start_pulses;
            @(negedge clk);
            ssi_reg = reg_index;
            ssi_wdata = value;
            ssi_write_strobe = 1'b1;
            @(posedge clk);
            #1;
            require(dut.backend_start_q == 1'b0,
                    {label, ": write while stopped started the backend"});
            @(negedge clk);
            ssi_write_strobe = 1'b0;
            @(posedge clk);
            #1;
            require(wrapper_start_pulses == start_before,
                    {label, ": stopped write changed the start count"});
        end
    endtask

    task automatic pulse_xck;
        begin
            @(negedge clk);
            xck_ce = 1'b1;
            @(posedge clk);
            #1;
            @(negedge clk);
            xck_ce = 1'b0;
        end
    endtask

    task automatic test_frame_ack_does_not_restart;
        integer index;
        integer start_before_ack;
        logic [3:0] frame_before_ack;
        begin
            // Apple reset leaves an AP part stopped while retaining its data
            // registers. Set frame mode and the fastest rate before CTL falls.
            write_stopped_ssi(SSI_DURPHON, 8'h71,
                              "frame-mode DURPHON setup");
            write_stopped_ssi(SSI_RATEINF, 8'hF0,
                              "frame-mode RATE setup");
            issue_ssi(SSI_CTTRAMP, 8'h00, 6'h31, 6'h3F,
                      "frame-mode CTL start");

            for (index = 0; index < 8192; index = index + 1) begin
                pulse_xck();
            end
            repeat (3) @(posedge clk);
            #1;
            require(ssi_d7,
                    "first frame boundary did not assert D7/A-R");
            require(dut.formant_backend_i.digital_core_i.ticks != 5'h10,
                    "first frame boundary ended the phone");

            start_before_ack = wrapper_start_pulses;
            frame_before_ack =
                dut.formant_backend_i.digital_core_i.ssi_duration_frame_q;
            @(negedge clk);
            ssi_reg = SSI_INFLECT;
            ssi_wdata = 8'h55;
            ssi_write_strobe = 1'b1;
            @(posedge clk);
            #1;
            require(!ssi_d7,
                    "frame ACK did not clear D7/A-R");
            require(dut.backend_start_q == 1'b0,
                    "frame ACK restarted the backend");
            @(negedge clk);
            ssi_write_strobe = 1'b0;
            @(posedge clk);
            #1;
            require(wrapper_start_pulses == start_before_ack,
                    "frame ACK emitted a hidden start pulse");
            require(dut.formant_backend_i.digital_core_i.ssi_duration_active_q &&
                    dut.formant_backend_i.digital_core_i.ssi_duration_frame_q ==
                        frame_before_ack,
                    "frame ACK reset the active phone timer");
        end
    endtask

    task automatic test_dr00_disables_response_not_mode;
        integer index;
        begin
            write_stopped_ssi(SSI_CTTRAMP, 8'h80,
                              "DR00 CTL stop");
            write_stopped_ssi(SSI_DURPHON, 8'h31,
                              "DR00 DURPHON setup");
            issue_ssi(SSI_CTTRAMP, 8'h00, 6'h31, 6'h3F,
                      "DR00 CTL start");
            require(dut.current_function_q == 2'd1 && !dbg_enable_ints,
                    "DR00 changed the retained frame mode or enabled A-R");

            for (index = 0; index < 8192; index = index + 1) begin
                pulse_xck();
            end
            repeat (3) @(posedge clk);
            #1;
            require(ssi_d7 && !direct_irq,
                    "DR00 did not keep D7 status while masking external A-R");
            require(dut.formant_backend_i.digital_core_i.ticks != 5'h10,
                    "DR00 changed the retained frame-mode phone timing");
        end
    endtask

    task automatic test_phone_ack_does_not_restart;
        integer index;
        integer start_before_ack;
        begin
            write_stopped_ssi(SSI_CTTRAMP, 8'h80,
                              "phone-mode CTL stop");
            write_stopped_ssi(SSI_DURPHON, 8'hF1,
                              "phone-mode DURPHON setup");
            write_stopped_ssi(SSI_RATEINF, 8'hF0,
                              "phone-mode RATE setup");
            issue_ssi(SSI_CTTRAMP, 8'h00, 6'h31, 6'h3F,
                      "phone-mode CTL start");

            for (index = 0; index < 8192; index = index + 1) begin
                pulse_xck();
            end
            repeat (3) @(posedge clk);
            #1;
            require(ssi_d7,
                    "phone boundary did not set D7");

            start_before_ack = wrapper_start_pulses;
            @(negedge clk);
            ssi_reg = SSI_INFLECT;
            ssi_wdata = 8'h66;
            ssi_write_strobe = 1'b1;
            @(posedge clk);
            #1;
            require(!ssi_d7 && dut.backend_start_q == 1'b0,
                    "phone ACK did not clear D7 without a restart");
            @(negedge clk);
            ssi_write_strobe = 1'b0;
            @(posedge clk);
            #1;
            require(wrapper_start_pulses == start_before_ack,
                    "phone ACK emitted a hidden start pulse");

            for (index = 0; index < 8192; index = index + 1) begin
                pulse_xck();
            end
            repeat (3) @(posedge clk);
            #1;
            require(ssi_d7,
                    "repeating phone did not produce its next D7 response");
        end
    endtask

    task automatic test_coincident_rate_write_uses_live_value;
        integer raw_pulses;
        begin
            write_stopped_ssi(SSI_CTTRAMP, 8'h80,
                              "live-RATE CTL stop");
            write_stopped_ssi(SSI_DURPHON, 8'h71,
                              "live-RATE DURPHON setup");
            write_stopped_ssi(SSI_RATEINF, 8'hF0,
                              "live-RATE initial setup");
            issue_ssi(SSI_CTTRAMP, 8'h00, 6'h31, 6'h3F,
                      "live-RATE CTL start");

            require(
                dut.formant_backend_i.digital_core_i.
                    ssi_response_subticks_left_q == 12'd255,
                "initial R=15 response reload is wrong");

            // Reach zero without crossing the slot boundary. The effective
            // edge which decrements one to zero leaves DIV2 phase low.
            raw_pulses = 0;
            while (dut.formant_backend_i.digital_core_i.
                       ssi_response_subticks_left_q != 12'd0 &&
                   raw_pulses < 512) begin
                pulse_xck();
                raw_pulses = raw_pulses + 1;
            end
            require(
                dut.formant_backend_i.digital_core_i.
                    ssi_response_subticks_left_q == 12'd0,
                "response counter did not reach the live-RATE boundary");

            if (!dut.formant_backend_i.digital_core_i.ssi_div2_phase_q) begin
                pulse_xck();
            end
            require(
                dut.formant_backend_i.digital_core_i.ssi_div2_phase_q &&
                dut.formant_backend_i.digital_core_i.
                    ssi_response_subticks_left_q == 12'd0,
                "could not align the live-RATE effective edge");

            // Accept R=5 on the same effective XCK edge that reloads the
            // response slot. The core must see the write data, not stale R=15.
            @(negedge clk);
            ssi_reg = SSI_RATEINF;
            ssi_wdata = 8'h50;
            ssi_write_strobe = 1'b1;
            xck_ce = 1'b1;
            @(posedge clk);
            #1;
            require(dut.rate_inflection_q == 8'h50,
                    "coincident RATE write was not accepted");
            require(
                dut.formant_backend_i.digital_core_i.
                    ssi_response_subticks_left_q == 12'd2815 &&
                dut.formant_backend_i.digital_core_i.ssi_response_slot_q ==
                    4'd1,
                "slot boundary did not reload from the coincident live RATE");

            @(negedge clk);
            ssi_write_strobe = 1'b0;
            xck_ce = 1'b0;
            @(posedge clk);
            #1;
        end
    endtask

    task automatic inject_response_then_start(input logic target_votrax,
                                              input string label);
        begin
            // Raise the core response after one fabric edge. The backend sees
            // it on the next edge, when the wrapper accepts a new start; its
            // registered response reaches the wrapper one clock later.
            @(posedge clk);
            #1;
            force dut.formant_backend_i.core_response_done = 1'b1;
            @(negedge clk);
            if (target_votrax) begin
                votrax_wdata = 8'h15;
                votrax_write_strobe = 1'b1;
            end else begin
                ssi_reg = SSI_DURPHON;
                ssi_wdata = 8'hF1;
                ssi_write_strobe = 1'b1;
            end
            @(posedge clk);
            #1;
            release dut.formant_backend_i.core_response_done;
            require(dut.backend_start_q,
                    {label, ": target start was not accepted"});

            @(negedge clk);
            ssi_write_strobe = 1'b0;
            votrax_write_strobe = 1'b0;
            @(posedge clk);
            #1;
            require(!ssi_d7 && !direct_irq && via_ifr_set == 7'd0,
                    {label, ": stale response crossed the new start"});
            require(dut.active_is_votrax_q == target_votrax,
                    {label, ": target mode tag did not win"});
        end
    endtask

    task automatic test_response_start_generation_guard;
        begin
            via_pcr = 8'hB0;
            // Start from SSI, collide its delayed response with a Votrax
            // start, then use that Votrax state for the reverse collision.
            issue_ssi(SSI_DURPHON, 8'hF1, 6'h31, 6'h3F,
                      "pre-collision SSI start");
            inject_response_then_start(1'b1,
                                       "SSI-to-Votrax response collision");
            inject_response_then_start(1'b0,
                                       "Votrax-to-SSI response collision");
            via_pcr = 8'h00;
        end
    endtask

    initial begin
        repeat (3) @(posedge clk);
        @(negedge clk);
        rstn = 1'b1;
        @(posedge clk);
        #1;
        require(dut.backend_start_q == 1'b0 && via_ifr_clr == 7'd0,
                "reset release generated a pulse");

        issue_votrax(6'h15, 6'h0F, "first Votrax write");

        // Put the formant state machine in flight, then prove the next Votrax
        // start clears it on the same edge that consumes backend_start_q.
        @(negedge clk);
        audio_tick = 1'b1;
        @(posedge clk);
        #1;
        require(dut.formant_backend_i.synth_state_q == 5'd1,
                "audio tick did not dirty the formant pipeline");
        @(negedge clk);
        audio_tick = 1'b0;
        @(posedge clk);
        #1;
        require(dut.formant_backend_i.synth_state_q != 5'd0,
                "formant pipeline was not in flight before restart");

        issue_votrax(6'h3C, 6'h01, "Votrax pipeline restart");

        // Clear CTTRAMP bit 7 to enable normal SSI playback. This edge starts
        // the latched phoneme, then a DURPHON write starts the requested one.
        issue_ssi(SSI_CTTRAMP, 8'h00, 6'h00, 6'h03,
                  "SSI263 enable start");
        issue_ssi(SSI_DURPHON, 8'h24, 6'h24, 6'h0E,
                  "SSI263 DURPHON start");

        expect_cancelled_votrax("hard reset cancellation", 1'b1, 1'b0, 1'b0);

        issue_votrax(6'h15, 6'h0F, "pre-disable Votrax write");
        expect_cancelled_votrax("card-disable cancellation", 1'b0, 1'b0, 1'b1);

        issue_votrax(6'h15, 6'h0F, "pre-Apple-reset Votrax write");
        expect_cancelled_votrax("Apple reset cancellation", 1'b0, 1'b1, 1'b0);

        test_frame_ack_does_not_restart();
        test_dr00_disables_response_not_mode();
        test_phone_ack_does_not_restart();
        test_coincident_rate_write_uses_live_value();
        test_response_start_generation_guard();

        drive_idle();
        $display("SSI263 START TIMING PASS checks=%0d starts=%0d clears=%0d consumed=%0d",
                 checks, wrapper_start_pulses, votrax_ifr_clear_pulses,
                 formant_start_edges);
        $finish;
    end

endmodule
