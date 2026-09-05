`timescale 1ns / 1ps

module tb_apple_bootstrap_guard;
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
    logic boot_menu_physical_visible;
    logic bootstrap_identity_valid;
    logic bootstrap_identity_legacy;
    logic bootstrap_identity_iigs;
    logic unknown_boot_select_seen;

    apple_bootstrap_guard dut (.*);

    int failures = 0;
    task automatic check(input logic condition, input string message);
        if (condition !== 1'b1) begin
            $error("FAIL: %s", message);
            failures++;
        end
    endtask

    task automatic read_cycle(
        input logic [15:0] addr,
        input logic pin39,
        input logic valid,
        input logic expect_visible
    );
        @(negedge clk);
        physical_ab_read.addr = addr;
        physical_ab_read.addr_early = addr;
        physical_ab_read.rw = 1'b1;
        physical_ab_read.rw_early = 1'b1;
        physical_ab_read.m2sel = pin39;
        physical_ab_read.cycle_valid = valid;
        physical_ab_read.serve_en = 1'b1;
        #1;
        if (expect_visible)
            check(!boot_menu_physical_visible,
                  "newly matched read waits for registered permit");
        else
            check(boot_menu_physical_visible == expect_visible,
                  $sformatf("visibility for %h pin39=%0b", addr, pin39));
        @(posedge clk);
        #1;
        if (expect_visible)
            check(boot_menu_physical_visible,
                  "accepted read token survives registered response edge");
        @(negedge clk);
        physical_ab_read.serve_en = 1'b0;
        physical_ab_read.addr_en = 1'b1;
        @(posedge clk);
        #1;
        physical_ab_read.addr_en = 1'b0;
    endtask

    task automatic fabric_reset;
        @(negedge clk);
        resetn = 1'b0;
        physical_ab_read = '0;
        physical_ab_read.res = 1'b1;
        repeat (2) @(posedge clk);
        @(negedge clk);
        resetn = 1'b1;
    endtask

    initial begin
        physical_ab_read = '0;
        physical_ab_read.res = 1'b1;
        fabric_reset();

        // Physical activity observed while ONEe owns the isolated virtual
        // machine cannot seed a verdict that leaks out when ONEe stops.
        onee_enable_effective = 1'b1;
        read_cycle(16'hC707, 1'b0, 1'b1, 1'b0);
        read_cycle(16'hC705, 1'b0, 1'b1, 1'b0);
        read_cycle(16'hC703, 1'b0, 1'b1, 1'b0);
        read_cycle(16'hC701, 1'b0, 1'b1, 1'b0);
        read_cycle(16'hC700, 1'b0, 1'b1, 1'b0);
        onee_enable_effective = 1'b0;
        #1;
        check(!bootstrap_identity_valid && !unknown_boot_select_seen,
              "ONEe sequence cannot leak a physical GS verdict");

        // No high-pin value is ever a pre-ID authorization, even if both
        // address samples are stable and look exactly like the boot entry.
        read_cycle(16'hC707, 1'b1, 1'b1, 1'b0);
        read_cycle(16'hC700, 1'b1, 1'b1, 1'b0);
        check(!bootstrap_identity_valid,
              "stable high C700 cannot classify legacy or GS");

        // Wrong low-selected order resets progress and remains silent.
        read_cycle(16'hC705, 1'b0, 1'b1, 1'b0);
        check(!unknown_boot_select_seen, "out-of-order read resets sequence");

        // Exact monitor order: C707,C705,C703,C701. Intervening high-pin
        // stale values are ignored and never visible.
        read_cycle(16'hC707, 1'b0, 1'b1, 1'b1);
        check(unknown_boot_select_seen, "C707 starts descending scan");
        read_cycle(16'hC701, 1'b1, 1'b1, 1'b0);
        read_cycle(16'hC705, 1'b0, 1'b1, 1'b1);
        read_cycle(16'hC703, 1'b0, 1'b1, 1'b1);
        read_cycle(16'hC701, 1'b0, 1'b1, 1'b1);

        // A changing early/late high C700 stays silent; only low C700 closes
        // the classifier and the same accepted read remains authorized.
        @(negedge clk);
        physical_ab_read.addr_early = 16'hC700;
        physical_ab_read.addr = 16'hC701;
        physical_ab_read.rw_early = 1'b1;
        physical_ab_read.rw = 1'b1;
        physical_ab_read.m2sel = 1'b1;
        physical_ab_read.cycle_valid = 1'b1;
        physical_ab_read.serve_en = 1'b1;
        #1;
        check(!boot_menu_physical_visible,
              "changing high C700/C701 remains silent");
        @(posedge clk);
        @(negedge clk);
        physical_ab_read.serve_en = 1'b0;
        read_cycle(16'hC700, 1'b0, 1'b1, 1'b1);
        check(bootstrap_identity_valid && bootstrap_identity_iigs &&
              !bootstrap_identity_legacy,
              "selected low C700 locks provisional GS only");

        // A stale high C7/FE1F fast cycle has no qualified strobe after the
        // provisional verdict arms /M2SEL in the wrapper.
        read_cycle(16'hC701, 1'b1, 1'b0, 1'b0);

        machine_identity_reported = 1'b1;
        machine_identity_iigs = 1'b1;
        iigs_external_slot_mask_valid = 1'b1;
        iigs_slot7_allowed = 1'b1;
        #1;
        check(!boot_menu_physical_visible,
              "GS responder remains off between selected cycles");
        read_cycle(16'hC700, 1'b0, 1'b1, 1'b1);

        // Apple RESET does not erase the sticky GS classifier. The boot ROM
        // may safely re-report the same ID/mask on a warm reset.
        @(negedge clk);
        physical_ab_read.res = 1'b0;
        repeat (2) @(posedge clk);
        physical_ab_read.res = 1'b1;
        #1;
        check(bootstrap_identity_iigs && !boot_menu_physical_visible,
              "warm reset preserves GS class but clears cycle permit");
        read_cycle(16'hC700, 1'b0, 1'b1, 1'b1);

        reported_identity_fault = 1'b1;
        #1;
        check(!boot_menu_physical_visible,
              "identity/policy fault hides responder");
        onee_enable_effective = 1'b1;
        check(!boot_menu_physical_visible,
              "ONEe never exposes physical boot responder");

        if (failures != 0)
            $fatal(1, "APPLE BOOTSTRAP GUARD FAIL: %0d checks", failures);
        $display("APPLE BOOTSTRAP GUARD PASS");
        $finish;
    end

    initial begin
        #100000;
        $fatal(1, "APPLE BOOTSTRAP GUARD TIMEOUT");
    end
endmodule
