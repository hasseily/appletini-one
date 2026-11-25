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
    //
    // The three banks are a 60-word distributed RAM, one RAM for each byte
    // lane. Flip-flops for all 60 words cost a write decode for each byte, a
    // 60-word read multiplexer and a 608-bit bank multiplexer. Only the three
    // tags stay in flip-flops, because all three are compared in one clock.
    // The RAM has no reset: a bank has no effect until its tag matches, and
    // the PS writes words 1-19 before it writes the tag.
    function automatic logic valid_address(input logic [7:0] address);
        valid_address = address >= 8'h40 && address < 8'ha0 && address[4:0] < 20;
    endfunction
    // RAM address: bank * 20 + word.
    function automatic logic [5:0] ram_address(input logic [1:0] bank_index,
                                               input logic [4:0] word);
        ram_address = 6'({bank_index, 4'b0} + {bank_index, 2'b0}) + 6'(word);
    endfunction
    function automatic logic [1:0] address_bank(input logic [7:0] address);
        logic [7:0] offset;
        offset = address - 8'h40;
        address_bank = offset[6:5];
    endfunction

    (* ram_style = "distributed" *) logic [7:0] lane0 [0:63];
    (* ram_style = "distributed" *) logic [7:0] lane1 [0:63];
    (* ram_style = "distributed" *) logic [7:0] lane2 [0:63];
    (* ram_style = "distributed" *) logic [7:0] lane3 [0:63];
    initial for (int k = 0; k < 64; k++) begin
        lane0[k] = 0; lane1[k] = 0; lane2[k] = 0; lane3[k] = 0;
    end

    wire write_valid = write_enable && valid_address(write_address);
    wire [5:0] write_ram_address =
        ram_address(address_bank(write_address), write_address[4:0]);
    wire [5:0] read_ram_address =
        ram_address(address_bank(read_address), read_address[4:0]);
    logic [31:0] tag [0:2];
    always_ff @(posedge clk) begin
        if (write_valid) begin
            if (write_strobe[0]) lane0[write_ram_address] <= write_data[7:0];
            if (write_strobe[1]) lane1[write_ram_address] <= write_data[15:8];
            if (write_strobe[2]) lane2[write_ram_address] <= write_data[23:16];
            if (write_strobe[3]) lane3[write_ram_address] <= write_data[31:24];
        end
        if (!resetn) begin
            for (int b = 0; b < 3; b++) tag[b] <= 0;
        end else if (write_valid && write_address[4:0] == 5'd0) begin
            tag[address_bank(write_address)] <= globals::apply_wstrb(
                tag[address_bank(write_address)], write_data, write_strobe);
        end
    end
    always_comb begin
        read_data = 0;
        if (valid_address(read_address)) begin
            // After a reset the tags are zero; the RAM keeps its old tag.
            if (read_address[4:0] == 5'd0)
                read_data = tag[address_bank(read_address)];
            else
                read_data = {lane3[read_ram_address], lane2[read_ram_address],
                             lane1[read_ram_address], lane0[read_ram_address]};
        end
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

    // The bank whose tag is the latched frame base. The highest bank wins,
    // as it did when all three banks were compared in parallel.
    logic matched;
    logic [1:0] matched_bank;
    always_comb begin
        matched = 0;
        matched_bank = 0;
        for (int b = 0; b < 3; b++)
            if (frame_base != 0 && tag[b] == frame_base) begin
                matched = 1;
                matched_bank = 2'(b);
            end
    end

    // Copy words 1-19 of the matched bank into the payload, one word in each
    // clock. The PS does not change the bank of a pending or scanning frame.
    // The payload becomes pending when the copy ends, 19 clocks after the
    // latch, which is far inside the vertical blanking that receives it.
    logic copying;
    logic [1:0] copy_bank;
    logic [4:0] copy_word;
    wire [5:0] copy_ram_address = ram_address(copy_bank, copy_word);
    wire [31:0] copy_data = {lane3[copy_ram_address], lane2[copy_ram_address],
                             lane1[copy_ram_address], lane0[copy_ram_address]};

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
            copying <= 0;
            copy_bank <= 0;
            copy_word <= 5'd1;
        end else begin
            if (send && received) send <= 0;
            if (!send && !received && pending) begin
                send_payload <= pending_payload;
                send <= 1;
                pending <= 0;
            end
            if (copying) begin
                pending_payload[(copy_word - 5'd1) * 32 +: 32] <= copy_data;
                copy_word <= copy_word + 5'd1;
                if (copy_word == 5'd19) begin
                    copying <= 0;
                    pending <= 1;
                end
            end
            if (frame_latched) begin
                // A payload of an earlier frame that was not sent is dropped,
                // as before.
                pending_payload <= {source_frame_id, 608'b0};
                copying <= matched;
                copy_bank <= matched_bank;
                copy_word <= 5'd1;
                pending <= !matched;
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
