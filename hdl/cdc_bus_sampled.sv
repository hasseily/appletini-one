`timescale 1ns / 1ps

// Sampled multi-bit CDC helper.
// This uses a simple 2-FF vector sampler and does not guarantee coherency
// between bits. Use only for software-visible monitoring/status values where
// occasional intermediate samples are acceptable.

module cdc_bus_sampled #(
    parameter integer WIDTH = 32
) (
    input  wire             clk,
    input  wire             resetn,
    input  wire [WIDTH-1:0] din,
    output wire [WIDTH-1:0] dout
);

    (* ASYNC_REG = "TRUE" *) reg [WIDTH-1:0] sync_meta = {WIDTH{1'b0}};
    (* ASYNC_REG = "TRUE" *) reg [WIDTH-1:0] sync_ff   = {WIDTH{1'b0}};

    always @(posedge clk) begin
        if (!resetn) begin
            sync_meta <= {WIDTH{1'b0}};
            sync_ff   <= {WIDTH{1'b0}};
        end else begin
            sync_meta <= din;
            sync_ff   <= sync_meta;
        end
    end

    assign dout = sync_ff;

endmodule

// Pad-capture variant for the Apple bus inputs: stage 0 is forced into
// the pad's IOB register, so the pad-to-first-flop delay is fixed in
// silicon. It costs one clk of latency and must be applied to all Apple
// bus inputs together
// (including PHI0) so their relative alignment -- and therefore every
// TAP_*_SNAP constant -- is unchanged.
//
// Note: stage 0 carries no ASYNC_REG (Vivado refuses to pack ASYNC_REG
// registers into the IOB) and no reset (ILOGIC packing is simplest with
// a free-running FF; the sampled pins are asynchronous anyway).

module cdc_bus_iob #(
    parameter integer WIDTH = 32
) (
    input  wire             clk,
    input  wire             resetn,
    input  wire [WIDTH-1:0] din,
    output wire [WIDTH-1:0] dout
);

    (* IOB = "TRUE" *)       reg [WIDTH-1:0] iob_capture = {WIDTH{1'b0}};
    (* ASYNC_REG = "TRUE" *) reg [WIDTH-1:0] sync_meta   = {WIDTH{1'b0}};
    (* ASYNC_REG = "TRUE" *) reg [WIDTH-1:0] sync_ff     = {WIDTH{1'b0}};

    always @(posedge clk) begin
        iob_capture <= din;
    end

    always @(posedge clk) begin
        if (!resetn) begin
            sync_meta <= {WIDTH{1'b0}};
            sync_ff   <= {WIDTH{1'b0}};
        end else begin
            sync_meta <= iob_capture;
            sync_ff   <= sync_meta;
        end
    end

    assign dout = sync_ff;

endmodule
