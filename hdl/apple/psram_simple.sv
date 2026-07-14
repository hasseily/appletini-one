`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// psram_simple: direct aux/RamWorks and PS-DMA access to PSRAM. The design
// has no cache, tags, speculative requests, or multi-client phase scheduler.
//
// What it serves (in priority order):
//
//   1. APPLE BUS AUX SERVE. On a CACHE-routed cycle whose decoded bank
//      (addr_decode[23:16]) is nonzero -- i.e. //e aux or a RamWorks
//      bank, never main/LC RAM -- and aux_provide_en is set, reads are
//      INH-served from PSRAM byte-by-byte and writes are captured into
//      PSRAM. Main-bank (bank 0) cycles are NEVER touched: the
//      motherboard owns main RAM and the language card. The INH side is
//      subject to the machine-mode interlock in apple_bus_write_arbiter,
//      so only a positively identified compatible machine can use it.
//
//   2. PS DMA CLIENT (apple_dma_engine). Full-line DDR<->PSRAM transfers
//      used by firmware staging operations.
//
// Byte writes from the Apple bus go through a small RMW ring FIFO
// (read the 64-bit line, patch it, write it back; one entry retired
// per RMW -- duplicate same-line entries just cost a redundant RMW,
// never correctness). At 1 MHz the worst realistic burst (interrupt
// pushing 3 stack bytes on consecutive cycles with ALTZP aux) fits the
// 4-deep FIFO with margin. Reads forward the newest matching queued
// byte so read-after-write is always coherent.
//
// Scheduling: a background op (RMW drain / ps-dma) is admitted at
// most once per Apple bus cycle, right after the serve decision point,
// so the PSRAM is idle again before the next cycle could need a
// deadline read. Admission keys off addr_en (which fires on every
// cycle, including M2-invalid ones on a GS) rather than sss_en (which
// does not).
//
// Address space: first PSRAM chip only, 8 MB. Banks 1..127 are
// servable (bank 128 -- RamWorks bank-select 127 -- would cross the
// 8 MB boundary and is not served; the PS advertises a matching size).
//////////////////////////////////////////////////////////////////////////////////

module psram_simple (
    input  logic                     clk,
    input  logic                     resetn,

    // ---- Apple bus serve ----
    input  globals::AppleBus_read    ab_read,
    input  globals::SoftSwitchState  sss,
    input  logic                     aux_provide_en,
    output globals::AppleBus_write   ab_write,

    // ---- PS DMA client ----
    input  logic [20:0]              dma_line_addr,
    input  logic                     dma_rw,
    input  logic [63:0]              dma_wdata,
    input  logic                     dma_valid,
    output logic                     dma_ready,
    output logic [63:0]              dma_rdata,
    output logic                     dma_rvalid,

    // ---- psram_driver command interface ----
    output logic                     psram_valid,
    input  logic                     psram_ready,
    output logic [7:0]               psram_cmd,
    output logic [23:0]              psram_addr,
    output logic [63:0]              psram_wdata,
    input  logic                     psram_rvalid,
    input  logic [63:0]              psram_rdata,

    // ---- Debug ----
    output logic [31:0]              dbg_aux_read_count,
    output logic [31:0]              dbg_aux_write_count,
    output logic [31:0]              dbg_deadline_miss_count,
    output logic [31:0]              dbg_dma_admit_count,
    output logic [31:0]              dbg_dma_complete_count,
    /* Serve-latency sentinels. QPI latency is fixed-cycle and refresh is
     * hidden by tCEM. The late counter must remain zero and the observed
     * watermark should remain near 28 fclk cycles. */
    output logic [31:0]              dbg_serve_late_count,
    output logic [31:0]              dbg_serve_max_latency,
    /* Writes dropped because the queue was full. MUST stay zero. */
    output logic [31:0]              dbg_wq_drop_count,
    /* Live queue/FSM state: {state[4:0], wq_count[3:0], rd[2:0],
     * wr[2:0], write_pending}. A stuck-full count with an idle FSM
     * means the count leaked (phantom push / lost retire). */
    output logic [15:0]              dbg_wq_state,
    /* Last deadline-miss context: {state[7:0], apple_addr[15:0]}. */
    output logic [23:0]              dbg_miss_ctx
);

    import globals::*;

    localparam [7:0] ENTER_QPI   = 8'h35;
    localparam [7:0] TOGGLE_WRAP = 8'hC0;
    localparam [7:0] QPI_WRITE   = 8'h02;
    localparam [7:0] QPI_READ    = 8'hEB;

    // ------------------------------------------------------------------
    // Serve decision (combinational, at sss_en time).
    // ------------------------------------------------------------------
    wire [7:0] decode_bank = sss.addr_decode[23:16];
    /* decode_bank 0x80 supplies RamWorks bank $7F through address bit 23.
     * Do not reject decode_bank[7]. */
    wire aux_cycle = (sss.route_kind == APPLE_ROUTE_CACHE) &&
                     sss.addr_decode_en &&
                     (decode_bank != 8'd0) &&
                     aux_provide_en;
    wire serve_read_start  = ab_read.sss_en && aux_cycle && ab_read.rw;
    wire serve_write_start = ab_read.sss_en && aux_cycle && !ab_read.rw;

    // ------------------------------------------------------------------
    // Write ring FIFO: 4 entries of {addr[23:0], data[7:0]}.
    // Push at data_en of a write cycle; head retired per background RMW.
    // Ring order IS age order: rd_ptr = oldest, (wr_ptr-1) = newest.
    // ------------------------------------------------------------------
    /* Chained RMW drains one byte per free window. Eight entries cover
     * interrupt bursts and in-flight lag; dbg_wq_drop_count must stay zero. */
    localparam int WQ_DEPTH = 8;
    logic [23:0] wq_addr [WQ_DEPTH];
    logic [7:0]  wq_data [WQ_DEPTH];
    logic [2:0]  wq_rd_ptr, wq_wr_ptr;
    logic [3:0]  wq_count;

    wire wq_empty = (wq_count == 4'd0);
    wire wq_full  = (wq_count >= 4'(WQ_DEPTH));
    wire [23:0] wq_head_addr = wq_addr[wq_rd_ptr];

    logic        write_pending_q;      // between sss_en and data_en
    logic [23:0] write_pending_addr_q;

    // Read forwarding: newest queued byte for this exact address wins.
    // Scan oldest -> newest, letting later matches overwrite.
    logic       fwd_hit;
    logic [7:0] fwd_data;
    always_comb begin
        fwd_hit  = 1'b0;
        fwd_data = 8'h00;
        for (int k = 0; k < WQ_DEPTH; k++) begin
            automatic logic [2:0] idx = wq_rd_ptr + 3'(k);
            if ((4'(k) < wq_count) &&
                (wq_addr[idx] == sss.addr_decode[23:0])) begin
                fwd_hit  = 1'b1;
                fwd_data = wq_data[idx];
            end
        end
    end

    logic [23:0] rmw_line_q;    // line byte-address of in-flight RMW
    logic [63:0] rmw_data_q;
    logic        rmw_park_q;    // chained write-leg yielded to a serve

    // Patch queued bytes for rmw_line_q into the line, oldest ->
    // newest (ring order) so the newest same-address byte lands last.
    /* Per-entry line-match flags, registered at RMW admission. The
     * patch indexes entries directly without a dynamic read-pointer mux or
     * live comparison. Entries pushed after admission remain queued for their
     * own RMW, and reads observe them through queue forwarding. Index order is
     * safe because no reader consumes the intermediate line image. */
    logic [WQ_DEPTH-1:0] rmw_hit_q;

    function automatic [63:0] rmw_patch(input [63:0] line);
        logic [63:0] r;
        r = line;
        for (int k = 0; k < WQ_DEPTH; k++) begin
            if (rmw_hit_q[k]) begin
                r[8*wq_addr[k][2:0] +: 8] = wq_data[k];
            end
        end
        return r;
    endfunction

    // ------------------------------------------------------------------
    // Main FSM
    // ------------------------------------------------------------------
    typedef enum logic [3:0] {
        S_INIT_QPI,
        S_INIT_QPI_WAIT,
        S_INIT_WRAP,
        S_INIT_WRAP_WAIT,
        S_IDLE,
        S_SERVE_READ,       // apple aux read in flight
        S_RMW_READ,         // background: read line for queued byte
        S_RMW_WRITE_ISSUE,  // issue the chained write leg as soon as the
                            // driver is ready; the queue's worst-case
                            // traffic requires one complete RMW per window
        S_RMW_WRITE_WAIT,   // background: wait for write completion
        S_DMA_OP            // background: ps dma op completion wait
    } state_e;

    state_e state;

    /* xvlog requires the state declaration before this debug assignment. */
    assign dbg_wq_state = {2'b0, 4'(state), wq_count,
                           wq_rd_ptr, wq_wr_ptr, write_pending_q};

    // One background admission per bus cycle, armed shortly after the
    // serve decision point and only within a bounded window. Background
    // pressure can make the FSM idle at any phase, so an armed level alone
    // could admit a chained RMW too late to finish before the next serve.
    // The bound guarantees admission + worst chained op < next sss_en:
    // accept(8) + read(28) + rest(5) + write(22) ~= 63 taps;
    // window 40 => done by ~T105 << T157.
    localparam [5:0] ADMIT_WINDOW_TAPS = 6'd40;
    logic [5:0] admit_window_q;
    logic [1:0] admit_delay_q;
    logic       admit_armed_q;

    logic [2:0]  serve_lane_q;   // byte lane of in-flight serve read
    logic [7:0]  serve_age_q;    // clk cycles since serve issue
    localparam [7:0] SERVE_LATE_THRESHOLD = 8'd75;
    // Queue-update events; a push (data_en) and a head retire
    // (S_RMW_WRITE) can land on the same clock, so wq_count gets ONE
    // unified update from these flags instead of two racing writes.
    logic        wq_push_now;
    logic        wq_retire_now;
    logic        serve_fwd_q;    // read satisfied without PSRAM data

    always_ff @(posedge clk) begin
        if (!resetn) begin
            state           <= S_INIT_QPI;
            psram_valid     <= 1'b0;
            psram_cmd       <= 8'h00;
            psram_addr      <= 24'd0;
            psram_wdata     <= 64'd0;
            dma_ready       <= 1'b0;
            dma_rvalid      <= 1'b0;
            dma_rdata       <= 64'd0;
            ab_write        <= '0;
            wq_rd_ptr       <= 3'd0;
            wq_wr_ptr       <= 3'd0;
            wq_count        <= 4'd0;
            write_pending_q <= 1'b0;
            write_pending_addr_q <= 24'd0;
            admit_delay_q   <= 2'd0;
            admit_armed_q   <= 1'b0;
            admit_window_q  <= 6'd0;
            serve_lane_q    <= 3'd0;
            serve_fwd_q     <= 1'b0;
            rmw_line_q      <= 24'd0;
            rmw_hit_q       <= '0;
            rmw_data_q      <= 64'd0;
            rmw_park_q      <= 1'b0;
            dbg_miss_ctx    <= 24'd0;
            dbg_aux_read_count      <= 32'd0;
            dbg_aux_write_count     <= 32'd0;
            dbg_deadline_miss_count <= 32'd0;
            dbg_dma_admit_count     <= 32'd0;
            dbg_dma_complete_count  <= 32'd0;
            dbg_serve_late_count    <= 32'd0;
            dbg_serve_max_latency   <= 32'd0;
            dbg_wq_drop_count       <= 32'd0;
            serve_age_q             <= 8'd0;
            for (int i = 0; i < WQ_DEPTH; i++) begin
                wq_addr[i] <= 24'd0;
                wq_data[i] <= 8'd0;
            end
        end else begin
            // Single-cycle pulses.
            dma_rvalid   <= 1'b0;
            dma_ready    <= 1'b0;
            if (psram_valid && psram_ready) begin
                psram_valid <= 1'b0;
            end

            // ---- Apple bus serve lifecycle ----
            // A new address phase clears any stale serve drive (the
            // same contract every card follows).
            if (ab_read.addr_en) begin
                ab_write.assert_inh <= 1'b0;
                ab_write.wr_data_en <= 1'b0;
                // Arm the background admission window for addr_en+3
                // (sss_en = addr_en+1, so the serve decision is
                // settled before the window opens).
                admit_delay_q <= 2'd3;
                admit_armed_q <= 1'b0;
                admit_window_q <= ADMIT_WINDOW_TAPS;
            end else if (admit_delay_q != 2'd0) begin
                admit_delay_q <= admit_delay_q - 2'd1;
                if (admit_delay_q == 2'd1) begin
                    admit_armed_q <= 1'b1;
                end
            end
            if (admit_window_q != 6'd0) begin
                admit_window_q <= admit_window_q - 6'd1;
            end

            // Serve decision at sss_en.
            if (serve_read_start) begin
                // A serve consumes this cycle's background budget: a
                // late background launch after the serve completes
                // could still be in flight at the NEXT cycle's serve
                // point.
                admit_armed_q <= 1'b0;
                admit_delay_q <= 2'd0;
                serve_lane_q  <= sss.addr_decode[2:0];
                ab_write.assert_inh <= 1'b1;
                ab_write.wr_data_en <= 1'b1;
                dbg_aux_read_count  <= dbg_aux_read_count + 32'd1;
                if (fwd_hit) begin
                    // Newest queued write wins; no PSRAM op needed.
                    ab_write.wr_data <= fwd_data;
                    serve_fwd_q      <= 1'b1;
                end else if (state == S_IDLE) begin
                    serve_fwd_q <= 1'b0;
                    serve_age_q <= 8'd0;
                    psram_valid <= 1'b1;
                    psram_cmd   <= QPI_READ;
                    psram_addr  <= {sss.addr_decode[23:3], 3'b000};
                    state       <= S_SERVE_READ;
                end else if (state == S_RMW_WRITE_ISSUE) begin
                    // Chained write-leg not yet issued: YIELD to the
                    // serve (the CPU's deadline outranks the drain).
                    // The patched line parks and re-issues from the
                    // next background admission window.
                    rmw_park_q  <= 1'b1;
                    serve_fwd_q <= 1'b0;
                    serve_age_q <= 8'd0;
                    psram_valid <= 1'b1;
                    psram_cmd   <= QPI_READ;
                    psram_addr  <= {sss.addr_decode[23:3], 3'b000};
                    state       <= S_SERVE_READ;
                end else begin
                    // PSRAM op genuinely in flight. Count it and latch
                    // context: which state and address missed.
                    serve_fwd_q <= 1'b1;
                    dbg_deadline_miss_count <=
                        dbg_deadline_miss_count + 32'd1;
                    dbg_miss_ctx <= {8'(state),
                                     sss.addr_decode[15:0]};
                end
            end

            if (serve_write_start) begin
                write_pending_q      <= 1'b1;
                write_pending_addr_q <= sss.addr_decode[23:0];
            end

            // Write data arrives at data_en; push to the ring.
            wq_push_now = 1'b0;
            if (write_pending_q && ab_read.data_en) begin
                write_pending_q <= 1'b0;
                dbg_aux_write_count <= dbg_aux_write_count + 32'd1;
                if (!wq_full) begin
                    wq_addr[wq_wr_ptr] <= write_pending_addr_q;
                    wq_data[wq_wr_ptr] <= ab_read.data;
                    wq_wr_ptr   <= wq_wr_ptr + 3'd1;
                    wq_push_now = 1'b1;
                end else begin
                    /* With the bounded-window drain this must be
                     * unreachable; if it ever counts, writes ARE
                     * being lost again. */
                    dbg_wq_drop_count <= dbg_wq_drop_count + 32'd1;
                end
            end
            wq_retire_now = 1'b0;

            // ---- FSM ----
            case (state)
                S_INIT_QPI: begin
                    psram_valid <= 1'b1;
                    psram_cmd   <= ENTER_QPI;
                    psram_addr  <= 24'd0;
                    state       <= S_INIT_QPI_WAIT;
                end
                S_INIT_QPI_WAIT: if (psram_rvalid) begin
                    state <= S_INIT_WRAP;
                end
                S_INIT_WRAP: begin
                    psram_valid <= 1'b1;
                    psram_cmd   <= TOGGLE_WRAP;
                    psram_addr  <= 24'd0;
                    state       <= S_INIT_WRAP_WAIT;
                end
                S_INIT_WRAP_WAIT: if (psram_rvalid) begin
                    state <= S_IDLE;
                end

                S_IDLE: begin
                    // Background admission, one DRIVER OP per bus
                    // cycle. Priority: pending second legs (committed
                    // data) > RMW drain > ps-dma.
                    if (admit_armed_q && admit_window_q != 6'd0 &&
                        !serve_read_start) begin
                        if (rmw_park_q) begin
                            admit_armed_q <= 1'b0;
                            rmw_park_q  <= 1'b0;
                            psram_valid <= 1'b1;
                            psram_cmd   <= QPI_WRITE;
                            psram_addr  <= rmw_line_q;
                            psram_wdata <= rmw_data_q;
                            state       <= S_RMW_WRITE_WAIT;
                        end else if (!wq_empty) begin
                            admit_armed_q <= 1'b0;
                            rmw_line_q  <= {wq_head_addr[23:3], 3'b000};
                            for (int i = 0; i < WQ_DEPTH; i++) begin
                                automatic logic [2:0] age =
                                    3'(i) - wq_rd_ptr;
                                rmw_hit_q[i] <=
                                    ({1'b0, age} < wq_count) &&
                                    (wq_addr[i][23:3] ==
                                     wq_head_addr[23:3]);
                            end
                            psram_valid <= 1'b1;
                            psram_cmd   <= QPI_READ;
                            psram_addr  <= {wq_head_addr[23:3], 3'b000};
                            state       <= S_RMW_READ;
                        end else if (dma_valid) begin
                            dbg_dma_admit_count <= dbg_dma_admit_count + 32'd1;
                            /* MC-port rw is BUS-style on BOTH clients:
                             * 1 = read, 0 = write (apple_dma_engine's
                             * S_W_MC_WRITE drives rw=0). Not to be
                             * confused with ps_dma_command's request-
                             * level rw flag, which encodes transfer
                             * direction and is inverted relative to
                             * this. Getting this wrong turns staging
                             * writes into reads: PSRAM never written,
                             * Disk II streams power-on noise forever. */
                            admit_armed_q <= 1'b0;
                            dma_ready   <= 1'b1;
                            psram_valid <= 1'b1;
                            psram_cmd   <= dma_rw ? QPI_READ : QPI_WRITE;
                            psram_addr  <= {dma_line_addr, 3'b000};
                            psram_wdata <= dma_wdata;
                            state       <= S_DMA_OP;
                        end
                    end
                end

                S_SERVE_READ: begin
                    if (serve_age_q != 8'hFF) begin
                        serve_age_q <= serve_age_q + 8'd1;
                    end
                    if (psram_rvalid) begin
                        if (!serve_fwd_q) begin
                            ab_write.wr_data <=
                                psram_rdata[8*serve_lane_q +: 8];
                        end
                        if ({24'd0, serve_age_q} > dbg_serve_max_latency) begin
                            dbg_serve_max_latency <= {24'd0, serve_age_q};
                        end
                        if (serve_age_q > SERVE_LATE_THRESHOLD) begin
                            dbg_serve_late_count <=
                                dbg_serve_late_count + 32'd1;
                        end
                        state <= S_IDLE;
                    end
                end

                S_RMW_READ: if (psram_rvalid) begin
                    // Chain straight into the write leg. The head
                    // entry is NOT retired until the write completes:
                    // reads keep forwarding it from the queue. A byte
                    // pushed after this patch is missed by the line
                    // image but stays queued for its own later RMW.
                    rmw_data_q <= rmw_patch(psram_rdata);
                    state      <= S_RMW_WRITE_ISSUE;
                end
                S_RMW_WRITE_ISSUE: if (psram_ready &&
                                        !serve_read_start) begin
                    // !serve_read_start: on the clock a serve arrives
                    // the yield path (above) owns the driver; without
                    // the gate both branches fire and this one, being
                    // textually last, clobbers the serve issue and returns
                    // a stale INH-served byte to the CPU.
                    psram_valid <= 1'b1;
                    psram_cmd   <= QPI_WRITE;
                    psram_addr  <= rmw_line_q;
                    psram_wdata <= rmw_data_q;
                    state       <= S_RMW_WRITE_WAIT;
                end
                S_RMW_WRITE_WAIT: if (psram_rvalid) begin
                    // Once the write lands, the head entry is redundant.
                    wq_rd_ptr     <= wq_rd_ptr + 3'd1;
                    wq_retire_now = 1'b1;
                    state         <= S_IDLE;
                end


                S_DMA_OP: if (psram_rvalid) begin
                    dma_rdata  <= psram_rdata;
                    dma_rvalid <= 1'b1;
                    dbg_dma_complete_count <= dbg_dma_complete_count + 32'd1;
                    state      <= S_IDLE;
                end

                default: state <= S_IDLE;
            endcase

            // Unified queue-count update (see wq_push_now decl).
            wq_count <= wq_count + (wq_push_now ? 4'd1 : 4'd0)
                                 - (wq_retire_now ? 4'd1 : 4'd0);
        end
    end

endmodule
