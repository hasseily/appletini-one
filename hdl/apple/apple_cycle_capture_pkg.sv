package apple_cycle_capture_pkg;

    // 64-bit packed record of one Apple bus cycle's relevant state, captured
    // at ab_read.data_en when either of two filter rules fires:
    //   (1) qualifying bus write (rw=0, addr_decode_en, addr in video ranges)
    //   (2) frame_en asserted (frame is one the PS will render)
    //
    // Record has two independently-nullable halves. Receiver uses
    // addr_decode_en and frame_en as per-half "valid" flags. When rule 1
    // does not fire, the bus-write half is zeroed; when frame_en is low,
    // the frame half is zeroed. An all-zero record is reserved as a gap
    // marker emitted by apple_cycle_egress on overflow.
    localparam logic [2:0] RECORD_KIND_LEGACY         = 3'b000;
    localparam logic [2:0] RECORD_KIND_IO_WRITE       = 3'b001;
    localparam logic [2:0] RECORD_KIND_SOFTSW_ACCESS  = 3'b010;

    typedef struct packed {
        logic [2:0]  record_kind;       // [63:61]

        // Frame half ([60:33]) -- valid when frame_en == 1
        logic        frame_en;          // [60]
        logic [8:0]  line_in_frame;     // [59:51]
        logic [6:0]  cycle_in_line;     // [50:44]
        logic        sw_80store;        // [43]
        logic        sw_ramrd;          // [42]
        logic        sw_ramwrt;         // [41]
        logic        sw_altzp;          // [40]
        logic        sw_text;           // [39]
        logic        sw_mixed;          // [38]
        logic        sw_page2;          // [37]
        logic        sw_hires;          // [36]
        logic        sw_altcharset;     // [35]
        logic        sw_80col;          // [34]
        logic        sw_dhires;         // [33]

        // Bus-write half ([32:0]) -- valid when addr_decode_en == 1
        logic [23:0] addr_decode;       // [32:9]
        logic        addr_decode_en;    // [8]
        logic [7:0]  data;              // [7:0]
    } AppleCycleRecord;

    // Expected width (informational; assertions live in modules that use the type).
    localparam int APPLE_CYCLE_RECORD_BITS = $bits(AppleCycleRecord);

    function automatic AppleCycleRecord pack_io_write_record(
        input logic [15:0] apple_addr,
        input logic [7:0]  data,
        input logic [8:0]  line_in_frame,
        input logic [6:0]  cycle_in_line
    );
        pack_io_write_record =
            AppleCycleRecord'({RECORD_KIND_IO_WRITE, apple_addr, data, line_in_frame, cycle_in_line, 21'd0});
    endfunction

    function automatic AppleCycleRecord pack_softswitch_access_record(
        input logic [15:0] apple_addr,
        input logic [10:0] softswitch_bits,
        input logic [8:0]  line_in_frame,
        input logic [6:0]  cycle_in_line
    );
        pack_softswitch_access_record = '0;
        pack_softswitch_access_record.record_kind     = RECORD_KIND_SOFTSW_ACCESS;
        pack_softswitch_access_record.line_in_frame   = line_in_frame;
        pack_softswitch_access_record.cycle_in_line   = cycle_in_line;
        pack_softswitch_access_record.sw_80store      = softswitch_bits[10];
        pack_softswitch_access_record.sw_ramrd        = softswitch_bits[9];
        pack_softswitch_access_record.sw_ramwrt       = softswitch_bits[8];
        pack_softswitch_access_record.sw_altzp        = softswitch_bits[7];
        pack_softswitch_access_record.sw_text         = softswitch_bits[6];
        pack_softswitch_access_record.sw_mixed        = softswitch_bits[5];
        pack_softswitch_access_record.sw_page2        = softswitch_bits[4];
        pack_softswitch_access_record.sw_hires        = softswitch_bits[3];
        pack_softswitch_access_record.sw_altcharset   = softswitch_bits[2];
        pack_softswitch_access_record.sw_80col        = softswitch_bits[1];
        pack_softswitch_access_record.sw_dhires       = softswitch_bits[0];
        pack_softswitch_access_record.addr_decode     = {8'd0, apple_addr};
    endfunction

endpackage
