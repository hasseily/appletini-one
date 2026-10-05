// Per-frame pixel-mask configuration. The PS finishes a free framebuffer's
// bank before publishing FB_BASE. Snapshot only the bank matched by the actual
// reader latch; a later PS publish cannot change the frame being displayed.
module video_mask_config (
    input logic clk, resetn,
    input logic write_enable,
    input logic [7:0] write_address, read_address,
    input logic [31:0] write_data,
    input logic [3:0] write_strobe,
    output logic [31:0] read_data,
    input logic frame_latched,
    input logic [31:0] frame_base,
    input logic pixel_clk, video_resetn,
    input logic vblank_start, vertical_blank,
    output logic [607:0] pixel_config
);
    // Byte 0x100 + bank*0x80: tag, control, viewport X/Y, 8 exclusion X/Y.
    // Half-open rectangles use 16-bit endpoints; only 12 bits are implemented.
    logic [31:0] bank [0:2][0:19];
    integer b, w;
    always_ff @(posedge clk) begin
        if (!resetn) begin
            for (b=0; b<3; b=b+1)
                for (w=0; w<20; w=w+1) bank[b][w] <= 0;
        end else if (write_enable && write_address >= 8'h40 &&
                     write_address < 8'ha0 && write_address[4:0] < 20) begin
            bank[(write_address-8'h40)>>5][write_address[4:0]] <=
                globals::apply_wstrb(
                    bank[(write_address-8'h40)>>5][write_address[4:0]],
                    write_data, write_strobe);
        end
    end
    always_comb begin
        read_data = 0;
        if (read_address >= 8'h40 && read_address < 8'ha0 && read_address[4:0] < 20)
            read_data = bank[(read_address-8'h40)>>5][read_address[4:0]];
    end

    // This counter survives pixel-clock mode resets. Returning its value with
    // each mailbox payload prevents a delayed pre-reset/previous-frame packet
    // from enabling a mask on a different frame. Gray CDC arrives before the
    // existing four-stage vblank pulse CDC and subsequent reader latch.
    logic [7:0] pixel_frame_id = 0;
    wire epoch_resetn;
    wire [7:0] source_frame_id;
    reset_sync reset_sync_mask_epoch_i (
        .clk(pixel_clk), .arst_n(resetn), .srst_n(epoch_resetn)
    );
    always_ff @(posedge pixel_clk) begin
        if (!epoch_resetn) pixel_frame_id <= 0;
        else if (vblank_start) pixel_frame_id <= pixel_frame_id + 1'b1;
    end
    xpm_cdc_gray #(.WIDTH(8), .DEST_SYNC_FF(2), .INIT_SYNC_FF(1),
        // The binary counter has an allowed global-reset discontinuity.
        .REG_OUTPUT(0), .SIM_ASSERT_CHK(0), .SIM_LOSSLESS_GRAY_CHK(0)) frame_id_cdc (
        .src_clk(pixel_clk), .src_in_bin(pixel_frame_id),
        .dest_clk(clk), .dest_out_bin(source_frame_id)
    );

    logic [615:0] matched_config;
    integer i, j;
    always_comb begin
        matched_config = {source_frame_id, 608'b0};
        for (i=0; i<3; i=i+1)
            if (frame_base != 0 && bank[i][0] == frame_base)
                for (j=1; j<20; j=j+1)
                    matched_config[(j-1)*32 +: 32] = bank[i][j];
    end
    logic [615:0] pending_payload, send_payload;
    logic pending, send = 0;
    wire received, destination_valid;
    wire [615:0] destination_payload;
    always_ff @(posedge clk) begin
        if (!resetn) begin
            pending <= 0;
            send <= 0;
            pending_payload <= 0;
            send_payload <= 0;
        end else begin
            if (send && received) send <= 0;
            if (!send && !received && pending) begin
                send_payload <= pending_payload;
                send <= 1;
                pending <= 0;
            end
            if (frame_latched) begin
                pending_payload <= matched_config;
                pending <= 1;
            end
        end
    end
    xpm_cdc_handshake #(.WIDTH(616), .DEST_EXT_HSK(0),
        .DEST_SYNC_FF(4), .SRC_SYNC_FF(4), .INIT_SYNC_FF(1),
        .SIM_ASSERT_CHK(1)) config_mailbox (
        .src_clk(clk), .src_in(send_payload), .src_send(send), .src_rcv(received),
        .dest_clk(pixel_clk), .dest_out(destination_payload),
        .dest_req(destination_valid), .dest_ack(1'b0)
    );
    always_ff @(posedge pixel_clk) begin
        if (!video_resetn || vblank_start) pixel_config <= 0;
        else if (destination_valid && vertical_blank &&
                 destination_payload[615:608] == pixel_frame_id)
            pixel_config <= destination_payload[607:0];
    end
endmodule
