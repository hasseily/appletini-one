////////////////////////////////////////////////////////////////////////////////
// Framebuffer Reader - AXI HP0 Read Master + Async FIFO
//
// Reads a 1920x1080 RGB565 framebuffer from DDR via AXI HP0 and feeds
// 16-bit pixels through an async FIFO into the pixel clock domain.
// Each 64-bit AXI beat carries four pixels (lowest pixel = beat[15:0],
// ascending).
//
// At vblank-start, fb_reader latches `base_addr_in` (set by the PS via
// FB_BASE_ADDR_REG) and uses it as the DDR base for the next frame's
// scanout. `last_latched_addr` reports back what was committed so the
// PS can avoid stomping the live frame; `vblank_latched_pulse` ticks
// once per latch.
////////////////////////////////////////////////////////////////////////////////

`timescale 1ns / 1ps

module fb_reader (
    // clk signals
    input  logic         clk,
    input  logic         resetn,

    Axi3_read_if.master axi_read_if,

    // Frame-base handshake with the PS compositor.
    input  logic [31:0]  base_addr_in,
    output logic [31:0]  last_latched_addr,
    output logic         vblank_latched_pulse,

    input  logic         vblank_start,

    // pixel_clk signals.  (There is no pixel-domain FF in this module;
    // the FIFO's single `rst` input handles both write and read domains
    // via its built-in xpm_fifo_rst synchroniser, so no pixel_resetn is
    // needed here.)
    input  logic         pixel_clk,

    input  logic         pixel_rd_en,
    output logic [15:0]  pixel_rgb565,

    // Sticky error flag: set whenever an accepted read beat arrives with
    // rresp != OKAY/EXOKAY (i.e. SLVERR / DECERR).  Cleared only by
    // module reset.  Consumed by video_top status register.
    output logic         axi_read_err,

    /* Diagnostic outputs (no functional effect; sampled into AXI
     * status register so PS can see whether fb_reader is wedged). */
    output logic [2:0]   dbg_state,
    output logic [17:0]  dbg_burst_count,
    output logic [15:0]  dbg_axi_err_count,
    /* Scanout starvation events (FIFO underflow while the DVI side was
     * reading). One count per starvation episode, not per cycle. */
    output logic [15:0]  dbg_underrun_count
);

    import video_pkg::*;

    //----------------------------------------------------------------------
    // AXI3 read channel constant signals
    //----------------------------------------------------------------------
    assign axi_read_if.arburst = 2'b01;
    assign axi_read_if.arsize  = 3'b011;
    assign axi_read_if.arlen   = 4'd15;
    // we set rready always to 1, to avoid stalls.  If our fifo passes
    // prog_full, we stop issuing read requests instead of stalling on
    // read acceptance.
    assign axi_read_if.rready  = 1;

    //----------------------------------------------------------------------
    // Parameters
    //----------------------------------------------------------------------
    localparam FRAME_BYTES      = VIDEO_ACTIVE_W * VIDEO_ACTIVE_H * FB_BYTES_PER_PIXEL;
    localparam BURST_BYTES      = AXI_HP0_BURST_BYTES;
    localparam BURSTS_PER_FRAME = FRAME_BYTES / BURST_BYTES;   // 32,400 @ 1920x1080 RGB565

    //----------------------------------------------------------------------
    // AXI read master state machine
    //
    // Pipelined burst issuance: up to MAX_OUTSTANDING bursts may have
    // their AR accepted before the first R-channel completion. This
    // hides DDR controller AR-to-first-beat latency behind ongoing
    // data beats and roughly doubles fb_reader's HP0 throughput.
    // 1080p60 RGB565 needs 4.15 MB/frame = 249 MB/s sustained.
    //----------------------------------------------------------------------
    localparam S_IDLE        = 3'd0;
    localparam S_RESET_FIFO  = 3'd1;
    localparam S_BURST       = 3'd2;
    localparam int MAX_OUTSTANDING = 8;

    reg [2:0]  state;
    reg [31:0] burst_addr;       // address of next AR to issue
    reg [17:0] bursts_issued;    // count of ARs accepted for this frame
    reg [17:0] bursts_completed; // count of bursts whose rlast we've seen
    reg [3:0]  outstanding;      // bursts_issued - bursts_completed
    reg vblank_latched;

    // FIFO signals (write side in axi_clk domain)
    logic        fifo_wr_en;
    logic [63:0] fifo_wr_data;
    logic        fifo_prog_full;
    logic        fifo_full;
    logic        fifo_wr_rst_busy;

    // FIFO signals (read side in pixel_clk domain)
    logic        fifo_rd_en;
    logic [15:0] fifo_rd_data;

    // FIFO reset (asserted in axi_clk domain)
    reg  fifo_reset_axi;
    reg [3:0] fifo_reset_count;

    // Write side: accept AXI read data directly into FIFO.
    assign fifo_wr_en   = axi_read_if.rvalid && !fifo_full;
    assign fifo_wr_data = axi_read_if.rdata;

    /* AXI read-response error counter (wrapping). axi_read_err is the
     * sticky one-bit alias of "any error ever"; the counter wraps so
     * the PS can compute a delta-per-second and tell one-time-at-
     * startup from ongoing-issue. rresp[1] is set for SLVERR (10)
     * and DECERR (11). */
    logic [15:0] axi_read_err_count;
    always_ff @(posedge clk) begin
        if (!resetn) begin
            axi_read_err       <= 1'b0;
            axi_read_err_count <= 16'd0;
        end else if (axi_read_if.rvalid && axi_read_if.rready
                     && axi_read_if.rresp[1]) begin
            axi_read_err <= 1'b1;
            axi_read_err_count <= axi_read_err_count + 16'd1;
        end
    end

    // Read side: drain pixels during display enable.
    assign fifo_rd_en   = pixel_rd_en;
    assign pixel_rgb565 = fifo_rd_data;

    /* Underrun event detection (pixel_clk domain): the FIFO 'underflow'
     * output pulses on rd_en-while-empty. Latch episode starts into a
     * toggle, sync the toggle into clk, count edges there. */
    logic fifo_underflow;
    logic underflow_d;
    logic underrun_toggle_px;
    always_ff @(posedge pixel_clk) begin
        underflow_d <= fifo_underflow;
        if (fifo_underflow && !underflow_d) begin
            underrun_toggle_px <= ~underrun_toggle_px;
        end
    end

    logic underrun_toggle_clk;
    logic underrun_toggle_clk_d;
    xpm_cdc_single #(
        .DEST_SYNC_FF   (2),
        .INIT_SYNC_FF   (0),
        .SIM_ASSERT_CHK (0),
        .SRC_INPUT_REG  (0)
    ) underrun_toggle_cdc (
        .src_clk  (pixel_clk),
        .src_in   (underrun_toggle_px),
        .dest_clk (clk),
        .dest_out (underrun_toggle_clk)
    );

    always_ff @(posedge clk) begin
        if (!resetn) begin
            underrun_toggle_clk_d <= 1'b0;
            dbg_underrun_count    <= 16'd0;
        end else begin
            underrun_toggle_clk_d <= underrun_toggle_clk;
            if (underrun_toggle_clk != underrun_toggle_clk_d) begin
                dbg_underrun_count <= dbg_underrun_count + 16'd1;
            end
        end
    end

    /* Diagnostic outputs. */
    assign dbg_state         = state;
    assign dbg_burst_count   = bursts_completed;
    assign dbg_axi_err_count = axi_read_err_count;

    /* Combinational helpers */
    wire ar_handshake     = axi_read_if.arvalid && axi_read_if.arready;
    wire rlast_accepted   = axi_read_if.rvalid  && axi_read_if.rready
                                                && axi_read_if.rlast;
    wire can_issue_burst  = !fifo_prog_full
                            && (outstanding < MAX_OUTSTANDING[3:0])
                            && (bursts_issued < BURSTS_PER_FRAME[17:0])
                            && !axi_read_if.arvalid;

    always @(posedge clk) begin
        if (!resetn) begin
            state                <= S_IDLE;
            axi_read_if.arvalid  <= 1'b0;
            axi_read_if.araddr   <= 32'h0;
            burst_addr           <= 32'h0;
            bursts_issued        <= 18'h0;
            bursts_completed     <= 18'h0;
            outstanding          <= 4'h0;
            fifo_reset_axi       <= 0;
            fifo_reset_count     <= 0;
            vblank_latched       <= 0;
            last_latched_addr    <= 32'h0;
            vblank_latched_pulse <= 1'b0;
        end else begin
            // Default: deassert after handshake.
            if (ar_handshake)
                axi_read_if.arvalid <= 1'b0;

            vblank_latched_pulse <= 1'b0;

            /* Only arm the next-frame latch once the PS has published a
             * non-zero base address. At reset base_addr_in is 0x0, which
             * points at the Zynq vector tables / FSBL state -- reading
             * from there returns garbage and storms SLVERR responses.
             * Holding S_IDLE until the PS compositor writes a real slot
             * address keeps fb_reader (and the FIFO/DVI output) in a
             * known-zero state across the early-boot window. */
            if (vblank_start && base_addr_in != 32'h0) begin
                vblank_latched <= 1;
            end

            /* Outstanding tracking. AR handshake increments; rlast
             * decrements. Same cycle is a no-op (both edges flip). */
            case ({ar_handshake, rlast_accepted})
                2'b10: outstanding <= outstanding + 4'd1;
                2'b01: outstanding <= outstanding - 4'd1;
                default: ;
            endcase

            case (state)
                S_IDLE: begin
                    if (vblank_latched) begin
                        vblank_latched       <= 0;
                        burst_addr           <= base_addr_in;
                        last_latched_addr    <= base_addr_in;
                        vblank_latched_pulse <= 1'b1;
                        bursts_issued        <= 18'd0;
                        bursts_completed     <= 18'd0;
                        outstanding          <= 4'd0;
                        fifo_reset_axi       <= 1;
                        fifo_reset_count     <= 4'd8;
                        state                <= S_RESET_FIFO;
                    end
                end

                S_RESET_FIFO: begin
                    if (fifo_reset_count == 0) begin
                        fifo_reset_axi <= 1'b0;
                        if (!fifo_wr_rst_busy) begin
                            state <= S_BURST;
                        end
                    end else begin
                        fifo_reset_count <= fifo_reset_count - 1'b1;
                    end
                end

                /* S_BURST -- pipelined. Issue the next AR whenever
                 * there's room (outstanding < MAX_OUTSTANDING) and
                 * the FIFO has space, regardless of whether prior
                 * bursts' R-channel data has finished. Track AR
                 * handshakes and rlast acceptances independently
                 * to know when the frame is complete.
                 *
                 * On vblank, abandon the in-flight bursts and bail
                 * back to S_IDLE. The PL FIFO will be reset at the
                 * start of the next frame; outstanding R-channel
                 * data is silently absorbed by the FIFO until the
                 * reset clears it. */
                S_BURST: begin
                    if (vblank_latched) begin
                        state <= S_IDLE;
                    end else begin
                        if (can_issue_burst) begin
                            axi_read_if.araddr  <= burst_addr;
                            axi_read_if.arvalid <= 1'b1;
                            burst_addr          <= burst_addr + BURST_BYTES;
                            bursts_issued       <= bursts_issued + 18'd1;
                        end
                        if (rlast_accepted) begin
                            bursts_completed <= bursts_completed + 18'd1;
                            if (bursts_completed >= BURSTS_PER_FRAME[17:0] - 18'd1) begin
                                /* Full frame received; idle until next
                                 * vblank. */
                                state <= S_IDLE;
                            end
                        end
                    end
                end

                default: state <= S_IDLE;
            endcase
        end
    end

    //----------------------------------------------------------------------
    // XPM async FIFO
    // Write side: 64-bit @ clk        (one AXI beat = 8 bytes = 4 pixels)
    // Read side:  16-bit @ pixel_clk  (one RGB565 pixel per pop)
    //----------------------------------------------------------------------
    // USE_ADV_FEATURES enables only the optional outputs we actually
    // consume.  Enabling (then tying off) read-side features such as
    // prog_empty / rd_data_count / data_valid leaves their backing FFs
    // with no load, which in turn lets Vivado trim the read-domain
    // reset-sync path inside xpm_fifo_rst and emit Synth 8-7129.  Only
    // prog_full is actually read by this module, so enable only that
    // (bit 1 = 0x0002).
    xpm_fifo_async #(
        .CDC_SYNC_STAGES     (2),
        .DOUT_RESET_VALUE    ("0"),
        .ECC_MODE            ("no_ecc"),
        .FIFO_MEMORY_TYPE    ("block"),
        .FIFO_READ_LATENCY   (1),
        .FIFO_WRITE_DEPTH    (8192),
        .FULL_RESET_VALUE    (0),
        .PROG_EMPTY_THRESH   (10),
        .PROG_FULL_THRESH    (7680),
        .RD_DATA_COUNT_WIDTH (15),
        .READ_DATA_WIDTH     (16),
        .READ_MODE           ("fwft"),
        .RELATED_CLOCKS      (0),
        .SIM_ASSERT_CHK      (0),
        .USE_ADV_FEATURES    ("0102"),
        .WAKEUP_TIME         (0),
        .WRITE_DATA_WIDTH    (64),
        .WR_DATA_COUNT_WIDTH (14)
    ) fifo_inst (
        // Write side
        .wr_clk         (clk),
        .wr_en          (fifo_wr_en),
        .din            (fifo_wr_data),
        .full           (fifo_full),
        .prog_full      (fifo_prog_full),
        .wr_data_count  (),
        .overflow       (),
        .wr_rst_busy    (fifo_wr_rst_busy),
        .almost_full    (),
        .wr_ack         (),

        // Read side
        .rd_clk         (pixel_clk),
        .rd_en          (fifo_rd_en),
        .dout           (fifo_rd_data),
        .empty          (),
        .prog_empty     (),
        .rd_data_count  (),
        .underflow      (fifo_underflow),
        .rd_rst_busy    (),
        .almost_empty   (),
        .data_valid     (),

        // Reset
        .rst            (fifo_reset_axi),

        // Unused
        .sleep          (1'b0),
        .injectsbiterr  (1'b0),
        .injectdbiterr  (1'b0),
        .sbiterr        (),
        .dbiterr        ()
    );

endmodule
