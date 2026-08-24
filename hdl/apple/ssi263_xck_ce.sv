`timescale 1ns / 1ps

// Clock-enable source for the SSI-263 XCK pin.
//
// This Phasor profile feeds the physical Apple Q3 input to both SSI-263AP XCK
// pins.  Synchronize that input into the fabric domain, then emit one
// fabric-clock enable for each observed rising edge.  This keeps all SSI state
// on clk and creates no generated FPGA clock.  Original-card continuity still
// needs a board trace or scope check.
module ssi263_xck_ce (
    input  logic clk,
    input  logic rstn,
    input  logic q3_raw,
    output logic xck_ce
);

    (* ASYNC_REG = "TRUE" *) logic q3_sync1_q;
    (* ASYNC_REG = "TRUE" *) logic q3_sync2_q;
    logic q3_sync2_d_q;

    assign xck_ce = q3_sync2_q && !q3_sync2_d_q;

    always_ff @(posedge clk) begin
        if (!rstn) begin
            q3_sync1_q <= 1'b0;
            q3_sync2_q <= 1'b0;
            q3_sync2_d_q <= 1'b0;
        end else begin
            q3_sync1_q <= q3_raw;
            q3_sync2_q <= q3_sync1_q;
            q3_sync2_d_q <= q3_sync2_q;
        end
    end

endmodule
