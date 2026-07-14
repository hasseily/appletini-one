`timescale 1ns / 1ps

module irq_pulse_stretcher (
    input  logic clk,
    input  logic rstn,
    input  logic irq_pulse,
    output logic irq_pulse_stretched
);

    logic [3:0] count;

    always_ff @(posedge clk) begin
        if (!rstn) begin
            count <= 0;
            irq_pulse_stretched <= 0;
        end else if (irq_pulse) begin
            count <= 4'd9;
            irq_pulse_stretched <= 1;
        end else if (count != 0) begin
            count <= count - 1;
            irq_pulse_stretched <= 1;
        end else begin
            irq_pulse_stretched <= 0;
        end
    end

endmodule
