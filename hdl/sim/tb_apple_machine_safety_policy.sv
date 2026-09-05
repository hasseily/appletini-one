`timescale 1ns / 1ps

module tb_apple_machine_safety_policy;

    logic [3:0] locked_machine_id;
    logic machine_id_fault;
    logic iigs_slot_policy_fault;
    logic [7:0] iigs_external_slot_mask;
    logic iigs_external_slot_mask_valid;
    logic bootstrap_identity_valid;
    logic bootstrap_identity_legacy;
    logic bootstrap_identity_iigs;
    logic [1:0] requested_machine_mode;
    logic requested_m2sel_active_high;
    logic machine_identity_reported;
    logic machine_identity_legacy;
    logic machine_identity_iigs;
    logic machine_identity_fault;
    logic [7:0] physical_slot_allowed_mask;
    logic machine_inh_allowed;
    logic machine_gs_m2_qualify;
    logic machine_is_iiplus;
    logic [1:0] effective_machine_mode;
    logic effective_m2sel_active_high;

    apple_machine_safety_policy dut (.*);

    int failures = 0;

    task automatic check(input logic condition, input string message);
        if (condition !== 1'b1) begin
            $error("FAIL: %s", message);
            failures++;
        end
    endtask

    initial begin
        locked_machine_id = 4'd0;
        machine_id_fault = 1'b0;
        iigs_slot_policy_fault = 1'b0;
        iigs_external_slot_mask = 8'hFF;
        iigs_external_slot_mask_valid = 1'b1;
        bootstrap_identity_valid = 1'b0;
        bootstrap_identity_legacy = 1'b0;
        bootstrap_identity_iigs = 1'b0;
        requested_machine_mode = 2'd2;
        requested_m2sel_active_high = 1'b1;
        #1;
        check(!machine_identity_reported && !machine_inh_allowed,
              "unknown host cannot own the bus");
        check(physical_slot_allowed_mask == 8'h00,
              "unknown host cannot enable physical slots");
        check(!effective_m2sel_active_high,
              "unknown host cannot request pin-39 inversion");

        // A raw ID4 write without the exact low-/M2SEL bootstrap sequence may
        // arm conservative GS qualification, but it cannot grant card access.
        locked_machine_id = 4'd4;
        #1;
        check(machine_identity_reported && machine_gs_m2_qualify &&
              !machine_identity_iigs && !machine_inh_allowed,
              "unproven ID4 cannot grant a GS identity or bus ownership");
        check(physical_slot_allowed_mask == 8'h00,
              "unproven ID4 cannot enable physical slot 7");
        locked_machine_id = 4'd0;

        // The selected low C700 verdict arms active-low GS qualification
        // before the ROM's later internal fast call, but grants no slot yet.
        bootstrap_identity_valid = 1'b1;
        bootstrap_identity_iigs = 1'b1;
        #1;
        check(machine_gs_m2_qualify && !machine_identity_reported,
              "provisional GS arms M2SEL before ID4");
        check(!machine_inh_allowed &&
              physical_slot_allowed_mask == 8'h00,
              "provisional GS grants no ownership");

        // Raw PS mode and polarity requests cannot relax an ID4 verdict.
        locked_machine_id = 4'd4;
        iigs_external_slot_mask_valid = 1'b0;
        requested_machine_mode = 2'd2;
        requested_m2sel_active_high = 1'b1;
        #1;
        check(machine_identity_iigs && machine_gs_m2_qualify,
              "ID4 enables fixed GS cycle qualification");
        check(!machine_inh_allowed,
              "ID4 can never grant INH or bus mastering");
        check(effective_machine_mode == 2'd3 &&
              !effective_m2sel_active_high,
              "GS ignores conflicting PS mode and polarity writes");
        check(physical_slot_allowed_mask == 8'h00,
              "GS slots remain disabled until C02D is valid");

        iigs_external_slot_mask = 8'hFF;
        iigs_external_slot_mask_valid = 1'b1;
        #1;
        check(physical_slot_allowed_mask == 8'h80,
              "GS permits only wired physical slot 7");
        iigs_external_slot_mask = 8'hA2;
        #1;
        check(physical_slot_allowed_mask == 8'h80,
              "GS phantom logical slots remain disabled");

        // A same-cycle direct fault drops ownership without waiting for state.
        machine_id_fault = 1'b1;
        #1;
        check(machine_identity_fault && !machine_identity_iigs &&
              !machine_inh_allowed,
              "identity fault drops the direct ownership verdict");
        check(physical_slot_allowed_mask == 8'h00,
              "identity fault clears every physical slot grant");
        check(machine_gs_m2_qualify && !effective_m2sel_active_high,
              "faulted ID4 keeps active-low GS qualification");

        machine_id_fault = 1'b0;
        locked_machine_id = 4'd2;
        bootstrap_identity_valid = 1'b0;
        bootstrap_identity_iigs = 1'b0;
        iigs_external_slot_mask_valid = 1'b0;
        requested_machine_mode = 2'd2;
        requested_m2sel_active_high = 1'b1;
        #1;
        check(!machine_identity_legacy && !machine_inh_allowed &&
              physical_slot_allowed_mask == 8'h00,
              "unproven legacy ID cannot grant physical rights");
        check(!effective_m2sel_active_high,
              "unproven legacy ID cannot invert pin 39");

        bootstrap_identity_valid = 1'b1;
        bootstrap_identity_legacy = 1'b1;
        #1;
        check(machine_identity_legacy && machine_inh_allowed &&
              physical_slot_allowed_mask == 8'hFE,
              "trusted legacy provenance is required for legacy rights");

        locked_machine_id = 4'd1;
        #1;
        check(machine_is_iiplus, "ID1 selects II/II+ behavior");

        bootstrap_identity_legacy = 1'b0;
        bootstrap_identity_iigs = 1'b1;
        locked_machine_id = 4'd2;
        #1;
        check(machine_identity_fault && !machine_inh_allowed &&
              physical_slot_allowed_mask == 8'h00,
              "classifier/report contradiction faults closed");

        if (failures != 0)
            $fatal(1, "APPLE MACHINE SAFETY POLICY FAIL: %0d checks", failures);
        $display("APPLE MACHINE SAFETY POLICY PASS");
        $finish;
    end

endmodule
