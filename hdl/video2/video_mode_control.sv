`timescale 1ns / 1ps
// Apply one mode as a transaction: drain HP0, blank/reset scanout, change
// the MMCM, wait for relock, then commit geometry and the staged frame.
// Clocking Wizard owns the device-specific DRP calculations (PG065).
module video_mode_control #(
    parameter integer TIMEOUT_CYCLES = 66666666
) (
    input  logic clk,
    input  logic resetn,
    input  logic request,
    input  logic [3:0] requested_mode,
    input  logic [31:0] requested_base,
    input  logic reader_quiescent,
    input  logic frame_latched,
    input  logic clock_locked,
    output logic [3:0] active_mode,
    output logic [31:0] committed_base,
    output logic commit,
    output logic busy,
    output logic error,
    output logic reader_pause,
    output logic video_hold,
    output logic clock_resetn,
    output logic [10:0] clock_awaddr,
    output logic clock_awvalid,
    input  logic clock_awready,
    output logic [31:0] clock_wdata,
    output logic clock_wvalid,
    input  logic clock_wready,
    input  logic [1:0] clock_bresp,
    input  logic clock_bvalid,
    output logic clock_bready
);
    import video_pkg::*;
    typedef enum logic [3:0] {
        IDLE, QUIESCE, WRITE_SEND, WRITE_RESP, WAIT_UNLOCK,
        WAIT_LOCK, COMMIT_FRAME, START_VIDEO, WAIT_FRAME, FAILED,
        RECOVER_DRAIN, RECOVER_RESET, RECOVER_LOCK
    } state_t;
    state_t state;
    logic [3:0] pending_mode;
    logic [31:0] pending_base;
    logic [4:0] write_index;
    logic [26:0] timeout_count;
    logic [4:0] settle_count;
    logic fallback;

    // Complete AXI-Lite AW and W independently; either may stall first.
    // Write every configuration register, including unused output clocks,
    // before LOAD as required by PG065. Disabled outputs use divide 10.
    function automatic logic [31:0] clock_data(
        input logic [4:0] index, input logic [3:0] mode
    );
        if (index == 0) clock_data = video_clock_feedback(mode);
        else if (index == 1) clock_data = 0;
        else if (index == 2) clock_data = video_clock_output(mode);
        else if (index == 23) clock_data = 3;
        else case ((index - 2) % 3)
            0: clock_data = 10;
            1: clock_data = 0;
            default: clock_data = 50000;
        endcase
    endfunction

    assign busy = state != IDLE && state != FAILED;
    assign reader_pause = state != IDLE && state != START_VIDEO && state != WAIT_FRAME;
    assign video_hold = reader_pause;
    assign clock_bready = 1'b1;
    assign clock_resetn = resetn && state != RECOVER_RESET;

    always_ff @(posedge clk) begin
        if (!resetn) begin
            state <= IDLE;
            active_mode <= VIDEO_DEFAULT_MODE;
            pending_mode <= VIDEO_DEFAULT_MODE;
            pending_base <= 0;
            committed_base <= 0;
            commit <= 0;
            error <= 0;
            fallback <= 0;
            write_index <= 0;
            timeout_count <= 0;
            settle_count <= 0;
            clock_awaddr <= 0;
            clock_awvalid <= 0;
            clock_wdata <= 0;
            clock_wvalid <= 0;
        end else begin
            commit <= 0;
            if (clock_awvalid && clock_awready) clock_awvalid <= 0;
            if (clock_wvalid && clock_wready) clock_wvalid <= 0;
            if (busy) timeout_count <= timeout_count + 1'b1;
            else timeout_count <= 0;

            case (state)
                IDLE, FAILED: if (request) begin
                    if (requested_mode >= VIDEO_MODE_COUNT || requested_base == 0 ||
                        requested_base[6:0] != 0) begin
                        error <= 1;
                    end else begin
                        pending_mode <= requested_mode;
                        pending_base <= requested_base;
                        fallback <= 0;
                        error <= 0;
                        timeout_count <= 0;
                        // FAILED may retain half of a stalled AXI write.
                        // A fresh request first resets that private link.
                        state <= state == FAILED ? RECOVER_DRAIN : QUIESCE;
                    end
                end
                QUIESCE: if (reader_quiescent && clock_locked) begin
                    write_index <= 0;
                    state <= WRITE_SEND;
                end
                WRITE_SEND: begin
                    clock_awaddr <= 11'h200 + {4'b0, write_index, 2'b0};
                    clock_wdata <= clock_data(write_index, pending_mode);
                    clock_awvalid <= 1;
                    clock_wvalid <= 1;
                    state <= WRITE_RESP;
                end
                WRITE_RESP: if (clock_bvalid) begin
                    if (clock_bresp != 0) begin
                        error <= 1;
                        if (fallback) begin
                            state <= FAILED;
                        end else begin
                            fallback <= 1;
                            pending_mode <= VIDEO_DEFAULT_MODE;
                            timeout_count <= 0;
                            state <= RECOVER_DRAIN;
                        end
                    end else if (write_index == 23) begin
                        state <= WAIT_UNLOCK;
                    end else begin
                        write_index <= write_index + 1'b1;
                        state <= WRITE_SEND;
                    end
                end
                WAIT_UNLOCK: if (!clock_locked) state <= WAIT_LOCK;
                WAIT_LOCK: if (clock_locked) begin
                    settle_count <= 0;
                    state <= COMMIT_FRAME;
                end
                COMMIT_FRAME: begin
                    // The mode bus is held constant while the destination
                    // remains reset. No independent-bit CDC for geometry.
                    active_mode <= pending_mode;
                    committed_base <= pending_base;
                    commit <= 1;
                    settle_count <= 0;
                    state <= START_VIDEO;
                end
                START_VIDEO: begin
                    settle_count <= settle_count + 1'b1;
                    if (frame_latched) state <= IDLE;
                    else if (&settle_count) state <= WAIT_FRAME;
                end
                WAIT_FRAME: if (frame_latched) state <= IDLE;
                RECOVER_DRAIN: if (reader_quiescent) begin
                    settle_count <= 0;
                    state <= RECOVER_RESET;
                end
                RECOVER_RESET: begin
                    // Reset both ends of the private AXI link. MMCM DRP
                    // dividers survive reset, so relock then explicitly
                    // load every default preset register before committing.
                    clock_awvalid <= 0;
                    clock_wvalid <= 0;
                    settle_count <= settle_count + 1'b1;
                    if (&settle_count) state <= RECOVER_LOCK;
                end
                RECOVER_LOCK: if (clock_locked) begin
                    write_index <= 0;
                    state <= WRITE_SEND;
                end
                default: ;
            endcase

            // A stalled clock transaction is bounded. Reset the private
            // wizard then reprogram 1080p once; retain error after recovery.
            if (busy && timeout_count == TIMEOUT_CYCLES - 1) begin
                error <= 1;
                timeout_count <= 0;
                if (!fallback) begin
                    fallback <= 1;
                    pending_mode <= VIDEO_DEFAULT_MODE;
                    state <= RECOVER_DRAIN;
                end else begin
                    state <= FAILED;
                end
            end
        end
    end
endmodule
