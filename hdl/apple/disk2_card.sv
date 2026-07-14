`timescale 1ns / 1ps

// Disk II PL front end.
//
// Exposes the standard 16-sector slot ROM and Disk II soft switches. The PS
// stages the active nibble stream in reserved DDR; this module keeps only
// a tiny line prefetch cache for serialized Apple reads.
module disk2_card (
    input  logic                     clk,
    input  logic                     rstn,
    input  globals::AppleBus_read    ab_read,
    input  globals::SoftSwitchState  sss,
    input  logic [2:0]               slot_assign,
    input  globals::AxiSimple_common as_common,
    AxiSimple_if.client              as_client,
    output logic [20:0]              mc_line_addr,
    output logic                     mc_rw,
    output logic [63:0]              mc_wdata,
    output logic [7:0]               mc_wstrb,
    output logic                     mc_valid,
    input  logic                     mc_ready,
    input  logic [63:0]              mc_rdata,
    input  logic                     mc_rvalid,
    output globals::AppleBus_write   ab_write,
    output logic                     sound_spinning,
    output logic [7:0]               sound_qtrack,
    output logic [3:0]               sound_event,
    output logic [7:0]               sound_seek_start_qtrack,
    output logic [7:0]               sound_seek_distance
);

    localparam logic [7:0] D2_REG_STATUS       = 8'h00;
    localparam logic [7:0] D2_REG_CONTROL      = 8'h01;
    localparam logic [7:0] D2_REG_PSRAM_BASE   = 8'h02;
    localparam logic [7:0] D2_REG_UNDERRUNS    = 8'h03;
    localparam logic [7:0] D2_REG_LAST_IO      = 8'h04;
    localparam logic [7:0] D2_REG_IO_COUNT     = 8'h05;
    localparam logic [7:0] D2_REG_TRACK_INFO   = 8'h06;
    localparam logic [7:0] D2_REG_TRACK_LENGTH = 8'h07;
    localparam logic [7:0] D2_REG_TRACK_INDEX  = 8'h08;
    localparam logic [7:0] D2_REG_TRACK_DATA   = 8'h09;
    localparam logic [7:0] D2_REG_STREAM_POS   = 8'h0A;
    localparam logic [7:0] D2_REG_STREAM_READS = 8'h0B;
    localparam logic [7:0] D2_REG_WRITE_INFO   = 8'h0C;
    localparam logic [7:0] D2_REG_WRITE_COUNT  = 8'h0D;
    localparam logic [7:0] D2_REG_TRACK_BIT_COUNT = 8'h0E;
    localparam logic [7:0] D2_REG_TRACK_BIT_OFFSET = 8'h0F;
    localparam logic [7:0] D2_REG_TRACK_BIT_TIMING = 8'h15;
    localparam logic [7:0] D2_REG_TRACK_SEAM = 8'h16;

    localparam logic [7:0] D2_REG_D1_INFO      = 8'h10;
    localparam logic [7:0] D2_REG_D1_SIZE      = 8'h11;
    localparam logic [7:0] D2_REG_D1_TRACKS    = 8'h12;
    localparam logic [7:0] D2_REG_D1_BASE      = 8'h13;
    localparam logic [7:0] D2_REG_D1_LENGTH    = 8'h14;

    localparam logic [7:0] D2_REG_D2_INFO      = 8'h18;
    localparam logic [7:0] D2_REG_D2_SIZE      = 8'h19;
    localparam logic [7:0] D2_REG_D2_TRACKS    = 8'h1A;
    localparam logic [7:0] D2_REG_D2_BASE      = 8'h1B;
    localparam logic [7:0] D2_REG_D2_LENGTH    = 8'h1C;
    localparam logic [7:0] D2_REG_WOZ_ALIAS_RANGE = 8'h20;

    localparam logic [31:0] D2_PSRAM_BASE_RESET = 32'h0070_0000;
    localparam logic [27:0] SPIN_DOWN_TICKS = 28'd133_000_000;

    localparam logic [3:0] IO_PHASE0_OFF = 4'h0;
    localparam logic [3:0] IO_PHASE0_ON  = 4'h1;
    localparam logic [3:0] IO_PHASE1_OFF = 4'h2;
    localparam logic [3:0] IO_PHASE1_ON  = 4'h3;
    localparam logic [3:0] IO_PHASE2_OFF = 4'h4;
    localparam logic [3:0] IO_PHASE2_ON  = 4'h5;
    localparam logic [3:0] IO_PHASE3_OFF = 4'h6;
    localparam logic [3:0] IO_PHASE3_ON  = 4'h7;
    localparam logic [3:0] IO_MOTOR_OFF  = 4'h8;
    localparam logic [3:0] IO_MOTOR_ON   = 4'h9;
    localparam logic [3:0] IO_DRIVE1     = 4'hA;
    localparam logic [3:0] IO_DRIVE2     = 4'hB;
    localparam logic [3:0] IO_Q6_LOW     = 4'hC;
    localparam logic [3:0] IO_Q6_HIGH    = 4'hD;
    localparam logic [3:0] IO_Q7_LOW     = 4'hE;
    localparam logic [3:0] IO_Q7_HIGH    = 4'hF;

    localparam logic [3:0] SOUND_EVENT_NONE         = 4'd0;
    localparam logic [3:0] SOUND_EVENT_SEEK_OUTWARD = 4'd1;
    localparam logic [3:0] SOUND_EVENT_SEEK_INWARD  = 4'd2;
    localparam logic [3:0] SOUND_EVENT_RECAL_ZERO   = 4'd3;
    localparam logic [7:0] RECAL_REARM_QTRACK     = 8'd4;
    localparam logic [5:0] STANDARD_SPIN_FIRST_IDLE = 6'd40;
    localparam logic [5:0] STANDARD_SPIN_SECOND_IDLE = 6'd23;
    localparam logic [5:0] STANDARD_SPIN_REPEAT_IDLE = 6'd32;
    localparam logic [2:0] STANDARD_READ_GAP_LIMIT = 3'd7;

    localparam int unsigned TRACK_STREAM_BYTES = 8192;
    localparam logic [12:0] TRACK_STREAM_LAST = 13'(TRACK_STREAM_BYTES - 1);
    localparam int unsigned PREFETCH_LINES = 4;
    localparam int unsigned WRITE_FIFO_DEPTH = 16;
    localparam logic [31:0] WOZ_WEAK_RAND_FIRST = 32'h0029_E2C0;
    localparam logic [31:0] WOZ_WEAK_RAND_SECOND = 32'hC823_F683;
    localparam logic [17:0] WOZ_WEAK_RAND_MUL18 = 18'd214013;
    localparam logic [31:0] WOZ_WEAK_RAND_INC = 32'd2531011;
    localparam logic [14:0] WOZ_WEAK_RAND_THRESHOLD = 15'd9830;
    localparam logic [16:0] WOZ_SEAM_PRE_START_INVALID = 17'h1FFFF;
    logic [7:0] slot_rom [0:255];
    initial begin
        $readmemh("disk2_slot6.mem", slot_rom);
    end

    globals::AppleBus_write ab_write_d;
    globals::AppleBus_write ab_write_q;
    assign ab_write = ab_write_q;

    logic [31:0] control_q;
    logic [31:0] psram_base_q;
    logic [31:0] underrun_count_q;
    logic [31:0] drive_info_q [0:1];
    logic [31:0] drive_size_q [0:1];
    logic [31:0] drive_tracks_q [0:1];
    logic [31:0] drive_base_q [0:1];
    logic [31:0] drive_length_q [0:1];
    logic [3:0] phase_on_q;
    logic       motor_on_q;
    logic       drive_select_q;
    logic [27:0] spin_countdown_q [0:1];
    logic       step_pending_q;
    logic [3:0] step_pending_addr_q;
    logic [3:0] step_delay_q;
    logic       q6_q;
    logic       q7_q;
    logic [7:0] last_io_q;
    logic [31:0] io_access_count_q;
    logic [7:0] drive_phase_q [0:1];
    logic [7:0] drive_qtrack_q [0:1];
    logic       track_loaded_q;
    logic       track_woz_q;
    logic       loaded_drive_q;
    logic [7:0] loaded_qtrack_q;
    logic [7:0] woz_alias_lo_q;
    logic [7:0] woz_alias_hi_q;
    logic       woz_alias_drive_q [0:1];
    logic [13:0] track_length_q;
    logic [12:0] track_write_index_q;
    logic [12:0] track_stream_pos_q;
    logic [5:0]  standard_spin_countdown_q;
    logic        standard_spin_repeat_q;
    logic [2:0]  standard_read_gap_q;
    logic [16:0] track_bit_count_q;
    logic [16:0] track_bit_offset_q;
    logic [7:0] disk_latch_q;
    logic [7:0] woz_shift_q;
    logic [3:0] woz_latch_delay_q;
    logic [3:0] woz_head_window_q;
    logic       woz_write_started_q;
    logic [7:0]  track_bit_timing_q;
    logic [15:0] woz_seam_start_q;
    logic [15:0] woz_seam_run_q;
    logic [16:0] woz_seam_pre_start_q;
    logic        woz_seam_recalc_q;
    logic        woz_seam_arm_q;
    logic [15:0] woz_bit_accum_q;
    logic [31:0] woz_weak_rand_q;
    logic        woz_weak_rand_bit_q;
    logic [31:0] woz_weak_next_rand_q;
    logic        woz_weak_next_rand_bit_q;
    logic        woz_weak_refill_pending_q;
    logic [1:0]  woz_weak_refill_stage_q;
    logic [33:0] woz_weak_lcg_lo_q;
    logic [33:0] woz_weak_lcg_hi_q;
    logic        woz_write_pending_q;
    logic [20:0] woz_write_line_q;
    logic [7:0]  woz_write_byte_q;
    logic [2:0]  woz_write_offset_q;
    logic        woz_cached_valid_q;
    logic        woz_cached_ready_q;
    logic [7:0]  woz_cached_byte_q;
    logic [7:0]  woz_cached_mask_q;
    logic [20:0] woz_cached_line_q;
    logic [2:0]  woz_cached_offset_q;
    logic [31:0] stream_read_count_q;
    logic        write_dirty_q;
    logic        write_dirty_drive_q;
    logic [7:0]  write_dirty_qtrack_q;
    logic [31:0] stream_write_count_q;
    logic [31:0] as_client_rdata_q;
    logic [3:0]  sound_event_q;
    logic [7:0]  sound_seek_start_qtrack_q;
    logic [7:0]  sound_seek_distance_q;
    logic        sound_step_valid_q;
    logic [7:0]  sound_step_start_qtrack_q;
    logic [7:0]  sound_step_end_qtrack_q;
    logic        sound_step_track0_stop_q;
    logic        sound_step_recal_armed_q;
    logic        recal_sound_armed_q [0:1];

    logic [20:0] prefetch_line_q [0:PREFETCH_LINES-1];
    logic [63:0] prefetch_data_q [0:PREFETCH_LINES-1];
    logic [PREFETCH_LINES-1:0] prefetch_valid_q;
    logic [1:0] prefetch_replace_q;
    logic       prefetch_req_q;
    logic       prefetch_resp_pending_q;
    logic [20:0] prefetch_req_line_q;
    logic [1:0] prefetch_req_slot_q;
    logic [20:0] prefetch_current_line_q;
    logic [20:0] prefetch_next_line_q;
    logic        cache_patch_pending_q;
    logic [20:0] cache_patch_line_q;
    logic [7:0]  cache_patch_byte_q;
    logic [2:0]  cache_patch_offset_q;

    logic [20:0] write_fifo_line_q [0:WRITE_FIFO_DEPTH-1];
    logic [7:0]  write_fifo_byte_q [0:WRITE_FIFO_DEPTH-1];
    logic [2:0]  write_fifo_offset_q [0:WRITE_FIFO_DEPTH-1];
    logic [3:0]  write_fifo_head_q;
    logic [3:0]  write_fifo_tail_q;
    logic [4:0]  write_fifo_count_q;
    logic        disk_write_pending_q;
    logic [20:0] disk_write_line_q;
    logic [7:0]  disk_write_byte_q;
    logic [2:0]  disk_write_offset_q;
    logic        write_req_q;
    logic [20:0] write_req_line_q;
    logic [7:0]  write_req_byte_q;
    logic [2:0]  write_req_offset_q;

    logic        current_line_hit;
    logic [63:0] current_line_data;
    logic [20:0] current_line_addr;
    logic [2:0]  current_line_offset;
    logic        stream_line_hit_q;
    logic [63:0] stream_line_data_q;
    logic [20:0] stream_line_addr_q;
    logic [2:0]  stream_line_offset_q;
    logic        prefetch_need_req;
    logic [20:0] prefetch_next_req_line;
    logic [1:0]  prefetch_next_req_slot;
    logic [12:0] active_stream_pos;

    wire enabled = (slot_assign != 3'h0);
    wire apple_bus_active = enabled &&
                            ((slot_assign != 3'h3) || sss.sw_slotc3rom) &&
                            ab_read.res;
    wire slot_rom_hit =
        apple_bus_active &&
        sss.slot_access &&
        (ab_read.addr[15:12] == 4'hC) &&
        (ab_read.addr[11] == 1'b0) &&
        (ab_read.addr[10:8] == slot_assign);
    wire slot_io_hit =
        apple_bus_active &&
        (ab_read.addr[15:8] == 8'hC0) &&
        (ab_read.addr[7:4] == (4'h8 + {1'b0, slot_assign}));
    wire ab_rom_read = ab_read.sss_en && ab_read.rw && slot_rom_hit;
    wire ab_io_read  = ab_read.sss_en && ab_read.rw && slot_io_hit;
    wire ab_io_write = ab_read.data_en && !ab_read.rw && slot_io_hit;
    wire [3:0] io_idx = ab_read.addr[3:0];
    wire stepper_io_access = (ab_io_read || ab_io_write) && (io_idx <= IO_PHASE3_ON);
    wire drive_connected = drive_info_q[drive_select_q][0];
    wire drive_read_only = drive_info_q[drive_select_q][1];
    wire [7:0] current_qtrack = drive_qtrack_q[drive_select_q];
    wire selected_track_loaded =
        track_loaded_q &&
        (loaded_drive_q == drive_select_q) &&
        (track_length_q != 14'd0) &&
        (!track_woz_q || (track_bit_count_q != 17'd0));
    // WOZ can keep one raw bitstream active across aliased quarter tracks.
    // Standard images use exact qtrack handoff; the non-WOZ load path clears
    // the alias range when TRACK_INFO is committed.
    wire exact_drive_loaded =
        selected_track_loaded &&
        (loaded_qtrack_q == current_qtrack);
    wire woz_alias_loaded =
        selected_track_loaded &&
        woz_alias_drive_q[drive_select_q];
    wire active_drive_loaded = track_woz_q ? woz_alias_loaded : exact_drive_loaded;
    wire stream_track_loaded = active_drive_loaded;
    wire woz_track_stream_ready = woz_alias_loaded;
    wire drive_spinning = drive_connected && (motor_on_q || (spin_countdown_q[drive_select_q] != 28'd0));
    assign sound_spinning = drive_spinning;
    assign sound_qtrack = current_qtrack;
    assign sound_event = sound_event_q;
    assign sound_seek_start_qtrack = sound_seek_start_qtrack_q;
    assign sound_seek_distance = sound_seek_distance_q;
    wire q6_after_access =
        (io_idx == IO_Q6_LOW)  ? 1'b0 :
        (io_idx == IO_Q6_HIGH) ? 1'b1 : q6_q;
    wire q7_after_access =
        (io_idx == IO_Q7_LOW)  ? 1'b0 :
        (io_idx == IO_Q7_HIGH) ? 1'b1 : q7_q;
    wire disk_stream_access =
        (ab_io_read || ab_io_write) &&
        drive_spinning &&
        !track_woz_q &&
        (io_idx == IO_Q6_LOW);
    wire standard_stream_active =
        drive_spinning &&
        !track_woz_q &&
        stream_track_loaded;
    wire standard_spin_tick =
        standard_stream_active &&
        ab_read.sss_en &&
        !disk_stream_access &&
        (standard_spin_countdown_q == 6'd1);
    wire standard_partial_read =
        disk_stream_access &&
        active_drive_loaded &&
        !q7_after_access &&
        (standard_read_gap_q <= 3'd6);
    wire [3:0] standard_invalid_bits =
        4'd8 - {3'b000, standard_read_gap_q[2]};
    wire woz_io_access =
        (ab_io_read || ab_io_write) &&
        drive_spinning &&
        track_woz_q &&
        woz_track_stream_ready;
    wire woz_q6_mode = woz_io_access ? q6_after_access : q6_q;
    wire woz_q7_mode = woz_io_access ? q7_after_access : q7_q;
    wire woz_data_load_access =
        woz_io_access &&
        ab_io_write &&
        q6_after_access &&
        q7_after_access;
    wire woz_load_write_protect_access =
        (q6_q && !q7_q) ||
        (woz_io_access &&
         woz_q6_mode &&
         !woz_q7_mode);
    wire woz_write_mode =
        (q7_q && !q6_q) ||
        (woz_io_access && q7_after_access && !q6_after_access);
    wire woz_read_mode =
        !woz_load_write_protect_access &&
        !woz_q7_mode &&
        !woz_q6_mode;
    wire woz_latch_load_mode = woz_q7_mode && woz_q6_mode;
    wire [7:0] woz_effective_bit_timing =
        (track_bit_timing_q < 8'd8) ? 8'd32 : track_bit_timing_q;
    wire [16:0] woz_accum_plus_cycle = {1'b0, woz_bit_accum_q} + 17'd8;
    wire [15:0] woz_accum_plus_saturated =
        woz_accum_plus_cycle[16] ? 16'hFFFF : woz_accum_plus_cycle[15:0];
    wire woz_stream_active =
        ab_read.sss_en &&
        drive_spinning &&
        track_woz_q &&
        woz_track_stream_ready;
    wire woz_bit_cell_tick =
        woz_stream_active &&
        (woz_accum_plus_cycle >= {9'h000, woz_effective_bit_timing});
    wire woz_cache_before_tick =
        woz_stream_active &&
        !woz_bit_cell_tick &&
        (woz_accum_plus_cycle + 17'd8 >= {9'h000, woz_effective_bit_timing});
    wire [7:0] disk_next_byte =
        (stream_track_loaded && stream_line_hit_q) ?
        line_byte(stream_line_data_q, stream_line_offset_q) :
        8'hFF;
    wire [7:0] standard_read_byte =
        standard_partial_read ? (disk_next_byte >> standard_invalid_bits) : disk_next_byte;
    wire [7:0] disk_read_byte = (disk_stream_access && !q7_after_access) ? standard_read_byte : disk_latch_q;
    wire disk_track_write =
        disk_stream_access &&
        q7_after_access &&
        active_drive_loaded &&
        !drive_read_only;
    wire disk_data_load =
        ab_io_write &&
        q6_after_access &&
        q7_after_access;
    wire write_fifo_full = (write_fifo_count_q == 5'd16);
    wire write_busy =
        write_req_q ||
        disk_write_pending_q ||
        woz_write_pending_q ||
        (write_fifo_count_q != 5'd0);

    function automatic logic [7:0] slot_rom_byte(input logic [7:0] addr);
        slot_rom_byte = slot_rom[addr];
    endfunction

    function automatic logic [12:0] stream_pos_next(input logic [12:0] pos,
                                                    input logic [13:0] length);
        if (length <= 14'd1 || {1'b0, pos} + 14'd1 >= length)
            stream_pos_next = 13'd0;
        else
            stream_pos_next = pos + 13'd1;
    endfunction

    function automatic logic [23:0] stream_byte_addr(input logic [31:0] base,
                                                     input logic [12:0] pos);
        stream_byte_addr = {base[23:3] + {11'd0, pos[12:3]}, pos[2:0]};
    endfunction

    function automatic logic [20:0] stream_line_addr(input logic [31:0] base,
                                                     input logic [12:0] pos);
        stream_line_addr = base[23:3] + {11'd0, pos[12:3]};
    endfunction

    function automatic logic [2:0] stream_line_offset(input logic [31:0] base,
                                                      input logic [12:0] pos);
        stream_line_offset = pos[2:0];
    endfunction

    function automatic logic prefetch_wrap_after_pos(input logic [12:0] pos,
                                                     input logic [13:0] length);
        logic [13:0] next_line_start;
        begin
            next_line_start = {1'b0, pos[12:3], 3'b000} + 14'd8;
            prefetch_wrap_after_pos = (length == 14'd0) || (next_line_start >= length);
        end
    endfunction

    function automatic logic [7:0] line_byte(input logic [63:0] line,
                                             input logic [2:0] offset);
        case (offset)
            3'd0: line_byte = line[7:0];
            3'd1: line_byte = line[15:8];
            3'd2: line_byte = line[23:16];
            3'd3: line_byte = line[31:24];
            3'd4: line_byte = line[39:32];
            3'd5: line_byte = line[47:40];
            3'd6: line_byte = line[55:48];
            default: line_byte = line[63:56];
        endcase
    endfunction

    function automatic logic [63:0] line_with_byte(input logic [63:0] line,
                                                   input logic [2:0] offset,
                                                   input logic [7:0] value);
        line_with_byte = line;
        case (offset)
            3'd0: line_with_byte[7:0] = value;
            3'd1: line_with_byte[15:8] = value;
            3'd2: line_with_byte[23:16] = value;
            3'd3: line_with_byte[31:24] = value;
            3'd4: line_with_byte[39:32] = value;
            3'd5: line_with_byte[47:40] = value;
            3'd6: line_with_byte[55:48] = value;
            default: line_with_byte[63:56] = value;
        endcase
    endfunction

    function automatic logic [12:0] woz_byte_pos(input logic [16:0] bit_offset);
        woz_byte_pos = bit_offset[15:3] & TRACK_STREAM_LAST;
    endfunction

    function automatic logic [16:0] raw_bit_offset_next(input logic [16:0] offset,
                                                        input logic [16:0] bit_count,
                                                        input logic step_two);
        logic [17:0] next_offset;
        next_offset = {1'b0, offset} + (step_two ? 18'd2 : 18'd1);
        if (bit_count != 17'd0 && next_offset >= {1'b0, bit_count}) begin
            next_offset = next_offset - {1'b0, bit_count};
            if (next_offset >= {1'b0, bit_count})
                next_offset = 18'd0;
        end
        raw_bit_offset_next = next_offset[16:0];
    endfunction

    function automatic logic [16:0] woz_seam_pre_start(input logic [15:0] seam_start,
                                                       input logic [16:0] bit_count);
        logic [16:0] seam_start_ext;
        begin
            seam_start_ext = {1'b0, seam_start};
            if (bit_count == 17'd0 || seam_start_ext >= bit_count)
                woz_seam_pre_start = WOZ_SEAM_PRE_START_INVALID;
            else if (seam_start == 16'd0)
                woz_seam_pre_start = bit_count - 17'd1;
            else
                woz_seam_pre_start = seam_start_ext - 17'd1;
        end
    endfunction

    function automatic logic woz_weak_rand_bit(input logic [31:0] rand_state);
        woz_weak_rand_bit = (rand_state[30:16] < WOZ_WEAK_RAND_THRESHOLD);
    endfunction

    function automatic logic woz_rand_5_10(input logic [31:0] rand_state);
        woz_rand_5_10 = (rand_state[30:16] < 15'd16384);
    endfunction

    function automatic logic [15:0] stepper_result(input logic [7:0] phase,
                                                   input logic [3:0] magnets,
                                                   input logic is_woz);
        logic [1:0] head_phase;
        logic [1:0] plus_phase;
        logic [1:0] minus_phase;
        logic [7:0] next_phase;
        logic [7:0] next_qtrack;
        logic       move_plus;
        logic       move_minus;
        logic       adjacent_pair;

        head_phase = phase[1:0];
        plus_phase = head_phase + 2'd1;
        minus_phase = head_phase - 2'd1;
        next_phase = phase;
        next_qtrack = phase + phase;
        move_plus = magnets[plus_phase] && !magnets[minus_phase];
        move_minus = magnets[minus_phase] && !magnets[plus_phase];
        adjacent_pair =
            magnets == 4'hC ||
            magnets == 4'h6 ||
            magnets == 4'h3 ||
            magnets == 4'h9;

        if (is_woz && adjacent_pair) begin
            if (move_plus)
                next_qtrack = phase + phase + 8'd1;
            else if (move_minus && phase != 8'd0)
                next_qtrack = phase + phase - 8'd1;
        end else begin
            if (move_plus && phase < 8'd79)
                next_phase = phase + 8'd1;
            else if (move_minus && phase != 8'd0)
                next_phase = phase - 8'd1;
            next_qtrack = next_phase + next_phase;
        end

        stepper_result = {next_qtrack, next_phase};
    endfunction

    function automatic logic [3:0] sound_event_for_step(input logic [7:0] old_qtrack,
                                                        input logic [7:0] new_qtrack,
                                                        input logic track0_stop_hit,
                                                        input logic recal_armed);
        begin
            if (track0_stop_hit && recal_armed)
                sound_event_for_step = SOUND_EVENT_RECAL_ZERO;
            else if (new_qtrack > old_qtrack)
                sound_event_for_step = SOUND_EVENT_SEEK_OUTWARD;
            else if (new_qtrack < old_qtrack)
                sound_event_for_step = SOUND_EVENT_SEEK_INWARD;
            else
                sound_event_for_step = SOUND_EVENT_NONE;
        end
    endfunction

    function automatic logic [7:0] sound_seek_distance_for_step(input logic [7:0] old_qtrack,
                                                               input logic [7:0] new_qtrack);
        begin
            if (new_qtrack > old_qtrack)
                sound_seek_distance_for_step = new_qtrack - old_qtrack;
            else if (old_qtrack > new_qtrack)
                sound_seek_distance_for_step = old_qtrack - new_qtrack;
            else
                sound_seek_distance_for_step = 8'd0;
        end
    endfunction

    function automatic logic stepper_hits_track0_stop(input logic [7:0] phase,
                                                      input logic [3:0] magnets);
        logic [1:0] head_phase;
        logic [1:0] minus_phase;
        logic [1:0] plus_phase;
        logic       move_minus;
        begin
            head_phase = phase[1:0];
            minus_phase = head_phase - 2'd1;
            plus_phase = head_phase + 2'd1;
            move_minus = magnets[minus_phase] && !magnets[plus_phase];
            stepper_hits_track0_stop = move_minus && (phase == 8'd0);
        end
    endfunction

    function automatic logic woz_alias_hit(input logic [7:0] qtrack);
        woz_alias_hit = (qtrack >= woz_alias_lo_q) && (qtrack <= woz_alias_hi_q);
    endfunction

    function automatic logic [1:0] choose_prefetch_slot;
        if (!prefetch_valid_q[0])
            choose_prefetch_slot = 2'd0;
        else if (!prefetch_valid_q[1])
            choose_prefetch_slot = 2'd1;
        else if (!prefetch_valid_q[2])
            choose_prefetch_slot = 2'd2;
        else if (!prefetch_valid_q[3])
            choose_prefetch_slot = 2'd3;
        else
            choose_prefetch_slot = prefetch_replace_q;
    endfunction

    function automatic logic [31:0] track_info_word;
        track_info_word = 32'h0000_0000;
        track_info_word[0] = track_loaded_q;
        track_info_word[1] = active_drive_loaded;
        track_info_word[2] = track_woz_q;
        track_info_word[15:8] = current_qtrack;
        track_info_word[16] = drive_select_q;
        track_info_word[20] = loaded_drive_q;
        track_info_word[31:24] = loaded_qtrack_q;
    endfunction

    always_comb begin
        if (track_woz_q)
            active_stream_pos = woz_byte_pos(track_bit_offset_q);
        else
            active_stream_pos = track_stream_pos_q;
    end

    always_comb begin
        logic        next_line_hit;
        logic        pending_current;
        logic        pending_next;

        current_line_addr = prefetch_current_line_q;
        current_line_offset = active_stream_pos[2:0];
        current_line_hit = 1'b0;
        current_line_data = 64'hFFFF_FFFF_FFFF_FFFF;
        for (int i = 0; i < PREFETCH_LINES; ++i) begin
            if (prefetch_valid_q[i] && prefetch_line_q[i] == current_line_addr) begin
                current_line_hit = 1'b1;
                current_line_data = prefetch_data_q[i];
            end
        end

        next_line_hit = 1'b0;
        for (int i = 0; i < PREFETCH_LINES; ++i) begin
            if (prefetch_valid_q[i] && prefetch_line_q[i] == prefetch_next_line_q)
                next_line_hit = 1'b1;
        end

        pending_current =
            (!write_req_q && prefetch_req_q && prefetch_req_line_q == prefetch_current_line_q) ||
            (prefetch_resp_pending_q && prefetch_req_line_q == prefetch_current_line_q);
        pending_next =
            (!write_req_q && prefetch_req_q && prefetch_req_line_q == prefetch_next_line_q) ||
            (prefetch_resp_pending_q && prefetch_req_line_q == prefetch_next_line_q);

        prefetch_need_req = 1'b0;
        prefetch_next_req_line = prefetch_current_line_q;
        prefetch_next_req_slot = choose_prefetch_slot();
        if (stream_track_loaded && !write_req_q && !prefetch_req_q && !prefetch_resp_pending_q) begin
            if (!current_line_hit && !pending_current) begin
                prefetch_need_req = 1'b1;
                prefetch_next_req_line = prefetch_current_line_q;
            end else if (!next_line_hit && !pending_next) begin
                prefetch_need_req = 1'b1;
                prefetch_next_req_line = prefetch_next_line_q;
            end
        end
    end

    assign mc_line_addr = write_req_q ? write_req_line_q : prefetch_req_line_q;
    assign mc_rw = !write_req_q;
    assign mc_wdata = {8{write_req_byte_q}};
    assign mc_wstrb = write_req_q ? (8'b1 << write_req_offset_q) : 8'h00;
    assign mc_valid = write_req_q || prefetch_req_q;

    always_ff @(posedge clk) begin
        if (!rstn) begin
            control_q <= 32'h0000_0000;
            psram_base_q <= D2_PSRAM_BASE_RESET;
            underrun_count_q <= 32'h0000_0000;
            drive_info_q[0] <= 32'h0000_0000;
            drive_info_q[1] <= 32'h0000_0000;
            drive_size_q[0] <= 32'h0000_0000;
            drive_size_q[1] <= 32'h0000_0000;
            drive_tracks_q[0] <= 32'h0000_0000;
            drive_tracks_q[1] <= 32'h0000_0000;
            drive_base_q[0] <= 32'h0000_0000;
            drive_base_q[1] <= 32'h0000_0000;
            drive_length_q[0] <= 32'h0000_0000;
            drive_length_q[1] <= 32'h0000_0000;
            phase_on_q <= 4'h0;
            motor_on_q <= 1'b0;
            drive_select_q <= 1'b0;
            spin_countdown_q[0] <= 28'd0;
            spin_countdown_q[1] <= 28'd0;
            step_pending_q <= 1'b0;
            step_pending_addr_q <= 4'h0;
            step_delay_q <= 4'd0;
            q6_q <= 1'b0;
            q7_q <= 1'b0;
            last_io_q <= 8'h00;
            io_access_count_q <= 32'h0000_0000;
            drive_phase_q[0] <= 8'h00;
            drive_phase_q[1] <= 8'h00;
            drive_qtrack_q[0] <= 8'h00;
            drive_qtrack_q[1] <= 8'h00;
            track_loaded_q <= 1'b0;
            track_woz_q <= 1'b0;
            loaded_drive_q <= 1'b0;
            loaded_qtrack_q <= 8'h00;
            woz_alias_lo_q <= 8'hFF;
            woz_alias_hi_q <= 8'h00;
            woz_alias_drive_q[0] <= 1'b0;
            woz_alias_drive_q[1] <= 1'b0;
            track_length_q <= 14'd0;
            track_write_index_q <= 13'd0;
            track_stream_pos_q <= 13'd0;
            standard_spin_countdown_q <= STANDARD_SPIN_FIRST_IDLE;
            standard_spin_repeat_q <= 1'b0;
            standard_read_gap_q <= STANDARD_READ_GAP_LIMIT;
            track_bit_count_q <= 17'd0;
            track_bit_offset_q <= 17'd0;
            track_bit_timing_q <= 8'd32;
            woz_seam_start_q <= 16'd0;
            woz_seam_run_q <= 16'd0;
            woz_seam_pre_start_q <= WOZ_SEAM_PRE_START_INVALID;
            woz_seam_recalc_q <= 1'b0;
            woz_seam_arm_q <= 1'b0;
            disk_latch_q <= 8'hFF;
            woz_shift_q <= 8'h00;
            woz_latch_delay_q <= 4'd0;
            woz_head_window_q <= 4'd0;
            woz_write_started_q <= 1'b0;
            woz_bit_accum_q <= 16'd0;
            woz_weak_rand_q <= WOZ_WEAK_RAND_FIRST;
            woz_weak_rand_bit_q <= woz_weak_rand_bit(WOZ_WEAK_RAND_FIRST);
            woz_weak_next_rand_q <= WOZ_WEAK_RAND_SECOND;
            woz_weak_next_rand_bit_q <= woz_weak_rand_bit(WOZ_WEAK_RAND_SECOND);
            woz_weak_refill_pending_q <= 1'b0;
            woz_weak_refill_stage_q <= 2'd0;
            woz_weak_lcg_lo_q <= 34'd0;
            woz_weak_lcg_hi_q <= 34'd0;
            woz_write_pending_q <= 1'b0;
            woz_write_line_q <= 21'd0;
            woz_write_byte_q <= 8'h00;
            woz_write_offset_q <= 3'd0;
            woz_cached_valid_q <= 1'b0;
            woz_cached_ready_q <= 1'b0;
            woz_cached_byte_q <= 8'hFF;
            woz_cached_mask_q <= 8'h80;
            woz_cached_line_q <= 21'd0;
            woz_cached_offset_q <= 3'd0;
            stream_read_count_q <= 32'h0000_0000;
            write_dirty_q <= 1'b0;
            write_dirty_drive_q <= 1'b0;
            write_dirty_qtrack_q <= 8'h00;
            stream_write_count_q <= 32'h0000_0000;
            as_client_rdata_q <= 32'h0000_0000;
            sound_event_q <= SOUND_EVENT_NONE;
            sound_seek_start_qtrack_q <= 8'd0;
            sound_seek_distance_q <= 8'd0;
            sound_step_valid_q <= 1'b0;
            sound_step_start_qtrack_q <= 8'd0;
            sound_step_end_qtrack_q <= 8'd0;
            sound_step_track0_stop_q <= 1'b0;
            sound_step_recal_armed_q <= 1'b0;
            recal_sound_armed_q[0] <= 1'b1;
            recal_sound_armed_q[1] <= 1'b1;
            prefetch_valid_q <= '0;
            prefetch_replace_q <= 2'd0;
            prefetch_req_q <= 1'b0;
            prefetch_resp_pending_q <= 1'b0;
            prefetch_req_line_q <= 21'd0;
            prefetch_req_slot_q <= 2'd0;
            prefetch_current_line_q <= 21'd0;
            prefetch_next_line_q <= 21'd0;
            stream_line_hit_q <= 1'b0;
            stream_line_data_q <= 64'hFFFF_FFFF_FFFF_FFFF;
            stream_line_addr_q <= 21'd0;
            stream_line_offset_q <= 3'd0;
            cache_patch_pending_q <= 1'b0;
            cache_patch_line_q <= 21'd0;
            cache_patch_byte_q <= 8'h00;
            cache_patch_offset_q <= 3'd0;
            write_fifo_head_q <= 4'd0;
            write_fifo_tail_q <= 4'd0;
            write_fifo_count_q <= 5'd0;
            disk_write_pending_q <= 1'b0;
            disk_write_line_q <= 21'd0;
            disk_write_byte_q <= 8'h00;
            disk_write_offset_q <= 3'd0;
            write_req_q <= 1'b0;
            write_req_line_q <= 21'd0;
            write_req_byte_q <= 8'h00;
            write_req_offset_q <= 3'd0;
            for (int i = 0; i < PREFETCH_LINES; ++i) begin
                prefetch_line_q[i] <= 21'd0;
                prefetch_data_q[i] <= 64'hFFFF_FFFF_FFFF_FFFF;
            end
            for (int i = 0; i < WRITE_FIFO_DEPTH; ++i) begin
                write_fifo_line_q[i] <= 21'd0;
                write_fifo_byte_q[i] <= 8'h00;
                write_fifo_offset_q[i] <= 3'd0;
            end
            ab_write_q <= '0;
        end else begin
            automatic logic [20:0] active_line_next =
                stream_line_addr(psram_base_q, active_stream_pos);
            automatic logic prefetch_wrap_next =
                prefetch_wrap_after_pos(active_stream_pos, track_length_q);
            automatic logic write_fifo_pop_v;
            automatic logic write_fifo_push_v;
            automatic logic write_fifo_push_woz_v;

            stream_line_hit_q <= current_line_hit;
            stream_line_data_q <= current_line_data;
            stream_line_addr_q <= current_line_addr;
            stream_line_offset_q <= current_line_offset;

            ab_write_q <= ab_write_d;
            sound_event_q <= SOUND_EVENT_NONE;
            sound_seek_start_qtrack_q <= 8'd0;
            sound_seek_distance_q <= 8'd0;
            if (sound_step_valid_q) begin
                sound_event_q <= sound_event_for_step(
                    sound_step_start_qtrack_q,
                    sound_step_end_qtrack_q,
                    sound_step_track0_stop_q,
                    sound_step_recal_armed_q);
                sound_seek_start_qtrack_q <= sound_step_start_qtrack_q;
                sound_seek_distance_q <= sound_seek_distance_for_step(
                    sound_step_start_qtrack_q,
                    sound_step_end_qtrack_q);
                sound_step_valid_q <= 1'b0;
            end
            woz_alias_drive_q[0] <= woz_alias_hit(drive_qtrack_q[0]);
            woz_alias_drive_q[1] <= woz_alias_hit(drive_qtrack_q[1]);
            prefetch_current_line_q <= active_line_next;
            prefetch_next_line_q <=
                prefetch_wrap_next ? psram_base_q[23:3] : (active_line_next + 21'd1);
            if (woz_seam_recalc_q) begin
                woz_seam_pre_start_q <= woz_seam_pre_start(woz_seam_start_q, track_bit_count_q);
                woz_seam_recalc_q <= 1'b0;
            end

            if (ab_read.sss_en) begin
                if (standard_read_gap_q != STANDARD_READ_GAP_LIMIT)
                    standard_read_gap_q <= standard_read_gap_q + 3'd1;

                if (standard_stream_active && !disk_stream_access) begin
                    if (standard_spin_tick) begin
                        track_stream_pos_q <= stream_pos_next(track_stream_pos_q, track_length_q);
                        standard_spin_countdown_q <=
                            standard_spin_repeat_q ?
                            STANDARD_SPIN_REPEAT_IDLE :
                            STANDARD_SPIN_SECOND_IDLE;
                        standard_spin_repeat_q <= 1'b1;
                    end else if (standard_spin_countdown_q != 6'd0) begin
                        standard_spin_countdown_q <= standard_spin_countdown_q - 6'd1;
                    end
                end else if (!standard_stream_active) begin
                    standard_spin_countdown_q <= STANDARD_SPIN_FIRST_IDLE;
                    standard_spin_repeat_q <= 1'b0;
                end
            end

            write_fifo_pop_v =
                !write_req_q &&
                write_fifo_count_q != 5'd0 &&
                (!disk_write_pending_q || write_fifo_full) &&
                (!woz_write_pending_q || write_fifo_full);
            write_fifo_push_v =
                (disk_write_pending_q || woz_write_pending_q) &&
                (!write_fifo_full || write_fifo_pop_v);
            write_fifo_push_woz_v =
                write_fifo_push_v && !disk_write_pending_q && woz_write_pending_q;

            if (write_req_q && mc_ready) begin
                write_req_q <= 1'b0;
            end
            if (write_fifo_pop_v) begin
                write_req_q <= 1'b1;
                write_req_line_q <= write_fifo_line_q[write_fifo_head_q];
                write_req_byte_q <= write_fifo_byte_q[write_fifo_head_q];
                write_req_offset_q <= write_fifo_offset_q[write_fifo_head_q];
                write_fifo_head_q <= write_fifo_head_q + 4'd1;
            end
            if (write_fifo_push_v) begin
                if (write_fifo_push_woz_v) begin
                    write_fifo_line_q[write_fifo_tail_q] <= woz_write_line_q;
                    write_fifo_byte_q[write_fifo_tail_q] <= woz_write_byte_q;
                    write_fifo_offset_q[write_fifo_tail_q] <= woz_write_offset_q;
                    write_dirty_q <= 1'b1;
                    write_dirty_drive_q <= loaded_drive_q;
                    write_dirty_qtrack_q <= loaded_qtrack_q;
                    stream_write_count_q <= stream_write_count_q + 32'd1;
                    woz_write_pending_q <= 1'b0;
                end else begin
                    write_fifo_line_q[write_fifo_tail_q] <= disk_write_line_q;
                    write_fifo_byte_q[write_fifo_tail_q] <= disk_write_byte_q;
                    write_fifo_offset_q[write_fifo_tail_q] <= disk_write_offset_q;
                    disk_write_pending_q <= 1'b0;
                end
                write_fifo_tail_q <= write_fifo_tail_q + 4'd1;
            end
            if (write_fifo_pop_v || write_fifo_push_v) begin
                write_fifo_count_q <= write_fifo_count_q +
                    {4'd0, write_fifo_push_v} -
                    {4'd0, write_fifo_pop_v};
            end
            case (woz_weak_refill_stage_q)
            2'd0: begin
                if (woz_weak_refill_pending_q) begin
                    woz_weak_lcg_lo_q <=
                        woz_weak_next_rand_q[15:0] * WOZ_WEAK_RAND_MUL18;
                    woz_weak_lcg_hi_q <=
                        woz_weak_next_rand_q[31:16] * WOZ_WEAK_RAND_MUL18;
                    woz_weak_refill_pending_q <= 1'b0;
                    woz_weak_refill_stage_q <= 2'd1;
                end
            end
            2'd1: begin
                woz_weak_next_rand_q <=
                    woz_weak_lcg_lo_q[31:0] +
                    {woz_weak_lcg_hi_q[15:0], 16'h0000} +
                    WOZ_WEAK_RAND_INC;
                woz_weak_refill_stage_q <= 2'd2;
            end
            2'd2: begin
                woz_weak_next_rand_bit_q <= woz_weak_rand_bit(woz_weak_next_rand_q);
                woz_weak_refill_stage_q <= 2'd0;
            end
            default: begin
                woz_weak_refill_stage_q <= 2'd0;
            end
            endcase

            if (!write_req_q && prefetch_req_q && mc_ready) begin
                prefetch_req_q <= 1'b0;
                prefetch_resp_pending_q <= 1'b1;
            end
            if (mc_rvalid && prefetch_resp_pending_q) begin
                prefetch_resp_pending_q <= 1'b0;
                prefetch_valid_q[prefetch_req_slot_q] <= 1'b1;
                prefetch_line_q[prefetch_req_slot_q] <= prefetch_req_line_q;
                prefetch_data_q[prefetch_req_slot_q] <= mc_rdata;
                prefetch_replace_q <= prefetch_req_slot_q + 2'd1;
            end
            if (cache_patch_pending_q) begin
                cache_patch_pending_q <= 1'b0;
                for (int i = 0; i < PREFETCH_LINES; ++i) begin
                    if (prefetch_valid_q[i] &&
                        prefetch_line_q[i] == cache_patch_line_q) begin
                        prefetch_data_q[i] <= line_with_byte(
                            prefetch_data_q[i],
                            cache_patch_offset_q,
                            cache_patch_byte_q);
                    end
                end
            end
            if (prefetch_need_req) begin
                prefetch_req_q <= 1'b1;
                prefetch_req_line_q <= prefetch_next_req_line;
                prefetch_req_slot_q <= prefetch_next_req_slot;
            end

            if (!motor_on_q) begin
                if (spin_countdown_q[0] != 28'd0)
                    spin_countdown_q[0] <= spin_countdown_q[0] - 28'd1;
                if (spin_countdown_q[1] != 28'd0)
                    spin_countdown_q[1] <= spin_countdown_q[1] - 28'd1;
            end else if (drive_connected) begin
                spin_countdown_q[drive_select_q] <= SPIN_DOWN_TICKS;
            end

            if (!enabled || !ab_read.res) begin
                phase_on_q <= 4'h0;
                motor_on_q <= 1'b0;
                drive_select_q <= 1'b0;
                spin_countdown_q[0] <= 28'd0;
                spin_countdown_q[1] <= 28'd0;
                step_pending_q <= 1'b0;
                step_pending_addr_q <= 4'h0;
                step_delay_q <= 4'd0;
                q6_q <= 1'b0;
                q7_q <= 1'b0;
                // NOTE: the emulated drive head position (drive_phase_q /
                // drive_qtrack_q) is deliberately NOT cleared here -- see the
                // separate `if (!enabled)` block below. A real Disk II has no
                // reset line; Apple RESET must not move the head.
                sound_seek_start_qtrack_q <= 8'd0;
                sound_step_valid_q <= 1'b0;
                sound_step_start_qtrack_q <= 8'd0;
                sound_step_end_qtrack_q <= 8'd0;
                sound_step_track0_stop_q <= 1'b0;
                sound_step_recal_armed_q <= 1'b0;
                recal_sound_armed_q[0] <= 1'b1;
                recal_sound_armed_q[1] <= 1'b1;
                track_stream_pos_q <= 13'd0;
                standard_spin_countdown_q <= STANDARD_SPIN_FIRST_IDLE;
                standard_spin_repeat_q <= 1'b0;
                standard_read_gap_q <= STANDARD_READ_GAP_LIMIT;
                track_bit_offset_q <= 17'd0;
                woz_shift_q <= 8'h00;
                woz_latch_delay_q <= 4'd0;
                woz_head_window_q <= 4'd0;
                woz_write_started_q <= 1'b0;
                woz_bit_accum_q <= 16'd0;
                woz_seam_arm_q <= 1'b0;
                woz_seam_recalc_q <= 1'b0;
                woz_cached_ready_q <= 1'b0;
                prefetch_current_line_q <= 21'd0;
                prefetch_next_line_q <= 21'd0;
                prefetch_valid_q <= '0;
                prefetch_req_q <= 1'b0;
                prefetch_resp_pending_q <= 1'b0;
                cache_patch_pending_q <= 1'b0;
                write_fifo_head_q <= 4'd0;
                write_fifo_tail_q <= 4'd0;
                write_fifo_count_q <= 5'd0;
                disk_write_pending_q <= 1'b0;
                woz_write_pending_q <= 1'b0;
                write_req_q <= 1'b0;
                disk_latch_q <= 8'hFF;
            end

            // Apple RESET must not move the emulated drive head. A Disk II has no
            // reset input, and DOS retains its current-track state across a warm
            // reset. Only power-on or disabling the virtual card clears the head.
            if (!enabled) begin
                drive_phase_q[0] <= 8'h00;
                drive_phase_q[1] <= 8'h00;
                drive_qtrack_q[0] <= 8'h00;
                drive_qtrack_q[1] <= 8'h00;
            end

            if (ab_io_read || ab_io_write) begin
                last_io_q <= {4'hC, io_idx};
                io_access_count_q <= io_access_count_q + 32'd1;

                case (io_idx)
                    IO_PHASE0_OFF, IO_PHASE0_ON,
                    IO_PHASE1_OFF, IO_PHASE1_ON,
                    IO_PHASE2_OFF, IO_PHASE2_ON,
                    IO_PHASE3_OFF, IO_PHASE3_ON: begin
                        automatic logic [3:0] next_phase_on = phase_on_q;
                        automatic logic [3:0] addr_delta;
                        automatic logic adjacent_quick_off;
                        automatic logic [15:0] step_next;
                        automatic logic track0_stop_hit;

                        if (drive_spinning) begin
                            next_phase_on[io_idx[2:1]] = io_idx[0];
                            phase_on_q <= next_phase_on;

                            addr_delta =
                                (step_pending_addr_q[2:0] > io_idx[2:0]) ?
                                ({1'b0, step_pending_addr_q[2:0]} - {1'b0, io_idx[2:0]}) :
                                ({1'b0, io_idx[2:0]} - {1'b0, step_pending_addr_q[2:0]});
                            adjacent_quick_off =
                                step_pending_q &&
                                (addr_delta == 4'd2 || addr_delta == 4'd6) &&
                                (io_idx[0] == 1'b0);

                            if (adjacent_quick_off) begin
                                step_pending_q <= 1'b0;
                                step_delay_q <= 4'd0;
                            end else begin
                                if (step_pending_q) begin
                                    step_next = stepper_result(
                                        drive_phase_q[drive_select_q],
                                        next_phase_on,
                                        drive_info_q[drive_select_q][7:4] == 4'd1);
                                    track0_stop_hit = stepper_hits_track0_stop(
                                        drive_phase_q[drive_select_q],
                                        next_phase_on);
                                    drive_phase_q[drive_select_q] <= step_next[7:0];
                                    drive_qtrack_q[drive_select_q] <= step_next[15:8];
                                    sound_step_start_qtrack_q <= drive_qtrack_q[drive_select_q];
                                    sound_step_end_qtrack_q <= step_next[15:8];
                                    sound_step_track0_stop_q <= track0_stop_hit;
                                    sound_step_recal_armed_q <= recal_sound_armed_q[drive_select_q];
                                    sound_step_valid_q <= 1'b1;
                                    if (step_next[15:8] >= RECAL_REARM_QTRACK) begin
                                        recal_sound_armed_q[drive_select_q] <= 1'b1;
                                    end else if (track0_stop_hit && recal_sound_armed_q[drive_select_q]) begin
                                        recal_sound_armed_q[drive_select_q] <= 1'b0;
                                    end
                                end
                                step_pending_q <= 1'b1;
                                step_pending_addr_q <= io_idx;
                                step_delay_q <= 4'd10;
                            end
                        end
                    end
                    IO_MOTOR_OFF: begin
                        motor_on_q <= 1'b0;
                        phase_on_q <= 4'h0;
                        step_pending_q <= 1'b0;
                        step_delay_q <= 4'd0;
                    end
                    IO_MOTOR_ON: begin
                        if (!drive_spinning) begin
                            standard_spin_countdown_q <= STANDARD_SPIN_FIRST_IDLE;
                            standard_spin_repeat_q <= 1'b0;
                        end
                        motor_on_q <= 1'b1;
                        if (drive_connected)
                            spin_countdown_q[drive_select_q] <= SPIN_DOWN_TICKS;
                    end
                    IO_DRIVE1: begin
                        if (motor_on_q && drive_info_q[0][0] && spin_countdown_q[0] == 28'd0) begin
                            standard_spin_countdown_q <= STANDARD_SPIN_FIRST_IDLE;
                            standard_spin_repeat_q <= 1'b0;
                        end
                        drive_select_q <= 1'b0;
                        spin_countdown_q[1] <= 28'd0;
                        if (motor_on_q && drive_info_q[0][0])
                            spin_countdown_q[0] <= SPIN_DOWN_TICKS;
                    end
                    IO_DRIVE2: begin
                        if (motor_on_q && drive_info_q[1][0] && spin_countdown_q[1] == 28'd0) begin
                            standard_spin_countdown_q <= STANDARD_SPIN_FIRST_IDLE;
                            standard_spin_repeat_q <= 1'b0;
                        end
                        drive_select_q <= 1'b1;
                        spin_countdown_q[0] <= 28'd0;
                        if (motor_on_q && drive_info_q[1][0])
                            spin_countdown_q[1] <= SPIN_DOWN_TICKS;
                    end
                    IO_Q6_LOW:     q6_q <= 1'b0;
                    IO_Q6_HIGH: begin
                        q6_q <= 1'b1;
                        if (drive_spinning) begin
                            disk_latch_q <= drive_read_only ? 8'hFF : 8'h00;
                            if (track_woz_q && !woz_write_started_q) begin
                                woz_shift_q <= 8'h00;
                                woz_latch_delay_q <= 4'd0;
                            end
                        end
                    end
                    IO_Q7_LOW:     q7_q <= 1'b0;
                    IO_Q7_HIGH:    q7_q <= 1'b1;
                    default: begin
                    end
                endcase

            end

            if (ab_read.sss_en && step_pending_q && !stepper_io_access) begin
                if (step_delay_q <= 4'd1) begin
                    automatic logic [15:0] step_next = stepper_result(
                        drive_phase_q[drive_select_q],
                        phase_on_q,
                        drive_info_q[drive_select_q][7:4] == 4'd1);
                    automatic logic track0_stop_hit = stepper_hits_track0_stop(
                        drive_phase_q[drive_select_q],
                        phase_on_q);
                    drive_phase_q[drive_select_q] <= step_next[7:0];
                    drive_qtrack_q[drive_select_q] <= step_next[15:8];
                    sound_step_start_qtrack_q <= drive_qtrack_q[drive_select_q];
                    sound_step_end_qtrack_q <= step_next[15:8];
                    sound_step_track0_stop_q <= track0_stop_hit;
                    sound_step_recal_armed_q <= recal_sound_armed_q[drive_select_q];
                    sound_step_valid_q <= 1'b1;
                    if (step_next[15:8] >= RECAL_REARM_QTRACK) begin
                        recal_sound_armed_q[drive_select_q] <= 1'b1;
                    end else if (track0_stop_hit && recal_sound_armed_q[drive_select_q]) begin
                        recal_sound_armed_q[drive_select_q] <= 1'b0;
                    end
                    step_pending_q <= 1'b0;
                    step_delay_q <= 4'd0;
                end else begin
                    step_delay_q <= step_delay_q - 4'd1;
                end
            end

            if (woz_io_access) begin
                if (woz_data_load_access) begin
                    woz_write_started_q <= 1'b1;
                    woz_shift_q <= ab_read.data;
                    disk_latch_q <= ab_read.data;
                    woz_seam_run_q <= 16'd0;
                    woz_seam_arm_q <= 1'b0;
                end
                if (!q7_after_access)
                    woz_write_started_q <= 1'b0;
            end

            if (woz_stream_active) begin
                automatic logic [16:0] next_bit_offset;
                automatic logic [8:0] cached_v;
                automatic logic [7:0] byte_v;
                automatic logic [7:0] mask_v;
                automatic logic [20:0] line_v;
                automatic logic [2:0] offset_v;
                automatic logic [7:0] shift_next_v;
                automatic logic [3:0] delay_next_v;
                automatic logic input_bit_v;
                automatic logic output_bit_v;
                automatic logic [3:0] head_next_v;
                automatic logic skip_bit_advance_v;
                automatic logic write_stall_v;
                automatic logic cache_retry_v;
                automatic logic seam_slip_v;
                automatic logic [16:0] accum_after_tick_v;

                if (woz_cache_before_tick) begin
                    automatic logic [7:0] cache_mask_v = 8'h80 >> track_bit_offset_q[2:0];

                    woz_cached_valid_q <= stream_line_hit_q;
                    woz_cached_ready_q <= 1'b1;
                    woz_cached_byte_q <= disk_next_byte;
                    woz_cached_mask_q <= cache_mask_v;
                    woz_cached_line_q <= stream_line_addr_q;
                    woz_cached_offset_q <= stream_line_offset_q;
                    woz_seam_arm_q <=
                        woz_read_mode &&
                        woz_seam_run_q > 16'd110 &&
                        woz_seam_pre_start_q != WOZ_SEAM_PRE_START_INVALID &&
                        track_bit_offset_q == woz_seam_pre_start_q;
                end

                if (woz_bit_cell_tick) begin
                    write_stall_v = 1'b0;
                    cache_retry_v = 1'b0;
                    if (woz_cached_ready_q) begin
                        cached_v = {woz_cached_valid_q, woz_cached_byte_q};
                        mask_v = woz_cached_mask_q;
                        line_v = woz_cached_line_q;
                        offset_v = woz_cached_offset_q;
                    end else begin
                        cached_v = {1'b0, 8'hFF};
                        mask_v = woz_cached_mask_q;
                        line_v = woz_cached_line_q;
                        offset_v = woz_cached_offset_q;
                        if (woz_write_mode &&
                            active_drive_loaded &&
                            !drive_read_only &&
                            stream_line_hit_q) begin
                            woz_cached_valid_q <= 1'b1;
                            woz_cached_ready_q <= 1'b1;
                            woz_cached_byte_q <= disk_next_byte;
                            woz_cached_mask_q <= 8'h80 >> track_bit_offset_q[2:0];
                            woz_cached_line_q <= stream_line_addr_q;
                            woz_cached_offset_q <= stream_line_offset_q;
                            cache_retry_v = 1'b1;
                        end
                    end
                    byte_v = cached_v[7:0];

                    if (woz_write_mode) begin
                        if (active_drive_loaded && !drive_read_only && cached_v[8] && !woz_write_pending_q) begin
                            if (woz_shift_q[7])
                                byte_v = byte_v | mask_v;
                            else
                                byte_v = byte_v & ~mask_v;
                            woz_write_pending_q <= 1'b1;
                            woz_write_line_q <= line_v;
                            woz_write_byte_q <= byte_v;
                            woz_write_offset_q <= offset_v;
                            cache_patch_pending_q <= 1'b1;
                            cache_patch_line_q <= line_v;
                            cache_patch_byte_q <= byte_v;
                            cache_patch_offset_q <= offset_v;
                        end else if (active_drive_loaded && (!cached_v[8] || woz_write_pending_q)) begin
                            underrun_count_q <= underrun_count_q + 32'd1;
                            if (!drive_read_only)
                                write_stall_v = 1'b1;
                        end
                        if (!write_stall_v && !woz_data_load_access)
                            woz_shift_q <= {woz_shift_q[6:0], 1'b0};
                    end else if (woz_load_write_protect_access) begin
                        // AppleWin's LoadWriteProtect() advances the WOZ bit
                        // stream and resets the LSS; it does not shift a read bit.
                    end else if (woz_read_mode) begin
                        input_bit_v = cached_v[8] && ((byte_v & mask_v) != 8'h00);
                        woz_head_window_q <= {woz_head_window_q[2:0], input_bit_v};
                        head_next_v = {woz_head_window_q[2:0], input_bit_v};
                        if (head_next_v != 4'h0) begin
                            output_bit_v = woz_head_window_q[0];
                        end else begin
                            output_bit_v = woz_weak_rand_bit_q;
                            woz_weak_rand_q <= woz_weak_next_rand_q;
                            woz_weak_rand_bit_q <= woz_weak_next_rand_bit_q;
                            woz_weak_refill_pending_q <= 1'b1;
                        end
                        if (!cached_v[8])
                            underrun_count_q <= underrun_count_q + 32'd1;

                        shift_next_v = {woz_shift_q[6:0], output_bit_v};
                        delay_next_v = woz_latch_delay_q;
                        woz_shift_q <= shift_next_v;
                        if (woz_latch_delay_q != 4'd0) begin
                            delay_next_v = (woz_latch_delay_q > 4'd4) ?
                                           (woz_latch_delay_q - 4'd4) : 4'd0;
                            if (shift_next_v == 8'h00)
                                delay_next_v = delay_next_v + 4'd4;
                            woz_latch_delay_q <= delay_next_v;
                        end
                        if (woz_latch_delay_q == 4'd0 ||
                            (woz_latch_delay_q <= 4'd4 &&
                             shift_next_v != 8'h00)) begin
                            disk_latch_q <= shift_next_v;
                            if (shift_next_v[7]) begin
                                woz_latch_delay_q <= 4'd7;
                                woz_shift_q <= 8'h00;
                            end
                        end
                    end else if (!cached_v[8]) begin
                        underrun_count_q <= underrun_count_q + 32'd1;
                    end

                    skip_bit_advance_v =
                        write_stall_v ||
                        (!woz_write_mode &&
                         woz_latch_load_mode &&
                         woz_write_started_q);
                    if (!skip_bit_advance_v) begin
                        seam_slip_v = woz_seam_arm_q && woz_rand_5_10(woz_weak_rand_q);
                        next_bit_offset =
                            raw_bit_offset_next(track_bit_offset_q, track_bit_count_q, seam_slip_v);
                        if (woz_seam_arm_q) begin
                            woz_weak_rand_q <= woz_weak_next_rand_q;
                            woz_weak_rand_bit_q <= woz_weak_next_rand_bit_q;
                            woz_weak_refill_pending_q <= 1'b1;
                        end
                        woz_seam_arm_q <= 1'b0;
                        track_bit_offset_q <= next_bit_offset;
                        accum_after_tick_v =
                            woz_accum_plus_cycle - {9'h000, woz_effective_bit_timing};
                        woz_bit_accum_q <= accum_after_tick_v[15:0];
                    end else begin
                        woz_seam_arm_q <= 1'b0;
                        // AppleWin does not call GetBitCellDelta() while the
                        // sequencer remains in dataLoadWrite. Carry that time
                        // forward so the next dataShiftWrite interval emits
                        // the same number of cells.
                        woz_bit_accum_q <= woz_accum_plus_saturated;
                    end
                    stream_read_count_q <= stream_read_count_q + 32'd1;
                    if (!cache_retry_v) begin
                        woz_cached_ready_q <= 1'b0;
                        woz_cached_valid_q <= 1'b0;
                    end
                end else begin
                    woz_bit_accum_q <= woz_accum_plus_saturated;
                end
            end else if (!drive_spinning || !track_woz_q || !stream_track_loaded) begin
                woz_bit_accum_q <= 16'd0;
                woz_seam_arm_q <= 1'b0;
                woz_cached_ready_q <= 1'b0;
                woz_cached_valid_q <= 1'b0;
            end

            if (disk_stream_access) begin
                automatic logic [12:0] next_pos =
                    standard_partial_read ?
                    active_stream_pos :
                    stream_pos_next(active_stream_pos, track_length_q);
                standard_spin_countdown_q <= STANDARD_SPIN_FIRST_IDLE;
                standard_spin_repeat_q <= 1'b0;
                if (disk_track_write) begin
                    if (!write_fifo_full && !disk_write_pending_q) begin
                        disk_write_pending_q <= 1'b1;
                        disk_write_line_q <= stream_line_addr_q;
                        disk_write_byte_q <= disk_latch_q;
                        disk_write_offset_q <= stream_line_offset_q;
                        cache_patch_pending_q <= 1'b1;
                        cache_patch_line_q <= stream_line_addr_q;
                        cache_patch_byte_q <= disk_latch_q;
                        cache_patch_offset_q <= stream_line_offset_q;
                        write_dirty_q <= 1'b1;
                        write_dirty_drive_q <= loaded_drive_q;
                        write_dirty_qtrack_q <= loaded_qtrack_q;
                        stream_write_count_q <= stream_write_count_q + 32'd1;
                    end else begin
                        underrun_count_q <= underrun_count_q + 32'd1;
                    end
                end else if (!q7_after_access) begin
                    disk_latch_q <= standard_read_byte;
                    if (!standard_partial_read && active_drive_loaded)
                        standard_read_gap_q <= 3'd0;
                    if (active_drive_loaded && !stream_line_hit_q)
                        underrun_count_q <= underrun_count_q + 32'd1;
                end
                track_stream_pos_q <= next_pos;
                stream_read_count_q <= stream_read_count_q + 32'd1;
            end

            if (disk_data_load)
                disk_latch_q <= ab_read.data;

            if (as_client.awvalid) begin
                case (as_common.awaddr)
                    D2_REG_CONTROL: begin
                        control_q <= globals::apply_wstrb(
                            control_q, as_common.wdata, as_common.wstrb);
                    end
                    D2_REG_PSRAM_BASE: begin
                        automatic logic [31:0] base_tmp = globals::apply_wstrb(
                            psram_base_q, as_common.wdata, as_common.wstrb);
                        psram_base_q <= {base_tmp[31:3], 3'b000};
                        prefetch_current_line_q <= 21'd0;
                        prefetch_next_line_q <= 21'd0;
                        prefetch_valid_q <= '0;
                        prefetch_req_q <= 1'b0;
                        prefetch_resp_pending_q <= 1'b0;
                        cache_patch_pending_q <= 1'b0;
                        woz_cached_valid_q <= 1'b0;
                        woz_cached_ready_q <= 1'b0;
                        woz_seam_arm_q <= 1'b0;
                        write_fifo_head_q <= 4'd0;
                        write_fifo_tail_q <= 4'd0;
                        write_fifo_count_q <= 5'd0;
                        disk_write_pending_q <= 1'b0;
                        woz_write_pending_q <= 1'b0;
                        write_req_q <= 1'b0;
                    end
                    D2_REG_UNDERRUNS: begin
                        underrun_count_q <= globals::apply_wstrb(
                            underrun_count_q, as_common.wdata, as_common.wstrb);
                    end
                    D2_REG_LAST_IO: begin
                        if (as_common.wstrb[0])
                            last_io_q <= as_common.wdata[7:0];
                    end
                    D2_REG_IO_COUNT: begin
                        io_access_count_q <= globals::apply_wstrb(
                            io_access_count_q, as_common.wdata, as_common.wstrb);
                    end
                    D2_REG_TRACK_INFO: begin
                        if (as_common.wstrb[0] && as_common.wdata[0] == 1'b0) begin
                            track_loaded_q <= 1'b0;
                            track_woz_q <= 1'b0;
                            woz_alias_lo_q <= 8'hFF;
                            woz_alias_hi_q <= 8'h00;
                            woz_alias_drive_q[0] <= 1'b0;
                            woz_alias_drive_q[1] <= 1'b0;
                            woz_seam_start_q <= 16'd0;
                            woz_seam_run_q <= 16'd0;
                            woz_seam_pre_start_q <= WOZ_SEAM_PRE_START_INVALID;
                            woz_seam_recalc_q <= 1'b0;
                            woz_seam_arm_q <= 1'b0;
                            prefetch_current_line_q <= 21'd0;
                            prefetch_next_line_q <= 21'd0;
                            prefetch_valid_q <= '0;
                            prefetch_req_q <= 1'b0;
                            prefetch_resp_pending_q <= 1'b0;
                            cache_patch_pending_q <= 1'b0;
                            woz_cached_valid_q <= 1'b0;
                            woz_cached_ready_q <= 1'b0;
                            write_fifo_head_q <= 4'd0;
                            write_fifo_tail_q <= 4'd0;
                            write_fifo_count_q <= 5'd0;
                            disk_write_pending_q <= 1'b0;
                            woz_write_pending_q <= 1'b0;
                            write_req_q <= 1'b0;
                            // DDR staging is about to be replaced or ejected.
                            // Clear write_dirty_q so a later flush cannot pair
                            // replacement bytes with this track's disk location.
                            write_dirty_q <= 1'b0;
                        end else if (as_common.wstrb[0] && as_common.wdata[0] == 1'b1) begin
                            track_loaded_q <= 1'b1;
                            track_woz_q <= as_common.wdata[2];
                            loaded_qtrack_q <= as_common.wdata[15:8];
                            loaded_drive_q <= as_common.wdata[20];
                            if (as_common.wdata[2]) begin
                                automatic logic [16:0] bit_offset_next = track_bit_offset_q;
                                if (track_bit_count_q != 17'd0 && track_bit_offset_q >= track_bit_count_q)
                                    bit_offset_next = 17'd0;
                                track_bit_offset_q <= bit_offset_next;
                                prefetch_current_line_q <= 21'd0;
                                prefetch_next_line_q <= 21'd0;
                                woz_head_window_q <= 4'd0;
                                woz_bit_accum_q <= 16'd0;
                            end else begin
                                woz_alias_lo_q <= 8'hFF;
                                woz_alias_hi_q <= 8'h00;
                                woz_alias_drive_q[0] <= 1'b0;
                                woz_alias_drive_q[1] <= 1'b0;
                                track_bit_offset_q <= 17'd0;
                                track_stream_pos_q <= 13'd0;
                                standard_spin_countdown_q <= STANDARD_SPIN_FIRST_IDLE;
                                standard_spin_repeat_q <= 1'b0;
                                standard_read_gap_q <= STANDARD_READ_GAP_LIMIT;
                                prefetch_current_line_q <= 21'd0;
                                prefetch_next_line_q <= 21'd0;
                                disk_latch_q <= 8'hFF;
                                woz_shift_q <= 8'h00;
                                woz_latch_delay_q <= 4'd0;
                                woz_head_window_q <= 4'd0;
                                woz_write_started_q <= 1'b0;
                                woz_bit_accum_q <= 16'd0;
                            end
                            prefetch_valid_q <= '0;
                            prefetch_req_q <= 1'b0;
                            prefetch_resp_pending_q <= 1'b0;
                            cache_patch_pending_q <= 1'b0;
                            write_fifo_head_q <= 4'd0;
                            write_fifo_tail_q <= 4'd0;
                            write_fifo_count_q <= 5'd0;
                            disk_write_pending_q <= 1'b0;
                            woz_write_pending_q <= 1'b0;
                            write_req_q <= 1'b0;
                            woz_seam_arm_q <= 1'b0;
                            woz_cached_valid_q <= 1'b0;
                            woz_cached_ready_q <= 1'b0;
                        end
                    end
                    D2_REG_TRACK_LENGTH: begin
                        automatic logic [31:0] length_tmp = globals::apply_wstrb(
                            {18'h00000, track_length_q}, as_common.wdata, as_common.wstrb);
                        automatic logic [13:0] length_next = length_tmp[13:0];
                        if (length_next > 14'(TRACK_STREAM_BYTES))
                            length_next = 14'(TRACK_STREAM_BYTES);
                        if (length_next == 14'd0)
                            track_length_q <= 14'd0;
                        else
                            track_length_q <= length_next;
                        prefetch_next_line_q <= 21'd0;
                        prefetch_valid_q <= '0;
                        prefetch_req_q <= 1'b0;
                        prefetch_resp_pending_q <= 1'b0;
                        cache_patch_pending_q <= 1'b0;
                        woz_cached_valid_q <= 1'b0;
                        woz_cached_ready_q <= 1'b0;
                        woz_seam_arm_q <= 1'b0;
                        write_fifo_head_q <= 4'd0;
                        write_fifo_tail_q <= 4'd0;
                        write_fifo_count_q <= 5'd0;
                        disk_write_pending_q <= 1'b0;
                        woz_write_pending_q <= 1'b0;
                        write_req_q <= 1'b0;
                    end
                    D2_REG_TRACK_BIT_COUNT: begin
                        automatic logic [31:0] bit_count_tmp = globals::apply_wstrb(
                            {15'h0000, track_bit_count_q}, as_common.wdata, as_common.wstrb);
                        automatic logic [16:0] bit_count_next;
                        if (bit_count_tmp > 32'd65536)
                            bit_count_next = 17'd65536;
                        else
                            bit_count_next = bit_count_tmp[16:0];
                        track_bit_count_q <= bit_count_next;
                        woz_seam_recalc_q <= 1'b1;
                        woz_seam_arm_q <= 1'b0;
                        woz_cached_valid_q <= 1'b0;
                        woz_cached_ready_q <= 1'b0;
                    end
                    D2_REG_TRACK_BIT_OFFSET: begin
                        automatic logic [31:0] bit_offset_tmp = globals::apply_wstrb(
                            {15'h0000, track_bit_offset_q}, as_common.wdata, as_common.wstrb);
                        automatic logic [16:0] bit_offset_next;
                        if (bit_offset_tmp > 32'd65535)
                            bit_offset_next = 17'd65535;
                        else
                            bit_offset_next = bit_offset_tmp[16:0];
                        track_bit_offset_q <= bit_offset_next;
                        woz_seam_arm_q <= 1'b0;
                        woz_cached_valid_q <= 1'b0;
                        woz_cached_ready_q <= 1'b0;
                    end
                    D2_REG_TRACK_BIT_TIMING: begin
                        if (as_common.wstrb[0]) begin
                            track_bit_timing_q <= as_common.wdata[7:0];
                            woz_bit_accum_q <= 16'd0;
                            woz_seam_arm_q <= 1'b0;
                            woz_cached_valid_q <= 1'b0;
                            woz_cached_ready_q <= 1'b0;
                        end
                    end
                    D2_REG_TRACK_SEAM: begin
                        automatic logic [31:0] seam_tmp = globals::apply_wstrb(
                            {woz_seam_run_q, woz_seam_start_q},
                            as_common.wdata,
                            as_common.wstrb);
                        automatic logic [15:0] seam_start_next = seam_tmp[15:0];
                        woz_seam_start_q <= seam_start_next;
                        woz_seam_run_q <= seam_tmp[31:16];
                        woz_seam_recalc_q <= 1'b1;
                        woz_seam_arm_q <= 1'b0;
                    end
                    D2_REG_TRACK_INDEX: begin
                        automatic logic [31:0] index_tmp = globals::apply_wstrb(
                            {19'h00000, track_write_index_q}, as_common.wdata, as_common.wstrb);
                        track_write_index_q <= index_tmp[12:0] & TRACK_STREAM_LAST;
                    end
                    D2_REG_TRACK_DATA: begin
                        track_write_index_q <= (track_write_index_q + 13'd4) & TRACK_STREAM_LAST;
                    end
                    D2_REG_STREAM_POS: begin
                        automatic logic [31:0] pos_tmp = globals::apply_wstrb(
                            {19'h00000, track_stream_pos_q}, as_common.wdata, as_common.wstrb);
                        automatic logic [12:0] pos_next = pos_tmp[12:0] & TRACK_STREAM_LAST;
                        track_stream_pos_q <= pos_next;
                        standard_spin_countdown_q <= STANDARD_SPIN_FIRST_IDLE;
                        standard_spin_repeat_q <= 1'b0;
                        prefetch_current_line_q <= 21'd0;
                        prefetch_next_line_q <= 21'd0;
                        prefetch_valid_q <= '0;
                        prefetch_req_q <= 1'b0;
                        prefetch_resp_pending_q <= 1'b0;
                        cache_patch_pending_q <= 1'b0;
                        woz_cached_valid_q <= 1'b0;
                        woz_cached_ready_q <= 1'b0;
                        woz_seam_arm_q <= 1'b0;
                        write_fifo_head_q <= 4'd0;
                        write_fifo_tail_q <= 4'd0;
                        write_fifo_count_q <= 5'd0;
                        disk_write_pending_q <= 1'b0;
                        woz_write_pending_q <= 1'b0;
                        write_req_q <= 1'b0;
                    end
                    D2_REG_STREAM_READS: begin
                        stream_read_count_q <= globals::apply_wstrb(
                            stream_read_count_q, as_common.wdata, as_common.wstrb);
                    end
                    D2_REG_WRITE_INFO: begin
                        if (as_common.wstrb[0] && as_common.wdata[0] &&
                            !write_busy &&
                            as_common.wdata[16] == write_dirty_drive_q &&
                            as_common.wdata[15:8] == write_dirty_qtrack_q) begin
                            write_dirty_q <= 1'b0;
                        end
                    end
                    D2_REG_WRITE_COUNT: begin
                        stream_write_count_q <= globals::apply_wstrb(
                            stream_write_count_q, as_common.wdata, as_common.wstrb);
                    end
                    D2_REG_D1_INFO: begin
                        drive_info_q[0] <= globals::apply_wstrb(
                            drive_info_q[0], as_common.wdata, as_common.wstrb);
                    end
                    D2_REG_D1_SIZE: begin
                        drive_size_q[0] <= globals::apply_wstrb(
                            drive_size_q[0], as_common.wdata, as_common.wstrb);
                    end
                    D2_REG_D1_TRACKS: begin
                        drive_tracks_q[0] <= globals::apply_wstrb(
                            drive_tracks_q[0], as_common.wdata, as_common.wstrb);
                    end
                    D2_REG_D1_BASE: begin
                        drive_base_q[0] <= globals::apply_wstrb(
                            drive_base_q[0], as_common.wdata, as_common.wstrb);
                    end
                    D2_REG_D1_LENGTH: begin
                        drive_length_q[0] <= globals::apply_wstrb(
                            drive_length_q[0], as_common.wdata, as_common.wstrb);
                    end
                    D2_REG_D2_INFO: begin
                        drive_info_q[1] <= globals::apply_wstrb(
                            drive_info_q[1], as_common.wdata, as_common.wstrb);
                    end
                    D2_REG_D2_SIZE: begin
                        drive_size_q[1] <= globals::apply_wstrb(
                            drive_size_q[1], as_common.wdata, as_common.wstrb);
                    end
                    D2_REG_D2_TRACKS: begin
                        drive_tracks_q[1] <= globals::apply_wstrb(
                            drive_tracks_q[1], as_common.wdata, as_common.wstrb);
                    end
                    D2_REG_D2_BASE: begin
                        drive_base_q[1] <= globals::apply_wstrb(
                            drive_base_q[1], as_common.wdata, as_common.wstrb);
                    end
                    D2_REG_D2_LENGTH: begin
                        drive_length_q[1] <= globals::apply_wstrb(
                            drive_length_q[1], as_common.wdata, as_common.wstrb);
                    end
                    D2_REG_WOZ_ALIAS_RANGE: begin
                        automatic logic [31:0] range_tmp = globals::apply_wstrb(
                            {16'h0000, woz_alias_hi_q, woz_alias_lo_q},
                            as_common.wdata, as_common.wstrb);
                        woz_alias_lo_q <= range_tmp[7:0];
                        woz_alias_hi_q <= range_tmp[15:8];
                    end
                    default: begin
                    end
                endcase
            end

            case (as_common.araddr)
                D2_REG_STATUS: begin
                    as_client_rdata_q <= {
                        8'hD2,
                        8'h00,
                        4'b0000,
                        drive_spinning,
                        q7_q,
                        q6_q,
                        phase_on_q,
                        drive_select_q,
                        motor_on_q,
                        enabled,
                        drive_info_q[1][0],
                        drive_info_q[0][0]
                    };
                end
                D2_REG_CONTROL:    as_client_rdata_q <= control_q;
                D2_REG_PSRAM_BASE: as_client_rdata_q <= psram_base_q;
                D2_REG_UNDERRUNS:  as_client_rdata_q <= underrun_count_q;
                D2_REG_LAST_IO:    as_client_rdata_q <= {24'h000000, last_io_q};
                D2_REG_IO_COUNT:   as_client_rdata_q <= io_access_count_q;
                D2_REG_TRACK_INFO: begin
                    as_client_rdata_q <= track_info_word();
                end
                D2_REG_TRACK_LENGTH: as_client_rdata_q <= {18'h00000, track_length_q};
                D2_REG_TRACK_INDEX:  as_client_rdata_q <= {19'h00000, track_write_index_q};
                D2_REG_TRACK_DATA:   as_client_rdata_q <= 32'h0000_0000;
                D2_REG_STREAM_POS:   as_client_rdata_q <= {19'h00000, active_stream_pos};
                D2_REG_STREAM_READS: as_client_rdata_q <= stream_read_count_q;
                D2_REG_TRACK_BIT_COUNT: as_client_rdata_q <= {15'h0000, track_bit_count_q};
                D2_REG_TRACK_BIT_OFFSET: as_client_rdata_q <= {15'h0000, track_bit_offset_q};
                D2_REG_TRACK_BIT_TIMING: as_client_rdata_q <= {24'h000000, track_bit_timing_q};
                D2_REG_TRACK_SEAM: as_client_rdata_q <= {woz_seam_run_q, woz_seam_start_q};
                D2_REG_WRITE_INFO: begin
                    as_client_rdata_q <= {
                        disk_latch_q,
                        7'h00,
                        write_dirty_drive_q,
                        write_dirty_qtrack_q,
                        6'h00,
                        write_busy,
                        write_dirty_q
                    };
                end
                D2_REG_WRITE_COUNT:  as_client_rdata_q <= stream_write_count_q;
                D2_REG_D1_INFO:    as_client_rdata_q <= drive_info_q[0];
                D2_REG_D1_SIZE:    as_client_rdata_q <= drive_size_q[0];
                D2_REG_D1_TRACKS:  as_client_rdata_q <= drive_tracks_q[0];
                D2_REG_D1_BASE:    as_client_rdata_q <= drive_base_q[0];
                D2_REG_D1_LENGTH:  as_client_rdata_q <= drive_length_q[0];
                D2_REG_D2_INFO:    as_client_rdata_q <= drive_info_q[1];
                D2_REG_D2_SIZE:    as_client_rdata_q <= drive_size_q[1];
                D2_REG_D2_TRACKS:  as_client_rdata_q <= drive_tracks_q[1];
                D2_REG_D2_BASE:    as_client_rdata_q <= drive_base_q[1];
                D2_REG_D2_LENGTH:  as_client_rdata_q <= drive_length_q[1];
                D2_REG_WOZ_ALIAS_RANGE: as_client_rdata_q <= {16'h0000, woz_alias_hi_q, woz_alias_lo_q};
                default:           as_client_rdata_q <= 32'h0000_0000;
            endcase
        end
    end

    always_comb begin
        ab_write_d = ab_write_q;
        ab_write_d.wr_addr = 16'h0000;
        ab_write_d.wr_rw = 1'b1;
        ab_write_d.wr_addr_rw_en = 1'b0;
        ab_write_d.assert_inh = 1'b0;
        ab_write_d.assert_res = 1'b0;
        ab_write_d.assert_irq = 1'b0;
        ab_write_d.assert_rdy = 1'b0;
        ab_write_d.assert_nmi = 1'b0;
        ab_write_d.assert_dma = 1'b0;

        if (ab_read.sss_en) begin
            if (ab_rom_read) begin
                ab_write_d.wr_data = slot_rom_byte(ab_read.addr[7:0]);
                ab_write_d.wr_data_en = 1'b1;
            end else if (ab_io_read && !ab_read.addr[0]) begin
                ab_write_d.wr_data = disk_read_byte;
                ab_write_d.wr_data_en = 1'b1;
            end else begin
                ab_write_d.wr_data = 8'h00;
                ab_write_d.wr_data_en = 1'b0;
            end
        end else if (ab_read.data_en) begin
            ab_write_d.wr_data = 8'h00;
            ab_write_d.wr_data_en = 1'b0;
        end
    end

    assign as_client.rdata = as_client_rdata_q;

endmodule
