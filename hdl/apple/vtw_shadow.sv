`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Virtual TransWarp shadow memory.
//
// The vTW core executes entirely out of this BRAM; the motherboard only ever
// sees $C000-$CFFF cycles and posted video write-through (vtw_bus_engine).
// Layout mirrors translate_apple_addr's output space exactly, so the routing
// in vtw_core_top is a thin physical-address mapping with no second opinion
// about //e banking:
//
//   main 64K   -- bank 0. LC RAM is folded inside the 64K by the translate
//   aux  64K   -- bank 1 (base aux; RamWorks banks >1 are phase-2 bus cycles)
//                 function's bank-1 bit-12 remap: LC bank-1 $Dxxx lands in
//                 the otherwise-RAM-less $Cxxx hole, LC bank-2 + $E000-$FFFF
//                 occupy their natural offsets.
//   ROM  16K   -- $C000-$FFFF motherboard ROM image addressed by addr[13:0]
//                 (translate's ROM offset), covering internal $C1xx-$CFFF
//                 firmware and the $D000-$FFFF autostart/monitor ROM.
//
// Total 144 KB = 36 RAMB36. Port A belongs to the core wrapper: one access
// per fabric clock, read data valid the next cycle. Port B belongs to the
// ARM (AxiSimple-mapped in apple_top): boot-time ROM copy, debug peek/poke,
// and state dumps. Port B uses the flat physical map below.
//////////////////////////////////////////////////////////////////////////////////

package vtw_shadow_pkg;

    // Flat physical map (18-bit byte address), shared by port B consumers
    // and the port-A mapping helper.
    localparam logic [17:0] VTW_SHADOW_MAIN_BASE = 18'h00000;
    localparam logic [17:0] VTW_SHADOW_AUX_BASE  = 18'h10000;
    localparam logic [17:0] VTW_SHADOW_ROM_BASE  = 18'h20000;
    localparam int          VTW_SHADOW_BYTES     = 18'h24000; // 144 KB

    // Map a translate_apple_addr result onto the flat physical space.
    // valid=0 means the access is not shadow-backed (bus route, or a
    // RamWorks bank beyond base aux -- both belong to vtw_bus_engine).
    function automatic void vtw_shadow_map(
        input  globals::apple_route_kind_e route_kind,
        input  logic [31:0]                decoded_addr,
        output logic                       valid,
        output logic [17:0]                phys
    );
        valid = 1'b0;
        phys  = '0;
        unique case (route_kind)
            globals::APPLE_ROUTE_CACHE: begin
                unique case (decoded_addr[23:16])
                    8'd0: begin
                        valid = 1'b1;
                        phys  = VTW_SHADOW_MAIN_BASE | {2'b00, decoded_addr[15:0]};
                    end
                    8'd1: begin
                        valid = 1'b1;
                        phys  = VTW_SHADOW_AUX_BASE | {2'b00, decoded_addr[15:0]};
                    end
                    default: ; // RamWorks banks >1: real bus cycles (phase 2)
                endcase
            end
            globals::APPLE_ROUTE_ROM: begin
                valid = 1'b1;
                phys  = VTW_SHADOW_ROM_BASE | {4'b0000, decoded_addr[13:0]};
            end
            default: ;
        endcase
    endfunction

endpackage

module vtw_shadow (
    input  logic        clk,

    // Port A: the core wrapper. One access per clock; a_rdata is valid the
    // cycle after a_en. Writes to the ROM region are ignored (translate
    // never routes a write to ROM; this is belt-and-braces).
    input  logic        a_en,
    input  logic [17:0] a_addr,
    input  logic        a_we,
    input  logic [7:0]  a_wdata,
    output logic [7:0]  a_rdata,

    // Port B: ARM access (boot ROM copy, debug peek/poke). Same timing.
    input  logic        b_en,
    input  logic [17:0] b_addr,
    input  logic        b_we,
    input  logic [7:0]  b_wdata,
    output logic [7:0]  b_rdata
);

    import vtw_shadow_pkg::*;

    // Three inferred true-dual-port BRAM groups. No reset: contents are
    // ARM-initialized (ROM copy) or software-written; the core is held in
    // reset until the ARM releases it.
    // Split the 64 KiB memories at address bit 15. A 64Kx8 inference on a
    // 7-series part uses two RAMB36E1s in a hardware depth cascade for every
    // data bit. That fixed cascade delay consumes most of the core read
    // interval. Explicit 32 KiB banks retain the same RAM count and one-clock
    // read contract while selecting the bank in fabric.
    (* ram_style = "block" *) logic [7:0] mem_main_lo [0:32767];
    (* ram_style = "block" *) logic [7:0] mem_main_hi [0:32767];
    (* ram_style = "block" *) logic [7:0] mem_aux_lo  [0:32767];
    (* ram_style = "block" *) logic [7:0] mem_aux_hi  [0:32767];
    (* ram_style = "block" *) logic [7:0] mem_rom     [0:16383];

    wire        a_is_rom  = a_addr[17];              // 0x20000-0x23FFF
    wire        a_is_aux  = !a_addr[17] && a_addr[16];
    wire        a_is_main = !a_addr[17] && !a_addr[16];
    wire        b_is_rom  = b_addr[17];
    wire        b_is_aux  = !b_addr[17] && b_addr[16];
    wire        b_is_main = !b_addr[17] && !b_addr[16];

    logic [7:0] a_rdata_main_lo, a_rdata_main_hi;
    logic [7:0] a_rdata_aux_lo, a_rdata_aux_hi, a_rdata_rom;
    logic [7:0] b_rdata_main_lo, b_rdata_main_hi;
    logic [7:0] b_rdata_aux_lo, b_rdata_aux_hi, b_rdata_rom;
    logic       a_sel_rom_q, a_sel_aux_q;
    logic       b_sel_rom_q, b_sel_aux_q;
    logic       a_sel_hi_q, b_sel_hi_q;

    wire [7:0] a_rdata_main = a_sel_hi_q ? a_rdata_main_hi :
                                               a_rdata_main_lo;
    wire [7:0] a_rdata_aux  = a_sel_hi_q ? a_rdata_aux_hi :
                                               a_rdata_aux_lo;
    wire [7:0] b_rdata_main = b_sel_hi_q ? b_rdata_main_hi :
                                               b_rdata_main_lo;
    wire [7:0] b_rdata_aux  = b_sel_hi_q ? b_rdata_aux_hi :
                                               b_rdata_aux_lo;

    // ---- Port A ----
    always_ff @(posedge clk) begin
        if (a_en) begin
            if (a_we) begin
                if (a_is_main && !a_addr[15])
                    mem_main_lo[a_addr[14:0]] <= a_wdata;
                if (a_is_main && a_addr[15])
                    mem_main_hi[a_addr[14:0]] <= a_wdata;
                if (a_is_aux && !a_addr[15])
                    mem_aux_lo[a_addr[14:0]] <= a_wdata;
                if (a_is_aux && a_addr[15])
                    mem_aux_hi[a_addr[14:0]] <= a_wdata;
                // ROM region: read-only from the core side.
            end
            a_rdata_main_lo <= mem_main_lo[a_addr[14:0]];
            a_rdata_main_hi <= mem_main_hi[a_addr[14:0]];
            a_rdata_aux_lo  <= mem_aux_lo[a_addr[14:0]];
            a_rdata_aux_hi  <= mem_aux_hi[a_addr[14:0]];
            a_rdata_rom     <= mem_rom[a_addr[13:0]];
            a_sel_rom_q     <= a_is_rom;
            a_sel_aux_q     <= a_is_aux;
            a_sel_hi_q      <= a_addr[15];
        end
    end
    assign a_rdata = a_sel_rom_q ? a_rdata_rom :
                     a_sel_aux_q ? a_rdata_aux : a_rdata_main;

    // ---- Port B ----
    always_ff @(posedge clk) begin
        if (b_en) begin
            if (b_we) begin
                if (b_is_main && !b_addr[15])
                    mem_main_lo[b_addr[14:0]] <= b_wdata;
                if (b_is_main && b_addr[15])
                    mem_main_hi[b_addr[14:0]] <= b_wdata;
                if (b_is_aux && !b_addr[15])
                    mem_aux_lo[b_addr[14:0]] <= b_wdata;
                if (b_is_aux && b_addr[15])
                    mem_aux_hi[b_addr[14:0]] <= b_wdata;
                if (b_is_rom)  mem_rom[b_addr[13:0]]  <= b_wdata;
            end
            b_rdata_main_lo <= mem_main_lo[b_addr[14:0]];
            b_rdata_main_hi <= mem_main_hi[b_addr[14:0]];
            b_rdata_aux_lo  <= mem_aux_lo[b_addr[14:0]];
            b_rdata_aux_hi  <= mem_aux_hi[b_addr[14:0]];
            b_rdata_rom     <= mem_rom[b_addr[13:0]];
            b_sel_rom_q     <= b_is_rom;
            b_sel_aux_q     <= b_is_aux;
            b_sel_hi_q      <= b_addr[15];
        end
    end
    assign b_rdata = b_sel_rom_q ? b_rdata_rom :
                     b_sel_aux_q ? b_rdata_aux : b_rdata_main;

endmodule
