`timescale 1ns / 1ps
module apple_cycle_capture (
    input  logic                            clk,
    input  logic                            resetn,
    input  logic                            soft_reset,
    input  globals::AppleBus_read           ab_read,
    input  globals::SoftSwitchState         sss,
    input  logic [8:0]                      line_in_frame,
    input  logic [6:0]                      cycle_in_line,
    input  logic                            frame_en,

    output apple_cycle_capture_pkg::AppleCycleRecord cycle_capture_data,
    input  logic                            cycle_capture_rd_en,
    output logic                            cycle_capture_empty,

    // Drop-sticky handshake. Asserted when a push_request was suppressed
    // by fifo_full; cleared when egress acks. Egress emits a gap marker
    // record into the PS-side ring on ack.
    output logic                            capture_drop_sticky,
    input  logic                            capture_drop_ack
);

    import apple_cycle_capture_pkg::*;

    /* 4096 entries hold roughly 4 ms of sustained 1 MHz Apple traffic.
     * This covers a gap marker, notification, and several ring bursts while
     * the PS poll is delayed; an overflow invalidates the renderer's frame.
     * The 64-bit FIFO consumes approximately eight BRAM18 blocks. */
    localparam int FIFO_DEPTH = 4096;
    localparam int FIFO_WIDTH = $bits(AppleCycleRecord);

    // Apple-bus video-write ranges: main text 0x000400-0x000BFF, main HGR
    // 0x002000-0x005FFF, aux text 0x010400-0x010BFF, aux HGR/SHR
    // 0x012000-0x019FFF. Aux text includes page 2 so DLORES/TEXT80 PAGE2
    // frames can render from the captured AUX $0800-$0BFF shadow.
    function automatic logic in_video_range(input logic [23:0] a);
        return ((a >= 24'h000400) && (a <= 24'h000BFF)) ||
               ((a >= 24'h002000) && (a <= 24'h005FFF)) ||
               ((a >= 24'h010400) && (a <= 24'h010BFF)) ||
               ((a >= 24'h012000) && (a <= 24'h019FFF));
    endfunction

    function automatic logic is_vidhd_register_write(input logic [15:0] a);
        return (a == 16'hC022) ||
               (a == 16'hC029) ||
               (a == 16'hC034) ||
               (a == 16'hC035);
    endfunction

    function automatic logic is_video7_an3_access(input logic [15:0] a);
        return (a == 16'hC05E) ||
               (a == 16'hC05F);
    endfunction

    /* ab_read.addr/rw are the authoritative PHI0-high sample (valid for
     * 6502 and DMA masters alike), and sss.addr_decode_late is their
     * translation -- both registered well before the data_en push below.
     * The early-snapshot fields exist only for the INH/PSRAM serving arm
     * and are never used for capture. */
    wire [15:0] cap_addr = ab_read.addr;
    wire        cap_rw   = ab_read.rw;
    wire [23:0] cap_addr_decode    = sss.addr_decode_late;
    wire        cap_addr_decode_en = sss.addr_decode_late_en;

    // Rule 1: qualifying bus write into video memory (in addr_decode space).
    logic rule1_valid;
    assign rule1_valid =
        (cap_rw == 1'b0) &&
        cap_addr_decode_en &&
        in_video_range(cap_addr_decode);

    logic vidhd_register_write;
    assign vidhd_register_write =
        ab_read.data_en &&
        (cap_rw == 1'b0) &&
        is_vidhd_register_write(cap_addr);

    logic video7_softswitch_access;
    assign video7_softswitch_access =
        ab_read.data_en &&
        is_video7_an3_access(cap_addr);

    logic io_push_request;
    assign io_push_request =
        vidhd_register_write ||
        video7_softswitch_access;

    // Fake-SHR on the Apple //e is selected by C029 only. While active,
    // the PS renderer builds complete frames from the captured AUX shadow,
    // so streaming every Apple cycle just overloads the capture path. Keep
    // memory/IO write records, and emit one frame marker per Apple frame.
    logic shr_capture_active_q;
    wire c029_write_cycle =
        ab_read.data_en &&
        (cap_rw == 1'b0) &&
        (cap_addr == 16'hC029);
    wire c029_write_shr_active = (ab_read.data[7:6] == 2'b11);
    wire shr_capture_active_next =
        c029_write_cycle ? c029_write_shr_active : shr_capture_active_q;
    wire shr_frame_marker =
        shr_capture_active_next &&
        (line_in_frame == 9'd0) &&
        (cycle_in_line == 7'd0);
    wire capture_frame_en =
        frame_en &&
        (!shr_capture_active_next || shr_frame_marker);

    always_ff @(posedge clk) begin
        if (~resetn || soft_reset) begin
            shr_capture_active_q <= 1'b0;
        end else if (c029_write_cycle) begin
            shr_capture_active_q <= c029_write_shr_active;
        end
    end

    logic apple_push_request;
    assign apple_push_request = ab_read.data_en && (rule1_valid || capture_frame_en);

    // Apple-bus record (frame and bus-write halves; each gated to zero when
    // its triggering condition isn't met).
    AppleCycleRecord apple_record_din;

    always_comb begin
        apple_record_din = '0;
        apple_record_din.record_kind = RECORD_KIND_LEGACY;

        if (capture_frame_en) begin
            apple_record_din.frame_en      = 1'b1;
            apple_record_din.line_in_frame = line_in_frame;
            apple_record_din.cycle_in_line = cycle_in_line;
            apple_record_din.sw_80store    = sss.sw_80store;
            apple_record_din.sw_ramrd      = sss.sw_ramrd;
            apple_record_din.sw_ramwrt     = sss.sw_ramwrt;
            apple_record_din.sw_altzp      = sss.sw_altzp;
            apple_record_din.sw_text       = sss.sw_text;
            apple_record_din.sw_mixed      = sss.sw_mixed;
            apple_record_din.sw_page2      = sss.sw_page2;
            apple_record_din.sw_hires      = sss.sw_hires;
            apple_record_din.sw_altcharset = sss.sw_altcharset;
            apple_record_din.sw_80col      = sss.sw_80col;
            apple_record_din.sw_dhires     = sss.sw_dhires;
        end

        if (rule1_valid) begin
            apple_record_din.addr_decode    = cap_addr_decode;
            apple_record_din.addr_decode_en = 1'b1;
            apple_record_din.data           = ab_read.data;
        end
    end

    AppleCycleRecord io_record_din;
    wire [10:0] current_softswitch_bits = {
        sss.sw_80store,
        sss.sw_ramrd,
        sss.sw_ramwrt,
        sss.sw_altzp,
        sss.sw_text,
        sss.sw_mixed,
        sss.sw_page2,
        sss.sw_hires,
        sss.sw_altcharset,
        sss.sw_80col,
        sss.sw_dhires
    };

    always_comb begin
        if (video7_softswitch_access) begin
            io_record_din = pack_softswitch_access_record(
                cap_addr,
                current_softswitch_bits,
                line_in_frame,
                cycle_in_line
            );
        end else begin
            io_record_din = pack_io_write_record(
                cap_addr,
                ab_read.data,
                line_in_frame,
                cycle_in_line
            );
        end
    end

    logic                  fifo_full;
    logic                  fifo_empty;
    logic [FIFO_WIDTH-1:0] fifo_dout;
    logic                  fifo_wr_en;
    logic                  fifo_rd_en;
    AppleCycleRecord       record_din;
    AppleCycleRecord       pending_record_q;
    logic                  pending_record_valid;

    always_comb begin
        if (pending_record_valid)
            record_din = pending_record_q;
        else if (io_push_request)
            record_din = io_record_din;
        else
            record_din = apple_record_din;
    end

    // Combined push request -- used for the drop-sticky check below.
    wire push_request =
        pending_record_valid ||
        io_push_request ||
        apple_push_request;

    assign fifo_wr_en = push_request && !fifo_full;
    assign fifo_rd_en = cycle_capture_rd_en && !fifo_empty;

    // Set wins. A full FIFO or a third simultaneous record source marks
    // the stream incomplete until egress acknowledges the gap.
    logic capture_drop_sticky_q;
    always_ff @(posedge clk) begin
        if (~resetn || soft_reset) begin
            capture_drop_sticky_q <= 1'b0;
            pending_record_valid  <= 1'b0;
            pending_record_q      <= '0;
        end else begin
            if (pending_record_valid && fifo_wr_en)
                pending_record_valid <= 1'b0;

            if (io_push_request && apple_push_request && !pending_record_valid && !fifo_full) begin
                pending_record_q     <= apple_record_din;
                pending_record_valid <= 1'b1;
            end

            if ((push_request && fifo_full && !pending_record_valid) ||
                (pending_record_valid &&
                 (io_push_request || apple_push_request)))
                capture_drop_sticky_q <= 1'b1;
            else if (capture_drop_ack)
                capture_drop_sticky_q <= 1'b0;
        end
    end
    assign capture_drop_sticky = capture_drop_sticky_q;

    assign cycle_capture_data  = AppleCycleRecord'(fifo_dout);
    assign cycle_capture_empty = fifo_empty;

    // USE_ADV_FEATURES bit 2 enables wr_data_count (per xpm_fifo.sv source:
    // localparam EN_WDC = EN_ADV_FEATURE[2]). Hex "0004" = bit 2.
    xpm_fifo_sync #(
        .FIFO_MEMORY_TYPE   ("block"),
        .FIFO_WRITE_DEPTH   (FIFO_DEPTH),
        .WRITE_DATA_WIDTH   (FIFO_WIDTH),
        .READ_DATA_WIDTH    (FIFO_WIDTH),
        .READ_MODE          ("fwft"),
        .FIFO_READ_LATENCY  (0),
        .USE_ADV_FEATURES   ("0004"),
        .WR_DATA_COUNT_WIDTH(13),
        .FULL_RESET_VALUE   (0),
        .DOUT_RESET_VALUE   ("0"),
        .ECC_MODE           ("no_ecc"),
        .WAKEUP_TIME        (0)
    ) cycle_fifo_inst (
        .rst            (~resetn || soft_reset),
        .wr_clk         (clk),
        .din            (record_din),
        .wr_en          (fifo_wr_en),
        .rd_en          (fifo_rd_en),
        .dout           (fifo_dout),
        .full           (fifo_full),
        .empty          (fifo_empty),
        .almost_empty   (), .almost_full   (), .data_valid    (),
        .dbiterr        (), .overflow      (), .prog_empty    (),
        .prog_full      (), .rd_data_count (), .rd_rst_busy   (),
        .sbiterr        (), .underflow     (), .wr_ack        (),
        .wr_data_count  (),
        .wr_rst_busy    (),
        .injectdbiterr  (1'b0), .injectsbiterr (1'b0), .sleep (1'b0)
    );

endmodule
