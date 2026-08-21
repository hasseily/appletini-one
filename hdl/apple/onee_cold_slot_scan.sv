`timescale 1ns / 1ps

// Cold-slot ordering for the isolated ONE//e motherboard.
//
// An Enhanced //e scans slot ROMs downward. A SmartPort boot must expose
// slot 7 from the first $C7xx probe. A Disk II boot hides slot 7 on ONE//e
// entry; the first $C6xx probe ends that hold so slot 7 remains available
// after the ROM hands control to Disk II.
module onee_cold_slot_scan (
    input  logic                  clk,
    input  logic                  resetn,
    input  logic                  enabled,
    input  logic                  manual_enable_request,
    input  logic                  boot_target_disk2,
    input  logic                  warm_reset_active,
    input  globals::AppleBus_read ab_read,
    output logic                  session_boot_target_disk2,
    output logic                  slot7_hidden
);

    logic enabled_q;
    logic request_q;
    wire slot6_probe =
        enabled && slot7_hidden &&
        ab_read.serve_en && ab_read.rw &&
        (ab_read.addr[15:8] == 8'hC6);

    always_ff @(posedge clk) begin
        if (!resetn) begin
            enabled_q                   <= 1'b0;
            request_q                   <= 1'b0;
            session_boot_target_disk2   <= 1'b0;
            slot7_hidden                <= 1'b0;
        end else begin
            enabled_q <= enabled;
            request_q <= manual_enable_request;
            if (manual_enable_request && !request_q)
                session_boot_target_disk2 <= boot_target_disk2;

            if (!enabled) begin
                slot7_hidden <= 1'b0;
            end else if (!enabled_q) begin
                slot7_hidden <= (manual_enable_request && !request_q) ?
                    boot_target_disk2 : session_boot_target_disk2;
            end else if (!ab_read.res && !warm_reset_active) begin
                // A cold restart publishes its boot choice while RESET is
                // low. The separate warm-reset controller marks private warm
                // resets, so they cannot restart this slot scan.
                session_boot_target_disk2 <= boot_target_disk2;
                slot7_hidden <= boot_target_disk2;
            end else if (slot6_probe)
                slot7_hidden <= 1'b0;
        end
    end

endmodule
