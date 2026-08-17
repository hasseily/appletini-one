`timescale 1ns / 1ps

/*
 * Bounded PS-to-W5100S block bridge.
 *
 * CPU0 stages write data or requests read data in chunks of at most 256
 * bytes. The bridge then feeds the existing single-byte Uthernet host port
 * without a PS round trip between bytes. Direct register commands remain on
 * the old path and cannot overlap a FIFO transfer.
 */
module uthernet2_host_fifo #(
    parameter int unsigned FIFO_DEPTH = 256
) (
    input  logic        clk,
    input  logic        rstn,

    input  logic        addr_we,
    input  logic [15:0] addr_wdata,
    input  logic        control_we,
    input  logic [31:0] control_wdata,
    input  logic        data_we,
    input  logic [31:0] data_wdata,
    input  logic        data_re,
    output logic [31:0] data_rdata,
    output logic [31:0] status,

    input  logic        direct_busy,
    input  logic        host_ready,
    input  logic        host_done,
    input  logic        host_error,
    input  logic [7:0]  host_rdata,
    output logic        host_req,
    output logic        host_write,
    output logic [15:0] host_addr,
    output logic [7:0]  host_wdata,
    output logic        busy
);

    localparam int unsigned FIFO_AW = $clog2(FIFO_DEPTH);
    localparam int unsigned FIFO_CW = FIFO_AW + 1;
    localparam logic [31:0] CTRL_START = 32'h8000_0000;
    localparam logic [31:0] CTRL_WRITE = 32'h4000_0000;
    localparam logic [31:0] CTRL_FLUSH = 32'h2000_0000;

    logic [7:0] write_fifo [0:FIFO_DEPTH-1];
    logic [7:0] read_fifo  [0:FIFO_DEPTH-1];
    logic [FIFO_AW-1:0] write_wr_q;
    logic [FIFO_AW-1:0] write_rd_q;
    logic [FIFO_AW-1:0] read_wr_q;
    logic [FIFO_AW-1:0] read_rd_q;
    logic [FIFO_CW-1:0] write_count_q;
    logic [FIFO_CW-1:0] read_count_q;

    logic [15:0] configured_addr_q;
    logic [15:0] current_addr_q;
    logic [FIFO_CW-1:0] remaining_q;
    logic active_q;
    logic inflight_q;
    logic write_q;
    logic done_q;
    logic error_q;

    wire [FIFO_CW-1:0] requested_length =
        {{(FIFO_CW-9){1'b0}}, control_wdata[8:0]};
    wire request_length_valid =
        requested_length != '0 && requested_length <= FIFO_DEPTH;
    wire [FIFO_CW-1:0] read_pop_count =
        (read_count_q >= 4) ? FIFO_CW'(4) : read_count_q;

    assign busy = active_q || inflight_q;
    assign host_req = active_q && !inflight_q && host_ready;
    assign host_write = write_q;
    assign host_addr = current_addr_q;
    assign host_wdata = write_fifo[write_rd_q];

    always_comb begin
        data_rdata = 32'h0000_0000;
        if (read_count_q > 0) begin
            data_rdata[7:0] = read_fifo[read_rd_q];
        end
        if (read_count_q > 1) begin
            data_rdata[15:8] = read_fifo[read_rd_q + FIFO_AW'(1)];
        end
        if (read_count_q > 2) begin
            data_rdata[23:16] = read_fifo[read_rd_q + FIFO_AW'(2)];
        end
        if (read_count_q > 3) begin
            data_rdata[31:24] = read_fifo[read_rd_q + FIFO_AW'(3)];
        end

        status = 32'h0000_0000;
        status[31] = busy;
        status[30] = done_q;
        status[29] = error_q;
        status[28] = write_q;
        status[26:18] = 9'(remaining_q);
        status[17:9] = 9'(read_count_q);
        status[8:0] = 9'(write_count_q);
    end

    always_ff @(posedge clk) begin
        if (!rstn) begin
            write_wr_q <= '0;
            write_rd_q <= '0;
            read_wr_q <= '0;
            read_rd_q <= '0;
            write_count_q <= '0;
            read_count_q <= '0;
            configured_addr_q <= 16'h0000;
            current_addr_q <= 16'h0000;
            remaining_q <= '0;
            active_q <= 1'b0;
            inflight_q <= 1'b0;
            write_q <= 1'b0;
            done_q <= 1'b0;
            error_q <= 1'b0;
        end else begin
            if (addr_we && !busy) begin
                configured_addr_q <= addr_wdata;
            end

            if (data_we && !busy) begin
                if (write_count_q <= FIFO_DEPTH - 4) begin
                    write_fifo[write_wr_q] <= data_wdata[7:0];
                    write_fifo[write_wr_q + FIFO_AW'(1)] <= data_wdata[15:8];
                    write_fifo[write_wr_q + FIFO_AW'(2)] <= data_wdata[23:16];
                    write_fifo[write_wr_q + FIFO_AW'(3)] <= data_wdata[31:24];
                    write_wr_q <= write_wr_q + FIFO_AW'(4);
                    write_count_q <= write_count_q + FIFO_CW'(4);
                end else begin
                    done_q <= 1'b1;
                    error_q <= 1'b1;
                end
            end

            if (data_re && !busy && read_count_q != 0) begin
                read_rd_q <= read_rd_q + FIFO_AW'(read_pop_count);
                read_count_q <= read_count_q - read_pop_count;
            end

            if (control_we && (control_wdata & CTRL_FLUSH) != 0) begin
                if (!busy) begin
                    write_wr_q <= '0;
                    write_rd_q <= '0;
                    read_wr_q <= '0;
                    read_rd_q <= '0;
                    write_count_q <= '0;
                    read_count_q <= '0;
                    remaining_q <= '0;
                    done_q <= 1'b0;
                    error_q <= 1'b0;
                end
            end else if (control_we &&
                         (control_wdata & CTRL_START) != 0) begin
                if (busy || direct_busy || !request_length_valid ||
                    (((control_wdata & CTRL_WRITE) != 0) &&
                     write_count_q < requested_length)) begin
                    done_q <= 1'b1;
                    error_q <= 1'b1;
                end else begin
                    active_q <= 1'b1;
                    inflight_q <= 1'b0;
                    write_q <= (control_wdata & CTRL_WRITE) != 0;
                    current_addr_q <= configured_addr_q;
                    remaining_q <= requested_length;
                    done_q <= 1'b0;
                    error_q <= 1'b0;
                    read_wr_q <= '0;
                    read_rd_q <= '0;
                    read_count_q <= '0;
                end
            end

            if (host_req) begin
                inflight_q <= 1'b1;
            end

            if (host_done && inflight_q) begin
                inflight_q <= 1'b0;
                if (host_error) begin
                    active_q <= 1'b0;
                    remaining_q <= '0;
                    write_wr_q <= '0;
                    write_rd_q <= '0;
                    write_count_q <= '0;
                    read_wr_q <= '0;
                    read_rd_q <= '0;
                    read_count_q <= '0;
                    done_q <= 1'b1;
                    error_q <= 1'b1;
                end else begin
                    current_addr_q <= current_addr_q + 16'h0001;
                    remaining_q <= remaining_q - FIFO_CW'(1);
                    if (write_q) begin
                        write_rd_q <= write_rd_q + FIFO_AW'(1);
                        write_count_q <= write_count_q - FIFO_CW'(1);
                    end else if (read_count_q < FIFO_DEPTH) begin
                        read_fifo[read_wr_q] <= host_rdata;
                        read_wr_q <= read_wr_q + FIFO_AW'(1);
                        read_count_q <= read_count_q + FIFO_CW'(1);
                    end else begin
                        active_q <= 1'b0;
                        remaining_q <= '0;
                        done_q <= 1'b1;
                        error_q <= 1'b1;
                    end

                    if (remaining_q == FIFO_CW'(1)) begin
                        active_q <= 1'b0;
                        remaining_q <= '0;
                        done_q <= 1'b1;
                        if (write_q) begin
                            write_wr_q <= '0;
                            write_rd_q <= '0;
                            write_count_q <= '0;
                        end
                    end
                end
            end
        end
    end

endmodule
