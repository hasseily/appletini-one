// apple_dma_engine: bidirectional copy between the PSRAM line interface and
// PS DDR via HP1. Both sides transfer 8-byte units.
//
// Client interface:
//   - mc_addr  : 24-bit PSRAM byte address (need not be 8-aligned)
//   - ddr_addr : 32-bit byte address into DDR (must be 8-aligned)
//   - length   : byte length (need not be a multiple of 8)
//   - rw       : 1 = DDR to PSRAM ("read"), 0 = PSRAM to DDR ("write")
// Ready/valid handshake accepts the request; req_done pulses for one cycle
// when the operation completes.
//
// Alignment handling:
//   AXI beats are always 8 bytes. DDR is aligned and the AXI transfer size
//   is rounded up to a multiple of 8. MC lines that are partially covered
//   (first / last) are handled by read-modify-write; fully-covered MC lines
//   are written directly. The contract guarantees the transfer region is
//   exclusive on the PSRAM side, so RMW need not be atomic.
//
// AXI bursts:
//   AR/AW transactions issue bursts of up to 16 beats (AXI3 max), capped by
//   the remaining beats in the request and by the 4 KB boundary rule.
//   Bursts are serialized: each burst's data phase completes before the next
//   AR/AW is issued.
//
// Counting strategy:
//   The FSM control path expresses progress in *beats* and *lines* via small
//   decrement-to-zero counters (ar_remaining_q, aw_remaining_q, beats_left_q,
//   mc_lines_left_q, line_beats_to_fetch_q, beat_lines_to_fetch_q). The wide
//   Byte-domain registers feed the byte-selection helpers (ddr_byte_for and
//   mc_byte_for) without entering the FSM next-state path.

