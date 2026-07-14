// ps_dma_command: PS-facing register interface that commands the
// apple_dma_engine. The PS programs mc_addr / ddr_addr / length+rw
// and triggers a transfer by writing the length+rw register; a
// status register reports completion.
//
// Register map (AxiSimple, byte addresses are awaddr<<2):
//   0x00  MC_ADDR     [23:0]  : PSRAM byte address
//   0x01  DDR_ADDR    [31:0]  : DDR byte address (8-byte aligned)
//   0x02  LENGTH_RW   [31]=rw, [15:0]=length
//                              : writing this register kicks off a DMA
//                                with the currently-latched values
//   0x03  STATUS      [0]=ps_cmd_complete
//                              : 0 until apple_dma_engine signals done,
//                                then 1 until consumed by a PS read.
//
// rw=1 transfers DDR to PSRAM; rw=0 transfers PSRAM to DDR.

module ps_dma_command (
    input  logic                     clk,
    input  logic                     rstn,

    input  globals::AxiSimple_common as_common,
    AxiSimple_if.client              as_client,

    // Client interface to apple_dma_engine
    output logic [23:0]              dma_req_mc_addr,
    output logic [31:0]              dma_req_ddr_addr,
    output logic [15:0]              dma_req_length,
    output logic                     dma_req_rw,
    output logic                     dma_req_valid,
    input  logic                     dma_req_ready,
    input  logic                     dma_req_done
);

    localparam logic [7:0] REG_MC_ADDR   = 8'h00;
    localparam logic [7:0] REG_DDR_ADDR  = 8'h01;
    localparam logic [7:0] REG_LENGTH_RW = 8'h02;
    localparam logic [7:0] REG_STATUS    = 8'h03;

    logic [23:0] mc_addr_q;
    logic [31:0] ddr_addr_q;
    logic [15:0] length_q;
    logic        rw_q;
    logic        req_valid_q;
    logic        ps_cmd_complete_q;

    // Detect a read of STATUS by tracking the prior araddr. axidouble's
    // addrdecode advances araddr the cycle after a read fires (see the
    // comment in apple_top), so consecutive reads always go through a
    // non-STATUS araddr value between them.
    logic [7:0]  araddr_prev_q;
    wire         status_read_pulse =
        (as_common.araddr == REG_STATUS) &&
        (araddr_prev_q    != REG_STATUS);

    wire [31:0] mc_addr_word    = {8'h00, mc_addr_q};
    wire [31:0] length_rw_word  = {rw_q, 15'h0, length_q};

    wire [31:0] mc_addr_next    = globals::apply_wstrb(mc_addr_word,   as_common.wdata, as_common.wstrb);
    wire [31:0] ddr_addr_next   = globals::apply_wstrb(ddr_addr_q,     as_common.wdata, as_common.wstrb);
    wire [31:0] length_rw_next  = globals::apply_wstrb(length_rw_word, as_common.wdata, as_common.wstrb);

    assign dma_req_mc_addr  = mc_addr_q;
    assign dma_req_ddr_addr = ddr_addr_q;
    assign dma_req_length   = length_q;
    assign dma_req_rw       = rw_q;
    assign dma_req_valid    = req_valid_q;

    // Registered rdata mux. axidouble samples rdata one cycle after araddr
    // is presented, so the read path needs a matching one-cycle latency
    // (same pattern as apple_top's debug-register mux).
    logic [31:0] as_client_rdata_q;
    always_ff @(posedge clk) begin
        case (as_common.araddr)
            REG_MC_ADDR:   as_client_rdata_q <= mc_addr_word;
            REG_DDR_ADDR:  as_client_rdata_q <= ddr_addr_q;
            REG_LENGTH_RW: as_client_rdata_q <= length_rw_word;
            REG_STATUS:    as_client_rdata_q <= {31'h0, ps_cmd_complete_q};
            default:       as_client_rdata_q <= 32'h0000_0000;
        endcase
    end
    assign as_client.rdata = as_client_rdata_q;

    always_ff @(posedge clk) begin
        if (!rstn) begin
            mc_addr_q         <= '0;
            ddr_addr_q        <= '0;
            length_q          <= '0;
            rw_q              <= 1'b0;
            req_valid_q       <= 1'b0;
            ps_cmd_complete_q <= 1'b0;
            araddr_prev_q     <= 8'hFF;
        end else begin
            araddr_prev_q <= as_common.araddr;

            if (req_valid_q && dma_req_ready)
                req_valid_q <= 1'b0;

            if (dma_req_done)
                ps_cmd_complete_q <= 1'b1;
            else if (status_read_pulse)
                ps_cmd_complete_q <= 1'b0;

            if (as_client.awvalid) begin
                case (as_common.awaddr)
                    REG_MC_ADDR:   mc_addr_q  <= mc_addr_next[23:0];
                    REG_DDR_ADDR:  ddr_addr_q <= ddr_addr_next;
                    REG_LENGTH_RW: begin
                        length_q          <= length_rw_next[15:0];
                        rw_q              <= length_rw_next[31];
                        req_valid_q       <= 1'b1;
                        ps_cmd_complete_q <= 1'b0;
                    end
                    default: ;
                endcase
            end
        end
    end

endmodule
