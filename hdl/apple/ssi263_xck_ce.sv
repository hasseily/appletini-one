`timescale 1ns / 1ps

// Rational clock-enable source for the SSI-263 XCK pin.
//
// Keep XCK in the fabric clock domain.  The accumulator emits evenly spaced
// one-cycle enables and never creates a generated FPGA clock.  The default
// 3,579,545 / 2 Hz pin rate is the colorburst-derived Phasor candidate; an AP
// core with DIV2 high sees an effective 894,886.25 Hz time base.
module ssi263_xck_ce #(
    // The Zynq clock wizard resolves the requested fabric clock to this rate.
    parameter longint unsigned FABRIC_HZ = 133_333_344,
    parameter longint unsigned XCK_NUMERATOR_HZ = 3_579_545,
    parameter longint unsigned XCK_DENOMINATOR = 2
) (
    input  logic clk,
    input  logic rstn,
    output logic xck_ce
);

    localparam longint unsigned MODULUS = FABRIC_HZ * XCK_DENOMINATOR;
    localparam int unsigned ACC_WIDTH = (MODULUS <= 1) ? 1 : $clog2(MODULUS);

    logic [ACC_WIDTH-1:0] accumulator_q;
    logic [ACC_WIDTH:0] accumulator_sum;

    initial begin
        if (FABRIC_HZ == 0 || XCK_DENOMINATOR == 0 ||
            XCK_NUMERATOR_HZ == 0 || XCK_NUMERATOR_HZ >= MODULUS) begin
            $error("Invalid SSI-263 XCK rational clock parameters");
        end
    end

    always_comb begin
        accumulator_sum = {1'b0, accumulator_q} + XCK_NUMERATOR_HZ;
    end

    always_ff @(posedge clk) begin
        if (!rstn) begin
            accumulator_q <= '0;
            xck_ce <= 1'b0;
        end else if (accumulator_sum >= MODULUS) begin
            accumulator_q <= accumulator_sum - MODULUS;
            xck_ce <= 1'b1;
        end else begin
            accumulator_q <= accumulator_sum[ACC_WIDTH-1:0];
            xck_ce <= 1'b0;
        end
    end

endmodule
