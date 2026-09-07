`timescale 1ns / 1ps

// Replay the monitor's signature reads and the boot ROM's report protocol
// through the production ROM, GS I/O guard, and machine policy. This is a
// bus-transaction regression, not a 6502 CPU or physical-pad simulation.
module apple_boot_report_trace #(parameter bit GS = 0);
    logic clk = 0;
    always #3.75 clk = ~clk;
    logic resetn = 0;
    logic onee_enable_effective = 0;
    globals::AppleBus_read physical_ab_read, guarded_read, boot_ab_read;
    globals::AppleBus_write boot_raw_write, guarded_write, boot_ab_write;
    globals::SoftSwitchState sss;
    globals::AxiSimple_common as_common;
    AxiSimple_if as_if();
    logic [3:0] boot_machine_id;
    logic boot_machine_id_fault, boot_iigs_policy_fault;
    logic [7:0] boot_iigs_slot_mask;
    logic boot_iigs_slot_mask_valid;
    logic machine_identity_reported, machine_identity_legacy;
    logic machine_identity_iigs, reported_identity_fault;
    logic decoded_legacy, decoded_iigs, decoded_iiplus;
    logic [7:0] decoded_slot_mask, physical_slot_allowed_mask;
    logic machine_gs_m2_qualify, aux_probe_pulse;
    logic [1:0] aux_status;
    logic [7:0] rom [0:255];
    logic [7:0] c8rom [0:255];
    wire output_kill = !resetn || onee_enable_effective ||
        boot_machine_id_fault || boot_iigs_policy_fault;
    wire ownership_allowed = machine_identity_legacy && !output_kill;

    apple_machine_safety_policy policy_i (
        .locked_machine_id(boot_machine_id),
        .machine_id_fault(boot_machine_id_fault),
        .iigs_slot_policy_fault(boot_iigs_policy_fault),
        .iigs_external_slot_mask(boot_iigs_slot_mask),
        .iigs_external_slot_mask_valid(boot_iigs_slot_mask_valid),
        .requested_machine_mode(2'd2), .requested_m2sel_active_high(1'b1),
        .machine_identity_reported,
        .machine_identity_legacy(decoded_legacy),
        .machine_identity_iigs(decoded_iigs),
        .machine_identity_fault(reported_identity_fault),
        .physical_slot_allowed_mask(decoded_slot_mask), .machine_inh_allowed(),
        .machine_gs_m2_qualify, .machine_is_iiplus(decoded_iiplus),
        .effective_machine_mode(), .effective_m2sel_active_high()
    );
    apple_machine_policy_latch policy_latch_i (
        .clk, .resetn, .host_policy_resetn(resetn), .onee_enable_effective,
        .machine_id_fault(boot_machine_id_fault),
        .iigs_slot_policy_fault(boot_iigs_policy_fault),
        .decoded_legacy, .decoded_iigs, .decoded_iiplus, .decoded_slot_mask,
        .machine_identity_legacy, .machine_identity_iigs,
        .machine_is_iiplus(), .physical_slot_allowed_mask
    );
    apple_slot7_devsel_guard slot_guard_i (
        .devsel_required(machine_gs_m2_qualify),
        .ab_read_in(physical_ab_read), .physical_ab_read,
        .supersprite_write_in('0), .smartport_write_in('0),
        .boot_menu_write_in(boot_raw_write), .ab_read_out(guarded_read),
        .supersprite_write_out(), .smartport_write_out(),
        .boot_menu_write_out(guarded_write)
    );
    always_comb begin
        boot_ab_read = guarded_read;
        boot_ab_write = guarded_write;
        if (output_kill) begin
            boot_ab_read.serve_en = 0;
            boot_ab_read.data_en = 0;
            boot_ab_write = '0;
        end
    end
    boot_menu_card boot_i (
        .clk, .rstn(resetn), .ab_read(boot_ab_read), .sss,
        .disk2_enabled(1'b0), .apple_video_mode_valid(1'b1),
        .apple_video_mode_50hz(1'b0), .as_common, .as_client(as_if),
        .ab_write(boot_raw_write), .smartport_active(), .disk2_active(),
        .boot_target_disk2(), .configured_boot_target_disk2(),
        .boot_slot(), .boot_slot_valid(), .apple_vblank_start_pulse(),
        .machine_id(boot_machine_id), .machine_id_fault(boot_machine_id_fault),
        .iigs_external_slot_mask(boot_iigs_slot_mask),
        .iigs_external_slot_mask_valid(boot_iigs_slot_mask_valid),
        .iigs_policy_fault(boot_iigs_policy_fault), .aux_probe_pulse,
        .aux_status, .aux_status_clear(1'b0)
    );
    task automatic check(input logic condition, input string message);
        if (condition !== 1'b1) $fatal(1, "%s", message);
    endtask
    task automatic boundary;
        @(negedge clk);
        physical_ab_read.serve_en = 0;
        physical_ab_read.data_en = 0;
        physical_ab_read.addr_en = 1;
        @(posedge clk);
        #1;
        check(!boot_ab_write.wr_data_en, "address boundary releases boot data");
        @(negedge clk);
        physical_ab_read.addr_en = 0;
    endtask
    task automatic rom_read(input logic [15:0] addr, input logic pin39,
                            input logic expect_drive, input logic [7:0] value);
        boundary();
        physical_ab_read.addr = addr;
        physical_ab_read.addr_early = addr;
        physical_ab_read.rw = 1;
        physical_ab_read.rw_early = 1;
        physical_ab_read.m2sel = pin39;
        // This models the wrapper's cycle-valid contract after the ROM ID.
        physical_ab_read.cycle_valid = !(machine_gs_m2_qualify && pin39);
        physical_ab_read.serve_en = physical_ab_read.cycle_valid;
        @(posedge clk);
        #1;
        check(boot_ab_write.wr_data_en == expect_drive,
              $sformatf("ROM response enable at %h", addr));
        if (expect_drive)
            check(boot_ab_write.wr_data == value,
                  $sformatf("generated ROM byte at %h", addr));
        boundary();
    endtask
    task automatic write_cycle(input logic [15:0] addr,
                               input logic [7:0] value,
                               input logic devsel_n = !GS);
        boundary();
        physical_ab_read.addr = addr;
        physical_ab_read.addr_early = addr;
        physical_ab_read.rw = 0;
        physical_ab_read.rw_early = 0;
        physical_ab_read.m2sel = 0;
        physical_ab_read.devsel_n = devsel_n;
        physical_ab_read.devsel_n_early = devsel_n;
        physical_ab_read.cycle_valid = 1;
        physical_ab_read.data = value;
        physical_ab_read.data_en = 1;
        @(posedge clk);
        #1;
        if (value == 8'h26 && addr == 16'hC0F0)
            check(aux_probe_pulse, "26 emits the auxiliary probe pulse");
        boundary();
        repeat (2) @(posedge clk);
        #1;
    endtask
    task automatic cold_reset;
        @(negedge clk);
        resetn = 0;
        physical_ab_read = '0;
        sss = '0;
        sss.slot_access = 1;
        physical_ab_read.devsel_n = !GS;
        physical_ab_read.devsel_n_early = !GS;
        repeat (3) @(posedge clk);
        @(negedge clk);
        resetn = 1;
        physical_ab_read.res = 1;
        repeat (3) @(posedge clk);
        #1;
    endtask
    task automatic begin_boot;
        // Actual enhanced //e scan: no C707 and no screen/banner events.
        rom_read(16'hC705, 0, 1, 8'h03);
        rom_read(16'hC703, 0, 1, 8'h00);
        rom_read(16'hC701, 0, 1, 8'h20);
        rom_read(16'hC700, !GS, 1, rom[0]);
        for (int offset = 1; offset <= 6; offset++)
            rom_read(16'hC700 + offset, !GS && !offset[0], 1, rom[offset]);
        check(!ownership_allowed && physical_slot_allowed_mask == 0,
              "slot ROM execution alone grants no physical ownership");
        sss.io_select[7] = 1;
        write_cycle(16'hCA00, 8'h70);
        rom_read(16'hCA00, 0, 1, 8'h70);
        write_cycle(16'hC0F0, 8'h01);
        rom_read(16'hC800, !GS, 1, c8rom[0]);
    endtask
    initial begin
        $readmemh("boot_menu_slot7.mem", rom);
        $readmemh("boot_menu_slot7_c8.mem", c8rom);
        as_common = '0;
        as_if.awvalid = 0;
        cold_reset();
        write_cycle(16'hC0F0, 8'h24);
        write_cycle(16'hC0F0, 8'h27);
        write_cycle(16'hC0F0, 8'h80);
        check(boot_machine_id == 0 && !reported_identity_fault &&
              !ownership_allowed, "unopened report writes cannot seed identity");
        begin_boot();
        if (GS) begin
            write_cycle(16'hC0F0, 8'h24);
            check(machine_identity_iigs && machine_gs_m2_qualify &&
                  !ownership_allowed && physical_slot_allowed_mask == 0,
                  "GS report fixes M2SEL policy without ownership or slots");
            write_cycle(16'hC0F0, 8'h27);
            write_cycle(16'hC0F0, 8'h80);
            check(boot_iigs_slot_mask_valid && physical_slot_allowed_mask == 8'h80 &&
                  !ownership_allowed, "GS report pair grants slot 7 data only");
            rom_read(16'hC800, 1, 0, 0);
            rom_read(16'hC800, 0, 1, c8rom[0]);
            write_cycle(16'hC0F0, 8'h23, 1);
            check(boot_machine_id == 4 && !reported_identity_fault,
                  "GS internal device remap ignores a stale command");
            check(!ownership_allowed, "GS never acquires legacy controls");
            $display("IIGS BOOTSTRAP PATH PASS");
        end else begin
            write_cycle(16'hC0F0, 8'h26);
            check(boot_machine_id == 0 && !reported_identity_fault && !ownership_allowed,
                  "26 is a probe command and cannot fault or grant identity");
            rom_read(16'hC8AE, 1, 1, c8rom[8'hAE]);
            write_cycle(16'hC0F0, 8'h31);
            check(aux_status == 2'b11, "physical auxiliary-memory report is retained");
            write_cycle(16'hC0F0, 8'h23);
            check(machine_identity_legacy && ownership_allowed &&
                  physical_slot_allowed_mask == 8'hFE,
                  "enhanced IIe boot report grants supported legacy functions");
            @(negedge clk);
            physical_ab_read.res = 0;
            repeat (3) @(posedge clk);
            @(negedge clk);
            physical_ab_read.res = 1;
            repeat (3) @(posedge clk);
            #1;
            check(boot_machine_id == 3 && ownership_allowed,
                  "Apple warm reset preserves the physical host report");
            cold_reset();
            begin_boot();
            write_cycle(16'hC0F0, 8'h26);
            write_cycle(16'hC0F0, 8'h30);
            write_cycle(16'hC0F0, 8'h22);
            check(boot_machine_id == 2 && ownership_allowed && aux_status == 2'b10,
                  "unenhanced IIe boots without a banner or auxiliary RAM");
            $display("IIE BOOTSTRAP PATH PASS");
        end
        $finish;
    end
    initial begin
        #100000;
        $fatal(1, "boot report trace timeout");
    end
endmodule

module tb_iie_bootstrap_path;
    apple_boot_report_trace #(.GS(0)) trace_i();
endmodule