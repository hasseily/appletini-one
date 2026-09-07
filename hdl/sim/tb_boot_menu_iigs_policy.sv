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
    logic aux_probe_pulse;
    logic [1:0] aux_status;
    int aux_probe_count = 0;
    always @(posedge clk)
        if (aux_probe_pulse) aux_probe_count++;

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
        .aux_probe_pulse(aux_probe_pulse),
        .aux_status(aux_status),
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
        @(negedge clk);
        rstn = 1'b0;
        idle_bus();
        sss = '0;
        sss.slot_access = 1'b1;
        repeat (5) @(posedge clk);
        @(negedge clk);
        rstn = 1'b1;
        repeat (3) @(posedge clk);
    endtask

    task automatic apple_reset_release;
        @(negedge clk);
        ab_read.res = 1'b0;
        sss.io_select = '0;
        repeat (3) @(posedge clk);
        @(negedge clk);
        ab_read.res = 1'b1;
        repeat (3) @(posedge clk);
    endtask

    task automatic apple_command(input logic [7:0] value);
        @(negedge clk);
        ab_read.addr = 16'hC0F0;
        ab_read.rw = 1'b0;
        ab_read.data = value;
        ab_read.data_en = 1'b1;
        @(posedge clk);
        @(negedge clk);
        ab_read.data_en = 1'b0;
        ab_read.rw = 1'b1;
        repeat (2) @(posedge clk);
    endtask

    task automatic boot_entry;
        @(negedge clk);
        ab_read.addr = 16'hC700;
        ab_read.rw = 1'b1;
        ab_read.serve_en = 1'b1;
        @(posedge clk);
        #1;
        check(ab_write.wr_data_en && ab_write.wr_data == 8'hA9,
              "C700 entry receives the boot ROM opcode");
        @(negedge clk);
        ab_read.serve_en = 1'b0;
        // The real soft-switch manager latches this claim on the C7 fetch.
        sss.io_select = 8'h80;
        repeat (2) @(posedge clk);
    endtask

    task automatic open_boot_window;
        apple_reset_release();
        boot_entry();
        apple_command(8'h01);
        check(dut.report_session_q, "owned boot entry opens a report session");
    endtask

    task automatic set_timeout(input logic [31:0] ticks);
        @(negedge clk);
        as_common.awaddr = 8'h02;
        as_common.wdata = ticks;
        as_common.wstrb = 4'hF;
        as_if.awvalid = 1'b1;
        @(posedge clk);
        @(negedge clk);
        as_if.awvalid = 1'b0;
        as_common.wstrb = 4'h0;
    endtask

    task automatic check_alias_ignored;
        apple_command(8'h01);
        apple_command(8'h26);
        apple_command(8'h22);
        apple_command(8'h27);
        apple_command(8'h80);
        apple_command(8'h25);
        check(machine_id == 4'd0 && !machine_id_fault && !iigs_policy_fault,
              "C0F register aliases cannot seed a host ID or policy fault");
        check(!dut.report_session_q && !dut.boot_slot_valid_q,
              "unowned C0F WINDOW_BEGIN does not open boot state");
        check(!aux_status[1], "unowned aliases do not publish an aux report");
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
    int probe_count_before;

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

        // Device I/O can be used without running our ROM (LINTXT or VDP).
        // Neither the command values nor a C8 claim alone prove boot entry.
        fabric_reset();
        apple_reset_release();
        probe_count_before = aux_probe_count;
        check_alias_ignored();
        sss.io_select = 8'h80;
        check_alias_ignored();
        check(aux_probe_count == probe_count_before,
              "unowned C0F $26 does not force an aux probe");

        // C700 alone is insufficient after another card takes C8.
        boot_entry();
        sss.io_select = 8'h00;
        check_alias_ignored();
        sss.io_select = 8'h80;
        sss.sw_intcxrom = 1'b1;
        check_alias_ignored();
        sss.sw_intcxrom = 1'b0;

        // Exercise the actual //e report order, including the old $26 fault.
        fabric_reset();
        open_boot_window();
        probe_count_before = aux_probe_count;
        apple_command(8'h26);
        check(aux_probe_count == probe_count_before + 1,
              "$26 forces one aux probe");
        check(machine_id == 4'd0 && !machine_id_fault,
              "$26 is not a machine ID");
        apple_command(8'h30);
        apple_command(8'h22);
        check(machine_id == 4'd2 && !iigs_policy_fault && aux_status == 2'b10,
              "IIe reports absent aux before native ID without a fault");
        check(!dut.report_session_q, "legacy ID completes the report session");
        apple_command(8'h24);
        apple_command(8'h25);
        check(machine_id == 4'd2 && !iigs_policy_fault,
              "finished report ignores later C0F ID-like values");

        // vTW changes the running ROM, not the physical host.
        open_boot_window();
        apple_command(8'h26);
        apple_command(8'h31);
        apple_command(8'h23);
        check(machine_id == 4'd2 && !iigs_policy_fault && aux_status == 2'b11,
              "enhanced vTW report preserves the first native IIe ID");
        fabric_reset();
        open_boot_window();
        apple_command(8'h21);
        open_boot_window();
        apple_command(8'h23);
        check(machine_id == 4'd1 && !iigs_policy_fault,
              "enhanced vTW report preserves native II+ pin semantics");

        // Reporting is independent of the prompt timeout on both host paths.
        fabric_reset();
        set_timeout(32'd0);
        open_boot_window();
        check(!dut.window_active_q && dut.report_session_q,
              "zero prompt timeout leaves host reporting armed");
        apple_command(8'h26);
        apple_command(8'h30);
        apple_command(8'h23);
        check(machine_id == 4'd3 && !iigs_policy_fault,
              "zero-timeout IIe report succeeds");
        fabric_reset();
        set_timeout(32'd0);
        open_boot_window();
        apple_command(8'h24);
        apple_command(8'h27);
        apple_command(8'h80);
        check(machine_id == 4'd4 && iigs_external_slot_mask_valid &&
              !iigs_policy_fault && !dut.report_session_q,
              "zero-timeout GS pair succeeds and closes reporting");

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
        open_boot_window();
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

        // A same-family repeat is harmless; a GS/legacy conflict faults.
        fabric_reset();
        open_boot_window();
        apple_command(8'h22);
        open_boot_window();
        apple_command(8'h22);
        check(machine_id == 4'd2 && !machine_id_fault,
              "same legacy ID repeat accepted");
        open_boot_window();
        apple_command(8'h24);
        check(machine_id == 4'd2 && machine_id_fault,
              "legacy-to-GS conflict cannot change first ID");

        // Raw masks that look like commands must have no command side effect.
        // Invalid bit 7 still faults the GS report, but $26 cannot probe aux,
        // $03 cannot request a menu, and $02 cannot start a handoff.
        for (int raw = 0; raw < 5; raw++) begin
            fabric_reset();
            open_boot_window();
            apple_command(8'h24);
            apple_command(8'h27);
            probe_count_before = aux_probe_count;
            case (raw)
                0: apple_command(8'h26);
                1: apple_command(8'h03);
                2: apple_command(8'h02);
                3: apple_command(8'h31);
                4: apple_command(8'h01);
            endcase
            check(iigs_policy_fault && !iigs_external_slot_mask_valid,
                  "invalid raw GS mask faults the report");
            check(aux_probe_count == probe_count_before &&
                  !dut.menu_requested_q && !dut.handoff_pending_q &&
                  !aux_status[1],
                  "escaped mask has no ordinary command side effects");
            check(!dut.report_session_q && !dut.iigs_slot_mask_escape_q,
                  "raw GS payload completes and closes the report");
        end

        fabric_reset();
        open_boot_window();
        apple_command(8'h25);
        check(machine_id_fault && machine_id == 4'd0 && !dut.report_session_q,
              "unsupported IIc report faults only an active report session");

        // A handoff closes an incomplete session and warm reset keeps the
        // storage mapping rather than reviving the boot command decoder.
        fabric_reset();
        open_boot_window();
        apple_command(8'h02);
        check(!dut.report_session_q && !dut.boot_entry_seen_q,
              "handoff command cancels incomplete reports");
        @(negedge clk);
        ab_read.addr = 16'hC700;
        ab_read.serve_en = 1'b1;
        @(posedge clk);
        @(negedge clk);
        ab_read.serve_en = 1'b0;
        apple_reset_release();
        check(dut.slot7_mode_q == 2'd1 && !dut.report_session_q,
              "warm reset preserves handed-off storage and closed reports");

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
