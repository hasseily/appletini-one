module apple_top(
    input clk,
    // 4-bit reset bus from proc_sys_reset_0 (C_NUM_PERP_ARESETN=8). Each bit
    // is functionally identical (same source, same deassert cycle) but the
    // placer can put each driver flop near its load cluster, breaking up
    // what would otherwise be a single high-fanout reset broadcast.
    //   [0] memory subsystem (psram_simple, psram_driver, dma_engine,
    //       cycle_capture, cycle_egress)
    //   [1] apple bus path (bus_wrapper, soft_switch_manager,
    //       timing_gen, write_arbiter)
    //   [2] card emulation (mockingboard, smartport, boot_menu, no_slot_clock)
    //   [3] axisimple shim + control register block (ps_dma_command,
    //       card_feature_enable, reset_release_ready, etc.)
    input [3:0] rstn,
    input globals::AxiSimple_common as_common,
    AxiSimple_if.client as_client,
    AxiSimple_if.client smartport_as_client,
    AxiSimple_if.client boot_menu_as_client,
    AxiSimple_if.client ps_dma_as_client,
    AxiSimple_if.client mouse_as_client,
    AxiSimple_if.client disk2_as_client,
    AxiSimple_if.client applicard_as_client,
    inout [7:0] apple_data_pin,
    inout [15:0] apple_addr_pin,
    inout apple_rw_pin,
    input apple_phi0_pin,
    input apple_m2sel_pin,
    input apple_m2b0_pin,
    inout apple_inh_pin,
    inout apple_res_pin,
    inout apple_irq_pin,
    inout apple_rdy_pin,
    inout apple_dma_pin,
    inout apple_nmi_pin,
    output reg tini_oe_pin,
    input tini_5v_pin,
    output reg tini_addr_dir_pin,
    output reg tini_data_dir_pin,
    output logic video_mode_50hz_out,
    output logic apple_vblank_start_pulse,
    output logic smartport_irq,
    output logic apple_reset_n_out,
    output logic signed [15:0] mockingboard_audio_l,
    output logic signed [15:0] mockingboard_audio_r,
    output logic signed [15:0] disk2_audio_l,
    output logic signed [15:0] disk2_audio_r,
    output logic menu_chime_start,
    input  logic audio_sample_tick,

    // HP1 AXI3 master to PS DDR (used by apple_dma_engine)
    Axi3_read_if.master  axi_hp1_read,
    Axi3_write_if.master axi_hp1_write,

    // HP2 AXI3 read master to PS DDR (dedicated to Disk II audio sample fetch)
    Axi3_read_if.master  axi_audio_read,

    // HP2 AXI3 write master to PS DDR (dedicated to the SuperDuperDisplay
    // bus-event egress; the HP2 read channel above is independent)
    Axi3_write_if.master axi_sdd_write,

    // HP0 AXI3 write master to PS DDR (used by apple_cycle_egress)
    Axi3_write_if.master axi_hp0_write,

    // HP3 AXI3 masters to PS DDR for Disk II track staging. Keeping tracks in
    // DDR leaves the complete 8 MB PSRAM address space available to RamWorks.
    Axi3_read_if.master  axi_hp3_read,
    Axi3_write_if.master axi_hp3_write,

    // Frame-render gate: 1 when current Apple video frame should be streamed
    // to the PS, 0 when the frame is being skipped.
    input  logic                           frame_en,

    // PSRAM physical pins
    output wire psram_ce_n,
    output wire psram_clk,
    output wire [3:0] psram_oe,
    output wire [3:0] psram_a_o,
    output wire [3:0] psram_b_o,
    input wire [3:0] psram_a_i,
    input wire [3:0] psram_b_i,

    // W5100S Ethernet controller parallel MCU bus
    input  wire [7:0] eth_d_i,
    output wire [7:0] eth_d_o,
    output wire       eth_d_oe,
    output wire [1:0] eth_a,
    output wire       eth_rd_n,
    output wire       eth_wr_n,
    output wire       eth_cs_n,
    output wire       eth_rst_n,
    input  wire       eth_int_n
);

    globals::AppleBus_read ab_read;
    globals::AppleBus_write ab_write;
    globals::SoftSwitchState sss;
    logic ramworks_en_q;  // RamWorks 8 MB expansion, card-control register 0x62

    soft_switch_manager ssm(
        .clk(clk),
        .rstn(rstn[1]),
        .ramworks_en(ramworks_en_q),
        .ab_read(ab_read),
        .sss(sss)
    );

    // Timing generator signals
    logic apple_bus_pulse;
    logic video_mode_50hz;
    logic set_frame_zero_pulse;
    logic set_vblank_start_pulse;
    logic apple_vblank_lock_seen_q;
    logic apple_reset_prev_q;
    logic apple_reset_release_q;
    logic update_pulse;
    logic [8:0] line_in_frame;
    logic [6:0] cycle_in_line;
    logic       video_mode_50hz_detected_q;
    logic       video_mode_50hz_valid_q;
    logic [6:0] detect_cycle_in_line;
    logic [15:0] detect_line_clk_count;
    logic mouse_vblank_start_pulse;
    localparam [15:0] APPLE_LINE_PERIOD_50HZ_THRESHOLD = 16'd8513;

    assign apple_bus_pulse = ab_read.sss_en;
    assign video_mode_50hz = video_mode_50hz_valid_q && video_mode_50hz_detected_q;
    assign video_mode_50hz_out = video_mode_50hz;
    assign mouse_vblank_start_pulse =
        update_pulse && (line_in_frame == 9'd192) && (cycle_in_line == 7'd0);
    wire apple_reset_assert_pulse = rstn[1] && apple_reset_prev_q && !ab_read.res;
    assign set_frame_zero_pulse = apple_reset_release_q;
    // The ROM's first VBL command after reset is the calibrated lock point.
    // Later commands are useful as activity pulses, but must not re-phase the
    // Apple timing counters with raw polling jitter.
    assign set_vblank_start_pulse =
        apple_vblank_start_pulse && !apple_vblank_lock_seen_q;

    localparam logic [7:0] CARD_CTRL_REG_SLOT_ENABLE_MASK   = 8'h00;
    localparam logic [7:0] CARD_CTRL_REG_FEATURE_ENABLE_MASK = 8'h01;
    localparam logic [7:0] CARD_CTRL_REG_SOFTSW_STATE        = 8'h02;
    localparam logic [7:0] CARD_CTRL_REG_RESET_RELEASE       = 8'h03;
    localparam logic [7:0] CARD_CTRL_REG_NSC_TIME_LO         = 8'h04;
    localparam logic [7:0] CARD_CTRL_REG_NSC_TIME_HI         = 8'h05;
    localparam logic [7:0] CARD_CTRL_REG_NSC_WRITE_SEQ       = 8'h06;
    localparam logic [7:0] CARD_CTRL_REG_MENU_CHIME          = 8'h07;
    localparam logic [7:0] CARD_CTRL_REG_PHASOR_PAN_LO       = 8'h08;
    localparam logic [7:0] CARD_CTRL_REG_APPLE_RESET_STATUS  = 8'h09;
    localparam logic [7:0] CARD_CTRL_REG_PHASOR_PAN_HI       = 8'h0A;
    localparam logic [7:0] CARD_CTRL_REG_PHASOR_AUDIO        = 8'h0C;
    localparam logic [7:0] CARD_CTRL_REG_DISK2_SOUND_BASE    = 8'h10;
    localparam logic [7:0] CARD_CTRL_REG_DISK2_SOUND_CONTROL = 8'h11;
    // SuperSprite (TMS9918 VDP) PS-facing readback window.
    localparam logic [7:0] CARD_CTRL_REG_SS_REGS_LO   = 8'h40; // R0..R3
    localparam logic [7:0] CARD_CTRL_REG_SS_REGS_HI   = 8'h41; // R4..R7
    localparam logic [7:0] CARD_CTRL_REG_SS_STATUS    = 8'h42; // status/frame/switches
    localparam logic [7:0] CARD_CTRL_REG_SS_VRAM_DATA = 8'h43; // read VRAM[addr]
    localparam logic [7:0] CARD_CTRL_REG_SS_VRAM_ADDR = 8'h44; // set VRAM read addr
    localparam logic [7:0] CARD_CTRL_REG_SS_SPR_FLAGS = 8'h45; // PS sprite status
    localparam logic [7:0] CARD_CTRL_REG_ETH_ADDR     = 8'h46; // W5100S host access address
    localparam logic [7:0] CARD_CTRL_REG_ETH_DATA     = 8'h47; // W5100S host access byte
    localparam logic [7:0] CARD_CTRL_REG_ETH_CMD      = 8'h48; // bit0 go, bit1 write
    localparam logic [7:0] CARD_CTRL_REG_ETH_STATUS   = 8'h49; // ready/busy/done/error + read byte
    localparam int unsigned CARD_CTRL_FEATURE_NSC_ENABLE_BIT = 0;
    localparam int unsigned CARD_CTRL_FEATURE_SS_ENABLE_BIT  = 1;
    localparam int unsigned CARD_CTRL_DISK2_SOUND_EVENT_SHIFT = 16;
    localparam logic [31:0] CARD_CTRL_DISK2_SOUND_EVENT_MASK  = 32'h000F_0000;
    localparam logic [31:0] CARD_CTRL_ETH_CMD_GO              = 32'h0000_0001;
    localparam logic [31:0] CARD_CTRL_ETH_CMD_WRITE           = 32'h0000_0002;
    localparam logic [31:0] CARD_CTRL_ETH_STATUS_READY        = 32'h0000_0001;
    localparam logic [31:0] CARD_CTRL_ETH_STATUS_BUSY         = 32'h0000_0002;
    localparam logic [31:0] CARD_CTRL_ETH_STATUS_DONE         = 32'h0000_0004;
    localparam logic [31:0] CARD_CTRL_ETH_STATUS_ERROR        = 32'h0000_0008;
    localparam logic [31:0] RESET_RELEASE_CPU0_READY         = 32'h0000_0001;
    localparam logic [31:0] RESET_RELEASE_CPU1_READY         = 32'h0000_0002;
    localparam logic [31:0] RESET_RELEASE_READY_MASK =
        RESET_RELEASE_CPU0_READY | RESET_RELEASE_CPU1_READY;
    localparam logic [31:0] CARD_CTRL_SLOT_ENABLE_RESET      = 32'h0000_0016;
    localparam logic [31:0] CARD_CTRL_SLOT_ENABLE_VALID_MASK = 32'h0000_007E;
    localparam logic [31:0] CARD_CTRL_SLOT_ENABLE_REQUIRED   = 32'h0000_0080;
    localparam logic [47:0] PHASOR_PAN_RESET                 = 48'h5B5B5B5B5B5B;
    localparam logic [31:0] PHASOR_AUDIO_RESET               = 32'h0204_0000;

    function automatic logic [31:0] card_slot_enable_normalize(input logic [31:0] value);
        card_slot_enable_normalize =
            (value & CARD_CTRL_SLOT_ENABLE_VALID_MASK) | CARD_CTRL_SLOT_ENABLE_REQUIRED;
    endfunction

    logic [31:0] card_slot_enable_mask_q =
        CARD_CTRL_SLOT_ENABLE_RESET | CARD_CTRL_SLOT_ENABLE_REQUIRED;
    logic [31:0] card_feature_enable_mask_q = 32'h0000_0000;
    logic [31:0] reset_release_ready_q = 32'h0000_0000;
    logic menu_chime_start_q = 1'b0;
    logic [47:0] phasor_pan_q = PHASOR_PAN_RESET;
    logic [31:0] phasor_audio_q = PHASOR_AUDIO_RESET;
    logic [15:0] eth_host_addr_q = 16'h0000;
    logic [7:0]  eth_host_wdata_q = 8'h00;
    logic [7:0]  eth_host_rdata_q = 8'h00;
    logic        eth_host_req_pulse = 1'b0;
    logic        eth_host_write_q = 1'b0;
    logic        eth_host_busy_q = 1'b0;
    logic        eth_host_done_q = 1'b0;
    logic        eth_host_error_q = 1'b0;
    logic        eth_host_ready;
    logic        eth_host_done;
    logic        eth_host_error;
    logic [7:0]  eth_host_rdata;
    logic [31:0] disk2_sound_sample_base_q = 32'h0000_0000;
    logic [31:0] disk2_sound_control_q = 32'h0000_0000; // bit 0 enable, [11:8] volume 0..10
    Axi3_read_if #(.ADDR_WIDTH(32), .DATA_WIDTH(64)) disk2_sound_read();
    localparam logic [63:0] NSC_TIME_RESET =
        {8'h26, 8'h01, 8'h01, 8'h01, 8'h00, 8'h00, 8'h00, 8'h00};
    logic [63:0] nsc_time_shadow_q = NSC_TIME_RESET;
    logic [63:0] nsc_time_bcd_q = NSC_TIME_RESET;
    logic [31:0] nsc_write_seq_q = 32'h0000_0000;
    logic [7:0] apple_reset_seq_q = 8'h00;
    logic [13:0] ss_vram_addr_q = 14'd0;    // PS-set VRAM read address
    logic [6:0]  ss_status_flags_q = 7'd0;  // PS-set sprite status flags
    wire card_slot1_enable = card_slot_enable_mask_q[1];
    wire card_slot2_enable = card_slot_enable_mask_q[2];
    wire card_slot4_enable = card_slot_enable_mask_q[4];
    wire card_slot5_enable = card_slot_enable_mask_q[5];
    wire card_slot6_enable = card_slot_enable_mask_q[6];
    // SuperSprite hardware and software require slot 7, which is also the
    // SmartPort slot. Enabling SuperSprite therefore gates off SmartPort, SD
    // storage, and the SmartPort GETDIB detection channel.
    wire card_supersprite_enable =
        card_feature_enable_mask_q[CARD_CTRL_FEATURE_SS_ENABLE_BIT];
    wire no_slot_clock_enabled =
        card_feature_enable_mask_q[CARD_CTRL_FEATURE_NSC_ENABLE_BIT];
    wire [7:0] no_slot_clock_slot_mask =
        (card_slot2_enable ? 8'h04 : 8'h00) |
        (card_slot4_enable ? 8'h10 : 8'h00) |
        (card_slot6_enable ? 8'h40 : 8'h00) |
        8'h80;
    wire apple_reset_release =
        (reset_release_ready_q & RESET_RELEASE_READY_MASK) == RESET_RELEASE_READY_MASK;
    assign apple_reset_n_out = apple_reset_release;
    assign menu_chime_start = menu_chime_start_q;
    logic smartport_active;
    logic disk2_active;
    logic disk2_sound_spinning;
    logic [7:0] disk2_sound_qtrack;
    logic [3:0] disk2_sound_event;
    logic [7:0] disk2_sound_seek_start_qtrack;
    logic [7:0] disk2_sound_seek_distance;
    logic [3:0] disk2_menu_sound_event_q;
    logic [2:0] boot_menu_slot;
    logic       boot_menu_slot_valid;
    wire [20:0] current_softswitch_state = {
        sss.sw_ramworks_bank,
        sss.sw_lcram_write,
        sss.sw_lcram_read,
        sss.sw_lcram_bank2,
        sss.sw_dhires,
        sss.sw_80col,
        sss.sw_altcharset,
        sss.sw_hires,
        sss.sw_page2,
        sss.sw_mixed,
        sss.sw_text,
        sss.sw_altzp,
        sss.sw_ramwrt,
        sss.sw_ramrd,
        sss.sw_80store
    };

    always_ff @(posedge clk) begin
        if (!rstn[1]) begin
            apple_reset_prev_q          <= 1'b1;
            apple_reset_release_q       <= 1'b0;
            apple_vblank_lock_seen_q    <= 1'b0;
            detect_cycle_in_line       <= 7'd0;
            detect_line_clk_count      <= 16'd0;
            video_mode_50hz_detected_q <= 1'b0;
            video_mode_50hz_valid_q    <= 1'b0;
        end else begin
            // Apple RES# is not a PSRAM coherency boundary. Cache contents
            // remain valid across CTRL+RESET.
            apple_reset_release_q     <= !apple_reset_prev_q && ab_read.res;
            apple_reset_prev_q <= ab_read.res;
            detect_line_clk_count <= detect_line_clk_count + 16'd1;

            if (!ab_read.res) begin
                apple_vblank_lock_seen_q <= 1'b0;
            end else if (set_vblank_start_pulse) begin
                apple_vblank_lock_seen_q <= 1'b1;
            end

            if (apple_bus_pulse) begin
                if (detect_cycle_in_line == 7'd64) begin
                    detect_cycle_in_line       <= 7'd0;
                    detect_line_clk_count      <= 16'd0;
                    video_mode_50hz_detected_q <=
                        ((detect_line_clk_count + 16'd1) >= APPLE_LINE_PERIOD_50HZ_THRESHOLD);
                    video_mode_50hz_valid_q    <= 1'b1;
                end else begin
                    detect_cycle_in_line <= detect_cycle_in_line + 7'd1;
                end
            end
        end
    end

    apple_timing_gen apple_timing_gen_i (
        .clk(clk),
        .resetn(rstn[1]),
        .apple_bus_pulse(apple_bus_pulse),
        .video_mode_50hz(video_mode_50hz),
        .update_pulse(update_pulse),
        .line_in_frame(line_in_frame),
        .cycle_in_line(cycle_in_line),
        .set_frame_zero_pulse(set_frame_zero_pulse),
        .set_vblank_start_pulse(set_vblank_start_pulse)
    );

    /* psram_simple owns the physical PSRAM command interface. */
    logic        mc_psram_valid;
    logic        mc_psram_ready;
    logic [7:0]  mc_psram_cmd;
    logic [23:0] mc_psram_addr;
    logic [63:0] mc_psram_wdata;
    logic        mc_psram_rvalid;
    logic [63:0] mc_psram_rdata;

    // PS DMA line interface, driven by apple_dma_engine.
    logic [20:0] mc_dma_line_addr;
    logic        mc_dma_rw;
    logic [63:0] mc_dma_wdata;
    logic        mc_dma_valid;
    logic        mc_dma_ready;
    logic [63:0] mc_dma_rdata;
    logic        mc_dma_rvalid;
    logic [20:0] mc_disk2_line_addr;
    logic        mc_disk2_rw;
    logic [63:0] mc_disk2_wdata;
    logic [7:0]  mc_disk2_wstrb;
    logic        mc_disk2_valid;
    logic        mc_disk2_ready;
    logic [63:0] mc_disk2_rdata;
    logic        mc_disk2_rvalid;

    // ------------------------------------------------------------------
    // Apple bus cycle capture + PS DDR egress (stages 1 & 2a).
    // capture FIFO is consumed internally by egress; no top-level FIFO ports.
    // ------------------------------------------------------------------
    apple_cycle_capture_pkg::AppleCycleRecord cycle_capture_data_internal;
    logic                                     cycle_capture_empty_internal;
    logic                                     cycle_capture_rd_en_internal;
    logic                                     capture_drop_sticky_internal;

    logic        egress_cfg_enable_q;
    logic [31:0] egress_cfg_ring_base_q;
    logic [4:0]  egress_cfg_ring_size_log2_q;
    logic [31:0] egress_cfg_producer_ptr_addr_q;
    logic [31:0] egress_cfg_consumer_ptr_q;
    logic        egress_cfg_reset_pulse;

    logic [31:0] egress_stat_producer_ptr;
    logic [31:0] egress_stat_records_written;
    logic [31:0] egress_stat_gap_markers;
    logic [31:0] egress_stat_bursts_issued;
    logic [31:0] egress_stat_full_stall_cycles;

    logic        egress_capture_drop_ack;

    apple_cycle_capture apple_cycle_capture_i (
        .clk(clk),
        .resetn(rstn[0]),
        .soft_reset(!ab_read.res),
        .ab_read(ab_read),
        .sss(sss),
        .line_in_frame(line_in_frame),
        .cycle_in_line(cycle_in_line),
        .frame_en(frame_en),
        .cycle_capture_data(cycle_capture_data_internal),
        .cycle_capture_rd_en(cycle_capture_rd_en_internal),
        .cycle_capture_empty(cycle_capture_empty_internal),
        .capture_drop_sticky(capture_drop_sticky_internal),
        .capture_drop_ack(egress_capture_drop_ack)
    );

    apple_cycle_egress apple_cycle_egress_i (
        .clk                       (clk),
        .resetn                    (rstn[0] && !egress_cfg_reset_pulse),
        .cycle_capture_data        (cycle_capture_data_internal),
        .cycle_capture_empty       (cycle_capture_empty_internal),
        .cycle_capture_rd_en       (cycle_capture_rd_en_internal),
        .capture_drop_sticky       (capture_drop_sticky_internal),
        .capture_drop_ack          (egress_capture_drop_ack),
        .cfg_enable                (egress_cfg_enable_q),
        .cfg_ring_base_addr        (egress_cfg_ring_base_q),
        .cfg_ring_size_log2        (egress_cfg_ring_size_log2_q),
        .cfg_producer_ptr_addr     (egress_cfg_producer_ptr_addr_q),
        .cfg_consumer_ptr          (egress_cfg_consumer_ptr_q),
        .stat_producer_ptr         (egress_stat_producer_ptr),
        .stat_records_written      (egress_stat_records_written),
        .stat_gap_markers          (egress_stat_gap_markers),
        .stat_bursts_issued        (egress_stat_bursts_issued),
        .stat_full_stall_cycles    (egress_stat_full_stall_cycles),
        .axi_hp0_write             (axi_hp0_write)
    );

    // ------------------------------------------------------------------
    // SuperDuperDisplay full-rate bus-event tap + dedicated egress.
    // Independent of the renderer capture path: its own FIFO, its own
    // DDR ring, its own AXI port (HP2 write). Enabled only while the PS
    // SDD streaming service is active.
    // ------------------------------------------------------------------
    apple_cycle_capture_pkg::AppleCycleRecord sdd_tap_data_internal;
    logic                                     sdd_tap_empty_internal;
    logic                                     sdd_tap_rd_en_internal;
    logic                                     sdd_tap_drop_sticky_internal;
    logic                                     sdd_egress_drop_ack;

    logic        sdd_cfg_enable_q;
    logic [31:0] sdd_cfg_ring_base_q;
    logic [4:0]  sdd_cfg_ring_size_log2_q;
    logic [31:0] sdd_cfg_producer_ptr_addr_q;
    logic [31:0] sdd_cfg_consumer_ptr_q;
    logic        sdd_cfg_reset_pulse;

    /* Machine mode reported by the boot ROM: 0=unknown, 1=II/II+,
     * 2=IIe, 3=IIgs. Unknown and IIgs modes prohibit INH and DMA; the PS
     * must positively identify a compatible machine before either signal
     * can be driven. */
    logic [1:0]  machine_mode_q;
    /* AUX_PROVIDE: card serves the aux 64K + RamWorks banks from PSRAM
     * (//e only -- the arbiter INH interlock enforces the machine
     * gate). Reset = 0: snoop-only, GS-safe. */
    logic        aux_provide_en_q;
    /* PSRAM read-capture delay: [4:0] delay taps, [5] sample edge.
     * Writing the register pulses the driver's load strobe. */
    logic [4:0]  psram_dcount_q;
    logic        psram_dcount_edge_q;
    logic        psram_dcount_wr_pulse_q;
    logic        machine_inh_allowed;
    logic        machine_m2sel_active_high;
    logic        machine_gs_m2_qualify;
    assign machine_inh_allowed = (machine_mode_q == 2'd1) ||
                                 (machine_mode_q == 2'd2);
    /* M2SEL qualification arms only on a POSITIVE GS identification.
     * It must NOT apply in UNKNOWN mode: the //e's level on the M2SEL
     * pin is not guaranteed to read "asserted", and qualifying before
     * identification could invalidate every cycle -- the boot ROM
     * could then never serve, the machine ID could never be reported,
     * and the card would fail to boot on supported Apple II/IIe hosts.
     * Pre-ID exposure on a GS is the few milliseconds of its own ROM
     * slot scan; the INH/DMA interlock covers the serving hazards in
     * that window. */
    assign machine_gs_m2_qualify = (machine_mode_q == 2'd3);

    logic [31:0] sdd_stat_producer_ptr;
    logic [31:0] sdd_stat_records_written;
    logic [31:0] sdd_stat_gap_markers;
    logic [31:0] sdd_stat_bursts_issued;
    logic [31:0] sdd_stat_full_stall_cycles;

    sdd_bus_tap sdd_bus_tap_i (
        .clk                      (clk),
        .resetn                   (rstn[0]),
        .enable                   (sdd_cfg_enable_q),
        .ab_read                  (ab_read),
        .route_info               ({
            (sss.route_kind == globals::APPLE_ROUTE_CACHE) && sss.addr_decode_en,
            sss.addr_decode[23:16] != 8'd0,
            sss.route_kind == globals::APPLE_ROUTE_ROM
        }),
        .cycle_capture_data       (sdd_tap_data_internal),
        .cycle_capture_empty      (sdd_tap_empty_internal),
        .cycle_capture_rd_en      (sdd_tap_rd_en_internal),
        .capture_drop_sticky      (sdd_tap_drop_sticky_internal),
        .capture_drop_ack         (sdd_egress_drop_ack)
    );

    apple_cycle_egress sdd_cycle_egress_i (
        .clk                       (clk),
        .resetn                    (rstn[0] && !sdd_cfg_reset_pulse),
        .cycle_capture_data        (sdd_tap_data_internal),
        .cycle_capture_empty       (sdd_tap_empty_internal),
        .cycle_capture_rd_en       (sdd_tap_rd_en_internal),
        .capture_drop_sticky       (sdd_tap_drop_sticky_internal),
        .capture_drop_ack          (sdd_egress_drop_ack),
        .cfg_enable                (sdd_cfg_enable_q),
        .cfg_ring_base_addr        (sdd_cfg_ring_base_q),
        .cfg_ring_size_log2        (sdd_cfg_ring_size_log2_q),
        .cfg_producer_ptr_addr     (sdd_cfg_producer_ptr_addr_q),
        .cfg_consumer_ptr          (sdd_cfg_consumer_ptr_q),
        .stat_producer_ptr         (sdd_stat_producer_ptr),
        .stat_records_written      (sdd_stat_records_written),
        .stat_gap_markers          (sdd_stat_gap_markers),
        .stat_bursts_issued        (sdd_stat_bursts_issued),
        .stat_full_stall_cycles    (sdd_stat_full_stall_cycles),
        .axi_hp0_write             (axi_sdd_write)
    );

    logic [31:0] dbg_apple_access_count;
    logic [31:0] dbg_apple_miss_count;
    logic [31:0] dbg_aux_write_count;
    logic [31:0] dbg_dma_admit_count;
    logic [31:0] dbg_dma_complete_count;
    logic [31:0] dbg_serve_late_count;
    logic [31:0] dbg_serve_max_latency;
    logic [31:0] dbg_wq_drop_count;
    logic [15:0] dbg_wq_state;
    logic [23:0] dbg_miss_ctx;
    logic        res_filtered_dbg;
    logic [31:0] dbg_lost_cycle_count;

    // Physical PSRAM command/data driver.
    psram_driver psram_driver_i (
        .clk(clk),
        .resetn(rstn[0]),
        .valid(mc_psram_valid),
        .ready(mc_psram_ready),
        .cmd(mc_psram_cmd),
        .addr(mc_psram_addr),
        .wdata(mc_psram_wdata),
        .rvalid(mc_psram_rvalid),
        .rdata(mc_psram_rdata),
        .done(),
        /* The PS boot-time eye scan programs CARD_CTRL 0x63 to center
         * the read-capture point across board and temperature variation. */
        .dcount_wr_en(psram_dcount_wr_pulse_q),
        .dcount_wr(psram_dcount_q),
        .dcount_edge(psram_dcount_edge_q),
        .psram_oe(psram_oe),
        .psram_a_i(psram_a_i),
        .psram_a_o(psram_a_o),
        .psram_b_i(psram_b_i),
        .psram_b_o(psram_b_o),
        .psram_ce_n(psram_ce_n),
        .psram_clk(psram_clk)
    );

    // individual writer interfaces that get arbited into the mux
    globals::AppleBus_write brain_transplant_write;
    globals::AppleBus_write mb1_ab_write;
    globals::AppleBus_write mouse_ab_write;
    globals::AppleBus_write applicard_ab_write;
    globals::AppleBus_write uthernet_ab_write;
    globals::AppleBus_write vidhd_ab_write;
    globals::AppleBus_write disk2_ab_write;
    globals::AppleBus_write smartport_ab_write;
    globals::AppleBus_write boot_menu_ab_write;
    globals::AppleBus_write no_slot_clock_ab_write;
    globals::AppleBus_write supersprite_ab_write;
    logic signed [15:0] ss_psg_audio;   // SuperSprite AY-3-8910 output
    logic signed [15:0] mb1_audio_l;    // mockingboard pre-mix (SS PSG summed in)
    logic signed [15:0] mb1_audio_r;
    // SuperSprite PS-facing readback (driven by supersprite_card_i).
    logic [63:0] ss_regs;
    logic [7:0]  ss_status;
    logic [15:0] ss_frame;
    logic        ss_apple_video;
    logic        ss_vdp_overlay;
    logic [7:0]  ss_vram_data;
    globals::AppleBus_write ab_write_arb;
    logic [63:0] nsc_write_time_bcd;
    logic        nsc_write_time_strobe;

    apple_bus_wrapper apple_bus_wrapper_i (
        .res_filtered_out(res_filtered_dbg),
        .dbg_lost_cycle_count(dbg_lost_cycle_count),
        .clk(clk),
        .rstn(rstn[1]),
        .inh_allowed(machine_inh_allowed),
        .gs_m2_qualify(machine_gs_m2_qualify),
        .m2sel_active_high(machine_m2sel_active_high),
        .apple_data_pin(apple_data_pin),
        .apple_addr_pin(apple_addr_pin),
        .apple_rw_pin(apple_rw_pin),
        .apple_phi0_pin(apple_phi0_pin),
        .apple_m2sel_pin(apple_m2sel_pin),
        .apple_m2b0_pin(apple_m2b0_pin),
        .apple_inh_pin(apple_inh_pin),
        .apple_res_pin(apple_res_pin),
        .apple_irq_pin(apple_irq_pin),
        .apple_rdy_pin(apple_rdy_pin),
        .apple_dma_pin(apple_dma_pin),
        .apple_nmi_pin(apple_nmi_pin),
        .tini_oe_pin(tini_oe_pin),
        .tini_5v_pin(tini_5v_pin), // hardcoded to 0 (active low)
        .tini_addr_dir_pin(tini_addr_dir_pin),
        .tini_data_dir_pin(tini_data_dir_pin),
        .ab_read(ab_read),
        .ab_write(ab_write)
    );

    /* PSRAM serves aux/RamWorks through the //e-only INH interlock and
     * provides the PS DMA staging port. Main RAM, language-card RAM, and
     * Disk II track staging are owned by other memory paths. */
    psram_simple psram_simple_i (
        .clk(clk),
        .resetn(rstn[0]),
        .ab_read(ab_read),
        .sss(sss),
        .aux_provide_en(aux_provide_en_q),
        .ab_write(brain_transplant_write),
        .dma_line_addr(mc_dma_line_addr),
        .dma_rw(mc_dma_rw),
        .dma_wdata(mc_dma_wdata),
        .dma_valid(mc_dma_valid),
        .dma_ready(mc_dma_ready),
        .dma_rdata(mc_dma_rdata),
        .dma_rvalid(mc_dma_rvalid),
        .psram_valid(mc_psram_valid),
        .psram_ready(mc_psram_ready),
        .psram_cmd(mc_psram_cmd),
        .psram_addr(mc_psram_addr),
        .psram_wdata(mc_psram_wdata),
        .psram_rvalid(mc_psram_rvalid),
        .psram_rdata(mc_psram_rdata),
        .dbg_aux_read_count(dbg_apple_access_count),
        .dbg_aux_write_count(dbg_aux_write_count),
        .dbg_deadline_miss_count(dbg_apple_miss_count),
        .dbg_dma_admit_count(dbg_dma_admit_count),
        .dbg_dma_complete_count(dbg_dma_complete_count),
        .dbg_serve_late_count(dbg_serve_late_count),
        .dbg_serve_max_latency(dbg_serve_max_latency),
        .dbg_wq_drop_count(dbg_wq_drop_count),
        .dbg_wq_state(dbg_wq_state),
        .dbg_miss_ctx(dbg_miss_ctx)
    );
    /* Disk II line staging bridge over HP3 DDR. */
    disk2_ddr_bridge disk2_ddr_bridge_i (
        .clk(clk),
        .rstn(rstn[0]),
        .mc_line_addr(mc_disk2_line_addr),
        .mc_rw(mc_disk2_rw),
        .mc_wdata(mc_disk2_wdata),
        .mc_wstrb(mc_disk2_wstrb),
        .mc_valid(mc_disk2_valid),
        .mc_ready(mc_disk2_ready),
        .mc_rdata(mc_disk2_rdata),
        .mc_rvalid(mc_disk2_rvalid),
        .axi_read(axi_hp3_read),
        .axi_write(axi_hp3_write)
    );
    /* Reset forensics: no-reset sticky flops that latch any post-arm
     * dip of the replicated rstn bits or an Apple-reset assertion
     * (post-filter). Writing CARD_CTRL 0x64 clears/arms; reading it
     * returns {res_seen, rstn_dips[3:0]}. Deliberately never reset --
     * they must survive the very events they record. */
    logic [3:0]  rstn_dip_sticky = 4'b0;
    logic        res_seen_sticky = 1'b0;
    logic        forensics_clear_pulse;
    always_ff @(posedge clk) begin
        if (forensics_clear_pulse) begin
            rstn_dip_sticky <= 4'b0;
            res_seen_sticky <= 1'b0;
        end else begin
            rstn_dip_sticky <= rstn_dip_sticky | ~rstn;
            if (!res_filtered_dbg) begin
                res_seen_sticky <= 1'b1;
            end
        end
    end
    /* Disable a card by suppressing its phase strobes. Assigning slot 0 is
     * invalid because its slot-I/O decode aliases the $C080-$C08F language-
     * card switches and can create phantom device traffic. */
    function automatic globals::AppleBus_read gate_ab(
        input globals::AppleBus_read ab,
        input logic en
    );
        globals::AppleBus_read g;
        g = ab;
        if (!en) begin
            g.addr_en       = 1'b0;
            g.data_en       = 1'b0;
            g.sss_en        = 1'b0;
        end
        return g;
    endfunction

    localparam logic [2:0] MB1_SLOT_ASSIGN = 3'h4;

    mockingboard mb1(
        .clk(clk),
        .rstn(rstn[2]),
        .slot_assign(MB1_SLOT_ASSIGN),
        .pan(phasor_pan_q),
        .audio_control(phasor_audio_q),
        .audio_sample_tick(audio_sample_tick),
        .sss(sss),
        .ab_read(gate_ab(ab_read, card_slot4_enable)),
        .ab_write(mb1_ab_write),
        .audio_l(mb1_audio_l),
        .audio_r(mb1_audio_r)
    );

    // Sum the SuperSprite PSG into the card-audio bus with saturation. The PSG
    // output is pre-scaled with headroom; halve it so a full Mockingboard mix
    // plus PSG cannot wrap.
    wire signed [16:0] ss_mix_l = mb1_audio_l + (ss_psg_audio >>> 1);
    wire signed [16:0] ss_mix_r = mb1_audio_r + (ss_psg_audio >>> 1);
    assign mockingboard_audio_l =
        (ss_mix_l >  17'sd32767) ?  16'sd32767 :
        (ss_mix_l < -17'sd32768) ? -16'sd32768 : ss_mix_l[15:0];
    assign mockingboard_audio_r =
        (ss_mix_r >  17'sd32767) ?  16'sd32767 :
        (ss_mix_r < -17'sd32768) ? -16'sd32768 : ss_mix_r[15:0];

    mouse_card mouse_card_i (
        .clk(clk),
        .rstn(rstn[2]),
        .vblank_start_pulse(mouse_vblank_start_pulse),
        .ab_read(gate_ab(ab_read, card_slot2_enable)),
        .sss(sss),
        .slot_assign(3'h2),
        .as_common(as_common),
        .as_client(mouse_as_client),
        .ab_write(mouse_ab_write)
    );

    // PCPI Appli-Card (Z80 coprocessor) -- PL latches/flags for the PS
    // software Z80 in applicard_service.c. Slot 5 only ($C0D0-$C0DF).
    applicard_card applicard_card_i (
        .clk(clk),
        .rstn(rstn[2]),
        .ab_read(gate_ab(ab_read, card_slot5_enable)),
        .slot_assign(3'h5),
        .as_common(as_common),
        .as_client(applicard_as_client),
        .ab_write(applicard_ab_write)
    );

    uthernet2_card uthernet2_card_i (
        .clk(clk),
        .rstn(rstn[2]),
        .ab_read(gate_ab(ab_read, card_slot1_enable)),
        .sss(sss),
        .slot_assign(3'h1),
        .ab_write(uthernet_ab_write),
        .eth_d_i(eth_d_i),
        .eth_d_o(eth_d_o),
        .eth_d_oe(eth_d_oe),
        .eth_a(eth_a),
        .eth_rd_n(eth_rd_n),
        .eth_wr_n(eth_wr_n),
        .eth_cs_n(eth_cs_n),
        .eth_rst_n(eth_rst_n),
        .eth_int_n(eth_int_n),
        .host_req(eth_host_req_pulse),
        .host_write(eth_host_write_q),
        .host_addr(eth_host_addr_q),
        .host_wdata(eth_host_wdata_q),
        .host_ready(eth_host_ready),
        .host_done(eth_host_done),
        .host_error(eth_host_error),
        .host_rdata(eth_host_rdata)
    );

    vidhd_card vidhd_card_i (
        .clk(clk),
        .rstn(rstn[2]),
        .ab_read(ab_read),
        .sss(sss),
        .slot_assign(3'h3),
        .ab_write(vidhd_ab_write)
    );

    // SuperSprite (TMS9918 VDP) -- PL front end for the PS software VDP.
    // Registers/VRAM/status live here; the PS renders the picture and the
    // compositor overlays it. vblank_tick reuses the Apple frame pulse.
    supersprite_card supersprite_card_i (
        .clk(clk),
        .rstn(rstn[2]),
        .ab_read(gate_ab(ab_read, card_supersprite_enable)),
        .sss(sss),
        .slot_assign(3'h7),
        .vblank_tick(apple_vblank_start_pulse),
        .ps_status_flags(ss_status_flags_q),
        .ab_write(supersprite_ab_write),
        .irq_n(),                       // aggregated via ab_write.assert_irq
        .ssp_audio(ss_psg_audio),
        .ps_vram_addr(ss_vram_addr_q),
        .ps_vram_data(ss_vram_data),
        .ps_regs(ss_regs),
        .ps_status(ss_status),
        .ps_frame(ss_frame),
        .ps_status_read(),
        .ps_apple_video(ss_apple_video),
        .ps_vdp_overlay(ss_vdp_overlay)
    );

    disk2_card disk2_card_i (
        .clk(clk),
        .rstn(rstn[2]),
        .ab_read(gate_ab(ab_read, card_slot6_enable && disk2_active)),
        .sss(sss),
        .slot_assign(3'h6),
        .as_common(as_common),
        .as_client(disk2_as_client),
        .mc_line_addr(mc_disk2_line_addr),
        .mc_rw(mc_disk2_rw),
        .mc_wdata(mc_disk2_wdata),
        .mc_wstrb(mc_disk2_wstrb),
        .mc_valid(mc_disk2_valid),
        .mc_ready(mc_disk2_ready),
        .mc_rdata(mc_disk2_rdata),
        .mc_rvalid(mc_disk2_rvalid),
        .ab_write(disk2_ab_write),
        .sound_spinning(disk2_sound_spinning),
        .sound_qtrack(disk2_sound_qtrack),
        .sound_event(disk2_sound_event),
        .sound_seek_start_qtrack(disk2_sound_seek_start_qtrack),
        .sound_seek_distance(disk2_sound_seek_distance)
    );

    wire [3:0] disk2_player_sound_event =
        (disk2_menu_sound_event_q != 4'd0) ? disk2_menu_sound_event_q : disk2_sound_event;
    wire [7:0] disk2_player_seek_start_qtrack =
        (disk2_menu_sound_event_q != 4'd0) ? 8'd0 : disk2_sound_seek_start_qtrack;
    wire [7:0] disk2_player_seek_distance = disk2_sound_seek_distance;

    disk2_sound_player disk2_sound_player_i (
        .clk(clk),
        .rstn(rstn[2]),
        .enable(disk2_sound_control_q[0]),
        .volume(disk2_sound_control_q[11:8]),
        .audio_tick(audio_sample_tick),
        .sample_base_addr(disk2_sound_sample_base_q),
        .drive_spinning(disk2_sound_spinning),
        .qtrack(disk2_sound_qtrack),
        .sound_event(disk2_player_sound_event),
        .seek_start_qtrack(disk2_player_seek_start_qtrack),
        .seek_distance(disk2_player_seek_distance),
        .sample_read(disk2_sound_read),
        .audio_l(disk2_audio_l),
        .audio_r(disk2_audio_r)
    );

    assign axi_audio_read.araddr = disk2_sound_read.araddr;
    assign axi_audio_read.arlen = disk2_sound_read.arlen;
    assign axi_audio_read.arsize = disk2_sound_read.arsize;
    assign axi_audio_read.arburst = disk2_sound_read.arburst;
    assign axi_audio_read.arvalid = disk2_sound_read.arvalid;
    assign disk2_sound_read.arready = axi_audio_read.arready;
    assign disk2_sound_read.rdata = axi_audio_read.rdata;
    assign disk2_sound_read.rresp = axi_audio_read.rresp;
    assign disk2_sound_read.rlast = axi_audio_read.rlast;
    assign disk2_sound_read.rvalid = axi_audio_read.rvalid;
    assign axi_audio_read.rready = disk2_sound_read.rready;

    smartport_card smartport_card_i (
        .clk(clk),
        .rstn(rstn[2]),
        .ab_read(gate_ab(ab_read,
                         smartport_active && !card_supersprite_enable)),
        .sss(sss),
        // SuperSprite wins the shared slot when enabled.
        .slot_assign(3'h7),
        .as_common(as_common),
        .as_client(smartport_as_client),
        .ab_write(smartport_ab_write),
        .smartport_irq(smartport_irq)
    );

    // PS-driven DMA command interface to apple_dma_engine.
    logic [23:0] dma_req_mc_addr;
    logic [31:0] dma_req_ddr_addr;
    logic [15:0] dma_req_length;
    logic        dma_req_rw;
    logic        dma_req_valid;
    logic        dma_req_ready;
    logic        dma_req_done;

    ps_dma_command ps_dma_command_i (
        .clk(clk),
        .rstn(rstn[3]),
        .as_common(as_common),
        .as_client(ps_dma_as_client),
        .dma_req_mc_addr(dma_req_mc_addr),
        .dma_req_ddr_addr(dma_req_ddr_addr),
        .dma_req_length(dma_req_length),
        .dma_req_rw(dma_req_rw),
        .dma_req_valid(dma_req_valid),
        .dma_req_ready(dma_req_ready),
        .dma_req_done(dma_req_done)
    );

    // smartport_card splits any transfer at 256-byte page boundaries
    // (and at LC-window remap edges), and the SP_STATUS payloads are
    // tiny, so req_length is bounded well below 1024 bytes. A 10-bit
    // engine shaves ~6 bits off every counter / carry chain inside
    // apple_dma_engine, which is what closes timing at 133 MHz.
    apple_dma_engine #(.LENGTH_W(10)) apple_dma_engine_i (
        .clk(clk),
        .rstn(rstn[0]),
        .req_mc_addr(dma_req_mc_addr),
        .req_ddr_addr(dma_req_ddr_addr),
        .req_length(dma_req_length[9:0]),
        .req_rw(dma_req_rw),
        .req_valid(dma_req_valid),
        .req_ready(dma_req_ready),
        .req_done(dma_req_done),
        .dma_line_addr(mc_dma_line_addr),
        .dma_rw(mc_dma_rw),
        .dma_wdata(mc_dma_wdata),
        .dma_valid(mc_dma_valid),
        .dma_ready(mc_dma_ready),
        .dma_rdata(mc_dma_rdata),
        .dma_rvalid(mc_dma_rvalid),
        .axi_hp1_read(axi_hp1_read),
        .axi_hp1_write(axi_hp1_write)
    );

    logic       bm_aux_probe_pulse;
    logic [1:0] bm_aux_status;
    logic       bm_aux_status_clear;

    boot_menu_card boot_menu_card_i (
        .clk(clk),
        .rstn(rstn[2]),
        .ab_read(ab_read),
        .sss(sss),
        .disk2_enabled(card_slot6_enable),
        .apple_video_mode_valid(video_mode_50hz_valid_q),
        .apple_video_mode_50hz(video_mode_50hz),
        .as_common(as_common),
        .as_client(boot_menu_as_client),
        .ab_write(boot_menu_ab_write),
        .smartport_active(smartport_active),
        .disk2_active(disk2_active),
        .boot_slot(boot_menu_slot),
        .boot_slot_valid(boot_menu_slot_valid),
        .apple_vblank_start_pulse(apple_vblank_start_pulse),
        .aux_probe_pulse(bm_aux_probe_pulse),
        .aux_status(bm_aux_status),
        .aux_status_clear(bm_aux_status_clear)
    );

    no_slot_clock no_slot_clock_i (
        .clk(clk),
        .rstn(rstn[2]),
        .enabled(no_slot_clock_enabled),
        .slot_mask(no_slot_clock_slot_mask),
        .time_bcd(nsc_time_bcd_q),
        .ab_read(ab_read),
        .sss(sss),
        .write_time_bcd(nsc_write_time_bcd),
        .write_time_strobe(nsc_write_time_strobe),
        .ab_write(no_slot_clock_ab_write)
    );

    // apple_bus_write_arbiter merges virtual-card responses and control-line
    // requests before they reach apple_bus_wrapper.
    apple_bus_write_arbiter #(.NUM_CLIENTS(11))
    apple_bus_write_arbiter_i(
        .inh_allowed(machine_inh_allowed),
        .client_writes({
            brain_transplant_write,
            mb1_ab_write,
            mouse_ab_write,
            applicard_ab_write,
            uthernet_ab_write,
            vidhd_ab_write,
            supersprite_ab_write,
            disk2_ab_write,
            smartport_ab_write,
            boot_menu_ab_write,
            no_slot_clock_ab_write
        }),
        .ab_write(ab_write_arb)
    );

    // Virtual-card bus requests feed the physical bus wrapper directly.
    assign ab_write = ab_write_arb;

    // AxiSimple card-control and diagnostic register mux.
    //   araddr 0x13: aux/RamWorks reads
    //   araddr 0x14: aux/RamWorks read deadline misses
    //   araddr 0x15: PS DMA requests admitted
    //   araddr 0x16: PS DMA requests completed
    //   araddr 0x17: aux/RamWorks writes
    //   araddr 0x18: late PSRAM serves
    //   araddr 0x1D: maximum PSRAM serve latency
    // Card control registers:
    //   araddr/awaddr 0x00: slot enable mask, bits 1..7 map to Apple slots
    //   araddr/awaddr 0x01: feature enable mask
    //                         bit 0 = DS1216E no-slot clock under this ROM
    //   araddr 0x02: current soft-switch snapshot from soft_switch_manager
    //   awaddr 0x03: reset release latch:
    //                   bit 0 = CPU0/frontend ready
    //                   bit 1 = CPU1/renderer ready
    //                   Apple RESET# releases when both are set
    //   araddr/awaddr 0x04: no-slot clock BCD time bits 31..0
    //   araddr/awaddr 0x05: no-slot clock BCD time bits 63..32
    //                         write low first, then high; high commits.
    //   araddr 0x06: no-slot clock Apple-write sequence counter
    //   awaddr 0x07: menu chime trigger, bit 0 starts the fixed PL chime.
    //   araddr/awaddr 0x08: Phasor pan low, AY0/AY1 six 4-bit values.
    //   araddr 0x09: Apple reset status, bits 7..0 reset assert sequence,
    //                 bit 8 = sampled Apple RES# level.
    //   araddr/awaddr 0x0A: Phasor pan high, AY2/AY3 six 4-bit values.
    //   araddr/awaddr 0x0C: Phasor audio controls, signed 5-bit fields:
    //                         bass, mid, treble, warmth, volume;
    //                         bit 25 selects PSG volume table (0=YM, 1=AY).
    //   araddr/awaddr 0x10: Disk II mechanical sound sample table base address.
    //   araddr/awaddr 0x11: Disk II mechanical sound control, bit 0 enables,
    //                         [11:8] volume, write-only [19:16] menu event.
    //
    // rdata MUST be registered (not always_comb): the axidouble crossbar's
    // addrdecode is OPT_REGISTERED=1 without OPT_LOWPOWER, so it advances
    // o_addr to next_araddr the cycle after a read fires. axidouble samples
    // M_AXI_RDATA in that already-advanced cycle, so a combinational slave
    // returns the next register's value (off-by-one shift). Registering the
    // mux adds the matching one-cycle latency.
    logic [31:0] as_client_rdata_q;
    always_ff @(posedge clk) begin
        if (!rstn[3]) begin
            card_slot_enable_mask_q <= card_slot_enable_normalize(CARD_CTRL_SLOT_ENABLE_RESET);
            card_feature_enable_mask_q <= 32'h0000_0000;
            reset_release_ready_q <= 32'h0000_0000;
            menu_chime_start_q <= 1'b0;
            phasor_pan_q <= PHASOR_PAN_RESET;
            phasor_audio_q <= PHASOR_AUDIO_RESET;
            eth_host_addr_q <= 16'h0000;
            eth_host_wdata_q <= 8'h00;
            eth_host_rdata_q <= 8'h00;
            eth_host_req_pulse <= 1'b0;
            eth_host_write_q <= 1'b0;
            eth_host_busy_q <= 1'b0;
            eth_host_done_q <= 1'b0;
            eth_host_error_q <= 1'b0;
            disk2_sound_sample_base_q <= 32'h0000_0000;
            disk2_sound_control_q <= 32'h0000_0000;
            disk2_menu_sound_event_q <= 4'd0;
            nsc_time_shadow_q <= NSC_TIME_RESET;
            nsc_time_bcd_q <= NSC_TIME_RESET;
            nsc_write_seq_q <= 32'h0000_0000;
            apple_reset_seq_q <= 8'h00;
            ss_vram_addr_q <= 14'd0;
            ss_status_flags_q <= 7'd0;
            as_client_rdata_q <= 32'h0000_0000;
            egress_cfg_enable_q             <= 1'b0;
            egress_cfg_ring_base_q          <= 32'h0;
            egress_cfg_ring_size_log2_q     <= 5'd16; // 64 KB default
            egress_cfg_producer_ptr_addr_q  <= 32'h0;
            egress_cfg_consumer_ptr_q       <= 32'h0;
            egress_cfg_reset_pulse          <= 1'b0;
            sdd_cfg_enable_q                <= 1'b0;
            sdd_cfg_ring_base_q             <= 32'h0;
            sdd_cfg_ring_size_log2_q        <= 5'd17; // 128 KB default
            sdd_cfg_producer_ptr_addr_q     <= 32'h0;
            sdd_cfg_consumer_ptr_q          <= 32'h0;
            sdd_cfg_reset_pulse             <= 1'b0;
            machine_mode_q                  <= 2'd0;
            machine_m2sel_active_high       <= 1'b0;
            aux_provide_en_q                <= 1'b0;
            psram_dcount_q                  <= 5'd0;
            ramworks_en_q                   <= 1'b0;
            psram_dcount_edge_q             <= 1'b0;
            psram_dcount_wr_pulse_q         <= 1'b0;
        end else begin
            // cfg_reset_pulse is a one-cycle pulse: cleared every cycle and
            // set only when an awvalid lands on 8'h25.
            egress_cfg_reset_pulse <= 1'b0;
            sdd_cfg_reset_pulse <= 1'b0;
            psram_dcount_wr_pulse_q <= 1'b0;
            forensics_clear_pulse <= 1'b0;
            bm_aux_status_clear <= 1'b0;
            menu_chime_start_q <= 1'b0;
            disk2_menu_sound_event_q <= 4'd0;
            eth_host_req_pulse <= 1'b0;

            if (eth_host_done) begin
                eth_host_rdata_q <= eth_host_rdata;
                eth_host_busy_q <= 1'b0;
                eth_host_done_q <= 1'b1;
                eth_host_error_q <= eth_host_error_q | eth_host_error;
            end

            if (apple_reset_assert_pulse) begin
                apple_reset_seq_q <= apple_reset_seq_q + 8'd1;
            end

            if (as_client.awvalid) begin
                case (as_common.awaddr)
                    CARD_CTRL_REG_SLOT_ENABLE_MASK: begin
                        card_slot_enable_mask_q <= card_slot_enable_normalize(
                            globals::apply_wstrb(
                                card_slot_enable_mask_q,
                                as_common.wdata,
                                as_common.wstrb
                            )
                        );
                    end
                    CARD_CTRL_REG_FEATURE_ENABLE_MASK: begin
                        card_feature_enable_mask_q <= globals::apply_wstrb(
                            card_feature_enable_mask_q,
                            as_common.wdata,
                            as_common.wstrb
                        );
                    end
                    CARD_CTRL_REG_RESET_RELEASE: begin
                        automatic logic [31:0] tmp = globals::apply_wstrb(
                            32'h0000_0000, as_common.wdata, as_common.wstrb);
                        reset_release_ready_q <=
                            reset_release_ready_q | (tmp & RESET_RELEASE_READY_MASK);
                    end
                    CARD_CTRL_REG_NSC_TIME_LO: begin
                        nsc_time_shadow_q[31:0] <= globals::apply_wstrb(
                            nsc_time_shadow_q[31:0],
                            as_common.wdata,
                            as_common.wstrb
                        );
                    end
                    CARD_CTRL_REG_NSC_TIME_HI: begin
                        automatic logic [31:0] nsc_hi = globals::apply_wstrb(
                            nsc_time_shadow_q[63:32],
                            as_common.wdata,
                            as_common.wstrb
                        );
                        nsc_time_shadow_q[63:32] <= nsc_hi;
                        nsc_time_bcd_q <= {nsc_hi, nsc_time_shadow_q[31:0]};
                    end
                    CARD_CTRL_REG_MENU_CHIME: begin
                        menu_chime_start_q <= as_common.wstrb[0] && as_common.wdata[0];
                    end
                    CARD_CTRL_REG_PHASOR_PAN_LO: begin
                        automatic logic [31:0] pan_tmp = globals::apply_wstrb(
                            {8'h00, phasor_pan_q[23:0]}, as_common.wdata, as_common.wstrb);
                        phasor_pan_q[23:0] <= pan_tmp[23:0];
                    end
                    CARD_CTRL_REG_PHASOR_PAN_HI: begin
                        automatic logic [31:0] pan_tmp = globals::apply_wstrb(
                            {8'h00, phasor_pan_q[47:24]}, as_common.wdata, as_common.wstrb);
                        phasor_pan_q[47:24] <= pan_tmp[23:0];
                    end
                    CARD_CTRL_REG_PHASOR_AUDIO: begin
                        phasor_audio_q <= globals::apply_wstrb(
                            phasor_audio_q, as_common.wdata, as_common.wstrb);
                    end
                    CARD_CTRL_REG_ETH_ADDR: begin
                        automatic logic [31:0] tmp = globals::apply_wstrb(
                            {16'h0000, eth_host_addr_q},
                            as_common.wdata,
                            as_common.wstrb);
                        eth_host_addr_q <= tmp[15:0];
                    end
                    CARD_CTRL_REG_ETH_DATA: begin
                        automatic logic [31:0] tmp = globals::apply_wstrb(
                            {24'h000000, eth_host_wdata_q},
                            as_common.wdata,
                            as_common.wstrb);
                        eth_host_wdata_q <= tmp[7:0];
                    end
                    CARD_CTRL_REG_ETH_CMD: begin
                        if ((as_common.wstrb[0] != 1'b0) &&
                            ((as_common.wdata & CARD_CTRL_ETH_CMD_GO) != 32'h0000_0000)) begin
                            /* The card latches the request even when it
                             * cannot start this cycle (pending latch),
                             * so done always arrives; only a GO while a
                             * previous op is still in flight is an
                             * error. */
                            if (!eth_host_busy_q) begin
                                eth_host_write_q <=
                                    ((as_common.wdata & CARD_CTRL_ETH_CMD_WRITE) !=
                                     32'h0000_0000);
                                eth_host_done_q <= 1'b0;
                                eth_host_error_q <= 1'b0;
                                eth_host_req_pulse <= 1'b1;
                                eth_host_busy_q <= 1'b1;
                            end else begin
                                eth_host_done_q <= 1'b1;
                                eth_host_error_q <= 1'b1;
                            end
                        end
                    end
                    CARD_CTRL_REG_DISK2_SOUND_BASE: begin
                        disk2_sound_sample_base_q <= globals::apply_wstrb(
                            disk2_sound_sample_base_q, as_common.wdata, as_common.wstrb);
                    end
                    CARD_CTRL_REG_DISK2_SOUND_CONTROL: begin
                        automatic logic [31:0] disk2_sound_control_tmp =
                            globals::apply_wstrb(
                                disk2_sound_control_q,
                                as_common.wdata,
                                as_common.wstrb);
                        disk2_sound_control_q <=
                            disk2_sound_control_tmp & ~CARD_CTRL_DISK2_SOUND_EVENT_MASK;
                        if (as_common.wstrb[CARD_CTRL_DISK2_SOUND_EVENT_SHIFT / 8])
                            disk2_menu_sound_event_q <=
                                disk2_sound_control_tmp[
                                    CARD_CTRL_DISK2_SOUND_EVENT_SHIFT +: 4];
                    end
                    8'h20: begin
                        automatic logic [31:0] tmp = globals::apply_wstrb(
                            {31'b0, egress_cfg_enable_q}, as_common.wdata, as_common.wstrb);
                        egress_cfg_enable_q <= tmp[0];
                    end
                    8'h21: egress_cfg_ring_base_q <= globals::apply_wstrb(
                                egress_cfg_ring_base_q, as_common.wdata, as_common.wstrb);
                    8'h22: begin
                        automatic logic [31:0] tmp = globals::apply_wstrb(
                            {27'b0, egress_cfg_ring_size_log2_q},
                            as_common.wdata, as_common.wstrb);
                        egress_cfg_ring_size_log2_q <= tmp[4:0];
                    end
                    8'h23: egress_cfg_producer_ptr_addr_q <= globals::apply_wstrb(
                                egress_cfg_producer_ptr_addr_q, as_common.wdata, as_common.wstrb);
                    8'h24: egress_cfg_consumer_ptr_q <= globals::apply_wstrb(
                                egress_cfg_consumer_ptr_q, as_common.wdata, as_common.wstrb);
                    8'h25: egress_cfg_reset_pulse <= as_common.wdata[0];
                    8'h50: begin
                        automatic logic [31:0] tmp = globals::apply_wstrb(
                            {31'b0, sdd_cfg_enable_q}, as_common.wdata, as_common.wstrb);
                        sdd_cfg_enable_q <= tmp[0];
                    end
                    8'h51: sdd_cfg_ring_base_q <= globals::apply_wstrb(
                                sdd_cfg_ring_base_q, as_common.wdata, as_common.wstrb);
                    8'h52: begin
                        automatic logic [31:0] tmp = globals::apply_wstrb(
                            {27'b0, sdd_cfg_ring_size_log2_q},
                            as_common.wdata, as_common.wstrb);
                        sdd_cfg_ring_size_log2_q <= tmp[4:0];
                    end
                    8'h53: sdd_cfg_producer_ptr_addr_q <= globals::apply_wstrb(
                                sdd_cfg_producer_ptr_addr_q, as_common.wdata, as_common.wstrb);
                    8'h54: sdd_cfg_consumer_ptr_q <= globals::apply_wstrb(
                                sdd_cfg_consumer_ptr_q, as_common.wdata, as_common.wstrb);
                    8'h55: sdd_cfg_reset_pulse <= as_common.wdata[0];
                    8'h60: begin
                        machine_mode_q            <= as_common.wdata[1:0];
                        machine_m2sel_active_high <= as_common.wdata[2];
                    end
                    8'h61: aux_provide_en_q <= as_common.wdata[0];
                    /* (aux_provide is also force-dropped by the boot
                     * ROM's aux probe -- see bm_aux_probe_pulse term
                     * below this case block.) */
                    8'h62: ramworks_en_q <= as_common.wdata[0];
                    8'h63: begin
                        psram_dcount_q          <= as_common.wdata[4:0];
                        psram_dcount_edge_q     <= as_common.wdata[5];
                        psram_dcount_wr_pulse_q <= 1'b1;
                    end
                    8'h64: forensics_clear_pulse <= 1'b1;
                    8'h6A: bm_aux_status_clear <= 1'b1;
                    CARD_CTRL_REG_SS_VRAM_ADDR: begin
                        automatic logic [31:0] a = globals::apply_wstrb(
                            {18'b0, ss_vram_addr_q}, as_common.wdata, as_common.wstrb);
                        ss_vram_addr_q <= a[13:0];
                    end
                    CARD_CTRL_REG_SS_SPR_FLAGS: begin
                        automatic logic [31:0] f = globals::apply_wstrb(
                            {25'b0, ss_status_flags_q}, as_common.wdata, as_common.wstrb);
                        ss_status_flags_q <= f[6:0];
                    end
                    default: begin
                    end
                endcase
            end

            if (bm_aux_probe_pulse) begin
                aux_provide_en_q <= 1'b0;
            end

            if (nsc_write_time_strobe) begin
                nsc_time_shadow_q <= nsc_write_time_bcd;
                nsc_time_bcd_q <= nsc_write_time_bcd;
                nsc_write_seq_q <= nsc_write_seq_q + 32'd1;
            end

            case (as_common.araddr)
                CARD_CTRL_REG_SLOT_ENABLE_MASK:   as_client_rdata_q <= card_slot_enable_mask_q;
                CARD_CTRL_REG_FEATURE_ENABLE_MASK: as_client_rdata_q <= card_feature_enable_mask_q;
                CARD_CTRL_REG_SOFTSW_STATE:        as_client_rdata_q <= {11'h000, current_softswitch_state};
                CARD_CTRL_REG_NSC_TIME_LO:         as_client_rdata_q <= nsc_time_bcd_q[31:0];
                CARD_CTRL_REG_NSC_TIME_HI:         as_client_rdata_q <= nsc_time_bcd_q[63:32];
                CARD_CTRL_REG_NSC_WRITE_SEQ:       as_client_rdata_q <= nsc_write_seq_q;
                CARD_CTRL_REG_MENU_CHIME:          as_client_rdata_q <= 32'h0000_0000;
                CARD_CTRL_REG_PHASOR_PAN_LO:       as_client_rdata_q <= {8'h00, phasor_pan_q[23:0]};
                CARD_CTRL_REG_APPLE_RESET_STATUS:  as_client_rdata_q <= {23'h000000, ab_read.res, apple_reset_seq_q};
                CARD_CTRL_REG_PHASOR_PAN_HI:       as_client_rdata_q <= {8'h00, phasor_pan_q[47:24]};
                CARD_CTRL_REG_PHASOR_AUDIO:        as_client_rdata_q <= phasor_audio_q;
                CARD_CTRL_REG_ETH_ADDR:            as_client_rdata_q <= {16'h0000, eth_host_addr_q};
                CARD_CTRL_REG_ETH_DATA:            as_client_rdata_q <= {24'h000000, eth_host_wdata_q};
                CARD_CTRL_REG_ETH_CMD:             as_client_rdata_q <= {30'h00000000,
                                                                         eth_host_write_q,
                                                                         1'b0};
                CARD_CTRL_REG_ETH_STATUS:          as_client_rdata_q <= {
                    16'h0000,
                    eth_host_rdata_q,
                    4'h0,
                    eth_host_error_q,
                    eth_host_done_q,
                    eth_host_busy_q,
                    eth_host_ready
                };
                CARD_CTRL_REG_DISK2_SOUND_BASE:    as_client_rdata_q <= disk2_sound_sample_base_q;
                CARD_CTRL_REG_DISK2_SOUND_CONTROL: as_client_rdata_q <= disk2_sound_control_q;
                8'h13:   as_client_rdata_q <= dbg_apple_access_count;
                8'h14:   as_client_rdata_q <= dbg_apple_miss_count;
                8'h15:   as_client_rdata_q <= dbg_dma_admit_count;
                8'h16:   as_client_rdata_q <= dbg_dma_complete_count;
                8'h18:   as_client_rdata_q <= dbg_serve_late_count;
                8'h1D:   as_client_rdata_q <= dbg_serve_max_latency;
                8'h17:   as_client_rdata_q <= dbg_aux_write_count;
                8'h20:   as_client_rdata_q <= {31'b0, egress_cfg_enable_q};
                8'h21:   as_client_rdata_q <= egress_cfg_ring_base_q;
                8'h22:   as_client_rdata_q <= {27'b0, egress_cfg_ring_size_log2_q};
                8'h23:   as_client_rdata_q <= egress_cfg_producer_ptr_addr_q;
                8'h24:   as_client_rdata_q <= egress_cfg_consumer_ptr_q;
                8'h26:   as_client_rdata_q <= egress_stat_producer_ptr;
                8'h27:   as_client_rdata_q <= egress_stat_records_written;
                8'h28:   as_client_rdata_q <= egress_stat_gap_markers;
                8'h29:   as_client_rdata_q <= egress_stat_bursts_issued;
                8'h2A:   as_client_rdata_q <= egress_stat_full_stall_cycles;
                8'h50:   as_client_rdata_q <= {31'b0, sdd_cfg_enable_q};
                8'h51:   as_client_rdata_q <= sdd_cfg_ring_base_q;
                8'h52:   as_client_rdata_q <= {27'b0, sdd_cfg_ring_size_log2_q};
                8'h53:   as_client_rdata_q <= sdd_cfg_producer_ptr_addr_q;
                8'h54:   as_client_rdata_q <= sdd_cfg_consumer_ptr_q;
                8'h56:   as_client_rdata_q <= sdd_stat_producer_ptr;
                8'h57:   as_client_rdata_q <= sdd_stat_records_written;
                8'h58:   as_client_rdata_q <= sdd_stat_gap_markers;
                8'h59:   as_client_rdata_q <= sdd_stat_bursts_issued;
                8'h5A:   as_client_rdata_q <= sdd_stat_full_stall_cycles;
                8'h60:   as_client_rdata_q <= {29'b0,
                                               machine_m2sel_active_high,
                                               machine_mode_q};
                8'h61:   as_client_rdata_q <= {31'b0, aux_provide_en_q};
                8'h62:   as_client_rdata_q <= {31'b0, ramworks_en_q};
                8'h63:   as_client_rdata_q <= {26'b0,
                                               psram_dcount_edge_q,
                                               psram_dcount_q};
                8'h64:   as_client_rdata_q <= {27'b0,
                                               res_seen_sticky,
                                               rstn_dip_sticky};
                8'h66:   as_client_rdata_q <= dbg_lost_cycle_count;
                8'h67:   as_client_rdata_q <= dbg_wq_drop_count;
                8'h68:   as_client_rdata_q <= {16'b0, dbg_wq_state};
                8'h69:   as_client_rdata_q <= {8'b0, dbg_miss_ctx};
                8'h6A:   as_client_rdata_q <= {30'b0, bm_aux_status};
                8'h65:   as_client_rdata_q <= {26'b0,
                                               sss.c8_select,
                                               sss.c8_internal_rom,
                                               sss.sw_slotc3rom,
                                               sss.sw_intcxrom};
                CARD_CTRL_REG_SS_REGS_LO:   as_client_rdata_q <= ss_regs[31:0];
                CARD_CTRL_REG_SS_REGS_HI:   as_client_rdata_q <= ss_regs[63:32];
                CARD_CTRL_REG_SS_STATUS:    as_client_rdata_q <=
                    {6'b0, ss_vdp_overlay, ss_apple_video, ss_frame, ss_status};
                CARD_CTRL_REG_SS_VRAM_DATA: as_client_rdata_q <= {24'b0, ss_vram_data};
                default: as_client_rdata_q <= 32'h00000000;
            endcase
        end
    end
    assign as_client.rdata = as_client_rdata_q;

endmodule
