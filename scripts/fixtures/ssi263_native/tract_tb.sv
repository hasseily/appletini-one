// Test-only visibility into every retained charge state. No debug ports are
// required on the synthesizable tract interface.
module tract_tb (
    input logic clk, rstn, event_valid,
    output logic event_ready,
    input logic phase, phase_edge,
    input logic [31:0] codes,
    input logic signed [23:0] voice_drive,
    input logic signed [17:0] fric_drive,
    input logic fric1_route, fric2_route, output_open,
    output logic busy, event_done,
    output logic signed [23:0] reconstruction,
    output logic signed [15:0] sample,
    output logic fault, state_saturated,
    output logic signed [31:0] debug_state [0:60]
);
    ssi263_native_tract #(.OUTPUT_GAIN(8)) dut (.*);
    always_comb begin
        debug_state[0]=32'(dut.voice_q);
        debug_state[1]=32'(dut.fric1_q);
        debug_state[2]=32'(dut.fric2_source_q);
        debug_state[3]=32'(dut.fric2_shape_q);
        for (integer k=0;k<4;k=k+1) begin
            debug_state[4+k]=32'(dut.voice_plates[k]);
            debug_state[8+k]=32'(dut.fric1_plates[k]);
            debug_state[12+k]=32'(dut.f2q_plates[k]);
            debug_state[16+k]=32'(dut.filter_plates[k]);
        end
        for (integer k=0;k<5;k=k+1) begin
            debug_state[20+k*7]=32'(dut.f_y[k]);
            debug_state[21+k*7]=32'(dut.f_p[k]);
            debug_state[22+k*7]=32'(dut.f_fixed[k]);
            for (integer j=0;j<4;j=j+1) debug_state[23+k*7+j]=32'(dut.f_plates[k][j]);
        end
        debug_state[55]=32'(dut.c143_plate_q);
        debug_state[56]=32'(dut.c151_plate_q);
        debug_state[57]=32'(dut.c150_delta_q);
        debug_state[58]=32'(dut.c151_delta_q);
        debug_state[59]=32'(dut.output_q);
        debug_state[60]=32'(dut.reconstruction);
    end
endmodule
