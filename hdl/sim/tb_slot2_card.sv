`timescale 1ns / 1ps

// Exercise the public Apple bus and PS registers, including the real mouse.
module tb_slot2_card;
    logic clk = 1'b0;
    always #3.75 clk = ~clk;

    logic resetn = 1'b0;
    logic card_resetn = 1'b0;
    logic enabled = 1'b1;
    logic ps_wr_en = 1'b0;
    logic vblank_start_pulse = 1'b0;
    logic [31:0] ps_rdata;
    logic mouse_selected;
    logic [3:0] dbg_mouse_mode;
    logic dbg_mouse_vbl_pending;
    logic dbg_mouse_irq_pending;
    globals::AppleBus_read ab_read;
    globals::AppleBus_write ab_write;
    globals::AppleBus_write arbiter_write;
    globals::AppleBus_write peer_write;
    globals::AppleBus_read mouse_ab_read;
    globals::AppleBus_write mouse_ab_write;
    globals::SoftSwitchState sss;
    globals::AxiSimple_common as_common;
    AxiSimple_if mouse_as_client();

    slot2_card dut (
        .clk(clk),
        .resetn(resetn),
        .card_resetn(card_resetn),
        .enabled(enabled),
        .ab_read(ab_read),
        .as_common(as_common),
        .ps_wr_en(ps_wr_en),
        .ps_rdata(ps_rdata),
        .mouse_selected(mouse_selected),
        .mouse_ab_read(mouse_ab_read),
        .mouse_ab_write(mouse_ab_write),
        .ab_write(ab_write)
    );

    mouse_card mouse_card_i (
        .clk(clk),
        .rstn(mouse_selected),
        .vblank_start_pulse(vblank_start_pulse),
        .ab_read(mouse_ab_read),
        .sss(sss),
        .slot_assign(3'd2),
        .as_common(as_common),
        .as_client(mouse_as_client),
        .ab_write(mouse_ab_write),
        .dbg_mode(dbg_mouse_mode),
        .dbg_vbl_pending(dbg_mouse_vbl_pending),
        .dbg_irq_pending(dbg_mouse_irq_pending)
    );

    apple_bus_write_arbiter #(.NUM_CLIENTS(2)) output_arbiter (
        .inh_allowed(1'b1),
        .client_writes({peer_write, ab_write}),
        .ab_write(arbiter_write)
    );

    function automatic bit no_bus_controls(input globals::AppleBus_write value);
        return {value.wr_data_en, value.wr_dma_data_en, value.wr_addr_rw_en,
                value.assert_inh, value.assert_res, value.assert_irq,
                value.assert_rdy, value.assert_nmi, value.assert_dma} === 9'd0;
    endfunction

    int errors = 0;
    int checks = 0;

    task automatic check(input bit condition, input string message);
        checks++;
        if (!condition) begin
            errors++;
            $display("FAIL: %s", message);
        end
    endtask

    task automatic settle;
        repeat (3) @(posedge clk);
        #1;
    endtask

    task automatic check_ignored_payload;
        check(arbiter_write === '0, "arbiter accepted inactive nonzero payload");
        peer_write.wr_data = 8'hA5;
        peer_write.wr_data_en = 1'b1;
        #0.1;
        check(arbiter_write === peer_write,
              "inactive slot-2 payload or control disturbed another client");
        peer_write = '0;
        #0.1;
    endtask

    task automatic ps_write(input logic [7:0] address,
                            input logic [31:0] data,
                            input logic [3:0] strobes);
        @(negedge clk);
        as_common.awaddr = address;
        as_common.wdata = data;
        as_common.wstrb = strobes;
        ps_wr_en = 1'b1;
        @(posedge clk);
        #1;
        @(negedge clk);
        ps_wr_en = 1'b0;
        settle();
    endtask

    task automatic ps_expect(input logic [7:0] address,
                             input logic [31:0] expected);
        @(negedge clk);
        as_common.araddr = address;
        #1;
        check(ps_rdata === expected,
              $sformatf("PS %02x expected %08x, got %08x",
                        address, expected, ps_rdata));
    endtask

    task automatic select_card(input logic [1:0] mode,
                               input logic [3:0] present);
        ps_write(8'hAD, {24'd0, present, 2'b00, mode}, 4'h1);
    endtask

    task automatic stage_pads(input logic [11:0] pad1,
                              input logic [11:0] pad2,
                              input logic [11:0] pad3,
                              input logic [11:0] pad4);
        ps_write(8'hAE, {4'd0, pad2, 4'd0, pad1}, 4'hF);
        ps_write(8'hAF, {4'd0, pad4, 4'd0, pad3}, 4'hF);
    endtask

    task automatic bus_address(input logic [15:0] address,
                               input logic read_cycle);
        @(negedge clk);
        ab_read.addr = address;
        ab_read.addr_early = address;
        ab_read.rw = read_cycle;
        ab_read.rw_early = read_cycle;
        ab_read.addr_en = 1'b1;
        @(posedge clk);
        #1;
        @(negedge clk);
        ab_read.addr_en = 1'b0;
        ab_read.serve_en = 1'b1;
        @(posedge clk);
        #1;
    endtask

    task automatic bus_retire;
        @(negedge clk);
        ab_read.serve_en = 1'b0;
        ab_read.data_en = 1'b1;
        @(posedge clk);
        #1;
        @(negedge clk);
        ab_read.data_en = 1'b0;
        ab_read.rw = 1'b1;
        settle();
    endtask

    task automatic bus_expect(input logic [15:0] address,
                              input bit expected_claim,
                              input logic [7:0] expected_data);
        bus_address(address, 1'b1);
        check(ab_write.wr_data_en === expected_claim,
              $sformatf("Apple %04x claim expected %0b, got %0b",
                        address, expected_claim, ab_write.wr_data_en));
        if (expected_claim) begin
            check(ab_write.wr_data === expected_data,
                  $sformatf("Apple %04x expected %02x, got %02x",
                            address, expected_data, ab_write.wr_data));
            // The serving pulse ends before the Apple data window does.
            @(negedge clk);
            ab_read.serve_en = 1'b0;
            @(posedge clk);
            #1;
            check(ab_write.wr_data_en && ab_write.wr_data === expected_data,
                  "card did not hold its read response until data phase");
        end
        check(!ab_write.wr_dma_data_en && !ab_write.wr_addr_rw_en &&
              !ab_write.assert_inh && !ab_write.assert_res &&
              !ab_write.assert_rdy && !ab_write.assert_nmi &&
              !ab_write.assert_dma,
              "slot-2 card drove an unrelated bus control signal");
        bus_retire();
        check(ab_write.wr_data_en === 1'b0,
              "card held its read response after data phase");
    endtask

    task automatic bus_write(input logic [15:0] address,
                             input logic [7:0] data);
        // Data is valid at data_en; a decoder must not treat serve_en as a write.
        bus_address(address, 1'b0);
        check(ab_write.wr_data_en === 1'b0,
              "card drove Apple data during an Apple write");
        @(negedge clk);
        ab_read.data = data;
        ab_read.serve_en = 1'b0;
        bus_retire();
    endtask

    function automatic logic [7:0] fourplay_button(input int button);
        // Canonical SNES order: B,Y,Select,Start,Up,Down,Left,Right,A,X,L,R.
        // FourPlay Rev B has four direction bits and three triggers.
        case (button)
            0: fourplay_button = 8'h80;
            1: fourplay_button = 8'h10;
            4: fourplay_button = 8'h01;
            5: fourplay_button = 8'h02;
            6: fourplay_button = 8'h04;
            7: fourplay_button = 8'h08;
            8: fourplay_button = 8'h40;
            default: fourplay_button = 8'h00;
        endcase
    endfunction

    function automatic logic [7:0] snes_byte(input logic [11:0] pad1,
                                             input logic [11:0] pad2,
                                             input logic [1:0] present,
                                             input int bit_index);
        logic line1;
        logic line2;
        begin
            line1 = 1'b1;
            line2 = 1'b1;
            if (bit_index < 12) begin
                if (present[0]) line1 = !pad1[bit_index];
                if (present[1]) line2 = !pad2[bit_index];
            end else if (bit_index >= 16) begin
                line1 = !present[0];
                line2 = !present[1];
            end
            snes_byte = {line1, line2, 6'b0};
        end
    endfunction

    task automatic pulse_vblank;
        @(negedge clk);
        vblank_start_pulse = 1'b1;
        @(negedge clk);
        vblank_start_pulse = 1'b0;
        settle();
    endtask

    initial begin
        ab_read = '0;
        peer_write = '0;
        ab_read.res = 1'b1;
        ab_read.rw = 1'b1;
        ab_read.cycle_valid = 1'b1;
        sss = '0;
        sss.slot_access = 1'b1;
        as_common = '0;
        mouse_as_client.awvalid = 1'b0;
        settle();
        @(negedge clk);
        resetn = 1'b1;
        card_resetn = 1'b1;
        settle();

        ps_expect(8'hAD, 32'h5332_0001);
        ps_expect(8'hAE, 32'd0);
        ps_expect(8'hAF, 32'd0);
        check(mouse_selected, "reset did not select Mouse");
        bus_expect(16'hC205, 1'b1, 8'h38);

        // Staging keeps only the 12 input bits for each controller.
        ps_write(8'hAE, 32'hFFFF_FFFF, 4'h1);
        ps_expect(8'hAE, 32'h0000_00FF);
        ps_write(8'hAE, 32'hFFFF_FFFF, 4'h2);
        ps_expect(8'hAE, 32'h0000_0FFF);
        ps_write(8'hAE, 32'hFFFF_FFFF, 4'h4);
        ps_expect(8'hAE, 32'h00FF_0FFF);
        ps_write(8'hAE, 32'hFFFF_FFFF, 4'h8);
        ps_expect(8'hAE, 32'h0FFF_0FFF);
        ps_write(8'hAE, 32'h0044_0055, 4'h5);
        ps_expect(8'hAE, 32'h0F44_0F55);
        ps_write(8'hAE, 32'd0, 4'h0);
        ps_expect(8'hAE, 32'h0F44_0F55);
        ps_write(8'hAF, 32'hFFFF_FFFF, 4'hF);
        ps_expect(8'hAF, 32'h0FFF_0FFF);
        ps_write(8'hAF, 32'h0102_0304, 4'hA);
        ps_expect(8'hAF, 32'h01FF_03FF);

        // All 16 FourPlay addresses mirror the four connected players.
        stage_pads(12'd0, 12'd0, 12'd0, 12'd0);
        select_card(2'd2, 4'hF);
        check(!mouse_selected, "FourPlay left Mouse selected");
        for (int offset = 0; offset < 16; offset++)
            bus_expect(16'hC0A0 + offset, 1'b1, 8'h20);
        bus_expect(16'hC200, 1'b0, 8'd0);
        bus_expect(16'hC090, 1'b0, 8'd0);
        bus_expect(16'hC0B0, 1'b0, 8'd0);

        for (int button = 0; button < 12; button++) begin
            stage_pads(12'h001 << button, 12'h001 << button,
                       12'h001 << button, 12'h001 << button);
            select_card(2'd2, 4'hF);
            for (int player = 0; player < 4; player++)
                bus_expect(16'hC0A0 + player, 1'b1,
                           8'h20 | fourplay_button(button));
        end

        stage_pads(12'h001, 12'h100, 12'h002, 12'd0);
        select_card(2'd2, 4'hF);
        for (int mirror = 0; mirror < 4; mirror++) begin
            bus_expect(16'hC0A0 + mirror * 4, 1'b1, 8'hA0);
            bus_expect(16'hC0A1 + mirror * 4, 1'b1, 8'h60);
            bus_expect(16'hC0A2 + mirror * 4, 1'b1, 8'h30);
            bus_expect(16'hC0A3 + mirror * 4, 1'b1, 8'h20);
        end

        // Neither staging nor a control write without byte zero may commit.
        stage_pads(12'hFFF, 12'hFFF, 12'hFFF, 12'hFFF);
        bus_expect(16'hC0A0, 1'b1, 8'hA0);
        bus_expect(16'hC0A2, 1'b1, 8'h30);
        ps_write(8'hAD, 32'hFFFF_FF00, 4'hE);
        ps_expect(8'hAD, 32'h5332_00F2);
        bus_expect(16'hC0A0, 1'b1, 8'hA0);
        bus_expect(16'hC0A2, 1'b1, 8'h30);
        ps_write(8'hAD, 32'hFFFF_FFD6, 4'h1);
        ps_expect(8'hAD, 32'h5332_00D2);
        bus_expect(16'hC0A0, 1'b1, 8'hFF);
        bus_expect(16'hC0A1, 1'b1, 8'h20);
        bus_expect(16'hC0A2, 1'b1, 8'hFF);
        bus_expect(16'hC0A3, 1'b1, 8'hFF);
        check(!ab_write.assert_irq, "FourPlay asserted IRQ");

        // 4Play's fixed ID bit is card hardware, not a pad-presence flag.
        // Every disconnected port stays $20, even with stale held buttons.
        // The author specifies four $20 bytes for an idle card; cover all
        // presence masks and all four mirrors independently of button decode.
        stage_pads(12'hFFF, 12'hFFF, 12'hFFF, 12'hFFF);
        for (int presence = 0; presence < 16; presence++) begin
            select_card(2'd2, presence[3:0]);
            for (int offset = 0; offset < 16; offset++)
                bus_expect(16'hC0A0 + offset, 1'b1,
                           presence[offset & 3] ? 8'hFF : 8'h20);
        end

        // Disable/reset a held nonzero response before its next clock edge.
        // Raw data may remain, but no control or arbiter payload may escape.
        for (int guard = 0; guard < 4; guard++) begin
            stage_pads(12'hFFF, 12'hFFF, 12'hFFF, 12'hFFF);
            select_card(2'd2, 4'hF);
            bus_address(16'hC0A0, 1'b1);
            check(ab_write.wr_data_en && ab_write.wr_data === 8'hFF,
                  "payload boundary did not start with a held response");
            peer_write.wr_data = 8'hA5;
            peer_write.wr_data_en = 1'b1;
            #0.1;
            check(arbiter_write === ab_write,
                  "active slot-2 response lost priority to the peer");
            peer_write = '0;
            @(negedge clk);
            ab_read.serve_en = 1'b0;
            case (guard)
                0: enabled = 1'b0;
                1: card_resetn = 1'b0;
                2: ab_read.res = 1'b0;
                3: resetn = 1'b0;
            endcase
            #1;
            check(ab_write.wr_data === 8'hFF && no_bus_controls(ab_write),
                  "inactive payload was masked or still drove a bus control");
            check_ignored_payload();
            settle();
            @(negedge clk);
            enabled = 1'b1;
            card_resetn = 1'b1;
            ab_read.res = 1'b1;
            resetn = 1'b1;
            settle();
            check(no_bus_controls(ab_write),
                  "reset release revived a held response without a new read");
        end
        stage_pads(12'hFFF, 12'hFFF, 12'hFFF, 12'hFFF);
        select_card(2'd2, 4'hF);
        bus_address(16'hC0A0, 1'b1);
        @(negedge clk);
        ab_read.serve_en = 1'b0;
        as_common.awaddr = 8'hAD;
        as_common.wdata = 32'hF0;
        as_common.wstrb = 4'h1;
        ps_wr_en = 1'b1;
        @(posedge clk);
        #1;
        check(ab_write.wr_data === 8'hFF && no_bus_controls(ab_write),
              "switching Off exposed control from the old nonzero response");
        check_ignored_payload();
        @(negedge clk);
        ps_wr_en = 1'b0;
        settle();

        // SNES reads two serial lines. Reads never advance either line.
        stage_pads(12'hA55, 12'h5AA, 12'hFFF, 12'hFFF);
        select_card(2'd3, 4'h3);
        bus_write(16'hC0A0, 8'hA5);
        for (int bit_index = 0; bit_index <= 16; bit_index++) begin
            if (bit_index == 5) begin
                // Commit new input and presence while a serial read is active.
                stage_pads(12'hFFF, 12'h123, 12'd0, 12'd0);
                select_card(2'd3, 4'h2);
                bus_write(16'hC0B2, 8'hFF);
                bus_write(16'hC0B1, 8'hFF);
                bus_expect(16'hC0A1, 1'b1,
                           snes_byte(12'hA55, 12'h5AA, 2'b11, bit_index));
                bus_expect(16'hC0A4, 1'b1,
                           snes_byte(12'hA55, 12'h5AA, 2'b11, bit_index));
            end
            bus_expect(16'hC0A0, 1'b1,
                       snes_byte(12'hA55, 12'h5AA, 2'b11, bit_index));
            bus_expect(16'hC0A0, 1'b1,
                       snes_byte(12'hA55, 12'h5AA, 2'b11, bit_index));
            bus_write(16'hC0A1, 8'h00);
        end
        for (int extra_clock = 0; extra_clock < 20; extra_clock++) begin
            bus_write(16'hC0A1, 8'hFF);
            bus_expect(16'hC0A0, 1'b1, 8'h00);
        end
        // A latch takes the newly committed state and connection bits.
        bus_write(16'hC0A0, 8'h00);
        for (int bit_index = 0; bit_index <= 16; bit_index++) begin
            bus_expect(16'hC0A0, 1'b1,
                       snes_byte(12'hFFF, 12'h123, 2'b10, bit_index));
            bus_write(16'hC0A1, 8'hFF);
        end
        for (int offset = 0; offset < 16; offset++)
            bus_expect(16'hC0A0 + offset, 1'b1, 8'h80);
        // The hardware decodes A0 only: every even write latches and every
        // odd write clocks. All 16 reads expose the same current serial bit.
        for (int offset = 0; offset < 16; offset += 2) begin
            bus_write(16'hC0A0 + offset, 8'hA5);
            bus_expect(16'hC0AF - offset, 1'b1,
                       snes_byte(12'hFFF, 12'h123, 2'b10, 0));
            bus_write(16'hC0A1 + offset, 8'h5A);
            bus_expect(16'hC0AE - offset, 1'b1,
                       snes_byte(12'hFFF, 12'h123, 2'b10, 1));
        end
        bus_expect(16'hC09F, 1'b0, 8'd0);
        bus_expect(16'hC0B0, 1'b0, 8'd0);
        bus_expect(16'hC200, 1'b0, 8'd0);
        check(!ab_write.assert_irq, "SNES MAX asserted IRQ");

        // Disabled slots, Apple reset, and card reset must stop every driver.
        @(negedge clk);
        enabled = 1'b0;
        settle();
        check(no_bus_controls(ab_write), "disabled slot left a bus control active");
        check(!mouse_selected, "disabled slot exposed Mouse selection");
        bus_expect(16'hC0A0, 1'b0, 8'd0);
        bus_expect(16'hC200, 1'b0, 8'd0);
        @(negedge clk);
        enabled = 1'b1;
        ab_read.res = 1'b0;
        settle();
        bus_expect(16'hC0A0, 1'b0, 8'd0);
        check(!ab_write.assert_irq, "Apple reset left an IRQ asserted");
        @(negedge clk);
        ab_read.res = 1'b1;
        settle();
        bus_expect(16'hC0A0, 1'b1, 8'hC0);
        ps_expect(8'hAD, 32'h5332_0023);
        ps_expect(8'hAE, 32'h0123_0FFF);
        ps_expect(8'hAF, 32'd0);
        @(negedge clk);
        card_resetn = 1'b0;
        settle();
        bus_expect(16'hC0A0, 1'b0, 8'd0);
        ps_expect(8'hAD, 32'h5332_0023);
        @(negedge clk);
        card_resetn = 1'b1;
        settle();
        bus_expect(16'hC0A0, 1'b1, 8'hC0);
        bus_write(16'hC0A0, 8'hFF);
        bus_expect(16'hC0A0, 1'b1, snes_byte(12'hFFF, 12'h123, 2'b10, 0));

        // Leave a real Mouse VBL interrupt pending, then replace the card.
        select_card(2'd1, 4'd0);
        bus_write(16'hC0AE, 8'h08);
        pulse_vblank();
        check(dbg_mouse_mode === 4'h8 && dbg_mouse_vbl_pending &&
              dbg_mouse_irq_pending && ab_write.assert_irq,
              "Mouse VBL setup did not produce a pending interrupt");
        select_card(2'd2, 4'd0);
        check(!mouse_selected && !ab_write.assert_irq &&
              !dbg_mouse_irq_pending && !dbg_mouse_vbl_pending &&
              dbg_mouse_mode === 4'd0,
              "switching to FourPlay did not reset the inactive Mouse");
        bus_expect(16'hC200, 1'b0, 8'd0);
        bus_expect(16'hC0A0, 1'b1, 8'h20);
        pulse_vblank();
        check(!ab_write.assert_irq, "inactive Mouse generated an IRQ");
        select_card(2'd1, 4'd0);
        check(mouse_selected && !ab_write.assert_irq && dbg_mouse_mode === 4'd0,
              "switching back to Mouse restored stale mode or IRQ");
        bus_expect(16'hC205, 1'b1, 8'h38);
        bus_write(16'hC0AE, 8'h08);
        pulse_vblank();
        check(ab_write.assert_irq, "second Mouse VBL setup failed");

        // Disable after serving a Mouse read, before its data retirement.
        // No data_en reaches the disabled card: reset must clear both the
        // held response and pending IRQ before the slot can be re-enabled.
        bus_address(16'hC205, 1'b1);
        check(ab_write.wr_data_en && ab_write.wr_data === 8'h38 &&
              ab_write.assert_irq,
              "Mouse disable boundary did not start with held data and IRQ");
        @(negedge clk);
        ab_read.serve_en = 1'b0;
        enabled = 1'b0;
        #1;
        check(ab_write.wr_data === 8'h38 && no_bus_controls(ab_write),
              "disabled Mouse payload escaped through a bus control");
        check_ignored_payload();
        settle();
        check(no_bus_controls(ab_write) && !mouse_selected,
              "disabled Mouse exposed its held response or IRQ");
        check(!dbg_mouse_irq_pending && !dbg_mouse_vbl_pending &&
              dbg_mouse_mode === 4'd0,
              "logical disable did not reset Mouse interrupt state");
        pulse_vblank();
        check(no_bus_controls(ab_write) && !dbg_mouse_irq_pending &&
              !dbg_mouse_vbl_pending,
              "VBL while Mouse was disabled created a pending interrupt");
        @(negedge clk);
        enabled = 1'b1;
        settle();
        check(mouse_selected && no_bus_controls(ab_write) && !dbg_mouse_irq_pending,
              "re-enabled Mouse restored stale data or IRQ without a new read");
        bus_expect(16'hC205, 1'b1, 8'h38);
        bus_write(16'hC0AE, 8'h08);
        pulse_vblank();
        check(dbg_mouse_irq_pending && ab_write.assert_irq,
              "re-enabled Mouse could not generate a fresh VBL interrupt");

        select_card(2'd0, 4'hF);
        check(no_bus_controls(ab_write) && !mouse_selected,
              "Off mode did not suppress the full pending Mouse output");
        for (int offset = 0; offset < 16; offset++)
            bus_expect(16'hC0A0 + offset, 1'b0, 8'd0);
        bus_expect(16'hC205, 1'b0, 8'd0);

        // A control reset restores defaults, independently of the card reset.
        @(negedge clk);
        resetn = 1'b0;
        settle();
        @(negedge clk);
        resetn = 1'b1;
        settle();
        ps_expect(8'hAD, 32'h5332_0001);
        ps_expect(8'hAE, 32'd0);
        ps_expect(8'hAF, 32'd0);
        check(mouse_selected && !ab_write.assert_irq,
              "control reset failed to restore a clean Mouse selection");

        if (errors != 0)
            $fatal(1, "SLOT2 CARD FAIL: %0d of %0d checks failed", errors, checks);
        $display("SLOT2 CARD PASS: %0d checks", checks);
        $finish;
    end

    initial begin
        #1000000;
        $fatal(1, "SLOT2 CARD FAIL: simulation timed out");
    end
endmodule
