`timescale 1ns / 1ps

// Virtual Apple Super Serial Card, fixed to printer duty.
//
// The card serves the real 1981 SSC firmware (slot page + 2 KB $C800 ROM,
// built by scripts/build_ssc_rom.py) and emulates just enough of a 6551
// ACIA for the firmware's printer path: the status register always reports
// "transmit ready", and every byte written to the transmit data register
// lands in a FIFO that the PS drains through apple_top's card-control
// registers. There is no receive path and no IRQ.
//
// Apple slot I/O (DEVSEL, $C0n0-$C0nF):
//   $C0n1: DIP switch block 1 (read)   -- hardwired printer-mode value
//   $C0n2: DIP switch block 2 (read)   -- hardwired printer-mode value
//   $C0n8: ACIA transmit data (write -> FIFO), receive data (read, $00)
//   $C0n9: ACIA status (read, always $10), programmed reset (write)
//   $C0nA: ACIA command register (read/write latch)
//   $C0nB: ACIA control register (read/write latch)
//   $C0nC-$C0nF alias $C0n8-$C0nB, as on the real card (A3 selects the
//   ACIA, A1:A0 select its register).
//
// $C0n0, $C0n3, and $C0n4-$C0n7 do not respond: the Uthernet II shares the
// slot and owns $C0n4-$C0n7 (see uthernet2_card.sv).
//
// The Apple RES# level clears the ACIA latches but not the FIFO: the PS
// printer service owns job lifetime and closes a print job on idle timeout.
module ssc_card (
    input  logic                    clk,
    input  logic                    rstn,
    input  globals::AppleBus_read   ab_read,
    input  globals::SoftSwitchState sss,
    input  logic [2:0]              slot_assign,
    output globals::AppleBus_write  ab_write,

    // Printer FIFO drain port, served by apple_top's card-control registers.
    output logic [11:0]             tx_count,
    output logic [7:0]              tx_head,
    output logic                    tx_head_valid,
    input  logic                    tx_pop,            // one-clock pulse
    input  logic                    tx_clear,          // one-clock pulse
    output logic                    tx_overflow,       // sticky until cleared
    input  logic                    tx_overflow_clear, // one-clock pulse
    output logic [7:0]              acia_command,
    output logic [7:0]              acia_control
);

    // Printer-mode DIP values (switch ON reads 0):
    //   DIPSW1 $E2: baud code $E (9600), PPC printer firmware mode.
    //   DIPSW2 $20: 8 data 1 stop, no CR delay, no width formatting,
    //               LF after CR enabled for the BASIC entry.
    localparam logic [7:0] DIPSW1_VALUE = 8'hE2;
    localparam logic [7:0] DIPSW2_VALUE = 8'h20;

    // Idle 6551 status: TDR empty, no RDR data, DCD/DSR asserted.
    localparam logic [7:0] ACIA_STATUS_VALUE = 8'h10;

    localparam int FIFO_AW = 11;   // 2 KB printer FIFO

    logic [7:0] slot_rom [0:255];
    logic [7:0] c8_rom   [0:2047];
    initial begin
        $readmemh("ssc_slot1_c100.mem", slot_rom);
        $readmemh("ssc_c800.mem", c8_rom);
    end

    globals::AppleBus_write ab_write_q;
    globals::AppleBus_write ab_write_d;
    assign ab_write = ab_write_q;

    logic [7:0] cmd_q;
    logic [7:0] ctl_q;
    assign acia_command = cmd_q;
    assign acia_control = ctl_q;

    (* ram_style = "block" *)
    logic [7:0] tx_fifo [0:(1 << FIFO_AW) - 1];
    logic [FIFO_AW-1:0] tx_wr_q;
    logic [FIFO_AW-1:0] tx_rd_q;
    logic [FIFO_AW:0]   tx_count_q;
    logic [7:0]         tx_head_q;
    logic               tx_overflow_q;

    wire tx_full  = tx_count_q[FIFO_AW];
    wire tx_empty = (tx_count_q == '0);

    assign tx_count      = {1'b0, tx_count_q};
    assign tx_head       = tx_head_q;
    assign tx_head_valid = !tx_empty;
    assign tx_overflow   = tx_overflow_q;

    wire enabled = (slot_assign != 3'h0);
    wire apple_bus_active = enabled &&
                            ((slot_assign != 3'h3) || sss.sw_slotc3rom) &&
                            ab_read.res;

    wire slot_rom_hit =
        apple_bus_active &&
        sss.slot_access &&
        (ab_read.addr[15:12] == 4'hC) &&
        (ab_read.addr[11] == 1'b0) &&
        (ab_read.addr[10:8] == slot_assign);

    // $C800-$CFFE while this slot owns the expansion window. $CFFF is never
    // served so the shared release access still floats.
    wire c8_owner =
        apple_bus_active &&
        !sss.sw_intcxrom &&
        sss.io_select[slot_assign] &&
        (ab_read.addr[15:12] == 4'hC) &&
        (ab_read.addr[11] == 1'b1) &&
        (ab_read.addr[10:0] != 11'h7FF);

    wire slot_io_hit =
        apple_bus_active &&
        (ab_read.addr[15:8] == 8'hC0) &&
        (ab_read.addr[7:4] == (4'h8 + {1'b0, slot_assign}));
    wire [3:0] io_idx = ab_read.addr[3:0];
    wire dipsw_hit = slot_io_hit &&
                     ((io_idx == 4'h1) || (io_idx == 4'h2));
    wire acia_hit  = slot_io_hit && io_idx[3];

    /* ROM lookups are registered (BRAM), so read serving keys on a
     * one-clock-delayed serve_en that consumes them coherently and still
     * registers wr_data before the TAP_DATA_EMIT drive window. */
    logic serve_en_q;
    logic [7:0] slot_rom_data_q;
    logic [7:0] c8_rom_data_q;
    always_ff @(posedge clk) begin
        serve_en_q      <= ab_read.serve_en;
        slot_rom_data_q <= slot_rom[ab_read.addr[7:0]];
        c8_rom_data_q   <= c8_rom[ab_read.addr[10:0]];
    end

    wire ab_rom_read  = serve_en_q && ab_read.rw && slot_rom_hit;
    wire ab_c8_read   = serve_en_q && ab_read.rw && c8_owner;
    wire ab_io_read   = serve_en_q && ab_read.rw &&
                        (dipsw_hit || acia_hit);
    wire ab_io_write  = ab_read.data_en && !ab_read.rw && acia_hit;

    logic [7:0] io_read_byte;
    always_comb begin
        io_read_byte = 8'h00;
        if (dipsw_hit) begin
            io_read_byte = (io_idx[1:0] == 2'b01) ? DIPSW1_VALUE
                                                  : DIPSW2_VALUE;
        end else begin
            case (io_idx[1:0])
                2'b00: io_read_byte = 8'h00;              // RDR: no input
                2'b01: io_read_byte = ACIA_STATUS_VALUE;  // always ready
                2'b10: io_read_byte = cmd_q;
                2'b11: io_read_byte = ctl_q;
            endcase
        end
    end

    always_comb begin
        ab_write_d = ab_write_q;
        ab_write_d.assert_inh = 1'b0;
        ab_write_d.assert_res = 1'b0;
        ab_write_d.assert_irq = 1'b0;
        ab_write_d.assert_rdy = 1'b0;
        ab_write_d.assert_nmi = 1'b0;
        ab_write_d.assert_dma = 1'b0;
        ab_write_d.wr_dma_data_en = 1'b0;
        ab_write_d.wr_addr = 16'h0000;
        ab_write_d.wr_rw = 1'b0;
        ab_write_d.wr_addr_rw_en = 1'b0;

        if (serve_en_q) begin
            if (ab_rom_read) begin
                ab_write_d.wr_data = slot_rom_data_q;
                ab_write_d.wr_data_en = 1'b1;
            end else if (ab_c8_read) begin
                ab_write_d.wr_data = c8_rom_data_q;
                ab_write_d.wr_data_en = 1'b1;
            end else if (ab_io_read) begin
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

    wire tdr_write = ab_io_write && (io_idx[1:0] == 2'b00);
    wire fifo_push = tdr_write && !tx_full;
    wire fifo_pop  = tx_pop && !tx_empty;

    always_ff @(posedge clk) begin
        if (!rstn) begin
            ab_write_q    <= '0;
            cmd_q         <= 8'h00;
            ctl_q         <= 8'h00;
            tx_wr_q       <= '0;
            tx_rd_q       <= '0;
            tx_count_q    <= '0;
            tx_head_q     <= 8'h00;
            tx_overflow_q <= 1'b0;
        end else begin
            ab_write_q <= ab_write_d;

            if (!ab_read.res || !enabled) begin
                ab_write_q <= '0;
                cmd_q      <= 8'h00;
                ctl_q      <= 8'h00;
            end else if (ab_io_write) begin
                case (io_idx[1:0])
                    2'b00: ;                              // TDR: FIFO below
                    2'b01: cmd_q <= {cmd_q[7:5], 5'b0};   // programmed reset
                    2'b10: cmd_q <= ab_read.data;
                    2'b11: ctl_q <= ab_read.data;
                endcase
            end

            if (tdr_write && tx_full) begin
                tx_overflow_q <= 1'b1;
            end
            if (tx_overflow_clear) begin
                tx_overflow_q <= 1'b0;
            end

            if (tx_clear) begin
                tx_wr_q    <= '0;
                tx_rd_q    <= '0;
                tx_count_q <= '0;
            end else begin
                if (fifo_push) begin
                    tx_fifo[tx_wr_q] <= ab_read.data;
                    tx_wr_q <= tx_wr_q + 1'b1;
                end
                if (fifo_pop) begin
                    tx_rd_q <= tx_rd_q + 1'b1;
                end
                case ({fifo_push, fifo_pop})
                    2'b10: tx_count_q <= tx_count_q + 1'b1;
                    2'b01: tx_count_q <= tx_count_q - 1'b1;
                    default: ;
                endcase
            end

            /* Registered BRAM head read. After a push into an empty FIFO the
             * head byte lags the count by one clock; the PS sees the two
             * through separate AXI reads many clocks apart, so it never
             * observes the stale window. */
            tx_head_q <= tx_fifo[fifo_pop ? tx_rd_q + 1'b1 : tx_rd_q];
        end
    end

endmodule
