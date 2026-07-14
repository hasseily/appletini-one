`timescale 1ns / 1ps

module axi3_read_arbiter_2 (
    input logic clk,
    input logic rstn,

    Axi3_read_if.slave  in0,
    Axi3_read_if.slave  in1,
    Axi3_read_if.master out
);

    localparam logic S_IDLE = 1'b0;
    localparam logic S_R    = 1'b1;

    logic state_q;
    logic active_q;

    wire grant0 = (state_q == S_IDLE) && in0.arvalid;
    wire grant1 = (state_q == S_IDLE) && !in0.arvalid && in1.arvalid;

    always_comb begin
        out.araddr  = grant0 ? in0.araddr  : (grant1 ? in1.araddr  : '0);
        out.arlen   = grant0 ? in0.arlen   : (grant1 ? in1.arlen   : '0);
        out.arsize  = grant0 ? in0.arsize  : (grant1 ? in1.arsize  : '0);
        out.arburst = grant0 ? in0.arburst : (grant1 ? in1.arburst : '0);
        out.arvalid = grant0 || grant1;
        out.rready  = (state_q == S_R) ? (active_q ? in1.rready : in0.rready) : 1'b0;

        in0.arready = grant0 && out.arready;
        in1.arready = grant1 && out.arready;

        in0.rdata  = out.rdata;
        in0.rresp  = out.rresp;
        in0.rlast  = out.rlast;
        in0.rvalid = (state_q == S_R) && !active_q && out.rvalid;

        in1.rdata  = out.rdata;
        in1.rresp  = out.rresp;
        in1.rlast  = out.rlast;
        in1.rvalid = (state_q == S_R) && active_q && out.rvalid;
    end

    always_ff @(posedge clk) begin
        if (!rstn) begin
            state_q  <= S_IDLE;
            active_q <= 1'b0;
        end else begin
            unique case (state_q)
                S_IDLE: begin
                    if ((grant0 || grant1) && out.arready) begin
                        active_q <= grant1;
                        state_q <= S_R;
                    end
                end

                S_R: begin
                    if (out.rvalid && out.rready && out.rlast)
                        state_q <= S_IDLE;
                end

                default: state_q <= S_IDLE;
            endcase
        end
    end

endmodule
