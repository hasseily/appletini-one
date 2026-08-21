`timescale 1ns / 1ps

// ONE//e slot-order state across entry and an ordered cold reboot. Both
// instances stay enabled while firmware changes the target under RES#.
module tb_onee_cold_slot_scan;

    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;

    logic resetn = 1'b0;
    logic enabled = 1'b1;
    logic manual_enable_request = 1'b1;
    logic configured_disk2_target = 1'b1;
    logic configured_smartport_target = 1'b0;
    logic warm_reset_active = 1'b0;
    globals::AppleBus_read ab_read = '0;
    logic disk2_session_target;
    logic smartport_session_target;
    logic disk2_slot7_hidden;
    logic smartport_slot7_hidden;

    onee_cold_slot_scan disk2_scan_i (
        .clk(clk),
        .resetn(resetn),
        .enabled(enabled),
        .manual_enable_request(manual_enable_request),
        .boot_target_disk2(configured_disk2_target),
        .warm_reset_active(warm_reset_active),
        .ab_read(ab_read),
        .session_boot_target_disk2(disk2_session_target),
        .slot7_hidden(disk2_slot7_hidden)
    );

    onee_cold_slot_scan smartport_scan_i (
        .clk(clk),
        .resetn(resetn),
        .enabled(enabled),
        .manual_enable_request(manual_enable_request),
        .boot_target_disk2(configured_smartport_target),
        .warm_reset_active(warm_reset_active),
        .ab_read(ab_read),
        .session_boot_target_disk2(smartport_session_target),
        .slot7_hidden(smartport_slot7_hidden)
    );

    task automatic check(input logic condition, input string message);
        if (condition !== 1'b1)
            $fatal(1, "ONEE SLOT SCAN FAIL: %s", message);
    endtask

    task automatic probe(input logic [15:0] addr);
        @(negedge clk);
        ab_read.addr = addr;
        ab_read.rw = 1'b1;
        ab_read.serve_en = 1'b1;
        @(posedge clk);
        @(negedge clk);
        ab_read.serve_en = 1'b0;
    endtask

    initial begin
        // Match startup: ONE//e is selected while the virtual motherboard
        // reset is still held low.
        ab_read.res = 1'b0;
        repeat (4) @(posedge clk);
        resetn = 1'b1;
        repeat (2) @(posedge clk);
        check(disk2_slot7_hidden,
              "cold startup did not hide slot 7 for Disk II");
        check(!smartport_slot7_hidden,
              "cold startup hid slot 7 for SmartPort");
        check(disk2_session_target && !smartport_session_target,
              "request edge did not latch both configured targets");

        @(negedge clk);
        ab_read.res = 1'b1;
        probe(16'hC700);
        check(disk2_slot7_hidden,
              "Disk II target released slot 7 on the $C700 probe");
        check(!smartport_slot7_hidden,
              "SmartPort target hid its first $C700 probe");
        probe(16'hC600);
        check(!disk2_slot7_hidden,
              "Disk II target did not release slot 7 at $C600");

        // Editing the saved next-boot choice must not change who owns slot 7
        // before a cold-boot boundary.
        configured_disk2_target = 1'b0;
        configured_smartport_target = 1'b1;
        repeat (2) @(posedge clk);
        check(disk2_session_target && !smartport_session_target,
              "configured target changed active session ownership");

        // A marked warm reset must not restart the cold scan or apply a newly
        // saved boot target, regardless of how long RESET remains low.
        @(negedge clk);
        warm_reset_active = 1'b1;
        ab_read.res = 1'b0;
        repeat (10) @(posedge clk);
        check(disk2_session_target && !smartport_session_target,
              "warm reset changed the active boot targets");
        check(!disk2_slot7_hidden && !smartport_slot7_hidden,
              "warm reset re-hid slot 7");

        // An unmarked low RESET is an explicit cold-boot boundary.
        @(negedge clk);
        warm_reset_active = 1'b0;
        @(posedge clk);
        @(negedge clk);
        check(enabled, "test left ONE//e during cold reboot");
        check(!disk2_session_target && smartport_session_target,
              "cold reboot did not sample the changed boot targets");
        check(!disk2_slot7_hidden,
              "cold reboot hid slot 7 for the new SmartPort target");
        check(smartport_slot7_hidden,
              "cold reboot did not hide slot 7 for the new Disk II target");

        @(negedge clk);
        ab_read.res = 1'b1;
        probe(16'hC700);
        check(!disk2_slot7_hidden,
              "new SmartPort target hid the first $C700 probe");
        check(smartport_slot7_hidden,
              "new Disk II target exposed the first $C700 probe");
        probe(16'hC600);
        check(!smartport_slot7_hidden,
              "new Disk II target did not release slot 7 at $C600");

        // A second edit still waits for the next boundary. A manual off/on
        // selection remains a valid boundary and accepts it.
        configured_disk2_target = 1'b1;
        configured_smartport_target = 1'b0;
        repeat (2) @(posedge clk);
        check(!disk2_session_target && smartport_session_target,
              "second edit changed the running scan target");
        @(negedge clk);
        enabled = 1'b0;
        manual_enable_request = 1'b0;
        repeat (2) @(posedge clk);
        check(!disk2_slot7_hidden && !smartport_slot7_hidden,
              "manual off did not release slot visibility state");
        @(negedge clk);
        manual_enable_request = 1'b1;
        enabled = 1'b1;
        repeat (2) @(posedge clk);
        check(disk2_session_target && !smartport_session_target,
              "manual reselect did not latch the new configured targets");
        check(disk2_slot7_hidden && !smartport_slot7_hidden,
              "new session did not apply the new target visibility");

        $display("ONEE COLD SLOT SCAN RESET PASS");
        $finish;
    end

    initial begin
        #10us;
        $fatal(1, "ONEE SLOT SCAN FAIL: timeout");
    end

endmodule
