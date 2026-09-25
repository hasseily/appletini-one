`timescale 1ns / 1ps

import disk2_sound_pkg::*;

module disk2_sound_player (
    input  logic               clk,
    input  logic               rstn,
    input  logic               enable,
    input  logic [3:0]         volume,
    input  logic               audio_tick,
    input  logic [31:0]        sample_base_addr,

    input  logic               drive_spinning,
    input  logic [7:0]         qtrack,
    input  logic [3:0]         sound_event,
    input  logic [7:0]         seek_start_qtrack,
    input  logic [7:0]         seek_distance,

    Axi3_read_if.master        sample_read,
    output logic signed [15:0] audio_l,
    output logic signed [15:0] audio_r
);

    localparam logic [3:0] EVENT_NONE         = 4'd0;
    localparam logic [3:0] EVENT_SEEK_OUTWARD = 4'd1;
    localparam logic [3:0] EVENT_SEEK_INWARD  = 4'd2;
    localparam logic [3:0] EVENT_RECAL_ZERO   = 4'd3;
    localparam logic [3:0] EVENT_DOOR_OPEN    = 4'd4;
    localparam logic [3:0] EVENT_DOOR_CLOSE   = 4'd5;
    // The boot ROM steps about every 19.25 ms. Keep one seek recording
    // running between steps, then stop 25 ms after the last movement.
    localparam logic [17:0] SEEK_TIMEOUT_SAMPLES = 18'd1200;

    logic [17:0] idle_addr_q;
    logic [17:0] idle_fetch_addr_q;
    logic        idle_req_q;
    logic signed [15:0] idle_fetch_data;
    logic               idle_fetch_valid;
    logic signed [15:0] idle_sample_q;

    logic [17:0] event_fetch_addr_q;
    logic [17:0] event_pending_addr_q;
    logic [17:0] event_next_addr_q;
    logic [17:0] event_fetch_remaining_q;
    logic [17:0] event_output_remaining_q;
    logic        event_need_fetch_q;
    logic        event_req_q;
    logic        event_fetch_discard_q;
    logic signed [15:0] event_fetch_data;
    logic               event_fetch_valid;
    logic signed [15:0] event_sample_q;
    logic               event_recal_q;
    logic               event_seek_q;
    logic [3:0]         event_direction_q;
    logic [17:0]        event_sound_begin_q;
    logic [17:0]        event_sound_end_q;
    logic signed [15:0] mix_q;
    logic [3:0]         volume_q;
    logic               mix_valid_q;
    logic signed [26:0] volume_product_q;
    logic               volume_product_valid_q;
    logic               event_setup_valid_q;
    logic [2:0]         event_setup_sound_id_q;
    logic [3:0]         event_setup_event_q;
    logic [7:0]         event_setup_seek_start_qtrack_q;
    logic [7:0]         event_setup_seek_distance_q;
    logic               event_pos_valid_q;
    logic [2:0]         event_pos_sound_id_q;
    logic [3:0]         event_pos_event_q;
    logic [17:0]        event_pos_sound_offset_q;
    logic [17:0]        event_pos_full_length_q;
    logic [7:0]         event_pos_sample_start_pos_q;
    logic [7:0]         event_pos_sample_end_pos_q;
    logic               event_pos_seek_q;
    logic               event_calc_valid_q;
    logic [3:0]         event_calc_event_q;
    logic [17:0]        event_calc_sound_offset_q;
    logic [17:0]        event_calc_full_length_q;
    logic [17:0]        event_calc_start_offset_q;
    logic               event_calc_seek_q;
    logic               event_calc_zero_length_q;

    wire active = enable && (volume != 4'd0) && (sample_base_addr != 32'h00000000);
    wire event_playing = (event_output_remaining_q != 18'd0);
    wire recal_playing = event_playing && event_recal_q;
    wire seek_motor_stopped = event_seek_q && !drive_spinning;
    wire seek_refresh = event_seek_q && event_playing && drive_spinning &&
        (sound_event == event_direction_q) && (seek_distance != 8'd0) &&
        (((sound_event == EVENT_SEEK_OUTWARD) &&
          (seek_start_qtrack < DISK2_SOUND_SEEK_FULL_QTRACK_DISTANCE)) ||
         ((sound_event == EVENT_SEEK_INWARD) && (seek_start_qtrack != 8'd0)));
    wire event_calc_commit = event_calc_valid_q && !recal_playing &&
        !(event_calc_seek_q && (event_calc_zero_length_q || !drive_spinning));
    wire seek_continues = event_calc_commit && event_calc_seek_q &&
        event_seek_q && event_playing && (event_calc_event_q == event_direction_q);
    wire event_replaces = event_calc_commit && !seek_continues;

    audio_pcm16_ddr_fetcher #(
        .SAMPLE_ADDR_WIDTH(18)
    ) sample_fetcher_i (
        .clk(clk),
        .rstn(rstn && active),
        .sample_base_addr(sample_base_addr),
        .voice0_addr(idle_fetch_addr_q),
        // The fetcher's reply is registered. Suppress the held request on
        // that reply cycle so its idle state cannot grant the same request twice.
        .voice0_req(idle_req_q && !idle_fetch_valid),
        .voice0_data(idle_fetch_data),
        .voice0_valid(idle_fetch_valid),
        .voice1_addr(event_fetch_addr_q),
        .voice1_req(event_req_q && !event_fetch_valid),
        .voice1_data(event_fetch_data),
        .voice1_valid(event_fetch_valid),
        .axi_read(sample_read)
    );

    function automatic logic [2:0] select_event_sound(
        input logic [3:0] event_code,
        input logic [7:0] current_qtrack
    );
        begin
            unique case (event_code)
                EVENT_RECAL_ZERO: select_event_sound = DISK2_SOUND_TRACK0_RECAL;
                EVENT_SEEK_OUTWARD: select_event_sound = DISK2_SOUND_SEEK_0_34;
                EVENT_SEEK_INWARD: select_event_sound = DISK2_SOUND_SEEK_34_0;
                EVENT_DOOR_OPEN: select_event_sound = DISK2_SOUND_DOOR_OPEN;
                EVENT_DOOR_CLOSE: select_event_sound = DISK2_SOUND_DOOR_CLOSE;
                default: select_event_sound = DISK2_SOUND_IDLE_SPIN;
            endcase
        end
    endfunction

    function automatic logic event_is_seek(input logic [3:0] event_code);
        event_is_seek = (event_code == EVENT_SEEK_OUTWARD) ||
                        (event_code == EVENT_SEEK_INWARD);
    endfunction

    function automatic logic [7:0] seek_position(input logic [7:0] raw_qtrack);
        begin
            if (raw_qtrack > DISK2_SOUND_SEEK_FULL_QTRACK_DISTANCE)
                seek_position = DISK2_SOUND_SEEK_FULL_QTRACK_DISTANCE;
            else
                seek_position = raw_qtrack;
        end
    endfunction

    function automatic logic [7:0] seek_forward_end(
        input logic [7:0] start_pos,
        input logic [7:0] distance
    );
        logic [8:0] end_ext;
        begin
            end_ext = {1'b0, start_pos} + {1'b0, distance};
            if (end_ext > {1'b0, DISK2_SOUND_SEEK_FULL_QTRACK_DISTANCE})
                seek_forward_end = DISK2_SOUND_SEEK_FULL_QTRACK_DISTANCE;
            else
                seek_forward_end = end_ext[7:0];
        end
    endfunction

    function automatic logic [7:0] seek_reverse_end(
        input logic [7:0] start_pos,
        input logic [7:0] distance
    );
        begin
            if (distance >= start_pos)
                seek_reverse_end = 8'd0;
            else
                seek_reverse_end = start_pos - distance;
        end
    endfunction

    function automatic logic [7:0] seek_reverse_sample_position(input logic [7:0] position);
        seek_reverse_sample_position = DISK2_SOUND_SEEK_FULL_QTRACK_DISTANCE - position;
    endfunction

    function automatic logic [9:0] disk2_volume_scale_coeff(input logic [3:0] volume_level);
        begin
            unique case (volume_level)
                4'd0: disk2_volume_scale_coeff = 10'd0;
                4'd1: disk2_volume_scale_coeff = 10'd51;
                4'd2: disk2_volume_scale_coeff = 10'd102;
                4'd3: disk2_volume_scale_coeff = 10'd154;
                4'd4: disk2_volume_scale_coeff = 10'd205;
                4'd5: disk2_volume_scale_coeff = 10'd256;
                4'd6: disk2_volume_scale_coeff = 10'd307;
                4'd7: disk2_volume_scale_coeff = 10'd358;
                4'd8: disk2_volume_scale_coeff = 10'd410;
                4'd9: disk2_volume_scale_coeff = 10'd461;
                default: disk2_volume_scale_coeff = 10'd512;
            endcase
        end
    endfunction

    function automatic logic signed [26:0] volume_product(
        input logic signed [15:0] sample,
        input logic [3:0] volume_level
    );
        begin
            volume_product = $signed(sample) * $signed({1'b0, disk2_volume_scale_coeff(volume_level)});
        end
    endfunction

    function automatic logic signed [15:0] sat_volume_product(
        input logic signed [26:0] scaled
    );
        logic signed [18:0] shifted;
        begin
            shifted = scaled >>> 8;
            if (shifted > 19'sd32767)
                sat_volume_product = 16'sh7FFF;
            else if (shifted < -19'sd32768)
                sat_volume_product = -16'sd32768;
            else
                sat_volume_product = shifted[15:0];
        end
    endfunction

    always_ff @(posedge clk) begin
        if (!rstn || !active) begin
            idle_addr_q <= 18'd0;
            idle_fetch_addr_q <= 18'd0;
            idle_req_q <= 1'b0;
            idle_sample_q <= 16'sd0;

            event_fetch_addr_q <= 18'd0;
            event_pending_addr_q <= 18'd0;
            event_next_addr_q <= 18'd0;
            event_fetch_remaining_q <= 18'd0;
            event_output_remaining_q <= 18'd0;
            event_need_fetch_q <= 1'b0;
            event_req_q <= 1'b0;
            event_fetch_discard_q <= 1'b0;
            event_sample_q <= 16'sd0;
            event_recal_q <= 1'b0;
            event_seek_q <= 1'b0;
            event_direction_q <= EVENT_NONE;
            event_sound_begin_q <= 18'd0;
            event_sound_end_q <= 18'd0;
            mix_q <= 16'sd0;
            volume_q <= 4'd0;
            mix_valid_q <= 1'b0;
            volume_product_q <= 27'sd0;
            volume_product_valid_q <= 1'b0;
            event_setup_valid_q <= 1'b0;
            event_setup_sound_id_q <= DISK2_SOUND_IDLE_SPIN;
            event_setup_event_q <= EVENT_NONE;
            event_setup_seek_start_qtrack_q <= 8'd0;
            event_setup_seek_distance_q <= 8'd0;
            event_pos_valid_q <= 1'b0;
            event_pos_sound_id_q <= DISK2_SOUND_IDLE_SPIN;
            event_pos_event_q <= EVENT_NONE;
            event_pos_sound_offset_q <= 18'd0;
            event_pos_full_length_q <= 18'd0;
            event_pos_sample_start_pos_q <= 8'd0;
            event_pos_sample_end_pos_q <= 8'd0;
            event_pos_seek_q <= 1'b0;
            event_calc_valid_q <= 1'b0;
            event_calc_event_q <= EVENT_NONE;
            event_calc_sound_offset_q <= 18'd0;
            event_calc_full_length_q <= 18'd0;
            event_calc_start_offset_q <= 18'd0;
            event_calc_seek_q <= 1'b0;
            event_calc_zero_length_q <= 1'b0;
            audio_l <= 16'sd0;
            audio_r <= 16'sd0;
        end else begin
            if (volume_product_valid_q) begin
                audio_l <= sat_volume_product(volume_product_q);
                audio_r <= sat_volume_product(volume_product_q);
                volume_product_valid_q <= 1'b0;
            end

            if (mix_valid_q) begin
                volume_product_q <= volume_product(mix_q, volume_q);
                volume_product_valid_q <= 1'b1;
                mix_valid_q <= 1'b0;
            end

            if (idle_fetch_valid) begin
                idle_sample_q <= idle_fetch_data;
                idle_req_q <= 1'b0;
            end

            if (event_fetch_valid) begin
                if (!event_fetch_discard_q)
                    event_sample_q <= event_fetch_data;
                event_req_q <= 1'b0;
                event_fetch_discard_q <= 1'b0;
            end

            if (event_calc_valid_q) begin
                event_calc_valid_q <= 1'b0;
                if (event_replaces) begin
                    automatic logic [17:0] sound_start_addr;
                    automatic logic [17:0] sound_end_addr;
                    automatic logic [17:0] sound_length;

                    sound_start_addr = event_calc_sound_offset_q +
                        (event_calc_seek_q ? event_calc_start_offset_q : 18'd0);
                    sound_end_addr = event_calc_sound_offset_q + event_calc_full_length_q;
                    sound_length = event_calc_seek_q ?
                        SEEK_TIMEOUT_SAMPLES : event_calc_full_length_q;

                    // Keep an outstanding request's address stable and drain
                    // its reply before fetching the replacement clip's first sample.
                    event_pending_addr_q <= sound_start_addr;
                    event_next_addr_q <= (event_calc_seek_q &&
                        sound_start_addr + 18'd1 >= sound_end_addr) ?
                        event_calc_sound_offset_q : (sound_start_addr + 18'd1);
                    event_fetch_remaining_q <= event_calc_seek_q || sound_length == 18'd0 ?
                        18'd0 : (sound_length - 18'd1);
                    event_output_remaining_q <= sound_length;
                    event_need_fetch_q <= (sound_length != 18'd0);
                    event_fetch_discard_q <= event_req_q && !event_fetch_valid;
                    event_sample_q <= 16'sd0;
                    event_recal_q <= (event_calc_event_q == EVENT_RECAL_ZERO) &&
                        (sound_length != 18'd0);
                    event_seek_q <= event_calc_seek_q;
                    event_direction_q <= event_calc_event_q;
                    event_sound_begin_q <= event_calc_sound_offset_q;
                    event_sound_end_q <= sound_end_addr;
                end
            end

            if (event_pos_valid_q) begin
                event_calc_event_q <= event_pos_event_q;
                event_calc_sound_offset_q <= event_pos_sound_offset_q;
                event_calc_full_length_q <= event_pos_full_length_q;
                event_calc_start_offset_q <=
                    event_pos_seek_q ?
                        disk2_sound_seek_position_offset(
                            event_pos_sound_id_q,
                            event_pos_sample_start_pos_q) :
                        18'd0;
                event_calc_seek_q <= event_pos_seek_q;
                /* Compare the registered positions in this existing stage.
                 * This keeps event latency unchanged and avoids carrying the
                 * clamp/add/reverse cone into the prior stage's equality. */
                event_calc_zero_length_q <= event_pos_seek_q &&
                    (event_pos_sample_end_pos_q == event_pos_sample_start_pos_q);
                event_calc_valid_q <= 1'b1;
                event_pos_valid_q <= 1'b0;
            end

            if (event_setup_valid_q) begin
                automatic logic       seek_event;
                automatic logic [7:0] start_pos;
                automatic logic [7:0] end_pos;
                automatic logic [7:0] sample_start_pos;
                automatic logic [7:0] sample_end_pos;

                seek_event = event_is_seek(event_setup_event_q);
                start_pos = seek_position(event_setup_seek_start_qtrack_q);
                end_pos = start_pos;
                sample_start_pos = start_pos;
                sample_end_pos = start_pos;

                if (event_setup_event_q == EVENT_SEEK_OUTWARD) begin
                    end_pos = seek_forward_end(start_pos, event_setup_seek_distance_q);
                    sample_start_pos = start_pos;
                    sample_end_pos = end_pos;
                end else if (event_setup_event_q == EVENT_SEEK_INWARD) begin
                    end_pos = seek_reverse_end(start_pos, event_setup_seek_distance_q);
                    sample_start_pos = seek_reverse_sample_position(start_pos);
                    sample_end_pos = seek_reverse_sample_position(end_pos);
                end

                event_pos_sound_id_q <= event_setup_sound_id_q;
                event_pos_event_q <= event_setup_event_q;
                event_pos_sound_offset_q <= disk2_sound_offset(event_setup_sound_id_q);
                event_pos_full_length_q <= disk2_sound_length(event_setup_sound_id_q);
                event_pos_sample_start_pos_q <= sample_start_pos;
                event_pos_sample_end_pos_q <= sample_end_pos;
                event_pos_seek_q <= seek_event;
                event_pos_valid_q <= 1'b1;
                event_setup_valid_q <= 1'b0;
            end

            if (sound_event != EVENT_NONE && !recal_playing) begin
                automatic logic [2:0] sound_id = select_event_sound(sound_event, qtrack);

                event_setup_sound_id_q <= sound_id;
                event_setup_event_q <= sound_event;
                event_setup_seek_start_qtrack_q <= seek_start_qtrack;
                event_setup_seek_distance_q <= seek_distance;
                event_setup_valid_q <= 1'b1;
            end

            if (event_need_fetch_q && !event_req_q &&
                !event_replaces && !seek_motor_stopped) begin
                event_fetch_addr_q <= event_pending_addr_q;
                event_req_q <= 1'b1;
                event_need_fetch_q <= 1'b0;
            end

            if (audio_tick) begin
                if (drive_spinning) begin
                    if (!idle_req_q) begin
                        idle_fetch_addr_q <= disk2_sound_offset(DISK2_SOUND_IDLE_SPIN) + idle_addr_q;
                        idle_req_q <= 1'b1;

                        if (idle_addr_q + 18'd1 >= disk2_sound_length(DISK2_SOUND_IDLE_SPIN))
                            idle_addr_q <= 18'd0;
                        else
                            idle_addr_q <= idle_addr_q + 18'd1;
                    end
                end else begin
                    idle_addr_q <= 18'd0;
                    idle_req_q <= 1'b0;
                    idle_sample_q <= 16'sd0;
                end

                // A new clip owns its counters and queued address on this
                // cycle. A same-direction step leaves the sample cursor alone.
                if (!event_replaces && !seek_motor_stopped) begin
                    if (event_output_remaining_q != 18'd0) begin
                        event_output_remaining_q <= event_output_remaining_q - 18'd1;
                        if (event_output_remaining_q == 18'd1)
                            event_recal_q <= 1'b0;
                        if ((event_output_remaining_q > 18'd1 || seek_refresh) &&
                            (event_seek_q || event_fetch_remaining_q != 18'd0) &&
                            !event_req_q && !event_need_fetch_q) begin
                            event_pending_addr_q <= event_next_addr_q;
                            event_next_addr_q <= (event_seek_q &&
                                event_next_addr_q + 18'd1 >= event_sound_end_q) ?
                                event_sound_begin_q : (event_next_addr_q + 18'd1);
                            if (!event_seek_q)
                                event_fetch_remaining_q <= event_fetch_remaining_q - 18'd1;
                            event_need_fetch_q <= 1'b1;
                        end
                    end else begin
                        event_sample_q <= 16'sd0;
                    end
                end

                begin
                    automatic logic signed [15:0] idle_mix =
                        drive_spinning ? (idle_sample_q >>> 2) : 16'sd0;
                    automatic logic signed [15:0] event_mix =
                        (event_playing && !seek_motor_stopped) ?
                            (event_sample_q >>> 1) : 16'sd0;
                    // The /4 and /2 inputs sum to [-24576,24574], so this
                    // addition cannot clip. Keep saturation after volume
                    // scaling, which can still exceed the 16-bit range.
                    mix_q <= idle_mix + event_mix;
                    volume_q <= volume;
                    mix_valid_q <= 1'b1;
                end
            end

            // Refresh on receipt, before the setup pipeline, so a step on the
            // final timeout tick continues the same recording without a gap.
            if (seek_refresh && !event_replaces)
                event_output_remaining_q <= SEEK_TIMEOUT_SAMPLES;

            if (seek_motor_stopped && !event_replaces) begin
                event_output_remaining_q <= 18'd0;
                event_fetch_remaining_q <= 18'd0;
                event_need_fetch_q <= 1'b0;
                event_fetch_discard_q <= event_req_q && !event_fetch_valid;
                event_sample_q <= 16'sd0;
                event_seek_q <= 1'b0;
            end
        end
    end

endmodule
