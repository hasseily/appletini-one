`timescale 1ns / 1ps

module vidhd_card (
    input  logic                    clk,
    input  logic                    rstn,
    input  globals::AppleBus_read   ab_read,
    input  globals::SoftSwitchState sss,
    input  logic [2:0]              slot_assign,
    output globals::AppleBus_write  ab_write
);

    wire enabled = (slot_assign != 3'h0) &&
                   ((slot_assign != 3'h3) || sss.sw_slotc3rom) &&
                   ab_read.res;
    wire slot_io_hit =
        enabled &&
        (ab_read.addr[15:8] == 8'hC0) &&
        (ab_read.addr[7:4] == (4'h8 + {1'b0, slot_assign}));
    wire slot_rom_hit =
        enabled &&
        sss.slot_access &&
        (ab_read.addr[15:12] == 4'hC) &&
        (ab_read.addr[11] == 1'b0) &&
        (ab_read.addr[10:8] == slot_assign);
    wire ab_io_read = ab_read.sss_en && ab_read.rw && slot_io_hit;
    wire ab_rom_read = ab_read.sss_en && ab_read.rw && slot_rom_hit;
    wire [3:0] io_idx = ab_read.addr[3:0];
    wire [7:0] rom_idx = ab_read.addr[7:0];
    wire ab_magic_read =
        (ab_io_read && (io_idx <= 4'h2)) ||
        (ab_rom_read && (rom_idx <= 8'h02));
    wire [3:0] magic_idx = ab_io_read ? io_idx : rom_idx[3:0];

    function automatic logic [7:0] vidhd_magic_byte(input logic [3:0] idx);
        case (idx)
        4'h0: vidhd_magic_byte = 8'h24;
        4'h1: vidhd_magic_byte = 8'hEA;
        4'h2: vidhd_magic_byte = 8'h4C;
        default: vidhd_magic_byte = 8'h00;
        endcase
    endfunction

    globals::AppleBus_write ab_write_d;
    globals::AppleBus_write ab_write_q;
    assign ab_write = ab_write_q;

    always_comb begin
        ab_write_d               = ab_write_q;
        ab_write_d.wr_addr       = 16'h0000;
        ab_write_d.wr_rw         = 1'b0;
        ab_write_d.wr_addr_rw_en = 1'b0;
        ab_write_d.assert_res    = 1'b0;
        ab_write_d.assert_irq    = 1'b0;
        ab_write_d.assert_rdy    = 1'b0;
        ab_write_d.assert_nmi    = 1'b0;
        ab_write_d.assert_dma    = 1'b0;

        if (!enabled || ab_read.addr_en || ab_read.data_en) begin
            ab_write_d.wr_data    = 8'h00;
            ab_write_d.wr_data_en = 1'b0;
            ab_write_d.assert_inh = 1'b0;
        end

        if (ab_read.sss_en) begin
            if (ab_magic_read) begin
                ab_write_d.wr_data    = vidhd_magic_byte(magic_idx);
                ab_write_d.wr_data_en = 1'b1;
                ab_write_d.assert_inh = 1'b0;
            end else begin
                ab_write_d.wr_data    = 8'h00;
                ab_write_d.wr_data_en = 1'b0;
                ab_write_d.assert_inh = 1'b0;
            end
        end
    end

    always_ff @(posedge clk) begin
        if (!rstn) begin
            ab_write_q <= '0;
        end else begin
            ab_write_q <= ab_write_d;
        end
    end
endmodule
