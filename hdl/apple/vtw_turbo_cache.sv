`timescale 1ns / 1ps
// Small, asynchronous TURBO caches. The address table and word cache have
// independent fills: a store needs only a writable address-table entry.
// Pages 0 and 1 (zero page and stack) have a separate direct store.
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
    // A writer outside the CPU changed the RAM word of drop_addr (an Apple
    // address below $C000, where the physical offset is the address).
    // Only that word leaves the caches; no fill is taken in this clock.
    input  logic        drop,
    input  logic [15:0] drop_addr,
    // No fill or snoop write in this clock (a flush or a drop is active).
    input  logic        hold,
    input  logic [15:0] addr,
    input  logic        rw,
    output logic        read_valid,
    output logic [15-BYTE_INDEX_BITS:0] read_tag,
    output logic        write_valid,
    output logic [7:0]  write_tag,
    output logic        write_fast,
    output logic [7:0]  rdata,
    // The two bytes after rdata, when they are in the same cache word:
    // rahead[7:0] at addr + 1, rahead[15:8] at addr + 2.
    output logic [15:0] rahead,
    output logic [1:0]  rahead_valid,
    output logic [17:0] write_phys,

    // Second read port of the word cache, for the code prefetch.
    input  logic [15:0] addr2,
    output logic        read2_valid,
    output logic [15-BYTE_INDEX_BITS:0] read2_tag,
    output logic [7:0]  rdata2,
    output logic [31:0] rword2,   // the whole word of addr2
    // Third read port of the word cache: the word after that of addr2.
    input  logic [15:0] addr3,
    output logic        read3_valid,
    output logic [15-BYTE_INDEX_BITS:0] read3_tag,
    output logic [31:0] rword3,

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
    (* ram_style = "distributed" *) logic [TAG_BITS+9:0] word_tag_mem [0:WORD_COUNT-1];
    wire [31:0] read_word;
    wire [31:0] read2_word;
    wire [31:0] read3_word;

    /* Zero page and stack. These two pages hold the working registers of
     * 6502 code, and in the shared word cache they both miss often and push
     * code out. Here each word of the two pages has its own place: 128
     * words, addressed directly, with no eviction. The valid bits and the
     * invalidation are the same as for the word cache. The physical page of
     * the fill is kept, so that a write snoops only the bank it was read
     * from. Code in these pages gets no prefetch. */
    localparam int LOW_INDEX_BITS = 7;
    localparam int LOW_COUNT = 1 << LOW_INDEX_BITS;
    (* ram_style = "distributed" *) logic [9:0] low_phys_mem [0:LOW_COUNT-1];
    logic [LOW_COUNT-1:0] low_valid_q;
    wire [31:0] low_word;
    wire low_page = (addr[15:9] == 7'd0);
    wire [LOW_INDEX_BITS-1:0] low_index = addr[8:2];
    wire fill_low = (byte_addr[15:9] == 7'd0);
    wire snoop_low = (snoop_addr[15:9] == 7'd0);
    wire [LOW_INDEX_BITS-1:0] low_fill_index = byte_addr[8:2];
    wire [LOW_INDEX_BITS-1:0] low_snoop_index = snoop_addr[8:2];
    wire low_snoop_hit = snoop_low && low_valid_q[low_snoop_index] &&
        low_phys_mem[low_snoop_index] == snoop_phys[17:8];
    wire low_fill = byte_fill && !snoop_write && fill_low;
    logic [MAP_COUNT-1:0] map_valid_q;
    wire block = hold;
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
    // Pages 0 and 1 have their own store (see below). Its tag is the
    // address itself: a valid entry is always the byte that was asked for.
    assign read_valid = low_page ? low_valid_q[low_index]
                                 : word_valid_q[word_index];
    assign read_tag = low_page ? addr[15:BYTE_INDEX_BITS]
                               : word_tag[TAG_BITS+9:10];
    assign write_valid = map_valid_q[map_index];
    assign write_tag = map_entry[18:11];
    assign write_fast = map_entry[10];
    assign rdata = low_page ? low_word[8*addr[1:0] +: 8]
                            : read_word[8*addr[1:0] +: 8];
    assign write_phys = {map_entry[9:0], addr[7:0]};
    wire [1:0] ahead_sel1 = addr[1:0] + 2'd1;
    wire [1:0] ahead_sel2 = addr[1:0] + 2'd2;
    assign rahead = {read_word[8*ahead_sel2 +: 8], read_word[8*ahead_sel1 +: 8]};
    assign rahead_valid = {!low_page && !addr[1],
                           !low_page && addr[1:0] != 2'd3};

    wire [WORD_INDEX_BITS-1:0] word2_index = word_set(addr2);
    wire [TAG_BITS+9:0] word2_tag = word_tag_mem[word2_index];
    assign read2_valid = word_valid_q[word2_index] && addr2[15:9] != 7'd0;
    assign read2_tag = word2_tag[TAG_BITS+9:10];
    assign rdata2 = read2_word[8*addr2[1:0] +: 8];
    assign rword2 = read2_word;

    wire [WORD_INDEX_BITS-1:0] word3_index = word_set(addr3);
    wire [TAG_BITS+9:0] word3_tag = word_tag_mem[word3_index];
    assign read3_valid = word_valid_q[word3_index] && addr3[15:9] != 7'd0;
    assign read3_tag = word3_tag[TAG_BITS+9:10];
    assign rword3 = read3_word;

    wire [MAP_INDEX_BITS:0] fill_map_index = {map_rw, page_set(map_addr[15:8])};
    wire [WORD_INDEX_BITS-1:0] snoop_index = word_set(snoop_addr);
    wire [WORD_INDEX_BITS-1:0] fill_index = word_set(byte_addr);
    // Physical-page identity survives address-table eviction and separate
    // RAMRD/RAMWRT mappings. A write to the other bank leaves this line alone.
    wire snoop_hit = !snoop_low && word_valid_q[snoop_index] &&
        word_tag_mem[snoop_index] == {snoop_addr[15:BYTE_INDEX_BITS], snoop_phys[17:8]};

    // No reset on LUT RAM contents: the separate valid bits own lifetime.
    // Fill and snoop are mutually exclusive in the wrapper (a slow read
    // response versus a CPU write), giving the byte array one write port.
    always_ff @(posedge clk) begin
        if (map_fill && !block)
            map_mem[fill_map_index] <= {map_addr[15:8], map_fast_write, map_phys[17:8]};
        if (!block) begin
            if (byte_fill && !snoop_write && !fill_low)
                word_tag_mem[fill_index] <= {byte_addr[15:BYTE_INDEX_BITS], byte_phys[17:8]};
            if (low_fill)
                low_phys_mem[low_fill_index] <= byte_phys[17:8];
        end
    end

    // Give each lane one inferred LUT-RAM write port. Separate arrays and
    // an explicit address mux prevent fill/snoop from becoming two ports
    // on a shared word array and falling back to registers.
    wire data_fill = byte_fill && !snoop_write && !fill_low;
    wire [LOW_INDEX_BITS-1:0] low_write_index =
        snoop_write ? low_snoop_index : low_fill_index;
    wire [WORD_INDEX_BITS-1:0] data_write_index = snoop_write ? snoop_index : fill_index;
    for (genvar lane = 0; lane < 4; lane++) begin : data_lane
        (* ram_style = "distributed" *) logic [7:0] byte_mem [0:WORD_COUNT-1];
        wire lane_write = !block &&
            (data_fill || (snoop_write && snoop_hit && snoop_addr[1:0] == 2'(lane)));
        wire [7:0] lane_wdata = snoop_write ? snoop_data : word_data[8*lane +: 8];
        assign read_word[8*lane +: 8] = byte_mem[word_index];
        assign read2_word[8*lane +: 8] = byte_mem[word2_index];
        assign read3_word[8*lane +: 8] = byte_mem[word3_index];
        always_ff @(posedge clk) begin
            if (lane_write)
                byte_mem[data_write_index] <= lane_wdata;
        end

        (* ram_style = "distributed" *) logic [7:0] low_mem [0:LOW_COUNT-1];
        wire low_lane_write = !block &&
            (low_fill || (snoop_write && low_snoop_hit && snoop_addr[1:0] == 2'(lane)));
        assign low_word[8*lane +: 8] = low_mem[low_index];
        always_ff @(posedge clk) begin
            if (low_lane_write)
                low_mem[low_write_index] <= lane_wdata;
        end
    end

    always_ff @(posedge clk) begin
        if (!rstn || invalidate) begin
            map_valid_q <= '0;
            word_valid_q <= '0;
            low_valid_q <= '0;
        end
        else if (drop) begin
            // Both stores can hold the word only at the place of its own
            // address, so one valid bit in each is enough.
            word_valid_q[word_set(drop_addr)] <= 1'b0;
            if (drop_addr[15:9] == 7'd0)
                low_valid_q[drop_addr[8:2]] <= 1'b0;
        end
        // A hold blocks the tag and data writes. Keep the valid bits
        // unchanged so a blocked fill cannot expose an old entry.
        else if (!block) begin
            if (map_fill)
                map_valid_q[fill_map_index] <= 1'b1;
            if (byte_fill && !snoop_write && !fill_low)
                word_valid_q[fill_index] <= 1'b1;
            if (low_fill)
                low_valid_q[low_fill_index] <= 1'b1;
        end
    end
endmodule
