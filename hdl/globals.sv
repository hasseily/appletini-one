`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 02/23/2025 03:08:08 PM
// Design Name: 
// Module Name: globals
// Project Name: 
// Target Devices: 
// Tool Versions: 
// Description: 
// 
// Dependencies: 
// 
// Revision:
// Revision 0.01 - File Created
// Additional Comments:
// 
//////////////////////////////////////////////////////////////////////////////////

package globals;

    function automatic [31:0] apply_wstrb(
        input [31:0] current_value,
        input [31:0] new_value,
        input [3:0] wstrb
    );
        reg [31:0] result;
        integer i;

        // Iterate over each byte lane (assuming 32-bit data bus and 4-bit wstrb)
        for (i = 0; i < 4; i = i + 1) begin
            if (wstrb[i]) begin
                // If the strobe bit is high, update the corresponding byte
                result[i*8 +: 8] = new_value[i*8 +: 8];
            end else begin
                // Otherwise, keep the current value of the byte
                result[i*8 +: 8] = current_value[i*8 +: 8];
            end
        end
        apply_wstrb = result; // Assign the result to the function name
    endfunction


    typedef struct packed {
        logic [7:0] data;
        logic [15:0] addr;
        logic rw;
        logic phi0;
        logic m2sel;
        logic m2b0;
        /* A decodable Apple bus cycle. Identified IIgs machines require
         * M2SEL at the address sample; IIe, II+, and unidentified machines
         * accept every sampled cycle. addr_en still fires for an invalid
         * cycle so clients can clear any pending bus drive, while sss_en and
         * data_en are suppressed. */
        logic cycle_valid;
        logic inh;
        logic res;
        logic irq;
        logic rdy;
        logic dma;
        /* Sampled NMI# level (added for the vTW core's NMI input; the
         * motherboard 6502 sees the same line). Refreshed once per Apple
         * cycle like irq -- 1 MHz interrupt sampling, same as hardware. */
        logic nmi;
        logic data_en;       // 1 in the phase that data field updates
        logic addr_en;       // 1 in the phase that addr_early/rw_early
                             // update (= TAP_ADDR_SNAP, early PHI1 snap)
        logic sss_en;        // early-decode strobe: INH-path translate and
                             // the Apple-cycle cadence tick (timing gen)
        logic serve_en;      // 1 in the phase that addr/rw update (the
                             // PHI0-high sample); the universal strobe for
                             // observation and card serving
        logic drive_en;      // 1 at the bus-master address-drive point
                             // (fall+TAP_DRIVE_ADDR, early PHI1) -- the only
                             // instant a bus-master client (vtw_bus_engine)
                             // may change its driven address/R-W, so every
                             // cycle it emits is fully driven edge to edge
        /* Early PHI1 snapshot of the address bus. Only a socketed
         * 6502/65C02 guarantees addr/RW are valid this early; a DMA bus
         * master (e.g. the TransWarp) may assert them only around PHI0
         * rise, so at this point the bus may still show the previous
         * cycle's parked state. Used EXCLUSIVELY by the INH/PSRAM serving
         * path, whose assert deadline (TAP_INH_DEADLINE, mid-PHI1) falls
         * before the authoritative sample exists. Everything else -- the
         * capture path, soft-switch tracking, and every virtual card --
         * must use addr/rw, which are sampled inside PHI0-high where any
         * master the motherboard itself can accept has the bus valid. */
        logic [15:0] addr_early;
        logic rw_early;
    } AppleBus_read;

    typedef struct packed {
        logic [7:0] wr_data;
        logic wr_data_en;
        /* A physical motherboard-RAM DMA write. Unlike an ordinary card
         * response, this request must use the Apple IIe Tech Note #2 data
         * window: begin after the PH0-rise bus turnaround, meet CAS setup,
         * then release before PH1. The wrapper owns that pin timing; the
         * client holds wr_data and address/R-W stable for the whole cycle. */
        logic wr_dma_data_en;
        logic [15:0] wr_addr;
        logic wr_rw;
        logic wr_addr_rw_en;
        logic assert_inh;
        logic assert_res;
        logic assert_irq;
        logic assert_rdy;
        logic assert_nmi;
        logic assert_dma;
    } AppleBus_write;

    // Apple-address routing from soft-switch state. APPLE_ROUTE_CACHE carries
    // a translated memory address: bank 0 remains motherboard-owned, while
    // nonzero aux/RamWorks banks are served from PSRAM. APPLE_ROUTE_ROM marks
    // motherboard ROM reads, APPLE_ROUTE_BUS marks soft switches and slot I/O,
    // and APPLE_ROUTE_INVALID is the reset value. ADDR_REGION_* tags provide
    // the same ownership class in decoded_addr[31:24].
    localparam logic [7:0] ADDR_REGION_PSRAM = 8'h00;
    localparam logic [7:0] ADDR_REGION_ROM   = 8'h01;
    typedef enum logic [1:0] {
        APPLE_ROUTE_CACHE   = 2'd0,
        APPLE_ROUTE_ROM     = 2'd1,
        APPLE_ROUTE_BUS     = 2'd2,
        APPLE_ROUTE_INVALID = 2'd3
    } apple_route_kind_e;

    typedef struct packed {
        logic [2:0] c8_select;
        logic slot_access;
        logic [23:0] addr_decode;
        logic addr_decode_en;
        // Registered with addr_decode so consumers see one coherent route.
        // addr_decode_en is true for translated memory cycles; PSRAM clients
        // additionally require a nonzero bank.
        apple_route_kind_e route_kind;
        /* Observation-path translation computed from ab_read.addr/rw (the
         * authoritative PHI0-high sample) at serve_en. This is the decode
         * the capture path uses. addr_decode above is its INH/serving
         * counterpart, translated from addr_early at sss_en -- too early to
         * be authoritative under a DMA master, but the only sample that
         * meets the INH assert deadline. */
        logic [23:0] addr_decode_late;
        logic addr_decode_late_en;
        logic sw_80store;
        logic sw_intcxrom;
        logic sw_slotc3rom;
        logic c8_internal_rom;   // INTC8ROM latch (diagnostic export)
        // Each slot has an independent C8 claim latch, set by its own $Cnxx
        // access and cleared only by $CFFF or reset. Accessing another slot
        // does not release an existing claim.
        logic [7:0] io_select;
        logic sw_ramrd;
        logic sw_ramwrt;
        logic sw_altzp;
        logic sw_text;
        logic sw_mixed;
        logic sw_page2;
        logic sw_hires;
        logic sw_altcharset;
        logic sw_80col;
        logic sw_dhires;
        logic sw_lcram_bank2;
        logic sw_lcram_read;
        logic sw_lcram_write;
        logic [6:0] sw_ramworks_bank;
    } SoftSwitchState;

    /* Switch-state inputs consumed by translate_apple_addr. Two producers
     * assemble one of these: soft_switch_manager (the motherboard-tracked
     * state) and vtw_core_top (the virtual TransWarp's private copy, which
     * it tracks from its own accesses). The function below is the single
     * source of truth for //e banking semantics -- any change to Apple
     * address routing belongs there, never in a per-consumer copy. */
    typedef struct packed {
        logic       sw_80store;
        logic       sw_auxread;      // RAMRD
        logic       sw_auxwrite;     // RAMWRT
        logic       sw_altzp;
        logic       sw_page2;
        logic       sw_hires;
        logic       sw_intcxrom;
        logic       sw_slotc3rom;
        logic       c8_internal_rom; // INTC8ROM latch
        logic       sw_lcram_bank2;
        logic       sw_lcram_read;
        logic       sw_lcram_write;
        logic [6:0] sw_ramworks_bank;
    } TranslateState;

    function automatic TranslateState translate_state_from_sss(
        input SoftSwitchState sss
    );
        TranslateState st;
        st.sw_80store       = sss.sw_80store;
        st.sw_auxread       = sss.sw_ramrd;
        st.sw_auxwrite      = sss.sw_ramwrt;
        st.sw_altzp         = sss.sw_altzp;
        st.sw_page2         = sss.sw_page2;
        st.sw_hires         = sss.sw_hires;
        st.sw_intcxrom      = sss.sw_intcxrom;
        st.sw_slotc3rom     = sss.sw_slotc3rom;
        st.c8_internal_rom  = sss.c8_internal_rom;
        st.sw_lcram_bank2   = sss.sw_lcram_bank2;
        st.sw_lcram_read    = sss.sw_lcram_read;
        st.sw_lcram_write   = sss.sw_lcram_write;
        st.sw_ramworks_bank = sss.sw_ramworks_bank;
        return st;
    endfunction

    // Translate a 16-bit Apple address using the supplied soft-switch state.
    // decoded_addr carries the selected 64K bank and Apple offset for memory
    // cycles, or a ROM offset for motherboard-ROM reads. route_kind identifies
    // whether memory, motherboard ROM, or bus/card I/O owns the cycle.
    function automatic void translate_apple_addr(
        input  TranslateState     st,
        input  logic [15:0]       addr_in,
        input  logic              rw_in,        // 1=read, 0=write
        output logic [31:0]       decoded_addr,
        output apple_route_kind_e route_kind
    );
        logic        q_is_cxxx;
        logic        q_is_c0xx;
        logic        q_is_zp_stack;
        logic        q_is_high_ram;
        logic        q_is_hires_pg;
        logic        q_is_text_pg1;
        logic        q_is_display_window;
        logic [7:0]  q_aux_bank_full;
        logic [7:0]  q_bank_sel;
        logic [23:0] q_psram_addr;
        logic        q_cxxx_intcxrom_rom;
        logic        q_cxxx_slot3_rom;
        logic        q_extrom_intcxrom;
        logic        q_extrom_internal_rom;
        logic        q_lcrange_rom_read;

        q_is_cxxx      = (addr_in[15:12] == 4'hc);
        q_is_c0xx      = (addr_in[15:8]  == 8'hc0);
        q_is_zp_stack  = (addr_in[15:9]  == 7'h00);    // 0000-01ff
        q_is_high_ram  = (addr_in[15:14] == 2'b11);    // c000-ffff
        q_is_hires_pg  = (addr_in[15:13] == 3'b001);   // 2000-3fff
        q_is_text_pg1  = (addr_in[15:10] == 6'b000001); // 0400-07ff
        q_is_display_window = q_is_text_pg1 || (st.sw_hires && q_is_hires_pg);
        q_aux_bank_full = {1'b0, st.sw_ramworks_bank} + 8'd1;

        // ---- Physical 64K bank selection ----
        if (q_is_cxxx) begin
            q_bank_sel = 8'd0;
        end
        else if (q_is_zp_stack) begin
            q_bank_sel = st.sw_altzp ? q_aux_bank_full : 8'd0;
        end
        else if (q_is_high_ram) begin
            q_bank_sel = st.sw_altzp ? q_aux_bank_full : 8'd0;
        end
        else begin
            q_bank_sel = (rw_in ? st.sw_auxread : st.sw_auxwrite)
                         ? q_aux_bank_full : 8'd0;
            if (st.sw_80store && q_is_display_window) begin
                q_bank_sel = st.sw_page2 ? q_aux_bank_full : 8'd0;
            end
        end

        // ---- PSRAM byte address, including the LC bank-1 bit-12 remap.
        //      Bits 23:16 carry bank_sel.
        q_psram_addr      = {8'b0, addr_in};
        if (q_is_high_ram && !st.sw_lcram_bank2 && addr_in[13:12] == 2'b01) begin
            q_psram_addr[12] = 1'b0;
        end
        q_psram_addr[23:16] = q_bank_sel;

        // ---- Route selection ----
        //
        // ROM cases. Reads only (writes never go to ROM).
        //   $C100-$C7FF read with intcxrom            -> ROM
        //   $C300-$C3FF read with !slotc3rom          -> ROM (slot-3 internal)
        //   $C800-$CFFF read with intcxrom            -> ROM (expansion)
        //   $C800-$CFFF read after internal $C3xx     -> ROM (slot-3 exp)
        //   $D000-$FFFF read with !sw_lcram_read      -> ROM (LC-ROM)
        q_cxxx_intcxrom_rom = q_is_cxxx && !q_is_c0xx && rw_in &&
                              st.sw_intcxrom &&
                              (addr_in[11:8] >= 4'h1 && addr_in[11:8] <= 4'h7);
        q_cxxx_slot3_rom    = q_is_cxxx && rw_in && !st.sw_slotc3rom &&
                              (addr_in[11:8] == 4'h3);
        q_extrom_intcxrom   = q_is_cxxx && rw_in && st.sw_intcxrom &&
                              (addr_in[11:8] >= 4'h8 && addr_in[11:8] <= 4'hF);
        q_extrom_internal_rom = q_is_cxxx && rw_in && !st.sw_intcxrom &&
                                st.c8_internal_rom &&
                                (addr_in[11:8] >= 4'h8 && addr_in[11:8] <= 4'hF);
        q_lcrange_rom_read  = q_is_high_ram && !q_is_cxxx && rw_in &&
                              !st.sw_lcram_read;

        if (q_is_c0xx) begin
            // $C000-$C0FF soft switches are owned by the Apple bus.
            decoded_addr = {16'h0000, addr_in};
            route_kind   = APPLE_ROUTE_BUS;
        end
        else if (q_cxxx_intcxrom_rom || q_cxxx_slot3_rom ||
                 q_extrom_intcxrom   || q_extrom_internal_rom ||
                 q_lcrange_rom_read) begin
            // ROM-bound read. ROM offset = addr - $C000 = addr[13:0]
            // because addr[15:14] == 2'b11 in all ROM-range addresses.
            decoded_addr = {ADDR_REGION_ROM, 10'd0, addr_in[13:0]};
            route_kind   = APPLE_ROUTE_ROM;
        end
        else if (q_is_cxxx) begin
            // $C100-$CFFF non-ROM cases are motherboard or virtual-card I/O.
            decoded_addr = {16'h0000, addr_in};
            route_kind   = APPLE_ROUTE_BUS;
        end
        else if (q_is_high_ram) begin
            // $D000-$FFFF reaches language-card RAM only when the matching
            // read or write latch is enabled. Other accesses remain bus-owned;
            // motherboard ROM reads were classified above.
            if (rw_in && st.sw_lcram_read) begin
                decoded_addr = {ADDR_REGION_PSRAM, q_psram_addr};
                route_kind   = APPLE_ROUTE_CACHE;
            end
            else if (!rw_in && st.sw_lcram_write) begin
                decoded_addr = {ADDR_REGION_PSRAM, q_psram_addr};
                route_kind   = APPLE_ROUTE_CACHE;
            end
            else begin
                // Disabled language-card writes are ignored by the motherboard.
                decoded_addr = {16'h0000, addr_in};
                route_kind   = APPLE_ROUTE_BUS;
            end
        end
        else begin
            // $0000-$BFFF memory. Bank 0 remains on the motherboard; selected
            // aux/RamWorks banks are served by psram_simple.
            decoded_addr = {ADDR_REGION_PSRAM, q_psram_addr};
            route_kind   = APPLE_ROUTE_CACHE;
        end
    endfunction

    typedef struct packed {
        logic [19:0] centisecond_ticks;
        logic [3:0] year_hi;
        logic [3:0] year_lo;
        logic [3:0] month_hi;
        logic [3:0] month_lo;
        logic [3:0] day_hi;
        logic [3:0] day_lo;
        logic [3:0] day_of_week_hi;
        logic [3:0] day_of_week_lo;
        logic [3:0] hour_hi;
        logic [3:0] hour_lo;
        logic [3:0] minute_hi;
        logic [3:0] minute_lo;
        logic [3:0] second_hi;
        logic [3:0] second_lo;
        logic [3:0] centisecond_hi;
        logic [3:0] centisecond_lo;
    } NSC_time;

    // save signal width by limiting clients to 256
    typedef struct packed {
        logic [7:0] awaddr;
        logic [7:0] araddr;
        logic [31:0] wdata;
        logic [3:0] wstrb;
    } AxiSimple_common;

    // 4-way set-associative cache tag entry (64 bits).
    // Line data lives in a separate, way-indexed BRAM so only the
    // relevant way is fetched per request.
    //
    // plru is a tree-based pseudo-LRU:
    //   plru[0]: root -- 0 => LRU is in pair {A,B}, 1 => LRU is in pair {C,D}
    //   plru[1]: 0 => A is LRU in {A,B}, 1 => B is LRU in {A,B}
    //   plru[2]: 0 => C is LRU in {C,D}, 1 => D is LRU in {C,D}
    // On access of a way, each bit on the path flips to point AWAY from it.
    typedef struct packed {
        logic [8:0] unused;
        logic [2:0] plru;
        logic [10:0] tag_a;
        logic dirty_a;
        logic valid_a;
        logic [10:0] tag_b;
        logic dirty_b;
        logic valid_b;
        logic [10:0] tag_c;
        logic dirty_c;
        logic valid_c;
        logic [10:0] tag_d;
        logic dirty_d;
        logic valid_d;
    } CacheTagEntry;

endpackage

interface AxiSimple_if;
    logic awvalid;
    logic [31:0] rdata;
    modport master (output awvalid, input rdata);
    modport client (input awvalid, output rdata);
endinterface : AxiSimple_if

interface Axi3_read_if #(
    parameter ADDR_WIDTH = 32,
    parameter DATA_WIDTH = 32
);

    logic [ADDR_WIDTH-1:0] araddr;
    logic [3:0] arlen;
    logic [2:0] arsize;
    logic [1:0] arburst;
    logic arvalid;
    logic arready;

    logic [DATA_WIDTH-1:0] rdata;
    logic [1:0] rresp;
    logic rlast;
    logic rvalid;
    logic rready;

    modport master (
        // AR channel: Master outputs address/len/size/burst/valid, slave outputs ready
        output araddr, arlen, arsize, arburst, arvalid,
        input arready,

        // R channel: Master outputs ready, slave outputs data/response/last/valid
        input rdata, rresp, rlast, rvalid,
        output rready
    );

    // Modport for the Slave (reacts to transactions)
    modport slave (
        // AR channel: Slave inputs address/len/size/burst/valid, outputs ready
        input araddr, arlen, arsize, arburst, arvalid,
        output arready,

        // R channel: Slave inputs ready, outputs data/response/last/valid
        output rdata, rresp, rlast, rvalid,
        input rready
    );
endinterface : Axi3_read_if

interface Axi3_write_if #(
    parameter ADDR_WIDTH = 32,
    parameter DATA_WIDTH = 32
);

    logic [ADDR_WIDTH-1:0] awaddr;
    logic [3:0] awlen;
    logic [2:0] awsize;
    logic [1:0] awburst;
    logic awvalid;
    logic awready;

    logic [DATA_WIDTH-1:0] wdata;
    logic [(DATA_WIDTH/8)-1:0] wstrb;
    logic wlast;
    logic wvalid;
    logic wready;

    logic [1:0] bresp;
    logic bvalid;
    logic bready;
    
    modport master(
        // AW channel: Master outputs valid, slave outputs ready
        output awaddr, awlen, awsize, awburst, awvalid,
        input awready,

        // W channel: Master outputs data/strobe/last/valid, slave outputs ready
        output wdata, wstrb, wlast, wvalid,
        input wready,

        // B channel: Master outputs ready, slave outputs response/valid
        input bresp, bvalid,
        output bready
    );

    // Modport for the Slave (reacts to transactions)
    modport Slave (
        // AW channel: Slave inputs valid, outputs ready
        input awaddr, awlen, awsize, awburst, awvalid,
        output awready,

        // W channel: Slave inputs data/strobe/last/valid, outputs ready
        input wdata, wstrb, wlast, wvalid,
        output wready,

        // B channel: Slave inputs ready, outputs response/valid
        output bresp, bvalid,
        input bready
    );

endinterface : Axi3_write_if
