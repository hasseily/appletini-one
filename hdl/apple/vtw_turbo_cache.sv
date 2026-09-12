`timescale 1ns / 1ps
// Small, asynchronous TURBO caches. The address table and byte cache have
// independent fills: a store needs only a writable address-table entry.
// All backing writes still commit to the shadow BRAM on the accepting edge.
// The wrapper invalidates both tables whenever the Apple mapping changes,
// an external writer touches the shadow, or the execution mode changes.
module vtw_turbo_cache #(
    parameter int BYTE_INDEX_BITS = 8,
    parameter int MAP_INDEX_BITS = 5
) (
    input  logic        clk,
    input  logic        rstn,
    input  logic        invalidate,
    input  logic [15:0] addr,
    input  logic        rw,
    output logic        read_valid,
    output logic [15-BYTE_INDEX_BITS:0] read_tag,
    output logic        write_valid,
    output logic [7:0]  write_tag,
    output logic        write_fast,
    output logic [7:0]  rdata,
    output logic [17:0] write_phys,

    input  logic        map_fill,
    input  logic [15:0] map_addr,
    input  logic        map_rw,
    input  logic [17:0] map_phys,
    input  logic        map_fast_write,

    input  logic        byte_fill,
    input  logic [15:0] byte_addr,
    input  logic [7:0]  byte_data,

    // Every CPU shadow write snoops the read cache, including slow writes.
    // Updating only when read and write mappings agree handles RAMRD !=
    // RAMWRT without returning bytes from the wrong physical bank.
    input  logic        snoop_write,
    input  logic [15:0] snoop_addr,
    input  logic [17:0] snoop_phys,
    input  logic [7:0]  snoop_data
);
    localparam int BYTE_COUNT = 1 << BYTE_INDEX_BITS;
    localparam int TAG_BITS = 16 - BYTE_INDEX_BITS;
    localparam int MAP_COUNT = 2 << MAP_INDEX_BITS;
    (* ram_style = "distributed" *) logic [18:0] map_mem [0:MAP_COUNT-1];
    (* ram_style = "distributed" *) logic [TAG_BITS+7:0] byte_mem [0:BYTE_COUNT-1];
    logic [MAP_COUNT-1:0] map_valid_q;
    logic [BYTE_COUNT-1:0] byte_valid_q;

    // Apple programs often put code and buffers on the same page offset.
    // Fold page bits into the set index so those streams do not evict each
    // byte of one another. The high-address tag makes this mapping exact.
    function automatic logic [BYTE_INDEX_BITS-1:0] byte_set(input logic [15:0] a);
        byte_set = a[BYTE_INDEX_BITS-1:0] ^
                   BYTE_INDEX_BITS'(a[15:BYTE_INDEX_BITS]);
    endfunction
    function automatic logic [MAP_INDEX_BITS-1:0] page_set(input logic [7:0] page);
        page_set = MAP_INDEX_BITS'(page) ^
                   (MAP_INDEX_BITS'(page >> MAP_INDEX_BITS) << 2);
    endfunction

    wire [MAP_INDEX_BITS:0] map_index = {rw, page_set(addr[15:8])};
    wire [BYTE_INDEX_BITS-1:0] byte_index = byte_set(addr);
    wire [TAG_BITS+7:0] byte_entry = byte_mem[byte_index];
    wire [18:0] map_entry = map_mem[map_index];
    // A byte was filled only after a translated shadow read, and every
    // mapping change invalidates bytes. Address-table eviction therefore
    // need not evict a still-valid byte or add another lookup to read hits.
    // The wrapper captures these raw entries with the request, then checks
    // their tags. Keep the tag comparison after that register boundary.
    assign read_valid = byte_valid_q[byte_index];
    assign read_tag = byte_entry[TAG_BITS+7:8];
    assign write_valid = map_valid_q[map_index];
    assign write_tag = map_entry[18:11];
    assign write_fast = map_entry[10];
    assign rdata = byte_entry[7:0];
    assign write_phys = {map_entry[9:0], addr[7:0]};

    wire [MAP_INDEX_BITS:0] fill_map_index = {map_rw, page_set(map_addr[15:8])};
    wire [MAP_INDEX_BITS:0] snoop_read_index = {1'b1, page_set(snoop_addr[15:8])};
    wire snoop_same_bank = map_valid_q[snoop_read_index] &&
                          map_mem[snoop_read_index][18:11] == snoop_addr[15:8] &&
                          map_mem[snoop_read_index][9:0] == snoop_phys[17:8];
    wire [BYTE_INDEX_BITS-1:0] snoop_index = byte_set(snoop_addr);
    wire [BYTE_INDEX_BITS-1:0] fill_index = byte_set(byte_addr);

    // No reset on LUT RAM contents: the separate valid bits own lifetime.
    // Fill and snoop are mutually exclusive in the wrapper (a slow read
    // response versus a CPU write), giving the byte array one write port.
    always_ff @(posedge clk) begin
        if (map_fill && !invalidate)
            map_mem[fill_map_index] <= {map_addr[15:8], map_fast_write, map_phys[17:8]};
        if (!invalidate) begin
            // Wrong-bank snoops clear validity below. Every later operation
            // that validates the entry also replaces this complete payload.
            if (snoop_write)
                byte_mem[snoop_index] <= {snoop_addr[15:BYTE_INDEX_BITS], snoop_data};
            else if (byte_fill)
                byte_mem[fill_index] <= {byte_addr[15:BYTE_INDEX_BITS], byte_data};
        end
    end

    always_ff @(posedge clk) begin
        if (!rstn || invalidate) begin
            map_valid_q <= '0;
            byte_valid_q <= '0;
        end
        else begin
            if (map_fill)
                map_valid_q[fill_map_index] <= 1'b1;
            if (snoop_write)
                byte_valid_q[snoop_index] <= snoop_same_bank;
            else if (byte_fill)
                byte_valid_q[fill_index] <= 1'b1;
        end
    end
endmodule
