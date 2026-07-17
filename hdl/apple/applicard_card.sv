`timescale 1ns / 1ps

// PCPI Appli-Card compatible slot front end (Z80 coprocessor).
//
// The card is two one-byte latches plus two handshake flags; the Z80 itself
// (and its RAM/ROM) is emulated by the PS in applicard_service.c. This module
// owns everything the 6502 can observe so every $C0nX access is answered
// in-cycle, and exposes the latches/flags to the PS over AxiSimple.
//
//   TOZ80  latch + F_Z80  flag: 6502 -> Z80 direction
//   TO6502 latch + F_6502 flag: Z80 -> 6502 direction
//
// 6502 view ($C0n0-$C0nF, slot 5 => $C0D0-$C0DF):
//   +0 R: TO6502 latch, clears F_6502          +0 W: ignored
//   +1 R: TOZ80 latch readback (no effect)     +1 W: TOZ80 <= data, sets F_Z80
//   +2 R: bit7 = F_Z80                         +2 W: ignored
//   +3 R: bit7 = F_6502                        +3 W: ignored
//   +5 R/W: Z80 reset (clears latches+flags in-cycle, latches RESET_REQ)
//   +6 R/W: Z80 IRQ via CTC socket -- unpopulated on the real card, no-op
//   +7 R/W: Z80 NMI (latches NMI_REQ)
//   others: read $FF
//
// The PS consumes TOZ80 with an explicit seq-matched ACK write (AxiSimple
// clients get no read strobe, so a read side effect is not possible), and
// deposits TO6502 bytes with a register write that sets F_6502. RESET_REQ /
// NMI_REQ are sticky until the PS acknowledges them; an Apple bus reset also
// raises RESET_REQ so the PS restarts the Z80 with the machine.
module applicard_card (
    input  logic                     clk,
    input  logic                     rstn,
    input  globals::AppleBus_read    ab_read,
    input  logic [2:0]               slot_assign,
    input  globals::AxiSimple_common as_common,
    AxiSimple_if.client              as_client,
    output globals::AppleBus_write   ab_write
);

    // AxiSimple register indices (byte address / 4 on the PS side).
    localparam logic [7:0] AXI_REG_STATUS  = 8'h00; // R  [0] F_Z80, [1] F_6502,
                                                    //    [2] RESET_REQ, [3] NMI_REQ,
                                                    //    [15:8] TOZ80 seq, [23:16] TOZO80 data
    localparam logic [7:0] AXI_REG_TO6502  = 8'h01; // W  [7:0] byte -> TO6502, sets F_6502
    localparam logic [7:0] AXI_REG_CONTROL = 8'h02; // W  [0] ACK TOZ80 iff [15:8] == seq,
                                                    //    [1] clear RESET_REQ, [2] clear NMI_REQ
    localparam logic [7:0] AXI_REG_DEBUG   = 8'h03; // R  [7:0] TO6502, [15:8] TOZ80,
                                                    //    [19:16] flags as in STATUS

    localparam logic [3:0] IO_READ_Z80_DATA   = 4'h0;
    localparam logic [3:0] IO_WRITE_Z80_DATA  = 4'h1;
    localparam logic [3:0] IO_Z80_BUSY        = 4'h2;
    localparam logic [3:0] IO_Z80_DATA_READY  = 4'h3;
    localparam logic [3:0] IO_RESET           = 4'h5;
    localparam logic [3:0] IO_CTC_IRQ         = 4'h6;
    localparam logic [3:0] IO_NMI             = 4'h7;

    logic [7:0] toz80_q;
    logic [7:0] to6502_q;
    logic       f_z80_q;      // byte pending for the Z80 (6502 wrote $C0n1)
    logic       f_6502_q;     // byte pending for the 6502 (PS wrote TO6502)
    logic       reset_req_q;  // sticky: $C0n5 access or Apple bus reset
    logic       nmi_req_q;    // sticky: $C0n7 access
    logic [7:0] toz80_seq_q;  // bumps on every 6502 write to $C0n1 (and reset)
    logic       apple_res_q;  // previous ab_read.res, for edge detection

    wire enabled = (slot_assign != 3'h0);
    wire apple_bus_active = enabled && ab_read.res;

    wire slot_io_hit =
        apple_bus_active &&
        (ab_read.addr[15:8] == 8'hC0) &&
        (ab_read.addr[7:4] == (4'h8 + {1'b0, slot_assign}));

    wire ab_io_read  = ab_read.serve_en && ab_read.rw && slot_io_hit;
    wire ab_io_write = ab_read.data_en && !ab_read.rw && slot_io_hit;
    wire [3:0] io_idx = ab_read.addr[3:0];

    logic [7:0] io_read_byte;
    always_comb begin
        case (io_idx)
            IO_READ_Z80_DATA:  io_read_byte = to6502_q;
            IO_WRITE_Z80_DATA: io_read_byte = toz80_q;
            IO_Z80_BUSY:       io_read_byte = {f_z80_q, 7'b0};
            IO_Z80_DATA_READY: io_read_byte = {f_6502_q, 7'b0};
            default:           io_read_byte = 8'hFF;
        endcase
    end

    globals::AppleBus_write ab_write_d;
    globals::AppleBus_write ab_write_q;
    assign ab_write = ab_write_q;

    always_comb begin
        ab_write_d = ab_write_q;
        ab_write_d.assert_inh = 1'b0;
        ab_write_d.assert_res = 1'b0;
        ab_write_d.assert_irq = 1'b0;
        ab_write_d.assert_rdy = 1'b0;
        ab_write_d.assert_nmi = 1'b0;
        ab_write_d.assert_dma = 1'b0;
        ab_write_d.wr_addr = 16'h0000;
        ab_write_d.wr_rw = 1'b0;
        ab_write_d.wr_addr_rw_en = 1'b0;

        if (ab_read.serve_en) begin
            if (ab_io_read) begin
                ab_write_d.wr_data = io_read_byte;
                ab_write_d.wr_data_en = 1'b1;
            end else begin
                ab_write_d.wr_data = 8'h00;
                ab_write_d.wr_data_en = 1'b0;
            end
        end else if (ab_read.data_en) begin
            ab_write_d.wr_data = 8'h00;
            ab_write_d.wr_data_en = 1'b0;
        end
    end

    logic [7:0] axi_read_addr_q;
    always_ff @(posedge clk) begin
        if (!rstn) begin
            toz80_q <= 8'h00;
            to6502_q <= 8'h00;
            f_z80_q <= 1'b0;
            f_6502_q <= 1'b0;
            reset_req_q <= 1'b0;
            nmi_req_q <= 1'b0;
            toz80_seq_q <= 8'h00;
            apple_res_q <= 1'b1;
            ab_write_q <= '0;
            axi_read_addr_q <= 8'd0;
        end else begin
            ab_write_q <= ab_write_d;
            axi_read_addr_q <= as_common.araddr;

            /* PS side first: on a same-cycle conflict with a 6502 access the
             * Apple bus action below wins (last assignment), which matches
             * the hardware where the 6502-facing latch operation is what the
             * 6502 actually observed that cycle. */
            if (as_client.awvalid) begin
                case (as_common.awaddr)
                    AXI_REG_TO6502: begin
                        if (as_common.wstrb[0]) begin
                            to6502_q <= as_common.wdata[7:0];
                            f_6502_q <= 1'b1;
                        end
                    end
                    AXI_REG_CONTROL: begin
                        if (as_common.wstrb[0]) begin
                            /* Seq-matched consume: reject a stale ACK when the
                             * 6502 managed to post a fresh byte (or reset the
                             * card) between the PS's STATUS read and this
                             * write. */
                            if (as_common.wdata[0] &&
                                (as_common.wdata[15:8] == toz80_seq_q))
                                f_z80_q <= 1'b0;
                            if (as_common.wdata[1])
                                reset_req_q <= 1'b0;
                            if (as_common.wdata[2])
                                nmi_req_q <= 1'b0;
                        end
                    end
                    default: begin
                    end
                endcase
            end

            if (ab_io_write) begin
                case (io_idx)
                    IO_WRITE_Z80_DATA: begin
                        toz80_q <= ab_read.data;
                        f_z80_q <= 1'b1;
                        toz80_seq_q <= toz80_seq_q + 8'd1;
                    end
                    IO_RESET: begin
                        toz80_q <= 8'h00;
                        to6502_q <= 8'h00;
                        f_z80_q <= 1'b0;
                        f_6502_q <= 1'b0;
                        reset_req_q <= 1'b1;
                        toz80_seq_q <= toz80_seq_q + 8'd1;
                    end
                    IO_NMI: nmi_req_q <= 1'b1;
                    default: begin
                    end
                endcase
            end

            if (ab_io_read) begin
                case (io_idx)
                    IO_READ_Z80_DATA: f_6502_q <= 1'b0;
                    IO_RESET: begin
                        toz80_q <= 8'h00;
                        to6502_q <= 8'h00;
                        f_z80_q <= 1'b0;
                        f_6502_q <= 1'b0;
                        reset_req_q <= 1'b1;
                        toz80_seq_q <= toz80_seq_q + 8'd1;
                    end
                    IO_NMI: nmi_req_q <= 1'b1;
                    default: begin
                    end
                endcase
            end

            /* Apple bus reset resets the Z80 like the real card's RST
             * wiring; RESET_REQ tells the PS to restart the core with ROM
             * mapped. Edge-detected: latching it level-sensitively would
             * re-arm RESET_REQ on every clock of a *held* reset (boot holds
             * RESET# for a long time), making the PS count thousands of
             * spurious resets. */
            apple_res_q <= ab_read.res;
            if (apple_res_q && !ab_read.res) begin
                toz80_q <= 8'h00;
                to6502_q <= 8'h00;
                f_z80_q <= 1'b0;
                f_6502_q <= 1'b0;
                reset_req_q <= 1'b1;
                nmi_req_q <= 1'b0;
                toz80_seq_q <= toz80_seq_q + 8'd1;
                ab_write_q <= '0;
            end
        end
    end

    always_comb begin
        case (axi_read_addr_q)
            AXI_REG_STATUS: as_client.rdata = {
                8'd0,
                toz80_q,
                toz80_seq_q,
                4'd0,
                nmi_req_q,
                reset_req_q,
                f_6502_q,
                f_z80_q
            };
            AXI_REG_DEBUG: as_client.rdata = {
                12'd0,
                nmi_req_q,
                reset_req_q,
                f_6502_q,
                f_z80_q,
                toz80_q,
                to6502_q
            };
            default: as_client.rdata = 32'h0000_0000;
        endcase
    end

endmodule
