`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Module: sdd_bus_tap
//
// Full-rate Apple bus event tap for SuperDuperDisplay streaming.
//
// Packs one 32-bit SDD bus event per Apple bus cycle (ab_read.data_en) into
// the low half of an AppleCycleRecord-shaped 64-bit word and feeds a second
// apple_cycle_egress instance (own DDR ring on AXI HP2). SuperDuperDisplay's
// USB parser expects this event layout on register 0x1004:
//
//   [15:0]  addr
//   [16]    rw            (1 = read)
//   [17]    res           (RES# pin level; 0 = in reset)
//   [18]    m2sel
//   [19]    m2b0
//   [27:20] data
//   [28]    always 1 (events are never all-zero; zero = ring gap)
//   [29]    route_rom   (tracker routed this access to ROM)
//   [30]    bank_nonzero (tracker decoded a nonzero PSRAM bank)
//   [31]    route_cache (tracker routed this access to CACHE/serve)
//
// The record kind field [63:61] is set to SDD_RECORD_KIND so the PS-side
// reader can sanity-check records; an all-zero record remains reserved as
// the egress overflow gap marker (this tap never produces one because
// [28] is always 1).
//
// Unlike the renderer capture path, this tap is NOT reset by Apple RES# --
// SuperDuperDisplay needs to observe the res bit toggling to detect
// reboots. The FIFO is held in reset while disabled so stale events never
// leak into a fresh enable.
//////////////////////////////////////////////////////////////////////////////////

module sdd_bus_tap (
    input  logic                                    clk,
    input  logic                                    resetn,

    input  logic                                    enable,
    input  globals::AppleBus_read                   ab_read,
    /* {route_cache, bank_nonzero, route_rom} sampled from the live
     * soft-switch translator -- lets the PS audit routing decisions
     * against a reference //e memory map per bus event. */
    input  logic [2:0]                              route_info,

    // Consumer interface (drains into a dedicated apple_cycle_egress)
    output apple_cycle_capture_pkg::AppleCycleRecord cycle_capture_data,
    output logic                                    cycle_capture_empty,
    input  logic                                    cycle_capture_rd_en,

    output logic                                    capture_drop_sticky,
    input  logic                                    capture_drop_ack
);

    localparam int FIFO_DEPTH = 4096;
    localparam int FIFO_WIDTH = $bits(apple_cycle_capture_pkg::AppleCycleRecord);

    localparam logic [2:0] SDD_RECORD_KIND = 3'd3;

    logic [31:0] event_word;
    assign event_word = {route_info, 1'b1,
                         ab_read.data,
                         ab_read.m2b0, ab_read.m2sel, ab_read.res, ab_read.rw,
                         ab_read.addr};

    wire [FIFO_WIDTH-1:0] record_din =
        {SDD_RECORD_KIND, {(FIFO_WIDTH - 3 - 32){1'b0}}, event_word};

    /* Hardware storm trap: a BRK/IRQ storm fetches $FFFE every few
     * events; four such fetches with <256-event gaps freezes the tap
     * IN THE SAME CYCLE, preserving the pre-storm ring (the software
     * trap's poll latency let the storm overwrite all history).
     * Normal interrupts (one $FFFE per ~17k events at 60 Hz) never
     * cluster; the quiet counter resets the streak. Re-arm by
     * toggling enable. */
    logic [2:0] vec_streak_q;
    logic [7:0] vec_quiet_q;
    logic       storm_frozen_q;
    /* Jam detector: a KIL-parked 6502 repeats one address forever
     * (bench: R FFFF for the whole ring, onset lost). 64 identical
     * consecutive addresses never occur in real execution. */
    logic [15:0] jam_addr_q;
    logic [5:0]  jam_count_q;
    wire vec_fetch = ab_read.data_en && ab_read.rw &&
                     (ab_read.addr == 16'hFFFE);
    always_ff @(posedge clk) begin
        if (!resetn || !enable) begin
            vec_streak_q   <= 3'd0;
            vec_quiet_q    <= 8'd0;
            storm_frozen_q <= 1'b0;
            jam_addr_q     <= 16'h0000;
            jam_count_q    <= 6'd0;
        end else if (!storm_frozen_q && ab_read.data_en) begin
            if (ab_read.addr == jam_addr_q) begin
                if (jam_count_q == 6'd63) begin
                    storm_frozen_q <= 1'b1;
                end else begin
                    jam_count_q <= jam_count_q + 6'd1;
                end
            end else begin
                jam_addr_q  <= ab_read.addr;
                jam_count_q <= 6'd0;
            end
            if (vec_fetch) begin
                vec_quiet_q <= 8'd0;
                if (vec_streak_q == 3'd3) begin
                    storm_frozen_q <= 1'b1;
                end else begin
                    vec_streak_q <= vec_streak_q + 3'd1;
                end
            end else begin
                if (vec_quiet_q == 8'hFF) begin
                    vec_streak_q <= 3'd0;
                end else begin
                    vec_quiet_q <= vec_quiet_q + 8'd1;
                end
            end
        end
    end

    wire push_request = enable && ab_read.data_en && !storm_frozen_q;

    logic                  fifo_full;
    logic                  fifo_empty;
    logic [FIFO_WIDTH-1:0] fifo_dout;

    wire fifo_wr_en = push_request && !fifo_full;
    wire fifo_rd_en = cycle_capture_rd_en && !fifo_empty;

    // Overflow latches until the egress acks (it emits a gap marker into
    // the ring, which the PS reader translates into the SDD overflow
    // protocol on register 0x1000).
    logic capture_drop_sticky_q;
    always_ff @(posedge clk) begin
        if (~resetn || !enable) begin
            capture_drop_sticky_q <= 1'b0;
        end else begin
            if (push_request && fifo_full)
                capture_drop_sticky_q <= 1'b1;
            else if (capture_drop_ack)
                capture_drop_sticky_q <= 1'b0;
        end
    end
    assign capture_drop_sticky = capture_drop_sticky_q;

    assign cycle_capture_data  = apple_cycle_capture_pkg::AppleCycleRecord'(fifo_dout);
    assign cycle_capture_empty = fifo_empty;

    // USE_ADV_FEATURES bit 2 enables wr_data_count (same as the renderer
    // capture FIFO). Held in reset while disabled so a re-enable starts
    // from an empty FIFO.
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
    ) sdd_fifo_inst (
        .rst            (~resetn || !enable),
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
