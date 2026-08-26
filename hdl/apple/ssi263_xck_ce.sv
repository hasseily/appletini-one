`timescale 1ns / 1ps

// Convert the physical Apple Q3 clock into a one-cycle fabric enable.
//
// The Phasor feeds Q3 to both SSI-263AP XCK pins. Each socket has DIV2
// asserted, so the SSI control code consumes every second enable. Keeping Q3
// as a clock enable avoids creating another FPGA clock domain.
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
            q3_sync1_q   <= 1'b0;
            q3_sync2_q   <= 1'b0;
            q3_sync2_d_q <= 1'b0;
        end else begin
            q3_sync1_q   <= q3_raw;
            q3_sync2_q   <= q3_sync1_q;
            q3_sync2_d_q <= q3_sync2_q;
        end
    end

endmodule