module apple_dma_engine #(
    parameter int LENGTH_W = 16
) (
    input  logic                 clk,
    input  logic                 rstn,

    // Client request interface
    input  logic [23:0]          req_mc_addr,
    input  logic [31:0]          req_ddr_addr,    // 8-byte aligned
    input  logic [LENGTH_W-1:0]  req_length,      // bytes; need not be %8
    input  logic                 req_rw,          // 1=DDR to PSRAM, 0=PSRAM to DDR
    input  logic                 req_valid,
    output logic                 req_ready,
    output logic                 req_done,

    // PSRAM line interface
    output logic [20:0]          dma_line_addr,
    output logic                 dma_rw,
    output logic [63:0]          dma_wdata,
    output logic                 dma_valid,
    input  logic                 dma_ready,
    input  logic [63:0]          dma_rdata,
    input  logic                 dma_rvalid,

    // HP1 AXI3 master to PS DDR
    Axi3_read_if.master          axi_hp1_read,
    Axi3_write_if.master         axi_hp1_write
);

    // ---------------- Latched request ----------------
    logic [23:0]         mc_base_q;
    logic [31:0]         ddr_base_q;
    logic [LENGTH_W-1:0] length_q;
    logic                rw_q;

    // Request-derived constants. Functions of req_mc_addr / req_ddr_addr /
    // req_length only, registered at request-accept time so the (length+7)>>3
    // carry chain happens once and the FSM control path sees only registered
    // values.
    logic [2:0]        mc_offset_q;
    logic [20:0]       first_mc_line_q;
    logic [28:0]       first_ddr_beat_q;
    logic [LENGTH_W:0] num_mc_lines_q;
    logic [LENGTH_W:0] num_ddr_beats_q;
    logic [LENGTH_W:0] num_ddr_beats_minus1_q;  // num_ddr_beats - 1
    logic [3:0]        last_byte_count_q;
    logic              mc_offset_nonzero_q;     // mc_offset != 0
    logic              first_line_is_partial_q;
    logic              last_line_is_partial_q;
    logic              has_extra_mc_line_q;     // num_mc_lines > num_ddr_beats

    wire [2:0]        mc_offset       = mc_offset_q;
    wire [20:0]       first_mc_line   = first_mc_line_q;
    wire [28:0]       first_ddr_beat  = first_ddr_beat_q;
    wire [LENGTH_W:0] num_mc_lines    = num_mc_lines_q;
    wire [LENGTH_W:0] num_ddr_beats   = num_ddr_beats_q;
    wire [3:0]        last_byte_count = last_byte_count_q;

    // ---------------- FSM ----------------
    typedef enum logic [3:0] {
        S_IDLE,
        // DDR to PSRAM
        S_W_LINE_BEGIN,
        S_W_DDR_AR, S_W_DDR_R,
        S_W_RMW_REQ, S_W_RMW_WAIT,
        S_W_MC_WRITE, S_W_MC_WRITE_WAIT,
        // PSRAM to DDR
        S_R_AW_PREP,    // settle aw_burst_size_q before issuing AW
        S_R_AW,
        S_R_BEAT_BEGIN,
        S_R_MC_REQ, S_R_MC_WAIT,
        S_R_DDR_W, S_R_DDR_B,
        S_DONE
    } state_t;
    state_t state_q;

    // ---------------- Index counters (data-path / address) ----------------
    // line_idx_q / beat_idx_q drive dma_line_addr offsets and the byte
    // selection helpers; they're not on the FSM next-state path.
    logic [LENGTH_W:0] line_idx_q;
    logic [LENGTH_W:0] beat_idx_q;
    logic [LENGTH_W:0] ddr_beats_loaded_q;
    logic [LENGTH_W:0] mc_lines_loaded_q;

    // ---------------- Control-path counters (beats/lines) ----------------
    // These are the *only* signals the FSM next-state logic compares against
    // wider registers. All decrement-to-zero, all stay narrow.
    logic [LENGTH_W:0] ar_remaining_q;     // DDR beats not yet AR-issued
    logic [LENGTH_W:0] aw_remaining_q;     // DDR beats not yet AW-issued
    logic [LENGTH_W:0] beats_left_q;       // DDR W beats not yet sent
    logic [LENGTH_W:0] mc_lines_left_q;    // MC lines not yet written
    logic [4:0]        burst_remaining_q;  // beats still to xfer in current AR/AW burst

    // Per-line / per-beat fetch obligations: how many more DDR beats /
    // MC lines need to be loaded before the current line / beat is ready.
    // Set at line/beat boundaries, decrement on each load completion.
    logic [1:0] line_beats_to_fetch_q;     // 0..2 in practice
    logic [1:0] beat_lines_to_fetch_q;     // 0..2

    // 1-bit "is last" markers, updated at line/beat boundaries. They feed
    // the byte-selection helpers (combinationally) and the FSM (registered).
    logic is_last_mc_line_q;
    logic is_last_ddr_beat_q;
    logic cur_line_is_partial_q;

    // ---------------- Data buffers ----------------
    logic [63:0] mc_old_q;
    logic [63:0] ddr_lo_q, ddr_hi_q;
    logic [63:0] mc_lo_q,  mc_hi_q;

    // ---------------- Per-line / per-beat byte boundaries ----------------
    // Used by the byte-selection helpers and the partial-line merge. Driven
    // off line_idx_q + the registered last-line marker; not on the FSM path.
    wire [3:0] cur_line_byte_lo =
        (line_idx_q == 0) ? {1'b0, mc_offset} : 4'h0;
    wire [3:0] cur_line_byte_hi =
        is_last_mc_line_q ? last_byte_count : 4'h8;

    wire [LENGTH_W:0] cur_line_last_ddr_beat =
        is_last_mc_line_q ? num_ddr_beats_minus1_q : line_idx_q;

    wire [LENGTH_W:0] cur_beat_last_mc_line =
        (!mc_offset_nonzero_q)         ? beat_idx_q :
        is_last_ddr_beat_q             ? (num_mc_lines - 1) :
                                         (beat_idx_q + 1);

    // ---------------- Byte selection helpers ----------------
    function automatic logic [7:0] ddr_byte_for(input logic [3:0] b);
        logic [LENGTH_W+4:0] t;
        logic [LENGTH_W:0]   beat;
        logic [2:0]          byte_in_beat;
        t            = ({{(LENGTH_W-3){1'b0}}, line_idx_q, 3'b000})
                     + {{(LENGTH_W+1){1'b0}}, b}
                     - {{(LENGTH_W+2){1'b0}}, mc_offset};
        beat         = t[LENGTH_W+3:3];
        byte_in_beat = t[2:0];
        return (beat == cur_line_last_ddr_beat) ?
                   ddr_hi_q[byte_in_beat*8 +: 8] :
                   ddr_lo_q[byte_in_beat*8 +: 8];
    endfunction

    function automatic logic [7:0] mc_byte_for(input logic [3:0] k);
        logic [LENGTH_W+4:0] mc_pos;
        logic [LENGTH_W:0]   line;
        logic [2:0]          byte_in_line;
        mc_pos       = ({{(LENGTH_W-3){1'b0}}, beat_idx_q, 3'b000})
                     + {{(LENGTH_W+1){1'b0}}, k}
                     + {{(LENGTH_W+2){1'b0}}, mc_offset};
        line         = mc_pos[LENGTH_W+3:3];
        byte_in_line = mc_pos[2:0];
        return (line == cur_beat_last_mc_line) ?
                   mc_hi_q[byte_in_line*8 +: 8] :
                   mc_lo_q[byte_in_line*8 +: 8];
    endfunction

    logic [63:0] merged_mc_line;
    always_comb begin
        for (int b = 0; b < 8; b++) begin
            if ((b[3:0] >= cur_line_byte_lo) && (b[3:0] < cur_line_byte_hi))
                merged_mc_line[b*8 +: 8] = ddr_byte_for(b[3:0]);
            else
                merged_mc_line[b*8 +: 8] = mc_old_q[b*8 +: 8];
        end
    end

    logic [63:0] beat_ddr_data;
    logic [63:0] beat_ddr_data_q;
    always_comb begin
        for (int k = 0; k < 8; k++)
            beat_ddr_data[k*8 +: 8] = mc_byte_for(k[3:0]);
    end

    // ---------------- Burst length computation ----------------
    // min(16, remaining, beats_to_4k_boundary). Beat addresses are 8B-aligned;
    // a 16-beat burst is 128 bytes. The 4 KB boundary only matters when the
    // current beat address is in the last 16-beat slot of a 4 KB page.
    function automatic logic [4:0] burst_cap(
        input logic [LENGTH_W:0] remaining,
        input logic [28:0]       beat_addr
    );
        logic [4:0] sz_remain;
        logic [4:0] sz_4k;
        sz_remain = (remaining > 'd16) ? 5'd16 : remaining[4:0];
        sz_4k     = (beat_addr[8:4] == 5'b11111) ?
                        (5'd16 - {1'b0, beat_addr[3:0]}) : 5'd16;
        return (sz_remain < sz_4k) ? sz_remain : sz_4k;
    endfunction

    // Burst progress uses decrementing remaining counts and registered beat
    // addresses. burst_cap therefore sees only registered inputs.
    logic [28:0]      ar_beat_addr_q;
    logic [28:0]      aw_beat_addr_q;
    wire [28:0]       ar_beat_addr    = ar_beat_addr_q;
    wire [28:0]       aw_beat_addr    = aw_beat_addr_q;
    wire [4:0]        ar_burst_size_d = burst_cap(ar_remaining_q, ar_beat_addr_q);
    wire [4:0]        aw_burst_size_d = burst_cap(aw_remaining_q, aw_beat_addr_q);

    // Registered burst sizes -- the burst_cap path (subtract -> min cap ->
    // 4 KB-edge compare) is too long to feed ddr_aw/ar_remaining_q on the
    // same clock edge. The FSM sits in S_W_DDR_AR / S_R_AW for at least the
    // settling cycle (S_W_LINE_BEGIN / S_R_AW_PREP) before driving awvalid /
    // arvalid, so the registered value is current by the time we use it.
    logic [4:0] ar_burst_size_q;
    logic [4:0] aw_burst_size_q;
    wire [4:0]  ar_burst_size = ar_burst_size_q;
    wire [4:0]  aw_burst_size = aw_burst_size_q;

    // ---------------- Output drives ----------------
    always_comb begin
        req_ready             = (state_q == S_IDLE);
        req_done              = (state_q == S_DONE);

        dma_line_addr         = '0;
        dma_rw                = 1'b1;
        dma_wdata             = '0;
        dma_valid             = 1'b0;

        axi_hp1_read.araddr   = '0;
        axi_hp1_read.arlen    = 4'd0;
        axi_hp1_read.arsize   = 3'd3;
        axi_hp1_read.arburst  = 2'b01;
        axi_hp1_read.arvalid  = 1'b0;
        axi_hp1_read.rready   = 1'b0;

        axi_hp1_write.awaddr  = '0;
        axi_hp1_write.awlen   = 4'd0;
        axi_hp1_write.awsize  = 3'd3;
        axi_hp1_write.awburst = 2'b01;
        axi_hp1_write.awvalid = 1'b0;
        axi_hp1_write.wdata   = '0;
        axi_hp1_write.wstrb   = 8'hFF;
        axi_hp1_write.wlast   = 1'b0;
        axi_hp1_write.wvalid  = 1'b0;
        axi_hp1_write.bready  = 1'b0;

        unique case (state_q)
            S_W_DDR_AR: begin
                axi_hp1_read.araddr  = {ar_beat_addr, 3'b000};
                axi_hp1_read.arlen   = ar_burst_size - 5'd1;
                axi_hp1_read.arvalid = 1'b1;
            end
            S_W_DDR_R: begin
                axi_hp1_read.rready  = 1'b1;
            end
            S_W_RMW_REQ: begin
                dma_line_addr        = first_mc_line + 21'(line_idx_q);
                dma_rw               = 1'b1;
                dma_valid            = 1'b1;
            end
            S_W_MC_WRITE: begin
                dma_line_addr        = first_mc_line + 21'(line_idx_q);
                dma_rw               = 1'b0;
                dma_wdata            = merged_mc_line;
                dma_valid            = 1'b1;
            end
            S_R_MC_REQ: begin
                dma_line_addr        = first_mc_line + 21'(mc_lines_loaded_q);
                dma_rw               = 1'b1;
                dma_valid            = 1'b1;
            end
            S_R_AW: begin
                axi_hp1_write.awaddr  = {aw_beat_addr, 3'b000};
                axi_hp1_write.awlen   = aw_burst_size - 5'd1;
                axi_hp1_write.awvalid = 1'b1;
            end
            S_R_DDR_W: begin
                axi_hp1_write.wdata   = beat_ddr_data_q;
                axi_hp1_write.wlast   = (burst_remaining_q == 5'd1);
                axi_hp1_write.wvalid  = 1'b1;
            end
            S_R_DDR_B: begin
                axi_hp1_write.bready  = 1'b1;
            end
            default: ;
        endcase
    end

    // ---------------- FSM update ----------------
    always_ff @(posedge clk) begin
        if (!rstn) begin
            state_q                   <= S_IDLE;

            line_idx_q                <= '0;
            beat_idx_q                <= '0;
            ddr_beats_loaded_q        <= '0;
            mc_lines_loaded_q         <= '0;

            ar_remaining_q            <= '0;
            aw_remaining_q            <= '0;
            beats_left_q              <= '0;
            mc_lines_left_q           <= '0;
            burst_remaining_q         <= '0;

            line_beats_to_fetch_q     <= 2'd0;
            beat_lines_to_fetch_q     <= 2'd0;
            is_last_mc_line_q         <= 1'b0;
            is_last_ddr_beat_q        <= 1'b0;
            cur_line_is_partial_q     <= 1'b0;

            ar_burst_size_q           <= '0;
            aw_burst_size_q           <= '0;
            ar_beat_addr_q            <= '0;
            aw_beat_addr_q            <= '0;
            beat_ddr_data_q           <= '0;

            mc_offset_q               <= '0;
            first_mc_line_q           <= '0;
            first_ddr_beat_q          <= '0;
            num_mc_lines_q            <= '0;
            num_ddr_beats_q           <= '0;
            num_ddr_beats_minus1_q    <= '0;
            last_byte_count_q         <= '0;
            mc_offset_nonzero_q       <= 1'b0;
            first_line_is_partial_q   <= 1'b0;
            last_line_is_partial_q    <= 1'b0;
            has_extra_mc_line_q       <= 1'b0;
        end else begin
            ar_burst_size_q <= ar_burst_size_d;
            aw_burst_size_q <= aw_burst_size_d;

            unique case (state_q)
                S_IDLE: begin
                    if (req_valid) begin
                        // Latch the request and pre-compute the registered
                        // shape constants. Wide arithmetic happens here once,
                        // not every cycle.
                        automatic logic [2:0] mc_off  = req_mc_addr[2:0];
                        automatic logic [LENGTH_W+2:0] len_pl_off =
                            {{(LENGTH_W){1'b0}}, mc_off}
                            + {{3'b000}, req_length}
                            + 'd7;
                        automatic logic [LENGTH_W+2:0] len_pl_seven =
                            {{3'b000}, req_length} + 'd7;
                        automatic logic [LENGTH_W:0] num_mc_lines_v =
                            len_pl_off[LENGTH_W+2:3];
                        automatic logic [LENGTH_W:0] num_ddr_beats_v =
                            len_pl_seven[LENGTH_W+2:3];
                        automatic logic [3:0] last_bc =
                            {1'b0, ((mc_off + req_length[2:0] + 3'h7) & 3'h7)} + 4'h1;
                        automatic logic mc_off_nz   = (mc_off != 3'd0);
                        automatic logic last_partial = (last_bc != 4'd8);

                        mc_base_q              <= req_mc_addr;
                        ddr_base_q             <= req_ddr_addr;
                        length_q               <= req_length;
                        rw_q                   <= req_rw;
                        mc_offset_q            <= mc_off;
                        first_mc_line_q        <= req_mc_addr[23:3];
                        first_ddr_beat_q       <= req_ddr_addr[31:3];
                        num_mc_lines_q         <= num_mc_lines_v;
                        num_ddr_beats_q        <= num_ddr_beats_v;
                        num_ddr_beats_minus1_q <= num_ddr_beats_v - 1;
                        last_byte_count_q      <= last_bc;
                        mc_offset_nonzero_q    <= mc_off_nz;
                        first_line_is_partial_q<= mc_off_nz;
                        last_line_is_partial_q <= last_partial;
                        has_extra_mc_line_q    <= (num_mc_lines_v != num_ddr_beats_v);

                        line_idx_q             <= '0;
                        beat_idx_q             <= '0;
                        ddr_beats_loaded_q     <= '0;
                        mc_lines_loaded_q      <= '0;

                        // Beats/lines remaining counters seed from the
                        // precomputed totals. ar/aw start at num_ddr_beats
                        // and walk down; beats_left likewise; mc_lines_left
                        // tracks the W path.
                        ar_remaining_q         <= num_ddr_beats_v;
                        aw_remaining_q         <= num_ddr_beats_v;
                        beats_left_q           <= num_ddr_beats_v;
                        mc_lines_left_q        <= num_mc_lines_v;
                        burst_remaining_q      <= '0;
                        ar_beat_addr_q         <= req_ddr_addr[31:3];
                        aw_beat_addr_q         <= req_ddr_addr[31:3];

                        // First line / first beat obligations.
                        // Write path (DDR to PSRAM), line 0:
                        //   needs cur_line_beats_needed = is_last_mc_line(0)
                        //         ? num_ddr_beats : 1 (= line_idx_q + 1).
                        //   ddr_beats_loaded_q is 0, so to_fetch = needed.
                        //   needed for line 0 is min(1, num_ddr_beats), which
                        //   is 0 only if num_ddr_beats==0 (zero-length req,
                        //   forbidden) and 1 otherwise.
                        line_beats_to_fetch_q  <= 2'd1;
                        is_last_mc_line_q      <= (num_mc_lines_v == 1);
                        cur_line_is_partial_q  <=
                            mc_off_nz || (num_mc_lines_v == 1 && last_partial);

                        // Read path (PSRAM to DDR), beat 0:
                        //   cur_beat_last_mc_line = mc_off_nz
                        //     ? min(1, num_mc_lines-1) : 0
                        //   cur_beat_lines_needed = that + 1
                        //   to_fetch = needed - mc_lines_loaded(0)
                        // For mc_off_nz: needed = min(2, num_mc_lines), so
                        // to_fetch is 1 if num_mc_lines==1, else 2.
                        // For !mc_off_nz: needed = 1, to_fetch = 1.
                        beat_lines_to_fetch_q  <=
                            (!mc_off_nz)              ? 2'd1 :
                            (num_mc_lines_v == 1)     ? 2'd1 :
                                                        2'd2;
                        is_last_ddr_beat_q     <= (num_ddr_beats_v == 1);

                        state_q                <= req_rw ? S_W_LINE_BEGIN : S_R_AW_PREP;
                    end
                end

                // ---------- DDR to PSRAM ----------
                S_W_LINE_BEGIN: begin
                    if (line_beats_to_fetch_q != 2'd0) begin
                        if (burst_remaining_q != 0)
                            state_q <= S_W_DDR_R;
                        else
                            state_q <= S_W_DDR_AR;
                    end else if (cur_line_is_partial_q)
                        state_q <= S_W_RMW_REQ;
                    else
                        state_q <= S_W_MC_WRITE;
                end

                S_W_DDR_AR: begin
                    if (axi_hp1_read.arready) begin
                        burst_remaining_q <= ar_burst_size;
                        ar_remaining_q    <= ar_remaining_q
                                             - {{(LENGTH_W-4){1'b0}}, ar_burst_size};
                        ar_beat_addr_q    <= ar_beat_addr_q
                                             + {{24{1'b0}}, ar_burst_size};
                        state_q           <= S_W_DDR_R;
                    end
                end

                S_W_DDR_R: begin
                    if (axi_hp1_read.rvalid) begin
                        ddr_lo_q              <= ddr_hi_q;
                        ddr_hi_q              <= axi_hp1_read.rdata;
                        ddr_beats_loaded_q    <= ddr_beats_loaded_q + 1;
                        burst_remaining_q     <= burst_remaining_q - 5'd1;
                        line_beats_to_fetch_q <= line_beats_to_fetch_q - 2'd1;
                        state_q               <= S_W_LINE_BEGIN;
                    end
                end

                S_W_RMW_REQ: begin
                    if (dma_ready)
                        state_q <= S_W_RMW_WAIT;
                end

                S_W_RMW_WAIT: begin
                    if (dma_rvalid) begin
                        mc_old_q <= dma_rdata;
                        state_q  <= S_W_MC_WRITE;
                    end
                end

                S_W_MC_WRITE: begin
                    if (dma_ready) begin
                        state_q <= S_W_MC_WRITE_WAIT;
                    end
                end

                S_W_MC_WRITE_WAIT: begin
                    if (dma_rvalid) begin
                        // Decrement the line counter; if this was the last
                        // line we're done, otherwise advance to the next
                        // line and seed its fetch obligation.
                        mc_lines_left_q <= mc_lines_left_q - 1;
                        if (mc_lines_left_q == 1) begin
                            state_q <= S_DONE;
                        end else begin
                            // Advance to next line. The new line is the last
                            // iff mc_lines_left_q == 2 after this decrement.
                            automatic logic next_is_last = (mc_lines_left_q == 2);
                            line_idx_q            <= line_idx_q + 1;
                            is_last_mc_line_q     <= next_is_last;
                            // Beats to fetch for the new line:
                            //   normal line:  1
                            //   last line, when num_mc_lines > num_ddr_beats:
                            //                  0 (no new beats; reuse held data)
                            //   last line, when num_mc_lines == num_ddr_beats:
                            //                  1
                            line_beats_to_fetch_q <=
                                (next_is_last && has_extra_mc_line_q) ? 2'd0 : 2'd1;
                            cur_line_is_partial_q <=
                                next_is_last ? last_line_is_partial_q : 1'b0;
                            state_q               <= S_W_LINE_BEGIN;
                        end
                    end
                end

                // ---------- PSRAM to DDR ----------
                S_R_AW_PREP: begin
                    state_q <= S_R_AW;
                end

                S_R_AW: begin
                    if (axi_hp1_write.awready) begin
                        burst_remaining_q <= aw_burst_size;
                        aw_remaining_q    <= aw_remaining_q
                                             - {{(LENGTH_W-4){1'b0}}, aw_burst_size};
                        aw_beat_addr_q    <= aw_beat_addr_q
                                             + {{24{1'b0}}, aw_burst_size};
                        state_q           <= S_R_BEAT_BEGIN;
                    end
                end

                S_R_BEAT_BEGIN: begin
                    if (beat_lines_to_fetch_q != 2'd0) begin
                        state_q <= S_R_MC_REQ;
                    end else begin
                        beat_ddr_data_q <= beat_ddr_data;
                        state_q <= S_R_DDR_W;
                    end
                end

                S_R_MC_REQ: begin
                    if (dma_ready)
                        state_q <= S_R_MC_WAIT;
                end

                S_R_MC_WAIT: begin
                    if (dma_rvalid) begin
                        mc_lo_q               <= mc_hi_q;
                        mc_hi_q               <= dma_rdata;
                        mc_lines_loaded_q     <= mc_lines_loaded_q + 1;
                        beat_lines_to_fetch_q <= beat_lines_to_fetch_q - 2'd1;
                        state_q               <= S_R_BEAT_BEGIN;
                    end
                end

                S_R_DDR_W: begin
                    if (axi_hp1_write.wready) begin
                        // Send a beat. Decrement burst-local and global
                        // beat counters, advance beat_idx, and seed the
                        // next beat's fetch obligation.
                        automatic logic next_is_last = (beats_left_q == 2);
                        burst_remaining_q  <= burst_remaining_q - 5'd1;
                        beat_idx_q         <= beat_idx_q + 1;
                        beats_left_q       <= beats_left_q - 1;
                        is_last_ddr_beat_q <= next_is_last;
                        // For the next beat (only meaningful if there is one):
                        //   mc_offset == 0:                  1 line per beat
                        //   mc_offset != 0, normal beat:     1 line
                        //   mc_offset != 0, next is last AND
                        //     has_extra_mc_line_q (i.e. one
                        //     more line beyond the per-beat
                        //     1:1 mapping):                  1
                        //   mc_offset != 0, next is last AND
                        //     !has_extra_mc_line_q:          0 (the line
                        //     was already loaded for the previous beat)
                        // Simplification: we always need to load 1 new MC
                        // line per beat except in the !mc_offset_nonzero_q
                        // last-beat special case, but actually for
                        // mc_offset==0 it's always exactly 1 per beat.
                        // For mc_offset!=0 the very first beat fetched 2
                        // (handled at request seed); every subsequent beat
                        // fetches 1, except possibly the last beat which
                        // fetches 0 if mc_offset!=0 and num_mc_lines did
                        // not gain an extra line.
                        beat_lines_to_fetch_q <=
                            (next_is_last && mc_offset_nonzero_q && !has_extra_mc_line_q)
                                ? 2'd0 : 2'd1;

                        if (burst_remaining_q == 5'd1)
                            state_q <= S_R_DDR_B;
                        else
                            state_q <= S_R_BEAT_BEGIN;
                    end
                end

                S_R_DDR_B: begin
                    if (axi_hp1_write.bvalid) begin
                        if (beats_left_q == 0)
                            state_q <= S_DONE;
                        else
                            state_q <= S_R_AW;
                    end
                end

                S_DONE: begin
                    state_q <= S_IDLE;
                end

                default: state_q <= S_IDLE;
            endcase
        end
    end

endmodule
