// Test-only functional models of the Xilinx LUT primitives used by the
// Apple bus wrapper. INIT bit order matches UNISIM; no timing is modeled.
module LUT6 #(parameter [63:0] INIT = 64'd0) (
    input I0, I1, I2, I3, I4, I5,
    output O
);
    assign O = INIT[{I5, I4, I3, I2, I1, I0}];
endmodule

module LUT2 #(parameter [3:0] INIT = 4'd0) (
    input I0, I1,
    output O
);
    assign O = INIT[{I1, I0}];
endmodule
