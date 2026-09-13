`timescale 1ns / 1ps

// Drain deferred TURBO video writes into their original MAIN/AUX banks.
// The caller freezes CPU/ARM requests before start and holds them until busy
// clears. Never change 80STORE: with it set, PAGE2 steers writes but does not
// select the displayed page. RamWorks switches must drain before they run,
// so any deferred AUX bytes still belong to the base auxiliary bank.
module vtw_video_bank_sync (
    input  logic        clk,
    input  logic        rstn,
    input  logic        clear,
    input  logic        start,
    input  logic [1:0]  bank_pending,
    input  logic        bus_idle,
    input  logic        sw_ramwrt,
    input  logic        sw_page2,
    input  logic        sw_80store,
    output logic        busy,
    output logic        flush_valid,
    output logic        flush_bank,
    input  logic        flush_drained,
    output logic        sync_req_valid,
    output logic [15:0] sync_req_addr,
    output logic        sync_req_rw,
    output logic [7:0]  sync_req_wdata,
    input  logic        sync_req_ready,
    input  logic        sync_resp_valid,
    output logic        restore_ramwrt,
    output logic        restore_page2
);
    typedef enum logic [3:0] {
        IDLE, PREPARE_BANK, SET_RAMWRT, WAIT_RAMWRT,
        SET_PAGE2, WAIT_PAGE2, DRAIN_BANK,
        RESTORE_RAMWRT, WAIT_RESTORE_RAMWRT,
        RESTORE_PAGE2, WAIT_RESTORE_PAGE2, FINISH
    } state_t;
    state_t state_q;
    logic [1:0] pending_q;
    logic bank_q;
    logic ramwrt_q, page2_q, store80_q;
    logic saved_ramwrt_q, saved_page2_q;

    assign busy = (state_q != IDLE);
    assign flush_valid = rstn && !clear && (state_q == DRAIN_BANK);
    assign flush_bank = bank_q;
    assign restore_ramwrt = saved_ramwrt_q;
    assign restore_page2 = saved_page2_q;
    assign sync_req_rw = 1'b0;
    assign sync_req_wdata = 8'd0;

    always_comb begin
        sync_req_valid = 1'b0;
        sync_req_addr = 16'hC004;
        if (rstn && !clear) begin
            unique case (state_q)
                SET_RAMWRT: begin
                    sync_req_valid = 1'b1;
                    sync_req_addr = bank_q ? 16'hC005 : 16'hC004;
                end
                SET_PAGE2: begin
                    sync_req_valid = 1'b1;
                    sync_req_addr = bank_q ? 16'hC055 : 16'hC054;
                end
                RESTORE_RAMWRT: begin
                    sync_req_valid = ramwrt_q != saved_ramwrt_q;
                    sync_req_addr = saved_ramwrt_q ? 16'hC005 : 16'hC004;
                end
                RESTORE_PAGE2: begin
                    sync_req_valid = page2_q != saved_page2_q;
                    sync_req_addr = saved_page2_q ? 16'hC055 : 16'hC054;
                end
                default: begin end
            endcase
        end
    end

    always_ff @(posedge clk) begin
        if (!rstn || clear) begin
            state_q <= IDLE;
            pending_q <= '0;
            bank_q <= 1'b0;
            ramwrt_q <= 1'b0;
            page2_q <= 1'b0;
            store80_q <= 1'b0;
            saved_ramwrt_q <= 1'b0;
            saved_page2_q <= 1'b0;
        end else begin
            unique case (state_q)
                IDLE: begin
                    if (start && bus_idle) begin
                        pending_q <= bank_pending;
                        bank_q <= 1'b0;
                        ramwrt_q <= sw_ramwrt;
                        page2_q <= sw_page2;
                        store80_q <= sw_80store;
                        saved_ramwrt_q <= sw_ramwrt;
                        saved_page2_q <= sw_page2;
                        state_q <= PREPARE_BANK;
                    end
                end
                PREPARE_BANK: begin
                    if (!pending_q[bank_q]) begin
                        if (!bank_q)
                            bank_q <= 1'b1;
                        else
                            state_q <= RESTORE_RAMWRT;
                    end else if (ramwrt_q != bank_q) begin
                        state_q <= SET_RAMWRT;
                    end else if (store80_q && page2_q != bank_q) begin
                        state_q <= SET_PAGE2;
                    end else begin
                        state_q <= DRAIN_BANK;
                    end
                end
                SET_RAMWRT: begin
                    if (sync_req_ready)
                        state_q <= WAIT_RAMWRT;
                end
                WAIT_RAMWRT: begin
                    if (sync_resp_valid) begin
                        ramwrt_q <= bank_q;
                        state_q <= PREPARE_BANK;
                    end
                end
                SET_PAGE2: begin
                    if (sync_req_ready)
                        state_q <= WAIT_PAGE2;
                end
                WAIT_PAGE2: begin
                    if (sync_resp_valid) begin
                        page2_q <= bank_q;
                        state_q <= PREPARE_BANK;
                    end
                end
                DRAIN_BANK: begin
                    if (flush_drained) begin
                        pending_q[bank_q] <= 1'b0;
                        if (!bank_q)
                            bank_q <= 1'b1;
                        state_q <= PREPARE_BANK;
                    end
                end
                RESTORE_RAMWRT: begin
                    if (ramwrt_q == saved_ramwrt_q)
                        state_q <= RESTORE_PAGE2;
                    else if (sync_req_ready)
                        state_q <= WAIT_RESTORE_RAMWRT;
                end
                WAIT_RESTORE_RAMWRT: begin
                    if (sync_resp_valid) begin
                        ramwrt_q <= saved_ramwrt_q;
                        state_q <= RESTORE_PAGE2;
                    end
                end
                RESTORE_PAGE2: begin
                    if (page2_q == saved_page2_q)
                        state_q <= FINISH;
                    else if (sync_req_ready)
                        state_q <= WAIT_RESTORE_PAGE2;
                end
                WAIT_RESTORE_PAGE2: begin
                    if (sync_resp_valid) begin
                        page2_q <= saved_page2_q;
                        state_q <= FINISH;
                    end
                end
                FINISH: state_q <= IDLE;
                default: state_q <= IDLE;
            endcase
        end
    end
endmodule
