`timescale 1ns / 1ps

module tb_iigs_bootstrap_path;
    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;
    logic resetn = 1'b0;
    logic onee_enable_effective = 1'b0;
    logic machine_identity_reported = 1'b0;
    logic machine_identity_legacy = 1'b0;
    logic machine_identity_iigs = 1'b0;
    logic reported_identity_fault = 1'b0;
    logic iigs_external_slot_mask_valid = 1'b0;
    logic iigs_slot7_allowed = 1'b0;
    globals::AppleBus_read physical_ab_read;
    globals::AppleBus_read boot_ab_read;
    globals::SoftSwitchState sss;
    globals::AppleBus_write boot_raw_write;
    globals::AppleBus_write boot_ab_write;
    globals::AxiSimple_common as_common;
    AxiSimple_if as_if();
    logic boot_menu_physical_visible;
    logic bootstrap_identity_valid;
    logic bootstrap_identity_legacy;
    logic bootstrap_identity_iigs;
    logic unknown_boot_select_seen;
    logic [3:0] boot_machine_id;
    logic boot_machine_id_fault;
    logic [7:0] boot_iigs_slot_mask;
    logic boot_iigs_slot_mask_valid;

    apple_bootstrap_guard guard_i (
        .clk(clk), .resetn(resetn), .onee_enable_effective,
        .machine_identity_reported, .machine_identity_legacy,
        .machine_identity_iigs, .reported_identity_fault,
        .iigs_external_slot_mask_valid, .iigs_slot7_allowed,
        .physical_ab_read, .boot_menu_physical_visible,
        .bootstrap_identity_valid, .bootstrap_identity_legacy,
        .bootstrap_identity_iigs, .unknown_boot_select_seen
    );

    always_comb begin
        boot_ab_read = physical_ab_read;
        if (!boot_menu_physical_visible) begin
            boot_ab_read.data_en = 1'b0;
            boot_ab_write = '0;
        end else begin
            boot_ab_write = boot_raw_write;
        end
    end

    boot_menu_card boot_i (
        .clk(clk), .rstn(resetn), .ab_read(boot_ab_read), .sss(sss),
        .disk2_enabled(1'b0), .apple_video_mode_valid(1'b1),
        .apple_video_mode_50hz(1'b0), .as_common(as_common),
        .as_client(as_if), .ab_write(boot_raw_write), .smartport_active(),
        .disk2_active(), .boot_target_disk2(),
        .configured_boot_target_disk2(), .boot_slot(), .boot_slot_valid(),
        .apple_vblank_start_pulse(), .machine_id(boot_machine_id),
        .machine_id_fault(boot_machine_id_fault),
        .iigs_external_slot_mask(boot_iigs_slot_mask),
        .iigs_external_slot_mask_valid(boot_iigs_slot_mask_valid),
        .iigs_policy_fault(), .aux_probe_pulse(), .aux_status(),
        .aux_status_clear(1'b0)
    );

    int failures = 0;
    task automatic check(input logic condition, input string message);
        if (condition !== 1'b1) begin
            $error("FAIL: %s", message);
            failures++;
        end
    endtask

    task automatic blocked_prebootstrap_write(input logic [7:0] value);
        @(negedge clk);
        physical_ab_read.addr = 16'hC0F0;
        physical_ab_read.addr_early = 16'hC0F0;
        physical_ab_read.rw = 1'b0;
        physical_ab_read.rw_early = 1'b0;
        physical_ab_read.m2sel = 1'b0;
        physical_ab_read.cycle_valid = 1'b1;
        physical_ab_read.serve_en = 1'b1;
        physical_ab_read.data_en = 1'b0;
        @(posedge clk);
        @(negedge clk);
        physical_ab_read.serve_en = 1'b0;
        physical_ab_read.data = value;
        physical_ab_read.data_en = 1'b1;
        @(posedge clk);
        #1;
        check(boot_machine_id == 4'd0 && !boot_machine_id_fault &&
              !boot_iigs_slot_mask_valid,
              "pre-bootstrap command write cannot seed trusted GS state");
        @(negedge clk);
        physical_ab_read.data_en = 1'b0;
        physical_ab_read.addr_en = 1'b1;
        @(posedge clk);
        physical_ab_read.addr_en = 1'b0;
    endtask

    task automatic rom_read(
        input logic [15:0] addr,
        input logic pin39,
        input logic valid,
        input logic expect_drive,
        input logic [7:0] expected_data
    );
        @(negedge clk);
        physical_ab_read.addr = addr;
        physical_ab_read.addr_early = addr;
        physical_ab_read.rw = 1'b1;
        physical_ab_read.rw_early = 1'b1;
        physical_ab_read.m2sel = pin39;
        physical_ab_read.cycle_valid = valid;
        /* Match the wrapper contract: an M2SEL-high IIgs fast cycle has
         * cycle_valid=0 and therefore no serve strobe, even if stale C7
         * address bits remain on the connector. */
        physical_ab_read.serve_en = valid;
        physical_ab_read.addr_en = 1'b0;
        #1;
        @(posedge clk);
        #1;
        if (valid && (addr[15:8] == 8'hC7))
            check(boot_raw_write.wr_data_en,
                  $sformatf("raw boot reply prepared for %h", addr));
        check(boot_ab_write.wr_data_en == expect_drive,
              $sformatf("physical response enable for %h", addr));
        if (expect_drive)
            check(boot_ab_write.wr_data == expected_data,
                  $sformatf("physical response byte for %h", addr));

        @(negedge clk);
        physical_ab_read.serve_en = 1'b0;
        physical_ab_read.addr_en = 1'b1;
        @(posedge clk);
        #1;
        physical_ab_read.addr_en = 1'b0;
        check(!boot_ab_write.wr_data_en,
              "next address boundary clears registered boot response");
    endtask

    initial begin
        physical_ab_read = '0;
        physical_ab_read.res = 1'b1;
        sss = '0;
        sss.slot_access = 1'b1;
        as_common = '0;
        as_if.awvalid = 1'b0;
        repeat (3) @(posedge clk);
        @(negedge clk);
        resetn = 1'b1;

        blocked_prebootstrap_write(8'h24);
        blocked_prebootstrap_write(8'h27);
        blocked_prebootstrap_write(8'h80);
        // An unmatched low-/M2SEL read may prepare an internal reply, but it
        // stays off the pins and the next address boundary removes it.
        rom_read(16'hC700, 1'b0, 1'b1, 1'b0, 8'h00);
        rom_read(16'hC707, 1'b0, 1'b1, 1'b1, 8'h3C);
        rom_read(16'hC705, 1'b0, 1'b1, 1'b1, 8'h03);
        rom_read(16'hC703, 1'b0, 1'b1, 1'b1, 8'h00);
        rom_read(16'hC701, 1'b0, 1'b1, 1'b1, 8'h20);
        rom_read(16'hC700, 1'b0, 1'b1, 1'b1, 8'hA9);
        check(bootstrap_identity_iigs,
              "low C700 locks GS before the next bus cycle");

        // Model the later $FE1F internal fast access after the wrapper has
        // armed active-low M2 qualification. Stale C7 pins cannot drive.
        rom_read(16'hC701, 1'b1, 1'b0, 1'b0, 8'h00);

        if (failures != 0)
            $fatal(1, "IIGS BOOTSTRAP PATH FAIL: %0d checks", failures);
        $display("IIGS BOOTSTRAP PATH PASS");
        $finish;
    end

    initial begin
        #100000;
        $fatal(1, "IIGS BOOTSTRAP PATH TIMEOUT");
    end
endmodule
