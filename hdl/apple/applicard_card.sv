`timescale 1ns / 1ps

// Slot-5 coprocessor front end.
//
// MODE=0 preserves the PCPI Appli-Card latch protocol used by the PS Z80
// emulator. MODE=1 implements the ALF AD8088's sixteen-port mailbox and a
// deliberately sparse Apple-memory master for its $10000-$1FFFF shared-memory
// window. The 8088 itself and the AD128K RAM are emulated by the PS.
//
// The original four Z80 AxiSimple registers are intentionally unchanged.
// AD8088 and bus-master registers begin at index 4.
module applicard_card (
    input  logic                     clk,
    input  logic                     rstn,
    input  globals::AppleBus_read    ab_read,
    input  logic                     card_enabled,
    input  logic                     disk2_timing_active,
    input  logic                     vtw_enabled,
    input  logic                     vtw_bus_owned,
    input  logic [2:0]               slot_assign,
    input  globals::AxiSimple_common as_common,
    AxiSimple_if.client              as_client,
    output globals::AppleBus_write   ab_write
);

    // Original PCPI register indices. Do not renumber: released firmware and
    // source tests depend on this exact ABI.
    localparam logic [7:0] AXI_REG_STATUS      = 8'h00;
    localparam logic [7:0] AXI_REG_TO6502      = 8'h01;
    localparam logic [7:0] AXI_REG_CONTROL     = 8'h02;
    localparam logic [7:0] AXI_REG_DEBUG       = 8'h03;

    // AD8088 extension.
    localparam logic [7:0] AXI_REG_MODE        = 8'h04;
    localparam logic [7:0] AXI_REG_AD_STATUS   = 8'h05;
    localparam logic [7:0] AXI_REG_AD_CONTROL  = 8'h06;
    localparam logic [7:0] AXI_REG_AD_PORTS0   = 8'h07;
    localparam logic [7:0] AXI_REG_AD_PORTS1   = 8'h08;
    localparam logic [7:0] AXI_REG_AD_PORTS2   = 8'h09;
    localparam logic [7:0] AXI_REG_AD_PORTS3   = 8'h0A;
    localparam logic [7:0] AXI_REG_BUS_COMMAND = 8'h0B;
    localparam logic [7:0] AXI_REG_BUS_STATUS  = 8'h0C;
    localparam logic [7:0] AXI_REG_BUS_BUFFER0 = 8'h40;

    localparam logic MODE_Z80    = 1'b0;
    localparam logic MODE_AD8088 = 1'b1;

    localparam logic [3:0] IO_READ_Z80_DATA  = 4'h0;
    localparam logic [3:0] IO_WRITE_Z80_DATA = 4'h1;
    localparam logic [3:0] IO_Z80_BUSY       = 4'h2;
    localparam logic [3:0] IO_Z80_DATA_READY = 4'h3;
    localparam logic [3:0] IO_RESET          = 4'h5;
    localparam logic [3:0] IO_NMI            = 4'h7;

    // Z80 PCPI state.
    logic [7:0] toz80_q;
    logic [7:0] to6502_q;
    logic       f_z80_q;
    logic       f_6502_q;
    logic       reset_req_q;
    logic       nmi_req_q;
    logic [7:0] toz80_seq_q;

    // AD8088 mailbox state. ad_flag_q is the real card's shared busy/ready
    // flag: PS completion or 8088 OUT 0 sets it; an Apple write to port 0
    // clears it. While the PROM monitor is idle, a port-0 write also posts a
    // command for the PS dispatcher.
    logic       mode_q;
    logic [7:0] ad_port_q [0:15];
    logic       ad_cmd_pending_q;
    logic       ad_flag_q;
    logic       ad_running_q;
    logic       ad_abort_req_q;
    logic       ad_reset_req_q;
    logic [3:0] ad_last_port_q;
    logic [7:0] ad_last_data_q;
    logic [7:0] ad_seq_q;

    logic apple_res_q;

    wire enabled = card_enabled && (slot_assign != 3'h0);
    wire apple_bus_active = enabled && ab_read.res;
    wire slot_io_hit =
        apple_bus_active &&
        (ab_read.addr[15:8] == 8'hC0) &&
        (ab_read.addr[7:4] == (4'h8 + {1'b0, slot_assign}));
    wire [3:0] io_idx = ab_read.addr[3:0];

    logic [7:0] io_read_byte;
    logic       io_read_hold_q;
    logic [7:0] io_read_data_q;
    always_comb begin
        if (mode_q == MODE_AD8088) begin
            // Only port 0 is readable on the real AD8088. Bits 6:0 are
            // unspecified; zero is deterministic and software-safe.
            io_read_byte = (io_idx == 4'h0) ? {ad_flag_q, 7'b0} : 8'hFF;
        end else begin
            case (io_idx)
                IO_READ_Z80_DATA:  io_read_byte = to6502_q;
                IO_WRITE_Z80_DATA: io_read_byte = toz80_q;
                IO_Z80_BUSY:       io_read_byte = {f_z80_q, 7'b0};
                IO_Z80_DATA_READY: io_read_byte = {f_6502_q, 7'b0};
                default:           io_read_byte = 8'hFF;
            endcase
        end
    end

    // A sparse bus-master transaction asserts DMA at the PH1 drive point,
    // waits a few fabric clocks for the motherboard CPU buffers to
    // disconnect, then drives an inert $0200 read for the remainder of that
    // cut cycle and one further full cycle before the first transfer. After
    // the last transfer it parks one cycle, releases address/R-W late in
    // that cycle's PH0 (after the DRAM CAS window), and releases DMA at the
    // next drive point. Keep DMA released for two complete Apple cycles
    // before another request starts; otherwise back-to-back sparse holds can
    // stop an NMOS 6502 indefinitely even though DMA briefly rises between
    // them.
    //
    // No owned cycle may ever present a floating bus to a PH0 decode window.
    // The level-shifted bus holds its last value briefly and then decays LOW
    // (hardware crash dumps show staged bytes rewritten as bit-cleared
    // copies of themselves), so a floating R/W residue matures into a rogue
    // DRAM or soft-switch WRITE at a decaying address. The only undriven
    // owned intervals left are two sub-100 ns PH1 slivers at acquisition
    // and release, which no PH0 window ever samples.
    typedef enum logic [2:0] {
        BUS_IDLE,
        BUS_DMA_GRACE_1,
        BUS_DMA_GRACE_2,
        BUS_DRIVE,
        BUS_BULK_NEXT,
        BUS_CAPTURE,
        BUS_RELEASE_PARK,
        BUS_RELEASE_GUARD
    } bus_state_t;
    localparam logic [15:0] BUS_PARK_ADDR = 16'h0200;
    localparam logic [1:0] BUS_RECOVERY_CYCLES = 2'd2;
    /* Fabric clocks between the /DMA assert and the first parked drive.
     * The motherboard buffers disconnect within tens of nanoseconds; eight
     * clocks (~60 ns) clear them while the cut cycle's PH0 decode window is
     * still hundreds of nanoseconds away. */
    localparam logic [3:0] BUS_PARK_DELAY_CLKS = 4'd8;
    bus_state_t bus_state_q;
    logic        bus_pending_q;
    logic [15:0] bus_addr_q;
    logic        bus_rw_q;
    logic [7:0]  bus_wdata_q;
    logic [7:0]  bus_rdata_q;
    logic        bus_done_q;
    logic        bus_error_q;
    logic [15:0] bus_drive_addr_q;
    logic        bus_drive_rw_q;
    logic [7:0]  bus_drive_data_q;
    logic        bus_addr_drive_q;
    logic        bus_data_drive_q;
    logic        bus_dma_q;
    logic        bus_bulk_q;
    logic [7:0]  bus_bulk_remaining_q;
    logic [7:0]  bus_bulk_index_q;
    logic [7:0]  bus_seq_q;
    logic        bus_cancel_pending_q;
    logic [1:0]  bus_recovery_q;
    logic [3:0]  bus_park_delay_q;
    /* Sticky per transaction: the command was rejected or stopped while
     * another owner blocked the bus. The PS classifies a failed transfer
     * as retryable from this latch; the live bus_blocked signal may have
     * dropped again by the time the PS reads the completion. */
    logic        bus_blocked_cause_q;
    /* Four bytes reduce PS traffic without holding /DMA past the 6502's
     * 10 us register-retention limit. Including acquisition and release, a
     * four-byte burst stays below that limit on the 1 MHz Apple bus. */
    logic [31:0] bus_buffer_q;

    function automatic logic [7:0] bus_buffer_byte(input logic [7:0] index);
        case (index[1:0])
            2'd0: bus_buffer_byte = bus_buffer_q[7:0];
            2'd1: bus_buffer_byte = bus_buffer_q[15:8];
            2'd2: bus_buffer_byte = bus_buffer_q[23:16];
            default: bus_buffer_byte = bus_buffer_q[31:24];
        endcase
    endfunction

    // vTW and the AD8088 are both Apple-bus masters. Block from the moment
    // vTW is enabled, rather than waiting for bus_owned, so neither master
    // can enter its acquisition sequence in the other's handoff window.
    wire bus_blocked = disk2_timing_active || vtw_enabled || vtw_bus_owned;
    wire bus_busy = bus_pending_q || (bus_state_q != BUS_IDLE);
    wire buffer_bus_write = bus_state_q == BUS_DRIVE &&
                            ab_read.data_en && bus_rw_q && bus_bulk_q;
    wire buffer_axi_write = as_client.awvalid &&
                            as_common.awaddr == AXI_REG_BUS_BUFFER0 &&
                            !bus_busy;
    wire bus_cancel_write = as_client.awvalid &&
                            (as_common.awaddr == AXI_REG_AD_CONTROL) &&
                            as_common.wstrb[0] &&
                            (as_common.wdata[7] || as_common.wdata[6]) &&
                            (mode_q == MODE_AD8088);
    wire bus_stop_requested = bus_cancel_pending_q || bus_cancel_write ||
                              bus_blocked;

    globals::AppleBus_write ab_write_d;
    globals::AppleBus_write ab_write_q;
    assign ab_write = ab_write_q;

    /* A stopped motherboard CPU cannot issue a real slot access. Ignore the
     * physical address residue throughout acquisition, transfer, and release;
     * otherwise a held $C0D0 write can repost or abort a mailbox command. The
     * registered client word covers the final clock after the FSM returns to
     * IDLE but DMA is still present at the arbiter. */
    wire bus_master_owns = (bus_state_q != BUS_IDLE) || bus_dma_q ||
                           ab_write_q.assert_dma;
    wire ab_io_read  = ab_read.serve_en && ab_read.rw && slot_io_hit &&
                       !bus_master_owns;
    wire ab_io_write = ab_read.data_en && !ab_read.rw && slot_io_hit &&
                       !bus_master_owns;

    always_comb begin
        ab_write_d = '0;
        ab_write_d.wr_addr       = bus_drive_addr_q;
        ab_write_d.wr_rw         = bus_drive_rw_q;
        ab_write_d.wr_addr_rw_en = bus_addr_drive_q;
        ab_write_d.wr_data       = bus_drive_data_q;
        /* Motherboard-RAM writes do not use the ordinary late card-response
         * window. The wrapper converts this logical whole-cycle request into
         * the Tech Note #2 PH0 write window at the pins. */
        ab_write_d.wr_dma_data_en = bus_data_drive_q;
        ab_write_d.assert_dma    = bus_dma_q;

        // Slot-I/O reads and bus-master cycles cannot legitimately overlap:
        // the motherboard CPU is stopped while bus_addr_drive_q is true.
        if ((ab_io_read || io_read_hold_q) && !bus_addr_drive_q &&
            !bus_master_owns) begin
            ab_write_d.wr_data    = ab_io_read ? io_read_byte
                                               : io_read_data_q;
            ab_write_d.wr_data_en = 1'b1;
        end
    end

    logic [7:0] axi_read_addr_q;
    integer i;

    // The Apple read capture and PS preload cannot overlap: AXI writes are
    // accepted only while the bus engine is idle.
    always_ff @(posedge clk) begin
        if (buffer_bus_write) begin
            case (bus_bulk_index_q[1:0])
                2'd0: bus_buffer_q[7:0]   <= ab_read.data;
                2'd1: bus_buffer_q[15:8]  <= ab_read.data;
                2'd2: bus_buffer_q[23:16] <= ab_read.data;
                default: bus_buffer_q[31:24] <= ab_read.data;
            endcase
        end else if (buffer_axi_write) begin
            if (as_common.wstrb[0])
                bus_buffer_q[7:0] <= as_common.wdata[7:0];
            if (as_common.wstrb[1])
                bus_buffer_q[15:8] <= as_common.wdata[15:8];
            if (as_common.wstrb[2])
                bus_buffer_q[23:16] <= as_common.wdata[23:16];
            if (as_common.wstrb[3])
                bus_buffer_q[31:24] <= as_common.wdata[31:24];
        end
    end

    always_ff @(posedge clk) begin
        if (!rstn) begin
            toz80_q          <= 8'h00;
            to6502_q         <= 8'h00;
            f_z80_q          <= 1'b0;
            f_6502_q         <= 1'b0;
            reset_req_q      <= 1'b0;
            nmi_req_q        <= 1'b0;
            toz80_seq_q      <= 8'h00;
            mode_q           <= MODE_Z80;
            ad_cmd_pending_q <= 1'b0;
            ad_flag_q        <= 1'b1;
            ad_running_q     <= 1'b0;
            ad_abort_req_q   <= 1'b0;
            ad_reset_req_q   <= 1'b0;
            ad_last_port_q   <= 4'h0;
            ad_last_data_q   <= 8'h00;
            ad_seq_q         <= 8'h00;
            for (i = 0; i < 16; i = i + 1) begin
                ad_port_q[i] <= 8'h00;
            end
            apple_res_q      <= 1'b1;
            ab_write_q       <= '0;
            io_read_hold_q   <= 1'b0;
            io_read_data_q   <= 8'hFF;
            axi_read_addr_q  <= 8'd0;

            bus_state_q      <= BUS_IDLE;
            bus_pending_q    <= 1'b0;
            bus_addr_q       <= 16'h0000;
            bus_rw_q         <= 1'b1;
            bus_wdata_q      <= 8'h00;
            bus_rdata_q      <= 8'hFF;
            bus_done_q       <= 1'b0;
            bus_error_q      <= 1'b0;
            bus_drive_addr_q <= 16'h0000;
            bus_drive_rw_q   <= 1'b1;
            bus_drive_data_q <= 8'h00;
            bus_addr_drive_q <= 1'b0;
            bus_data_drive_q <= 1'b0;
            bus_dma_q        <= 1'b0;
            bus_bulk_q       <= 1'b0;
            bus_bulk_remaining_q <= 8'd0;
            bus_bulk_index_q <= 8'd0;
            bus_seq_q        <= 8'd0;
            bus_cancel_pending_q <= 1'b0;
            bus_recovery_q   <= 2'd0;
            bus_park_delay_q <= 4'd0;
            bus_blocked_cause_q <= 1'b0;
        end else begin
            ab_write_q      <= ab_write_d;
            axi_read_addr_q <= as_common.araddr;
            apple_res_q     <= ab_read.res;

            // serve_en is an early, one-clock decode pulse. Preserve the
            // selected byte and keep driving it through the authoritative
            // data_en sample, exactly as the original Z80-only front end did.
            // Releasing after serve_en made late Apple reads see floating-bus
            // data and could falsely advance the AD8088 command handshake.
            if (bus_master_owns) begin
                io_read_hold_q <= 1'b0;
            end else begin
                if (ab_read.data_en) begin
                    io_read_hold_q <= 1'b0;
                end
                if (ab_io_read) begin
                    io_read_hold_q <= 1'b1;
                    io_read_data_q <= io_read_byte;
                end
            end

            // PS register writes. Apple-facing operations later in this
            // block win a same-cycle conflict because they are what the
            // physical Apple actually observed.
            if (as_client.awvalid) begin
                case (as_common.awaddr)
                    AXI_REG_TO6502: begin
                        if (as_common.wstrb[0] && mode_q == MODE_Z80) begin
                            to6502_q <= as_common.wdata[7:0];
                            f_6502_q <= 1'b1;
                        end
                    end
                    AXI_REG_CONTROL: begin
                        if (as_common.wstrb[0] && mode_q == MODE_Z80) begin
                            if (as_common.wdata[0] &&
                                (as_common.wdata[15:8] == toz80_seq_q))
                                f_z80_q <= 1'b0;
                            if (as_common.wdata[1]) reset_req_q <= 1'b0;
                            if (as_common.wdata[2]) nmi_req_q <= 1'b0;
                        end
                    end
                    AXI_REG_AD_CONTROL: begin
                        if (as_common.wstrb[0] && mode_q == MODE_AD8088) begin
                            if (as_common.wdata[0] &&
                                (as_common.wdata[15:8] == ad_seq_q))
                                ad_cmd_pending_q <= 1'b0;
                            if (as_common.wdata[1]) ad_flag_q <= 1'b1;
                            if (as_common.wdata[2]) ad_flag_q <= 1'b0;
                            if (as_common.wdata[3]) ad_running_q <= 1'b1;
                            if (as_common.wdata[4]) ad_running_q <= 1'b0;
                            if (as_common.wdata[5]) begin
                                ad_abort_req_q <= 1'b0;
                                ad_reset_req_q <= 1'b0;
                            end
                            if (as_common.wdata[6]) begin
                                ad_cmd_pending_q <= 1'b0;
                                ad_flag_q        <= 1'b1;
                                ad_running_q     <= 1'b0;
                                ad_abort_req_q   <= 1'b0;
                                ad_reset_req_q   <= 1'b0;
                            end
                        end
                    end
                    AXI_REG_AD_PORTS0,
                    AXI_REG_AD_PORTS1,
                    AXI_REG_AD_PORTS2,
                    AXI_REG_AD_PORTS3: begin
                        if (mode_q == MODE_AD8088) begin
                            for (i = 0; i < 4; i = i + 1) begin
                                if (as_common.wstrb[i]) begin
                                    ad_port_q[((as_common.awaddr -
                                                AXI_REG_AD_PORTS0) << 2) + i]
                                        <= as_common.wdata[i*8 +: 8];
                                end
                            end
                        end
                    end
                    AXI_REG_BUS_COMMAND: begin
                        if (as_common.wstrb == 4'hF && as_common.wdata[31]) begin
                            bus_seq_q   <= bus_seq_q + 8'd1;
                            bus_done_q  <= 1'b0;
                            bus_error_q <= 1'b0;
                            bus_blocked_cause_q <= 1'b0;
                            if (mode_q != MODE_AD8088 || !enabled || bus_busy ||
                                bus_blocked || !ab_read.res ||
                                (as_common.wdata[30] &&
                                 as_common.wdata[23:16] > 8'd3)) begin
                                bus_error_q <= 1'b1;
                                bus_done_q  <= 1'b1;
                                /* Latched, not live: the PS may read this
                                 * completion after the blocking window has
                                 * already closed. */
                                bus_blocked_cause_q <= bus_blocked;
                            end else begin
                                bus_addr_q    <= as_common.wdata[15:0];
                                bus_wdata_q   <= as_common.wdata[23:16];
                                bus_rw_q      <= as_common.wdata[24];
                                bus_bulk_q    <= as_common.wdata[30];
                                bus_bulk_remaining_q <= as_common.wdata[30]
                                    ? as_common.wdata[23:16] : 8'd0;
                                bus_bulk_index_q <= 8'd0;
                                bus_pending_q <= 1'b1;
                                bus_cancel_pending_q <= 1'b0;
                            end
                        end
                    end
                    default: begin
                    end
                endcase
            end

            // Apple slot-I/O protocol.
            if (ab_io_write) begin
                if (mode_q == MODE_AD8088) begin
                    ad_port_q[io_idx] <= ab_read.data;
                    ad_last_port_q    <= io_idx;
                    ad_last_data_q    <= ab_read.data;
                    if (io_idx == 4'h0) begin
                        if (ad_running_q) begin
                            if (ad_flag_q) begin
                                // Hardware rule: any port-0 write clears an
                                // outstanding 8088 signal. Reboot Camp uses
                                // zero here after each command.
                                ad_flag_q <= 1'b0;
                            end else if (ab_read.data == 8'h00) begin
                                // With no signal pending, zero asks the clean
                                // room monitor to stop the running program.
                                ad_abort_req_q <= 1'b1;
                            end
                        end else begin
                            ad_flag_q        <= 1'b0;
                            ad_cmd_pending_q <= 1'b1;
                            ad_seq_q         <= ad_seq_q + 8'd1;
                        end
                    end
                end else begin
                    case (io_idx)
                        IO_WRITE_Z80_DATA: begin
                            toz80_q     <= ab_read.data;
                            f_z80_q     <= 1'b1;
                            toz80_seq_q <= toz80_seq_q + 8'd1;
                        end
                        IO_RESET: begin
                            toz80_q     <= 8'h00;
                            to6502_q    <= 8'h00;
                            f_z80_q     <= 1'b0;
                            f_6502_q    <= 1'b0;
                            reset_req_q <= 1'b1;
                            toz80_seq_q <= toz80_seq_q + 8'd1;
                        end
                        IO_NMI: nmi_req_q <= 1'b1;
                        default: begin
                        end
                    endcase
                end
            end

            if (ab_io_read && mode_q == MODE_Z80) begin
                case (io_idx)
                    IO_READ_Z80_DATA: f_6502_q <= 1'b0;
                    IO_RESET: begin
                        toz80_q     <= 8'h00;
                        to6502_q    <= 8'h00;
                        f_z80_q     <= 1'b0;
                        f_6502_q    <= 1'b0;
                        reset_req_q <= 1'b1;
                        toz80_seq_q <= toz80_seq_q + 8'd1;
                    end
                    IO_NMI: nmi_req_q <= 1'b1;
                    default: begin
                    end
                endcase
            end

            // Sparse Apple-memory transaction state machine.
            case (bus_state_q)
                BUS_IDLE: begin
                    bus_addr_drive_q <= 1'b0;
                    bus_data_drive_q <= 1'b0;
                    bus_dma_q        <= 1'b0;
                    if (ab_read.drive_en && bus_recovery_q != 2'd0) begin
                        bus_recovery_q <= bus_recovery_q - 2'd1;
                    end else if (bus_pending_q &&
                        bus_recovery_q == 2'd0 && !bus_blocked && enabled &&
                        mode_q == MODE_AD8088 && ab_read.res &&
                        ab_read.drive_en) begin
                        bus_dma_q        <= 1'b1;
                        bus_park_delay_q <= BUS_PARK_DELAY_CLKS;
                        bus_state_q      <= BUS_DMA_GRACE_1;
                    end
                end
                BUS_DMA_GRACE_1: begin
                    bus_dma_q <= 1'b1;
                    /* Park the cut cycle itself. The motherboard CPU froze
                     * inside this cycle; once its buffers have let go, the
                     * card must own what this cycle's PH0 decodes, or the
                     * decaying residue of the interrupted access replays --
                     * as a rogue write when that access was a write. */
                    if (bus_park_delay_q != 4'd0) begin
                        bus_park_delay_q <= bus_park_delay_q - 4'd1;
                    end else begin
                        bus_drive_addr_q <= BUS_PARK_ADDR;
                        bus_drive_rw_q   <= 1'b1;
                        bus_addr_drive_q <= 1'b1;
                        bus_data_drive_q <= 1'b0;
                    end
                    if (bus_stop_requested) begin
                        /* Stops release through the driven park as well: a
                         * full guarded float decays into the same rogue
                         * write on the error path as anywhere else. */
                        bus_error_q <= 1'b1;
                        if (bus_blocked) bus_blocked_cause_q <= 1'b1;
                        if (ab_read.drive_en) begin
                            bus_drive_addr_q <= BUS_PARK_ADDR;
                            bus_drive_rw_q   <= 1'b1;
                            bus_addr_drive_q <= 1'b1;
                            bus_data_drive_q <= 1'b0;
                            bus_state_q      <= BUS_RELEASE_PARK;
                        end
                    end else if (ab_read.drive_en) begin
                        bus_drive_addr_q <= BUS_PARK_ADDR;
                        bus_drive_rw_q   <= 1'b1;
                        bus_addr_drive_q <= 1'b1;
                        bus_data_drive_q <= 1'b0;
                        bus_state_q <= BUS_DMA_GRACE_2;
                    end
                end
                BUS_DMA_GRACE_2: begin
                    bus_dma_q <= 1'b1;
                    if (bus_stop_requested) begin
                        bus_error_q <= 1'b1;
                        if (bus_blocked) bus_blocked_cause_q <= 1'b1;
                        if (ab_read.drive_en) begin
                            bus_drive_addr_q <= BUS_PARK_ADDR;
                            bus_drive_rw_q   <= 1'b1;
                            bus_addr_drive_q <= 1'b1;
                            bus_data_drive_q <= 1'b0;
                            bus_state_q      <= BUS_RELEASE_PARK;
                        end
                    end else if (ab_read.drive_en) begin
                        bus_drive_addr_q <= bus_addr_q;
                        bus_drive_rw_q   <= bus_rw_q;
                        bus_drive_data_q <= bus_bulk_q
                            ? bus_buffer_byte(8'd0) : bus_wdata_q;
                        bus_addr_drive_q <= 1'b1;
                        bus_data_drive_q <= !bus_rw_q;
                        bus_state_q      <= BUS_DRIVE;
                    end
                end
                BUS_DRIVE: begin
                    bus_dma_q <= 1'b1;
                    if (ab_read.data_en) begin
                        if (bus_rw_q) begin
                            bus_rdata_q <= ab_read.data;
                        end
                        if (bus_stop_requested) begin
                            bus_error_q <= 1'b1;
                            if (bus_blocked) bus_blocked_cause_q <= 1'b1;
                            bus_state_q <= BUS_CAPTURE;
                        end else if (bus_bulk_q &&
                                     bus_bulk_remaining_q != 8'd0) begin
                            bus_bulk_remaining_q <= bus_bulk_remaining_q - 8'd1;
                            bus_bulk_index_q <= bus_bulk_index_q + 8'd1;
                            bus_state_q <= BUS_BULK_NEXT;
                        end else begin
                            bus_state_q <= BUS_CAPTURE;
                        end
                    end
                end
                BUS_BULK_NEXT: begin
                    bus_dma_q <= 1'b1;
                    if (ab_read.drive_en) begin
                        if (bus_stop_requested) begin
                            bus_error_q      <= 1'b1;
                            if (bus_blocked) bus_blocked_cause_q <= 1'b1;
                            bus_drive_addr_q <= BUS_PARK_ADDR;
                            bus_drive_rw_q   <= 1'b1;
                            bus_addr_drive_q <= 1'b1;
                            bus_data_drive_q <= 1'b0;
                            bus_state_q      <= BUS_RELEASE_PARK;
                        end else begin
                            bus_drive_addr_q <= bus_drive_addr_q + 16'd1;
                            if (!bus_rw_q)
                                bus_drive_data_q <=
                                    bus_buffer_byte(bus_bulk_index_q);
                            bus_state_q <= BUS_DRIVE;
                        end
                    end
                end
                BUS_CAPTURE: begin
                    bus_dma_q <= 1'b1;
                    if (ab_read.drive_en) begin
                        bus_drive_addr_q <= BUS_PARK_ADDR;
                        bus_drive_rw_q   <= 1'b1;
                        bus_addr_drive_q <= 1'b1;
                        bus_data_drive_q <= 1'b0;
                        bus_state_q      <= BUS_RELEASE_PARK;
                    end
                end
                BUS_RELEASE_PARK: begin
                    bus_dma_q        <= 1'b1;
                    bus_drive_addr_q <= BUS_PARK_ADDR;
                    bus_drive_rw_q   <= 1'b1;
                    bus_data_drive_q <= 1'b0;
                    /* Drive the park read through this cycle's DRAM window,
                     * then release the address/R-W drivers late in its PH0,
                     * after CAS. The residue that follows lives only until
                     * the next drive point -- a PH1 sliver no PH0 decode
                     * ever samples, so it cannot decay into a ghost write
                     * the way a full guarded float cycle could. /DMA itself
                     * returns at that drive point. */
                    if (ab_read.data_en) begin
                        bus_addr_drive_q <= 1'b0;
                    end
                    if (ab_read.drive_en) begin
                        bus_addr_drive_q <= 1'b0;
                        bus_dma_q        <= 1'b0;
                        bus_pending_q    <= 1'b0;
                        bus_done_q       <= 1'b1;
                        bus_state_q      <= BUS_IDLE;
                        bus_recovery_q   <= BUS_RECOVERY_CYCLES;
                        bus_cancel_pending_q <= 1'b0;
                    end
                end
                BUS_RELEASE_GUARD: begin
                    /* Unreachable: every path, error paths included, now
                     * releases through the parked cycle, because a fully
                     * floating owned cycle decays into a rogue write. Kept
                     * only as a safe landing for a corrupted state
                     * register. */
                    bus_addr_drive_q <= 1'b0;
                    bus_data_drive_q <= 1'b0;
                    bus_dma_q        <= 1'b1;
                    if (ab_read.drive_en) begin
                        bus_dma_q     <= 1'b0;
                        bus_pending_q <= 1'b0;
                        bus_done_q    <= 1'b1;
                        bus_state_q   <= BUS_IDLE;
                        bus_recovery_q <= BUS_RECOVERY_CYCLES;
                        bus_cancel_pending_q <= 1'b0;
                    end
                end
                default: bus_state_q <= BUS_IDLE;
            endcase

            // A timeout asks for a guarded stop. If a cycle is active, finish
            // its data phase, release through the driven park cycle, then
            // release /DMA.
            if (bus_cancel_write) begin
                bus_error_q      <= 1'b1;
                if (bus_blocked) bus_blocked_cause_q <= 1'b1;
                io_read_hold_q   <= 1'b0;
                if (bus_state_q == BUS_IDLE) begin
                    bus_pending_q        <= 1'b0;
                    bus_done_q           <= 1'b1;
                    bus_addr_drive_q     <= 1'b0;
                    bus_data_drive_q     <= 1'b0;
                    bus_dma_q            <= 1'b0;
                    bus_bulk_q           <= 1'b0;
                    bus_cancel_pending_q <= 1'b0;
                end else begin
                    bus_cancel_pending_q <= 1'b1;
                end
            end

            // A mode switch is a hard transaction boundary for both virtual
            // processors and must release any in-flight bus ownership.
            if (as_client.awvalid && as_common.awaddr == AXI_REG_MODE &&
                as_common.wstrb[0]) begin
                mode_q           <= as_common.wdata[0];
                toz80_q          <= 8'h00;
                to6502_q         <= 8'h00;
                f_z80_q          <= 1'b0;
                f_6502_q         <= 1'b0;
                reset_req_q      <= (as_common.wdata[0] == MODE_Z80);
                nmi_req_q        <= 1'b0;
                toz80_seq_q      <= toz80_seq_q + 8'd1;
                ad_cmd_pending_q <= 1'b0;
                ad_flag_q        <= 1'b1;
                ad_running_q     <= 1'b0;
                ad_abort_req_q   <= 1'b0;
                ad_reset_req_q   <= (as_common.wdata[0] == MODE_AD8088);
                ad_seq_q         <= ad_seq_q + 8'd1;
                bus_state_q      <= BUS_IDLE;
                bus_pending_q    <= 1'b0;
                bus_done_q       <= 1'b0;
                bus_error_q      <= 1'b0;
                bus_addr_drive_q <= 1'b0;
                bus_data_drive_q <= 1'b0;
                bus_dma_q        <= 1'b0;
                bus_bulk_q       <= 1'b0;
                bus_cancel_pending_q <= 1'b0;
                bus_recovery_q   <= 2'd0;
                bus_blocked_cause_q <= 1'b0;
                io_read_hold_q   <= 1'b0;
            end

            // Disabling slot 5 or losing RESET aborts a bus transaction and
            // releases all pins immediately. The mailbox mode itself persists.
            if (!enabled) begin
                bus_state_q      <= BUS_IDLE;
                bus_pending_q    <= 1'b0;
                bus_addr_drive_q <= 1'b0;
                bus_data_drive_q <= 1'b0;
                bus_dma_q        <= 1'b0;
                bus_cancel_pending_q <= 1'b0;
                bus_recovery_q   <= 2'd0;
                bus_blocked_cause_q <= 1'b0;
                io_read_hold_q   <= 1'b0;
            end

            // Edge-detected Apple reset: a held reset must not repeatedly
            // increment sequences or re-arm requests every fabric clock.
            if (apple_res_q && !ab_read.res) begin
                toz80_q          <= 8'h00;
                to6502_q         <= 8'h00;
                f_z80_q          <= 1'b0;
                f_6502_q         <= 1'b0;
                reset_req_q      <= 1'b1;
                nmi_req_q        <= 1'b0;
                toz80_seq_q      <= toz80_seq_q + 8'd1;
                ad_cmd_pending_q <= 1'b0;
                ad_flag_q        <= 1'b1;
                ad_running_q     <= 1'b0;
                ad_abort_req_q   <= 1'b0;
                ad_reset_req_q   <= 1'b1;
                ad_seq_q         <= ad_seq_q + 8'd1;
                bus_state_q      <= BUS_IDLE;
                bus_pending_q    <= 1'b0;
                bus_done_q       <= 1'b0;
                bus_error_q      <= 1'b0;
                bus_addr_drive_q <= 1'b0;
                bus_data_drive_q <= 1'b0;
                bus_dma_q        <= 1'b0;
                bus_bulk_q       <= 1'b0;
                bus_cancel_pending_q <= 1'b0;
                bus_recovery_q   <= 2'd0;
                bus_blocked_cause_q <= 1'b0;
                ab_write_q       <= '0;
                io_read_hold_q   <= 1'b0;
            end
        end
    end

    always_comb begin
        if (axi_read_addr_q == AXI_REG_BUS_BUFFER0) begin
            as_client.rdata = bus_buffer_q;
        end else case (axi_read_addr_q)
            AXI_REG_STATUS: as_client.rdata = {
                8'd0, toz80_q, toz80_seq_q, 4'd0,
                nmi_req_q, reset_req_q, f_6502_q, f_z80_q
            };
            AXI_REG_DEBUG: as_client.rdata = {
                12'd0, nmi_req_q, reset_req_q, f_6502_q, f_z80_q,
                toz80_q, to6502_q
            };
            AXI_REG_MODE: as_client.rdata = {31'd0, mode_q};
            AXI_REG_AD_STATUS: as_client.rdata = {
                ad_seq_q,
                ad_last_data_q,
                3'd0,
                bus_blocked,
                ad_last_port_q,
                bus_error_q,
                bus_done_q,
                bus_busy,
                ad_reset_req_q,
                ad_abort_req_q,
                ad_running_q,
                ad_flag_q,
                ad_cmd_pending_q
            };
            AXI_REG_AD_PORTS0: as_client.rdata = {
                ad_port_q[3], ad_port_q[2], ad_port_q[1], ad_port_q[0]
            };
            AXI_REG_AD_PORTS1: as_client.rdata = {
                ad_port_q[7], ad_port_q[6], ad_port_q[5], ad_port_q[4]
            };
            AXI_REG_AD_PORTS2: as_client.rdata = {
                ad_port_q[11], ad_port_q[10], ad_port_q[9], ad_port_q[8]
            };
            AXI_REG_AD_PORTS3: as_client.rdata = {
                ad_port_q[15], ad_port_q[14], ad_port_q[13], ad_port_q[12]
            };
            AXI_REG_BUS_STATUS: as_client.rdata = {
                bus_seq_q, bus_bulk_index_q, 3'd0, bus_bulk_q,
                bus_blocked | bus_blocked_cause_q,
                bus_error_q, bus_done_q, bus_busy,
                bus_rdata_q
            };
            default: as_client.rdata = 32'h0000_0000;
        endcase
    end

endmodule
