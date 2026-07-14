`timescale 1ns / 1ps

// Reset synchronizer (async assert, sync deassert).
// Use this to derive a local active-low reset for a clock domain from an
// asynchronous/global reset condition (and optional lock gating).

module reset_sync #(
    parameter integer STAGES = 2
) (
    input  wire clk,
    input  wire arst_n,
    output wire srst_n
);

    (* ASYNC_REG = "TRUE" *) reg [STAGES-1:0] sync_ff = {STAGES{1'b0}};

    always @(posedge clk or negedge arst_n) begin
        if (!arst_n)
            sync_ff <= {STAGES{1'b0}};
        else
            sync_ff <= {sync_ff[STAGES-2:0], 1'b1};
    end

    assign srst_n = sync_ff[STAGES-1];

endmodule
