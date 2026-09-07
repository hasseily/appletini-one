`timescale 1ns / 1ps

module tb_apple_machine_safety_policy;
    logic [3:0] locked_machine_id;
    logic machine_id_fault, iigs_slot_policy_fault;
    logic [7:0] iigs_external_slot_mask;
    logic iigs_external_slot_mask_valid;
    logic [1:0] requested_machine_mode;
    logic requested_m2sel_active_high;
    logic machine_identity_reported, machine_identity_legacy;
    logic machine_identity_iigs, machine_identity_fault;
    logic [7:0] physical_slot_allowed_mask;
    logic machine_inh_allowed, machine_gs_m2_qualify, machine_is_iiplus;
    logic [1:0] effective_machine_mode;
    logic effective_m2sel_active_high;
    apple_machine_safety_policy dut (.*);
    task automatic check(input logic condition, input string message);
        if (condition !== 1'b1) $fatal(1, "%s", message);
    endtask
    initial begin
        locked_machine_id = 0;
        machine_id_fault = 0;
        iigs_slot_policy_fault = 0;
        iigs_external_slot_mask = 8'hFF;
        iigs_external_slot_mask_valid = 1;
        for (int mode = 0; mode < 4; mode++) begin
            requested_machine_mode = mode;
            for (int polarity = 0; polarity < 2; polarity++) begin
                requested_m2sel_active_high = polarity;
                #1;
                check(!machine_identity_reported && !machine_identity_legacy &&
                      !machine_identity_iigs && !machine_inh_allowed &&
                      physical_slot_allowed_mask == 0,
                      "PS requests cannot grant unknown host physical rights");
                check(!machine_gs_m2_qualify && !effective_m2sel_active_high,
                      "unknown boot uses unqualified pin 39 without inversion");
            end
        end
        locked_machine_id = 4;
        iigs_external_slot_mask_valid = 0;
        #1;
        check(machine_identity_iigs && machine_gs_m2_qualify &&
              !machine_identity_legacy && !machine_inh_allowed &&
              physical_slot_allowed_mask == 0,
              "GS ID restricts cycles before the slot-mask report");
        for (int mask = 0; mask < 256; mask++) begin
            iigs_external_slot_mask = mask;
            iigs_external_slot_mask_valid = 1;
            requested_machine_mode = mask[1:0];
            requested_m2sel_active_high = mask[2];
            #1;
            check(physical_slot_allowed_mask == (mask[7] ? 8'h80 : 8'h00),
                  "GS slot mask never grants phantom slots");
            check(!machine_inh_allowed && !machine_identity_legacy &&
                  effective_machine_mode == 3 && !effective_m2sel_active_high,
                  "GS never gains ownership or inverted M2SEL from PS settings");
        end
        machine_id_fault = 1;
        #1;
        check(machine_identity_fault && !machine_inh_allowed &&
              physical_slot_allowed_mask == 0 && machine_gs_m2_qualify,
              "fault removes grants while preserving GS cycle qualification");
        machine_id_fault = 0;
        for (int id = 1; id <= 3; id++) begin
            locked_machine_id = id;
            #1;
            check(machine_identity_legacy && machine_inh_allowed &&
                  physical_slot_allowed_mask == 8'hFE && !machine_gs_m2_qualify,
                  "accepted legacy ROM IDs grant supported physical cards");
            check(machine_is_iiplus == (id == 1), "II+ mode follows ID1 only");
        end
        iigs_slot_policy_fault = 1;
        #1;
        check(machine_identity_fault && !machine_inh_allowed &&
              physical_slot_allowed_mask == 0,
              "slot-policy fault also revokes legacy grants");
        $display("APPLE MACHINE SAFETY POLICY PASS");
        $finish;
    end
endmodule