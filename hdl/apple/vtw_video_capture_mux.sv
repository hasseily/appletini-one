`timescale 1ns / 1ps

// One ordered capture ingress. CPU records already pending when a held-CPU
// copy starts retain priority. Both producers must hold their byte until
// ready; the capture FIFO owns reservation for physical I/O/frame records.
module vtw_video_capture_mux (
    input logic enabled,
    input logic cpu_valid,
    input logic [16:0] cpu_addr,
    input logic [7:0] cpu_data,
    output logic cpu_ready,
    input logic copy_valid,
    input logic [16:0] copy_addr,
    input logic [7:0] copy_data,
    output logic copy_ready,
    output logic capture_valid,
    output logic [16:0] capture_addr,
    output logic [7:0] capture_data,
    input logic capture_ready
);
    assign capture_valid = enabled && (cpu_valid || copy_valid);
    assign capture_addr = cpu_valid ? cpu_addr : copy_addr;
    assign capture_data = cpu_valid ? cpu_data : copy_data;
    assign cpu_ready = enabled && capture_ready;
    assign copy_ready = enabled && !cpu_valid && capture_ready;
endmodule
