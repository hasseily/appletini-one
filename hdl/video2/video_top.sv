/*
 * video_top -- DVI scanout subsystem.
 *
 * Reads RGB565 framebuffer pixels from PS DDR over AXI HP0 via fb_reader
 * and drives them straight onto the 5:6:5 DVI output pins through
 * video_timing_gen at the selected resolution. The framebuffer and wire
 * are the same 16 bits end to end.
 *
 * The PS compositor produces frames into one of three slots in PS DDR
 * and tells the PL which slot to scan via FB_BASE_ADDR_REG. fb_reader
 * latches the new base at vblank-start; FB_LAST_LATCHED_REG reflects
 * what the PL committed to. FB_STATUS_REG counts vblank latches so the
 * PS can wait between publishes.
 */

module video_top (
    input logic clk,
    input logic resetn,
    input globals::AxiSimple_common as_common,
    AxiSimple_if.client as_client,
    input logic pixel_clk,
    input logic pixel_resetn,
    input logic pixel_locked,
    output logic clock_resetn,

    // Dedicated Clocking Wizard AXI-Lite write master. No PS address map
    // exposure: only the checked mode presets can program the pixel clock.
    output logic [10:0] clock_awaddr,
    output logic clock_awvalid,
    input logic clock_awready,
    output logic [31:0] clock_wdata,
    output logic clock_wvalid,
    input logic clock_wready,
    input logic [1:0] clock_bresp,
    input logic clock_bvalid,
    output logic clock_bready,

    /* Apple-side timing inputs that drive the DVI timing genlock. */
    input logic apple_video_mode_50hz,
    input logic apple_vblank_start_pulse,

    Axi3_read_if.master axi_read_if,

    output logic [4:0]   dvi_red,
    output logic [5:0]   dvi_grn,
    output logic [4:0]   dvi_blu,
    output logic         dvi_clk,
    output logic         dvi_de,
    output logic         dvi_hsync,
    output logic         dvi_vsync
);
    import video_pkg::*;

    // ------------------------------------------------------------------
    // FB control registers (offsets 0x00..0x0C). PS writes the base
    // address of the slot to scan into FB_BASE_ADDR_REG; fb_reader
    // latches it at vblank-start. FB_STATUS_REG is a vblank frame
    // counter; FB_LAST_LATCHED_REG is the base address fb_reader
    // committed to scanning at the most recent vblank.
    // ------------------------------------------------------------------
    /* axisimple_wrapper hands as_common.awaddr = ps_address[9:2], so
     * register offsets here are word-indexed (not byte-indexed): PS
     * write to base+0x04 lands as awaddr=8'h01, base+0x08 -> 8'h02. */
    localparam [7:0] FB_REGIDX_BASE_ADDR    = 8'h00;
    localparam [7:0] FB_REGIDX_CONTROL      = 8'h01; // reserved
    localparam [7:0] FB_REGIDX_STATUS       = 8'h02;
    localparam [7:0] FB_REGIDX_LAST_LATCHED = 8'h03;
    localparam [7:0] FB_REGIDX_DEBUG        = 8'h04;
    localparam [7:0] FB_REGIDX_DEBUG2       = 8'h05;
    localparam [7:0] FB_REGIDX_MODE_REQUEST = 8'h06;
    localparam [7:0] FB_REGIDX_MODE_STATUS  = 8'h07;
    localparam [7:0] FB_REGIDX_MODE_BASE    = 8'h08;
    localparam [7:0] FB_REGIDX_MASK_CAP     = 8'h09; // MSK1 at byte 0x24
    // Stage a 128-byte-aligned MODE_BASE, then write MODE_REQUEST[3:0].
    // STATUS: [31:24]=A9, [11]=held, [10]=locked, [9]=error,
    // [8]=busy, [3:0]=active mode. A successful fallback retains error.
    // Request/base writes while busy are ignored. FB_BASE changes only
    // when the new mode and staged base commit together after clock lock.
    /* DEBUG register layout (read-only):
     *   [2:0]   fb_reader FSM state
     *   [3]     axi_read_err sticky
     *   [25:8]  fb_reader burst_count (current frame's progress)
     * DEBUG2 register layout (read-only):
     *   [31:16] scanout underrun episodes (FIFO empty mid-scan)
     *   [15:0]  saturating count of AXI read-response errors */

    logic [31:0] fb_base_addr_q;
    logic [31:0] fb_frame_count_q;
    logic [31:0] fb_last_latched_q;
    logic [31:0] mode_base_q;
    logic [3:0] requested_mode_q;
    logic mode_request;
    logic [3:0] active_mode;
    logic mode_busy, mode_error, mode_commit;
    logic [31:0] mode_committed_base;
    logic reader_pause, reader_quiescent, video_hold;
    logic clock_locked_clk;
    logic video_resetn;

    cdc_bit_sync cdc_pixel_locked_i (
        .clk(clk), .resetn(resetn), .din(pixel_locked), .dout(clock_locked_clk)
    );
    reset_sync video_reset_sync_i (
        .clk(pixel_clk),
        .arst_n(pixel_resetn && resetn && !video_hold),
        .srst_n(video_resetn)
    );

    // ------------------------------------------------------------------
    // DVI timing generator -- pixel_clk domain
    // ------------------------------------------------------------------
    logic apple_video_mode_50hz_pixel;
    logic video_hsync_i;
    logic video_vsync_i;
    logic video_de_i;
    logic [11:0] video_h_count;
    logic [11:0] video_v_count;
    logic video_vblank_start;
    logic apple_vblank_start_pixel;

    cdc_bit_sync cdc_apple_video_mode_50hz_i (
        .clk(pixel_clk),
        .resetn(video_resetn),
        .din(apple_video_mode_50hz),
        .dout(apple_video_mode_50hz_pixel)
    );

    cdc_pulse_toggle cdc_apple_vblank_start_i (
        .src_clk   (clk),
        .src_resetn(resetn),
        .src_pulse (apple_vblank_start_pulse),
        .dst_clk   (pixel_clk),
        .dst_resetn(video_resetn),
        .dst_pulse (apple_vblank_start_pixel)
    );

    video_timing_gen video_timing (
        .clk_pixel    (pixel_clk),
        .rst_n        (video_resetn),
        .output_mode  (active_mode),
        .mode_1080p50 (apple_video_mode_50hz_pixel),
        .genlock_vblank_start(apple_vblank_start_pixel),
        .hsync        (video_hsync_i),
        .vsync        (video_vsync_i),
        .de           (video_de_i),
        .h_count      (video_h_count),
        .v_count      (video_v_count),
        .vblank_start (video_vblank_start)
    );

    /* CDC the pixel-clock-domain vblank pulse into clk for fb_reader. */
    logic video_vblank_start_clk;
    cdc_pulse_toggle cdc_video_vblank_start_i (
        .src_clk   (pixel_clk),
        .src_resetn(video_resetn),
        .src_pulse (video_vblank_start),
        .dst_clk   (clk),
        .dst_resetn(resetn),
        .dst_pulse (video_vblank_start_clk)
    );

    // ------------------------------------------------------------------
    // fb_reader -- reads the RGB565 framebuffer over AXI HP0, feeds
    // pixels through an async FIFO into the pixel clock domain.
    // ------------------------------------------------------------------
    logic        fb_pixel_rd_en;
    logic [15:0] fb_pixel;       // RGB565 from fb_reader
    logic [31:0] fb_reader_last_latched;
    logic        fb_reader_vblank_pulse;
    logic        fb_reader_axi_read_err;
    logic [2:0]  fb_reader_dbg_state;
    logic [17:0] fb_reader_dbg_burst_count;
    logic [15:0] fb_reader_dbg_axi_err_count;
    logic [15:0] fb_reader_dbg_underrun_count;

    video_mode_control mode_control_i (
        .clk(clk), .resetn(resetn),
        .request(mode_request), .requested_mode(requested_mode_q),
        .requested_base(mode_base_q), .reader_quiescent(reader_quiescent),
        .frame_latched(fb_reader_vblank_pulse), .clock_locked(clock_locked_clk),
        .active_mode(active_mode), .committed_base(mode_committed_base),
        .commit(mode_commit), .busy(mode_busy), .error(mode_error),
        .reader_pause(reader_pause), .video_hold(video_hold),
        .clock_resetn(clock_resetn),
        .clock_awaddr(clock_awaddr), .clock_awvalid(clock_awvalid),
        .clock_awready(clock_awready), .clock_wdata(clock_wdata),
        .clock_wvalid(clock_wvalid), .clock_wready(clock_wready),
        .clock_bresp(clock_bresp), .clock_bvalid(clock_bvalid),
        .clock_bready(clock_bready)
    );

    fb_reader fb_reader_i (
        .clk         (clk),
        .resetn      (resetn),

        .axi_read_if (axi_read_if),

        .base_addr_in        (fb_base_addr_q),
        .frame_bursts_in     (video_frame_bursts(active_mode)),
        .pause_request       (reader_pause),
        .quiescent           (reader_quiescent),
        .last_latched_addr   (fb_reader_last_latched),
        .vblank_latched_pulse(fb_reader_vblank_pulse),

        .vblank_start    (video_vblank_start_clk),

        .pixel_clk       (pixel_clk),
        .pixel_rd_en     (fb_pixel_rd_en),
        .pixel_rgb565    (fb_pixel),
        .axi_read_err    (fb_reader_axi_read_err),
        .dbg_state       (fb_reader_dbg_state),
        .dbg_burst_count (fb_reader_dbg_burst_count),
        .dbg_axi_err_count(fb_reader_dbg_axi_err_count),
        .dbg_underrun_count(fb_reader_dbg_underrun_count)
    );

    logic [31:0] mask_read_data;
    logic [607:0] mask_pixel_config;
    video_mode_t mask_timing;
    always_comb mask_timing = video_mode(active_mode);
    video_mask_config mask_config_i (
        .clk(clk), .resetn(resetn), .write_enable(as_client.awvalid),
        .write_address(as_common.awaddr), .read_address(as_common.araddr),
        .write_data(as_common.wdata), .write_strobe(as_common.wstrb),
        .read_data(mask_read_data), .frame_latched(fb_reader_vblank_pulse),
        .frame_base(fb_reader_last_latched), .pixel_clk(pixel_clk),
        .video_resetn(video_resetn), .vblank_start(video_vblank_start),
        .vertical_blank(video_v_count >= mask_timing.height),
        .pixel_config(mask_pixel_config)
    );

    // ------------------------------------------------------------------
    // FB control register block (AXI write decode + read mux + frame
    // counter / last-latched snapshot).
    // ------------------------------------------------------------------
    always_ff @(posedge clk) begin
        if (!resetn) begin
            fb_base_addr_q     <= 32'h0;
            fb_frame_count_q   <= 32'h0;
            fb_last_latched_q  <= 32'h0;
            mode_base_q        <= 32'h0;
            requested_mode_q   <= VIDEO_DEFAULT_MODE;
            mode_request      <= 1'b0;
        end else begin
            mode_request <= 1'b0;
            if (as_client.awvalid) begin
                case (as_common.awaddr)
                    FB_REGIDX_BASE_ADDR: fb_base_addr_q <= globals::apply_wstrb(
                        fb_base_addr_q, as_common.wdata, as_common.wstrb);
                    FB_REGIDX_CONTROL:   ;
                    FB_REGIDX_MODE_BASE: if (!mode_busy)
                        mode_base_q <= globals::apply_wstrb(
                            mode_base_q, as_common.wdata, as_common.wstrb);
                    FB_REGIDX_MODE_REQUEST: if (!mode_busy && as_common.wstrb[0]) begin
                        requested_mode_q <= as_common.wdata[3:0];
                        mode_request <= 1'b1;
                    end
                    default: ;
                endcase
            end
            if (mode_commit) fb_base_addr_q <= mode_committed_base;
            if (fb_reader_vblank_pulse) begin
                fb_frame_count_q  <= fb_frame_count_q + 32'd1;
                fb_last_latched_q <= fb_reader_last_latched;
            end
        end
    end

    /* rdata MUST be registered (not always_comb): the axidouble
     * crossbar's addrdecode is OPT_REGISTERED=1 without
     * OPT_LOWPOWER, so it advances o_addr to next_araddr the cycle
     * after a read fires. axidouble samples M_AXI_RDATA in that
     * already-advanced cycle, so a combinational slave returns the
     * next register's value (off-by-one shift). Registering the
     * mux adds the matching one-cycle latency. */
    logic [31:0] as_client_rdata_q;
    always_ff @(posedge clk) begin
        if (!resetn) begin
            as_client_rdata_q <= 32'h0000_0000;
        end else begin
            case (as_common.araddr)
                FB_REGIDX_BASE_ADDR:    as_client_rdata_q <= fb_base_addr_q;
                FB_REGIDX_CONTROL:      as_client_rdata_q <= 32'h0000_0000;
                FB_REGIDX_STATUS:       as_client_rdata_q <= fb_frame_count_q;
                FB_REGIDX_LAST_LATCHED: as_client_rdata_q <= fb_last_latched_q;
                FB_REGIDX_DEBUG:        as_client_rdata_q <= {
                    6'b0,                         // [31:26] reserved
                    fb_reader_dbg_burst_count,    // [25:8]
                    4'b0,                         // [7:4] reserved
                    fb_reader_axi_read_err,       // [3]
                    fb_reader_dbg_state           // [2:0]
                };
                FB_REGIDX_DEBUG2:       as_client_rdata_q <= {
                    fb_reader_dbg_underrun_count, // [31:16]
                    fb_reader_dbg_axi_err_count   // [15:0]
                };
                FB_REGIDX_MODE_REQUEST: as_client_rdata_q <= {28'b0, requested_mode_q};
                FB_REGIDX_MODE_BASE: as_client_rdata_q <= mode_base_q;
                FB_REGIDX_MASK_CAP: as_client_rdata_q <= 32'h4d534b31;
                FB_REGIDX_MODE_STATUS: as_client_rdata_q <= {
                    8'hA9, 12'b0, video_hold, clock_locked_clk, mode_error, mode_busy,
                    4'b0, active_mode
                };
                default:                as_client_rdata_q <= mask_read_data;
            endcase
        end
    end
    assign as_client.rdata = as_client_rdata_q;

    // ------------------------------------------------------------------
    // DVI output stage -- pixel_clk domain
    // ------------------------------------------------------------------
    (* IOB = "TRUE" *) reg [4:0] dvi_red_r;
    (* IOB = "TRUE" *) reg [5:0] dvi_grn_r;
    (* IOB = "TRUE" *) reg [4:0] dvi_blu_r;
    (* IOB = "TRUE" *) reg       dvi_de_r;
    (* IOB = "TRUE" *) reg       dvi_hsync_r;
    (* IOB = "TRUE" *) reg       dvi_vsync_r;

    // DE/sync already lag timing counters by one cycle. Match their
    // coordinates before inserting the fixed four-cycle mask pipeline.
    logic [11:0] mask_x, mask_y;
    logic [15:0] masked_pixel;
    logic masked_de, masked_hsync, masked_vsync;
    always_ff @(posedge pixel_clk) begin
        if (!video_resetn) begin mask_x <= 0; mask_y <= 0; end
        else begin mask_x <= video_h_count; mask_y <= video_v_count; end
    end
    video_pixel_mask pixel_mask_i (
        .pixel_clk(pixel_clk), .resetn(video_resetn),
        .config_data(mask_pixel_config), .pixel_in(fb_pixel), .x(mask_x), .y(mask_y),
        .de_in(video_de_i), .hsync_in(video_hsync_i), .vsync_in(video_vsync_i),
        .pixel_out(masked_pixel), .de_out(masked_de),
        .hsync_out(masked_hsync), .vsync_out(masked_vsync)
    );

    always @(posedge pixel_clk) begin
        if (!video_resetn) begin
            dvi_red_r <= 0;
            dvi_grn_r <= 0;
            dvi_blu_r <= 0;
            dvi_de_r <= 0;
            dvi_hsync_r <= 0;
            dvi_vsync_r <= 0;
            fb_pixel_rd_en <= 0;
        end else begin
            /* Only pop fb_reader's FIFO on data-enable cycles. The
             * FIFO is FWFT, so the front of the FIFO is the pixel for
             * the current cycle; rd_en advances to the next pixel. */
            fb_pixel_rd_en <= video_de_i;

            /* RGB565 maps directly onto the 5:6:5 pins. */
            dvi_red_r <= masked_pixel[15:11];
            dvi_grn_r <= masked_pixel[10:5];
            dvi_blu_r <= masked_pixel[4:0];
            dvi_de_r <= masked_de;
            dvi_hsync_r <= masked_hsync;
            dvi_vsync_r <= masked_vsync;
        end
    end

    /* DDR-generated DVI clock at pixel_clk rate, half-cycle delayed so
     * the TFP410 samples near the center of the data eye. */
    ODDR #(
        .DDR_CLK_EDGE("OPPOSITE_EDGE"),
        .INIT(1'b0),
        .SRTYPE("SYNC")
    ) dvi_clk_oddr (
        .Q  (dvi_clk),
        .C  (pixel_clk),
        .CE (1'b1),
        .D1 (1'b0),
        .D2 (1'b1),
        .R  (1'b0),
        .S  (1'b0)
    );

    assign dvi_red   = dvi_red_r;
    assign dvi_grn   = dvi_grn_r;
    assign dvi_blu   = dvi_blu_r;
    assign dvi_de    = dvi_de_r;
    assign dvi_hsync = dvi_hsync_r;
    assign dvi_vsync = dvi_vsync_r;

endmodule
