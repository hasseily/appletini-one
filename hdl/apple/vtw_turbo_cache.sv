`timescale 1ns / 1ps
// Small, asynchronous TURBO caches. The address table and word cache have
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
    input  logic [31:0] word_data,
    input  logic [17:0] byte_phys,

    // Every CPU shadow write snoops the read cache, including slow writes.
    // Updating only when read and write mappings agree handles RAMRD !=
    // RAMWRT without returning bytes from the wrong physical bank.
    input  logic        snoop_write,
    input  logic [15:0] snoop_addr,
    input  logic [17:0] snoop_phys,
    input  logic [7:0]  snoop_data
);
    localparam int WORD_INDEX_BITS = BYTE_INDEX_BITS - 2;
    localparam int WORD_COUNT = 1 << WORD_INDEX_BITS;
    localparam int TAG_BITS = 16 - BYTE_INDEX_BITS;
    localparam int MAP_COUNT = 2 << MAP_INDEX_BITS;
    (* ram_style = "distributed" *) logic [18:0] map_mem [0:MAP_COUNT-1];
    (* ram_style = "distributed" *) logic [31:0] word_mem [0:WORD_COUNT-1];
    (* ram_style = "distributed" *) logic [TAG_BITS+9:0] word_tag_mem [0:WORD_COUNT-1];
    logic [MAP_COUNT-1:0] map_valid_q;
    logic [WORD_COUNT-1:0] word_valid_q;

    // Apple programs often put code and buffers on the same page offset.
    // Fold page bits into the set index so those streams do not evict each
    // byte of one another. The high-address tag makes this mapping exact.
    function automatic logic [WORD_INDEX_BITS-1:0] word_set(input logic [15:0] a);
        word_set = a[BYTE_INDEX_BITS-1:2] ^
                   WORD_INDEX_BITS'(a[15:BYTE_INDEX_BITS]) ^
                   (WORD_INDEX_BITS'(a >> (BYTE_INDEX_BITS + WORD_INDEX_BITS)) << 1);
    endfunction
    function automatic logic [MAP_INDEX_BITS-1:0] page_set(input logic [7:0] page);
        page_set = MAP_INDEX_BITS'(page) ^
                   (MAP_INDEX_BITS'(page >> MAP_INDEX_BITS) << 2);
    endfunction

    wire [MAP_INDEX_BITS:0] map_index = {rw, page_set(addr[15:8])};
    wire [WORD_INDEX_BITS-1:0] word_index = word_set(addr);
    wire [TAG_BITS+9:0] word_tag = word_tag_mem[word_index];
    wire [18:0] map_entry = map_mem[map_index];
    // A byte was filled only after a translated shadow read, and every
    // mapping change invalidates bytes. Address-table eviction therefore
    // need not evict a still-valid byte or add another lookup to read hits.
    // The wrapper captures these raw entries with the request, then checks
    // their tags. Keep the tag comparison after that register boundary.
    assign read_valid = word_valid_q[word_index];
    assign read_tag = word_tag[TAG_BITS+9:10];
    assign write_valid = map_valid_q[map_index];
    assign write_tag = map_entry[18:11];
    assign write_fast = map_entry[10];
    assign rdata = word_mem[word_index][8*addr[1:0] +: 8];
    assign write_phys = {map_entry[9:0], addr[7:0]};

    wire [MAP_INDEX_BITS:0] fill_map_index = {map_rw, page_set(map_addr[15:8])};
    wire [WORD_INDEX_BITS-1:0] snoop_index = word_set(snoop_addr);
    wire [WORD_INDEX_BITS-1:0] fill_index = word_set(byte_addr);
    // Physical-page identity survives address-table eviction and separate
    // RAMRD/RAMWRT mappings. A write to the other bank leaves this line alone.
    wire snoop_hit = word_valid_q[snoop_index] &&
        word_tag_mem[snoop_index] == {snoop_addr[15:BYTE_INDEX_BITS], snoop_phys[17:8]};

    // No reset on LUT RAM contents: the separate valid bits own lifetime.
    // Fill and snoop are mutually exclusive in the wrapper (a slow read
    // response versus a CPU write), giving the byte array one write port.
    always_ff @(posedge clk) begin
        if (map_fill && !invalidate)
            map_mem[fill_map_index] <= {map_addr[15:8], map_fast_write, map_phys[17:8]};
        if (!invalidate) begin
            if (byte_fill && !snoop_write)
                word_tag_mem[fill_index] <= {byte_addr[15:BYTE_INDEX_BITS], byte_phys[17:8]};
        end
    end

    for (genvar lane = 0; lane < 4; lane++) begin : data_lane
        always_ff @(posedge clk) begin
            if (!invalidate) begin
                if (snoop_write && snoop_hit && snoop_addr[1:0] == 2'(lane))
                    word_mem[snoop_index][8*lane +: 8] <= snoop_data;
                else if (byte_fill && !snoop_write)
                    word_mem[fill_index][8*lane +: 8] <= word_data[8*lane +: 8];
            end
        end
    end

    always_ff @(posedge clk) begin
        if (!rstn || invalidate) begin
            map_valid_q <= '0;
            word_valid_q <= '0;
        end
        else begin
            if (map_fill)
                map_valid_q[fill_map_index] <= 1'b1;
            if (byte_fill && !snoop_write)
                word_valid_q[fill_index] <= 1'b1;
        end
    end
endmodule
