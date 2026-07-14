`timescale 1ns / 1ps

// SmartPort slot card with an a2retronet-style data port. The card never
// touches host memory: the 6502 transfers every byte through DATA and waits
// on CTRL while the PS services the request. This remains safe on hosts where
// the card is not allowed to assert INH or DMA.
//
// Apple-visible surface:
//   $Cn00-$CnFF : slot ROM (smartport_a2retronet_style_c700.mem)
//   $C800-$CFEF : expansion ROM (.._c800.mem), served only while
//                 soft_switch_manager says we own the C8 window
//   $CFF0 DATA  : read = pop response FIFO, write = push command FIFO
//   $CFF1 CTRL  : read  = {ready,7'b0} -- bit7 set once the PS has
//                         posted a response
//                 write = EXECUTE: firmware finished streaming the
//                         command (params, plus data for writes) into
//                         DATA; snapshot soft switches, raise the PS
//                         IRQ, clear ready
//   $CFFF       : never served (standard expansion-ROM release)
//
// Protocol (see 6502_SMARTPORT.S): the firmware streams command bytes
// into DATA, writes CTRL, then polls CTRL bit7. The PS drains the IN
// FIFO, executes against the disk image, pushes retcode + payload into
// the OUT FIFO and sets READY. The firmware then pops result bytes
// (RDBLOCK is 512 straight LDA DATAs). Block writes stream their 512
// bytes into DATA before the CTRL write.
//
// FIFO sizing: a write command = ~16 param bytes + 512 data, so 1K
// covers both directions with margin. Both are simple BRAM rings.
//
// PS AxiSimple map (word index):
//   0 STATUS  (RO): [10:0] in_count, [26:16] out_count,
//                   [28] exec_pending, [29] ready
//   1 IN_HEAD (RO): [7:0] oldest unpopped IN byte, [8] valid
//   2 OUT_PUSH(WO): pushes wdata[7:0] into the OUT FIFO
//   3 CONTROL (WO): [0] pop one IN byte     [1] clear IN FIFO
//                   [2] clear OUT FIFO      [3] set READY
//                   [4] ack exec_pending
//   4 SSS     (RO): soft-switch snapshot at EXECUTE for diagnostics
//
// When slot_assign == 0 the card is disabled: no decode fires, the
// AxiSimple mux returns zero, FIFOs hold their state.

module smartport_card (
    input  logic                     clk,
    input  logic                     rstn,
    input  globals::AppleBus_read    ab_read,
    input  globals::SoftSwitchState  sss,
    input  logic [2:0]               slot_assign,
    input  globals::AxiSimple_common as_common,
    AxiSimple_if.client              as_client,
    output globals::AppleBus_write   ab_write,
    output logic                     smartport_irq
);

    import globals::*;

    // ---- ROMs ----
    logic [7:0] slot_rom [0:255];
    logic [7:0] c8_rom   [0:2047];
    initial begin
        $readmemh("smartport_a2retronet_style_c700.mem", slot_rom);
        $readmemh("smartport_a2retronet_style_c800.mem", c8_rom);
    end

    // ---- FIFOs (BRAM rings) ----
    localparam int FIFO_AW = 10;                    // 1024 bytes each
    logic [7:0] in_fifo  [0:(1<<FIFO_AW)-1];
    logic [7:0] out_fifo [0:(1<<FIFO_AW)-1];
    logic [FIFO_AW-1:0] in_wr_q, in_rd_q, out_wr_q, out_rd_q;
    logic [FIFO_AW:0]   in_count_q, out_count_q;
    wire in_full   = in_count_q[FIFO_AW];
    wire out_full  = out_count_q[FIFO_AW];
    wire in_empty  = (in_count_q == '0);
    wire out_empty = (out_count_q == '0);

    // Registered BRAM head reads: rd pointers settle a cycle before
    // any consumer looks, and consumers are many cycles apart.
    logic [7:0] in_head_q;
    logic [7:0] out_head_q;
    always_ff @(posedge clk) begin
        in_head_q  <= in_fifo[in_rd_q];
        out_head_q <= out_fifo[out_rd_q];
    end

    logic ready_q;          // CTRL bit7: PS posted a response
    logic exec_pending_q;   // EXECUTE seen, PS has not acked
    /* Diagnostic: DATA reads that found the OUT FIFO empty (served
     * $00). Any nonzero here means the firmware popped more bytes
     * than the PS pushed for some command -- silent zero-fill of
     * block tails, invisible everywhere else. */
    logic [15:0] dry_pop_count_q;
    logic [7:0] ctrl_val_q; // value the firmware wrote to CTRL
                            // ($01 = ProDOS, $02 = SmartPort family)

    // ---- Soft-switch snapshot ----
    logic [20:0] sss_snapshot_q;
    wire [20:0] sss_snapshot_d = {
        sss.sw_ramworks_bank,
        sss.sw_lcram_write,
        sss.sw_lcram_read,
        sss.sw_lcram_bank2,
        sss.sw_dhires,
        sss.sw_80col,
        sss.sw_altcharset,
        sss.sw_hires,
        sss.sw_page2,
        sss.sw_mixed,
        sss.sw_text,
        sss.sw_altzp,
        sss.sw_ramwrt,
        sss.sw_ramrd,
        sss.sw_80store
    };

    // ---- Apple-bus decode ----
    wire configured = (slot_assign != 3'd0);
    wire apple_bus_enabled = configured && ab_read.res &&
                             ((slot_assign != 3'h3) || sss.sw_slotc3rom);

    wire slot_rom_hit = apple_bus_enabled && sss.slot_access &&
                        (ab_read.addr[15:12] == 4'hC) &&
                        (ab_read.addr[11] == 1'b0) &&
                        (ab_read.addr[10:8] == slot_assign);

    // C8 window, gated on ownership (soft_switch_manager latches the
    // owner on the $CnXX instruction fetch; $CFFF never served).
    wire c8_owner = apple_bus_enabled &&
                    sss.io_select[slot_assign] &&
                    (ab_read.addr[15:12] == 4'hC) &&
                    (ab_read.addr[11] == 1'b1) &&
                    (ab_read.addr[10:0] != 11'h7FF);

    wire data_reg_hit = c8_owner && (ab_read.addr[10:0] == 11'h7F0);
    wire ctrl_reg_hit = c8_owner && (ab_read.addr[10:0] == 11'h7F1);
    wire c8_rom_hit   = c8_owner && !data_reg_hit && !ctrl_reg_hit;

    wire ab_rom_read   = ab_read.sss_en  && ab_read.rw  && slot_rom_hit;
    wire ab_c8_read    = ab_read.sss_en  && ab_read.rw  && c8_rom_hit;
    wire ab_data_read  = ab_read.sss_en  && ab_read.rw  && data_reg_hit;
    wire ab_ctrl_read  = ab_read.sss_en  && ab_read.rw  && ctrl_reg_hit;
    wire ab_data_write = ab_read.data_en && !ab_read.rw && data_reg_hit;
    wire ab_ctrl_write = ab_read.data_en && !ab_read.rw && ctrl_reg_hit;

    // Synchronous ROM reads: addresses settle at the addr snap, serve
    // happens at sss_en one cycle later -- registered ROM data is
    // ready in time.
    logic [7:0] slot_rom_data_q;
    logic [7:0] c8_rom_data_q;
    always_ff @(posedge clk) begin
        slot_rom_data_q <= slot_rom[ab_read.addr[7:0]];
        c8_rom_data_q   <= c8_rom[ab_read.addr[10:0]];
    end

    // ---- AxiSimple decode ----
    localparam logic [7:0] SP_REG_STATUS   = 8'd0;
    localparam logic [7:0] SP_REG_IN_HEAD  = 8'd1;
    localparam logic [7:0] SP_REG_OUT_PUSH = 8'd2;
    localparam logic [7:0] SP_REG_CONTROL  = 8'd3;
    localparam logic [7:0] SP_REG_SSS      = 8'd4;

    wire axi_out_push = as_client.awvalid &&
                        (as_common.awaddr == SP_REG_OUT_PUSH) &&
                        as_common.wstrb[0];
    wire axi_ctrl_wr  = as_client.awvalid &&
                        (as_common.awaddr == SP_REG_CONTROL) &&
                        as_common.wstrb[0];
    wire axi_pop_in    = axi_ctrl_wr && as_common.wdata[0];
    wire axi_clear_in  = axi_ctrl_wr && as_common.wdata[1];
    wire axi_clear_out = axi_ctrl_wr && as_common.wdata[2];
    wire axi_set_ready = axi_ctrl_wr && as_common.wdata[3];
    wire axi_ack_exec  = axi_ctrl_wr && as_common.wdata[4];

    // ---- Bus write-side (serve) ----
    globals::AppleBus_write ab_write_d;
    globals::AppleBus_write ab_write_q;
    assign ab_write = ab_write_q;

    always_comb begin
        ab_write_d               = ab_write_q;
        ab_write_d.assert_inh    = 1'b0;
        ab_write_d.assert_res    = 1'b0;
        ab_write_d.assert_irq    = 1'b0;
        ab_write_d.assert_rdy    = 1'b0;
        ab_write_d.assert_nmi    = 1'b0;
        ab_write_d.assert_dma    = 1'b0;
        ab_write_d.wr_addr       = 16'h0000;
        ab_write_d.wr_rw         = 1'b0;
        ab_write_d.wr_addr_rw_en = 1'b0;

        if (ab_read.sss_en) begin
            if (ab_rom_read) begin
                ab_write_d.wr_data    = slot_rom_data_q;
                ab_write_d.wr_data_en = 1'b1;
            end
            else if (ab_c8_read) begin
                ab_write_d.wr_data    = c8_rom_data_q;
                ab_write_d.wr_data_en = 1'b1;
            end
            else if (ab_data_read) begin
                // Empty-pop returns $00; the firmware only reads DATA
                // after CTRL signalled ready, so this is a guard, not
                // a protocol case.
                ab_write_d.wr_data    = out_empty ? 8'h00 : out_head_q;
                ab_write_d.wr_data_en = 1'b1;
            end
            else if (ab_ctrl_read) begin
                ab_write_d.wr_data    = {ready_q, 7'b0000000};
                ab_write_d.wr_data_en = 1'b1;
            end
            else begin
                ab_write_d.wr_data    = 8'h00;
                ab_write_d.wr_data_en = 1'b0;
            end
        end
        else if (ab_read.data_en) begin
            ab_write_d.wr_data    = 8'h00;
            ab_write_d.wr_data_en = 1'b0;
        end
    end

    // ---- State ----
    always_ff @(posedge clk) begin
        if (!rstn) begin
            ab_write_q      <= '0;
            in_wr_q         <= '0;
            in_rd_q         <= '0;
            out_wr_q        <= '0;
            out_rd_q        <= '0;
            in_count_q      <= '0;
            out_count_q     <= '0;
            ready_q         <= 1'b0;
            exec_pending_q  <= 1'b0;
            ctrl_val_q      <= 8'd0;
            dry_pop_count_q <= 16'd0;
            sss_snapshot_q  <= 21'd0;
            smartport_irq   <= 1'b0;
        end else begin
            ab_write_q    <= ab_write_d;
            smartport_irq <= 1'b0;

            // Apple side ------------------------------------------------
            if (ab_data_write && !in_full) begin
                in_fifo[in_wr_q] <= ab_read.data;
                in_wr_q          <= in_wr_q + 1'b1;
            end

            if (ab_data_read && !out_empty) begin
                out_rd_q <= out_rd_q + 1'b1;
            end
            if (ab_data_read && out_empty &&
                (dry_pop_count_q != 16'hFFFF)) begin
                dry_pop_count_q <= dry_pop_count_q + 16'd1;
            end

            if (ab_ctrl_write) begin
                exec_pending_q <= 1'b1;
                ready_q        <= 1'b0;
                ctrl_val_q     <= ab_read.data;
                sss_snapshot_q <= sss_snapshot_d;
                smartport_irq  <= 1'b1;
            end

            // PS side ---------------------------------------------------
            if (axi_out_push && !out_full) begin
                out_fifo[out_wr_q] <= as_common.wdata[7:0];
                out_wr_q           <= out_wr_q + 1'b1;
            end

            if (axi_pop_in && !in_empty) begin
                in_rd_q <= in_rd_q + 1'b1;
            end

            if (axi_clear_in) begin
                in_rd_q <= '0;
                in_wr_q <= '0;
            end
            if (axi_clear_out) begin
                out_rd_q <= '0;
                out_wr_q <= '0;
            end
            if (axi_set_ready) ready_q        <= 1'b1;
            if (axi_ack_exec)  exec_pending_q <= 1'b0;

            // Unified counts (Apple and PS sides can move in the same
            // cycle; clears win).
            if (axi_clear_in) begin
                in_count_q <= '0;
            end else begin
                in_count_q <= in_count_q
                    + ((ab_data_write && !in_full) ? 1'b1 : 1'b0)
                    - ((axi_pop_in && !in_empty) ? 1'b1 : 1'b0);
            end
            if (axi_clear_out) begin
                out_count_q <= '0;
            end else begin
                out_count_q <= out_count_q
                    + ((axi_out_push && !out_full) ? 1'b1 : 1'b0)
                    - ((ab_data_read && !out_empty) ? 1'b1 : 1'b0);
            end
        end
    end

    // ---- AxiSimple read mux (registered araddr, axidouble timing) ----
    logic [7:0] axi_read_addr_q;
    always_ff @(posedge clk) begin
        axi_read_addr_q <= as_common.araddr;
    end

    always_comb begin
        if (!configured) begin
            as_client.rdata = 32'h0;
        end else begin
            case (axi_read_addr_q)
                SP_REG_STATUS: as_client.rdata = {
                    2'b00, ready_q, exec_pending_q, 1'b0,
                    out_count_q,            // [26:16]
                    5'b00000,
                    in_count_q              // [10:0]
                };
                SP_REG_CONTROL: as_client.rdata =
                    {8'h0, dry_pop_count_q, ctrl_val_q};
                SP_REG_IN_HEAD: as_client.rdata =
                    {23'h0, !in_empty, in_head_q};
                SP_REG_SSS: as_client.rdata = {11'h0, sss_snapshot_q};
                default: as_client.rdata = 32'h0;
            endcase
        end
    end

endmodule
