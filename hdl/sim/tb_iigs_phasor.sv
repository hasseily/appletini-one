`timescale 1ns / 1ps
module tb_iigs_phasor;
    import globals::*;
    logic clk = 0, resetn = 0;
    always #5 clk = !clk;
    AppleBus_read host = '0, card_bus, slot2_bus, slot4_bus;
    AppleBus_write slot2_reply, slot4_reply, card_reply, host_reply;
    logic location2 = 0, card_resetn = 1;
    assign card_bus = location2 ? slot2_bus : slot4_bus;
    assign host_reply = location2 ? slot2_reply : slot4_reply;
    SoftSwitchState sss;
    logic [15:0] sample_addr = 0;
    logic sample_early_en = 0, sample_late_en = 0;
    logic iigs_data_tick = 0;
    logic enable_elapsed_psg_clock = 0;
    logic [7:0] io_mask, rom_mask, early_slot, late_slot, data_slot, reply_slot;
    int psg_ticks = 0;
    int timer_ticks = 0;
    logic [7:0] observed_reply;

    soft_switch_manager switches (
        .clk, .rstn(resetn), .ramworks_en(1'b0), .iigs_profile(1'b1),
        .iigs_bootstrap_slot7(1'b0), .iigs_boot_mask(8'h00),
        .iigs_boot_mask_valid(1'b0), .iigs_native_video(1'b0),
        .iigs_bank_latch(1'b0), .ab_read(host), .sss,
        .iigs_live_mask(), .iigs_live_mask_valid(),
        .iigs_io_mask(io_mask), .iigs_rom_mask(rom_mask)
    );
    apple_iigs_slot_access ownership (
        .clk, .resetn, .ab_read(host), .sample_addr, .sample_early_en,
        .sample_late_en, .io_mask, .rom_mask, .c8_owner(sss.c8_select),
        .early_slot, .late_slot, .data_slot, .reply_slot
    );
    iigs_top_routing routing (
        .iigs_ownership_active(1'b1), .machine_identity_fault(1'b0),
        .onee_enable_effective(1'b0), .enabled(1'b1), .irq_enabled(rom_mask[4]),
        .iigs_early_slot(early_slot), .iigs_late_slot(late_slot),
        .iigs_data_slot(data_slot), .iigs_reply_slot(reply_slot), .slots(8'h10),
        .host_bus(host), .device_reply(location2 ? '0 : card_reply),
        .device_bus(slot4_bus), .host_reply(slot4_reply)
    );
    iigs_top_routing relocated_routing (
        .iigs_ownership_active(1'b1), .machine_identity_fault(1'b0),
        .onee_enable_effective(1'b0), .enabled(1'b1),
        .irq_enabled(location2 && rom_mask[2]),
        .iigs_early_slot(early_slot), .iigs_late_slot(late_slot),
        .iigs_data_slot(data_slot), .iigs_reply_slot(reply_slot), .slots(8'h04),
        .host_bus(host), .device_reply(location2 ? card_reply : '0),
        .device_bus(slot2_bus), .host_reply(slot2_reply)
    );
    mockingboard card (
        .clk, .rstn(resetn && card_resetn), .apple_q3_raw(1'b0), .standalone(1'b0), .iigs_data_tick,
        .ab_read(card_bus), .serve_data(host.data), .sss, .slot_assign(location2 ? 3'd2 : 3'd4),
        .pan(48'd0), .ssi_pan(8'd0), .audio_control(32'd0),
        .audio_sample_tick(1'b0), .ab_write(card_reply),
        .audio_l(), .audio_r(), .dbg_ssi_irq(), .dbg_ssi_backend_done(),
        .dbg_ssi_enable_ints()
    );

    always @(posedge clk) begin
        if (resetn && card.psg_clock) psg_ticks <= psg_ticks + 1;
        if (resetn && card.via_timer_clock) timer_ticks <= timer_ticks + 1;
    end

    task automatic access(input logic [15:0] address, input logic rw,
                          input logic [7:0] value, input logic selected,
                          input logic expect_reply, input logic check_value = 1);
        @(negedge clk);
        sample_addr = address;
        sample_early_en = 1;
        @(negedge clk);
        sample_early_en = 0;
        host.addr_en = 1;
        host.addr_early = address;
        host.rw_early = rw;
        host.m2sel = !selected;
        host.cycle_valid = selected;
        @(negedge clk);
        host.addr_en = 0;
        host.phi0_tick = 1;
        host.sss_en = selected;
        @(negedge clk);
        host.phi0_tick = 0;
        host.sss_en = 0;
        sample_late_en = 1;
        @(negedge clk);
        sample_late_en = 0;
        host.addr = address;
        host.rw = rw;
        host.serve_en = selected;
        @(negedge clk);
        host.serve_en = 0;
        #1;
        observed_reply = host_reply.wr_data;
        if (host_reply.wr_data_en !== expect_reply ||
            (expect_reply && check_value && host_reply.wr_data !== value))
            $fatal(1, "IIgs Phasor response at %04x: enabled=%b data=%02x expected enabled=%b data=%02x",
                   address, host_reply.wr_data_en, host_reply.wr_data, expect_reply, value);
        @(negedge clk);
        host.data = value;
        host.data_en = selected;
        iigs_data_tick = enable_elapsed_psg_clock;
        @(negedge clk);
        host.data_en = 0;
        iigs_data_tick = 0;
        repeat (3) @(negedge clk);
    endtask

    task automatic pure_noise_probe(input logic [15:0] address,
                                    input logic mapped);
        logic [7:0] previous_value;
        logic [7:0] difference;
        // Pure Noise 1.1 DETECTMB does two pairs with no VIA setup.
        // STA Temp (3 cycles), then LDA (OUT2),Y (5 cycles, no crossing).
        for (int pair = 0; pair < 2; pair++) begin
            access(address, 1, 0, 1, mapped, 0);
            previous_value = observed_reply;
            access(16'h0800, 1, 0, 1, 0); // STA opcode
            access(16'h0801, 1, 0, 1, 0); // STA operand
            access(16'h0004, 0, previous_value, 1, 0);
            access(16'h0802, 1, 0, 1, 0); // LDA opcode
            access(16'h0803, 1, 0, 1, 0); // LDA operand
            access(16'h0006, 1, 0, 1, 0); // OUT2 low byte
            access(16'h0007, 1, 0, 1, 0); // OUT2 high byte
            access(address, 1, 0, 1, mapped, 0);
            difference = observed_reply - previous_value;
            if (mapped && difference !== 8'hF8)
                $fatal(1, "Pure Noise T1 probe %0d got delta %02x instead of F8",
                       pair + 1, difference);
            // SEC, SBC zp, CMP immediate, two branches and DEX separate
            // the pairs. The detector imposes no delta between pairs.
            repeat (13) access(16'h0804, 1, 0, 1, 0);
        end
    endtask

    task automatic check_clocks(input int expected_psg);
        int start_psg, start_timer;
        start_psg = psg_ticks;
        start_timer = timer_ticks;
        for (int i = 0; i < 8; i++) begin
            access(16'h2000, 1, 0, 1, 0);
            access(16'hC412, 1, 0, 0, 0);
        end
        if (timer_ticks - start_timer != 16)
            $fatal(1, "VIA elapsed clock lost PHI0 ticks");
        if (psg_ticks - start_psg != expected_psg)
            $fatal(1, "AY clock stalled during unrelated/fast cycles: got %0d, expected %0d",
                   psg_ticks - start_psg, expected_psg);
    endtask

    task automatic check_timer(input logic [15:0] addr, input logic native_mode);
        access(addr, 0, 8'h40, 1, 0);
        access(addr + 16'd1, 0, 8'h12, 1, 0);
        for (int i = 0; i < 4; i++)
            access(16'h2000, 1, 0, 0, 0);
        access(addr, 1, 8'h3C, 1, 1);
        // Phasor mode advances the counter one extra tick after a T1 low
        // read. The first read's pre-tick value still comes from PHI0.
        access(addr, 1, native_mode ? 8'h3A : 8'h3B, 1, 1);
    endtask

    task automatic psg_write(input logic [7:0] reg_num, input logic [7:0] value);
        access(16'hC411, 0, reg_num, 1, 0);
        access(16'hC410, 0, 8'h0F, 1, 0);
        access(16'hC410, 0, 8'h0C, 1, 0);
        access(16'hC411, 0, value, 1, 0);
        access(16'hC410, 0, 8'h0E, 1, 0);
        access(16'hC410, 0, 8'h0C, 1, 0);
    endtask

    task automatic check_tone;
        bit saw_high, saw_low;
        saw_high = 0;
        saw_low = 0;
        access(16'hC412, 0, 8'hFF, 1, 0);
        access(16'hC413, 0, 8'hFF, 1, 0);
        access(16'hC410, 0, 8'h0C, 1, 0);
        psg_write(8'd7, 8'h3E);
        psg_write(8'd8, 8'h0F);
        psg_write(8'd0, 8'h01);
        for (int i = 0; i < 32; i++) begin
            access(16'h2000, 1, 0, 0, 0);
            if (card.psg0_chan_a === 8'h00) saw_low = 1;
            if (card.psg0_chan_a === 8'hFF) saw_high = 1;
        end
        if (!saw_high || !saw_low)
            $fatal(1, "Programmed AY tone did not oscillate during fast-memory cycles");
    endtask

    task automatic check_remap_irq(input logic [15:0] base,
                                   input logic [7:0] chosen,
                                   input logic [7:0] other);
        access(16'hC02D, 0, chosen | other, 1, 0);
        access(base + 2, 0, 8'hA5, 1, 0);
        access(base + 4, 0, 8'h01, 1, 0);
        access(base + 5, 0, 8'h00, 1, 0);
        access(base + 14, 0, 8'hC0, 1, 0);
        repeat (5) access(16'h2000, 1, 0, 0, 0);
        if (!card_reply.assert_irq || slot2_reply.assert_irq !== location2 ||
            slot4_reply.assert_irq !== !location2)
            $fatal(1, "Selected IIgs Phasor IRQ did not reach its own slot");
        access(16'hC02D, 0, other, 1, 0);
        access(base + 2, 1, 0, 1, 0);
        if (!card_reply.assert_irq || slot2_reply.assert_irq || slot4_reply.assert_irq)
            $fatal(1, "Other slot's ownership leaked the masked Phasor IRQ");
        access(16'hC02D, 0, chosen, 1, 0);
        access(base + 2, 1, 8'hA5, 1, 1);
        if (!card_reply.assert_irq || slot2_reply.assert_irq !== location2 ||
            slot4_reply.assert_irq !== !location2)
            $fatal(1, "Remap lost pending Phasor IRQ or restored it in the wrong slot");
    endtask

    initial begin
        int start_psg;
        repeat (3) @(negedge clk);
        resetn = 1;
        host.res = 1;
        host.devsel_n = 1;
        host.devsel_n_early = 1;
        access(16'hC02D, 0, 8'h10, 1, 0);
        pure_noise_probe(16'hC404, 1);
        $display("IIGS PURE NOISE T1 DETECTION PASS");
        enable_elapsed_psg_clock = 1;
        access(16'hC402, 0, 8'hA5, 1, 0);
        access(16'hC402, 1, 8'hA5, 1, 1);
        check_timer(16'hC404, 0);
        access(16'hC0C5, 0, 8'h00, 1, 0);
        pure_noise_probe(16'hC404, 0);
        access(16'hC412, 0, 8'h5A, 1, 0);
        access(16'hC412, 1, 8'h5A, 1, 1);
        access(16'hC402, 1, 8'h00, 1, 0);
        access(16'hC02D, 0, 8'h00, 1, 0);
        access(16'hC412, 1, 8'h00, 1, 0);
        access(16'hC02D, 0, 8'h10, 1, 0);
        start_psg = psg_ticks;
        access(16'hC412, 1, 8'h5A, 1, 1);
        if (psg_ticks - start_psg != 2)
            $fatal(1, "Owned native access must keep exactly two AY clocks");
        check_timer(16'hC414, 1);
        $display("IIGS PHASOR VIA ACCESS PASS");
        check_clocks(32);
        check_tone();
        access(16'hC0C8, 0, 8'h00, 1, 0);
        check_clocks(16);
        $display("IIGS PHASOR CLOCK PASS");
        check_remap_irq(16'hC400, 8'h10, 8'h04);
        card_resetn = 0;
        location2 = 1;
        repeat (3) @(negedge clk);
        card_resetn = 1;
        check_remap_irq(16'hC200, 8'h04, 8'h10);
        $display("IIGS PHASOR RELOCATED IRQ PASS");
        $finish;
    end
endmodule
