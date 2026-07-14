//******************************************************************************
// Video Timing Generator with Fixed-Rate Genlock
//
// Generates 1920x1080 progressive timing with runtime mode switching:
//   mode_1080p50 = 0  ->  1080p60 region (2200 pixels/line, 148.5 MHz)
//   mode_1080p50 = 1  ->  1080p50 region (2640 pixels/line, 148.5 MHz)
//
// GENLOCK STRATEGY
// ================
// When the Apple II bus is running, the HDMI output must match the Apple
// frame rate closely enough for the double-buffer mechanism to work without
// excessive latency, while keeping the monitor locked to a stable mode.
//
// Monitors require a FIXED, repeating frame structure to maintain sync.
// Any frame-to-frame variation in total line count -- even +/-1 line --
// presents a non-standard, unstable mode that many monitors will reject by
// blanking the display.
//
// The solution is simple: precompute the V_TOTAL that best matches each
// Apple standard and hold it constant for every frame in that mode.
//
// PRECOMPUTED V_TOTAL VALUES
// ==========================
// The Apple II crystal frequencies are known and stable (< 100 ppm):
//
//   NTSC crystal: 14.31818 MHz
//     Line = 912 master clocks (64x14 + 1x16 long cycle) = 63.695 us
//     Frame = 262 lines = 16688 us  ->  59.923 Hz
//     Ideal V_TOTAL = 148.5 MHz / (2200 x 59.923) = 1126.4  ->  1126
//     Actual rate at V_TOTAL=1126: 148.5M / (2200 x 1126) = 59.947 Hz
//     Residual error vs Apple: 0.04%  (1 full phase cycle every ~42 s)
//
//   PAL crystal: 14.25 MHz
//     Line = 912 master clocks (64x14 + 1x16 long cycle) = 63.998 us
//     Frame = 312 lines = 19967 us  ->  50.082 Hz
//     Ideal V_TOTAL = 148.5 MHz / (2640 x 50.082) = 1123.2  ->  1123
//     Actual rate at V_TOTAL=1123: 148.5M / (2640 x 1123) = 50.089 Hz
//     Residual error vs Apple: 0.01%  (1 full phase cycle every ~137 s)
//
//   Standard (Apple off):
//     V_TOTAL = 1125  ->  exactly 60.00 / 50.00 Hz
//
// The residual frequency error means the Apple and HDMI frames drift slowly
// relative to each other.  This is handled entirely by the double-buffer
// mechanism in the Apple framebuffer producer/reader pipeline.  No per-frame
// phase correction is attempted at the timing generator level.
//
// ACTIVITY DETECTION
// ==================
// The genlock_vblank_start input (a CDC'd pulse from the Apple frame
// scheduler) is used solely to detect whether the Apple bus is active.
// A 2-bit saturating counter (frames_without_vbl) tracks consecutive frames
// with no VBL pulse.  When the counter reaches 2, genlock is considered
// inactive and V_TOTAL reverts to the standard value.
//
// Activation takes 2 frames (first VBL detected, then V_TOTAL switches at
// the next frame boundary).  Deactivation takes 3-4 frames after the last
// VBL.  Both transitions are clean single-step V_TOTAL changes at frame
// boundaries.
//
// MODE SWITCHING
// ==============
// Both mode_1080p50 and the genlock V_TOTAL are latched at frame boundaries
// (v_counter wrapping to 0), so every frame has a consistent, fixed
// structure from start to finish.  A mode switch changes the horizontal
// total (2200 vs 2640) and/or vertical total, but each individual frame
// is internally self-consistent.
//
// VERTICAL BLANKING STRUCTURE
// ===========================
// Regardless of V_TOTAL, the blanking structure is:
//   Lines 0 to 1079:           Active video  (V_VISIBLE = 1080)
//   Lines 1080 to 1083:        Front porch   (V_FRONT = 4)
//   Lines 1084 to 1088:        Vsync pulse   (V_SYNC = 5)
//   Lines 1089 to V_TOTAL-1:   Back porch    (varies: 34-37 lines)
//
// Only the back porch length changes between modes.  Active video, front
// porch, and sync are always identical.
//******************************************************************************

