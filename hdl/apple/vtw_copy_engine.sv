`timescale 1ns / 1ps

// Held-CPU copy/fill engine. Physical banks 0/1 use shadow port B; higher
// banks use the PSRAM line port. A source line and a dirty destination line
// retain eight bytes each. Aligned shadow accesses move four bytes per edge.
// PUBLISH_SHR retains aligned source reads, then commits each byte to the
// AUX shadow and ordered capture FIFO on the same accepted edge. Private
// copies never emit records. COPY_ROWS applies byte gaps between bounded rows;
// progress counts only committed row bytes. Cancellation drains every accepted PSRAM op.
module vtw_copy_engine (
    input  logic clk, rstn,
    input  logic start, abort_req, permit,
    input  logic [23:0] source, destination,
    input  logic [15:0] length,
    input  logic copy_rows,
    input  logic [7:0] rows_minus1, source_gap, destination_gap,
    input  logic fill,
    input  logic [7:0] fill_data,
    input  logic publish, publish_active,
    output logic publish_valid,
    output logic [16:0] publish_addr,
    output logic [7:0] publish_data,
    input  logic publish_ready,
    output logic busy, done, error, aborted,
    output logic [15:0] completed,

    output logic sh_en, sh_we, sh_word_we,
    output logic [17:0] sh_addr,
    output logic [31:0] sh_wdata,
    input  logic [31:0] sh_rdata,

    output logic ps_valid, ps_rw,
    output logic [23:0] ps_addr,
    output logic [63:0] ps_wdata,
    input  logic ps_ready, ps_rvalid,
    input  logic [63:0] ps_rdata
);
    typedef enum logic [3:0] {
        IDLE, CHECK, NEXT, SOURCE, SH_READ, SH_CAPTURE, DEST,
        SH_WRITE, PATCH, ADVANCE, PS_REQ, PS_WAIT, DRAIN, PUBLISH
    } state_t;
    state_t state_q;
    typedef enum logic [1:0] {LOAD_SOURCE, LOAD_DEST, STORE_DEST} op_t;
    op_t op_q;
    logic [23:0] src_q, dst_q;
    logic [15:0] left_q;
    logic rows_q;
    logic [15:0] width_q, row_left_q;
    logic [7:0] source_gap_q, destination_gap_q;
    logic [24:0] total_q, source_span_q, destination_span_q;
    wire [24:0] row_total = {9'd0, length} * (25'd1 + {17'd0, rows_minus1});
    wire [24:0] row_source_span = row_total + ({17'd0, rows_minus1} * {17'd0, source_gap});
    wire [24:0] row_destination_span = row_total + ({17'd0, rows_minus1} * {17'd0, destination_gap});
    logic fill_q;
    logic publish_q;
    logic [2:0] publish_lane_q;
    logic [7:0] fill_qdata;
    logic [2:0] step_q;
    logic [31:0] data_q;
    logic source_valid_q, dest_valid_q;
    logic [20:0] source_line_q, dest_line_q;
    logic [63:0] source_data_q, dest_data_q;
    logic [3:0] dirty_bytes_q;
    logic cancel_q;
    wire cancelled = cancel_q || abort_req || !permit ||
                     (publish_q && !publish_active);
    wire source_shadow = src_q < 24'h020000;
    wire dest_shadow = dst_q < 24'h020000;

    assign busy = state_q != IDLE;
    always_comb begin
        publish_valid = rstn && !cancelled && state_q == PUBLISH;
        publish_addr = dst_q[16:0] + {14'd0, publish_lane_q};
        publish_data = 8'(data_q >> (8 * publish_lane_q));
        sh_en = rstn && !cancelled && (state_q == SH_READ || state_q == SH_WRITE ||
                                     (state_q == PUBLISH && publish_ready));
        sh_we = state_q == SH_WRITE || state_q == PUBLISH;
        sh_word_we = state_q == SH_WRITE && step_q == 3'd4;
        sh_addr = state_q == PUBLISH ? {1'b0, publish_addr} :
                  (sh_we ? dst_q[17:0] : src_q[17:0]);
        sh_wdata = state_q == PUBLISH ? {24'd0, publish_data} : data_q;
        ps_valid = rstn && state_q == PS_REQ && !cancelled;
        ps_rw = op_q != STORE_DEST;
        ps_addr = {(op_q == LOAD_SOURCE ? src_q[23:3] : dest_line_q), 3'b000};
        ps_wdata = dest_data_q;
    end

    always_ff @(posedge clk) begin
        if (!rstn) begin
            state_q <= IDLE;
            done <= 1'b0;
            error <= 1'b0;
            aborted <= 1'b0;
            completed <= '0;
            src_q <= '0;
            dst_q <= '0;
            left_q <= '0;
            rows_q <= 1'b0;
            width_q <= '0;
            row_left_q <= '0;
            source_gap_q <= '0;
            destination_gap_q <= '0;
            total_q <= '0;
            source_span_q <= '0;
            destination_span_q <= '0;
            fill_q <= 1'b0;
            publish_q <= 1'b0;
            publish_lane_q <= '0;
            fill_qdata <= '0;
            step_q <= 3'd1;
            data_q <= '0;
            source_valid_q <= 1'b0;
            dest_valid_q <= 1'b0;
            source_line_q <= '0;
            dest_line_q <= '0;
            source_data_q <= '0;
            dest_data_q <= '0;
            dirty_bytes_q <= '0;
            cancel_q <= 1'b0;
            op_q <= LOAD_SOURCE;
        end else begin
            if (state_q == IDLE) begin
                if (start) begin
                    src_q <= source;
                    dst_q <= destination;
                    rows_q <= copy_rows === 1'b1;
                    width_q <= length;
                    row_left_q <= length;
                    source_gap_q <= source_gap;
                    destination_gap_q <= destination_gap;
                    left_q <= (copy_rows === 1'b1) ? row_total[15:0] : length;
                    total_q <= (copy_rows === 1'b1) ? row_total : {9'd0, length};
                    source_span_q <= (copy_rows === 1'b1) ? row_source_span : {9'd0, length};
                    destination_span_q <= (copy_rows === 1'b1) ? row_destination_span : {9'd0, length};
                    fill_q <= fill;
                    publish_q <= publish === 1'b1;
                    publish_lane_q <= '0;
                    fill_qdata <= fill_data;
                    source_valid_q <= 1'b0;
                    dest_valid_q <= 1'b0;
                    dirty_bytes_q <= '0;
                    completed <= '0;
                    done <= 1'b0;
                    error <= 1'b0;
                    aborted <= 1'b0;
                    cancel_q <= 1'b0;
                    if (!permit || abort_req ||
                        ((publish === 1'b1) && !publish_active)) begin
                        done <= 1'b1;
                        error <= 1'b1;
                    end else begin
                        state_q <= CHECK;
                    end
                end
            end else if (cancelled) begin
                cancel_q <= 1'b1;
                // ready is a registered acceptance pulse. It must be
                // consumed even when permission vanishes on this edge.
                if ((state_q == PS_REQ && ps_ready) ||
                    ((state_q == PS_WAIT || state_q == DRAIN) && !ps_rvalid)) begin
                    state_q <= DRAIN;
                end else begin
                    if ((state_q == PS_WAIT || state_q == DRAIN) &&
                        ps_rvalid && op_q == STORE_DEST)
                        completed <= completed + {12'd0, dirty_bytes_q};
                    state_q <= IDLE;
                    done <= 1'b1;
                    aborted <= 1'b1;
                end
            end else begin
                case (state_q)
                    CHECK: begin
                        // No ROM writes, out-of-range banks, zero lengths,
                        // or overlapping copies. Wider sums catch overflow.
                        if (total_q == 0 || total_q > 65535 || dst_q >= 24'h800000 ||
                            (rows_q && (fill_q || width_q == 0 ||
                             src_q[15:0] < 16'h0200 || dst_q[15:0] < 16'h0200 ||
                             ({9'd0, src_q[15:0]} + source_span_q) > 25'h0C000 ||
                             ({9'd0, dst_q[15:0]} + destination_span_q) > 25'h0C000)) ||
                            (publish_q && (dst_q < 24'h012000 ||
                             ({1'b0, dst_q} + destination_span_q) > 25'h0019D00)) ||
                            ({1'b0, dst_q} + destination_span_q) > 25'h0800000 ||
                            (!fill_q && (src_q >= 24'h800000 ||
                            ({1'b0, src_q} + source_span_q) > 25'h0800000 ||
                            (({1'b0, src_q} < {1'b0, dst_q} + destination_span_q) &&
                             ({1'b0, dst_q} < {1'b0, src_q} + source_span_q))))) begin
                            error <= 1'b1;
                            done <= 1'b1;
                            state_q <= IDLE;
                        end else state_q <= NEXT;
                    end
                    NEXT: begin
                        // Commit a completed PSRAM destination line before
                        // replacing it or declaring the descriptor complete.
                        if (dirty_bytes_q != 0 &&
                            (left_q == 0 || dst_q[23:3] != dest_line_q)) begin
                            op_q <= STORE_DEST;
                            state_q <= PS_REQ;
                        end else if (left_q == 0) begin
                            done <= 1'b1;
                            state_q <= IDLE;
                        end else begin
                            step_q <= (dst_q[1:0] == 0 &&
                                       (fill_q || src_q[1:0] == 0) &&
                                       left_q >= 4 && (!rows_q || row_left_q >= 4)) ? 3'd4 : 3'd1;
                            state_q <= SOURCE;
                        end
                    end
                    SOURCE: begin
                        if (fill_q) begin
                            data_q <= {4{fill_qdata}};
                            state_q <= DEST;
                        end else if (source_shadow) begin
                            state_q <= SH_READ;
                        end else if (source_valid_q && source_line_q == src_q[23:3]) begin
                            data_q <= source_data_q >> (8 * src_q[2:0]);
                            state_q <= DEST;
                        end else begin
                            op_q <= LOAD_SOURCE;
                            state_q <= PS_REQ;
                        end
                    end
                    SH_READ: state_q <= SH_CAPTURE;
                    SH_CAPTURE: begin
                        data_q <= sh_rdata >> (8 * src_q[1:0]);
                        state_q <= DEST;
                    end
                    DEST: begin
                        if (publish_q) begin
                            publish_lane_q <= '0;
                            state_q <= PUBLISH;
                        end else if (dest_shadow) begin
                            state_q <= SH_WRITE;
                        end else if (dest_valid_q && dest_line_q == dst_q[23:3]) begin
                            state_q <= PATCH;
                        end else begin
                            dest_line_q <= dst_q[23:3];
                            dest_valid_q <= 1'b1;
                            if (dst_q[2:0] == 0 && left_q >= 8 &&
                                (!rows_q || row_left_q >= 8)) begin
                                // The entire line will be replaced; omit RMW.
                                dest_data_q <= '0;
                                state_q <= PATCH;
                            end else begin
                                op_q <= LOAD_DEST;
                                state_q <= PS_REQ;
                            end
                        end
                    end
                    SH_WRITE: begin
                        completed <= completed + {13'd0, step_q};
                        state_q <= ADVANCE;
                    end
                    PUBLISH: if (publish_ready) begin
                        completed <= completed + 16'd1;
                        if (publish_lane_q + 3'd1 == step_q)
                            state_q <= ADVANCE;
                        else
                            publish_lane_q <= publish_lane_q + 3'd1;
                    end
                    PATCH: begin
                        // Fixed byte enables avoid a 64-bit variable mask.
                        for (int lane = 0; lane < 8; lane++) begin
                            if (step_q == 4 && dst_q[2] == (lane >= 4))
                                dest_data_q[8*lane +: 8] <= data_q[8*(lane%4) +: 8];
                            else if (step_q == 1 && dst_q[2:0] == 3'(lane))
                                dest_data_q[8*lane +: 8] <= data_q[7:0];
                        end
                        dirty_bytes_q <= dirty_bytes_q + {1'b0, step_q};
                        state_q <= ADVANCE;
                    end
                    ADVANCE: begin
                        // Gaps are address motion only: never count or write them.
                        // A short row cannot use a word across its boundary.
                        if (rows_q && row_left_q == {13'd0, step_q} &&
                            left_q != {13'd0, step_q}) begin
                            src_q <= src_q + {21'd0, step_q} + {16'd0, source_gap_q};
                            dst_q <= dst_q + {21'd0, step_q} + {16'd0, destination_gap_q};
                            row_left_q <= width_q;
                        end else begin
                            src_q <= src_q + {21'd0, step_q};
                            dst_q <= dst_q + {21'd0, step_q};
                            row_left_q <= row_left_q - {13'd0, step_q};
                        end
                        left_q <= left_q - {13'd0, step_q};
                        state_q <= NEXT;
                    end
                    PS_REQ: if (ps_ready) state_q <= PS_WAIT;
                    PS_WAIT: if (ps_rvalid) begin
                        case (op_q)
                            LOAD_SOURCE: begin
                                source_data_q <= ps_rdata;
                                source_line_q <= src_q[23:3];
                                source_valid_q <= 1'b1;
                                state_q <= SOURCE;
                            end
                            LOAD_DEST: begin
                                dest_data_q <= ps_rdata;
                                state_q <= PATCH;
                            end
                            STORE_DEST: begin
                                completed <= completed + {12'd0, dirty_bytes_q};
                                dirty_bytes_q <= '0;
                                dest_valid_q <= 1'b0;
                                state_q <= NEXT;
                            end
                            default: state_q <= NEXT;
                        endcase
                    end
                    default: state_q <= IDLE;
                endcase
            end
        end
    end
endmodule
