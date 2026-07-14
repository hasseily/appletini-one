// apple_cycle_egress: drains the AppleCycleRecord FIFO produced by
// apple_cycle_capture into a circular buffer in PS DDR over AXI3 HP0 write.
//
// Three AXI burst flavors share one AW/W/B sequence, selected by aw_kind_q:
//   - RING:   1..16 beats of records into the ring at producer_ptr_q
//   - NOTIFY: single beat carrying current producer_ptr_q to producer_ptr_addr
//   - GAP:    single beat of 64'h0 into the ring (overflow safety net)
//
// Bursts are sized via min(stage_count, 16, 4KB-cap, ring-wrap-cap). Each
// RING burst is followed unconditionally by a NOTIFY burst so the PS sees
// the producer pointer advance promptly. GAP markers are emitted on
// FIFO-side drops (capture_drop_sticky) or on ring-full and are also
// followed by a NOTIFY for prompt PS visibility.

`timescale 1ns / 1ps
module apple_cycle_egress
    import apple_cycle_capture_pkg::*;
(
    input  logic                        clk,
    input  logic                        resetn,

    // FIFO read interface from apple_cycle_capture (FWFT)
    input  AppleCycleRecord             cycle_capture_data,
    input  logic                        cycle_capture_empty,
    output logic                        cycle_capture_rd_en,

    // Drop-sticky handshake from apple_cycle_capture
    input  logic                        capture_drop_sticky,
    output logic                        capture_drop_ack,

    // Configuration (driven from apple_top register cases)
    input  logic                        cfg_enable,
    input  logic [31:0]                 cfg_ring_base_addr,    // byte addr; >=64B aligned
    input  logic [4:0]                  cfg_ring_size_log2,    // valid 12..24 (4KB..16MB)
    input  logic [31:0]                 cfg_producer_ptr_addr, // byte addr; 8B aligned
    input  logic [31:0]                 cfg_consumer_ptr,      // byte offset within ring

    // Status
    output logic [31:0]                 stat_producer_ptr,
    output logic [31:0]                 stat_records_written,
    output logic [31:0]                 stat_gap_markers,
    output logic [31:0]                 stat_bursts_issued,
    output logic [31:0]                 stat_full_stall_cycles,

    // AXI3 HP0 write master
    Axi3_write_if.master                axi_hp0_write
);

    // ---------------- Constants ----------------
    localparam int STAGE_DEPTH = 16;     // matches AXI3 max burst beats
    localparam int FLUSH_TIMER_LIMIT = 2048;    // ~15 us at 133 MHz
    localparam int IDLE_SETTLE_CYCLES = 4;

    // ---------------- Ring sizing ----------------
    /* Registered to break the long combinational path from
     * cfg_ring_size_log2 through the (1 << log2) shift and out into
     * every burst-cap consumer. cfg_ring_size_log2 only changes on PS
     * config writes, so the 1-cycle latency is invisible to the
     * runtime data path. */
    logic [31:0] ring_size_bytes;
    logic [31:0] ring_mask;

    always_ff @(posedge clk) begin
        if (~resetn) begin
            ring_size_bytes            <= 32'h1 << 5'd16;  // matches PS reset default
            ring_mask                  <= (32'h1 << 5'd16) - 32'h1;
        end else begin
            ring_size_bytes            <= 32'h1 << cfg_ring_size_log2;
            ring_mask                  <= (32'h1 << cfg_ring_size_log2) - 32'h1;
        end
    end

    // ---------------- Pointers ----------------
    logic [31:0] producer_ptr_q;
    logic [31:0] consumer_ptr_q;
    logic [31:0] used_bytes;
    logic [31:0] free_bytes;

    always_ff @(posedge clk) begin
        if (~resetn)
            consumer_ptr_q <= 32'h0;
        else
            consumer_ptr_q <= cfg_consumer_ptr;
    end

    // used_bytes: how much of the ring holds unread data (producer - consumer mod size).
    // Computed as a positive value via masked subtraction.
    assign used_bytes = (producer_ptr_q - consumer_ptr_q) & ring_mask;

    // Two slots reserved at the high-water mark. One for the gap marker,
    // one to disambiguate full vs empty (producer == consumer always means
    // empty; we never let producer reach consumer with normal records).
    //
    // "Ring full for normal records": used >= ring_size - 16 (no room for
    // another 8-byte record without consuming the reservation).
    // "Ring full for gap marker too": used >= ring_size - 8 (no room at all).
    logic ring_full_for_records;
    logic ring_full_for_gap;
    assign ring_full_for_records = (used_bytes >= (ring_size_bytes - 32'd16));
    assign ring_full_for_gap     = (used_bytes >= (ring_size_bytes - 32'd8));
    // Burst-cap free space clamps at zero to prevent unsigned underflow from
    // being interpreted as available room. Caps at ring_size-16.
    assign free_bytes = ring_full_for_records ? 32'd0
                       : ((ring_size_bytes - 32'd16) - used_bytes);

    // ---------------- Stage register file ----------------
    // 16x64 distributed RAM; two head pointers form a circular buffer.
    AppleCycleRecord stage_q [STAGE_DEPTH-1:0];
    logic [4:0]      stage_count_q;       // 0..16 inclusive
    logic [3:0]      stage_wr_idx_q;
    logic [3:0]      stage_rd_idx_q;

    // ---------------- Burst kind ----------------
    typedef enum logic [1:0] {
        K_RING   = 2'd0,
        K_NOTIFY = 2'd1,
        K_GAP    = 2'd2
    } aw_kind_t;
    aw_kind_t aw_kind_q;

    // ---------------- FSM ----------------
    typedef enum logic [3:0] {
        S_IDLE,
        S_DRAIN,
        S_BURST_CAP,
        S_BURST_PREP,
        S_AW,
        S_W,
        S_B,
        S_NOTIFY_PREP,
        S_GAP_PREP
    } state_t;
    state_t state_q;

    // ---------------- Burst registers ----------------
    logic [4:0]  aw_burst_size_q;     // 1..16
    logic [4:0]  burst_remaining_q;   // counts beats left in current W phase
    logic [31:0] aw_addr_q;           // absolute PS DDR address for current AW

    /* Staged burst-cap result: S_BURST_CAP computes the four caps
     * (stage / 4KB / wrap / free_bytes) and registers the final 5-bit
     * size + the absolute beat address; S_BURST_PREP just commits them
     * into aw_burst_size_q / aw_addr_q. Splits the long carry chain
     * from cfg_ring_size_log2_q -> sz_wrap -> aw_burst_size_q to keep
     * the 133 MHz clock closing. */
    logic [4:0]  burst_cap_sz_q;
    logic [31:0] burst_cap_addr_q;

    // Notify data latched at NOTIFY_PREP entry
    logic [31:0] notify_value_q;

    // ---------------- Pending-gap tracking ----------------
    // gap_pending_q: a gap marker is queued (from FIFO drop or ring-full)
    //                and will be emitted at the next opportunity.
    logic gap_pending_q;

    // ---------------- Idle settle / flush timer ----------------
    logic [3:0]  idle_settle_q;       // counts cycles cycle_capture_empty has been high
    logic [11:0] flush_timer_q;       // 0..FLUSH_TIMER_LIMIT-1

    // ---------------- Burst-size pre-computation (registered) ----------------
    /* Register the burst-size caps continuously so S_BURST_CAP only computes
     * a small min() over 5-bit values. This keeps the 32-bit ring-space
     * subtraction and comparison off the cap-decision path. cfg_ring_size,
     * producer_ptr_q, and cfg_ring_base_addr change only between bursts, so
     * S_BURST_CAP samples stable values.
     *
     * sz_4k_q  : 4KB-boundary cap (1..16)
     * sz_wrap_q: ring-wrap cap (0..16)
     * free_beats_q: free_bytes >> 3, clamped to 5 bits
     * beat_addr_q : cfg_ring_base_addr + producer_ptr_q (absolute beat
     *               address); also the per-burst aw address. */
    logic [4:0]  sz_4k_q;
    logic [4:0]  sz_wrap_q;
    logic [4:0]  free_beats_q;
    logic [31:0] beat_addr_q;

    always_ff @(posedge clk) begin
        if (~resetn) begin
            sz_4k_q      <= 5'd16;
            sz_wrap_q    <= 5'd16;
            free_beats_q <= 5'd0;
            beat_addr_q  <= 32'h0;
        end else begin
            beat_addr_q <= cfg_ring_base_addr + producer_ptr_q;

            /* 4KB boundary cap: beat_addr_q[11:3] is the beat index
             * within the 4KB page (0..511); the last 16-beat slot has
             * beat_addr_q[11:7] == 5'b11111. Using the *already-
             * registered* beat_addr_q (one burst stale) is safe because
             * producer_ptr_q only changes at S_B completion, far away
             * from S_BURST_CAP sampling, so beat_addr_q has been
             * settled for many cycles by the time we use it. */
            if (beat_addr_q[11:7] == 5'b11111)
                sz_4k_q <= 5'd16 - {1'b0, beat_addr_q[6:3]};
            else
                sz_4k_q <= 5'd16;

            /* Ring-wrap cap: beats-until-wrap = (ring_size - producer)>>3.
             * Both ring_size_bytes and producer_ptr_q are registered. */
            if ((ring_size_bytes - producer_ptr_q) >= 32'd128)
                sz_wrap_q <= 5'd16;
            else
                sz_wrap_q <= (ring_size_bytes - producer_ptr_q) >> 3;

            /* free_bytes feeds many things; clamp to 5 bits for the cap. */
            if ((free_bytes >> 3) >= 32'd16)
                free_beats_q <= 5'd16;
            else
                free_beats_q <= (free_bytes >> 3);
        end
    end

    // ---------------- Burst trigger ----------------
    // Emit a RING burst when:
    //   stage_count >= 8                                            (half-full)
    //   OR (stage_count > 0 && flush_timer expired)
    //   OR (stage_count > 0 && capture FIFO has been empty >= 4 cycles)
    logic ring_burst_trigger;
    assign ring_burst_trigger = cfg_enable && (
        (stage_count_q >= 5'd8)
        || ((stage_count_q > 5'd0) && (flush_timer_q >= FLUSH_TIMER_LIMIT[11:0]))
        || ((stage_count_q > 5'd0) && (idle_settle_q >= IDLE_SETTLE_CYCLES[3:0]))
    );

    // ---------------- Capture-FIFO read ----------------
    // Pull from FIFO when room in stage and not currently in a non-DRAIN AXI
    // phase. FWFT semantics: data appears combinationally on cycle_capture_data
    // when empty=0; rd_en advances to the next entry on the next clock.
    logic stage_can_accept;
    assign stage_can_accept = (stage_count_q < 5'(STAGE_DEPTH));

    logic stage_pull;
    assign stage_pull = !cycle_capture_empty
                        && stage_can_accept
                        && (state_q == S_DRAIN)
                        && !gap_pending_q;
    assign cycle_capture_rd_en = stage_pull;

    // ---------------- Status counter mirror ----------------
    assign stat_producer_ptr = producer_ptr_q;

    // ---------------- AXI output drives ----------------
    AppleCycleRecord stage_w_data;
    assign stage_w_data = stage_q[stage_rd_idx_q];

    logic [63:0] gap_w_data;
    assign gap_w_data = 64'h0;

    logic [63:0] notify_w_data;
    assign notify_w_data = {32'h0, notify_value_q};

    always_comb begin
        axi_hp0_write.awaddr  = '0;
        axi_hp0_write.awlen   = 4'd0;
        axi_hp0_write.awsize  = 3'd3;
        axi_hp0_write.awburst = 2'b01;
        axi_hp0_write.awvalid = 1'b0;
        axi_hp0_write.wdata   = '0;
        axi_hp0_write.wstrb   = 8'hFF;
        axi_hp0_write.wlast   = 1'b0;
        axi_hp0_write.wvalid  = 1'b0;
        axi_hp0_write.bready  = 1'b0;

        unique case (state_q)
            S_AW: begin
                axi_hp0_write.awaddr  = aw_addr_q;
                axi_hp0_write.awlen   = aw_burst_size_q[3:0] - 4'd1;
                axi_hp0_write.awvalid = 1'b1;
            end
            S_W: begin
                unique case (aw_kind_q)
                    K_RING:   axi_hp0_write.wdata = stage_w_data;
                    K_NOTIFY: axi_hp0_write.wdata = notify_w_data;
                    K_GAP:    axi_hp0_write.wdata = gap_w_data;
                    default:  axi_hp0_write.wdata = '0;
                endcase
                axi_hp0_write.wlast  = (burst_remaining_q == 5'd1);
                axi_hp0_write.wvalid = 1'b1;
            end
            S_B: begin
                axi_hp0_write.bready = 1'b1;
            end
            default: ;
        endcase
    end

    // ---------------- Drop ack ----------------
    // Asserted for one cycle when we finish a GAP burst that was triggered
    // by a FIFO-side drop. The gap_pending_q tracking handles ring-full
    // gaps separately (no ack needed since the source is internal).
    logic drop_ack_q;
    assign capture_drop_ack = drop_ack_q;

    // gap_source_q: which trigger caused gap_pending_q to be set.
    //   0 = ring-full (no ack)
    //   1 = FIFO drop (ack on completion)
    logic gap_source_q;

    // ---------------- FSM update ----------------
    always_ff @(posedge clk) begin
        if (~resetn) begin
            state_q                       <= S_IDLE;
            producer_ptr_q                <= 32'h0;
            stage_count_q                 <= 5'd0;
            stage_wr_idx_q                <= 4'd0;
            stage_rd_idx_q                <= 4'd0;
            aw_kind_q                     <= K_RING;
            aw_burst_size_q               <= 5'd0;
            burst_remaining_q             <= 5'd0;
            aw_addr_q                     <= 32'h0;
            burst_cap_sz_q                <= 5'd0;
            burst_cap_addr_q              <= 32'h0;
            notify_value_q                <= 32'h0;
            gap_pending_q                 <= 1'b0;
            gap_source_q                  <= 1'b0;
            idle_settle_q                 <= 4'd0;
            flush_timer_q                 <= 12'd0;
            drop_ack_q                    <= 1'b0;

            stat_records_written          <= 32'h0;
            stat_gap_markers              <= 32'h0;
            stat_bursts_issued            <= 32'h0;
            stat_full_stall_cycles        <= 32'h0;
        end else begin
            // Defaults each cycle
            drop_ack_q <= 1'b0;

            // Stage-fill from capture FIFO (FWFT: data is valid on the
            // same cycle rd_en is asserted; it advances on next clock).
            if (stage_pull) begin
                stage_q[stage_wr_idx_q] <= cycle_capture_data;
                stage_wr_idx_q          <= stage_wr_idx_q + 4'd1;
                stage_count_q           <= stage_count_q + 5'd1;
            end

            // Idle settle counter — tracks consecutive cycles capture FIFO
            // has been empty, used for "flush a partial burst soon" decision.
            if (cycle_capture_empty) begin
                if (idle_settle_q != 4'hF)
                    idle_settle_q <= idle_settle_q + 4'd1;
            end else begin
                idle_settle_q <= 4'd0;
            end

            // Flush timer — increments while we have data sitting in stage
            // but haven't burst it yet. Resets on any AW issue.
            if (state_q == S_DRAIN && stage_count_q > 5'd0) begin
                if (flush_timer_q < FLUSH_TIMER_LIMIT[11:0])
                    flush_timer_q <= flush_timer_q + 12'd1;
            end else if (state_q == S_AW) begin
                flush_timer_q <= 12'd0;
            end

            // Latch FIFO-drop into pending gap (one slot used by the gap
            // marker; the dropped record is gone, but the marker informs
            // the PS that a gap occurred).
            if (capture_drop_sticky && !gap_pending_q) begin
                gap_pending_q <= 1'b1;
                gap_source_q  <= 1'b1; // FIFO-drop source
            end

            // Stall counter
            if (gap_pending_q && state_q == S_DRAIN)
                stat_full_stall_cycles <= stat_full_stall_cycles + 32'd1;

            // ---------------- State transitions ----------------
            unique case (state_q)
                S_IDLE: begin
                    if (cfg_enable)
                        state_q <= S_DRAIN;
                end

                S_DRAIN: begin
                    if (!cfg_enable) begin
                        state_q <= S_IDLE;
                    end else if (gap_pending_q) begin
                        // Gap markers take priority over normal drains.
                        // Skip emission if even the gap-reserved slot is
                        // gone (consumer fully stopped advancing). When
                        // consumer eventually moves, fall through and emit.
                        if (!ring_full_for_gap)
                            state_q <= S_GAP_PREP;
                    end else if (ring_burst_trigger) begin
                        if (ring_full_for_records) begin
                            // Ring full for normal records. Queue a
                            // gap marker (ring-full source) and stall.
                            // Consumer must advance before we can do
                            // anything else.
                            gap_pending_q <= 1'b1;
                            gap_source_q  <= 1'b0;
                        end else begin
                            state_q <= S_BURST_CAP;
                        end
                    end
                end

                S_BURST_CAP: begin
                    /* Pick the min of four already-registered 5-bit caps.
                     * sz_4k_q / sz_wrap_q / free_beats_q are kept up to
                     * date by the always_ff above so the only logic in
                     * this cycle is a 4-way 5-bit min plus the stage
                     * cap (stage_count_q is 5 bits, naturally <= 16).
                     *
                     * Always advance to S_BURST_PREP; the zero-sz check
                     * happens there from the registered burst_cap_sz_q. */
                    automatic logic [4:0] sz_stage;
                    automatic logic [4:0] m1, m2, m3;
                    sz_stage = (stage_count_q > 5'd16) ? 5'd16 : stage_count_q;
                    m1 = (sz_stage   < sz_4k_q)      ? sz_stage   : sz_4k_q;
                    m2 = (m1         < sz_wrap_q)    ? m1         : sz_wrap_q;
                    m3 = (m2         < free_beats_q) ? m2         : free_beats_q;

                    burst_cap_sz_q       <= m3;
                    burst_cap_addr_q     <= beat_addr_q;
                    state_q <= S_BURST_PREP;
                end

                S_BURST_PREP: begin
                    /* Commit the registered burst-cap values to the AW
                     * channel registers and advance. Pure register copy;
                     * no arithmetic in this path.
                     *
                     * If sz turned out to be zero (shouldn't happen since
                     * DRAIN guards on free_bytes >= 8), spin back to
                     * DRAIN instead of issuing a zero-length AXI burst. */
                    if (burst_cap_sz_q == 5'd0) begin
                        state_q <= S_DRAIN;
                    end else begin
                        aw_kind_q                    <= K_RING;
                        aw_burst_size_q              <= burst_cap_sz_q;
                        aw_addr_q                    <= burst_cap_addr_q;
                        state_q <= S_AW;
                    end
                end

                S_AW: begin
                    if (axi_hp0_write.awready) begin
                        burst_remaining_q  <= aw_burst_size_q;
                        stat_bursts_issued <= stat_bursts_issued + 32'd1;
                        state_q <= S_W;
                    end
                end

                S_W: begin
                    if (axi_hp0_write.wready) begin
                        burst_remaining_q <= burst_remaining_q - 5'd1;
                        if (aw_kind_q == K_RING) begin
                            stage_rd_idx_q       <= stage_rd_idx_q + 4'd1;
                            stat_records_written <= stat_records_written + 32'd1;
                        end
                        if (burst_remaining_q == 5'd1)
                            state_q <= S_B;
                    end
                end

                S_B: begin
                    if (axi_hp0_write.bvalid) begin
                        unique case (aw_kind_q)
                            K_RING: begin
                                automatic logic [31:0] burst_bytes;
                                burst_bytes = {24'h0, aw_burst_size_q, 3'b000};
                                stage_count_q  <= stage_count_q - aw_burst_size_q;
                                producer_ptr_q <= (producer_ptr_q + burst_bytes) & ring_mask;
                                notify_value_q <= (producer_ptr_q + burst_bytes) & ring_mask;
                                state_q <= S_NOTIFY_PREP;
                            end
                            K_NOTIFY: begin
                                state_q <= S_DRAIN;
                            end
                            K_GAP: begin
                                producer_ptr_q   <= (producer_ptr_q + 32'd8) & ring_mask;
                                notify_value_q   <= (producer_ptr_q + 32'd8) & ring_mask;
                                stat_gap_markers <= stat_gap_markers + 32'd1;
                                gap_pending_q    <= 1'b0;
                                if (gap_source_q)
                                    drop_ack_q <= 1'b1;
                                state_q <= S_NOTIFY_PREP;
                            end
                            default: state_q <= S_DRAIN;
                        endcase
                    end
                end

                S_NOTIFY_PREP: begin
                    aw_kind_q       <= K_NOTIFY;
                    aw_burst_size_q <= 5'd1;
                    aw_addr_q       <= cfg_producer_ptr_addr;
                    state_q         <= S_AW;
                end

                S_GAP_PREP: begin
                    aw_kind_q       <= K_GAP;
                    aw_burst_size_q <= 5'd1;
                    aw_addr_q       <= cfg_ring_base_addr + producer_ptr_q;
                    state_q         <= S_AW;
                end

                default: state_q <= S_IDLE;
            endcase
        end
    end

    // ---------------- Elaboration sanity checks ----------------
    // Width assertion lives in the package; here we warn on PS misconfig.
    initial begin
        // Note: these are evaluated at simulation t=0 against reset values,
        // not against PS-driven runtime values. They catch wiring/reset
        // bugs, not bad PS init. Kept as $warning so misconfig doesn't
        // halt elaboration.
        if (cfg_ring_size_log2 != 5'd0 && cfg_ring_size_log2 < 5'd12)
            $warning("apple_cycle_egress: cfg_ring_size_log2 < 12 (%0d)",
                     cfg_ring_size_log2);
    end

endmodule