module video_timing_gen (
    input  wire         clk_pixel,     // 148.5 MHz pixel clock
    input  wire         rst_n,         // Active-low reset
    input  wire         mode_1080p50,  // 0 = 60 Hz region, 1 = 50 Hz region
    input  wire         genlock_vblank_start, // Apple VBL pulse (activity detection only)

    output wire         hsync,         // Horizontal sync (active-high)
    output wire         vsync,         // Vertical sync   (active-high)
    output wire         de,            // Data enable (high during active pixels)
    output wire [11:0]  h_count,       // Current horizontal pixel position
    output wire [11:0]  v_count,       // Current vertical line position
    output wire         vblank_start  // Single-cycle pulse at (h=0, v=V_VISIBLE)
);
    import video_pkg::*;

    //==========================================================================
    // Timing parameters
    //==========================================================================
    localparam [11:0] H_VISIBLE    = VIDEO_ACTIVE_W;           // 1920
    localparam [11:0] V_VISIBLE    = VIDEO_ACTIVE_H;           // 1080
    localparam [11:0] V_FRONT      = 12'd4;                    // Front porch lines
    localparam [11:0] V_SYNC       = 12'd5;                    // Vsync pulse lines
    localparam [11:0] H_SYNC       = 12'd44;                   // Hsync pulse pixels
    localparam        H_SYNC_POL   = 1'b1;                     // Hsync active polarity
    localparam        V_SYNC_POL   = 1'b1;                     // Vsync active polarity

    //--------------------------------------------------------------------------
    // V_TOTAL variants
    //
    // Each value is the integer nearest to: pixel_clk / (h_total x apple_fps).
    // The resulting HDMI frame rate is fixed and stable -- no frame-to-frame
    // variation.  Only the back porch length differs between modes.
    //--------------------------------------------------------------------------
    localparam [11:0] V_TOTAL_STD      = 12'd1125; // Standard: 60.00 / 50.00 Hz
    localparam [11:0] V_TOTAL_NTSC_GL  = 12'd1126; // NTSC genlock: 59.95 Hz
    localparam [11:0] V_TOTAL_PAL_GL   = 12'd1123; // PAL genlock:  50.09 Hz

    //--------------------------------------------------------------------------
    // Horizontal timing (mode-dependent)
    // 1080p60: 1920 + 88 + 44 + 148 = 2200
    // 1080p50: 1920 + 528 + 44 + 148 = 2640
    //--------------------------------------------------------------------------
    localparam [11:0] H_FRONT_60   = 12'd88;
    localparam [11:0] H_TOTAL_60   = 12'd2200;
    localparam [11:0] H_FRONT_50   = 12'd528;
    localparam [11:0] H_TOTAL_50   = 12'd2640;

    //==========================================================================
    // Counter registers
    //==========================================================================
    reg [11:0] h_counter;
    reg [11:0] v_counter;
    reg        mode_1080p50_latched;    // Latched at frame boundaries
    reg [11:0] v_total_latched;         // Latched at frame boundaries

    //==========================================================================
    // Sync output registers
    //==========================================================================
    reg        hsync_reg;
    reg        vsync_reg;
    reg        de_reg;

    //==========================================================================
    // Genlock activity detection
    //
    // genlock_seen:         Set when a VBL pulse is observed during the current
    //                       frame.  Cleared at each frame boundary.
    //
    // frames_without_vbl:   2-bit saturating counter.  Reset to 0 when a VBL
    //                       is seen; incremented (up to 3) each frame without
    //                       one.  When >= 2, genlock is inactive.
    //
    // genlock_active:       Derived wire.  True when VBL pulses have been seen
    //                       recently (within the last 2 frames).
    //==========================================================================
    reg        genlock_seen;
    reg  [1:0] frames_without_vbl;
    wire       genlock_active = (frames_without_vbl < 2'd2);

    //==========================================================================
    // Derived signals
    //==========================================================================

    // Mode-dependent horizontal timing
    wire [11:0] h_front = mode_1080p50_latched ? H_FRONT_50 : H_FRONT_60;
    wire [11:0] h_total = mode_1080p50_latched ? H_TOTAL_50 : H_TOTAL_60;

    // Line and frame boundary detectors
    wire        line_end  = (h_counter == (h_total - 12'd1));
    wire        frame_end = (v_counter == (v_total_latched - 12'd1));
    // Keep frame wrap on the D path instead of the FDRE sync-reset pin.
    wire [11:0] v_counter_line_next = (v_counter + 12'd1) & {12{~frame_end}};

    // Horizontal/vertical timing regions
    wire h_in_visible = (h_counter < H_VISIBLE);
    wire h_in_sync    = (h_counter >= (H_VISIBLE + h_front)) &&
                        (h_counter <  (H_VISIBLE + h_front + H_SYNC));

    wire v_in_visible = (v_counter < V_VISIBLE);
    wire v_in_sync    = (v_counter >= (V_VISIBLE + V_FRONT)) &&
                        (v_counter <  (V_VISIBLE + V_FRONT + V_SYNC));

    // V_TOTAL to use for the NEXT frame.  Computed combinationally from the
    // current genlock_active state and the (unlatched) mode input.  Sampled
    // into v_total_latched at each frame boundary.
    wire [11:0] v_total_next = genlock_active ?
        (mode_1080p50 ? V_TOTAL_PAL_GL : V_TOTAL_NTSC_GL) :
        V_TOTAL_STD;

    //==========================================================================
    // Main counter logic
    //
    // Simple and deterministic: h_counter counts pixels within a line,
    // v_counter counts lines within a frame.  Both wrap at fixed totals
    // that are updated only at frame boundaries.  No per-frame variation,
    // no extension, no shortening.
    //==========================================================================
    always @(posedge clk_pixel) begin
        if (!rst_n) begin
            h_counter           <= 12'd0;
            v_counter           <= 12'd0;
            mode_1080p50_latched <= 1'b0;
            v_total_latched     <= V_TOTAL_STD;
            genlock_seen        <= 1'b0;
            frames_without_vbl  <= 2'd3;    // Start with genlock inactive
        end else begin

            if (line_end) begin
                h_counter <= 12'd0;
                v_counter <= v_counter_line_next;

                if (frame_end) begin
                    //----------------------------------------------------------
                    // Frame boundary: wrap v_counter and latch new parameters.
                    //
                    // Update the activity counter based on whether a VBL was
                    // seen during the frame that just ended.  Also check
                    // genlock_vblank_start directly in case the pulse arrives
                    // on this exact cycle.
                    //
                    // Latch mode_1080p50 and v_total_next so the upcoming
                    // frame has a fully consistent, fixed structure.
                    //----------------------------------------------------------
                    mode_1080p50_latched <= mode_1080p50;
                    v_total_latched      <= v_total_next;

                    if (genlock_seen || genlock_vblank_start)
                        frames_without_vbl <= 2'd0;
                    else
                        frames_without_vbl <= (frames_without_vbl < 2'd3) ?
                                               frames_without_vbl + 2'd1 : 2'd3;

                    genlock_seen <= 1'b0;

                end else begin
                    //----------------------------------------------------------
                    // Line boundary (not frame end): advance to next line.
                    // Capture any VBL pulse arriving on this cycle.
                    //----------------------------------------------------------
                    if (genlock_vblank_start)
                        genlock_seen <= 1'b1;
                end

            end else begin
                //--------------------------------------------------------------
                // Mid-line: advance h_counter.
                // Capture any VBL pulse for activity detection.
                //--------------------------------------------------------------
                h_counter <= h_counter + 12'd1;

                if (genlock_vblank_start)
                    genlock_seen <= 1'b1;
            end

        end
    end

    //==========================================================================
    // HSYNC generation
    //==========================================================================
    always @(posedge clk_pixel) begin
        if (!rst_n)
            hsync_reg <= ~H_SYNC_POL;
        else
            hsync_reg <= h_in_sync ? H_SYNC_POL : ~H_SYNC_POL;
    end

    //==========================================================================
    // VSYNC generation
    //==========================================================================
    always @(posedge clk_pixel) begin
        if (!rst_n)
            vsync_reg <= ~V_SYNC_POL;
        else
            vsync_reg <= v_in_sync ? V_SYNC_POL : ~V_SYNC_POL;
    end

    //==========================================================================
    // Data Enable generation
    //==========================================================================
    always @(posedge clk_pixel) begin
        if (!rst_n)
            de_reg <= 1'b0;
        else
            de_reg <= h_in_visible && v_in_visible;
    end

    //==========================================================================
    // Output assignments
    //==========================================================================
    assign hsync        = hsync_reg;
    assign vsync        = vsync_reg;
    assign de           = de_reg;
    assign h_count      = h_counter;
    assign v_count      = v_counter;
    assign vblank_start = rst_n && (h_counter == 12'd0) && (v_counter == V_VISIBLE);

endmodule
