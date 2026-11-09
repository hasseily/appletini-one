`timescale 1ns / 1ps

// Apple-side PC Transporter bridge. The RAM port requires a bounded response.
// There is one request in flight. Each accepted request gets one response,
// including writes. The backend and bridge must share the fabric reset.
module pc_transporter_card #(
    parameter int READ_WAIT_CYCLES = 12,
    parameter int VIRTUAL_READ_WAIT_CYCLES = 561
) (
    input  logic                     clk,
    input  logic                     rstn,
    input  globals::AppleBus_read    ab_read,
    input  globals::SoftSwitchState  sss,
    input  logic                     card_enabled,
    input  logic                     virtual_bus,
    input  logic [2:0]               slot_assign,
    input  globals::AxiSimple_common as_common,
    AxiSimple_if.client              as_client,
    output logic                     mem_req_valid,
    input  logic                     mem_req_ready,
    output logic                     mem_req_write,
    output logic [19:0]              mem_req_addr,
    output logic [63:0]              mem_req_wdata,
    output logic [7:0]               mem_req_wstrb,
    input  logic                     mem_rsp_valid,
    input  logic [63:0]              mem_rsp_rdata,
    input  logic                     mem_rsp_error,
    input  logic                     virtual_capture_wait,
    output logic                     virtual_data_hold,
    output globals::AppleBus_write   ab_write
);

    localparam logic [7:0] REG_ID        = 8'h00;
    localparam logic [7:0] REG_STATUS    = 8'h01;
    localparam logic [7:0] REG_XFER      = 8'h02;
    localparam logic [7:0] REG_IO_INDEX  = 8'h03;
    localparam logic [7:0] REG_IO_DATA   = 8'h04;
    localparam logic [7:0] REG_MISSES    = 8'h05;
    localparam logic [7:0] REG_DROPS     = 8'h06;
    localparam logic [7:0] REG_ERRORS    = 8'h07;
    localparam logic [7:0] REG_DIAG      = 8'h08;
    localparam logic [7:0] REG_KEYS      = 8'h09;
    localparam logic [7:0] REG_ACK_DATA  = 8'h0A;
    localparam logic [7:0] REG_CGA_STATE = 8'h0B;
    localparam logic [7:0] REG_CRTC_0A   = 8'h0C;
    localparam logic [7:0] REG_CRTC_0E   = 8'h0D;
    localparam logic [7:0] REG_HOLDS     = 8'h0E;
    localparam logic [7:0] REG_HOLD_MAX  = 8'h0F;
    localparam logic [7:0] REG_CGA_GEOMETRY = 8'h10;
    localparam logic [7:0] REG_CGA_RASTER = 8'h11;

    localparam logic [24:0] RAM_BYTES = 25'h0C0000;
    localparam logic [14:0] PORT_RAM_TAG = 15'h02FC; // $BF000 through $BF3FF
    localparam int MAX_READ_WAIT = (READ_WAIT_CYCLES > VIRTUAL_READ_WAIT_CYCLES) ?
                                   READ_WAIT_CYCLES : VIRTUAL_READ_WAIT_CYCLES;
    localparam int WAIT_WIDTH = $clog2(MAX_READ_WAIT + 1);
    localparam int WRITE_DEPTH = 4;
    localparam int WRITE_PTR_WIDTH = $clog2(WRITE_DEPTH);
    localparam int CGA_PHASE_BITS = 24;
    localparam longint CGA_FABRIC_HZ = 133000000;
    localparam longint CGA_DOT_HZ = 14318180;
    localparam logic [CGA_PHASE_BITS:0] CGA_DOT_STEP =
        (CGA_PHASE_BITS+1)'(((CGA_DOT_HZ << CGA_PHASE_BITS) + CGA_FABRIC_HZ/2) /
                            CGA_FABRIC_HZ);
    localparam logic [4:0] CGA_VSYNC_LINES = 5'd16;

    logic [23:0] pointer_q;
    logic [15:0] page_cc_q;
    logic [15:0] page_ce_q;
    logic [15:0] apple_addr_q;
    logic [24:0] ram_addr;
    logic [24:0] early_ram_addr;
    logic [16:0] page_cc_next_q;
    logic [3:0]  window_in_range_q;
    logic [3:0]  window_port_tag_q;
    logic        pointer_mirror_q;
    logic [19:0] apple_ram_addr_q;
    logic        apple_ram_valid_q;
    logic        apple_ctrl_q;
    logic        apple_cga_q;
    logic        apple_pit_q;
    logic        apple_port_ram_q;
    logic        apple_mirror_q;
    logic        apple_cycle_q;
    logic        apple_res_q;
    logic        guest_reset_q;
    logic        guest_halt_q;
    logic        guest_int_q;
    logic [7:0]  guest_int_seq_q;
    logic        timer0_pending_q;
    wire         timer0_tick;
    logic        irq_enable_q;
    logic        pit_gate2_q;
    logic        pit_out2;
    logic [7:0]  pit_read_data;
    logic        xfer_pending_q;
    logic [19:0] xfer_q;
    logic [7:0]  ack_seq_q;
    logic [7:0]  reset_seq_q;
    logic [15:0] ack_data_q;
    logic [15:0] xfer_data_q;
    logic [15:0] xfer_data_next;
    logic [1:0]  xfer_written_q;
    logic [1:0]  seed_need_q;
    logic        seed_capture_q;
    logic        seed_capture_high_q;
    logic [7:0]  key_modifiers_q;
    logic [7:0]  key_scan_q;
    logic        key_ready_q;
    logic [10:0] io_index_q;
    (* ram_style = "block" *) logic [7:0] port_bytes [0:1023];
    logic [9:0]  apple_port_index_q;
    logic [7:0]  apple_port_data_q;
    logic [7:0]  apple_port_reply_q;
    logic [7:0]  ps_port_data_q;
    logic        port_response_q;
    logic [1:0]  port_data_ready_q;
    logic        c8_owned_q;
    logic        virtual_bus_q;
    logic [7:0]  reply_data_q;
    logic        reply_valid_q;
    logic [7:0]  local_read_data;
    logic [7:0]  araddr_q;
    logic [4:0]  crtc_index_q;
    logic [7:0]  crtc_regs_q [0:17];
    logic [7:0]  cga_mode_q;
    logic [7:0]  cga_color_q;
    logic [15:0] cga_seq_q;
    logic [CGA_PHASE_BITS-1:0] cga_dot_phase_q;
    logic        cga_dot_tick_q;
    logic [3:0]  cga_dot_q;
    logic [7:0]  cga_column_q;
    logic [6:0]  cga_row_q;
    logic [4:0]  cga_raster_q;
    logic        cga_adjust_q;
    logic        cga_line_start_q;
    logic        cga_hblank_q;
    logic [4:0]  cga_vsync_lines_q;
    logic [15:0] cga_frame_q;
    wire cga_char_tick = cga_dot_tick_q && (&cga_dot_q[2:0]) &&
                         (cga_mode_q[0] || cga_dot_q[3]);
    wire cga_vsync = cga_vsync_lines_q != 5'd0;
    wire cga_timing_programmed = crtc_regs_q[0] != 8'd0;

    logic [19:0] write_addr_q [WRITE_DEPTH];
    logic [7:0]  write_data_q [WRITE_DEPTH];
    logic [WRITE_PTR_WIDTH-1:0] write_head_q;
    logic [WRITE_PTR_WIDTH-1:0] write_tail_q;
    logic [WRITE_PTR_WIDTH:0]   write_count_q;
    logic [WRITE_DEPTH-1:0]   slot_page_match_q [0:2];
    logic [WRITE_DEPTH-1:0]   slot_pointer_match_q;
    logic [WRITE_DEPTH-1:0]   queue_slot_hit;
    logic [1:0]               serve_window;
    logic                     forwarding_hit;
    logic [7:0]               forwarding_data;

    logic        mem_inflight_q;
    logic        mem_read_live_q;
    logic        mem_prefetch_q;
    logic [2:0]  mem_read_lane_q;
    logic        early_valid_q;
    logic        virtual_prefetch_pending_q;
    logic [15:0] early_apple_addr_q;
    logic        early_result_q;
    logic        early_error_q;
    logic [7:0]  early_data_q;
    logic        read_wait_q;
    logic        read_sent_q;
    logic [WAIT_WIDTH-1:0] read_cycles_q;
    logic [31:0] read_misses_q;
    logic [31:0] write_drops_q;
    logic [31:0] mem_errors_q;
    logic [31:0] virtual_holds_q;
    logic [WAIT_WIDTH-1:0] virtual_hold_cycles_q;
    logic [WAIT_WIDTH-1:0] virtual_max_hold_q;

    // The physical bus cannot wait. ONE//e captures only after the pending
    // read has registered its reply or reached the bounded timeout.
    assign virtual_data_hold = virtual_bus_q && read_wait_q;

    initial begin
        for (int i = 0; i < 1024; i++) begin
            port_bytes[i] = 8'h00;
        end
    end

    wire enabled = card_enabled && (slot_assign != 3'd0);
    wire active = enabled && ab_read.res;
    // A reply belongs to the bus that requested it. The two buses can be
    // in different phases when ONE//e enters or leaves service.
    wire bus_cycle_active = active && (virtual_bus == virtual_bus_q);
    wire c8_selected = bus_cycle_active && !sss.sw_intcxrom &&
                       !sss.c8_internal_rom &&
                       (sss.c8_select == slot_assign) &&
                       sss.io_select[slot_assign];
    wire slot_hit = sss.slot_access && (ab_read.addr[10:8] == slot_assign);
    wire io_hit = (ab_read.addr[15:8] == 8'hC0) &&
                  (ab_read.addr[7:4] == (4'h8 + {1'b0, slot_assign}));
    wire c8_hit = c8_selected && (ab_read.addr[15:11] == 5'h19) &&
                  (ab_read.addr != 16'hCFFF);
    wire port_hit = c8_hit && (ab_read.addr[10] == 1'b0);
    wire cc_hit = c8_hit && (ab_read.addr[10:9] == 2'b10);
    wire ce_hit = c8_hit && (ab_read.addr[10:8] == 3'b110);
    wire pointer_data_hit = c8_hit && (ab_read.addr[15:8] == 8'hCF) &&
                            ((ab_read.addr[7:0] == 8'h03) ||
                             (ab_read.addr[7:0] == 8'h04));
    wire ram_hit = cc_hit || ce_hit || pointer_data_hit;
    wire ctrl_hit = c8_hit && (ab_read.addr[15:6] == 10'h33C);
    wire apple_serve = bus_cycle_active && ab_read.cycle_valid && ab_read.serve_en;
    wire apple_commit = active && c8_selected && ab_read.cycle_valid &&
                        ab_read.data_en && apple_cycle_q;
    wire apple_write = apple_commit && !ab_read.rw;
    assign serve_window = window_index(ab_read.addr);
    wire ram_in_range = window_in_range_q[serve_window];
    wire port_ram_address = is_ram_window(ab_read.addr) &&
                            window_port_tag_q[serve_window];
    wire port_ram_read = c8_hit && port_ram_address;
    wire port_ram_write = apple_port_ram_q;
    wire mirror_write = apple_mirror_q;
    wire [19:0] mirror_addr = {6'h2E, apple_ram_addr_q[13:0]};
    wire [19:0] write_target = port_ram_write ? mirror_addr : apple_ram_addr_q;
    wire mem_accept = mem_req_valid && mem_req_ready;
    wire mem_complete = mem_rsp_valid && (mem_inflight_q || mem_accept);
    wire retire_write = mem_complete && mem_req_write;
    wire [WRITE_PTR_WIDTH:0] drained_count = write_count_q - (WRITE_PTR_WIDTH+1)'(retire_write);
    wire two_ddr_writes = mirror_write && !port_ram_write;
    wire [WRITE_PTR_WIDTH:0] append_count = two_ddr_writes ? 3'd2 : 3'd1;
    wire ram_write = apple_write && apple_ram_valid_q && (!port_ram_write || mirror_write);
    wire queue_write = ram_write && ((drained_count + append_count) <= 3'(WRITE_DEPTH));
    wire writes_drained = (write_count_q == '0) && !ram_write;

    wire apple_ctrl_write = apple_write && apple_ctrl_q;
    wire cga_address = (ab_read.addr[15:2] == 14'h33CB);
    wire cga_read = c8_hit && cga_address;
    wire cga_write = apple_write && apple_cga_q;
    wire pit_read = apple_serve && ab_read.rw && ctrl_hit &&
                    (ab_read.addr[5:2] == 4'hA);
    wire pit_write = apple_write && apple_pit_q;
    // A pointer write to port RAM also updates its CGA VRAM mirror. Do not
    // change the port byte unless the queue accepts that mirror write.
    wire apple_port_write = apple_write && (!apple_addr_q[10] || cga_write ||
                            (port_ram_write && (!mirror_write || queue_write)));
    wire ps_io_write = as_client.awvalid && (as_common.awaddr == REG_IO_DATA) &&
                       as_common.wstrb[0] && active;
    wire ps_cga_alias = (io_index_q[10:2] == 9'h10B);
    wire ps_port_selected = !io_index_q[10] || ps_cga_alias;
    wire ps_ctrl_write = ps_io_write && !ps_cga_alias && (io_index_q[10:6] == 5'h10);
    wire ps_diag_write = as_client.awvalid && (as_common.awaddr == REG_DIAG) &&
                         as_common.wstrb[0];
    wire reset_write = apple_ctrl_write && (apple_addr_q[5:0] == 6'h31) &&
                       ab_read.data[4];
    wire post_xfer = as_client.awvalid && (as_common.awaddr == REG_XFER) &&
                     (as_common.wstrb == 4'hF) && as_common.wdata[31] &&
                     (as_common.wdata[27:20] == reset_seq_q) && active &&
                     !xfer_pending_q && !reset_write;
    wire ack_xfer = apple_ctrl_write && (apple_addr_q[5:0] == 6'h32) &&
                    ab_read.data[7] && xfer_pending_q;
    wire cancel_xfer = ps_diag_write && as_common.wdata[1];
    wire accept_guest_int = ps_diag_write && as_common.wstrb == 4'hf &&
        as_common.wdata[2] && as_common.wdata[27:20] == reset_seq_q &&
        as_common.wdata[15:8] == guest_int_seq_q;
    wire apple_irq_pending = xfer_pending_q || key_ready_q || timer0_pending_q;

    function automatic logic [9:0] port_index(input logic [9:0] port_addr);
        if (port_addr[9:3] == 7'h7A) begin
            return {9'h1EB, port_addr[0]};
        end
        return port_addr;
    endfunction

    // CF2C through CF2F and the matching CGA I/O ports share four bytes.
    // This is backing storage for the bridge. It has no CGA device logic.
    wire [9:0] apple_port_read_index = cga_address ?
        (10'h3D6 + {8'd0, ab_read.addr[1:0]}) :
        port_index(port_ram_address ? ram_addr[9:0] : ab_read.addr[9:0]);
    wire [9:0] ps_port_index = ps_cga_alias ?
        (10'h3D6 + {8'd0, io_index_q[1:0]}) : port_index(io_index_q[9:0]);
    wire ps_port_write_intent = ps_io_write && ps_port_selected;
    wire ps_port_write = ps_port_write_intent &&
                         !(apple_port_write && (ps_port_index == apple_port_index_q));
    wire [9:0] xfer_port_low = port_index(xfer_q[9:0]);
    wire [9:0] xfer_port_high = port_index(xfer_q[9:0] + 10'd1);
    // A PS write attempt reserves its port even when an Apple write wins.
    wire seed_issue = active && xfer_pending_q && !ps_port_write_intent &&
                      (as_common.araddr != REG_IO_DATA) && (seed_need_q != '0);
    wire seed_issue_high = !seed_need_q[0];
    wire [9:0] ps_port_address = seed_issue ?
        (seed_issue_high ? xfer_port_high : xfer_port_low) : ps_port_index;
    wire apple_xfer_low = apple_port_write && (apple_port_index_q == xfer_port_low);
    wire apple_xfer_high = apple_port_write && xfer_q[17] &&
                           (apple_port_index_q == xfer_port_high);
    wire ps_xfer_low = ps_port_write && (ps_port_index == xfer_port_low);
    wire ps_xfer_high = ps_port_write && xfer_q[17] && (ps_port_index == xfer_port_high);
    wire apple_cga_write = apple_port_write && (apple_port_index_q >= 10'h3D6) &&
                           (apple_port_index_q <= 10'h3D9);
    wire ps_cga_write = ps_port_write && (ps_port_index >= 10'h3D6) &&
                        (ps_port_index <= 10'h3D9);

    pc_transporter_pit pit_i (
        .clk(clk),
        .rstn(rstn && active),
        .read_strobe(pit_read),
        .write_strobe(pit_write),
        .read_addr(ab_read.addr[1:0]),
        .write_addr(apple_addr_q[1:0]),
        .wdata(ab_read.data),
        .rdata(pit_read_data),
        .gate2(pit_gate2_q),
        .out2(pit_out2),
        .irq0_tick(timer0_tick)
    );

    function automatic logic is_ram_window(input logic [15:0] addr);
        return (addr[15:9] == 7'h66) || (addr[15:8] == 8'hCE) ||
               (addr == 16'hCF03) || (addr == 16'hCF04);
    endfunction

    // Window 0 and 1 are the low and high CC pages, 2 is CE and 3 is the
    // pointer.
    function automatic logic [1:0] window_index(input logic [15:0] addr);
        if (addr[15:9] == 7'h66) begin
            return {1'b0, addr[8]};
        end
        if (addr[15:8] == 8'hCE) begin
            return 2'd2;
        end
        return 2'd3;
    endfunction

    // CC00 through CDFF spans two pages. The high page uses the stored sum.
    function automatic logic [24:0] window_address(
        input logic [15:0] addr,
        input logic [23:0] pointer,
        input logic [15:0] page_cc,
        input logic [16:0] page_cc_next,
        input logic [15:0] page_ce
    );
        if (addr[15:9] == 7'h66) begin
            return {addr[8] ? page_cc_next : {1'b0, page_cc}, addr[7:0]};
        end
        if (addr[15:8] == 8'hCE) begin
            return {1'b0, page_ce, addr[7:0]};
        end
        return {1'b0, pointer};
    endfunction

    assign ram_addr = window_address(ab_read.addr, pointer_q, page_cc_q,
                                     page_cc_next_q, page_ce_q);
    assign early_ram_addr = window_address(ab_read.addr_early, pointer_q, page_cc_q,
                                           page_cc_next_q, page_ce_q);

    // These terms depend only on the pointer, the page registers and the
    // write queue. Those registers change only at an Apple data commit, while
    // the slot is inactive, or in reset. The next address that uses a term
    // arrives at least one bus phase later, so one clock of delay has no
    // effect. This keeps adders and wide compares off the serve path.
    wire [16:0] page_cc_plus = {1'b0, page_cc_q} + 17'd1;
    always_ff @(posedge clk) begin
        page_cc_next_q    <= page_cc_plus;
        window_in_range_q <= {{1'b0, pointer_q} < RAM_BYTES,
                              {1'b0, page_ce_q} < RAM_BYTES[24:8],
                              page_cc_plus < RAM_BYTES[24:8],
                              {1'b0, page_cc_q} < RAM_BYTES[24:8]};
        window_port_tag_q <= {{1'b0, pointer_q[23:10]} == PORT_RAM_TAG,
                              {1'b0, page_ce_q[15:2]} == PORT_RAM_TAG,
                              page_cc_plus[16:2] == PORT_RAM_TAG,
                              {1'b0, page_cc_q[15:2]} == PORT_RAM_TAG};
        pointer_mirror_q  <= (pointer_q[19:16] == 4'hB) && (pointer_q[15:14] != 2'b10);
        for (int slot = 0; slot < WRITE_DEPTH; slot++) begin
            slot_page_match_q[0][slot] <= write_addr_q[slot][19:8] == page_cc_q[11:0];
            slot_page_match_q[1][slot] <= write_addr_q[slot][19:8] == page_cc_plus[11:0];
            slot_page_match_q[2][slot] <= write_addr_q[slot][19:8] == page_ce_q[11:0];
            slot_pointer_match_q[slot] <= write_addr_q[slot] == pointer_q[19:0];
        end
    end

    // The generic control bytes use distributed RAM with one write port. A
    // PS write takes the port first. An Apple write waits in a holding
    // register for the next free clock, so it still wins a same-clock write
    // to its byte. A later PS write to the held byte replaces it. A valid bit
    // per byte gives the reset value of zero.
    function automatic logic control_byte_stored(input logic [5:0] index);
        case (index)
            6'h00, 6'h01, 6'h02, 6'h03, 6'h04, 6'h05, 6'h06, 6'h0E, 6'h0F,
            6'h28, 6'h29, 6'h2A, 6'h2B, 6'h2C, 6'h2D, 6'h2E, 6'h2F,
            6'h30, 6'h31, 6'h32: return 1'b0;
            default: return 1'b1;
        endcase
    endfunction

    (* ram_style = "distributed" *) logic [7:0] control_ram [0:63];
    logic [63:0] control_valid_q;
    logic        ctrl_hold_q;
    logic [5:0]  ctrl_hold_index_q;
    logic [7:0]  ctrl_hold_data_q;
    wire ctrl_apple_store = apple_ctrl_write && control_byte_stored(apple_addr_q[5:0]);
    wire ctrl_write = rstn && (ps_ctrl_write || ctrl_hold_q);
    wire [5:0] ctrl_write_index = ps_ctrl_write ? io_index_q[5:0] : ctrl_hold_index_q;
    wire [7:0] ctrl_write_data = ps_ctrl_write ? as_common.wdata[7:0] : ctrl_hold_data_q;
    wire [7:0] ctrl_apple_rdata = control_valid_q[ab_read.addr[5:0]] ?
                                  control_ram[ab_read.addr[5:0]] : 8'h00;
    wire [7:0] ctrl_ps_rdata =
        (ctrl_hold_q && (ctrl_hold_index_q == io_index_q[5:0])) ? ctrl_hold_data_q :
        control_valid_q[io_index_q[5:0]] ? control_ram[io_index_q[5:0]] : 8'h00;

    initial begin
        for (int i = 0; i < 64; i++) begin
            control_ram[i] = 8'h00;
        end
    end

    always @(posedge clk) begin
        if (ctrl_write) begin
            control_ram[ctrl_write_index] <= ctrl_write_data;
        end
    end

    always_ff @(posedge clk) begin
        if (!rstn) begin
            control_valid_q   <= '0;
            ctrl_hold_q       <= 1'b0;
            ctrl_hold_index_q <= '0;
            ctrl_hold_data_q  <= '0;
        end else begin
            if (ctrl_write) begin
                control_valid_q[ctrl_write_index] <= 1'b1;
            end
            if (ctrl_apple_store) begin
                ctrl_hold_q       <= 1'b1;
                ctrl_hold_index_q <= apple_addr_q[5:0];
                ctrl_hold_data_q  <= ab_read.data;
            end else if (!ps_ctrl_write || (io_index_q[5:0] == ctrl_hold_index_q)) begin
                ctrl_hold_q <= 1'b0;
            end
        end
    end

    always_comb begin
        local_read_data = 8'hFF;
        if (slot_hit) begin
            // Identification bytes read by the Apple slot scanner. Other
            // offsets, including the boot ROM entry points, return $FF.
            case (ab_read.addr[7:0])
                8'h05: local_read_data = 8'h03;
                8'h07: local_read_data = 8'h00;
                8'hF5: local_read_data = 8'hAE;
                8'hF6: local_read_data = 8'h50;
                8'hF7: local_read_data = 8'h43;
                8'hF8: local_read_data = 8'h54;
                default: ;
            endcase
        end else if (ctrl_hit) begin
            case (ab_read.addr[5:0])
                6'h00: local_read_data = pointer_q[7:0];
                6'h01: local_read_data = pointer_q[15:8];
                6'h02: local_read_data = pointer_q[23:16];
                6'h03, 6'h04: local_read_data = 8'hFF;
                6'h05: local_read_data = page_cc_q[15:8];
                6'h06: local_read_data = page_cc_q[7:0];
                6'h0E: local_read_data = page_ce_q[15:8];
                6'h0F: local_read_data = page_ce_q[7:0];
                6'h28, 6'h29, 6'h2A, 6'h2B: local_read_data = pit_read_data;
                6'h30: local_read_data = {apple_irq_pending, guest_int_q,
                                          ctrl_apple_rdata[5:0]};
                // PIT OUT2 serves calibration. Retrace stays live when CGA
                // output is blanked for a mode change.
                6'h31: local_read_data = {xfer_q[16], xfer_q[17], pit_out2,
                                         guest_reset_q, cga_timing_programmed && cga_vsync,
                                         ctrl_apple_rdata[2:1],
                                         cga_timing_programmed && cga_hblank_q};
                6'h32: local_read_data = {xfer_pending_q, key_ready_q,
                                          timer0_pending_q, ctrl_apple_rdata[4:0]};
                6'h33: local_read_data = key_scan_q;
                6'h34: local_read_data = xfer_q[7:0];
                6'h35: local_read_data = xfer_q[15:8];
                6'h36: local_read_data = key_modifiers_q;
                default: local_read_data = ctrl_apple_rdata;
            endcase
        end
    end

    // Two synchronous ports. Apple writes win if both ports write one byte.
    // The Apple port uses the index captured at serve for reads and writes,
    // so no live address path reaches the RAM. The RAM output register holds
    // the reply byte. Read data is ready three clocks after serve, long before
    // the bus samples a card reply.
    always_ff @(posedge clk) begin
        if (apple_serve) begin
            apple_port_index_q <= apple_port_read_index;
        end
    end

    always @(posedge clk) begin
        if (apple_port_write) begin
            port_bytes[apple_port_index_q] <= ab_read.data;
        end
        apple_port_data_q <= port_bytes[apple_port_index_q];
        apple_port_reply_q <= apple_port_data_q;
    end

    // The reply drive gate uses this registered C8 ownership. The card drives
    // only during its own C8 reads, and those accesses never change C8
    // ownership. Reset and a disabled slot still release the drive at once.
    always_ff @(posedge clk) begin
        c8_owned_q <= !sss.sw_intcxrom && !sss.c8_internal_rom &&
                      (sss.c8_select == slot_assign) && sss.io_select[slot_assign];
        port_data_ready_q <= rstn ? {port_data_ready_q[0], port_response_q} : 2'b00;
    end

    always @(posedge clk) begin
        if (ps_port_write) begin
            port_bytes[ps_port_address] <= as_common.wdata[7:0];
        end
        ps_port_data_q <= port_bytes[ps_port_address];
    end

    // The renderer reads indexed CRTC state without changing the live index.
    // Apple writes win a same-clock conflict on one CGA register.
    always_ff @(posedge clk) begin
        if (!rstn || !active) begin
            crtc_index_q <= '0;
            cga_mode_q   <= '0;
            cga_color_q  <= '0;
            cga_seq_q    <= '0;
            for (int i = 0; i < 18; i++) begin
                crtc_regs_q[i] <= '0;
            end
        end else begin
            if (ps_cga_write) begin
                case (ps_port_index)
                    10'h3D6: crtc_index_q <= as_common.wdata[4:0];
                    10'h3D7: if (crtc_index_q < 5'd18) begin
                        crtc_regs_q[crtc_index_q] <= as_common.wdata[7:0];
                    end
                    10'h3D8: cga_mode_q  <= as_common.wdata[7:0];
                    10'h3D9: cga_color_q <= as_common.wdata[7:0];
                    default: ;
                endcase
            end
            if (apple_cga_write) begin
                case (apple_port_index_q)
                    10'h3D6: crtc_index_q <= ab_read.data[4:0];
                    10'h3D7: if (crtc_index_q < 5'd18) begin
                        crtc_regs_q[crtc_index_q] <= ab_read.data;
                    end
                    10'h3D8: cga_mode_q  <= ab_read.data;
                    10'h3D9: cga_color_q <= ab_read.data;
                    default: ;
                endcase
            end
            if ((ps_cga_write && (ps_port_index != 10'h3D6)) ||
                (apple_cga_write && (apple_port_index_q != 10'h3D6))) begin
                cga_seq_q <= cga_seq_q + 1'b1;
            end
        end
    end

    // The Apple status follows the guest CRTC even if the compositor runs at
    // another rate. Bit 0 marks the whole horizontal blank interval, including
    // on lines in vertical blank, so software can count one edge per line.
    always_ff @(posedge clk) begin
        if (!rstn || !active) begin
            cga_dot_phase_q   <= '0;
            cga_dot_tick_q    <= 1'b0;
            cga_dot_q         <= '0;
            cga_column_q      <= '0;
            cga_row_q         <= '0;
            cga_raster_q      <= '0;
            cga_adjust_q      <= 1'b0;
            cga_line_start_q  <= 1'b0;
            cga_hblank_q      <= 1'b0;
            cga_vsync_lines_q <= '0;
            cga_frame_q       <= '0;
        end else begin
            {cga_dot_tick_q, cga_dot_phase_q} <= {1'b0, cga_dot_phase_q} + CGA_DOT_STEP;
            if (cga_dot_tick_q) cga_dot_q <= cga_dot_q + 1'b1;

            cga_hblank_q <= cga_column_q >= crtc_regs_q[1];
            cga_line_start_q <= 1'b0;
            if (cga_char_tick) begin
                if (cga_column_q >= crtc_regs_q[0]) begin
                    cga_column_q <= '0;
                    cga_line_start_q <= 1'b1;
                    if (cga_adjust_q) begin
                        if (cga_raster_q + 6'd1 >= {1'b0, crtc_regs_q[5][4:0]}) begin
                            cga_row_q <= '0;
                            cga_raster_q <= '0;
                            cga_adjust_q <= 1'b0;
                            cga_frame_q <= cga_frame_q + 1'b1;
                        end else begin
                            cga_raster_q <= cga_raster_q + 1'b1;
                        end
                    end else if (cga_raster_q >= crtc_regs_q[9][4:0]) begin
                        cga_raster_q <= '0;
                        if (cga_row_q >= crtc_regs_q[4][6:0]) begin
                            cga_row_q <= '0;
                            cga_adjust_q <= crtc_regs_q[5][4:0] != 5'd0;
                            if (crtc_regs_q[5][4:0] == 5'd0) begin
                                cga_frame_q <= cga_frame_q + 1'b1;
                            end
                        end else begin
                            cga_row_q <= cga_row_q + 1'b1;
                        end
                    end else begin
                        cga_raster_q <= cga_raster_q + 1'b1;
                    end
                end else begin
                    cga_column_q <= cga_column_q + 1'b1;
                end
            end

            if (cga_line_start_q) begin
                if (!cga_adjust_q && cga_raster_q == 5'd0 &&
                    cga_row_q == crtc_regs_q[7][6:0]) begin
                    cga_vsync_lines_q <= CGA_VSYNC_LINES;
                end else if (cga_vsync) begin
                    cga_vsync_lines_q <= cga_vsync_lines_q - 1'b1;
                end
            end
        end
    end

    // Keep the two reply bytes until CF32 acknowledges the transfer.
    // Apple writes take precedence over the initial read from port storage.
    always_comb begin
        xfer_data_next = xfer_data_q;
        if (seed_capture_q && !xfer_written_q[seed_capture_high_q]) begin
            xfer_data_next[8*seed_capture_high_q +: 8] = ps_port_data_q;
        end
        if (ps_xfer_low)     xfer_data_next[7:0] = as_common.wdata[7:0];
        if (ps_xfer_high)    xfer_data_next[15:8] = as_common.wdata[7:0];
        if (apple_xfer_low)  xfer_data_next[7:0] = ab_read.data;
        if (apple_xfer_high) xfer_data_next[15:8] = ab_read.data;
    end

    always_ff @(posedge clk) begin
        if (!rstn) begin
            ack_data_q          <= '0;
            xfer_data_q         <= '0;
            xfer_written_q      <= '0;
            seed_need_q         <= '0;
            seed_capture_q      <= 1'b0;
            seed_capture_high_q <= 1'b0;
        end else begin
            seed_capture_q <= seed_issue;
            if (seed_issue) begin
                seed_need_q[seed_issue_high] <= 1'b0;
                seed_capture_high_q <= seed_issue_high;
            end
            if (xfer_pending_q) begin
                xfer_data_q <= xfer_data_next;
                xfer_written_q <= xfer_written_q |
                    {apple_xfer_high || ps_xfer_high, apple_xfer_low || ps_xfer_low};
            end
            if (ack_xfer) begin
                ack_data_q <= xfer_data_next;
            end
            if (post_xfer) begin
                xfer_data_q    <= '0;
                xfer_written_q <= '0;
                seed_need_q    <= as_common.wdata[17] ? 2'b11 : 2'b01;
            end
            if (!active || reset_write || ack_xfer || cancel_xfer) begin
                seed_need_q    <= '0;
                seed_capture_q <= 1'b0;
            end
        end
    end

    wire cycle_closed = !c8_selected || ab_read.addr_en || ab_read.data_en;
    wire reply_boundary = cycle_closed || ab_read.serve_en;
    // A physical DMA master can leave an old address in the early sample.
    // The virtual bus captures its owner, address and R/W before addr_en,
    // including vTW DMA cycles. Only the later sample can drive data.
    wire early_candidate = c8_selected && ab_read.cycle_valid && ab_read.addr_en &&
                           ab_read.rw_early && (ab_read.dma || virtual_bus_q) &&
                           !ab_read.serve_en &&
                           is_ram_window(ab_read.addr_early) &&
                           window_in_range_q[window_index(ab_read.addr_early)] &&
                           !window_port_tag_q[window_index(ab_read.addr_early)];
    // ONE//e holds the early address through serve_en. If an older RAM
    // transaction blocks addr_en, keep the read eligible until serve_en.
    wire retry_virtual_prefetch = virtual_prefetch_pending_q && virtual_bus_q &&
                                  c8_selected && ab_read.cycle_valid && !reply_boundary;
    wire mem_idle = !mem_req_valid && !mem_inflight_q;
    wire prefetch_start = (early_candidate || retry_virtual_prefetch) &&
                          (write_count_q == '0) && (!virtual_bus_q || mem_idle);

    always_ff @(posedge clk) begin
        if (!rstn || !virtual_bus_q || !c8_selected || !ab_read.cycle_valid ||
            ab_read.serve_en || ab_read.data_en) begin
            virtual_prefetch_pending_q <= 1'b0;
        end else if (ab_read.addr_en) begin
            virtual_prefetch_pending_q <= early_candidate && !prefetch_start;
        end else if (prefetch_start) begin
            virtual_prefetch_pending_q <= 1'b0;
        end
    end
    // A data commit or a closed cycle clears early_valid_q. The window
    // registers change only at those points, so a matching Apple address
    // also selects the prefetched RAM byte.
    wire early_match = apple_serve && ab_read.rw && ram_hit && early_valid_q &&
                       (ab_read.addr == early_apple_addr_q);
    wire early_return = mem_complete && mem_prefetch_q && mem_read_live_q &&
                        early_valid_q && !cycle_closed &&
                        (!ab_read.serve_en || early_match);
    wire early_reply = early_match && ((early_result_q && !early_error_q) ||
                                      (early_return && !mem_rsp_error));
    wire early_error = early_match && ((early_result_q && early_error_q) ||
                                      (early_return && mem_rsp_error));
    wire early_pending = early_match && mem_prefetch_q && mem_read_live_q && !mem_complete;
    wire read_on_time = mem_complete && !mem_req_write && mem_read_live_q &&
                        read_wait_q && !reply_boundary;
    wire read_miss = read_wait_q && active && c8_selected &&
                     ((ab_read.data_en && ab_read.cycle_valid) ||
                      (!reply_boundary && (read_cycles_q == WAIT_WIDTH'(1)) &&
                       !read_on_time));

    // Scan from the oldest queued write. The last matching byte is newest.
    // Only the low address byte is compared at serve time.
    always_comb begin
        for (int slot = 0; slot < WRITE_DEPTH; slot++) begin
            if (serve_window == 2'd3) begin
                queue_slot_hit[slot] = slot_pointer_match_q[slot];
            end else begin
                queue_slot_hit[slot] = slot_page_match_q[serve_window][slot] &&
                                       (write_addr_q[slot][7:0] == ab_read.addr[7:0]);
            end
        end
        forwarding_hit = 1'b0;
        forwarding_data = 8'h00;
        for (int age = 0; age < WRITE_DEPTH; age++) begin
            if ((age < int'(write_count_q)) &&
                queue_slot_hit[write_head_q + WRITE_PTR_WIDTH'(age)]) begin
                forwarding_hit = 1'b1;
                forwarding_data = write_data_q[write_head_q + WRITE_PTR_WIDTH'(age)];
            end
        end
    end

    // RAM requests and committed writes remain ordered across C8 release.
    // A response from a closed read cycle can never drive the Apple bus.
    always_ff @(posedge clk) begin
        if (!rstn) begin
            mem_req_valid   <= 1'b0;
            mem_req_write   <= 1'b0;
            mem_req_addr    <= '0;
            mem_req_wdata   <= '0;
            mem_req_wstrb   <= '0;
            mem_inflight_q  <= 1'b0;
            mem_read_live_q <= 1'b0;
            mem_prefetch_q  <= 1'b0;
            mem_read_lane_q <= '0;
            write_head_q    <= '0;
            write_tail_q    <= '0;
            write_count_q   <= '0;
        end else begin
            if (queue_write) begin
                write_addr_q[write_tail_q] <= write_target;
                write_data_q[write_tail_q] <= ab_read.data;
                if (two_ddr_writes) begin
                    write_addr_q[write_tail_q + WRITE_PTR_WIDTH'(1)] <= mirror_addr;
                    write_data_q[write_tail_q + WRITE_PTR_WIDTH'(1)] <= ab_read.data;
                end
                write_tail_q <= write_tail_q + append_count[WRITE_PTR_WIDTH-1:0];
            end
            if (retire_write) begin
                write_head_q <= write_head_q + 1'b1;
            end
            if (queue_write || retire_write) begin
                write_count_q <= drained_count + (queue_write ? append_count : '0);
            end

            if (cycle_closed || (ab_read.serve_en && !early_match) ||
                read_miss || mem_complete) begin
                mem_read_live_q <= 1'b0;
            end
            if (mem_accept) begin
                mem_req_valid  <= 1'b0;
                mem_inflight_q <= 1'b1;
            end
            if (mem_complete) begin
                mem_inflight_q <= 1'b0;
            end
            if (mem_idle) begin
                if (prefetch_start) begin
                    mem_req_valid   <= 1'b1;
                    mem_req_write   <= 1'b0;
                    mem_req_addr    <= {early_ram_addr[19:3], 3'b000};
                    mem_req_wstrb   <= '0;
                    mem_read_lane_q <= early_ram_addr[2:0];
                    mem_read_live_q <= 1'b1;
                    mem_prefetch_q  <= 1'b1;
                end else if (read_wait_q && !read_sent_q && !reply_boundary && !read_miss) begin
                    mem_req_valid   <= 1'b1;
                    mem_req_write   <= 1'b0;
                    mem_req_addr    <= {apple_ram_addr_q[19:3], 3'b000};
                    mem_req_wstrb   <= '0;
                    mem_read_lane_q <= apple_ram_addr_q[2:0];
                    mem_read_live_q <= 1'b1;
                    mem_prefetch_q  <= 1'b0;
                end else if (write_count_q != '0) begin
                    mem_req_valid <= 1'b1;
                    mem_req_write <= 1'b1;
                    mem_req_addr  <= {write_addr_q[write_head_q][19:3], 3'b000};
                    mem_req_wdata <= {8{write_data_q[write_head_q]}};
                    mem_req_wstrb <= 8'b1 << write_addr_q[write_head_q][2:0];
                    mem_prefetch_q <= 1'b0;
                end
            end
        end
    end

    always_ff @(posedge clk) begin
        if (!rstn) begin
            apple_addr_q      <= '0;
            apple_ram_addr_q  <= '0;
            apple_ram_valid_q <= 1'b0;
            apple_ctrl_q      <= 1'b0;
            apple_cga_q       <= 1'b0;
            apple_pit_q       <= 1'b0;
            apple_port_ram_q  <= 1'b0;
            apple_mirror_q    <= 1'b0;
            apple_cycle_q     <= 1'b0;
            virtual_bus_q     <= 1'b0;
            port_response_q   <= 1'b0;
            reply_valid_q     <= 1'b0;
            reply_data_q      <= 8'hFF;
            read_wait_q       <= 1'b0;
            read_sent_q       <= 1'b0;
            read_cycles_q     <= '0;
            early_valid_q     <= 1'b0;
            early_apple_addr_q <= '0;
            early_result_q    <= 1'b0;
            early_error_q     <= 1'b0;
            early_data_q      <= '0;
        end else begin
            virtual_bus_q <= virtual_bus;
            if (!bus_cycle_active || (apple_cycle_q && !c8_selected) ||
                ab_read.addr_en || ab_read.serve_en || ab_read.data_en) begin
                apple_cycle_q   <= 1'b0;
                port_response_q <= 1'b0;
                reply_valid_q   <= 1'b0;
            end
            if (reply_boundary || read_miss || read_on_time) begin
                read_wait_q <= 1'b0;
            end else if (read_wait_q) begin
                read_cycles_q <= read_cycles_q - 1'b1;
            end
            if (mem_accept && !mem_req_write && mem_read_live_q) begin
                read_sent_q <= 1'b1;
            end
            if (reply_boundary) begin
                early_valid_q  <= 1'b0;
                early_result_q <= 1'b0;
            end
            if (prefetch_start) begin
                early_valid_q      <= 1'b1;
                early_apple_addr_q <= ab_read.addr_early;
            end
            if (early_return && !ab_read.serve_en) begin
                early_result_q <= 1'b1;
                early_error_q  <= mem_rsp_error;
                early_data_q   <= mem_rsp_rdata[8*mem_read_lane_q +: 8];
            end

            if (read_on_time && !mem_rsp_error) begin
                reply_data_q  <= mem_rsp_rdata[8*mem_read_lane_q +: 8];
                reply_valid_q <= 1'b1;
            end
            if (apple_serve) begin
                apple_cycle_q     <= c8_hit;
                apple_addr_q      <= ab_read.addr;
                apple_ram_addr_q  <= ram_addr[19:0];
                apple_ram_valid_q <= ram_hit && ram_in_range;
                apple_ctrl_q      <= ab_read.addr[15:6] == 10'h33C;
                apple_cga_q       <= cga_address;
                apple_pit_q       <= ab_read.addr[15:2] == 14'h33CA;
                apple_port_ram_q  <= ram_hit && ram_in_range && port_ram_address;
                apple_mirror_q    <= ((ab_read.addr == 16'hCF03) ||
                                      (ab_read.addr == 16'hCF04)) && pointer_mirror_q;
                if (ab_read.rw && (slot_hit || io_hit || c8_hit)) begin
                    if (port_hit || cga_read || port_ram_read) begin
                        port_response_q <= 1'b1;
                    end else if (ram_hit && ram_in_range) begin
                        if (forwarding_hit || early_reply) begin
                            reply_valid_q <= 1'b1;
                            reply_data_q <= forwarding_hit ? forwarding_data :
                                (early_return ? mem_rsp_rdata[8*mem_read_lane_q +: 8] : early_data_q);
                        end else if (!early_error) begin
                            read_wait_q   <= 1'b1;
                            read_sent_q   <= early_pending;
                            // ONE//e can wait up to about 4 us past normal
                            // capture. Physical reads keep their fixed window.
                            read_cycles_q <= virtual_bus_q ?
                                WAIT_WIDTH'(VIRTUAL_READ_WAIT_CYCLES) :
                                WAIT_WIDTH'(READ_WAIT_CYCLES);
                        end
                    end else begin
                        reply_valid_q <= 1'b1;
                        reply_data_q  <= local_read_data;
                    end
                end
            end
        end
    end

    always_ff @(posedge clk) begin
        if (!rstn) begin
            pointer_q       <= '0;
            page_cc_q       <= '0;
            page_ce_q       <= '0;
            apple_res_q     <= 1'b0;
            guest_reset_q   <= 1'b1;
            guest_halt_q    <= 1'b1;
            guest_int_q     <= 1'b0;
            guest_int_seq_q <= '0;
            timer0_pending_q <= 1'b0;
            irq_enable_q    <= 1'b0;
            pit_gate2_q     <= 1'b0;
            xfer_pending_q  <= 1'b0;
            xfer_q          <= '0;
            ack_seq_q       <= '0;
            reset_seq_q     <= '0;
            key_modifiers_q <= 8'h83;
            key_scan_q      <= '0;
            key_ready_q     <= 1'b0;
            io_index_q      <= '0;
        end else begin
            apple_res_q <= ab_read.res;
            if (as_client.awvalid && (as_common.awaddr == REG_IO_INDEX)) begin
                if (as_common.wstrb[0]) io_index_q[7:0] <= as_common.wdata[7:0];
                if (as_common.wstrb[1]) io_index_q[10:8] <= as_common.wdata[10:8];
            end

            if (!active) begin
                pointer_q       <= '0;
                page_cc_q       <= '0;
                page_ce_q       <= '0;
                guest_reset_q   <= 1'b1;
                guest_halt_q    <= 1'b1;
                guest_int_q     <= 1'b0;
                timer0_pending_q <= 1'b0;
                irq_enable_q    <= 1'b0;
                pit_gate2_q     <= 1'b0;
                xfer_pending_q  <= 1'b0;
                xfer_q          <= '0;
                key_ready_q     <= 1'b0;
                if (enabled && apple_res_q && !ab_read.res) begin
                    reset_seq_q <= reset_seq_q + 1'b1;
                end
            end else begin
                if (accept_guest_int) begin
                    guest_int_q <= 1'b0;
                end
                if (as_client.awvalid && (as_common.wstrb == 4'hF)) begin
                    if (post_xfer) begin
                        xfer_q         <= as_common.wdata[19:0];
                        xfer_pending_q <= 1'b1;
                    end
                    if (as_common.awaddr == REG_KEYS) begin
                        {key_ready_q, key_scan_q, key_modifiers_q} <= as_common.wdata[16:0];
                    end
                end
                if (apple_commit && (apple_addr_q == 16'hCF03)) begin
                    // Three visible bytes. The original wrap point needs a trace.
                    pointer_q <= pointer_q + 1'b1;
                end
                if (apple_commit && ab_read.rw && (apple_addr_q == 16'hCF33)) begin
                    if (!(as_client.awvalid && (as_common.awaddr == REG_KEYS) &&
                          (as_common.wstrb == 4'hF))) begin
                        key_ready_q <= 1'b0;
                    end
                end
                if (apple_ctrl_write) begin
                    case (apple_addr_q[5:0])
                        6'h00: pointer_q[7:0]   <= ab_read.data;
                        6'h01: pointer_q[15:8]  <= ab_read.data;
                        6'h02: pointer_q[23:16] <= ab_read.data;
                        6'h03, 6'h04: ;
                        6'h05: page_cc_q[15:8] <= ab_read.data;
                        6'h06: page_cc_q[7:0]  <= ab_read.data;
                        6'h0E: page_ce_q[15:8] <= ab_read.data;
                        6'h0F: page_ce_q[7:0]  <= ab_read.data;
                        6'h28, 6'h29, 6'h2A, 6'h2B,
                        6'h2C, 6'h2D, 6'h2E, 6'h2F: ;
                        6'h30: begin
                            if (ab_read.data[0]) pit_gate2_q    <= 1'b0;
                            if (ab_read.data[4]) guest_reset_q <= 1'b0;
                            if (ab_read.data[5]) guest_halt_q  <= 1'b0;
                            if (ab_read.data[6]) guest_int_q   <= 1'b0;
                            if (ab_read.data[7]) irq_enable_q  <= 1'b0;
                        end
                        6'h31: begin
                            if (ab_read.data[0]) pit_gate2_q <= 1'b1;
                            if (ab_read.data[4]) begin
                                guest_reset_q  <= 1'b1;
                                guest_int_q    <= 1'b0;
                                xfer_pending_q <= 1'b0;
                                if (!guest_reset_q) reset_seq_q <= reset_seq_q + 1'b1;
                            end
                            if (ab_read.data[5]) guest_halt_q <= 1'b1;
                            if (ab_read.data[6] && !ab_read.data[4]) begin
                                guest_int_q <= 1'b1;
                                guest_int_seq_q <= guest_int_seq_q + 1'b1;
                            end
                            if (ab_read.data[7]) irq_enable_q <= 1'b1;
                        end
                        6'h32: begin
                            if (ack_xfer) begin
                                xfer_pending_q <= 1'b0;
                                ack_seq_q      <= ack_seq_q + 1'b1;
                            end
                            if (ab_read.data[5]) timer0_pending_q <= 1'b0;
                        end
                        default: ; // Stored in control_ram.
                    endcase
                end
                if (cancel_xfer) begin
                    xfer_pending_q <= 1'b0;
                end
                if (timer0_tick) begin
                    timer0_pending_q <= 1'b1;
                end
            end
        end
    end

    always_ff @(posedge clk) begin
        if (!rstn) begin
            araddr_q <= '0;
        end else begin
            araddr_q <= as_common.araddr;
        end
        if (!rstn || (ps_diag_write && as_common.wdata[0])) begin
            read_misses_q <= '0;
            write_drops_q <= '0;
            mem_errors_q  <= '0;
            virtual_holds_q <= '0;
            virtual_hold_cycles_q <= '0;
            virtual_max_hold_q <= '0;
        end else begin
            if (read_miss) read_misses_q <= read_misses_q + 1'b1;
            if (ram_write && !queue_write) write_drops_q <= write_drops_q + 1'b1;
            if (early_error || (mem_complete && mem_rsp_error &&
                (mem_req_write || read_on_time))) mem_errors_q <= mem_errors_q + 1'b1;
            if (virtual_capture_wait) begin
                if (virtual_hold_cycles_q == '0) virtual_holds_q <= virtual_holds_q + 1'b1;
                if (virtual_hold_cycles_q == virtual_max_hold_q) begin
                    virtual_max_hold_q <= virtual_hold_cycles_q + 1'b1;
                end
                virtual_hold_cycles_q <= virtual_hold_cycles_q + 1'b1;
            end else begin
                virtual_hold_cycles_q <= '0;
            end
        end
    end

    always_comb begin
        as_client.rdata = '0;
        case (araddr_q)
            REG_ID: as_client.rdata = 32'h50435435;
            REG_STATUS: as_client.rdata = {guest_int_seq_q, reset_seq_q, ack_seq_q,
                ((read_misses_q != '0) || (write_drops_q != '0) || (mem_errors_q != '0)),
                writes_drained, xfer_pending_q, irq_enable_q, guest_int_q,
                guest_halt_q, guest_reset_q, enabled};
            REG_XFER: as_client.rdata = {xfer_pending_q, 3'd0, reset_seq_q, xfer_q};
            REG_IO_INDEX: as_client.rdata = {21'd0, io_index_q};
            REG_IO_DATA: begin
                if (ps_port_selected) begin
                    as_client.rdata = {24'd0, ps_port_data_q};
                end else if (io_index_q[10:6] == 5'h10) begin
                    as_client.rdata = {24'd0, ctrl_ps_rdata};
                end
            end
            REG_MISSES: as_client.rdata = read_misses_q;
            REG_DROPS: as_client.rdata = write_drops_q;
            REG_ERRORS: as_client.rdata = mem_errors_q;
            REG_HOLDS: as_client.rdata = virtual_holds_q;
            REG_HOLD_MAX: as_client.rdata = 32'(virtual_max_hold_q);
            REG_CGA_GEOMETRY: as_client.rdata =
                {8'b0, crtc_regs_q[9], crtc_regs_q[6], crtc_regs_q[1]};
            REG_CGA_RASTER: as_client.rdata =
                {cga_frame_q, cga_adjust_q, cga_hblank_q, 2'b0, cga_row_q, cga_raster_q};
            REG_KEYS: as_client.rdata = {15'd0, key_ready_q, key_scan_q, key_modifiers_q};
            REG_ACK_DATA: as_client.rdata = {16'd0, ack_data_q};
            REG_CGA_STATE: as_client.rdata = {cga_seq_q, cga_color_q, cga_mode_q};
            REG_CRTC_0A: as_client.rdata = {crtc_regs_q[13], crtc_regs_q[12],
                                           crtc_regs_q[11], crtc_regs_q[10]};
            REG_CRTC_0E: as_client.rdata = {16'd0, crtc_regs_q[15], crtc_regs_q[14]};
            default: ;
        endcase
    end

    always_comb begin
        ab_write = '0;
        ab_write.assert_irq = rstn && active && irq_enable_q && apple_irq_pending;
        ab_write.wr_data = port_response_q ? apple_port_reply_q : reply_data_q;
        ab_write.wr_data_en = rstn && bus_cycle_active && ab_read.cycle_valid && ab_read.rw &&
                              !ab_read.addr_en &&
                              (!apple_cycle_q || c8_owned_q) &&
                              (reply_valid_q || (port_response_q && port_data_ready_q[1]));
    end
endmodule
