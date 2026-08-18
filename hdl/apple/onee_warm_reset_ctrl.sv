`timescale 1ns / 1ps

// Virtual-only warm-reset pulse stretcher for ONE//e.
//
// The PS input bridge holds request until acknowledge. This block asserts the
// virtual motherboard RESET line for complete native Apple bus cycles, then
// acknowledges the request. It has no connection to the physical RESET pin.
module onee_warm_reset_ctrl #(
    parameter integer MIN_NATIVE_CYCLES = 8
) (
    input  logic clk,
    input  logic resetn,
    input  logic enabled,
    input  logic request,
    input  logic native_cycle_tick,
    output logic virtual_res_n,
    output logic acknowledge,
    output logic active
);

    localparam integer COUNT_WIDTH =
        (MIN_NATIVE_CYCLES < 2) ? 1 : $clog2(MIN_NATIVE_CYCLES + 1);

    logic                   active_q;
    logic [COUNT_WIDTH-1:0] native_cycle_count_q;

    always_comb begin
        active = resetn && enabled && active_q;
        virtual_res_n = !active;
        acknowledge = active && request &&
                      (native_cycle_count_q ==
                       COUNT_WIDTH'(MIN_NATIVE_CYCLES));
    end

    always_ff @(posedge clk) begin
        if (!resetn || !enabled) begin
            active_q             <= 1'b0;
            native_cycle_count_q <= '0;
        end else if (!active_q) begin
            native_cycle_count_q <= '0;
            if (request)
                active_q <= 1'b1;
        end else if (!request) begin
            // The bridge drops request after it samples acknowledge.
            active_q             <= 1'b0;
            native_cycle_count_q <= '0;
        end else if (native_cycle_tick &&
                     (native_cycle_count_q <
                      COUNT_WIDTH'(MIN_NATIVE_CYCLES))) begin
            native_cycle_count_q <= native_cycle_count_q + COUNT_WIDTH'(1);
        end
    end

endmodule
