// Frozen F1.1.4 coalescer (6335a98) for cycle-equivalence checks.
// Keep its original address control independent of later timing edits.
`timescale 1ns / 1ps

// Latest-value motherboard mirror for TURBO video and overlay RAM writes.
// Active writes drain under the caller's current motherboard mapping.
// Inactive writes retain their translated MAIN/AUX bank until an explicit
// bank flush; the caller steers and restores the motherboard for that flush.
// Every accepted write also enters the renderer stream at the caller.
module vtw_video_coalescer_reference (
    input  logic        clk,
    input  logic        rstn,
    input  logic        clear,
    input  logic        write_valid,
    input  logic [16:0] write_addr,
    input  logic [7:0]  write_data,
    input  logic        write_active,
    output logic        write_ready,
    input  logic        flush_valid,
    input  logic        flush_bank,
    output logic        mirror_valid,
    output logic [16:0] mirror_addr,
    output logic [7:0]  mirror_data,
    input  logic        mirror_ready,
    output logic        drained,
    output logic        active_drained,
    output logic [1:0]  bank_drained
);
    // Data needs no reset. The dirty bitmap is cleared through its scan
    // port after reset; this keeps both arrays in block RAM.
    (* ram_style = "block" *) logic [7:0] data_mem [0:131071];
    (* ram_style = "block" *) logic       dirty_mem [0:131071];
    logic [511:0] page_dirty_q;
    logic [511:0] active_page_dirty_q;
    logic [16:0] scan_addr_q;
    logic [8:0] next_page_q;
    logic [7:0] scan_data_q;
    logic scan_dirty_q;
    logic scan_collision_q;

    typedef enum logic [2:0] { CLEAR_BITMAP, SELECT_PAGE, FETCH_BYTE,
                               CHECK_BYTE, SEND_BYTE } state_t;
    state_t state_q;

    wire push = write_valid && write_ready;
    wire fetch = (state_q == FETCH_BYTE);
    wire select_page = (state_q == SELECT_PAGE) &&
        (flush_valid ? (next_page_q[8] == flush_bank &&
                        page_dirty_q[next_page_q]) :
                       active_page_dirty_q[next_page_q]);

    assign write_ready = rstn && !clear && (state_q != CLEAR_BITMAP);
    assign mirror_valid = rstn && !clear && (state_q == SEND_BYTE);
    assign mirror_addr = scan_addr_q;
    assign mirror_data = scan_data_q;
    assign drained = rstn && !clear && (state_q == SELECT_PAGE) &&
                     (page_dirty_q == '0) && !push;
    assign active_drained = rstn && !clear && (state_q == SELECT_PAGE) &&
        (active_page_dirty_q == '0) && !(push && write_active);
    for (genvar bank = 0; bank < 2; bank++) begin : bank_empty
        assign bank_drained[bank] = rstn && !clear &&
            (state_q != CLEAR_BITMAP) &&
            (page_dirty_q[bank*256 +: 256] == '0) &&
            ((state_q == SELECT_PAGE) || (scan_addr_q[16] != 1'(bank))) &&
            !(push && write_addr[16] == 1'(bank));
    end

    // Port A accepts new values. Port B takes a read-first snapshot and
    // clears its dirty bit on the SAME edge. A coincident writer keeps the
    // bit set, so a newer byte can never be lost while an old snapshot
    // waits for the physical bus. The page bit below schedules that retry.
    always_ff @(posedge clk) begin
        if (state_q == CLEAR_BITMAP && rstn && !clear) begin
            dirty_mem[scan_addr_q] <= 1'b0;
        end else if (fetch) begin
            scan_dirty_q <= dirty_mem[scan_addr_q];
            scan_data_q <= data_mem[scan_addr_q];
            // Cross-port read-during-write data is device-dependent. Keep
            // the dirty bit and retry instead of publishing that snapshot.
            scan_collision_q <= push && write_addr == scan_addr_q;
            if (!(push && write_addr == scan_addr_q)) begin
                dirty_mem[scan_addr_q] <= 1'b0;
            end
        end
    end
    // Separate clocked processes express the two physical BRAM ports.
    always_ff @(posedge clk) begin
        if (push) begin
            data_mem[write_addr] <= write_data;
            dirty_mem[write_addr] <= 1'b1;
        end
    end

    always_ff @(posedge clk) begin
        if (!rstn || clear) begin
            state_q <= CLEAR_BITMAP;
            scan_addr_q <= 17'd0;
            next_page_q <= 9'd0;
            page_dirty_q <= '0;
            active_page_dirty_q <= '0;
        end else begin
            // Clear on selection, not completion: any new write during a
            // scan schedules another pass, including already scanned bytes.
            if (select_page) begin
                page_dirty_q[next_page_q] <= 1'b0;
                active_page_dirty_q[next_page_q] <= 1'b0;
            end
            if (push) begin
                page_dirty_q[write_addr[16:8]] <= 1'b1;
                if (write_active)
                    active_page_dirty_q[write_addr[16:8]] <= 1'b1;
            end

            unique case (state_q)
                CLEAR_BITMAP: begin
                    scan_addr_q <= scan_addr_q + 17'd1;
                    if (scan_addr_q == 17'h1FFFF) begin
                        state_q <= SELECT_PAGE;
                    end
                end
                SELECT_PAGE: begin
                    next_page_q <= next_page_q + 9'd1;
                    if (select_page) begin
                        scan_addr_q <= {next_page_q, 8'd0};
                        state_q <= FETCH_BYTE;
                    end
                end
                FETCH_BYTE: state_q <= CHECK_BYTE;
                CHECK_BYTE: begin
                    if (scan_dirty_q && !scan_collision_q) begin
                        state_q <= SEND_BYTE;
                    end else if (scan_addr_q[7:0] == 8'hFF) begin
                        state_q <= SELECT_PAGE;
                    end else begin
                        scan_addr_q <= scan_addr_q + 17'd1;
                        state_q <= FETCH_BYTE;
                    end
                end
                SEND_BYTE: begin
                    if (mirror_ready) begin
                        if (scan_addr_q[7:0] == 8'hFF) begin
                            state_q <= SELECT_PAGE;
                        end else begin
                            scan_addr_q <= scan_addr_q + 17'd1;
                            state_q <= FETCH_BYTE;
                        end
                    end
                end
                default: state_q <= CLEAR_BITMAP;
            endcase
        end
    end
endmodule
