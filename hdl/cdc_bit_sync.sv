`timescale 1ns / 1ps

// Single-bit clock-domain crossing helper (level synchronizer).
// This is a 2-FF sampled level synchronizer intended for control/status bits.
// It is not suitable for pulse transfer without pulse stretching/toggle logic.

module cdc_bit_sync (
    input  wire clk,
    input  wire resetn,
    input  wire din,
    output wire dout
);

    (* ASYNC_REG = "TRUE" *) reg sync_meta = 1'b0;
    (* ASYNC_REG = "TRUE" *) reg sync_ff   = 1'b0;

    always @(posedge clk) begin
        if (!resetn) begin
            sync_meta <= 1'b0;
            sync_ff   <= 1'b0;
        end else begin
            sync_meta <= din;
            sync_ff   <= sync_meta;
        end
    end

    assign dout = sync_ff;

endmodule
