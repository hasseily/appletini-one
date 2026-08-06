`timescale 1ns / 1ps

/* CPU0 access sequencer for vTW shadow port B.
 *
 * Scalar access keeps the boot/debug contract: set an address, read the
 * fetched byte, or write one byte and advance. Packed writes use a two-word
 * queue and drain four little-endian bytes per accepted word. A new address
 * or scalar write cancels packed work, which lets the SmartPort fallback
 * path stop a partial direct copy before it resumes the 6502. */
module vtw_shadow_host_port (
    input  logic        clk,
    input  logic        rstn,

    input  logic        addr_set,
    input  logic [17:0] addr_value,
    input  logic        byte_write,
    input  logic [7:0]  byte_wdata,
    input  logic        word_write,
    input  logic [31:0] word_wdata,
    input  logic        word_read,

    output logic [17:0] pointer,
    output logic [7:0]  read_data,
    output logic        word_ready,
    output logic        word_busy,
    output logic [29:0] word_accept_count,
    output logic [31:0] word_read_data,
    output logic        word_read_ready,
    output logic        word_read_busy,
    output logic [29:0] word_read_count,

    output logic        sh_en,
    output logic [17:0] sh_addr,
    output logic        sh_we,
    output logic [7:0]  sh_wdata,
    input  logic [7:0]  sh_rdata
);

    typedef enum logic [2:0] {
        SH_IDLE,
        SH_BYTE_WRITE,
        SH_READ,
        SH_CAPTURE,
        SH_WORD_WRITE,
        SH_WORD_READ,
        SH_WORD_CAPTURE
    } sh_state_t;

    sh_state_t state_q;
    logic [7:0] byte_wdata_q;
    logic [31:0] word_fifo_q [0:1];
    logic [31:0] word_active_q;
    logic        word_wr_q;
    logic        word_rd_q;
    logic [1:0]  word_count_q;
    logic [1:0]  word_lane_q;
    logic [1:0]  word_read_lane_q;

    wire word_push = word_write && !addr_set && !byte_write &&
                     (word_count_q != 2'd2);
    wire word_pop = (state_q == SH_WORD_WRITE) &&
                    (word_lane_q == 2'd3);

    assign word_ready = (word_count_q != 2'd2);
    assign word_busy = (word_count_q != 2'd0) ||
                       (state_q == SH_WORD_WRITE);
    assign word_read_ready = (state_q == SH_IDLE) &&
                             (word_count_q == 2'd0);
    assign word_read_busy = (state_q == SH_WORD_READ) ||
                            (state_q == SH_WORD_CAPTURE);
    wire word_read_fire = word_read && word_read_ready &&
                          !addr_set && !byte_write && !word_write;

    assign sh_en = (state_q == SH_BYTE_WRITE) ||
                   (state_q == SH_READ) ||
                   (state_q == SH_WORD_WRITE) ||
                   (state_q == SH_WORD_READ);
    assign sh_addr = pointer;
    assign sh_we = (state_q == SH_BYTE_WRITE) ||
                   (state_q == SH_WORD_WRITE);
    always_comb begin
        sh_wdata = byte_wdata_q;
        unique case (word_lane_q)
            2'd0: sh_wdata = word_active_q[7:0];
            2'd1: sh_wdata = word_active_q[15:8];
            2'd2: sh_wdata = word_active_q[23:16];
            2'd3: sh_wdata = word_active_q[31:24];
        endcase
        if (state_q != SH_WORD_WRITE) begin
            sh_wdata = byte_wdata_q;
        end
    end

    always_ff @(posedge clk) begin
        if (!rstn) begin
            state_q          <= SH_IDLE;
            pointer          <= 18'd0;
            read_data        <= 8'd0;
            byte_wdata_q     <= 8'd0;
            word_fifo_q[0]   <= 32'd0;
            word_fifo_q[1]   <= 32'd0;
            word_active_q    <= 32'd0;
            word_wr_q        <= 1'b0;
            word_rd_q        <= 1'b0;
            word_count_q     <= 2'd0;
            word_lane_q      <= 2'd0;
            word_read_lane_q <= 2'd0;
            word_accept_count <= 30'd0;
            word_read_data   <= 32'd0;
            word_read_count  <= 30'd0;
        end else begin
            if (word_push) begin
                word_fifo_q[word_wr_q] <= word_wdata;
                word_wr_q <= ~word_wr_q;
                word_accept_count <= word_accept_count + 30'd1;
            end
            if (word_pop) begin
                word_rd_q <= ~word_rd_q;
            end
            unique case ({word_push, word_pop})
                2'b10: word_count_q <= word_count_q + 2'd1;
                2'b01: word_count_q <= word_count_q - 2'd1;
                default: ;
            endcase

            unique case (state_q)
                SH_IDLE: begin
                    if (word_count_q != 2'd0) begin
                        word_active_q <= word_fifo_q[word_rd_q];
                        word_lane_q   <= 2'd0;
                        state_q       <= SH_WORD_WRITE;
                    end
                end
                SH_BYTE_WRITE: begin
                    pointer <= pointer + 18'd1;
                    state_q <= SH_READ;
                end
                SH_READ: state_q <= SH_CAPTURE;
                SH_CAPTURE: begin
                    read_data <= sh_rdata;
                    state_q   <= SH_IDLE;
                end
                SH_WORD_WRITE: begin
                    pointer <= pointer + 18'd1;
                    if (word_lane_q == 2'd3) begin
                        state_q <= SH_IDLE;
                    end else begin
                        word_lane_q <= word_lane_q + 2'd1;
                    end
                end
                SH_WORD_READ: begin
                    state_q <= SH_WORD_CAPTURE;
                end
                SH_WORD_CAPTURE: begin
                    unique case (word_read_lane_q)
                        2'd0: word_read_data[7:0]   <= sh_rdata;
                        2'd1: word_read_data[15:8]  <= sh_rdata;
                        2'd2: word_read_data[23:16] <= sh_rdata;
                        2'd3: word_read_data[31:24] <= sh_rdata;
                    endcase
                    pointer <= pointer + 18'd1;
                    if (word_read_lane_q == 2'd3) begin
                        word_read_count <= word_read_count + 30'd1;
                        state_q <= SH_IDLE;
                    end else begin
                        word_read_lane_q <= word_read_lane_q + 2'd1;
                        state_q <= SH_WORD_READ;
                    end
                end
                default: state_q <= SH_IDLE;
            endcase

            if (word_read_fire) begin
                word_read_lane_q <= 2'd0;
                state_q <= SH_WORD_READ;
            end

            /* Scalar commands take priority and cancel queued packed data.
             * A byte already presented in this clock may land; fallback
             * rewrites the full destination, so that partial byte is safe. */
            if (addr_set) begin
                pointer      <= addr_value;
                state_q      <= SH_READ;
                word_wr_q    <= 1'b0;
                word_rd_q    <= 1'b0;
                word_count_q <= 2'd0;
            end else if (byte_write) begin
                byte_wdata_q <= byte_wdata;
                state_q      <= SH_BYTE_WRITE;
                word_wr_q    <= 1'b0;
                word_rd_q    <= 1'b0;
                word_count_q <= 2'd0;
            end
        end
    end

endmodule
