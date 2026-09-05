`timescale 1ns / 1ps

module tb_boot_menu_iigs_policy;

    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;

    logic rstn = 1'b0;
    globals::AppleBus_read ab_read;
    globals::SoftSwitchState sss;
    globals::AppleBus_write ab_write;
    globals::AxiSimple_common as_common;
    AxiSimple_if as_if();

    logic [3:0] machine_id;
    logic machine_id_fault;
    logic [7:0] iigs_external_slot_mask;
    logic iigs_external_slot_mask_valid;
    logic iigs_policy_fault;

    boot_menu_card dut (
        .clk(clk),
        .rstn(rstn),
        .ab_read(ab_read),
        .sss(sss),
        .disk2_enabled(1'b1),
        .apple_video_mode_valid(1'b1),
        .apple_video_mode_50hz(1'b0),
        .as_common(as_common),
        .as_client(as_if),
        .ab_write(ab_write),
        .smartport_active(),
        .disk2_active(),
        .boot_target_disk2(),
        .configured_boot_target_disk2(),
        .boot_slot(),
        .boot_slot_valid(),
        .apple_vblank_start_pulse(),
        .machine_id(machine_id),
        .machine_id_fault(machine_id_fault),
        .iigs_external_slot_mask(iigs_external_slot_mask),
        .iigs_external_slot_mask_valid(iigs_external_slot_mask_valid),
        .iigs_policy_fault(iigs_policy_fault),
        .aux_probe_pulse(),
        .aux_status(),
        .aux_status_clear(1'b0)
    );

    int failures = 0;

    task automatic check(input logic condition, input string message);
        if (condition !== 1'b1) begin
            $error("FAIL: %s", message);
            failures++;
        end
    endtask

    task automatic idle_bus;
        ab_read = '0;
        ab_read.res = 1'b1;
        ab_read.rw = 1'b1;
        ab_read.cycle_valid = 1'b1;
    endtask

    task automatic fabric_reset;
        rstn = 1'b0;
        idle_bus();
        repeat (5) @(posedge clk);
        rstn = 1'b1;
        repeat (3) @(posedge clk);
    endtask

    task automatic apple_reset_release;
        @(posedge clk);
        ab_read.res <= 1'b0;
        repeat (3) @(posedge clk);
        ab_read.res <= 1'b1;
        repeat (3) @(posedge clk);
    endtask

    task automatic apple_command(input logic [7:0] value);
        @(posedge clk);
        ab_read.addr <= 16'hC0F0;
        ab_read.rw <= 1'b0;
        ab_read.data <= value;
        ab_read.data_en <= 1'b1;
        @(posedge clk);
        ab_read.data_en <= 1'b0;
        ab_read.rw <= 1'b1;
        repeat (2) @(posedge clk);
    endtask

    task automatic open_boot_window;
        apple_reset_release();
        apple_command(8'h01);
    endtask

    task automatic axi_read_slot_policy(output logic [31:0] value);
        @(posedge clk);
        as_common.araddr <= 8'h07;
        repeat (2) @(posedge clk);
        value = as_if.rdata;
    endtask

    task automatic c8_scratch_read(
        input logic owns_c8,
        input logic expect_drive
    );
        @(negedge clk);
        sss.io_select = owns_c8 ? 8'h80 : 8'h00;
        sss.sw_intcxrom = 1'b0;
        sss.c8_internal_rom = 1'b0;
        ab_read.addr = 16'hCA00;
        ab_read.rw = 1'b1;
        ab_read.serve_en = 1'b1;
        @(posedge clk);
        #1;
        if (owns_c8) begin
            check(ab_write.wr_data_en == expect_drive,
                  "owned CA00 read drives data");
        end else begin
            check(ab_write.wr_data_en == expect_drive,
                  "unowned CA00 read stays silent");
        end

        @(negedge clk);
        ab_read.serve_en = 1'b0;
        ab_read.addr_en = 1'b1;
        @(posedge clk);
        #1;
        ab_read.addr_en = 1'b0;
        check(!ab_write.wr_data_en,
              "CA00 response clears at the next address boundary");
    endtask

    logic [31:0] policy_word;

    initial begin
        idle_bus();
        sss = '0;
        as_common = '0;
        as_if.awvalid = 1'b0;

        // The scratch page is part of this card's C8 window. It must never
        // answer until slot 7 has claimed C8, including before slot_setup
        // has written the resolved slot byte.
        fabric_reset();
        c8_scratch_read(1'b0, 1'b0);
        c8_scratch_read(1'b1, 1'b1);
        c8_scratch_read(1'b0, 1'b0);

        // A valid GS sequence locks ID 4 and the exact external-slot mask.
        fabric_reset();
        open_boot_window();
        apple_command(8'h24);
        apple_command(8'h27);
        apple_command(8'hA2);
        check(machine_id == 4'd4, "GS ID latched");
        check(!machine_id_fault, "valid GS ID has no conflict");
        check(iigs_external_slot_mask_valid, "GS slot mask valid");
        check(iigs_external_slot_mask == 8'hA2, "GS slot mask exact");
        check(!iigs_policy_fault, "valid GS policy has no fault");
        axi_read_slot_policy(policy_word);
        check(policy_word[7:0] == 8'hA2 && policy_word[8] &&
              !policy_word[10:9], "AXI slot-policy readback");

        // The boot ROM re-reports after a warm reset. The exact ID and mask
        // are idempotent and cannot erase or broaden either safety fact.
        open_boot_window();
        apple_command(8'h24);
        apple_command(8'h27);
        apple_command(8'hA2);
        check(machine_id == 4'd4 && iigs_external_slot_mask_valid &&
              iigs_external_slot_mask == 8'hA2,
              "warm reset exact re-report preserves locked GS facts");
        check(!iigs_policy_fault,
              "warm reset exact re-report remains fault-free");

        // A conflicting machine report leaves ID 4 in place and faults safe.
        apple_command(8'h22);
        check(machine_id == 4'd4, "conflict cannot downgrade GS ID");
        check(machine_id_fault && iigs_policy_fault,
              "conflicting machine ID faults safe");

        // GS/OS may change unrelated slot assignments. Only physical slot 7
        // is a safety fact, so an exact bit-7 repeat remains valid.
        fabric_reset();
        open_boot_window();
        apple_command(8'h24);
        apple_command(8'h27);
        apple_command(8'h82);
        open_boot_window();
        apple_command(8'h24);
        apple_command(8'h27);
        apple_command(8'hA2);
        check(!iigs_policy_fault && iigs_external_slot_mask_valid,
              "unrelated slot-bit changes do not disable slot 7");
        check(iigs_external_slot_mask == 8'hA2,
              "diagnostic readback tracks latest full C02D byte");

        // Running this external slot-7 ROM while C02D bit 7 is clear is an
        // inconsistent report. Reject every bit rather than trusting slots.
        fabric_reset();
        open_boot_window();
        apple_command(8'h24);
        apple_command(8'h27);
        apple_command(8'h72);
        check(iigs_policy_fault,
              "GS mask with external slot 7 clear faults safe");
        check(!iigs_external_slot_mask_valid &&
              iigs_external_slot_mask == 8'h00,
              "inconsistent GS mask grants no slots");

        // Reset in the middle of the escape cancels it. A later command byte
        // must not be mistaken for raw $C02D data.
        fabric_reset();
        open_boot_window();
        apple_command(8'h24);
        apple_command(8'h27);
        apple_reset_release();
        apple_command(8'h80);
        check(!iigs_external_slot_mask_valid,
              "warm reset cancels pending mask escape");
        check(iigs_external_slot_mask == 8'h00,
              "cancelled escape cannot capture a later byte");

        // A same-ID repeat is harmless; a different ID is sticky-faulted.
        fabric_reset();
        open_boot_window();
        apple_command(8'h22);
        apple_command(8'h22);
        check(machine_id == 4'd2 && !machine_id_fault,
              "same legacy ID repeat accepted");
        apple_command(8'h24);
        check(machine_id == 4'd2 && machine_id_fault,
              "legacy-to-GS conflict cannot change first ID");

        if (failures != 0)
            $fatal(1, "BOOT MENU IIGS POLICY FAIL: %0d checks", failures);
        $display("BOOT MENU IIGS POLICY PASS");
        $finish;
    end

    initial begin
        #100000;
        $fatal(1, "BOOT MENU IIGS POLICY TIMEOUT");
    end

endmodule
