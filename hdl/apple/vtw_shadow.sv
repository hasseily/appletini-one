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
    output logic [31:0] a_rdata32,

    // Port B: ARM access (boot ROM copy, debug peek/poke). Same timing.
    input  logic        b_en,
    input  logic [17:0] b_addr,
    input  logic        b_we,
    input  logic [7:0]  b_wdata,
    output logic [7:0]  b_rdata,
    output logic [31:0] b_rdata32,
    input  logic        b_word_we,
    input  logic [31:0] b_wdata32
);

    import vtw_shadow_pkg::*;

    // Three inferred true-dual-port BRAM groups. No reset: contents are
    // ARM-initialized (ROM copy) or software-written; the core is held in
    // reset until the ARM releases it.
    // Four byte lanes share an aligned word address. Byte enables preserve
    // the CPU/debug interface while both ports fetch four bytes at once.
    (* ram_style = "block" *) logic [31:0] mem_main [0:16383];
    (* ram_style = "block" *) logic [31:0] mem_aux  [0:16383];
    (* ram_style = "block" *) logic [31:0] mem_rom  [0:4095];

    wire        a_is_rom  = a_addr[17];              // 0x20000-0x23FFF
    wire        a_is_aux  = !a_addr[17] && a_addr[16];
    wire        a_is_main = !a_addr[17] && !a_addr[16];
    wire        b_is_rom  = b_addr[17];
    wire        b_is_aux  = !b_addr[17] && b_addr[16];
    wire        b_is_main = !b_addr[17] && !b_addr[16];

    logic [31:0] a_rdata_main, a_rdata_aux, a_rdata_rom;
    logic [31:0] b_rdata_main, b_rdata_aux, b_rdata_rom;
    logic       a_sel_rom_q, a_sel_aux_q;
    logic       b_sel_rom_q, b_sel_aux_q;
    logic [1:0] a_lane_q, b_lane_q;
    wire b_write_word = (b_word_we === 1'b1) && b_addr[1:0] == 2'b00;

    // ---- Port A ----
    always_ff @(posedge clk) begin
        if (a_en) begin
            for (int lane = 0; lane < 4; lane++) begin
                if (a_we && a_addr[1:0] == 2'(lane)) begin
                    if (a_is_main) mem_main[a_addr[15:2]][8*lane +: 8] <= a_wdata;
                    if (a_is_aux) mem_aux[a_addr[15:2]][8*lane +: 8] <= a_wdata;
                end
            end
            a_rdata_main <= mem_main[a_addr[15:2]];
            a_rdata_aux  <= mem_aux[a_addr[15:2]];
            a_rdata_rom  <= mem_rom[a_addr[13:2]];
            a_sel_rom_q  <= a_is_rom;
            a_sel_aux_q  <= a_is_aux;
            a_lane_q     <= a_addr[1:0];
        end
    end
    assign a_rdata32 = a_sel_rom_q ? a_rdata_rom :
                      a_sel_aux_q ? a_rdata_aux : a_rdata_main;
    assign a_rdata = a_rdata32[8*a_lane_q +: 8];

    // ---- Port B ----
    always_ff @(posedge clk) begin
        if (b_en) begin
            for (int lane = 0; lane < 4; lane++) begin
                if (b_we && (b_write_word || b_addr[1:0] == 2'(lane))) begin
                    if (b_is_main) mem_main[b_addr[15:2]][8*lane +: 8] <=
                        b_write_word ? b_wdata32[8*lane +: 8] : b_wdata;
                    if (b_is_aux) mem_aux[b_addr[15:2]][8*lane +: 8] <=
                        b_write_word ? b_wdata32[8*lane +: 8] : b_wdata;
                    if (b_is_rom) mem_rom[b_addr[13:2]][8*lane +: 8] <=
                        b_write_word ? b_wdata32[8*lane +: 8] : b_wdata;
                end
            end
            b_rdata_main <= mem_main[b_addr[15:2]];
            b_rdata_aux  <= mem_aux[b_addr[15:2]];
            b_rdata_rom  <= mem_rom[b_addr[13:2]];
            b_sel_rom_q  <= b_is_rom;
            b_sel_aux_q  <= b_is_aux;
            b_lane_q     <= b_addr[1:0];
        end
    end
    assign b_rdata32 = b_sel_rom_q ? b_rdata_rom :
                      b_sel_aux_q ? b_rdata_aux : b_rdata_main;
    assign b_rdata = b_rdata32[8*b_lane_q +: 8];

endmodule
