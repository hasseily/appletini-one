`timescale 1ns / 1ps

module tb_apple_machine_policy_latch;
    logic clk = 1'b0;
    always #3.75 clk = ~clk;
    logic resetn = 1'b0;
    logic host_policy_resetn = 1'b0;
    logic onee_enable_effective = 1'b0;
    logic machine_id_fault = 1'b0;
    logic iigs_slot_policy_fault = 1'b0;
    logic decoded_legacy = 1'b0;
    logic decoded_iigs = 1'b0;
    logic decoded_iiplus = 1'b0;
    logic [7:0] decoded_slot_mask = 8'h00;
    logic machine_identity_legacy;
    logic machine_identity_iigs;
    logic machine_is_iiplus;
    logic [7:0] physical_slot_allowed_mask;
    // This is the caller's direct physical-output isolation boundary. The
    // bus-wrapper tests verify that it kills even previously staged replies.
    wire physical_output_kill = !resetn || !host_policy_resetn ||
        onee_enable_effective || machine_id_fault || iigs_slot_policy_fault;

    apple_machine_policy_latch dut (.*);

    task automatic tick;
        @(posedge clk);
        #1;
        @(negedge clk);
    endtask

    task automatic require(input logic condition, input string message);
        if (condition !== 1'b1)
            $fatal(1, "%s", message);
    endtask

    task automatic quiet;
        require(physical_output_kill ||
                (!machine_identity_legacy && !machine_identity_iigs &&
                 !machine_is_iiplus && physical_slot_allowed_mask == 8'h00),
                "revoked or unknown policy must grant nothing");
    endtask

    initial begin
        tick();
        resetn = 1'b1;
        host_policy_resetn = 1'b1;
        tick();
        quiet();
        decoded_legacy = 1'b1;
        decoded_slot_mask = 8'hFE;
        #1;
        quiet(); // A fresh grant must pass the register first.
        tick();
        require(machine_identity_legacy && !machine_identity_iigs &&
                physical_slot_allowed_mask == 8'hFE, "IIe policy grant");
        machine_id_fault = 1'b1;
        #1;
        quiet(); // Revoke before another clock edge.
        tick();
        require(!machine_identity_legacy && physical_slot_allowed_mask == 0,
                "internal policy clears on the next clock after a fault");
        resetn = 1'b0;
        decoded_legacy = 1'b0;
        decoded_iigs = 1'b1;
        decoded_slot_mask = 8'h80;
        machine_id_fault = 1'b0;
        tick();
        resetn = 1'b1;
        tick();
        require(!machine_identity_legacy && machine_identity_iigs &&
                !machine_is_iiplus && physical_slot_allowed_mask == 8'h80,
                "GS policy permits only slot 7 and no ownership");
        iigs_slot_policy_fault = 1'b1;
        #1;
        quiet();
        tick();
        host_policy_resetn = 1'b0;
        iigs_slot_policy_fault = 1'b0;
        decoded_iigs = 1'b0;
        decoded_slot_mask = 8'h00;
        #1;
        quiet();
        tick();
        host_policy_resetn = 1'b1;
        tick();
        quiet();
        decoded_legacy = 1'b1;
        decoded_slot_mask = 8'hFE;
        tick();
        onee_enable_effective = 1'b1;
        #1;
        quiet();
        tick();
        decoded_legacy = 1'b0;
        decoded_slot_mask = 8'h00;
        onee_enable_effective = 1'b0;
        #1;
        quiet();
        tick();
        quiet();
        $display("APPLE MACHINE POLICY LATCH PASS");
        $finish;
    end
endmodule
