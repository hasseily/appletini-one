`timescale 1ns / 1ps

module tb_iigs_live_slots;
    import globals::*;
    logic clk = 1'b0;
    always #5 clk = ~clk;
    logic resetn = 1'b0;
    AppleBus_read host, boot_bus, slot5_bus;
    AppleBus_write boot_reply, boot_host_reply, slot5_reply, slot5_host_reply;
    SoftSwitchState sss;
    AxiSimple_common as_common;
    AxiSimple_if as_if();
    logic [7:0] live_mask, io_mask, rom_mask, early_slot, late_slot;
    logic [7:0] data_slot, reply_slot;
    logic live_valid, report_valid, id_fault, map_fault;
    logic [7:0] report_mask, allowed_slots;
    logic [3:0] id;
    logic gs_qualified, inh_allowed;
    logic fault;
    // Wrapper pre-sample address and load strobes (see start_cycle).
    logic [15:0] sample_addr = 16'h0000;
    logic sample_early_en = 1'b0, sample_late_en = 1'b0;

    soft_switch_manager switches (
        .clk, .rstn(resetn), .ramworks_en(1'b0),
        .iigs_profile(gs_qualified), .iigs_bootstrap_slot7(1'b0),
        .iigs_boot_mask(report_mask), .iigs_boot_mask_valid(report_valid),
        .ab_read(host), .sss, .iigs_live_mask(live_mask),
        .iigs_live_mask_valid(live_valid), .iigs_io_mask(io_mask),
        .iigs_rom_mask(rom_mask)
    );
    apple_iigs_slot_access access_decode (
        .clk, .resetn, .ab_read(host), .sample_addr, .sample_early_en,
        .sample_late_en, .io_mask, .rom_mask,
        .c8_owner(sss.c8_select), .early_slot, .late_slot, .data_slot, .reply_slot
    );
    apple_machine_safety_policy policy (
        .locked_machine_id(id), .machine_id_fault(id_fault),
        .iigs_slot_policy_fault(map_fault), .iigs_slot3_profile(1'b1),
        .iigs_external_slot_mask(live_mask),
        .iigs_external_slot_mask_valid(live_valid), .requested_machine_mode(2'd2),
        .requested_m2sel_active_high(1'b1),
        .machine_identity_reported(), .machine_identity_legacy(),
        .machine_identity_iigs(), .machine_identity_fault(fault),
        .physical_slot_allowed_mask(allowed_slots),
        .machine_inh_allowed(inh_allowed), .machine_gs_m2_qualify(gs_qualified),
        .machine_is_iiplus(), .effective_machine_mode(),
        .effective_m2sel_active_high()
    );
    iigs_top_routing boot_routing (
        .iigs_ownership_active(gs_qualified), .machine_identity_fault(fault),
        .onee_enable_effective(1'b0), .enabled(1'b1), .irq_enabled(1'b0),
        .iigs_early_slot(early_slot), .iigs_late_slot(late_slot),
        .iigs_data_slot(data_slot), .iigs_reply_slot(reply_slot),
        .slots(8'h80), .host_bus(host),
        .device_reply(boot_reply), .device_bus(boot_bus),
        .host_reply(boot_host_reply)
    );
    iigs_top_routing slot5_routing (
        .iigs_ownership_active(gs_qualified), .machine_identity_fault(fault),
        .onee_enable_effective(1'b0), .enabled(id == 4'd4),
        .irq_enabled(id == 4'd4 && rom_mask[5]),
        .iigs_early_slot(early_slot), .iigs_late_slot(late_slot),
        .iigs_data_slot(data_slot), .iigs_reply_slot(reply_slot),
        .slots(8'h20), .host_bus(host),
        .device_reply(slot5_reply), .device_bus(slot5_bus),
        .host_reply(slot5_host_reply)
    );
    boot_menu_card boot (
        .clk, .rstn(resetn), .ab_read(boot_bus), .sss,
        .disk2_enabled(1'b0), .apple_video_mode_valid(1'b1),
        .apple_video_mode_50hz(1'b0), .as_common, .as_client(as_if),
        .ab_write(boot_reply), .smartport_active(), .disk2_active(),
        .boot_target_disk2(), .configured_boot_target_disk2(),
        .boot_slot(), .boot_slot_valid(), .apple_vblank_start_pulse(),
        .machine_id(id), .machine_id_fault(id_fault),
        .iigs_external_slot_mask(report_mask),
        .iigs_external_slot_mask_valid(report_valid), .iigs_policy_fault(map_fault),
        .aux_probe_pulse(), .aux_status(), .aux_status_clear(1'b0)
    );

    task automatic check(input logic condition, input string message);
        if (condition !== 1'b1) $fatal(1, "%s", message);
    endtask

    // The wrapper presents its pre-sample address one clock before it loads
    // addr_early and again one clock before it loads addr. The ownership
    // decode registers on those edges, so model one pre-sample clock.
    task automatic start_cycle(input logic [15:0] address, input logic rw,
                               input logic m2sel);
        @(negedge clk);
        sample_addr = address;
        sample_early_en = 1;
        @(negedge clk);
        sample_early_en = 0;
        sample_late_en = 1;
        host.addr_early = address;
        host.rw_early = rw;
        host.m2sel = m2sel;
        host.cycle_valid = !m2sel;
        host.serve_en = 0;
        host.data_en = 0;
        host.addr_en = 1;
        #1;
        check(data_slot == 0 && reply_slot == 0,
              "previous ownership leaked into a new cycle");
        @(negedge clk);
        sample_late_en = 0;
        host.addr_en = 0;
        host.addr = address;
        host.rw = rw;
        host.serve_en = !m2sel;
    endtask

    task automatic read_cycle(input logic [15:0] address, input logic m2sel,
                              input logic [7:0] owner, input logic drive);
        start_cycle(address, 1'b1, m2sel);
        @(posedge clk);
        #1;
        check(data_slot == owner && reply_slot == owner,
              $sformatf("read owner at %h", address));
        check(boot_host_reply.wr_data_en == drive,
              $sformatf("boot reply at %h", address));
        check(slot5_host_reply.wr_data_en == owner[5] &&
              slot5_host_reply.assert_irq == (id == 4'd4 && rom_mask[5]),
              "buffered slot reply and IRQ use independent permission");
        @(negedge clk);
        host.serve_en = 0;
        host.data_en = !m2sel;
        @(negedge clk);
        host.data_en = 0;
        #1;
        check(data_slot == 0 && reply_slot == 0 && !boot_host_reply.wr_data_en &&
              !slot5_host_reply.wr_data_en, "data phase must retire buffered replies");
    endtask

    task automatic write_cycle(input logic [15:0] address,
                               input logic [7:0] data);
        start_cycle(address, 1'b0, 1'b0);
        @(negedge clk);
        host.serve_en = 0;
        check(data_slot == late_slot && reply_slot == 0,
              $sformatf("write owner at %h", address));
        host.data_en = 1;
        host.data = data;
        @(negedge clk);
        host.data_en = 0;
        // Registered masks and the C8 owner follow a write one or two
        // clocks later. The bus gives about 70 clocks before the next cycle.
        repeat (2) @(negedge clk);
    endtask

    task automatic check_slot5_write(input logic owned);
        start_cycle(16'hC0D0, 1'b0, 1'b0);
        #1;
        check(slot5_bus.serve_en == owned, "slot 5 write serve phase");
        @(negedge clk);
        host.serve_en = 0;
        check(data_slot == (owned ? 8'h20 : 8'h00) &&
              reply_slot == 0 && !slot5_host_reply.wr_data_en,
              "a write needs an access tag and no reply");
        host.data_en = 1;
        #1;
        check(slot5_bus.data_en == owned, "slot 5 write data phase");
        @(negedge clk);
        host.data_en = 0;
        #1;
        check(data_slot == 0 && reply_slot == 0, "write must retire its tag");
    endtask

    initial begin
        host = '0;
        host.devsel_n = 1'b1;
        host.devsel_n_early = 1'b1;
        host.phi0_tick = 1'b1;
        slot5_reply = '0;
        slot5_reply.wr_data_en = 1'b1;
        slot5_reply.assert_irq = 1'b1;
        as_common = '0;
        as_if.awvalid = 1'b0;
        repeat (3) @(negedge clk);
        resetn = 1'b1;
        host.res = 1'b1;
        read_cycle(16'hC705, 1'b0, 8'h00, 1'b0);
        write_cycle(16'hC02D, 8'hBB);
        check(live_valid && live_mask == 8'hB2 && io_mask == 8'hBA &&
              rom_mask == 8'hB2 && allowed_slots == 0 && !inh_allowed,
              "observed map qualifies bootstrap before optional cards");
        read_cycle(16'hC705, 1'b1, 8'h00, 1'b0);
        read_cycle(16'hC705, 1'b0, 8'h80, 1'b1);
        read_cycle(16'hC700, 1'b0, 8'h80, 1'b1);
        read_cycle(16'hCFFF, 1'b0, 8'h00, 1'b0);
        read_cycle(16'hC800, 1'b0, 8'h00, 1'b0);
        read_cycle(16'hC701, 1'b0, 8'h80, 1'b1);
        write_cycle(16'hCA00, 8'h70);
        read_cycle(16'hCA00, 1'b0, 8'h80, 1'b1);
        write_cycle(16'hC0F0, 8'h01);
        read_cycle(16'hC800, 1'b0, 8'h80, 1'b1);
        write_cycle(16'hC0F0, 8'h24);
        write_cycle(16'hC0F0, 8'h27);
        write_cycle(16'hC0F0, 8'h80);
        check(id == 4 && report_valid && !map_fault && !id_fault &&
              allowed_slots == 8'hB2 && !inh_allowed,
              "virtual C0F accepts the GS report with physical DEVSEL high");
        check(live_mask == 8'hB2, "boot report cannot replace an observed map");
        write_cycle(16'hC02D, 8'h22);
        check(allowed_slots == 8'h22 && sss.c8_select == 0,
              "live remap revokes slot 7 and its C8 claim");
        check_slot5_write(1'b1);
        read_cycle(16'hC0F0, 1'b0, 8'h00, 1'b0);
        read_cycle(16'hC0C0, 1'b0, 8'h10, 1'b0);
        read_cycle(16'hC400, 1'b0, 8'h00, 1'b0);
        read_cycle(16'hC300, 1'b0, 8'h00, 1'b0);
        check(sss.c8_internal_rom, "internal C3 claims C8");
        read_cycle(16'hC500, 1'b0, 8'h20, 1'b0);
        read_cycle(16'hCA00, 1'b0, 8'h00, 1'b0);
        write_cycle(16'hCFFF, 8'h00);
        check(!sss.c8_internal_rom, "CFFF write releases internal C8");
        read_cycle(16'hC500, 1'b0, 8'h20, 1'b0);
        read_cycle(16'hCA00, 1'b0, 8'h20, 1'b0);
        read_cycle(16'hCA00, 1'b1, 8'h00, 1'b0);
        check(!slot5_bus.data_en && !slot5_bus.serve_en && slot5_bus.phi0_tick &&
              slot5_host_reply.assert_irq,
              "unselected cycles block side effects while clocks and IRQ continue");
        write_cycle(16'hC02D, 8'h02);
        check(!slot5_host_reply.assert_irq && slot5_bus.phi0_tick &&
              sss.c8_select == 0,
              "remap closes the aperture and releases IRQ while device clock continues");
        check_slot5_write(1'b0);
        write_cycle(16'hC02D, 8'h22);
        read_cycle(16'hCA00, 1'b0, 8'h00, 1'b0);
        read_cycle(16'hC500, 1'b0, 8'h20, 1'b0);
        write_cycle(16'hC068, 8'hF5);
        check(rom_mask == 0 && sss.sw_intcxrom && sss.sw_altzp &&
              sss.sw_page2 && sss.sw_ramrd && sss.sw_ramwrt &&
              sss.sw_lcram_bank2 && sss.sw_lcram_read,
              "C068 mirrors memory and ROM switch state");
        read_cycle(16'hC0D0, 1'b0, 8'h20, 1'b0);
        read_cycle(16'hCA00, 1'b0, 8'h00, 1'b0);
        write_cycle(16'hC006, 8'h00);
        read_cycle(16'hCA00, 1'b0, 8'h00, 1'b0);
        write_cycle(16'hC00B, 8'h00);
        read_cycle(16'hC300, 1'b0, 8'h08, 1'b0);

        // End a read without data_en, then leave an unqualified address
        // on the bus while the client still offers its old buffered byte.
        start_cycle(16'hC500, 1'b1, 1'b0);
        @(negedge clk);
        host.serve_en = 1'b0;
        check(slot5_host_reply.wr_data_en, "slot 5 did not open a reply");
        start_cycle(16'hCA00, 1'b1, 1'b1);
        check(data_slot == 0 && reply_slot == 0 && !slot5_host_reply.wr_data_en,
              "unselected address boundary must retire a pending reply");

        start_cycle(16'hC500, 1'b1, 1'b0);
        @(negedge clk);
        host.serve_en = 1'b0;
        check(slot5_host_reply.wr_data_en, "slot 5 did not open a reply");
        host.res = 1'b0;
        #1;
        check(data_slot == 0 && reply_slot == 0 && !slot5_host_reply.wr_data_en,
              "warm reset must immediately veto a buffered reply");
        repeat (2) @(negedge clk);
        check(live_mask == 8'h22 && sss.c8_select == 0 && reply_slot == 0,
              "warm reset clears C8 and retains the latest slot map");
        host.res = 1'b1;
        #1;
        check(reply_slot == 0 && !slot5_host_reply.wr_data_en,
              "reset release must wait for a new qualified read");
        $display("IIGS LIVE SLOTS PASS");
        $finish;
    end
endmodule
