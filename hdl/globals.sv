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
        logic data_en;       // 1 in the phase that data field updates
        logic addr_en;       // 1 in the phase that addr/rw field updates
                             // (= TAP_ADDR_SNAP, after addr has settled)
        logic sss_en;        // 1 in the phase that soft switches update
    } AppleBus_read;

    typedef struct packed {
        logic [7:0] wr_data;
        logic wr_data_en;
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
