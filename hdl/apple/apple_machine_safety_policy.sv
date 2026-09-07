`timescale 1ns / 1ps

/* Direct physical-bus safety policy from boot-ROM facts.
 *
 * PS registers may request a display mode and legacy pin-39 polarity, but
 * they cannot grant bus ownership, broaden IIgs slot ownership, or invert
 * active-low /M2SEL after ID 4 has latched. */
module apple_machine_safety_policy (
    input  logic [3:0] locked_machine_id,
    input  logic       machine_id_fault,
    input  logic       iigs_slot_policy_fault,
    input  logic [7:0] iigs_external_slot_mask,
    input  logic       iigs_external_slot_mask_valid,
    input  logic [1:0] requested_machine_mode,
    input  logic       requested_m2sel_active_high,
    output logic       machine_identity_reported,
    output logic       machine_identity_legacy,
    output logic       machine_identity_iigs,
    output logic       machine_identity_fault,
    output logic [7:0] physical_slot_allowed_mask,
    output logic       machine_inh_allowed,
    output logic       machine_gs_m2_qualify,
    output logic       machine_is_iiplus,
    output logic [1:0] effective_machine_mode,
    output logic       effective_m2sel_active_high
);

    always_comb begin
        machine_identity_fault = machine_id_fault || iigs_slot_policy_fault;
        machine_identity_reported = (locked_machine_id != 4'd0);
        // Only the boot ROM's locked physical-host report grants ownership.
        machine_identity_legacy =
            (locked_machine_id >= 4'd1) &&
            (locked_machine_id <= 4'd3) &&
            !machine_identity_fault;
        machine_identity_iigs =
            (locked_machine_id == 4'd4) && !machine_identity_fault;

        physical_slot_allowed_mask = 8'h00;
        if (machine_identity_legacy) begin
            physical_slot_allowed_mask = 8'hFE;
        end else if (machine_identity_iigs &&
                     iigs_external_slot_mask_valid &&
                     iigs_external_slot_mask[7]) begin
            /* Only the wired physical slot survives on GS. Other logical
             * slots lack live select inputs and cannot be proved external. */
            physical_slot_allowed_mask = 8'h80;
        end

        machine_inh_allowed = machine_identity_legacy;
        // Keep GS qualification active even after a later identity conflict.
        machine_gs_m2_qualify = (locked_machine_id == 4'd4);
        machine_is_iiplus = machine_identity_legacy &&
                            (locked_machine_id == 4'd1);

        effective_machine_mode = requested_machine_mode;
        effective_m2sel_active_high = requested_m2sel_active_high;
        if (machine_identity_fault) begin
            effective_machine_mode = 2'd0;
            effective_m2sel_active_high = 1'b0;
        end else if (locked_machine_id == 4'd4) begin
            effective_machine_mode = 2'd3;
            effective_m2sel_active_high = 1'b0;
        end else if (!machine_identity_legacy) begin
            effective_m2sel_active_high = 1'b0;
        end
    end

endmodule

// Stage the decoded host facts before they reach card decoders and the
// physical arbiter. The ROM locks its first ID until reset; later conflicting
// reports set sticky fault flops. The caller routes those faults directly to
// physical-output isolation; internal card policy clears on the next clock.
module apple_machine_policy_latch (
    input  logic       clk,
    input  logic       resetn,
    input  logic       host_policy_resetn,
    input  logic       onee_enable_effective,
    input  logic       machine_id_fault,
    input  logic       iigs_slot_policy_fault,
    input  logic       decoded_legacy,
    input  logic       decoded_iigs,
    input  logic       decoded_iiplus,
    input  logic [7:0] decoded_slot_mask,
    output logic       machine_identity_legacy,
    output logic       machine_identity_iigs,
    output logic       machine_is_iiplus,
    output logic [7:0] physical_slot_allowed_mask
);
    logic [10:0] policy_q = 11'd0;
    wire revoke = !resetn || !host_policy_resetn || onee_enable_effective ||
                  machine_id_fault || iigs_slot_policy_fault;

    always_ff @(posedge clk) begin
        if (revoke)
            policy_q <= 11'd0;
        else
            policy_q <= {decoded_slot_mask, decoded_iiplus,
                         decoded_iigs, decoded_legacy};
    end

    assign machine_identity_legacy = policy_q[0];
    assign machine_identity_iigs = policy_q[1];
    assign machine_is_iiplus = policy_q[2];
    assign physical_slot_allowed_mask = policy_q[10:3];
endmodule
