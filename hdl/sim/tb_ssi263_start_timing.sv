`timescale 1ns / 1ps

module tb_ssi263_start_timing;

    localparam logic [2:0] SSI_DURPHON = 3'd0;
    localparam logic [2:0] SSI_CTTRAMP = 3'd3;
    localparam int unsigned IFR_CB1_VOTRAX = 4;

    logic clk = 1'b0;
    logic rstn = 1'b0;
    logic apple_res = 1'b1;
    logic card_enabled = 1'b1;
    logic [2:0] card_mode = 3'd0;
    logic audio_tick = 1'b0;
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

        drive_idle();
        $display("SSI263 START TIMING PASS checks=%0d starts=%0d clears=%0d consumed=%0d",
                 checks, wrapper_start_pulses, votrax_ifr_clear_pulses,
                 formant_start_edges);
        $finish;
    end

endmodule
