`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// disk2_ddr_bridge -- serves disk2_card's line-oriented staging memory from
// DDR over an AXI3 HP port. DDR keeps all 8 MB of PSRAM available to RamWorks,
// and AXI WSTRB performs partial-line byte merging in the DDR controller.
//
// Client protocol:
//   mc_valid high with {line_addr, rw(1=read), wdata, wstrb} -> one-cycle
//   mc_ready accept pulse -> one-cycle mc_rvalid completion pulse (rdata
//   valid for reads). One outstanding op; the Disk II byte cadence is
//   ~32us, so latency is irrelevant -- simplicity wins.
//
// A line is 8 bytes = exactly one 64-bit AXI beat; DDR address =
// DDR_BASE + line*8. The staging region is 1 MB at 0x3D800000 (see
// disk2_service.h DISK2_DDR_STAGING_BASE -- the PS stages with plain
// memcpy/f_read and flushes dcache, or maps the MB non-cacheable).
//////////////////////////////////////////////////////////////////////////////////

module disk2_ddr_bridge #(
    parameter logic [31:0] DDR_BASE = 32'h3D80_0000
)(
    input  logic        clk,
    input  logic        rstn,

    // disk2_card line port
    input  logic [20:0] mc_line_addr,
    input  logic        mc_rw,          // 1 = read, 0 = write
    input  logic [63:0] mc_wdata,
    input  logic [7:0]  mc_wstrb,
    input  logic        mc_valid,
    output logic        mc_ready,       // one-cycle accept pulse
    output logic [63:0] mc_rdata,
    output logic        mc_rvalid,      // one-cycle completion pulse

    Axi3_read_if.master  axi_read,
    Axi3_write_if.master axi_write
);

    typedef enum logic [2:0] {
        B_IDLE,
        B_AR,           // read address handshake
        B_R,            // read data
        B_AW,           // write address handshake
        B_W,            // write data
        B_B             // write response
    } bstate_e;

    bstate_e     bst;
    logic [31:0] addr_q;
    logic [63:0] wdata_q;
    logic [7:0]  wstrb_q;

    always_ff @(posedge clk) begin
        if (!rstn) begin
            bst        <= B_IDLE;
            mc_ready   <= 1'b0;
            mc_rvalid  <= 1'b0;
            mc_rdata   <= 64'd0;
            addr_q     <= 32'd0;
            wdata_q    <= 64'd0;
            wstrb_q    <= 8'd0;
            axi_read.arvalid  <= 1'b0;
            axi_read.rready   <= 1'b0;
            axi_write.awvalid <= 1'b0;
            axi_write.wvalid  <= 1'b0;
            axi_write.bready  <= 1'b0;
        end else begin
            mc_ready  <= 1'b0;
            mc_rvalid <= 1'b0;
            case (bst)
                B_IDLE: if (mc_valid) begin
                    mc_ready <= 1'b1;
                    addr_q   <= DDR_BASE + {8'b0, mc_line_addr, 3'b000};
                    wdata_q  <= mc_wdata;
                    wstrb_q  <= mc_wstrb;
                    if (mc_rw) begin
                        axi_read.arvalid <= 1'b1;
                        bst <= B_AR;
                    end else begin
                        axi_write.awvalid <= 1'b1;
                        bst <= B_AW;
                    end
                end
                B_AR: if (axi_read.arready && axi_read.arvalid) begin
                    axi_read.arvalid <= 1'b0;
                    axi_read.rready  <= 1'b1;
                    bst <= B_R;
                end
                B_R: if (axi_read.rvalid) begin
                    mc_rdata  <= axi_read.rdata;
                    mc_rvalid <= 1'b1;
                    axi_read.rready <= 1'b0;
                    bst <= B_IDLE;
                end
                B_AW: if (axi_write.awready && axi_write.awvalid) begin
                    axi_write.awvalid <= 1'b0;
                    axi_write.wvalid  <= 1'b1;
                    bst <= B_W;
                end
                B_W: if (axi_write.wready && axi_write.wvalid) begin
                    axi_write.wvalid <= 1'b0;
                    axi_write.bready <= 1'b1;
                    bst <= B_B;
                end
                B_B: if (axi_write.bvalid) begin
                    axi_write.bready <= 1'b0;
                    mc_rvalid <= 1'b1;      // write completion pulse
                    bst <= B_IDLE;
                end
                default: bst <= B_IDLE;
            endcase
        end
    end

    // single-beat, 64-bit, INCR
    assign axi_read.araddr   = addr_q;
    assign axi_read.arlen    = 4'd0;
    assign axi_read.arsize   = 3'd3;
    assign axi_read.arburst  = 2'b01;
    assign axi_write.awaddr  = addr_q;
    assign axi_write.awlen   = 4'd0;
    assign axi_write.awsize  = 3'd3;
    assign axi_write.awburst = 2'b01;
    assign axi_write.wdata   = wdata_q;
    assign axi_write.wstrb   = wstrb_q;
    assign axi_write.wlast   = 1'b1;

endmodule
