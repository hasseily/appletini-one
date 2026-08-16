`timescale 1ns / 1ps

// Cold-slot ordering for the isolated ONE//e motherboard.
//
// An Enhanced //e scans slot ROMs downward. Hide slot 7 on each ONE//e
// entry so the first useful card is the virtual Disk II in slot 6. The C600
// ROM probe itself ends the hold; slot 7 may answer from the next cycle.
module onee_cold_slot_scan (
    input  logic                  clk,
    input  logic                  resetn,
    input  logic                  enabled,
    input  globals::AppleBus_read ab_read,
    output logic                  slot7_hidden
);

    logic enabled_q;
    wire slot6_probe =
        enabled && slot7_hidden &&
        ab_read.serve_en && ab_read.rw &&
        (ab_read.addr[15:8] == 8'hC6);

    always_ff @(posedge clk) begin
        if (!resetn) begin
            enabled_q    <= 1'b0;
            slot7_hidden <= 1'b0;
        end else begin
            enabled_q <= enabled;
            if (!enabled)
                slot7_hidden <= 1'b0;
            else if (!enabled_q)
                slot7_hidden <= 1'b1;
            else if (slot6_probe)
                slot7_hidden <= 1'b0;
        end
    end

endmodule
